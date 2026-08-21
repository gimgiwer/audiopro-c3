include $(TOPDIR)/rules.mk

PKG_NAME:=alsa-plugin-c3eq
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/alsa-plugin-c3eq
  SECTION:=sound
  CATEGORY:=Sound
  TITLE:=Fixed-point biquad EQ plugin for Audio Pro C3
  DEPENDS:=+alsa-lib
endef

define Package/alsa-plugin-c3eq/description
  ALSA external PCM plugin: cascaded biquads in Q24 fixed point, configured
  from /etc/c3eq.conf. MT7628AN has no FPU, so the float DSP plugins (LADSPA,
  CamillaDSP) are unusable here. Ships bypassed - the C3's voicing is analog,
  this is for the optional bass/treble tone control (see c3tone).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

# -DPIC is not optional: SND_PCM_PLUGIN_SYMBOL only emits the versioned
# snd_dlsym_start marker under PIC, and alsa-lib refuses to load without it.
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(FPIC) -DPIC -Wall -Wextra -shared \
		-o $(PKG_BUILD_DIR)/libasound_module_pcm_c3eq.so \
		$(PKG_BUILD_DIR)/c3eq.c \
		$(TARGET_LDFLAGS) -lasound -lm
endef

define Package/alsa-plugin-c3eq/install
	$(INSTALL_DIR) $(1)/usr/lib/alsa-lib
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/libasound_module_pcm_c3eq.so \
		$(1)/usr/lib/alsa-lib/
endef

$(eval $(call BuildPackage,alsa-plugin-c3eq))
