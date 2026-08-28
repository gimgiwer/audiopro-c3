# Shared way to stop an alsa client. Sourced, never executed.
#
# A dmix client taken out with -9 leaves its slot live in the mixer's shared
# segment, and from then on every open on default blocks forever - measured on
# hardware: aplay -d 2 still hanging after 5 s with the hardware substream
# already free, and only a reboot cleared it. A client restart does not. So we
# never send KILL to anything that might be attached to dmix. Worst case of
# being polite is one stuck player; worst case of KILL is the whole card gone
# until someone power-cycles the speaker.
#
# busybox here has neither usleep nor fractional sleep, so the grace is counted
# in whole seconds. mpg123 and aplay both leave on the first TERM well inside
# one of them; the loop only ever runs long when something is truly stuck, and
# then it gives up rather than escalating.

ALSA_KILL_GRACE=${ALSA_KILL_GRACE:-6}

# kill_audio <pid>...
kill_audio() {
	local p left="" still="" i=0
	for p in "$@"; do
		[ "$p" = "$$" ] && continue
		# CONT first: a SIGSTOPped player (that is how we pause) would never
		# see the TERM otherwise and we would sit out the whole grace period.
		kill -CONT "$p" 2>/dev/null
		kill -TERM "$p" 2>/dev/null
		left="$left $p"
	done
	[ -n "$left" ] || return 0
	while [ "$i" -lt "$ALSA_KILL_GRACE" ]; do
		still=""
		for p in $left; do
			kill -0 "$p" 2>/dev/null && still="$still $p"
		done
		[ -n "$still" ] || return 0
		left="$still"
		sleep 1
		i=$((i + 1))
	done
	logger -t audiopro "TERM ignored by$left after ${ALSA_KILL_GRACE}s, leaving it - KILL would wedge dmix"
	return 1
}

# kill_audio_names <name>...
kill_audio_names() {
	local n pids=""
	for n in "$@"; do
		pids="$pids $(pgrep -x "$n" 2>/dev/null)"
	done
	kill_audio $pids
}

# kill_stream
# A web stream is "wget -O - | mpg123 -" (play_stream.sh), and the whole subtree
# inherits mcud's ignored SIGPIPE, so taking out the decoder leaves the feeder
# parked in epoll_wait and still pulling the stream - measured: three wrapper/wget
# pairs left over after three stops. The wrapper is /bin/sh so pgrep -x cannot see
# it either; both halves have to be matched on the full command line.
kill_stream() {
	# both patterns are anchored: unanchored -f matched any shell whose own command
	# line merely mentioned them, and the first run of this took out its own caller
	kill_audio $(pgrep -f '^/bin/sh /usr/bin/play_stream\.sh' 2>/dev/null) \
		$(pgrep -f '^wget -q -O - --' 2>/dev/null) \
		$(pgrep -x mpg123 2>/dev/null) $(pgrep -x madplay 2>/dev/null)
}
