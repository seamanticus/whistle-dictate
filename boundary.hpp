// Everything that enters whistle-dictate is normalised here, by constexpr
// functions whose tests are the static_asserts at the bottom, so they run on
// every build:
//   build time  model file -> CactV2         (exists only if the header checks)
//   argv        --language -> Language       (only parse() can make one)
//   stdin       float      -> [-1, 1]        to_unit, a retraction
//   engine      JSON reply -> typeable text  committed_text, ends in a retraction
// A retraction r leaves everything already in its target alone, which makes
// it idempotent: r(r(x)) == r(x). The tests check that law on every result.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace whistle {

// libneedle reads format-v2 .cact files only: tag 0x05E12A84, little-endian,
// at offset 0 (its rejection message names v1's tag, 0x05E12A83).
[[nodiscard]] constexpr bool has_cact_v2_tag(std::span<const unsigned char> bytes)
{
    return bytes.size() >= 4 && bytes[0] == 0x84 && bytes[1] == 0x2A && bytes[2] == 0xE1 &&
           bytes[3] == 0x05;
}

// Model bytes whose header the engine accepts. The constructor runs only at
// compile time, so a wrong embedded file fails the build instead of the run.
class CactV2 {
public:
    consteval explicit CactV2(std::span<const unsigned char> bytes) : bytes_{bytes}
    {
        if (!has_cact_v2_tag(bytes))
            throw "embedded model is not a format-v2 .cact; libneedle would refuse it";
    }
    [[nodiscard]] constexpr std::span<const unsigned char> bytes() const { return bytes_; }

private:
    std::span<const unsigned char> bytes_;
};

// A speech language of the engine. Only parse() makes one, so holding a
// Language is the proof that its code is one the engine accepts.
class Language {
public:
    static constexpr std::array<const char*, 7> codes{"en", "de", "fr", "es", "it", "nl", "pl"};

    [[nodiscard]] static constexpr std::optional<Language> parse(std::string_view code)
    {
        const auto it = std::ranges::find(codes, code, [](const char* c) { return std::string_view{c}; });
        if (it == codes.end())
            return std::nullopt;
        return Language{static_cast<std::size_t>(it - codes.begin())};
    }
    // NUL-terminated, as the C API needs.
    [[nodiscard]] constexpr const char* code() const { return codes[index_]; }

private:
    constexpr explicit Language(std::size_t index) : index_{index} {}
    std::size_t index_;
};

// Retraction of float onto the [-1, 1] range the engine expects: samples
// already inside are kept, NaN becomes silence.
[[nodiscard]] constexpr float to_unit(float x)
{
    return std::isnan(x) ? 0.0f : std::clamp(x, -1.0f, 1.0f);
}

inline constexpr char32_t replacement_character = U'\uFFFD';

// Decodes the UTF-8 sequence at s[i] and moves i past it. A malformed
// sequence (bad bytes, overlong, surrogate, beyond U+10FFFF) yields U+FFFD
// and moves one byte, so decoding always makes progress.
[[nodiscard]] constexpr char32_t next_codepoint(std::string_view s, std::size_t& i)
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    const std::size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
    if (len == 0 || lead > 0xF4 || s.size() - i < len) {
        ++i;
        return replacement_character;
    }
    auto cp = static_cast<char32_t>(lead & (0x7F >> len));
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char next = byte(i + k);
        if ((next & 0xC0) != 0x80) {
            ++i;
            return replacement_character;
        }
        cp = (cp << 6) | (next & 0x3Fu);
    }
    constexpr std::array<char32_t, 5> smallest{0, 0, 0x80, 0x800, 0x10000};
    if (cp < smallest[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return replacement_character;
    }
    i += len;
    return cp;
}

