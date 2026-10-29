// whistle-dictate: live dictation with the Cactus Whistle speech model.
//
// stdin   16 kHz mono native-endian f32 PCM, e.g. from
//         pw-record --rate 16000 --channels 1 --format f32 --raw -
// stdout  each piece of transcript the engine commits, one line per piece
//
// The engine (libneedle.a) is linked in statically and the model is embedded
// at build time, so the program opens no files and no sockets of its own.
#include "boundary.hpp"
#include "needle.h"

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {

using whistle::CactV2;
using whistle::Language;
using Error = std::string;

// #embed is C++26 (P1967); clang 23 still labels it a C23 extension in C++.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
// 16-byte aligned like the malloc'd buffers upstream's own callers pass.
alignas(16) constexpr unsigned char whistle_cact[] = {
#embed "whistle.cact"
};
#pragma clang diagnostic pop
constexpr CactV2 model{whistle_cact};

// The stream API takes about one second of 16 kHz audio per call.
constexpr std::size_t chunk_samples = 16'000;
static_assert(chunk_samples <= INT_MAX);
// Upstream's Python binding uses the same reply buffer size.
constexpr int reply_capacity = 1 << 18;

constexpr std::string_view usage = R"(usage: whistle-dictate [--language CODE]

Transcribes 16 kHz mono native-endian 32-bit float PCM from stdin as it
arrives. Each piece of text the engine commits is written to stdout as one
line; join the lines with a space. The stream ends at end of input, SIGINT or
SIGTERM, and the words still unconfirmed are then committed as a last line.

  --language CODE  en, de, fr, es, it, nl or pl (default: detect)
  --help           show this help

example:
  pw-record --rate 16000 --channels 1 --format f32 --raw - | whistle-dictate
)";

[[nodiscard]] Error errno_message(std::string_view what)
{
    return std::format("{}: {}", what, std::strerror(errno));
}

struct Options {
    std::optional<Language> language;  // empty: the engine detects it
    bool help = false;
};

[[nodiscard]] std::expected<Options, Error> parse_args(std::span<char* const> args)
{
    Options options;
    for (std::size_t k = 0; k < args.size(); ++k) {
        const std::string_view arg = args[k];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--language") {
            if (++k == args.size())
                return std::unexpected("--language needs a value");
            options.language = Language::parse(args[k]);
            if (!options.language)
                return std::unexpected(std::format("unknown language '{}'", args[k]));
        } else {
            return std::unexpected(std::format("unknown argument '{}'", arg));
        }
    }
    return options;
}

class Fd {
public:
    explicit Fd(int fd) noexcept : fd_{fd} {}
    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    Fd& operator=(Fd&&) = delete("one owner for the lifetime of the descriptor");
    ~Fd()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

// SIGINT and SIGTERM end the stream the way end of input does, so the
// unconfirmed words still get committed. They are blocked before the engine
// starts its worker threads, which inherit the mask, and read from a signalfd.
[[nodiscard]] std::expected<Fd, Error> block_stop_signals()
{
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    if (const int e = pthread_sigmask(SIG_BLOCK, &stop, nullptr); e != 0)
        return std::unexpected(std::format("pthread_sigmask: {}", std::strerror(e)));
    const int fd = ::signalfd(-1, &stop, SFD_CLOEXEC);
    if (fd < 0)
        return std::unexpected(errno_message("signalfd"));
    return Fd{fd};
}

struct Filled {
    std::size_t samples;
    bool last;  // stdin ended or a stop signal arrived
};

// Fills chunk from stdin until it is full or the stream ends. A trailing
// partial sample at end of input is dropped.
[[nodiscard]] std::expected<Filled, Error> fill(std::span<float> chunk, const Fd& stop)
{
    const std::span<std::byte> bytes = std::as_writable_bytes(chunk);
    std::size_t got = 0;
    std::array<pollfd, 2> fds{{{STDIN_FILENO, POLLIN, 0}, {stop.get(), POLLIN, 0}}};
    while (got < bytes.size()) {
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errno_message("poll"));
        }
        if (fds[1].revents != 0)
            return Filled{got / sizeof(float), true};
        if (fds[0].revents == 0)
            continue;
        const ssize_t n = ::read(STDIN_FILENO, bytes.data() + got, bytes.size() - got);
        if (n == 0)
            return Filled{got / sizeof(float), true};
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return std::unexpected(errno_message("stdin"));
        }
        got += static_cast<std::size_t>(n);
    }
    return Filled{chunk.size(), false};
}

