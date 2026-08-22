# /overlay/upper vs repository - reconciliation (2026-08-23)

Full sweep of the speaker's overlay (200 entries: 129 files, 21 symlinks,
20 whiteouts, 30 dirs) against `files/` and the package/image build.
Opcodes: A=repo matches, B=repo differs, C=not in repo, D=runtime-generated,
E=diagnostic/backup, F=whiteout.

## A - in repo, byte-identical (24)

`/etc/avahi/avahi-daemon.conf`, `/etc/asound.conf`, `/etc/shairport-sync.conf`,
`/etc/rc.local`, `/etc/modules.d/30-sound-core`, `/etc/modules.d/90-sound-mt7620`,
`/etc/config/dropbear`, `/etc/init.d/{librespot,boot,shairport-sync,mcud,squeezelite}`,
`/usr/bin/{audio_arbiter,ha_ducking,smart_timer,player_control,smart_alarm,audiopro_preset_handler,spotify_event_handler,wifi-reset-ap,c3tone}.sh`,
`/www/{api.lua,index.html,manifest.json}`.

## B - in repo, differs (15): resolution

All twelve `/etc/config/*` diffs are comment-only or personalization:

| file | delta | resolution |
|---|---|---|
| librespot, shairport-sync, squeezelite, network | line order/comments only, values equal | nothing |
| uhttpd | repo `key_type ec`/`ec_curve` vs box `bits/ecdsa` (same EC cert, uci syntax drift) | exception |
| mcud | box lacks mqtt defaults (uci defaults cover); `sound_file` .mp3 vs .wav | **fixed repo default to .wav** (smart_alarm plays it with aplay) |
| audiopro_presets | box holds the user's three radio presets | personalization, restore from backup |
| system | timezone, LED trigger, remote syslog on box | personalization (timezone must not enter the repo), restore from backup |
| firewall | box adds Allow-HTTP/HTTPS + lan6/wwan6 | no-op: image ships no firewall (firewall4 deselected; box also whiteouts S19firewall) |
| dhcp | box adds odhcpd section + ra flags | runtime-added by odhcpd, restore from backup |

Other B files:
- `/etc/sysctl.d/30-zram-latency.conf`, `/etc/c3eq.conf`: repo = box + explanatory comments, values equal.
- `/etc/hotplug.d/iface/30-network-indicator`: **repo carried the fixed version
  (routing-table based, no writes to the wrong fifo) that was never deployed** -
  deployed to the box as part of this task.
- `/usr/bin/librespot`: repo carried a **stale 7.0 MB binary** (`3c1bbe54…`) while
  the speaker runs `5e967cb2…`. Moved out of `files/` to `artifacts/librespot`
  (byte-identical to the deployed one) because a 9.1 MB image + 7.0 MB binary
  does not fit the 16064k firmware partition. Flash procedure deploys it.

## C - not in repo (90): resolution

- **D runtime-generated (do not ship):** `/etc/uhttpd.{crt,key}`,
  `/etc/dropbear/*_host_key`, `/etc/urandom.seed`, `/etc/board.json`,
  `/etc/mcud.volume`, 43 files under `/usr/lib/opkg/`, 13 `/etc/rc.d` symlinks,
  `/root/.cache/librespot/credentials.json` (**secret, also restore from backup
  or the user re-links Spotify**),
  `/etc/init.d/{cron,avahi-daemon,dbus,uhttpd,odhcpd}` (stock package files that
  ended up on overlay during bootfix experiments; the image provides them).
- **E diagnostic/backup (do not ship):** `/etc/rc.common` (rcmark boot tracer),
  `/root/bootfix.orig/*` (13), `/etc/avahi/*.bak0821`, `/root/mcud.bak2.0821`,
  `/usr/bin/mcud.pre-cubic`, `/etc/shairport-sync.conf-opkg`,
  `/tmp/mcud.pre-*` backups, debug tools `/usr/bin/io`, `/usr/bin/picocom`,
  `/usr/libexec/stty-coreutils` (opkg-installed, referenced by nothing).
- **Added to repo by this task:** `/etc/sysctl.d/11-no-forwarding.conf`;
  `/usr/share/sounds/{alarm_sharp,alarm_wake_up,bell,timer_sharp}.wav`
  (aplay consumers: smart_alarm, smart_timer, ha_ducking chime; rom ships them,
  the repo-built image did not).
- **Provided by the image build (verify at image time):** `/usr/bin/mcud`
  (package), `/usr/bin/shairport-sync` (feed package, patched), `/usr/bin/squeezelite`
  (squeezelite-full), decoder libs `libvorbis/libvorbisfile/libFLAC/libogg/libfaad`
  (deps of squeezelite-full), `/usr/lib/libasound.so.2.0.0` (alsa-lib, patched),
  `/usr/lib/alsa-lib/libasound_module_pcm_c3eq.so` (alsa-plugin-c3eq),
  `/lib/modules/5.15.167/{snd-soc-ralink-i2s,ralink-gdma}.ko` (kernel patches).
- **Not provided by the image and not wanted:** `/usr/bin/io`, `picocom`,
  `coreutils-stty` (debug-only; `opkg install` them if ever needed again).

## F - whiteouts (20)

`boot.wav` (intentionally muted boot chime; the repo deliberately does not ship it),
`aec_tap_control.sh` (removed tool), `baresip` ×3 (never in our image),
`S19firewall` (no firewall package in image), `modules.d/pppoe`, `modules.d/ppp`
(ppp deselected in diffconfig), 10 × `uci-defaults/*` consumed one-shots (a fresh
image re-creates and re-consumes them on first boot - expected).

## The number

Flashing before this task would have lost or broken: the four aplay alarm/chime
wavs, `11-no-forwarding.conf` (forwarding silently re-enabled), working Spotify
(librespot downgraded to the stale repo binary, credentials cache wiped), the
user's presets/timezone/LED config, and the alarm sound default would have been
an aplay-unplayable .mp3. That is **5 broken pieces + personalization**; all are
now either in the repo, in `artifacts/`, or in the backup/restore list of the
flash procedure (docs/FLASHING.md).

## Backup/restore list for sysupgrade (device-local, never into the repo)

    /root/.cache/librespot/credentials.json   (Spotify login)
    /etc/config/{wireless,system,audiopro_presets,dhcp,firewall,network}
    /etc/mcud.volume /etc/dropbear/ /etc/uhttpd.crt /etc/uhttpd.key
    artifacts/librespot -> /usr/bin/librespot (see artifacts/README.md)
