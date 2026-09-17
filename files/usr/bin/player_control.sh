#!/bin/sh
# Audio Pro C3 - transport control across the players we actually ship.
# Called from the web api, the physical play/pause key and MQTT.

ACTION="${1:-toggle}"
TARGET="${2:-all}"
SAVED_STATE="/tmp/paused_players.state"
LR_FIFO="/tmp/librespot_cmd"
LEVEL_DIR="/tmp"
META_JSON="/tmp/audiopro_meta.json"

. /usr/lib/audiopro/kill_audio.sh

# Real Spotify Connect commands, so the phone's ui and the track position stay
# in sync -- muting the softvol instead would keep the stream running and lose
# your place. librespot holds the fifo open read+write, so the write cannot
# block while it is alive; backgrounded anyway, because if it died between the
# check and the write a blocked writer must not take uhttpd or mcud down with it.
# No -x on any pgrep here: busybox matches it against argv[0], which for
# anything procd starts is the full /usr/bin path, so -x never hits.
spotify_cmd() {
	[ -p "$LR_FIFO" ] || return 1
	pgrep librespot >/dev/null 2>&1 || return 1
	( echo "$1" > "$LR_FIFO" ) &
}

pause_spotify()  { spotify_cmd pause; }
resume_spotify() { spotify_cmd play; }

# AirPlay has no reachable transport control channel on this build, so it is
# paused by muting its input.
mute_ctl() {
	local ctl="$1" f="$LEVEL_DIR/premute_$1.level"
	amixer -c 0 sget "$ctl" >/dev/null 2>&1 || return 1
	amixer -c 0 sget "$ctl" | sed -n 's/.*\[\([0-9]*\)%\].*/\1/p' | head -1 > "$f"
	amixer -q -c 0 sset "$ctl" 0%
}

unmute_ctl() {
	local ctl="$1" f="$LEVEL_DIR/premute_$1.level" lvl=100
	amixer -c 0 sget "$ctl" >/dev/null 2>&1 || return 1
	[ -r "$f" ] && read -r lvl < "$f"
	case "$lvl" in ''|*[!0-9]*|0) lvl=100 ;; esac
	amixer -q -c 0 sset "$ctl" "${lvl}%"
	rm -f "$f"
}

pause_airplay()  { mute_ctl AirPlay; }
resume_airplay() { unmute_ctl AirPlay; }

# MA streams from the server, so muting a fader is not a transport at all - the
# only real one is the LMS CLI, and mcud is what holds that socket. It answers
# not-found when MA is not the one playing, which is the cue to fall through to
# the local players. Deliberately not used for pause/resume: those are the alarm
# and ducking paths, and they must silence the speaker without taking MA's place
# away from it.
ma_transport() {
	ubus call mcud transport "{\"cmd\":\"$1\"}" >/dev/null 2>&1
}

# Music Assistant owns squeezelite's transport and never reads the mixer back, so
# muting a fader here would leave MA streaming a track it still shows as playing,
# with the position running on. The play/pause key must not fake that: mcud
# publishes the press as an mqtt device trigger (audiopro_c3/button/play_pause)
# and HA turns it into media_player.media_play_pause on the MA player, which is
# the only thing that can really stop it. An explicit pause/stop from the api or
# an alarm is a plain local mute and stays one - it silences the speaker and MA
# keeps its place.
pause_squeeze()  { [ "$TOGGLING" = 1 ] && return 1; mute_ctl Squeeze; }
resume_squeeze() { [ "$TOGGLING" = 1 ] && return 1; unmute_ctl Squeeze; }

# UPnP / DLNA transport via mpc
pause_upnp()  { mpc pause >/dev/null 2>&1 || true; }
resume_upnp() { mpc play >/dev/null 2>&1 || true; }

# Web radio stream controls (kill on pause, respawn last preset on resume)
pause_webradio() {
	kill_stream
	sed -i 's/"playing"[[:space:]]*:[[:space:]]*true/"playing": false/' "$META_JSON" 2>/dev/null || true
}
resume_webradio() {
	local preset="1"
	[ -f /tmp/last_webradio_preset ] && read -r preset < /tmp/last_webradio_preset
	case "$preset" in 1|2|3|4) ;; *) preset="1" ;; esac
	/usr/bin/audiopro_preset_handler.sh "$preset" >/dev/null 2>&1 &
}

# Record a player only when its pause actually took. The softvol controls are
# created lazily by alsa on first use of the plugin, so AirPlay simply does not
# exist as a control until something has played through it -- pausing it then
# fails, and resuming it later would be a no-op on a control nobody touched.
do_pause_all() {
	local paused="" skip="$1"
	if [ "$skip" != nospotify ] && pgrep librespot >/dev/null 2>&1 && pause_spotify; then
		paused="$paused spotify"
	fi
	if pgrep shairport-sync >/dev/null 2>&1 && pause_airplay; then
		paused="$paused airplay"
	fi
	if pgrep squeezelite >/dev/null 2>&1 && pause_squeeze; then
		paused="$paused squeeze"
	fi
	if pgrep mpd >/dev/null 2>&1 && pause_upnp; then
		paused="$paused upnp"
	fi
	if pgrep mpg123 >/dev/null 2>&1 && pause_webradio; then
		paused="$paused webradio"
	fi
	# No state file when nothing was playing, so the next toggle tries pause again
	if [ -n "$paused" ]; then
		echo "$paused" > "$SAVED_STATE"
	fi
}

