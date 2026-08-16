# Audio Pro Addon C3 (Linkplay A28 / MediaTek MT7688AN) Rooted Stock Firmware 2021

Custom modified build of the latest official factory firmware for **Audio Pro Addon C3** (Linkplay A28 module, MediaTek MT7688AN SoC, 16MB SPI NOR Flash).

## Features
* **Based on latest official release:** WiiMu `4.2.337151` / Release date: `20211130` (`build: release`).
* **Root Telnet Server:** Auto-starts on port `23` with full PTY support (`telnet <speaker-ip>` or `telnet 10.10.10.254` in default AP setup mode).
* **UART Shell:** Direct root shell on `ttyS0` and `ttyS1` (57600 8N1).
* **Fully functional audio services:** AirPlay, Spotify Connect, TuneIn, Audio Pro Multi-room App, Tidal, Web API (`httpapi.asp`).
* **Valid Dual-uImage Headers:** Includes fixed `"Wiimu Rootfs"` filesystem uImage header with recalculated CRC32, completely avoiding the `"Firmware is broken / backup image"` U-Boot fallback issue.

---

## Documentation

* [Stock Firmware Architecture & Hardware Analysis](docs/STOCK_FIRMWARE_ANALYSIS.md)
* [I2S & Hardware Register Reference](docs/I2S_HARDWARE_REGISTERS.md)
* [OpenWrt 5.15 Porting Guide](docs/OPENWRT_PORTING_GUIDE.md)

---

## Files in this release

| File | Size | Description |
| :--- | :--- | :--- |
| `a28audiopro_20211130_mod_telnet_uImage.bin` | 7.55 MB | **Rooted uImage** with Telnet enabled. Flash via Web HTTP updater, U-Boot TFTP (Option 4), or `mtd write ... firmware` in OpenWrt RAM mode. |
| `a28audiopro_20211130_stock_unmodified_uImage.bin` | 7.56 MB | **Official Clean uImage** (untouched 2021 vendor release). Use to revert back to 100% factory state. |
| `a28audiopro_20211130_mod_telnet_full_14mb.bin` | 13.68 MB (`0xDB0000`) | **14MB Partial Flash dump** (Kernel + RootFS + User2 partitions). For hardware CH341A programmer or low-level chip recovery. |
| `mt7628_i2s_dump` | 54 KB | Standalone static MIPS32 binary to inspect `/dev/mem` hardware registers (PINMUX, I2S, GDMA) in real-time. |

---

## Official Vendor CDN Sources (Linkplay / Audio Pro)

For transparency and independent verification, you can download original factory firmware directly from Linkplay OTA servers:

