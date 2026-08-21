#!/usr/bin/env bash
#
# Cross-build librespot with the fixed-point Tremor Vorbis decoder.
#
#   scripts/build_librespot.sh /path/to/openwrt-src [/path/to/librespot]
#
# Why bother: the 24KEc core in this speaker has no FPU, so symphonia's float
# Vorbis path burns most of the CPU and starves the ALSA writer -> stutter.
# Tremor is integer-only. patches/librespot/ adds it as a feature.

set -euo pipefail

OPENWRT_DIR="${1:-}"
LIBRESPOT_SRC="${2:-$PWD/librespot}"
LIBRESPOT_REV="9c7d756"   # upstream commit the patch is cut against

[ -n "$OPENWRT_DIR" ] || { echo "Usage: $0 /path/to/openwrt-src [librespot-src]" >&2; exit 1; }
OPENWRT_DIR="$(cd "$OPENWRT_DIR" && pwd)"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TC=$(echo "$OPENWRT_DIR"/staging_dir/toolchain-mipsel_24kc_gcc-*_musl)
ST="$OPENWRT_DIR/staging_dir/target-mipsel_24kc_musl"
[ -d "$TC" ] && [ -d "$ST" ] || {
    echo "toolchain/staging dir missing - build the firmware once first" >&2; exit 1; }

if [ ! -d "$LIBRESPOT_SRC/.git" ]; then
    git clone https://github.com/librespot-org/librespot "$LIBRESPOT_SRC"
    git -C "$LIBRESPOT_SRC" checkout "$LIBRESPOT_REV"
    git -C "$LIBRESPOT_SRC" apply "$REPO_ROOT/patches/librespot/"*.patch
fi

# mipsel-unknown-linux-musl is tier 3, so there is no prebuilt std: build it.
rustup toolchain list | grep -q nightly || rustup toolchain install nightly
rustup component add rust-src --toolchain nightly

export PATH="$TC/bin:$PATH" STAGING_DIR="$ST"
export CARGO_TARGET_MIPSEL_UNKNOWN_LINUX_MUSL_LINKER="$TC/bin/mipsel-openwrt-linux-gcc"
export CC_mipsel_unknown_linux_musl="$TC/bin/mipsel-openwrt-linux-gcc"
export AR_mipsel_unknown_linux_musl="$TC/bin/mipsel-openwrt-linux-gcc-ar"
export PKG_CONFIG_ALLOW_CROSS=1
export PKG_CONFIG_LIBDIR="$ST/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$ST"
# the deployed binary is dynamic, so turn crt-static back off
export RUSTFLAGS="-C target-feature=-crt-static -L $ST/usr/lib -C link-arg=-L$ST/usr/lib"

cd "$LIBRESPOT_SRC"
# Feature set matches what the speaker actually links: alsa + rustls + libmdns.
# The defaults would pull in rodio and native-tls and fail.
cargo +nightly build --release --target mipsel-unknown-linux-musl \
    -Z build-std=std,panic_abort \
    --no-default-features \
    --features "alsa-backend,rustls-tls-webpki-roots,with-libmdns,tremor-decoder"

BIN="target/mipsel-unknown-linux-musl/release/librespot"
"$TC/bin/mipsel-openwrt-linux-strip" "$BIN"
echo
echo "built: $(du -h "$BIN" | cut -f1)"
readelf -A "$BIN" | grep -E "FP ABI|CPR1" || true
echo "copy it to files/usr/bin/librespot to ship it in the image"
