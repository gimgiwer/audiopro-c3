#!/bin/sh
# Advanced Home Assistant Audio Ducking & TTS Announcer
ACTION="${1:-play}"
TARGET="$2"
MODE="${3:-normal}" # normal | critical
CHIME="/usr/share/sounds/bell.wav"

duck_down() {
    local is_critical="$1"
    local voip_target="45%"
    [ "$is_critical" = "critical" ] && voip_target="15%"

    # Smoothly ramp down background music to 20% (-14 dB) and VoIP to audible background level
    amixer -q -c 0 sset Music 50% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 50% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 50% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 50% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 50% 2>/dev/null || true
    amixer -q -c 0 sset VoIP 70% 2>/dev/null || true
    usleep 40000
    amixer -q -c 0 sset Music 20% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 20% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 20% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 20% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 20% 2>/dev/null || true
    amixer -q -c 0 sset VoIP "$voip_target" 2>/dev/null || true
}

duck_up() {
    # Smoothly restore VoIP and background music volume back to 100%
    amixer -q -c 0 sset VoIP 80% 2>/dev/null || true
    amixer -q -c 0 sset Music 60% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 60% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 60% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 60% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 60% 2>/dev/null || true
    usleep 40000
    amixer -q -c 0 sset VoIP 100% 2>/dev/null || true
    amixer -q -c 0 sset Music 100% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 100% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 100% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 100% 2>/dev/null || true
    amixer -q -c 0 sset Alarm 100% 2>/dev/null || true
}

case "$ACTION" in
    start|down)
        duck_down "$TARGET"
        ;;
    stop|up)
        duck_up
        ;;
    play|tts|critical)
        is_crit="normal"
        [ "$ACTION" = "critical" ] || [ "$MODE" = "critical" ] && is_crit="critical"

        # Serialize concurrent TTS playbacks via flock
        LOCKFILE="/tmp/ha_ducking.lock"
        exec 9>"$LOCKFILE"
        flock -x 9 2>/dev/null || true

        duck_down "$is_crit"
        sleep 0.1
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
        
        sleep 0.2
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
