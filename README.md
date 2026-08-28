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
| `squeezelite` | slimproto, so Music Assistant and LMS see the speaker as a player |
| `mcud` | front-panel MCU over UART: buttons, LEDs, battery, charge state; MQTT bridge and Music Assistant volume/transport over the LMS CLI |
| `uhttpd` + Lua | web UI and JSON API on `/api` |
| `avahi` | mDNS, so the speaker is `audiopro.local` |
| `chronyd` | the clock — there is no RTC on this board, so it starts wrong every boot |
| `dnsmasq` | DHCP for the setup access point |

`librespot`, `shairport-sync` and `avahi` are the `standalone` half of the service
profile below; `smarthome` mode stops them.

`asound.conf` also declares `Music`, `TTS`, `Alarm` and `Timer` inputs with faders
of their own. Nothing in the image plays through them by itself — they are for
`ha_ducking.sh`, `smart_alarm.sh` and whatever else you point at them.

## Audio path

```
service -> softvol (per input) -> softvol "Master" -> dmix -> hw:0,0
        -> ralink-i2s (master) -> GDMA -> PCM5102A DAC
```

* dmix runs 44100 Hz S16_LE, `period_size 1024`, `buffer_size 16384` — a 371 ms
  ring. That is deliberately generous: `AlsaSink` in librespot is synchronous, so
  decode and output share one thread and the ring is the only slack there is.
* One `Master` control spans −60…0 dB over 1001 steps. The steps are that fine on
  purpose: librespot polls the control every second and reads anything bigger than
  0.6 % of the range as somebody else moving the knob, so a coarse control walks
  itself down.
* The eight per-input controls (`Spotify`, `AirPlay`, `Squeeze`, `Slimproto`,
  `Music`, `TTS`, `Alarm`, `Timer`) sit in front of it and are what you duck for
  notifications. `Slimproto` is the one Music Assistant moves; it spans −23…0 dB
  because squeezelite has no dB info on a softvol and falls back to a linear map.
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
patches/librespot/           the Tremor port and the volume law
patches/alsa-lib/            softvol dither and gain ramp
dts/                         device tree
image/audiopro_c3.mk         device profile, appended to mt76x8.mk
config/audiopro_c3.diffconfig   config seed for `make defconfig`
openwrt/package/utils/mcud/  the MCU daemon package
files/                       root overlay, exactly what ships in the image
homeassistant/               the HA package for the media card
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

### Service profile

Which of the speaker's own services run is one switch:

```sh
uci set profile.main.mode='smarthome'    # or 'standalone'
uci commit profile
/etc/init.d/profile start
```

`standalone` is the factory-like set: Spotify Connect, AirPlay and the slimproto
player side by side. `smarthome` keeps only what Music Assistant and Home
Assistant need and stops everything in `list disable` of `/etc/config/profile` —
by default `dbus`, `avahi-daemon`, `librespot`, `shairport-sync`.

The reason is the volume. Every source owns a fader in front of `Master`, so a
phone that reaches librespot or AirPlay directly moves a level Music Assistant
knows nothing about, and the front panel, the HA slider and the MA slider then
show three different numbers. With those two gone there is exactly one fader
being moved, and the profile also sets mcud's `ma_owns_volume`, which keeps the
front-panel keys on MA's own fader even while its player is idle. The price:
no direct Spotify Connect and no AirPlay (MA still streams Spotify over
slimproto), and `audiopro.local` stops resolving with avahi off.

Each service gets both of its switches flipped — the `/etc/rc.d` symlink and the
service's own uci `enabled` flag — because an `enable` from anywhere else would
otherwise bring it back on the next boot. `/etc/init.d/boot-order` reads the same
list and skips those services instead of starting them for `profile` to stop
again. Keep the list in dependency order: `standalone` starts it front to back,
and shairport-sync needs dbus and avahi already up.

### Home Assistant

`mcud` publishes MQTT discovery on its own (volume, mute, input, transport, sleep
timer, restart, power off), but MQTT discovery has no `media_player` platform, so
artwork, title and transport have to come from the Music Assistant player.
`homeassistant/audiopro_c3.yaml` fuses the two into one card: drop it in
`/config/packages/`, add

```yaml
homeassistant:
  packages: !include_dir_named packages
```

to `configuration.yaml` and reload YAML. Do not add automations that mirror volume
between the two sides or bridge the play/pause button — the speaker already owns
both, and a second writer stacks the faders or double-toggles the press.

### Clock

There is no RTC. Every boot starts on whatever `sysfixtime` guesses from the newest
file on flash, and chronyd steps the clock forward once the network is up — which is
why nothing in `mcud` measures time with the wall clock; every deadline in it is
`CLOCK_MONOTONIC`. Servers live in `/etc/config/chrony`.

## Status and honesty

This is a hobbyist port of a speaker with no public documentation, so it is worth
being blunt about what is and is not established. `docs/` was written across
several passes and earlier revisions contained claims that later turned out to be
untrue; those have been corrected in place rather than quietly deleted, and
`docs/I2S_HARDWARE_REGISTERS.md` documents which ones and why. If you find
something in here that disagrees with your hardware, trust your hardware.

Known open items: the meaning of `CFG0` bit 16 is established empirically but not
from documentation; the front-panel MCU re-runs its init poll on a ~20 minute timer
for reasons that are not yet understood; and the AUX input still plays nothing.

The input selector itself is no longer a guess. `AXX+PLM+nnn` is stock's
source-mode channel — `000` Wi-Fi, `005` line-in, `006` Bluetooth, and eleven more
codes this cabinet has no jack for — so `mcud` now answers `MCU+PLM+GET` with the
current source and announces every switch, and the front-panel source key arrives
as `MCU+PLM+nnn` and lands on the right label. What is missing is the audio: stock
routed line-in with `MCU+BYP+001` → `ioctl(/dev/i2s_config, 0x2005)`, an I2S
loopback in the vendor driver. Our driver has the same bit (`CFG1` bit 30,
`EXTLBK`) but no knob to set it, and whether the carrier board even has an ADC on
I2S SDI is untested. See
[docs/STOCK_FIRMWARE_ANALYSIS.md](docs/STOCK_FIRMWARE_ANALYSIS.md).

## Licence

GPL-2.0, matching OpenWrt and the kernel patches. See [LICENSE](LICENSE).
