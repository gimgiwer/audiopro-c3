# OpenWrt firmware for the Audio Pro Addon C3

Open replacement firmware for the **Audio Pro Addon C3** wireless speaker
(Linkplay A28 V01 module, MediaTek **MT7628AN**, 64 MB DDR2, 16 MB SPI NOR),
built on **OpenWrt 23.05.5** with **Linux 5.15.167**.

The stock firmware is a vendor Linux 3.10 build with a closed MediaTek audio
stack. This replaces all of it: a mainline-style ASoC I2S driver, a working GDMA
path, an open MCU daemon for the front panel, and Spotify Connect / AirPlay that
you can actually rebuild yourself.

```sh
git clone https://github.com/gimgiwer/audiopro-c3
cd audiopro-c3
./build.sh
```

Read [docs/FLASHING.md](docs/FLASHING.md) before writing anything to flash — the
layout reclaims the vendor `user`/`user2` regions (the MAC address is preserved).

## What actually runs

Verified on a running unit, not aspirational:

| service | what it does |
| --- | --- |
| `librespot` | Spotify Connect, patched for fixed-point Vorbis (see below) |
| `shairport-sync` | AirPlay 1 |
| `mcud` | front-panel MCU over UART: buttons, LEDs, battery, charge state; MQTT bridge |
| `uhttpd` + Lua | web UI and JSON API on `/api` |
| `avahi` | mDNS, so the speaker is `audiopro.local` |
| `dnsmasq` | DHCP for the setup access point |

`asound.conf` also declares inputs for Squeezelite, DLNA, Snapcast and SIP. Those
daemons are **not** in the image — the mixer inputs exist, the software does not.
Add them to `DEVICE_PACKAGES` in `image/audiopro_c3.mk` if you want them.

## Audio path

```
service -> softvol (per input) -> softvol "Master" -> dmix -> hw:0,0
        -> ralink-i2s (master) -> GDMA -> PCM5102A DAC
```

* dmix runs 44100 Hz S16_LE, `period_size 1024`, `buffer_size 16384` — a 371 ms
  ring. That is deliberately generous: `AlsaSink` in librespot is synchronous, so
  decode and output share one thread and the ring is the only slack there is.
* One `Master` control spans −60…0 dB over 101 steps; the eight per-input controls
  (`Spotify`, `AirPlay`, `Squeeze`, `Music`, `TTS`, `VoIP`, `Alarm`, `Timer`) span
  −51…0 dB and are what you duck for notifications.
* The DAC is a PCM5102A with no control bus — `/sys/bus/i2c/devices/` is empty.
  Everything is set by the I2S clock alone. Register-level detail and a live dump:
  [docs/I2S_HARDWARE_REGISTERS.md](docs/I2S_HARDWARE_REGISTERS.md).

## The two kernel patches that matter

`patches/` is the part that makes audio work at all. The device tree alone is not
enough.

**`835-asoc-add-mt7620-support.patch`** — ASoC I2S driver for the MT7620/MT7628
family, plus three fixes found on this hardware:

* `CFG0` bit 28 byte-swaps every sample. Upstream set it whenever the SoC
  *advertised* big-endian capability, which shredded little-endian PCM into
  full-scale white noise. It is now gated on the runtime format actually being
  big-endian.
* `CFG0` bit 16 must be set or BCLK never paces the FIFO.
* The TX FIFO is flushed on `TRIGGER_START` to clear a leftover underrun.
* `DIVINT` lives at offset `0x24`, not `0x1c`.

**`836-drivers-staging-ralink-gdma-fixes.patch`** — two hangs in the GDMA driver:

* `terminate_all()` used to spin until the channel drained. ALSA calls it from
  `snd_pcm_drop()` with interrupts off, so jiffies never advance, the timeout can
  never fire, and the box wedges hard on every abrupt stop. It now kills the
  channel directly.
* Aborting a channel leaked the driver's in-flight counter. After two aborts the
  tasklet's two-transfer limit stayed blocked and DMA stopped silently.

## librespot with a fixed-point decoder

The 24KEc core has **no FPU**. Upstream librespot decodes Vorbis with symphonia,
in floating point, which the kernel has to emulate — it eats most of the CPU and
starves the ALSA writer, which you hear as a stutter every second or so. Stock
firmware used Tremor, the integer-only Vorbis decoder, for exactly this reason.

`patches/librespot/0001-tremor-fixed-point-vorbis-decoder.patch` adds a
`tremor-decoder` feature (C shim + decoder + build glue) on top of upstream
`9c7d756`. Build it with:

```sh
scripts/build_librespot.sh ./openwrt-src
```

`files/usr/bin/librespot` in this repo is that binary, prebuilt. It is dynamically
linked against the musl and ALSA libraries in the image.

## Layout

```
build.sh                     clone OpenWrt, graft this on, build
scripts/install_to_openwrt.sh   graft onto an existing OpenWrt tree
scripts/build_librespot.sh      cross-build librespot with Tremor
patches/                     kernel patches (I2S + GDMA)
patches/librespot/           the Tremor port
dts/                         device tree
image/audiopro_c3.mk         device profile, appended to mt76x8.mk
config/audiopro_c3.diffconfig   config seed for `make defconfig`
openwrt/package/utils/mcud/  the MCU daemon package
files/                       root overlay, exactly what ships in the image
docs/                        hardware notes, flashing, network, porting
src/ services/ tools/        standalone RE and bring-up utilities
```

## Networking

HTTP and HTTPS are both served, over IPv4 and IPv6, so you can pick. Verified
against a running unit:

| | HTTP | HTTPS |
| --- | --- | --- |
| IPv4 | 200 | 200 |
| IPv6 | 200 | 200 |
| `audiopro.local` | 200 | 200 |

TLS is mbedtls with a self-signed **EC P-256** certificate generated by `px5g` on
first boot into `/etc/`, so no private key ships in this repo. EC matters here:
with an RSA-2048 cert the handshake took **1.25 s** on this CPU; with P-256 it is
**0.36 s**, and the ECDHE-ECDSA ciphersuites can finally be negotiated.

There is no firewall in this image (`firewall4` and the nftables kmods are
deselected in the config seed) because the speaker sits on a LAN and every
removed package is flash and RAM back. Re-select `firewall4` in the config if you
want it.

## Configuration

`files/` ships neutral defaults, no personal data: empty Wi-Fi STA SSID, setup AP
`AudioPro-C3-Setup` / `setup12345`, `UTC`, hostname `audiopro`, no MAC addresses.
Set your own network from the web UI or `uci`.

## Status and honesty

This is a hobbyist port of a speaker with no public documentation, so it is worth
being blunt about what is and is not established. `docs/` was written across
several passes and earlier revisions contained claims that later turned out to be
untrue; those have been corrected in place rather than quietly deleted, and
`docs/I2S_HARDWARE_REGISTERS.md` documents which ones and why. If you find
something in here that disagrees with your hardware, trust your hardware.

Known open items: the meaning of `CFG0` bit 16 is established empirically but not
from documentation, and the front-panel MCU re-runs its init poll on a ~20 minute
timer for reasons that are not yet understood.

## Licence

GPL-2.0, matching OpenWrt and the kernel patches. See [LICENSE](LICENSE).