// libneedle keeps one process-global speech model and stream. This class is
// their only user and fixes the call order: load, process..., stop.
class Stream {
public:
    [[nodiscard]] static std::expected<Stream, Error> load(const CactV2& cact,
                                                           std::optional<Language> language)
    {
        const std::span<const unsigned char> bytes = cact.bytes();
        if (needle_load(bytes.data(), bytes.size()) < 0)
            return std::unexpected(engine_error("needle_load"));
        return Stream{language ? language->code() : nullptr};
    }

    // Feeds at most one chunk; returns the text it committed.
    [[nodiscard]] std::expected<std::string, Error> process(std::span<const float> pcm)
    {
        assert(pcm.size() <= chunk_samples);
        fed_ = true;
        return reply(needle_stream_transcribe_process(pcm.data(), static_cast<int>(pcm.size()), language_,
                                                      nullptr, reply_.data(), reply_capacity),
                     "needle_stream_transcribe_process");
    }

    // Ends the stream; returns its unconfirmed tail, now committed.
    [[nodiscard]] std::expected<std::string, Error> stop()
    {
        // The header does not say what stopping a stream that never started does.
        if (!fed_)
            return std::string{};
        return reply(needle_stream_transcribe_stop(reply_.data(), reply_capacity),
                     "needle_stream_transcribe_stop");
    }

private:
    explicit Stream(const char* language) : language_{language}, reply_(reply_capacity) {}

    [[nodiscard]] static Error engine_error(std::string_view call)
    {
        const char* reason = needle_last_error();
        return std::format("{}: {}", call, whistle::sanitize(reason ? reason : "unknown error"));
    }

    [[nodiscard]] std::expected<std::string, Error> reply(int rc, std::string_view call) const
    {
        if (rc < 0)
            return std::unexpected(engine_error(call));
        const std::string_view json{reply_.data(), ::strnlen(reply_.data(), reply_.size())};
        return whistle::committed_text(json).transform_error(
            [&](whistle::ReplyError e) { return std::format("{}: {}", call, whistle::describe(e)); });
    }

    const char* language_;  // nullptr: the engine detects it
    std::vector<char> reply_;
    bool fed_ = false;
};

// Writes one piece as a line, unbuffered, so the reader can type it at once.
[[nodiscard]] std::expected<void, Error> emit(const std::string& piece)
{
    if (piece.empty())
        return {};
    const std::string line = piece + '\n';
    for (std::string_view rest = line; !rest.empty();) {
        const ssize_t n = ::write(STDOUT_FILENO, rest.data(), rest.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errno_message("stdout"));
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> run(const Options& options)
{
    auto stop = block_stop_signals();
    if (!stop)
        return std::unexpected(std::move(stop.error()));
    auto stream = Stream::load(model, options.language);
    if (!stream)
        return std::unexpected(std::move(stream.error()));

    std::vector<float> chunk(chunk_samples);
    for (bool last = false; !last;) {
        const auto filled = fill(chunk, *stop);
        if (!filled)
            return std::unexpected(filled.error());
        last = filled->last;
        const std::span<float> pcm = std::span{chunk}.first(filled->samples);
        if (pcm.empty())
            continue;
        std::ranges::transform(pcm, pcm.begin(), whistle::to_unit);
        if (auto done = stream->process(pcm).and_then(emit); !done)
            return done;
    }
    return stream->stop().and_then(emit);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto options = parse_args(std::span{argv, static_cast<std::size_t>(argc)}.subspan(1));
    if (!options) {
        std::print(stderr, "whistle-dictate: {}\n\n{}", options.error(), usage);
        return 2;
    }
    if (options->help) {
        std::print("{}", usage);
        return 0;
    }
    if (const auto done = run(*options); !done) {
        std::println(stderr, "whistle-dictate: {}", done.error());
        return 1;
    }
    return 0;
}
