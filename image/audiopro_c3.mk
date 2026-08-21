# Appended to target/linux/ramips/image/mt76x8.mk by scripts/install_to_openwrt.sh
#
# IMAGE_SIZE must match the firmware partition in the DTS (0xfb0000 = 16064k).
# Shrinking it silently truncates rootfs_data instead of failing the build.
define Device/audiopro_c3
  SOC := mt7628an
  IMAGE_SIZE := 16064k
  DEVICE_VENDOR := Audio Pro
  DEVICE_MODEL := Addon C3
  DEVICE_VARIANT := Linkplay A28 V01
  DEVICE_DTS := mt7628an_audiopro_c3
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma -O linux
  DEVICE_PACKAGES := kmod-sound-mt7620 alsa-utils alsa-lib alsa-plugin-c3eq mpg123 \
                     shairport-sync-mbedtls avahi-dbus-daemon \
                     zram-swap mcud dnsmasq \
                     uhttpd uhttpd-mod-ubox uhttpd-mod-lua px5g-mbedtls \
                     liblua libuci-lua iwinfo
  SUPPORTED_DEVICES += audiopro,c3 linkplay,a28
endef
TARGET_DEVICES += audiopro_c3
