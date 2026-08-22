#!/bin/sh
# Audio Pro C3 - transport control across the players we actually ship.
# Called from the web api, the physical play/pause key and MQTT.

ACTION="${1:-toggle}"
TARGET="${2:-all}"
SAVED_STATE="/tmp/paused_players.state"
LR_FIFO="/tmp/librespot_cmd"
LEVEL_DIR="/tmp"

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

# AirPlay and squeezelite have no reachable control channel on this build:
# shairport-sync is compiled without dbus/mpris, and squeezelite only takes
# orders from its LMS server. Muting the input is the honest approximation --
# the sender keeps playing, so resume lands further along the track. Save the
# level we found instead of assuming 100%, or one pause would wipe whatever the
# user had set. SIGSTOP would be tighter but a stopped dmix client leaves its
# ring position frozen while the mixer keeps reading it, which loops a fragment.
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
pause_squeeze()  { mute_ctl Squeeze; }
resume_squeeze() { unmute_ctl Squeeze; }

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
	if [ -f "$SAVED_STATE" ]; then
		local p
		for p in $(cat "$SAVED_STATE" 2>/dev/null); do
			case "$p" in
				spotify)  resume_spotify ;;
				airplay)  resume_airplay ;;
				squeeze)  resume_squeeze ;;
				webradio) resume_webradio ;;
			esac
		done
		rm -f "$SAVED_STATE"
	else
		resume_spotify
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
		if [ -f "$SAVED_STATE" ]; then do_resume_all; else do_pause_all; fi
		;;
	next|prev)
		# Only Spotify has a notion of tracks we can drive from here
		spotify_cmd "$ACTION"
		;;
	stop)
		# TERM first: a -9'd alsa client leaves the dmix slave holding its slot,
		# and this busybox has neither usleep nor fractional sleep to shorten
		# the grace period.
		if pgrep mpg123 >/dev/null 2>&1; then
			killall -CONT mpg123 2>/dev/null
			killall -TERM mpg123 2>/dev/null
			sleep 1
			killall -KILL mpg123 2>/dev/null
		fi
		do_pause_all
		;;
	*)
		echo "Usage: $0 {pause|play|resume|toggle|next|prev|stop} [spotify|airplay|squeeze|webradio|all]"
		exit 1
		;;
esac
