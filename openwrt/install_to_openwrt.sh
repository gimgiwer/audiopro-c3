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

# 2. mcud Package
mkdir -p "$OPENWRT_DIR/package/utils/mcud"
cp -r "$REPO_ROOT/openwrt/package/utils/mcud/"* "$OPENWRT_DIR/package/utils/mcud/"
echo "[+] Package installed: package/utils/mcud"

# 3. Sound assets, scripts and default configurations
mkdir -p "$OPENWRT_DIR/files"
cp -r "$REPO_ROOT/files/"* "$OPENWRT_DIR/files/"
rm -rf "$OPENWRT_DIR/files/etc/rc.d" 2>/dev/null || true
echo "[+] Files rootoverlay installed: files/"

# 4. Device Profile in mt76x8.mk
MT76X8_MK="$OPENWRT_DIR/target/linux/ramips/image/mt76x8.mk"
if [ -f "$MT76X8_MK" ] && ! grep -q "define Device/audiopro_c3" "$MT76X8_MK"; then
    echo "[+] Adding Device/audiopro_c3 profile to target/linux/ramips/image/mt76x8.mk"
    cat >> "$MT76X8_MK" << 'EOF'

define Device/audiopro_c3
  SOC := mt7628an
  IMAGE_SIZE := 11456k
  DEVICE_VENDOR := Audio Pro
  DEVICE_MODEL := Addon C3
  DEVICE_VARIANT := Linkplay A28 V01
  DEVICE_DTS := mt7628an_audiopro_c3
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma -O svr4
  DEVICE_PACKAGES := kmod-sound-mt7620 alsa-utils alsa-lib \
                     shairport-sync-mbedtls librespot mpg123 squeezelite \
                     gmrender-resurrect snapclient baresip \
                     mosquitto-client-ssl zram-swap mcud \
                     uhttpd uhttpd-mod-lua liblua libuci-lua iwinfo coreutils-base64
  SUPPORTED_DEVICES += audiopro,c3 linkplay,a28
endef
TARGET_DEVICES += audiopro_c3
EOF
fi


echo "[*] Done! Audio Pro C3 BSP is fully installed into $OPENWRT_DIR"
