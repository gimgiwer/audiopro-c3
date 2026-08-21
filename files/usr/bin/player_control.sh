#!/bin/sh
# Audio Pro C3 - transport control across the players we actually ship.
# Called from the web api, the physical play/pause key and MQTT.

ACTION="${1:-toggle}"
TARGET="${2:-all}"
SAVED_STATE="/tmp/paused_players.state"
LR_FIFO="/tmp/librespot_cmd"
AP_LEVEL="/tmp/airplay_premute.level"

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

# No real pause for AirPlay yet: this shairport-sync is built without dbus or
# mpris, so there is no way into its DACP remote. Muting the input is the honest
# approximation -- the phone keeps playing, so resume lands further along the
# track. Restore the level we found rather than a hardcoded 100%, or one pause
# would wipe whatever level the user had set.
pause_airplay() {
	amixer -c 0 sget AirPlay >/dev/null 2>&1 || return 1
	amixer -c 0 sget AirPlay | sed -n 's/.*\[\([0-9]*\)%\].*/\1/p' | head -1 > "$AP_LEVEL"
	amixer -q -c 0 sset AirPlay 0%
}

resume_airplay() {
	amixer -c 0 sget AirPlay >/dev/null 2>&1 || return 1
	local lvl=100
	[ -r "$AP_LEVEL" ] && read -r lvl < "$AP_LEVEL"
	case "$lvl" in ''|*[!0-9]*) lvl=100 ;; esac
	amixer -q -c 0 sset AirPlay "${lvl}%"
	rm -f "$AP_LEVEL"
}

# SIGSTOP is crude but mpg123 has no control channel. It also catches a TTS
# prompt if one happens to be playing; mcud's alarm() reaps that either way.
pause_webradio()  { killall -STOP mpg123 2>/dev/null || true; }
resume_webradio() { killall -CONT mpg123 2>/dev/null || true; }

# Record a player only when its pause actually took. The softvol controls are
# created lazily by alsa on first use of the plugin, so AirPlay simply does not
# exist as a control until something has played through it -- pausing it then
# fails, and resuming it later would be a no-op on a control nobody touched.
do_pause_all() {
	local paused=""
	if pgrep librespot >/dev/null 2>&1 && pause_spotify; then
		paused="$paused spotify"
	fi
	if pgrep shairport-sync >/dev/null 2>&1 && pause_airplay; then
		paused="$paused airplay"
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
	if [ -f "$SAVED_STATE" ]; then
		local p
		for p in $(cat "$SAVED_STATE" 2>/dev/null); do
			case "$p" in
				spotify)  resume_spotify ;;
				airplay)  resume_airplay ;;
				webradio) resume_webradio ;;
			esac
		done
		rm -f "$SAVED_STATE"
	else
		resume_spotify
		resume_airplay
		resume_webradio
	fi
}

one_target() {
	local act="$1"
	case "$2" in
		spotify)  if [ "$act" = pause ]; then pause_spotify;  else resume_spotify;  fi ;;
		airplay)  if [ "$act" = pause ]; then pause_airplay;  else resume_airplay;  fi ;;
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
		if [ -f "$SAVED_STATE" ]; then do_resume_all; else do_pause_all; fi
		;;
	next|prev)
		# Only Spotify has a notion of tracks we can drive from here
		spotify_cmd "$ACTION"
		;;
	stop)
		killall -9 mpg123 2>/dev/null || true
		do_pause_all
		;;
	*)
		echo "Usage: $0 {pause|play|resume|toggle|next|prev|stop} [spotify|airplay|webradio|all]"
		exit 1
		;;
esac
