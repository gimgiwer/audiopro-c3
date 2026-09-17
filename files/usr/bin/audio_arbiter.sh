#!/bin/sh
# Audio Arbiter - Manages concurrent audio streams with strict LIFO ("Last In Wins")
# Usage: audio_arbiter.sh <new_source>
# Supported music sources: spotify, airplay, webradio, upnp, squeeze
# Note: System alerts (TTS, alarms, timers, chimes) are NEVER killed by the arbiter.

NEW_SOURCE="$1"
[ -z "$NEW_SOURCE" ] && exit 0

CURRENT_SRC_FILE="/tmp/current_music_source"
LOCK_FILE="/tmp/audio_arbiter.lock"
META_JSON="/tmp/audiopro_meta.json"

# Soft-pause implementation without dropping network connections or tearing down daemons
pause_source() {
    local src="$1"
    case "$src" in
        spotify)
            /usr/bin/player_control.sh pause spotify >/dev/null 2>&1
            ;;
        airplay)
            /usr/bin/player_control.sh pause airplay >/dev/null 2>&1
            ;;
        webradio)
            /usr/bin/player_control.sh pause webradio >/dev/null 2>&1
            ;;
        upnp)
            /usr/bin/player_control.sh pause upnp >/dev/null 2>&1
            ;;
        squeeze)
            /usr/bin/player_control.sh pause squeeze >/dev/null 2>&1
            ;;
    esac
}

unmute_source() {
    local src="$1"
    case "$src" in
        airplay)
            /usr/bin/player_control.sh resume airplay >/dev/null 2>&1
            ;;
        upnp)
            amixer -q -c 0 sset UPnP 100% 2>/dev/null || true
            ;;
    esac
}

# Acquire lock to prevent race conditions on rapid switching
(
    flock -x 200 || exit 0


    POLICY=$(uci -q get mcud.main.arbiter_policy || echo "lifo")
    if [ "$POLICY" = "mix" ]; then
        echo "$NEW_SOURCE" > "$CURRENT_SRC_FILE"
        exit 0
    fi

    # Read current active music source
    ACTIVE_SOURCE=""
    if [ -f "$CURRENT_SRC_FILE" ]; then
        read -r ACTIVE_SOURCE < "$CURRENT_SRC_FILE"
    fi

    # Fallback to meta.json if state file is missing
    if [ -z "$ACTIVE_SOURCE" ] && [ -f "$META_JSON" ]; then
        if grep -q '"playing"[[:space:]]*:[[:space:]]*true' "$META_JSON"; then
            ACTIVE_SOURCE=$(sed -n 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$META_JSON" | head -1)
        fi
    fi

    # If same source is already active, just ensure unmuted and exit
    if [ "$ACTIVE_SOURCE" = "$NEW_SOURCE" ]; then
        unmute_source "$NEW_SOURCE"
        exit 0
    fi

    # In LIFO mode, newest music stream pauses previous music stream
    if [ -n "$ACTIVE_SOURCE" ]; then
        pause_source "$ACTIVE_SOURCE"
    fi

    # Register new active source and unmute if needed
    echo "$NEW_SOURCE" > "$CURRENT_SRC_FILE"
    unmute_source "$NEW_SOURCE"

) 200>"$LOCK_FILE"

exit 0

