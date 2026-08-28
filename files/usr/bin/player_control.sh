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

# SIGSTOP is crude but mpg123 has no control channel. It also catches a TTS
# prompt if one happens to be playing; mcud's alarm() reaps that either way.
pause_webradio()  { killall -STOP mpg123 2>/dev/null || true; }
resume_webradio() { killall -CONT mpg123 2>/dev/null || true; }

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
				webradio) resume_webradio ;;
			esac
		done
		rm -f "$SAVED_STATE"
	else
		[ "$skip" = nospotify ] || resume_spotify
		resume_airplay
		resume_squeeze
		resume_webradio
	fi
}

one_target() {
	local act="$1"
	case "$2" in
		spotify)  if [ "$act" = pause ]; then pause_spotify;  else resume_spotify;  fi ;;
		airplay)  if [ "$act" = pause ]; then pause_airplay;  else resume_airplay;  fi ;;
		squeeze)  if [ "$act" = pause ]; then pause_squeeze;  else resume_squeeze;  fi ;;
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

		# The state file only records what we last did to a player, so anything the
		# phone does behind our back leaves the key inverted for one press. librespot
		# knows its own play_status, and the patched fifo takes a stateless toggle
		# (spirc.play_pause()), so let Spotify answer for itself and keep it out of
		# the file the muted-only players share. On an idle librespot the command is
		# a logged no-op ("ignored while Not Active"), so this cannot start Spotify
		# while something else is playing.
		# With no explicit target, follow the source that most recently reported
		# playback. This keeps the physical key stateless and phone/MA friendly.
		if [ "$TARGET" = all ] && [ -r "$META_JSON" ]; then
			case "$(sed -n 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$META_JSON" | head -1)" in
				spotify) TARGET=spotify ;;
				squeeze|music_assistant|ma) TARGET=squeeze ;;
				airplay) TARGET=airplay ;;
			esac
		fi
		case "$TARGET" in
			spotify)
				spotify_cmd toggle
				;;
			all)
				spotify_cmd toggle
				if [ -f "$SAVED_STATE" ]; then do_resume_all nospotify; else do_pause_all nospotify; fi
				;;
			*)
				if [ -f "$SAVED_STATE" ]; then one_target resume "$TARGET"; else one_target pause "$TARGET"; fi
				;;
		esac
		;;
	next|prev)
		# MA first, then Spotify - nothing else here has a notion of tracks
		ma_transport "$ACTION" && exit 0
		spotify_cmd "$ACTION"
		;;
	stop)
		kill_stream
		do_pause_all
		;;
	*)
		echo "Usage: $0 {pause|play|resume|toggle|next|prev|stop} [spotify|airplay|squeeze|webradio|all]"
		exit 1
		;;
esac
