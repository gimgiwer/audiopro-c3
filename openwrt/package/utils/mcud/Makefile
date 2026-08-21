include $(TOPDIR)/rules.mk

PKG_NAME:=mcud
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/mcud
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Linkplay Secondary MCU Hardware Supervisor Daemon for Audio Pro C3
  DEPENDS:=+alsa-lib +libmosquitto +libubus +libblobmsg-json +libubox
endef

define Package/mcud/description
  Hardware Supervisor Daemon for Audio Pro Addon C3 (Linkplay A28 V01).
  Manages TAS57xx/TPA3116D2 hardware master volume with psychoacoustic curve,
  top panel buttons, battery & charging status ADC, input source switching (Wi-Fi/BT/AUX),
  ubus RPC API, watchdog, auto-sleep, and Home Assistant MQTT Discovery.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/mcud $(PKG_BUILD_DIR)/mcud.c \
		$(TARGET_LDFLAGS) -lasound -lmosquitto -lubus -lubox -lblobmsg_json
endef

define Package/mcud/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mcud $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/mcud.init $(1)/etc/init.d/mcud
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/mcud.config $(1)/etc/config/mcud
endef

$(eval $(call BuildPackage,mcud))

