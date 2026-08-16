# Audio Pro Addon C3 (Linkplay A28 V01) — Complete Engineering & Porting Bundle

> **Purpose:** Standalone engineering guide and reference bundle for system architects and embedded developers porting OpenWrt 5.15 / Linux to the **Linkplay A28 V01** module (MediaTek MT7688AN SoC, 16MB SPI Flash).

---

## Table of Contents

1. [Hardware Architecture & Board Profile](#1-hardware-architecture--board-profile)
2. [Flash Memory Map (MTD Layout) & Overlay Protection](#2-flash-memory-map-mtd-layout--overlay-protection)
3. [Key Architectural Issues & Verified Solutions](#3-key-architectural-issues--verified-solutions)
4. [Reference Device Tree (`mt7628an_audiopro_c3.dts`)](#4-reference-device-tree-mt7628an_audiopro_c3dts)
5. [Kernel Patches & OpenWrt Build System Integration](#5-kernel-patches--openwrt-build-system-integration)
6. [Secondary MCU Daemon & Hardware Volume Architecture (`mcud.c`)](#6-secondary-mcu-daemon--hardware-volume-architecture)
7. [Procd Init Service (`/etc/init.d/mcud`)](#7-procd-init-service)
8. [ALSA Multi-Channel Audio Stack (`/etc/asound.conf`)](#8-alsa-multi-channel-audio-stack)
9. [Zero-Latency TTS Ducking (`/usr/bin/ha_ducking.sh`)](#9-zero-latency-tts-ducking)
10. [Streaming Services (AirPlay, Spotify Connect with Wrapper)](#10-streaming-services)
11. [Network Configuration & System Tuning](#11-network-configuration--system-tuning)
12. [Step-by-Step Deployment Workflow](#12-step-by-step-deployment-workflow)

---

## 1. Hardware Architecture & Board Profile

| Parameter | Chip / Value | Hardware Inspection Details |
| :--- | :--- | :--- |
| **Device** | Audio Pro Addon C3 | Portable Hi-Fi Wireless Active Speaker |
| **Module** | **Linkplay A28 V01** | Black PCB module, UUID `FF280012`, OUI `00:22:6C` |
| **SoC** | **MediaTek MT7688AN** | MIPS 24KEc @ 575 MHz, Little-Endian (`mipsel`), **Soft-Float** (no hardware FPU, MT7628 family) |
| **RAM** | **64 MB DDR2 SDRAM** | Winbond W9751G6KB-25 (512 Mbit, 16-bit, DDR2-800) |
| **Flash** | 16 MB SPI NOR Flash | Winbond W25Q128BV (`0x00000000` – `0x01000000`) |
| **Secondary MCU** | STM8 / Linkplay MCU | Manages 18650 battery charger, top panel pushbuttons, LEDs, input selector and amplifier power/mute |
| **Bluetooth** | BT2 Sub-module | Dedicated daughterboard with 26 MHz crystal. Connected directly to the analog/DSP multiplexer and controlled via MCU |
| **Audio Path** | I2S Master bus → DSP/Amp | MT7688AN generates MCLK/BCLK/WS, I2S Master stream, 44.1 kHz, 16-bit stereo |
| **UART Console** | **3.3V TTL @ 57600 8N1** | Exposed test pads: `GND`, `TX`, `RX`, `3.3V`. In Linux: **`ttyS1`** (via `&uart0`) |
| **UART MCU** | **`/dev/ttyS0` @ 57600 8N1** | Internal communication bus between MT7688AN SoC and Secondary MCU (via `&uart1`) |

---

## 2. Flash Memory Map (MTD Layout) & Overlay Protection

The physical 16 MB SPI NOR flash partition table from `/proc/mtd`:

```text
0x00000000 - 0x00030000 (192 KB)  : "u-boot"       (U-Boot 1.1.3 Ralink 4.3.0.0, 57600 baud, READ-ONLY)
0x00030000 - 0x00040000 (64 KB)   : "u-boot-env"   (Bootloader environment variables, READ-WRITE)
0x00040000 - 0x00050000 (64 KB)   : "factory"      (Wi-Fi RF EEPROM calibration @ 0x04, Ethernet MAC @ 0x28, READ-ONLY)
0x00050000 - 0x00250000 (2 MB)    : "bkKernel"     (Emergency factory backup image 2016, READ-ONLY)
0x00250000 - 0x00D80000 (11.18 MB): "firmware"     (OpenWrt target firmware: Kernel + SquashFS + RootFS_data)
0x00D80000 - 0x00E00000 (512 KB)  : "user"         (JFFS2 writable user configuration partition)
0x00E00000 - 0x01000000 (2 MB)    : "user2"        (JFFS2 vendor certificates, presets, keys)
```

> **CRITICAL: Overlay Partition Protection:**
> The `firmware` partition size in Device Tree is explicitly set to `0xB30000` (11.18 MB = `0xD80000 - 0x250000`). If `firmware` was set to the full flash end (`0xDB0000`), OpenWrt's `rootfs_data` (JFFS2/UBIFS overlay) would format across the `user` and `user2` partitions on first boot, irreversibly erasing vendor certificates and factory presets required for clean rollback to stock firmware.

---

## 3. Key Architectural Issues & Verified Solutions

### 3.1. I2S Master Clock & Fixed 44.1 kHz Sample Rate
* In MT7688AN, the AGPIO/GPIO_MODE register (`0x10000060`) must be set to `0x54154115` to ensure I2S signals (MCLK, BCLK, WS, SDO, SDI) do not conflict with UART or GPIO lines.
* The system is configured for a fixed 44.1 kHz master clock (`mclk-fs = <256>`, MCLK = 11.2896 MHz), providing jitter-free CD-quality audio for Spotify Connect and AirPlay. Other sample rates are seamlessly resampled to 44.1 kHz via ALSA `dmixer`.

### 3.2. SVR4 Operating System Flag for uImage
* U-Boot 1.1.3 on Linkplay devices checks the uImage OS field. Passing `-O svr4` (OS ID 20) during uImage creation in `target/linux/ramips/image/mt7628.mk` ensures compatibility with both standard U-Boot boot scripts and web recovery.

### 3.3. Pure Hardware Master Volume Architecture (Variant A)
* ALSA softvol controls (`Spotify`, `AirPlay`, `Music`, `Notification`) are reset to 100% (0 dB) at boot for pure bit-perfect pass-through to the DAC.
* Master volume is controlled entirely at the hardware level via the secondary MCU sending `AXX+VOL+<000-100>\n` to the Texas Instruments TAS57xx DSP/amplifier.

---

## 4. Reference Device Tree (`mt7628an_audiopro_c3.dts`)

Source file: [`dts/mt7628an_audiopro_c3.dts`](../dts/mt7628an_audiopro_c3.dts)

```dts
// SPDX-License-Identifier: GPL-2.0-or-later
/dts-v1/;

#include "mt7628an.dtsi"
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/input/input.h>

/ {
    compatible = "audiopro,c3", "linkplay,a28", "mediatek,mt7628an-soc";
    model = "Audio Pro C3 (Linkplay A28 V01)";

    aliases {
        serial0 = &uart1;  /* Linkplay Secondary MCU bus (/dev/ttyS0) */
        serial1 = &uart0;  /* Debug Console (/dev/ttyS1) */
    };

    chosen {
        bootargs = "console=ttyS1,57600";
    };

    sound {
        compatible = "simple-audio-card";
        simple-audio-card,name = "AudioPro-C3-I2S";
        simple-audio-card,format = "i2s";

        simple-audio-card,bitclock-master = <&sound0_cpu>;
        simple-audio-card,frame-master = <&sound0_cpu>;
        simple-audio-card,mclk-fs = <256>;

        sound0_cpu: simple-audio-card,cpu {
            sound-dai = <&i2s>;
        };

        simple-audio-card,codec {
            sound-dai = <&codec_dummy>;
        };
    };

    codec_dummy: dummy-codec {
        compatible = "linux,snd-soc-dummy";
        #sound-dai-cells = <0>;
        status = "okay";
    };
};

&spi0 {
    status = "okay";
    flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <40000000>;

        partitions {
            compatible = "fixed-partitions";
            #address-cells = <1>;
            #size-cells = <1>;

            partition@0 {
                label = "u-boot";
                reg = <0x0 0x30000>;
                read-only;
            };

            partition@30000 {
                label = "u-boot-env";
                reg = <0x30000 0x10000>;
            };

            factory: partition@40000 {
                label = "factory";
                reg = <0x40000 0x10000>;
                read-only;
            };

            partition@50000 {
                label = "bkKernel";
                reg = <0x50000 0x200000>;
                read-only;
            };

            /*
             * firmware partition is capped at 0xB30000 (11.18 MB) to prevent
             * OpenWrt's rootfs_data (overlay) from formatting over user (mtd8)
             * and user2 (mtd9) vendor certificates and presets.
             */
            firmware: partition@250000 {
                label = "firmware";
                reg = <0x250000 0xB30000>;
                compatible = "denx,uimage";
            };

            partition@d80000 {
                label = "user";
                reg = <0xd80000 0x80000>;   /* 512 KB */
            };

            partition@e00000 {
                label = "user2";
                reg = <0xe00000 0x200000>;  /* 2 MB */
            };
        };
    };
};

&factory {
    compatible = "nvmem-cells";
    #address-cells = <1>;
    #size-cells = <1>;

    macaddr_factory_wifi_4: macaddr@4 {
        reg = <0x4 0x6>;
    };
    macaddr_factory_eth_28: macaddr@28 {
        reg = <0x28 0x6>;
    };
};

&ethernet {
    nvmem-cells = <&macaddr_factory_eth_28>;
    nvmem-cell-names = "mac-address";
};

&wmac {
    status = "okay";
    mediatek,mtd-eeprom = <&factory 0x0>;
    nvmem-cells = <&macaddr_factory_wifi_4>;
    nvmem-cell-names = "mac-address";
};

&i2s {
    #sound-dai-cells = <0>;
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2s_pins>;
};

&uart0 {
    status = "okay";
};

&uart1 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&uart1_pins>;
};
```

---

## 5. Kernel Patches & OpenWrt Build System Integration

### 5.1. MT7688 I2S FIFO Flush Patch
File: `target/linux/ramips/patches-5.15/836-mt7688-i2s-audio-crash-workaround.patch`

```diff
--- a/sound/soc/ralink/ralink-i2s.c
+++ b/sound/soc/ralink/ralink-i2s.c
@@ -428,7 +428,14 @@ static int ralink_i2s_trigger(struct snd
 	case SNDRV_PCM_TRIGGER_STOP:
 	case SNDRV_PCM_TRIGGER_SUSPEND:
 	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
-		val = 0;
+		/* Flush hardware TX/RX FIFO buffer before stopping clock */
+		val = ralink_i2s_read(priv, RALINK_I2S_CON);
+		val |= I2S_FIFO_CLR;
+		ralink_i2s_write(priv, RALINK_I2S_CON, val);
+		udelay(100);
+		val = 0;
 		break;
 	default:
 		return -EINVAL;
```

### 5.2. OpenWrt Buildroot Integration Script
To build a custom firmware directly from OpenWrt source tree, run the installer:

```bash
./openwrt/install_to_openwrt.sh /path/to/openwrt
```

---

## 6. Secondary MCU Daemon & Hardware Volume Architecture

To prevent UART bus contention between the background heartbeat loop and external CGI/web commands, `mcud` acts as the **single exclusive owner** of `/dev/ttyS0`. External scripts send commands through `/tmp/mcu_cmd_fifo`.

```bash
# Example: Send command to MCU from any script/CGI
echo -e "AXX+VOL+025\n" > /tmp/mcu_cmd_fifo
```

---

## 7. Procd Init Service (`/etc/init.d/mcud`)

```sh
#!/bin/sh /etc/rc.common

START=95
STOP=10

USE_PROCD=1
PROG=/usr/bin/mcud

start_service() {
    local enabled mqtt_enabled mqtt_host mqtt_port mqtt_user mqtt_pass mqtt_prefix

    config_load mcud
    config_get_bool enabled main enabled 1
    [ "$enabled" -eq 1 ] || return 0

    config_get_bool mqtt_enabled main mqtt_enabled 1
    config_get mqtt_host main mqtt_host "127.0.0.1"
    config_get mqtt_port main mqtt_port "1883"
    config_get mqtt_user main mqtt_user ""
    config_get mqtt_pass main mqtt_pass ""
    config_get mqtt_prefix main mqtt_topic_prefix "audiopro_c3"

    local args=""
    if [ "$mqtt_enabled" -eq 1 ]; then
        args="-h $mqtt_host -p $mqtt_port -t $mqtt_prefix"
        [ -n "$mqtt_user" ] && args="$args -u $mqtt_user"
        [ -n "$mqtt_pass" ] && args="$args -P $mqtt_pass"
    else
        args="-m"
    fi

    procd_open_instance
    procd_set_param command "$PROG" $args
    procd_set_param respawn 3600 5 0
    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_set_param user root
    procd_close_instance
}

stop_service() {
    killall -TERM mcud 2>/dev/null
}
```

---

## 8. ALSA Multi-Channel Audio Stack (`/etc/asound.conf`)

```text
# Master hardware mixing device (Direct Mixing)
pcm.dmixer {
    type dmix
    ipc_key 1024
    ipc_perm 0666
    slave {
        pcm "hw:0,0"
        mmap_emulation 1
        period_time 0
        period_size 1024
        buffer_size 4096
        rate 44100
        channels 2
        format S16_LE
    }
    bindings {
        0 0
        1 1
    }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "Spotify"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "AirPlay"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "Music"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default {
    type plug
    slave.pcm "music_in"
}

ctl.!default {
    type hw
    card 0
}
```

---

## 9. Zero-Latency TTS Ducking (`/usr/bin/ha_ducking.sh`)

```sh
#!/bin/sh
ACTION="${1:-play}"
AUDIO_FILE="$2"

duck_down() {
    amixer -q -c 0 sset Music 30%- 2>/dev/null
    amixer -q -c 0 sset Spotify 30%- 2>/dev/null
    amixer -q -c 0 sset AirPlay 30%- 2>/dev/null
}

duck_up() {
    amixer -q -c 0 sset Music 30%+ 2>/dev/null
    amixer -q -c 0 sset Spotify 30%+ 2>/dev/null
    amixer -q -c 0 sset AirPlay 30%+ 2>/dev/null
}

case "$ACTION" in
    start|down) duck_down ;;
    stop|up)    duck_up ;;
    play)
        if [ -n "$AUDIO_FILE" ] && [ -f "$AUDIO_FILE" ]; then
            duck_down
            aplay -q -D music_in "$AUDIO_FILE"
            duck_up
        fi
        ;;
    *) echo "Usage: $0 {start|stop|play <file.wav>}"; exit 1 ;;
esac
```

---

## 10. Streaming Services

### 10.1. AirPlay (`/etc/shairport-sync.conf`)
```text
general = {
    name = "Audio Pro C3";
    output_backend = "alsa";
    mdns_backend = "avahi";
    port = 5000;
};

alsa = {
    output_device = "airplay_in";
    mixer_control_name = "AirPlay";
    audio_backend_buffer_desired_length = 6615; /* Standard 150ms default (customizable to 1323/30ms or 441/10ms) */;
    disable_synchronization = "no";
};
```

### 10.2. Spotify Connect Wrapper (`/usr/bin/librespot-wrapper.sh`)
```sh
#!/bin/sh
TIMEOUT=60
while [ "$(date +%s)" -lt 1700000000 ] && [ $TIMEOUT -gt 0 ]; do
    sleep 3
    TIMEOUT=$((TIMEOUT - 3))
done
exec /usr/bin/librespot "$@"
```

---

## 11. Network Configuration & System Tuning

First-boot script (`/etc/uci-defaults/99-network-init`):
```sh
#!/bin/sh
# 1. LAN Network Configuration
uci set network.lan.proto='static'
uci set network.lan.ipaddr='192.168.1.1'
uci set network.lan.netmask='255.255.255.0'
uci commit network

# 2. System ZRAM Swap (32MB)
if [ -x /etc/init.d/zram-swap ]; then
    uci set system.@system[0].zram_enabled='1'
    uci set system.@system[0].zram_size='32'
    uci commit system
    /etc/init.d/zram-swap enable 2>/dev/null || true
    /etc/init.d/zram-swap start 2>/dev/null || true
fi

exit 0
```

---

## 12. Step-by-Step Deployment Workflow

1. **Safety Step — RAM Boot:**
   ```bash
   python3 tools/one_touch_ram_boot.py
   ```
2. **Flash OpenWrt Sysupgrade to SPI Flash:**
   ```bash
   scp openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin root@192.168.1.1:/tmp/sysupgrade.bin
   ssh root@192.168.1.1 "sysupgrade -n /tmp/sysupgrade.bin"
   ```
