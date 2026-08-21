# Audio Pro Addon C3 (Linkplay A28 V01) Stock Firmware Architecture & Hardware Analysis

Deep technical breakdown of the factory firmware captured from a running Audio Pro Addon C3 speaker via Telnet and hardware inspection.

---

## 1. Audio Subsystem (ALSA & I2S)

* **Kernel Sound Card (`cat /proc/asound/cards`):**
  ```text
  0 [mtksnd         ]: noop - mtk_snd
                        mtk_snd (noop)
  ```
* **PCM Device (`cat /proc/asound/pcm`):**
  ```text
  00-00: mtk pcm noop-0 :  : playback 1 : capture 1
  ```
* **Hardware I2S Registers during active playback (`cat /proc/pcm_debug`):**
  ```text
  ========== playback info =============
  pcm_start:1
  period_size:0x1000
  dma_area:0xa3f00000
  ========== I2S Reg. info =============
  0x00 = 0xe1054040   (I2S Control: Enabled, Master mode, internal clock, 16-bit 44.1/48 kHz)
  0x04 = 0x0
  0x08 = 0x0
  0x0c = 0x00001003   (TX DMA enabled, FIFO depth)
  0x10 = 0x00001003   (RX DMA enabled)
  ```
* **Key Takeaway for OpenWrt Porting:**
  - There is **no external I2C audio codec chip** connected directly to ALSA (in stock it is `noop`, in OpenWrt 5.15 it maps to `linux,spdif-dit` / `CONFIG_SND_SOC_SPDIF=y` with `simple-audio-card`).
  - The I2S digital audio stream flows directly from the MT7628 SoC into the TAS57xx / DSP power amplifier.
  - The SoC DMA controller (`CONFIG_DMA_RALINK=y` / `Ralink_DMA` on IRQ 7) is mandatory for continuous I2S streaming without dropouts.

---

## 2. Power, Volume & Amplifier Control (MCU Protocol)

* **Microcontroller Communication (STM8 / Linkplay MCU):**
  - Communicates via UART `/dev/ttyS0` @ **57600 baud, 8N1** (verified in NVRAM: `wiimuuartconfig=57600,8,N,1`).
  - Serial console port is `/dev/ttyS1` @ 57600 (`console=ttyS1,57600n8`).
* **Linkplay MCU ASCII Protocol Commands:**
  - Power on / un-mute amplifier: `AXX+PLM+001\n` followed by `AXX+MUT+000\n`.
  - Set hardware volume: `AXX+VOL+020\n` (range `000` to `100`).
  - There is **no input-selector opcode**. `AXX+INP+*` was documented here earlier but
    it occurs zero times anywhere in the stock rootfs, so the MCU never understood it.
  - `AXX+PLM+%03d` is play/pause state, not a source selector — `mv_ioguard` emits
    `GNOTIFY=PLM_PAUSE` / `GNOTIFY=PLM_RESUME` for it. Out-of-range values are sent as
    `AXX+PLM+FFF`; the dispatcher range-checks `value < 101` (mv_ioguard `0x40cf5c`).

---

## 3. Flash Memory Layout (16MB SPI NOR Flash, `/proc/mtd`)

| Partition | Offset | Size | Purpose |
| :--- | :--- | :--- | :--- |
| `mtd0` | `0x00000000` | 16 MB | **ALL** (full SPI Flash chip) |
| `mtd1` | `0x00000000` | 192 KB | **Bootloader** (U-Boot) |
| `mtd2` | `0x00030000` | 64 KB | **Config** (NVRAM environment table) |
| `mtd3` | `0x00040000` | 64 KB | **Factory** (EEPROM, Wi-Fi RF calibration, MAC addresses) |
| `mtd4` | `0x00050000` | 2 MB | **bkKernel** (Emergency factory backup kernel 2016) |
| `mtd5` | `0x00250000` | ~1.7 MB | **Kernel** (Primary Linux uImage kernel 2021) |
| `mtd6` | `0x00407302` | ~9.5 MB | **RootFS** (SquashFS 4.0 XZ) |
| `mtd7` | **`0x00250000`** | **11.18 MB** | **Kernel_RootFS** (Target partition for OpenWrt `firmware`) |
| `mtd8` | `0x00d80000` | 512 KB | **user** (JFFS2 writable config partition) |
| `mtd9` | `0x00e00000` | 2 MB | **user2** (JFFS2 vendor certificates & keys) |

> **Note:** the table above is the *stock* layout. Our firmware partition spans
> `0x50000`–`0x1000000`, so `bkKernel`, `user` and `user2` are overwritten on flash.
> `factory` (MAC + Wi-Fi calibration) sits below it and survives. See `docs/FLASHING.md`.