// cp must be a Unicode scalar value, which next_codepoint guarantees.
constexpr void append_utf8(std::string& out, char32_t cp)
{
    const auto put = [&](char32_t unit) { out.push_back(static_cast<char>(unit)); };
    if (cp < 0x80) {
        put(cp);
    } else if (cp < 0x800) {
        put(0xC0 | (cp >> 6));
        put(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        put(0xE0 | (cp >> 12));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    } else {
        put(0xF0 | (cp >> 18));
        put(0x80 | ((cp >> 12) & 0x3F));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    }
}

// C0, DEL and C1 controls: typed into a terminal they act as keys (a newline
// is Enter) or, for C1, as escape sequences.
[[nodiscard]] constexpr bool is_control(char32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

// Retraction of arbitrary bytes onto text that is safe to type: valid UTF-8,
// controls replaced by spaces, no spaces at either end.
[[nodiscard]] constexpr std::string sanitize(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const char32_t cp = next_codepoint(text, i);
        append_utf8(out, is_control(cp) ? U' ' : cp);
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

enum class ReplyError : std::uint8_t { not_an_object, missing_text, malformed };

[[nodiscard]] constexpr std::string_view describe(ReplyError error)
{
    switch (error) {
    case ReplyError::not_an_object: return "reply is not a JSON object";
    case ReplyError::missing_text: return "reply has no \"text\" member";
    case ReplyError::malformed: return "reply is malformed JSON";
    }
    std::unreachable();
}

namespace detail {

// A cursor over JSON that decodes strings and skips everything else. It is
// not a validator: it checks only what reaching the wanted member needs.
struct Json {
    std::string_view s;
    std::size_t i = 0;

    constexpr void skip_space()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    constexpr bool eat(char c)
    {
        skip_space();
        if (i == s.size() || s[i] != c)
            return false;
        ++i;
        return true;
    }

    constexpr std::optional<char32_t> hex4()
    {
        if (s.size() - i < 4)
            return std::nullopt;
        char32_t v = 0;
        for (const char c : s.substr(i, 4)) {
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<char32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<char32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<char32_t>(c - 'A' + 10);
            else
                return std::nullopt;
        }
        i += 4;
        return v;
    }

    // After "\u": one code point, joining a surrogate pair. A lone surrogate
    // becomes U+FFFD because UTF-8 cannot carry it.
    constexpr std::optional<char32_t> escaped_codepoint()
    {
        const auto high = hex4();
        if (!high || *high < 0xD800 || *high > 0xDFFF)
            return high;
        if (*high <= 0xDBFF && s.substr(i).starts_with("\\u")) {
            const std::size_t mark = i;
            i += 2;
            if (const auto low = hex4(); low && *low >= 0xDC00 && *low <= 0xDFFF)
                return 0x10000 + ((*high - 0xD800) << 10) + (*low - 0xDC00);
            i = mark;  // not a pair: the next escape is decoded on its own
        }
        return replacement_character;
    }

    // Escapes are resolved; other bytes are copied as they are, for
    // sanitize() to judge.
    constexpr std::optional<std::string> string()
    {
        if (!eat('"'))
            return std::nullopt;
        std::string out;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"')
                return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i == s.size())
                return std::nullopt;
            switch (const char e = s[i++]) {
            case '"':
            case '\\':
            case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                const auto cp = escaped_codepoint();
                if (!cp)
                    return std::nullopt;
                append_utf8(out, *cp);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Skips one value of any type. Strings are decoded so that brackets and
    // commas inside them do not count.
    constexpr bool skip_value()
    {
        skip_space();
        std::size_t depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                if (!string())
                    return false;
            } else if (c == '{' || c == '[') {
                ++depth;
                ++i;
                continue;
            } else if (c == '}' || c == ']') {
                if (depth == 0)
                    return true;  // closes the enclosing object: a scalar ended
                --depth;
                ++i;
            } else {
                if (c == ',' && depth == 0)
                    return true;
                ++i;
                continue;
            }
            if (depth == 0)
                return true;
        }
        return false;
    }
};

}  // namespace detail

// The words a transcription call committed: the "text" member of the
// top-level reply object, made safe to type. Members of nested objects and
// look-alikes inside strings are never matched.
[[nodiscard]] constexpr std::expected<std::string, ReplyError> committed_text(std::string_view reply)
{
    detail::Json json{reply};
    if (!json.eat('{'))
        return std::unexpected(ReplyError::not_an_object);
    if (json.eat('}'))
        return std::unexpected(ReplyError::missing_text);
    do {
        const auto key = json.string();
        if (!key || !json.eat(':'))
            return std::unexpected(ReplyError::malformed);
        if (*key == "text") {
            const auto text = json.string();
            if (!text)
                return std::unexpected(ReplyError::malformed);
            return sanitize(*text);
        }
        if (!json.skip_value())
            return std::unexpected(ReplyError::malformed);
    } while (json.eat(','));
    return std::unexpected(json.eat('}') ? ReplyError::missing_text : ReplyError::malformed);
}

namespace checks {

constexpr bool commits(std::string_view reply, std::string_view text)
{
    const auto got = committed_text(reply);
    return got && *got == text && sanitize(*got) == *got;
}
constexpr bool rejects(std::string_view reply, ReplyError error)
{
    const auto got = committed_text(reply);
    return !got && got.error() == error;
}

// Exhaustive over the finite type, so this is a proof rather than a sample.
static_assert(std::ranges::all_of(Language::codes, [](const char* c) {
    const auto language = Language::parse(c);
    return language && std::string_view{language->code()} == c;
}));
static_assert(!Language::parse("EN") && !Language::parse("") && !Language::parse("eng"));

static_assert(has_cact_v2_tag(std::array<unsigned char, 5>{0x84, 0x2A, 0xE1, 0x05, 0x00}));
static_assert(!has_cact_v2_tag(std::array<unsigned char, 4>{0x83, 0x2A, 0xE1, 0x05}));  // v1
static_assert(!has_cact_v2_tag(std::array<unsigned char, 3>{0x84, 0x2A, 0xE1}));

static_assert(to_unit(0.25f) == 0.25f && to_unit(-1.0f) == -1.0f && to_unit(1.0f) == 1.0f);
static_assert(to_unit(3.0f) == 1.0f && to_unit(-std::numeric_limits<float>::infinity()) == -1.0f);
static_assert(to_unit(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

// The documented stream reply, and look-alike keys that must not match.
static_assert(commits(R"({"text":"hello world","words":[{"word":"hello","start":0.00}],)"
                      R"("pending":"and","language":"en","received":2.00,"pass_ms":81.5})",
                      "hello world"));
static_assert(commits(R"({"words":[{"text":"no"}],"pending":"\"text\":\"no\"","text":"yes"})", "yes"));
static_assert(commits(R"( { "received" : 1.5 , "text" : " spaced " } )", "spaced"));
static_assert(commits(R"({"text":"say \"hi\" \\ \/ ok"})", R"(say "hi" \ / ok)"));
// Nothing in the text can act as a key press or a terminal escape.
static_assert(commits(R"({"text":"rm -rf ~\nls\r\tx"})", "rm -rf ~ ls  x"));
static_assert(commits(R"({"text":"a\u0007b\u007fc\u009bd"})", "a b c d"));
static_assert(commits("{\"text\":\"a\xC2\x9B" "b\x1B[2Jc\"}", "a b [2Jc"));
static_assert(commits(R"({"text":"\n"})", ""));
// Unicode, escaped and raw; anything malformed becomes U+FFFD.
static_assert(commits(R"({"text":"caf\u00e9 \ud83d\ude00"})", "caf\xC3\xA9 \xF0\x9F\x98\x80"));
static_assert(commits("{\"text\":\"z\xC3\xA9z\"}", "z\xC3\xA9z"));
static_assert(commits(R"({"text":"\ud800x\udc00"})", "\xEF\xBF\xBDx\xEF\xBF\xBD"));
static_assert(commits("{\"text\":\"\xFF\xC0\xAF\"}", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
static_assert(commits("{\"text\":\"\xED\xA0\x80\"}", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));

static_assert(rejects("", ReplyError::not_an_object));
static_assert(rejects("[]", ReplyError::not_an_object));
static_assert(rejects("{}", ReplyError::missing_text));
static_assert(rejects(R"({"pending":"x","words":[]})", ReplyError::missing_text));
static_assert(rejects(R"({"text":1})", ReplyError::malformed));
static_assert(rejects(R"({"text":"open)", ReplyError::malformed));
static_assert(rejects(R"({"text":"bad \x escape"})", ReplyError::malformed));
static_assert(rejects(R"({"text":"\u12"})", ReplyError::malformed));
static_assert(rejects(R"({"pending":"x" "text":"y"})", ReplyError::malformed));

}  // namespace checks
}  // namespace whistle
