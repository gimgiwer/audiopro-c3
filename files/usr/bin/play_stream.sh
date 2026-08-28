#!/bin/sh
# mpg123 has no network code of its own here - it shells out to a gnu wget or curl, and
# busybox's wget does not even take --version, so every http url died with "missing
# working network helper program" and the radio presets played nothing at all.
# Feed it the bytes over a pipe instead. Slot is an asound.conf softvol input.
URL="$1"
SLOT="${2:-music_in}"
[ -n "$URL" ] || exit 1

case "$URL" in
    http://*|https://*) ;;
    *) exit 1 ;;
esac

wget -q -O - -- "$URL" | mpg123 -q -a "$SLOT" -
