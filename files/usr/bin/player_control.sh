#!/bin/sh
# Audio Pro C3 - Unified Media Player Transport Controller
# Handles stateful Play/Pause/Resume across Spotify, AirPlay, Web Radio & Squeezelite

ACTION="${1:-toggle}"
TARGET="${2:-all}"
SAVED_STATE="/tmp/paused_players.state"

pause_spotify() {
    # 1. Publish MQTT command for Home Assistant / Spotify Web API
    if [ -x /usr/bin/mosquitto_pub ]; then
        local prefix=$(uci -q get mcud.main.mqtt_topic_prefix || echo "audiopro_c3")
        local host=$(uci -q get mcud.main.mqtt_host || echo "127.0.0.1")
        local port=$(uci -q get mcud.main.mqtt_port || echo "1883")
        mosquitto_pub -h "$host" -p "$port" -t "${prefix}/player/state" -m "paused" 2>/dev/null || true
        mosquitto_pub -h "$host" -p "$port" -t "${prefix}/player/event" -m "{\"action\":\"pause\",\"source\":\"spotify\"}" 2>/dev/null || true
    fi
    # 2. Mute Spotify ALSA channel softvol
    amixer -q -c 0 sset Spotify 0% 2>/dev/null || true
}

resume_spotify() {
    # 1. Publish MQTT command for Home Assistant / Spotify Web API
    if [ -x /usr/bin/mosquitto_pub ]; then
        local prefix=$(uci -q get mcud.main.mqtt_topic_prefix || echo "audiopro_c3")
        local host=$(uci -q get mcud.main.mqtt_host || echo "127.0.0.1")
        local port=$(uci -q get mcud.main.mqtt_port || echo "1883")
        mosquitto_pub -h "$host" -p "$port" -t "${prefix}/player/state" -m "playing" 2>/dev/null || true
        mosquitto_pub -h "$host" -p "$port" -t "${prefix}/player/event" -m "{\"action\":\"play\",\"source\":\"spotify\"}" 2>/dev/null || true
    fi
    # 2. Unmute Spotify ALSA channel softvol
    amixer -q -c 0 sset Spotify 100% 2>/dev/null || true
}

pause_airplay() {
    # Native AirPlay DACP remote control via Shairport-Sync D-Bus (iPhone displays Pause icon)
    dbus-send --system --type=method_call --dest=org.gnome.ShairportSync /org/gnome/ShairportSync org.gnome.ShairportSync.RemoteControl.Pause >/dev/null 2>&1 || true
    amixer -q -c 0 sset AirPlay 0% 2>/dev/null || true
}

resume_airplay() {
    # Native AirPlay DACP remote control via Shairport-Sync D-Bus (iPhone displays Play icon)
    dbus-send --system --type=method_call --dest=org.gnome.ShairportSync /org/gnome/ShairportSync org.gnome.ShairportSync.RemoteControl.Play >/dev/null 2>&1 || true
    amixer -q -c 0 sset AirPlay 100% 2>/dev/null || true
}

pause_webradio() {
    killall -STOP mpg123 2>/dev/null || true
}

resume_webradio() {
    killall -CONT mpg123 2>/dev/null || true
}

pause_squeeze() {
    killall -STOP squeezelite 2>/dev/null || true
}

resume_squeeze() {
    killall -CONT squeezelite 2>/dev/null || true
}

do_pause_all() {
    local paused=""
    if pgrep librespot >/dev/null 2>&1; then
        pause_spotify
        paused="$paused spotify"
    fi
    if pgrep shairport-sync >/dev/null 2>&1; then
        pause_airplay
        paused="$paused airplay"
    fi
    if pgrep mpg123 >/dev/null 2>&1; then
        pause_webradio
        paused="$paused webradio"
    fi
    if pgrep squeezelite >/dev/null 2>&1; then
        pause_squeeze
        paused="$paused squeeze"
    fi
    echo "$paused" > "$SAVED_STATE"
}

do_resume_all() {
    if [ -f "$SAVED_STATE" ]; then
        local list=$(cat "$SAVED_STATE" 2>/dev/null)
        for p in $list; do
            case "$p" in
                spotify) resume_spotify ;;
                airplay) resume_airplay ;;
                webradio) resume_webradio ;;
                squeeze) resume_squeeze ;;
            esac
        done
        rm -f "$SAVED_STATE"
    else
        resume_spotify
        resume_airplay
        resume_webradio
        resume_squeeze
    fi
}

case "$ACTION" in
    pause)
        if [ "$TARGET" = "all" ]; then
            do_pause_all
        else
            case "$TARGET" in
                spotify) pause_spotify ;;
                airplay) pause_airplay ;;
                webradio) pause_webradio ;;
                squeeze) pause_squeeze ;;
            esac
        fi
        ;;
    resume|play)
        if [ "$TARGET" = "all" ]; then
            do_resume_all
        else
            case "$TARGET" in
                spotify) resume_spotify ;;
                airplay) resume_airplay ;;
                webradio) resume_webradio ;;
                squeeze) resume_squeeze ;;
            esac
        fi
        ;;
    toggle)
        if [ -f "$SAVED_STATE" ]; then
            do_resume_all
        else
            do_pause_all
        fi
        ;;
    stop)
        killall -9 mpg123 madplay 2>/dev/null || true
        do_pause_all
        ;;
    *)
        echo "Usage: $0 {pause|play|resume|toggle|stop} [target]"
        exit 1
        ;;
esac