do_resume_all() {
	local skip="$1"
	if [ -f "$SAVED_STATE" ]; then
		local p
		for p in $(cat "$SAVED_STATE" 2>/dev/null); do
			case "$p" in
				spotify)  [ "$skip" = nospotify ] || resume_spotify ;;
				airplay)  resume_airplay ;;
				squeeze)  resume_squeeze ;;
				upnp)     resume_upnp ;;
				webradio) resume_webradio ;;
			esac
		done
		rm -f "$SAVED_STATE"
	else
		[ "$skip" = nospotify ] || resume_spotify
		resume_airplay
		resume_squeeze
		resume_upnp
		resume_webradio
	fi
}

one_target() {
	local act="$1"
	case "$2" in
		spotify)  if [ "$act" = pause ]; then pause_spotify;  else resume_spotify;  fi ;;
		airplay)  if [ "$act" = pause ]; then pause_airplay;  else resume_airplay;  fi ;;
		squeeze)  if [ "$act" = pause ]; then pause_squeeze;  else resume_squeeze;  fi ;;
		upnp)     if [ "$act" = pause ]; then pause_upnp;     else resume_upnp;     fi ;;
		webradio) if [ "$act" = pause ]; then pause_webradio; else resume_webradio; fi ;;
	esac
}

case "$ACTION" in
	pause)
		if [ "$TARGET" = all ]; then do_pause_all; else one_target pause "$TARGET"; fi
		;;
	resume|play)
		if [ "$TARGET" = all ]; then do_resume_all; else one_target resume "$TARGET"; fi
		;;
	toggle)
		TOGGLING=1
		# If Smart Alarm or Timer is ringing, physical Play/Pause dismisses it
		if [ -f /tmp/alarm.pid ]; then
			/usr/bin/smart_alarm.sh stop
			exit 0
		fi
		if [ -f /tmp/timer_ring.pid ] || [ -f /tmp/timer.pid ]; then
			/usr/bin/smart_timer.sh cancel
			exit 0
		fi

		case "$TARGET" in
			all|squeeze) ma_transport toggle && exit 0 ;;
		esac

		# With no explicit target, follow the source that is currently active or playing.
		if [ "$TARGET" = all ]; then
			if [ -f /tmp/current_music_source ]; then
				read -r src < /tmp/current_music_source
				case "$src" in
					spotify|airplay|squeeze|webradio|upnp) TARGET="$src" ;;
				esac
			elif [ -r "$META_JSON" ]; then
				case "$(sed -n 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$META_JSON" | head -1)" in
					spotify) TARGET=spotify ;;
					squeeze|music_assistant|ma) TARGET=squeeze ;;
					airplay) TARGET=airplay ;;
					webradio) TARGET=webradio ;;
					upnp) TARGET=upnp ;;
				esac
			fi
		fi

		case "$TARGET" in
			spotify)
				spotify_cmd toggle
				;;
			upnp)
				mpc toggle >/dev/null 2>&1
				;;
			webradio)
				if pgrep mpg123 >/dev/null 2>&1 || pgrep wget >/dev/null 2>&1; then
					pause_webradio
				else
					resume_webradio
				fi
				;;
			airplay)
				if [ -f "$LEVEL_DIR/premute_AirPlay.level" ]; then
					resume_airplay
				else
					pause_airplay
				fi
				;;
			all)
				spotify_cmd toggle
				mpc toggle >/dev/null 2>&1
				if [ -f "$SAVED_STATE" ]; then do_resume_all nospotify; else do_pause_all nospotify; fi
				;;
			*)
				if [ -f "$SAVED_STATE" ]; then one_target resume "$TARGET"; else one_target pause "$TARGET"; fi
				;;
		esac
		;;
	next|prev)
		# Follow current active music source
		current="spotify"
		if [ -f /tmp/current_music_source ]; then
			read -r current < /tmp/current_music_source
		elif [ -r "$META_JSON" ]; then
			current=$(sed -n 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$META_JSON" | head -1)
		fi

		case "$current" in
			upnp)
				mpc "$ACTION" >/dev/null 2>&1 && exit 0
				;;
			webradio)
				cur_p=1
				[ -f /tmp/last_webradio_preset ] && read -r cur_p < /tmp/last_webradio_preset
				case "$cur_p" in 1|2|3|4) ;; *) cur_p=1 ;; esac
				if [ "$ACTION" = "next" ]; then
					next_p=$(( (cur_p % 4) + 1 ))
				else
					next_p=$(( cur_p - 1 ))
					[ "$next_p" -lt 1 ] && next_p=4
				fi
				/usr/bin/audiopro_preset_handler.sh "$next_p" >/dev/null 2>&1 &
				exit 0
				;;
			*)
				# MA first, then Spotify
				ma_transport "$ACTION" && exit 0
				spotify_cmd "$ACTION"
				;;
		esac
		;;
	stop)
		kill_stream
		do_pause_all
		;;
	*)
		echo "Usage: $0 {pause|play|resume|toggle|next|prev|stop} [spotify|airplay|squeeze|webradio|upnp|all]"
		exit 1
		;;
esac
