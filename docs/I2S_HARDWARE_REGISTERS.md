# I2S / GDMA registers on the MT7628AN (Audio Pro Addon C3)

> **This file was rewritten on 2026-08-21.** The previous version presented a
> register table as a "live dump from factory firmware via `/dev/mem` using
> `src/i2s_dump.c`". That capture never happened and the values in it were
> wrong — see [Why the old table was wrong](#why-the-old-table-was-wrong).
> Everything below is copy-pasted from the running speaker.

## How to read the registers

`/dev/mem` does not exist on this firmware (`CONFIG_DEVMEM` is off), so
`i2s_dump.c` and anything else that mmaps physical memory cannot work. The I2S
block is exposed through regmap debugfs instead:

```sh
cat /sys/kernel/debug/regmap/10000a00.i2s/registers
```

## Live capture, card idle (no stream open)

```text
000: e0054040   CFG0    enable / DMA / format / thresholds
004: 00000000   INT_STATUS
008: 00000000   INT_EN
00c: 00001003   FF_STATUS   FIFO level
010: 00001003   WREG        DMA write port
014: 00000000   RREG
018: 00000000   CFG1
020: 80000022   DIVCMP      fractional divider
024: 000000aa   DIVINT      integer divider = 170
100: 00160001
```

Offsets `0x028`–`0x0fc` all read back `000000aa`. The hardware only decodes a
few registers in that window, so `DIVINT` is mirrored across the rest; it is
aliasing, not 54 real registers.

Note `DIVINT` is at **0x24**, not 0x1c. Getting this wrong shifts every clock
write by two registers and you get no BCLK at all.

## Clock

    BCLK = 480 MHz / (DIVINT + DIVCMP_fraction/512),  BCLK = 64 x fs

With the values above at 44.1 kHz: `DIVINT = 170 (0xaa)`, `DIVCMP = 0x80000022`.
That is 480e6/170 = 2.824 MHz = 64 x 44.1 kHz, which checks out.

## Audio topology, as reported by the running kernel

```text
card 0: AudioProC3I2S [AudioPro-C3-I2S], simple-card
codec:  snd_soc_pcm5102a      (TI PCM5102A, hardware-strapped)
cpu:    snd_soc_ralink_i2s
dma:    ralink_gdma
```

There is **no I2C bus in use** — `/sys/bus/i2c/devices/` is empty. The PCM5102A
has no control interface; its format and de-emphasis are strapped in hardware.
So there is nothing to configure from software beyond the I2S clock itself.

`snd_soc_wm8960` is also loaded but has a refcount of 0. It is pulled in by the
kmod package and is not part of this board's path.

## Why the old table was wrong

| claim in the old file | what the hardware says |
| --- | --- |
| captured via `/dev/mem` + `i2s_dump.c` | `/dev/mem` does not exist on this build |
| `CFG0 = 0xE1054040` | `0xe0054040` |
| `DIVINT = 0x00000000` at offset `0x1C` | `0x000000aa` at offset `0x24` |
| `DIVCOMP = 0x00000000` | `0x80000022` |
| amplifier is a TAS5707 driven over I2C | codec is a PCM5102A, no I2C devices bound |
| `noop` codec, card `mtksnd` | `simple-card`, card `AudioPro-C3-I2S` |

The GDMA table in the old file (`0x10002800`…) was presented the same way and
came from the same non-existent capture. Live GDMA state is visible through
`/sys/kernel/debug/dmaengine/` and the driver in
`patches/836-drivers-staging-ralink-gdma-fixes.patch`.
