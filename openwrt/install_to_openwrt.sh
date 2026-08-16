#!/usr/bin/env bash
#
# Install Audio Pro C3 / Linkplay A28 Device Tree, Kernel Patches, and mcud daemon to OpenWrt source tree
#

set -e

OPENWRT_DIR="$1"

if [ -z "$OPENWRT_DIR" ] || [ ! -d "$OPENWRT_DIR" ]; then
    echo "Usage: $0 /path/to/openwrt"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[+] Installing Audio Pro C3 BSP into OpenWrt: $OPENWRT_DIR"

# 1. DTS
mkdir -p "$OPENWRT_DIR/target/linux/ramips/dts"
cp "$REPO_ROOT/dts/mt7628an_audiopro_c3.dts" "$OPENWRT_DIR/target/linux/ramips/dts/"
echo "[+] Device Tree installed: target/linux/ramips/dts/mt7628an_audiopro_c3.dts"

# 2. Kernel Patch (ASoC driver for MT7628 is natively integrated via 835-asoc in OpenWrt 23.05)

# 3. mcud Package
mkdir -p "$OPENWRT_DIR/package/utils/mcud"
cp -r "$REPO_ROOT/openwrt/package/utils/mcud/"* "$OPENWRT_DIR/package/utils/mcud/"
echo "[+] Package installed: package/utils/mcud"

# 4. Sound assets and default configurations
mkdir -p "$OPENWRT_DIR/files"
cp -r "$REPO_ROOT/files/"* "$OPENWRT_DIR/files/"
echo "[+] Files rootoverlay installed: files/"

echo "[*] Done! Now add Device/audiopro_c3 to target/linux/ramips/image/mt7628.mk with DEVICE_PACKAGES += mcud"
