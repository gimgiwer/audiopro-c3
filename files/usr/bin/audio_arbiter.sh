#!/bin/sh
# Audio Arbiter - Manages concurrent audio streams
# Usage: audio_arbiter.sh <new_source>
# Sources: spotify, airplay, webradio, squeeze

NEW_SOURCE=$1
[ -z "$NEW_SOURCE" ] && exit 0

# Get settings from UCI
POLICY=$(uci -q get mcud.main.arbiter_policy || echo "lifo")
# Priorities: lower number = higher priority. Defaults:
PRIO_WEBRADIO=$(uci -q get mcud.main.prio_webradio || echo "1")
PRIO_SPOTIFY=$(uci -q get mcud.main.prio_spotify || echo "2")
PRIO_AIRPLAY=$(uci -q get mcud.main.prio_airplay || echo "3")
PRIO_SQUEEZE=$(uci -q get mcud.main.prio_squeeze || echo "4")

META_JSON="/tmp/audiopro_meta.json"

stop_spotify() {
    /etc/init.d/librespot restart >/dev/null 2>&1
}
stop_airplay() {
    /etc/init.d/shairport-sync restart >/dev/null 2>&1
}
stop_webradio() {
    killall -9 mpg123 madplay >/dev/null 2>&1
}
stop_squeeze() {
    /etc/init.d/squeezelite restart >/dev/null 2>&1
}

kill_source() {
    case "$1" in
        spotify) stop_spotify ;;
        airplay) stop_airplay ;;
        webradio) stop_webradio ;;
        squeeze) stop_squeeze ;;
    esac
}

get_priority() {
    case "$1" in
        spotify) echo "$PRIO_SPOTIFY" ;;
        airplay) echo "$PRIO_AIRPLAY" ;;
        webradio) echo "$PRIO_WEBRADIO" ;;
        squeeze) echo "$PRIO_SQUEEZE" ;;
        *) echo "99" ;;
    esac
}

# If policy is mix/unlimited, do nothing
if [ "$POLICY" = "mix" ]; then
    exit 0
fi

if [ "$POLICY" = "lifo" ]; then
    # Last In Wins: kill everything else
    [ "$NEW_SOURCE" != "spotify" ] && stop_spotify
    [ "$NEW_SOURCE" != "airplay" ] && stop_airplay
    [ "$NEW_SOURCE" != "webradio" ] && stop_webradio
    [ "$NEW_SOURCE" != "squeeze" ] && stop_squeeze
    exit 0
fi

if [ "$POLICY" = "priority" ]; then
    # Get currently active source from JSON
    ACTIVE_SOURCE=""
    if [ -f "$META_JSON" ] && grep -q '"playing": true' "$META_JSON"; then
        ACTIVE_SOURCE=$(grep '"source":' "$META_JSON" | sed -n 's/.*"source": *"\([^"]*\)".*/\1/p')
    fi
    
    # If no active source or it's the same, let it play
    if [ -z "$ACTIVE_SOURCE" ] || [ "$ACTIVE_SOURCE" = "$NEW_SOURCE" ]; then
        exit 0
    fi
    
    NEW_PRIO=$(get_priority "$NEW_SOURCE")
    ACT_PRIO=$(get_priority "$ACTIVE_SOURCE")
    
    if [ "$NEW_PRIO" -le "$ACT_PRIO" ]; then
        # New source is higher or equal priority -> Kill active source
        kill_source "$ACTIVE_SOURCE"
    else
        # New source is lower priority -> Kill new source to prevent it from interrupting
        kill_source "$NEW_SOURCE"
    fi
    exit 0
fi
