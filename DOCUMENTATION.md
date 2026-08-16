# Audio Pro C3 (Linkplay A28) - OpenWrt Custom Firmware Architecture

This document provides a factual, comprehensive overview of the custom OpenWrt firmware designed for the Audio Pro C3 speaker (based on the MediaTek MT7688 / Linkplay A28 module).

## 1. Build System & Flash Layout (CRITICAL)
- **Target:** `ramips/mt76x8`
- **Device Tree:** `mt7628an_audiopro_c3.dts`
- **Flash Size:** 16MB SPI Flash.
- **MTD Layout Protection:** The `IMAGE_SIZE` in `target/linux/ramips/image/mt76x8.mk` MUST NEVER exceed **`11456k`** (Offset `0xB30000`).
  - **Reason:** The manufacturer stores unique device calibration (`rf` partition) and DRM certificates (`user1`, `user2` partitions) at the end of the flash. If `IMAGE_SIZE` is incorrectly set (e.g., to 16064k), the OpenWrt JFFS2 overlay will format this space on first boot, permanently destroying Spotify Connect/AirPlay keys and Wi-Fi calibration.

## 2. Audio Pipeline & Arbiter
The firmware transforms the speaker into a fully local, multi-source Hi-Fi network player without relying on Linkplay cloud servers.

- **Audio Arbiter (`/usr/bin/audio_arbiter.sh`)**:
  - Manages ALSA playback conflicts via a rigid priority system configured in UCI (`/etc/config/mcud`).
  - **Priority Hierarchy:** `Snapcast (1)` > `AirPlay (2)` > `Spotify (3)` > `DLNA (4)` > `WebRadio (5)` > `Squeezelite (6)`.
  - When a higher-priority stream starts, the arbiter terminates lower-priority processes (e.g., killing `gmrender` if AirPlay starts).

- **System Sounds & SIP Ducking**:
  - Assets in `/usr/share/sounds/` (`boot.wav`, `bell.wav`, etc.) are played directly to the `tts_in` or `music_in` ALSA channels.
  - **Baresip (`/etc/baresip/`)**: A SIP client running as an intercom daemon (port 5060). When a call arrives, it outputs to the `tts_in` channel, which hardware-ducks the main music channel and plays `bell.wav`.

## 3. Security & Web API
- **Web Interface (`/www/api.lua`)**:
  - Serves as the primary configuration backend, reading/writing to OpenWrt's UCI database.
- **Authentication & Kill-Switch (`/etc/config/mcud`)**:
  - Security policies are centrally enforced via UCI (`mcud.main`).
  - `auth_enabled`: Toggles token-based authentication for the Web API.
  - `allow_custom_commands`: A kill-switch for physical preset buttons. 
- **RCE Mitigation (`/usr/bin/audiopro_preset_handler.sh`)**:
  - Prevents Remote Code Execution (RCE) via preset buttons. The handler script explicitly refuses to execute `eval "$CMD"` if `auth_enabled=0` or `allow_custom_commands=0`, effectively mitigating command injection attacks on unauthenticated LANs.

## 4. Hardware Integrations (C Daemons)
- **MCUD (`services/mcud.c`)**:
  - Interfaces with the external MCU via UART to read battery levels, button presses, and control hardware sources (Wi-Fi, BT, AUX).
- **AEC Bridge (`services/aec_bridge.c`)**:
  - Acoustic Echo Cancellation daemon. Reads the final mixed ALSA loopback output (`snd-aloop`) and streams it over a TCP socket to Home Assistant, allowing software microphones to subtract the speaker's music from voice commands.

## 5. Compilation Pipeline (Podman)
The build must be executed in a rootless container (e.g., `openwrt-builder:22.04`) due to GCC compatibility.
1. Apply `openwrt/install_to_openwrt.sh` to the OpenWrt 23.05 source tree.
2. Ensure `IMAGE_SIZE := 11456k` in `mt76x8.mk`.
3. Ensure Kernel configs (`CONFIG_DMA_RALINK=y`, `CONFIG_SND_ALOOP=y`) are set.
4. Execute `make -j$(nproc) V=s` inside the container.
