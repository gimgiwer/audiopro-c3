#!/bin/sh
# Audio Arbiter - Manages concurrent audio streams
# Usage: audio_arbiter.sh <new_source>
# Sources: spotify, airplay, webradio, squeeze

. /usr/lib/audiopro/kill_audio.sh

NEW_SOURCE=$1
[ -z "$NEW_SOURCE" ] && exit 0

# Get settings from UCI
POLICY=$(uci -q get mcud.main.arbiter_policy || echo "lifo")
# Priorities: lower number = higher priority. Defaults:
PRIO_WEBRADIO=$(uci -q get mcud.main.prio_webradio || echo "5")
PRIO_SPOTIFY=$(uci -q get mcud.main.prio_spotify || echo "3")
PRIO_AIRPLAY=$(uci -q get mcud.main.prio_airplay || echo "2")
PRIO_SQUEEZE=$(uci -q get mcud.main.prio_squeeze || echo "6")

META_JSON="/tmp/audiopro_meta.json"

stop_spotify() {
    /etc/init.d/librespot restart >/dev/null 2>&1
}
stop_airplay() {
    /etc/init.d/shairport-sync restart >/dev/null 2>&1
}
stop_webradio() {
    kill_stream
}
stop_squeeze() {
    /etc/init.d/squeezelite restart >/dev/null 2>&1
}
stop_alarm() {
    /usr/bin/smart_alarm.sh stop >/dev/null 2>&1
}

kill_source() {
    case "$1" in
        spotify) stop_spotify ;;
        airplay) stop_airplay ;;
        webradio) stop_webradio ;;
        squeeze) stop_squeeze ;;
        alarm) stop_alarm ;;
    esac
}

get_priority() {
    case "$1" in
        alarm) echo "0" ;;
        airplay) echo "$PRIO_AIRPLAY" ;;
        spotify) echo "$PRIO_SPOTIFY" ;;
        webradio) echo "$PRIO_WEBRADIO" ;;
        squeeze) echo "$PRIO_SQUEEZE" ;;
        *) echo "99" ;;
    esac
}

if [ "$POLICY" = "mix" ]; then
    exit 0
fi

if [ "$POLICY" = "priority" ] || [ "$POLICY" = "lifo" ]; then
    ACTIVE_SOURCE=""
    if [ -f "$META_JSON" ] && grep -q '"playing": true' "$META_JSON"; then
        ACTIVE_SOURCE=$(grep '"source":' "$META_JSON" | sed -n 's/.*"source": *"\([^"]*\)".*/\1/p')
    fi
    
    [ -z "$ACTIVE_SOURCE" ] && exit 0
    [ "$ACTIVE_SOURCE" = "$NEW_SOURCE" ] && exit 0
    
    NEW_PRIO=$(get_priority "$NEW_SOURCE")
    ACT_PRIO=$(get_priority "$ACTIVE_SOURCE")
    
    if [ "$POLICY" = "priority" ]; then
        if [ "$NEW_PRIO" -le "$ACT_PRIO" ]; then
            kill_source "$ACTIVE_SOURCE"
        else
            kill_source "$NEW_SOURCE"
        fi
    elif [ "$POLICY" = "lifo" ]; then
        # Last In Wins
        kill_source "$ACTIVE_SOURCE"
    fi
fi

