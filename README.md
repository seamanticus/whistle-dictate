# whistle-dictate

Offline dictation for Wayland, built on the
[Cactus Whistle](https://huggingface.co/Cactus-Compute/whistle) speech model.

- `whistle-dictate` reads 16 kHz mono 32-bit float PCM on stdin and writes
  each piece of transcript the engine commits to stdout, one line per piece.
  It is a static PIE with the engine and the model linked in: about 18 MiB, of
  which 16 MiB is the model. It needs no shared library or file at run time.
- `whistle-dictate-toggle` runs `pw-record | whistle-dictate | wtype` on the
  first call and stops it on the next, so a single key starts and ends
  dictation into the focused window.

## Install on Arch

```sh
makepkg -si
```

The PKGBUILD pins the engine (`Cactus-Compute/needle3`, `linux-x86_64/`) and
the model (`Cactus-Compute/whistle`) to commits and checks their sha256 sums.
To update either, change `_engine_rev` / `_model_rev` and the sums. A model
that is not a format-v2 `.cact` fails the build.

## niri

Bind any free key in `~/.config/niri/config.kdl`:

```kdl
binds {
    Mod+Shift+D { spawn "whistle-dictate-toggle" "--language" "en"; }
}
```

Without `--language` the engine detects the language (en, de, fr, es, it, nl,
pl). `wtype` types through the virtual-keyboard protocol; niri fixed keycode
handling for it after 26.04 (niri PR #4548).

## Privacy

Nothing here phones home. The Python package `cactus-needle` sends usage
telemetry, but this project does not use it, only the engine archive.
`libneedle.a` imports no network, process or environment functions, and its
only raw syscall is the request for Intel AMX matrix registers.

The static binary still contains glibc symbols named `socket`, `sendto`,
`dlopen` and `getenv`. glibc's own thread and locale code links them in, not
the engine and not this program; `lld --why-extract` shows each chain.

## Development

```sh
make check                    # compile-time tests; needs no upstream files
make VENDOR=dir               # dir holds needle.h, libneedle.a, whistle.cact
```

Requirements: clang, lld, libc++ and libc++abi. `libneedle.a` is built against
libc++, so the program is too.
