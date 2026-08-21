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
