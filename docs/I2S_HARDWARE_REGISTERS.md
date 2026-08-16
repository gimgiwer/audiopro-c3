# Reference Hardware Registers: MediaTek MT7628AN (Audio Pro Addon C3)

Captured from a live factory firmware session on **Audio Pro Addon C3 (Linkplay A28 V01)**:
* **Firmware:** `4.2.337151`
* **Release:** `20211130` (`build: release`)
* **Inspection Method:** Direct physical memory inspection via `/dev/mem` using `src/i2s_dump.c`.

---

## 1. Pinmux & Clock Gating Configuration

```text
CHIP_ID (0x10000000):
  [0x10000000] = 0x3637544D ("MT")
  [0x10000004] = 0x20203832 ("28  ")
  [0x1000000C] = 0x00010102 (MT7628AN Rev 2)

CLKCFG (Clock configuration):
  [0x1000002C] = 0x0020100C
  [0x10000030] = 0xFBFFFFC0
  [0x10000034] = 0x04000000
  [0x10000038] = 0xC0030200

AGPIO_CFG / GPIO_MODE (Pinmux):
  [0x10000060] = 0x54154115   <--- REFERENCE PINMUX VALUE FOR OPENWRT DTS
  [0x10000064] = 0x05540554
```

> **Key for OpenWrt DTS:** Register `0x10000060` = `0x54154115` configures the hardware pin multiplexing for I2S (MCLK, BCLK, WS, SDO, SDI), UART1, and GPIO lines.

---

## 2. I2S Controller Registers (Base: `0x10000A00`)

| Offset | Register | Value | Description |
| :--- | :--- | :--- | :--- |
| `0x10000A00` | `I2S_REG_CFG0` | `0xE1054040` | I2S Enable, Master Mode, FIFO setup |
| `0x10000A04` | `I2S_REG_INT_STATUS` | `0x00000000` | Interrupt status |
| `0x10000A08` | `I2S_REG_INT_EN` | `0x00000000` | Interrupt mask |
| `0x10000A0C` | `I2S_REG_FF_STATUS` | `0x00001003` | FIFO buffer status |
| `0x10000A10` | `I2S_REG_WREG` | `0x00001003` | DMA write port to I2S FIFO |
| `0x10000A14` | `I2S_REG_RREG` | `0x00000000` | FIFO read port |
| `0x10000A18` | `I2S_REG_CFG1` | `0x00000000` | Channel configuration |
| `0x10000A1C` | `I2S_REG_DIVINT` | `0x00000000` | Integer MCLK clock divider |
| `0x10000A20` | `I2S_REG_DIVCOMP` | `0x00000000` | Fractional MCLK clock divider |
| `0x10000A24` | `I2S_REG_FLAGS` | `0x00000000` | Controller flags |

---

## 3. General DMA (GDMA) Controller Registers (Base: `0x10002800`)

```text
GDMA Channels Configuration:
  [0x10002800] = 0x03F05000
  [0x10002804] = 0x10000A10   <--- Destination points to I2S FIFO WREG (0x10000A10)
  [0x10002808] = 0x10000046
  [0x1000280C] = 0x00200209
  [0x10002810] = 0x03F06000
  [0x10002814] = 0x10000A10   <--- Second DMA channel destination also points to I2S FIFO WREG
  [0x10002818] = 0x10000046
  [0x1000281C] = 0x00200211
```

---

## 4. Real-time ALSA Hardware Parameters during Playback

During active Spotify Connect / AirPlay playback at 44.1 kHz:

```text
Sound Card:      0 [mtksnd]: noop - mtk_snd (MediaTek SoC I2S Master)
Format:          S16_LE (16-bit Signed Little-Endian)
Channels:        2 (Stereo)
Sample Rate:     44100 Hz (44.1 kHz)
Period Size:     1024 frames
Buffer Size:     8192 frames
Access:          MMAP_INTERLEAVED
```
