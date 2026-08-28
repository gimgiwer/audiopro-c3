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
  - Set hardware volume: `AXX+VOL+020\n` (range `000` to `100`).
  - Play / pause is the `PLY` family: `AXX+PLY+00%d` (`MCUCommandSend` id 17) and the
    raw `AXX+PLY+PUS\n` that `GNOTIFY=PLM_PAUSE` sends through the passthrough id 35.
    That misleading notify name is where this file's old "PLM is play/pause" claim
    came from; it was wrong.
  - `AXX+INP+*` occurs zero times anywhere in the stock rootfs, so the MCU never
    understood it. The real selector is `PLM`, below.

### 2.1 Input switching (`PLM` = play **source** mode)

Recovered from `system/workdir/bin/mv_ioguard`. Both directions are numeric:

| direction | wire | meaning |
| --- | --- | --- |
| host → MCU | `AXX+PLM+%03d` (`MCUCommandSend` id 1) | user picked a source; fed by `GNOTIFY=PLAY_SOURCE_MODE:<n>` (`cmdSocket_recive` `0x410c28`) |
| host → MCU | `AXX+PLM-%03d` (id 38) | the box switched by itself; fed by `GNOTIFY=PLAY_AUTO_SWITCH:<n>` |
| host → MCU | `AXX+PLM+GET` (id 27) | ask the MCU |
| MCU → host | `MCU+PLM+nnn` | MCU picked a source (front-panel key) |
| MCU → host | `MCU+PLM+GET` | MCU asks us; stock answers id 1 with its own mirror at `cfg+968` (`ParseCommonMCUEvent` `0x41783c`) |

Values ≥ 101 go out as `AXX+PLM+FFF` — the dispatcher range-checks with
`sltiu v0,a3,101` at `0x40cf5c`.

Inbound `MCU+PLM+nnn` becomes event 12, which is `Function_SwitchMode`
(`0x0040720c`). It turns the three digits into a name and writes
`setPlayerCmd:switchmode_by_mcu:<name>` into the FIFO `/tmp/Requesta01controller`,
where `a01controller` does the actual switch:

```
000 wifi   004 udisk    005 line-in   006 bluetooth  007 mirror   008 optical
009 RCA    010 co-axial 011 FM        012 line-in2   013 XLR      015 HDMI
014 switch_DAC_by_mcu:TFcard         017 cd
```

If the app is not up yet it falls back to `key_id_press:mode` on
`/tmp/RequestGoheadCmd`; an unknown code goes to `/tmp/pusheventinfo` as
`ButtonEvent:mode`. The same function is reached from the app side by
`GNOTIFY=INPUTMODESWITCH` (`0x4151f4`). Five OEM projects (`BUSH_31326`,
`JENSEN_31324`, `KS_31330`, `TIBO_31322/3`) remap code `016` to a different event;
the C3 is not one of them.

### 2.2 Line-in audio is an I2S loopback, not a mux

`MCU+BYP+nnn` → event 64 → `Function_I2SBypassMode` (`0x004076c0`) writes
`i2sbypasson` (`001`/`002`) or `i2sbypassoff` (`000`) to the same FIFO.
`a01controller` then calls `WMUtil_Set_I2SByPassMode()` in `lib/libmvmsg.so`
(`0x4a60`), which is nothing but:

```c
fd = open("/dev/i2s_config", O_RDWR);
ioctl(fd, on ? 0x2005 : 0x2004, arg);
close(fd);
```

`/dev/i2s_config` is created by `system/workdir/live.sh:98` as
`mknod /dev/i2s_config c 221 0`; siblings are `WMUtil_Set_I2SMode` (0x2001/0x2002)
and `WMUtil_Set_I2SFormat` (0x2006/0x2007). So the analog jack is wired to an I2S
receiver and the vendor driver simply loops RX back into TX in hardware — no GPIO,
no relay, no mux string exists anywhere in `a01controller`. There is a software
capture→playback thread as fallback, which is what
`!!!Exception: line-in mode is playing, not i2sbypass, but thread has quit` refers
to.

Our own `patches/835-asoc-add-mt7620-support.patch` already carries that bit:
`I2S_REG_CFG1_EXTLBK BIT(30)` at `CFG1` offset `0x18`, plus the whole RX side
(`CFG0_RX_EN BIT(20)`, `capture_dma_data`, RX threshold/overrun interrupts). The
card is playback-only because the PCM5102A is a playback-only codec, so exercising
this needs a driver-side knob and a rebuild — there is no `devmem` on the device.

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
