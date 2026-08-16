#!/bin/sh
# Fast non-blocking launch for librespot.
# OpenWrt's sysfixtime already restores a valid timestamp at S00sysfixtime.
# If clock is already valid (>= 1700000000), launch librespot immediately!
TARGET=1700000000
NOW=$(date +%s)

if [ "$NOW" -lt "$TARGET" ]; then
    # Clock not yet synced, wait up to 10s max for NTP
    TRIES=10
    while [ "$(date +%s)" -lt "$TARGET" ] && [ "$TRIES" -gt 0 ]; do
        sleep 1
        TRIES=$((TRIES - 1))
    done
fi

exec /usr/bin/librespot "$@"
