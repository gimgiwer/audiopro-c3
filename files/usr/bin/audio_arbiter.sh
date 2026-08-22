#!/bin/sh
# Audio Arbiter - Manages concurrent audio streams
# Usage: audio_arbiter.sh <new_source>
# Sources: spotify, airplay, webradio, squeeze

NEW_SOURCE=$1
[ -z "$NEW_SOURCE" ] && exit 0

# Get settings from UCI
POLICY=$(uci -q get mcud.main.arbiter_policy || echo "lifo")
# Priorities: lower number = higher priority. Defaults:
PRIO_WEBRADIO=$(uci -q get mcud.main.prio_webradio || echo "5")
PRIO_SPOTIFY=$(uci -q get mcud.main.prio_spotify || echo "3")
PRIO_AIRPLAY=$(uci -q get mcud.main.prio_airplay || echo "2")
PRIO_SQUEEZE=$(uci -q get mcud.main.prio_squeeze || echo "6")
PRIO_SNAPCAST=$(uci -q get mcud.main.prio_snapcast || echo "1")
PRIO_DLNA=$(uci -q get mcud.main.prio_dlna || echo "4")

META_JSON="/tmp/audiopro_meta.json"

stop_spotify() {
    /etc/init.d/librespot restart >/dev/null 2>&1
}
stop_airplay() {
    /etc/init.d/shairport-sync restart >/dev/null 2>&1
}
stop_webradio() {
    # -9 on an alsa client wedges the dmix segment for everyone, see kill_audio
    # in smart_alarm.sh - same reason player_control.sh stop does it this way.
    killall -TERM mpg123 madplay >/dev/null 2>&1
    sleep 1
    killall -KILL mpg123 madplay >/dev/null 2>&1
}
stop_squeeze() {
    /etc/init.d/squeezelite restart >/dev/null 2>&1
}
stop_snapcast() {
    /etc/init.d/snapclient restart >/dev/null 2>&1
}
stop_dlna() {
    /etc/init.d/gmrender restart >/dev/null 2>&1
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
        snapcast) stop_snapcast ;;
        dlna) stop_dlna ;;
        alarm) stop_alarm ;;
    esac
}

get_priority() {
    case "$1" in
        alarm) echo "0" ;;
        snapcast) echo "$PRIO_SNAPCAST" ;;
        airplay) echo "$PRIO_AIRPLAY" ;;
        spotify) echo "$PRIO_SPOTIFY" ;;
        dlna) echo "$PRIO_DLNA" ;;
        webradio) echo "$PRIO_WEBRADIO" ;;
        squeeze) echo "$PRIO_SQUEEZE" ;;
        *) echo "99" ;;
    esac
}

# Stream slot arbitration strategy (Limit: 2 concurrent streams + TTS)
# Slot 0 (TTS/Alerts) - Managed separately via Ducking
# Slot 1 (Medium Prio): Spotify, AirPlay
# Slot 2 (Low Prio): WebRadio, Squeeze

check_immortal() {
    local source=$1
    if [ -f "$META_JSON" ]; then
        # Check if the active stream has immortal flag set to true
        if grep -q '"source": *"'"$source"'"' "$META_JSON" && grep -q '"immortal": *true' "$META_JSON"; then
            return 0 # True, immortal
        fi
    fi
    return 1 # False
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
    
    # If active source is Immortal, do NOT kill it if it's equal or higher priority
    # (Immortal does not protect against STRICTLY HIGHER priority like TTS, but protects against LIFO/peers)
    if check_immortal "$ACTIVE_SOURCE"; then
        if [ "$NEW_PRIO" -ge "$ACT_PRIO" ]; then
            # New is lower or equal prio -> kill new source, protect the immortal one
            kill_source "$NEW_SOURCE"
            exit 0
        fi
    fi
    
    if [ "$POLICY" = "priority" ]; then
        if [ "$NEW_PRIO" -le "$ACT_PRIO" ]; then
            kill_source "$ACTIVE_SOURCE"
        else
            kill_source "$NEW_SOURCE"
        fi
    elif [ "$POLICY" = "lifo" ]; then
        # Last In Wins (unless immortal handled above)
        kill_source "$ACTIVE_SOURCE"
    fi
fi

