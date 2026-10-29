# Maintainer: seamanticus <339379769+seamanticus@users.noreply.github.com>
pkgname=whistle-dictate
pkgver=0.1.0
pkgrel=1
pkgdesc='Offline dictation for Wayland: Cactus Whistle speech-to-text as one static binary'
arch=('x86_64')
url='https://huggingface.co/Cactus-Compute/whistle'
# Apache-2.0 covers the upstream engine and model; the license of this
# repository's own code is not chosen yet.
license=('Apache-2.0')
# The binary needs nothing at run time; the toggle script needs these.
depends=('pipewire-audio' 'wtype')
makedepends=('clang' 'lld' 'libc++' 'libc++abi')
# One translation unit: LTO gains nothing and would carry the model as bitcode.
options=('!lto')

# Upstream publishes no releases, so both repositories are pinned to a commit.
_engine_rev=d6c7d6a2e9655505e0f8e23339356dd1705900eb
_model_rev=741118687e1f008f06d1351a402a731274789790
_hf=https://huggingface.co/Cactus-Compute
source=('Makefile' 'boundary.hpp' 'dictate.cpp' 'whistle-dictate-toggle'
        "libneedle-${_engine_rev:0:8}.a::$_hf/needle3/resolve/$_engine_rev/linux-x86_64/libneedle.a"
        "needle-${_engine_rev:0:8}.h::$_hf/needle3/resolve/$_engine_rev/linux-x86_64/needle.h"
        "whistle-${_model_rev:0:8}.cact::$_hf/whistle/resolve/$_model_rev/whistle.cact")
# Local files live next to this PKGBUILD in the same repository.
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP'
            '2f281877e24fed2658187a32d9a26e74c32d4f273ac9a2b36180b3a6e85942da'
            'a27dac4b3239eead91d124b7f7dbce5495c13019903fd817b68c16d39b055a99'
            'b6e02f048568ac5d01a2042556c658061e699acbc0aa2a1439f52f3d461dffeb')

prepare() {
  mkdir -p vendor
  ln -sf "../libneedle-${_engine_rev:0:8}.a" vendor/libneedle.a
  ln -sf "../needle-${_engine_rev:0:8}.h" vendor/needle.h
  ln -sf "../whistle-${_model_rev:0:8}.cact" vendor/whistle.cact
}

build() {
  make VENDOR=vendor
}

check() {
  make check
}

package() {
  make DESTDIR="$pkgdir" PREFIX=/usr install
  install -Dm755 whistle-dictate-toggle "$pkgdir/usr/bin/whistle-dictate-toggle"
}
