#!/bin/sh
# Audio Pro C3 - Smart Alarm Script
# Softly ramps up volume and starts selected internet radio stream

TARGET_VOL=${1:-40}
STREAM_URL=${2:-"http://icecast.vrtcdn.be/klara-high.mp3"}
FADE_STEP=2
FADE_DELAY=10

set_volume() {
    local v=$1
    if [ -p /tmp/mcu_cmd_fifo ]; then
        printf "AXX+VOL+%03d\n" "$v" > /tmp/mcu_cmd_fifo 2>/dev/null || true
    fi
    amixer -q -c 0 sset Music "${v}%" 2>/dev/null || true
}

# 1. Terminate competing playback processes
killall -9 mpg123 >/dev/null 2>&1
killall -9 snapclient >/dev/null 2>&1
/etc/init.d/shairport-sync restart >/dev/null 2>&1

# 2. Reset volume to minimum (prevent sudden burst)
set_volume 5

# 3. Play alarm chime
aplay -q -D music_in /usr/share/sounds/bell.wav 2>/dev/null || aplay -q /usr/share/sounds/bell.wav 2>/dev/null || true

# 4. Start radio stream in background
mpg123 -a music_in "$STREAM_URL" >/dev/null 2>&1 &
ALARM_PID=$!

# 5. Smooth volume fade-in
CURRENT_VOL=5
while [ "$CURRENT_VOL" -lt "$TARGET_VOL" ]; do
    CURRENT_VOL=$((CURRENT_VOL + FADE_STEP))
    set_volume "$CURRENT_VOL"
    sleep "$FADE_DELAY"
done

# 6. Play for 1 hour, then stop
sleep 3600
kill "$ALARM_PID" >/dev/null 2>&1
