#!/bin/sh
# Advanced Home Assistant Audio Ducking & TTS Announcer
ACTION="${1:-play}"
TARGET="$2"
MODE="${3:-normal}" # normal | critical
CHIME="/usr/share/sounds/bell.wav"

# Hold the ducking lock for the whole run, whatever the action: mcud checks it
# before restoring a control somebody abandoned, and flock frees itself the
# moment this process dies - even on SIGKILL.
exec 9>/tmp/ha_ducking.lock
flock -x 9 2>/dev/null || true

duck_down() {
    # Two steps instead of one hop, so the ear gets a hint of a ramp
    amixer -q -c 0 sset Music 50% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 50% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 50% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 50% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 50% 2>/dev/null || true
    amixer -q -c 0 sset Music 20% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 20% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 20% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 20% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 20% 2>/dev/null || true
}

duck_up() {
    amixer -q -c 0 sset Music 60% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 60% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 60% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 60% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 60% 2>/dev/null || true
    amixer -q -c 0 sset Music 100% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 100% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 100% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 100% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 100% 2>/dev/null || true
}

case "$ACTION" in
    start|down)
        duck_down
        ;;
    stop|up)
        duck_up
        ;;
    play|tts|critical)
        duck_down
        amixer -q -c 0 sset TTS 100% 2>/dev/null || true
        [ -f "$CHIME" ] && aplay -q -D tts_in -- "$CHIME" 2>/dev/null || true

        if [ -n "$TARGET" ]; then
            if [ -f "$TARGET" ]; then
                aplay -q -D tts_in -- "$TARGET" 2>/dev/null || true
            elif echo "$TARGET" | grep -qE "^https?://"; then
                if command -v mpg123 >/dev/null 2>&1; then
                    mpg123 -q -a tts_in -- "$TARGET" 2>/dev/null || true
                elif command -v wget >/dev/null 2>&1; then
                    wget -q -O - -- "$TARGET" | aplay -D tts_in -q - 2>/dev/null || true
                fi
            fi
        fi

        sleep 1
        duck_up
        flock -u 9 2>/dev/null || true
        exec 9>&-
        ;;
    *)
        echo "Usage: $0 {start|stop|play <file_or_url> [normal|critical]|critical <file_or_url>}"
        exit 1
        ;;
esac

exit 0
