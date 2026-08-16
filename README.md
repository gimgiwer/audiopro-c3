# Audio Pro Addon C3 (Linkplay A28 V01 / MediaTek MT7688AN) OpenWrt 23.05 Firmware

[![OpenWrt Version](https://img.shields.io/badge/OpenWrt-23.05.5-blue.svg)](https://openwrt.org/)
[![Kernel](https://img.shields.io/badge/Linux_Kernel-5.15.167-green.svg)](https://kernel.org/)
[![Architecture](https://img.shields.io/badge/Architecture-MIPS_24KEc_(mipsel__24kc)-orange.svg)](https://openwrt.org/docs/techref/instructionset/mips_24kc)
[![Audio](https://img.shields.io/badge/Audio-ALSA_I2S_Master_(TAS5707)-red.svg)](https://www.ti.com/product/TAS5707)
[![GitHub Release](https://img.shields.io/github/v/release/gimgiwer/audiopro-c3)](https://github.com/gimgiwer/audiopro-c3/releases/latest)

Production-ready, highly optimized, and modern **OpenWrt 23.05.5** custom firmware for the **Audio Pro Addon C3** portable wireless smart speaker (powered by the **Linkplay A28 V01** module with **MediaTek MT7688AN SoC**, 64MB DDR2 RAM, and 16MB SPI NOR Flash).

---

## 🌟 Key Features of the Custom Firmware

* **Modern Linux 5.15 Kernel & OpenWrt 23.05:** Replaces the obsolete, closed-source 2016/2021 factory vendor OS (Linux 3.10) with an open, maintained embedded Linux distribution.
* **MIPS 24KEc Optimized:** Built specifically for `mipsel_24kc` (`-march=24kc -mtune=24kc`) with hardware integer multiply/divide and soft-float optimizations.
* **Bit-Perfect Multi-Channel ALSA Sound Stack & Low Latency:**
  * Fixed **44.1 kHz / 16-bit Master I2S Clock** (`MCLK = 11.2896 MHz`, `mclk-fs = 256`) for jitter-free CD-quality playback.
  * Studio-grade **-1.0 dBFS Headroom Limiter** preventing digital inter-sample clipping when mixing concurrent streams.
  * **Ultra-Low Latency (~46.4 ms)** with `period_size 512` / `buffer_size 2048` (86 IRQ/sec), guaranteeing 100% glitch-free playback under heavy Wi-Fi/TLS loads.
  * Direct Hardware Mixing (`dmixer`) with 8 independent virtual Softvol channels: `Spotify`, `AirPlay`, `Music` (DLNA/Snapcast), `Squeeze`, `TTS`, `VoIP`, `Alarm`, `Timer`.
  * Hardware amplifier control via Texas Instruments **TAS5707** DSP & I2C.
* **Full Hi-Fi Streaming & Smart Home Audio Suite (10 Services):**
  * **Spotify Connect** via `librespot` (native MIPS binary, MQTT state sync, D-Bus/MQTT pause/play, alarm URI integration).
  * **Apple AirPlay 1** via `shairport-sync-mbedtls` (lossless ALAC 44.1kHz, DACP remote controls).
  * **Squeezelite / LMS / Roon** for synchronous multiroom FLAC audio streaming.
  * **DLNA / UPnP Renderer** via `gmrender-resurrect` for mobile and desktop media casting.
  * **Snapcast Client** via `snapclient` for sample-accurate multiroom whole-house synchronization.
  * **Home Assistant TTS & Speech** with automatic hardware ducking of background music.
  * **SIP / Doorbell / VoIP Intercom** via `baresip` + hardware AEC echo-cancellation bridge.
  * **Smart Alarm** (`smart_alarm.sh`) with soft volume ramp-up (Fade-In), morning harmonics (`alarm_wake_up.wav`), and Spotify fallback.
  * **Smart Kitchen Timer** (`smart_timer.sh`) with sharp triple-chime alert (`timer_sharp.wav`), countdown API, and priority ducking over alarm and music.
  * **Bluetooth / AUX Input** with direct hardware input switching handled by `mcud`.
* **Hardware Microcontroller Daemon (`mcud`):**
  * Native C daemon managing the secondary STM8/Linkplay MCU over internal UART (`/dev/ttyS0` @ 57600 baud).
  * Real-time monitoring of top panel pushbuttons (Preset 1–4, Play/Pause, Vol+/Vol-, Input Selection).
  * Priority button hierarchy: `Play/Pause` dismisses ringing Timer → ringing Alarm → toggles active Player.
  * Battery charge/voltage telemetry, hardware LEDs, and TAS5707 volume sync.
  * Unified FIFO command queue (`/tmp/mcu_cmd_fifo`) and MQTT publisher for Home Assistant.
* **Hardened Security & Modern Web UI:**
  * Complete removal of hardcoded credentials, serial numbers, MAC addresses, and cloud telemetry backdoors.
  * Atomic configuration writing, strict command injection sanitization, and input parameter validation.
  * Responsive Web Management UI with interactive kitchen timer, alarm scheduler, EQ controls, and WiFi telemetry.
* **Overlay Partition Safeguard:**
  * Target firmware partition size in DTS is capped at `0xB30000` (11.18 MB) to guarantee that OpenWrt's JFFS2/SquashFS overlay never overwrites the factory `user` and `user2` partitions holding original Wi-Fi calibration and vendor certificates.

---

## 📦 Ready-to-Flash Binary Releases

Pre-compiled production images:

| File | Size | SHA-256 Checksum | Purpose |
| :--- | :--- | :--- | :--- |
| **`openwrt-23.05.5-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin`** | **9.9 MB** | `0a482a4041b7e4774c723d35965a7d54718a5ee2a45fd26c0ca45deaf65eb1c9` | Full firmware image for permanent installation via `sysupgrade` or U-Boot. |
| `openwrt-ramips-mt76x8-audiopro_c3.manifest` | 4.2 KB | Package Manifest | Complete list of all compiled packages and kernel modules. |
| `sha256sums` | 683 B | Checksums | Cryptographic SHA-256 verification hash list. |

---

## 🚀 Quick Start & Flashing Guide

### Method 1: Safe RAM Boot via U-Boot TFTP (Recommended First Step)
Test the firmware in RAM without touching your speaker's SPI flash:

1. Connect a 3.3V USB-UART adapter to the internal test pads (`57600 8N1`):
   * `GND` → `GND`
   * `RX` → `TX` (UART0)
   * `TX` → `RX` (UART0)
2. Setup a TFTP server on your computer with IP `10.10.10.3` (or `192.168.1.2`).
3. Copy `openwrt-ramips-mt76x8-audiopro_c3-initramfs-kernel.bin` as `openwrt.bin` into your TFTP root folder.
4. Power on the speaker, press any key to enter the U-Boot console, and run:
   ```sh
   setenv ipaddr 10.10.10.123
   setenv serverip 10.10.10.3
   tftpboot 0x80800000 openwrt.bin
   bootm 0x80800000
   ```
5. The speaker boots into OpenWrt in RAM!

### Method 2: Permanent Installation via Sysupgrade
Once booted into OpenWrt (RAM mode) or updating an existing OpenWrt installation:
```bash
scp openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin root@192.168.1.1:/tmp/sysupgrade.bin
ssh root@192.168.1.1 "sysupgrade -v -n /tmp/sysupgrade.bin"
```

---

## 🛠 Building from Source

The firmware is built using the reproducible OpenWrt 23.05 buildroot in a clean Ubuntu 22.04 container.

```bash
# 1. Clone this repository
git clone https://github.com/gimgiwer/audiopro-c3.git
cd audiopro-c3

# 2. Clone OpenWrt 23.05.5
git clone -b openwrt-23.05 https://github.com/openwrt/openwrt.git /tmp/openwrt

# 3. Install Audio Pro C3 BSP overlay
./openwrt/install_to_openwrt.sh /tmp/openwrt

# 4. Build in isolated container
podman run --userns=keep-id --rm -v /tmp/openwrt:/openwrt:Z openwrt-builder:22.04 bash -c "
  cd /openwrt
  ./scripts/feeds update -a && ./scripts/feeds install -a
  make defconfig
  make -j\$(nproc)
"
```

---

## 📚 Technical Documentation

* [OpenWrt Porting & Architecture Guide](docs/OPENWRT_PORTING_GUIDE.md)
* [Custom Firmware Technical Specification](docs/OPENWRT_CUSTOM_FIRMWARE_SPECIFICATION.md)
* [I2S Registers & Hardware Audio Path Reference](docs/I2S_HARDWARE_REGISTERS.md)
* [Network & Firewall Architecture](docs/NETWORK_CONFIGURATION.md)
* [Final Hardware Audit & Security Report](docs/FINAL_AUDIT_REPORT_OPENWRT_AUDIO_PRO_C3.md)
* [Legacy Stock Firmware Analysis & Rooting Guide](docs/STOCK_FIRMWARE_ANALYSIS.md)

---

## 🔄 Factory Stock Firmware & Rollback

For stock firmware research or factory rollback, rooted and stock binaries are preserved:
* `a28audiopro_20211130_stock_unmodified_uImage.bin` (7.56 MB) — Untouched factory uImage (`4.2.337151`).
* `a28audiopro_20211130_mod_telnet_uImage.bin` (7.55 MB) — Rooted stock firmware with Telnet enabled.
* Flash via Web HTTP updater at `http://<speaker-ip>/index.html#systemPage`.

---

## ⚖️ Disclaimer & Trademarks

This is an independent open-source reverse-engineering and hardware preservation project. It is not affiliated with, sponsored, or endorsed by Audio Pro AB or Linkplay Technology Inc. All trademarks belong to their respective owners.
