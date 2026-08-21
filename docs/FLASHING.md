# Flashing

## Read this first: what the flash layout destroys

`dts/mt7628an_audiopro_c3.dts` declares one `firmware` partition covering
`0x50000`–`0x1000000`, i.e. everything from the end of `factory` to the end of
the 16 MB chip. The OpenWrt overlay (`rootfs_data`) grows into the tail of that
region, which means the stock **`user` and `user2`** areas — vendor presets and
vendor certificates — are formatted over and gone.

What survives:

| partition | offset | contents | status |
| --- | --- | --- | --- |
| `u-boot` | `0x0` | bootloader | untouched |
| `u-boot-env` | `0x30000` | boot env | untouched |
| `factory` | `0x40000` | **MAC addresses**, wifi calibration | **untouched** |
| `firmware` | `0x50000` | kernel + rootfs + overlay | replaced |

Verified on a running unit: `factory` offset `0x28` still holds the eth0 MAC and
it matches `ip link show eth0`. So flashing does not cost you your MAC or your
radio calibration.

If you would rather keep the vendor regions, shrink `firmware` in the DTS to end
at `0xd80000` and re-declare `user`/`user2` as read-only. That layout is untested
here — it leaves ~11.1 MB for kernel + rootfs + overlay, which fits the 8.7 MB
image but leaves much less room for the overlay.

## Getting the image on

```sh
./build.sh
ls bin/targets/ramips/mt76x8/*-squashfs-sysupgrade.bin
```

**Already running this firmware** — normal sysupgrade:

```sh
scp openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin root@<ip>:/tmp/
ssh root@<ip> 'sysupgrade -n /tmp/openwrt-*-sysupgrade.bin'
```

`-n` drops settings. Without it, sysupgrade keeps `/etc/config`, which will
happily preserve a stale config from an older build.

**Coming from stock, or recovering** — use the Breed bootloader's web recovery
and write the `sysupgrade.bin` to the firmware region. Do not try to sysupgrade
from the vendor OS.

## If it boots but has no network

The speaker has no working serial header from the outside: UART is disabled and
reaching it means opening the case. Plan for that before you flash, because a bad
network config means a physical power cycle and, in the worst case, a SPI flash
clip.

Defaults that make the first boot recoverable:

* `eth0` is a DHCP client (`metric 100`), so plugging it into any router gets you in.
* Wi-Fi STA is disabled with an empty SSID, so it cannot lock you out.
* Setup AP `AudioPro-C3-Setup` / `setup12345` on `10.10.10.254/24` with DHCP.
* `dropbear` on 22, `uhttpd` on 80 and 443, mDNS as `audiopro.local`.