* **Product Manifest:** [`product.xml`](http://silenceota.linkplay.com/wifi_audio_image/PoyrfPSq2E5HML9RfUHyha/product.xml)
* **Official 2021 Firmware (`4.2.337151`):** [`a28audiopro_new_uImage_20211130`](http://silenceota.linkplay.com/wifi_audio_image/PoyrfPSq2E5HML9RfUHyha/20211130/a28audiopro_new_uImage_20211130)
  * MD5: `a7f0c6a8958ee77ad6ad07949d994274`
  * Size: `7,922,434` bytes
* **Factory Rescue Kernel (`mtd3`):** [`backup_new_v1141.img`](http://silenceota.linkplay.com/wifi_audio_image/PoyrfPSq2E5HML9RfUHyha/backup_new_v1141.img) (MD5: `bc125966a1c9033830f900af87d4be40`)
* **Factory U-Boot (`mtd1`):** [`uboot_v632.img`](http://silenceota.linkplay.com/wifi_audio_image/PoyrfPSq2E5HML9RfUHyha/uboot_v632.img) (MD5: `cdb9d73905e036da5137a989fd2e2a2d`)

---

## Flashing Instructions

### Method 1: Via Web HTTP Updater (Easiest)
1. Open the speaker's web interface in your browser:
   * When connected to your home Wi-Fi: `http://<speaker-ip>/index.html#systemPage`
   * When connected to the speaker's direct Wi-Fi hotspot (`AudioPro_C3_xxxx`): `http://10.10.10.254/index.html#systemPage`
2. In the System / Firmware Upgrade section, select and upload `a28audiopro_20211130_mod_telnet_uImage.bin` (or `a28audiopro_20211130_stock_unmodified_uImage.bin` to return to stock).
3. Wait for the upload and flashing process to complete, then the speaker will automatically reboot.

### Method 2: Via OpenWrt RAM Boot (Safest for Recovery)
Boot OpenWrt temporarily into SDRAM (without touching SPI Flash), verify the system, then flash:
```bash
# 1. Boot OpenWrt in SDRAM (U-Boot Menu Option 5):
python3 tools/one_touch_ram_boot.py

# 2. Upload and flash the rooted uImage (7.55MB) to the firmware partition:
scp a28audiopro_20211130_mod_telnet_uImage.bin root@192.168.1.1:/tmp/fw.bin
ssh root@192.168.1.1 "mtd write /tmp/fw.bin firmware && sync && reboot"
```

### Method 3: Via U-Boot TFTP (Option 4)
For standard U-Boot recovery directly into Flash:
1. Setup a TFTP server with IP `192.168.1.202`.
2. Place `a28audiopro_20211130_mod_telnet_uImage.bin` (or stock) as `wiimu_uImage` in your TFTP root directory.
3. Connect to UART (`/dev/ttyUSB0` @ 57600 8N1).
4. Power on the speaker and press `4` ("Load system code then write to Flash via TFTP") in the U-Boot boot menu.
5. Set device IP to `192.168.1.122`, server IP to `192.168.1.202`, and filename to `wiimu_uImage`.
6. Wait for flashing to complete (`Done!`).

---

## ⚠️ Recovery Plan (Troubleshooting)

### Auto-Fallback (bkKernel)
If the main firmware partition is corrupted, the custom Linkplay U-Boot will automatically boot the emergency factory backup kernel in `mtd3` (2016 backup mode). The web interface will show `Firmware is broken!`. You can still recover the speaker via Web UI, TFTP, or UART.

### Hardware Programmer (CH341A)
If U-Boot is inaccessible or flash is completely wiped:
1. Disconnect speaker power and battery.
2. Attach a CH341A programmer with SOIC8 clip to the SPI NOR Flash (Winbond W25Q128).
3. Flash the 16MB full dump (or the 14MB image padded to 16MB):
   ```bash
   flashrom -p ch341a_spi -w a28audiopro_20211130_mod_telnet_full_14mb.bin
   ```

---

## Accessing Root Shell

Connect via Telnet to the speaker's assigned IP (or default `10.10.10.254` when connected to its `AudioPro_C3_xxxx` hotspot):
```bash
telnet <speaker-ip>
```
Output:
```text
BusyBox v1.12.1 () built-in shell (ash)
Enter 'help' for a list of built-in commands.

# uname -a
Linux AudioPro_C3 3.10.14 #10 Tue Nov 30 12:13:35 CST 2021 mips GNU/Linux
```

---

## Next Steps (OpenWrt Porting)
Using the hardware register dumps captured from this rooted stock firmware:
1. Refine OpenWrt Device Tree with verified Pinmux (`0x54154115`) and MTD layout (`0xB30000` firmware).
2. Implement proper hardware FIFO flush in `ralink-i2s.c` kernel driver to replace the idle clock-lockup workaround.
3. Validate clean bit-perfect playback (44.1 kHz) with Shairport-sync and Librespot on OpenWrt 5.15.

---

## Disclaimer & Trademarks

This is an independent open-source reverse-engineering and research project dedicated to hardware preservation, interoperability, and right-to-repair. This project is not affiliated with, endorsed by, or associated with Audio Pro AB or Linkplay Technology Inc. All product names, logos, brands, and trademarks are property of their respective owners.
