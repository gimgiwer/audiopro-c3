#!/usr/bin/env bash
#
# Graft the Audio Pro C3 BSP onto an existing OpenWrt 23.05 source tree.
#
#   scripts/install_to_openwrt.sh /path/to/openwrt
#
# Idempotent: safe to re-run after a `git pull` in the OpenWrt tree.

set -euo pipefail

OPENWRT_DIR="${1:-}"
if [ -z "$OPENWRT_DIR" ] || [ ! -f "$OPENWRT_DIR/include/version.mk" ]; then
    echo "Usage: $0 /path/to/openwrt   (must be an OpenWrt source tree)" >&2
    exit 1
fi
OPENWRT_DIR="$(cd "$OPENWRT_DIR" && pwd)"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

say() { printf '[+] %s\n' "$*"; }

# --- 1. device tree ---------------------------------------------------------
install -Dm644 "$REPO_ROOT/dts/mt7628an_audiopro_c3.dts" \
    "$OPENWRT_DIR/target/linux/ramips/dts/mt7628an_audiopro_c3.dts"
say "DTS -> target/linux/ramips/dts/"

# --- 2. kernel patches -----------------------------------------------------
# Without these the I2S/GDMA path does not work at all: no sound, and stopping
# playback hard-locks the SoC. This is the part the DTS alone cannot give you.
PATCHDIR="$OPENWRT_DIR/target/linux/ramips/patches-5.15"
mkdir -p "$PATCHDIR"
for p in "$REPO_ROOT"/patches/*.patch; do
    install -m644 "$p" "$PATCHDIR/$(basename "$p")"
    say "patch -> patches-5.15/$(basename "$p")"
done

# --- 3. mcud package -------------------------------------------------------
rm -rf "$OPENWRT_DIR/package/utils/mcud"
mkdir -p "$OPENWRT_DIR/package/utils/mcud"
cp -r "$REPO_ROOT/openwrt/package/utils/mcud/." "$OPENWRT_DIR/package/utils/mcud/"
say "package/utils/mcud"

# --- 3b. c3eq ALSA plugin --------------------------------------------------
rm -rf "$OPENWRT_DIR/package/sound/alsa-plugin-c3eq"
mkdir -p "$OPENWRT_DIR/package/sound/alsa-plugin-c3eq"
cp -r "$REPO_ROOT/openwrt/package/sound/alsa-plugin-c3eq/." \
    "$OPENWRT_DIR/package/sound/alsa-plugin-c3eq/"
say "package/sound/alsa-plugin-c3eq"

# --- 3c. package feed patches ----------------------------------------------
# These three packages live in the packages feed, so they get patched in place.
# Needs `./scripts/feeds install` to have run first - build.sh does that before
# calling us. Adding a file under a package's patches/ is enough to force a
# rebuild; OpenWrt hashes that directory into the prepared stamp.
feed_patches() {
    local pkg="$1" dir="$OPENWRT_DIR/feeds/packages/$2"
    if [ ! -d "$dir" ]; then
        echo "[!] $dir missing - run ./scripts/feeds install -a, then re-run me" >&2
        return
    fi
    mkdir -p "$dir/patches"
    for p in "$REPO_ROOT/patches/$pkg"/[0-9]*.patch; do
        [ -e "$p" ] || continue
        install -m644 "$p" "$dir/patches/$(basename "$p")"
        say "$pkg patch -> feeds/packages/$2/patches/$(basename "$p")"
    done
}

# The init scripts ship inside the feed, not as a build-time patch, so these are
# applied to the feed tree itself. Guarded on a string the patch adds, since
# patch(1) exits non-zero on an already-applied patch and we run with -e.
init_patch() {
    local dir="$OPENWRT_DIR/feeds/packages/$1" marker="$2" patch="$3"
    [ -d "$dir" ] || return
    if grep -qr "$marker" "$dir/files"; then
        say "$1 init already patched"
    else
        patch -p1 -d "$dir" < "$REPO_ROOT/$patch"
        say "$1 init patched"
    fi
}

feed_patches alsa-lib libs/alsa-lib
feed_patches shairport-sync sound/shairport-sync
init_patch sound/shairport-sync volume_control_profile \
    patches/shairport-sync/feed-init-volume-control-profile.patch
init_patch sound/squeezelite procd_append_param \
    patches/squeezelite/feed-init-quote-args.patch

# --- 4. root overlay -------------------------------------------------------
mkdir -p "$OPENWRT_DIR/files"
cp -r "$REPO_ROOT/files/." "$OPENWRT_DIR/files/"
# rc.d symlinks are generated at image time; shipping them breaks enable/disable
rm -rf "$OPENWRT_DIR/files/etc/rc.d"
say "files/ root overlay ($(find "$REPO_ROOT/files" -type f | wc -l) files)"

# --- 5. image profile ------------------------------------------------------
MK="$OPENWRT_DIR/target/linux/ramips/image/mt76x8.mk"
if grep -q "define Device/audiopro_c3" "$MK"; then
    say "device profile already present, leaving it alone"
else
    cat "$REPO_ROOT/image/audiopro_c3.mk" >> "$MK"
    say "device profile appended to image/mt76x8.mk"
fi

# --- 6. config seed --------------------------------------------------------
if [ ! -f "$OPENWRT_DIR/.config" ]; then
    cp "$REPO_ROOT/config/audiopro_c3.diffconfig" "$OPENWRT_DIR/.config"
    say "config seed -> .config (run 'make defconfig' to expand it)"
else
    say ".config exists, not overwriting (seed is in config/audiopro_c3.diffconfig)"
fi

cat <<EOF

Done. Next:
  cd $OPENWRT_DIR
  ./scripts/feeds update -a && ./scripts/feeds install -a
  make defconfig
  make -j\$(nproc)

Image lands in bin/targets/ramips/mt76x8/.
EOF
