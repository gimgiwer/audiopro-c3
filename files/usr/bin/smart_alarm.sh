#!/bin/sh
# Audio Pro C3 - Smart Alarm Engine
# Fully customizable autonomous alarm with sharp/gentle modes, sound selectors, hard ducking, Snooze & Web REST API

PID_FILE="/tmp/alarm.pid"
STATE_FILE="/tmp/alarm_state.json"
CRON_FILE="/etc/crontabs/root"

duck_down_hard() {
    # Completely suppress or heavily mute background music streams
    amixer -q -c 0 sset Music 0% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 0% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 0% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 0% 2>/dev/null || true
}

duck_restore() {
    # Smoothly restore background music channels
    amixer -q -c 0 sset Music 50% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 50% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 50% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 50% 2>/dev/null || true
    amixer -q -c 0 sset Music 100% 2>/dev/null || true
    amixer -q -c 0 sset Spotify 100% 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 100% 2>/dev/null || true
    amixer -q -c 0 sset Squeeze 100% 2>/dev/null || true
}

set_hw_volume() {
    local v=$1
    if [ -p /tmp/mcu_cmd_fifo ]; then
        printf "AXX+VOL+%03d\n" "$v" > /tmp/mcu_cmd_fifo 2>/dev/null || true
    fi
}

set_alarm_channel_vol() {
    local v=$1
    amixer -q -c 0 sset Alarm "${v}%" 2>/dev/null || true
}

mqtt_pub() {
    local topic=$1
    local payload=$2
    if [ -x /usr/bin/mosquitto_pub ]; then
        local prefix=$(uci -q get mcud.main.mqtt_topic_prefix || echo "audiopro_c3")
        local host=$(uci -q get mcud.main.mqtt_host || echo "127.0.0.1")
        local port=$(uci -q get mcud.main.mqtt_port || echo "1883")
        local user=$(uci -q get mcud.main.mqtt_user || echo "")
        local pass=$(uci -q get mcud.main.mqtt_password || echo "")
        local args="-h $host -p $port -t ${prefix}/${topic} -m $payload"
        [ -n "$user" ] && args="$args -u $user"
        [ -n "$pass" ] && args="$args -P $pass"
        mosquitto_pub $args 2>/dev/null || true
    fi
}

create_pid_file() {
    local tmp="/tmp/alarm.pid.$$"
    echo "$$" > "$tmp"
    mv -f "$tmp" "$PID_FILE" 2>/dev/null || true
}

do_stop() {
    if [ -f "$PID_FILE" ]; then
        local pids=$(cat "$PID_FILE" 2>/dev/null)
        rm -f "$PID_FILE"
        for p in $pids; do
            [ "$p" != "$$" ] && kill -9 "$p" 2>/dev/null || true
        done
    fi
    local extra_pids=$(pgrep -f "mpg123.*alarm_in|aplay.*alarm_in" 2>/dev/null)
    if [ -n "$extra_pids" ]; then
        for ep in $extra_pids; do
            kill -9 "$ep" 2>/dev/null || true
        done
    fi
    duck_restore
    /usr/bin/player_control.sh resume all >/dev/null 2>&1 || true
    printf '{"status":"idle","ringing":false}\n' > "$STATE_FILE"
    mqtt_pub "alarm/status" "idle"
}

do_snooze() {
    local snooze_min=$(uci -q get mcud.alarm.snooze_min || echo "9")
    do_stop
    printf '{"status":"snoozed","snooze_min":%d}\n' "$snooze_min" > "$STATE_FILE"
    mqtt_pub "alarm/status" "snoozed"

    (
        sleep $((snooze_min * 60))
        /usr/bin/smart_alarm.sh start 1
    ) &
}

do_start() {
    local is_test=${1:-0}
    do_stop

    local enabled=$(uci -q get mcud.alarm.enabled || echo "1")
    if [ "$enabled" != "1" ] && [ "$is_test" != "1" ]; then
        exit 0
    fi

    local target_vol=$(uci -q get mcud.alarm.target_volume || echo "60")
    local alarm_mode=$(uci -q get mcud.alarm.alarm_mode || echo "gentle")
    local sound_type=$(uci -q get mcud.alarm.sound_type || echo "chime")
    local sound_file=$(uci -q get mcud.alarm.sound_file || echo "/usr/share/sounds/alarm_wake_up.wav")
    local stream_url=$(uci -q get mcud.alarm.stream_url || echo "http://icecast.vrtcdn.be/klara-high.mp3")
    local fade_sec=$(uci -q get mcud.alarm.fade_sec || echo "0")
    local duration_min=$(uci -q get mcud.alarm.duration_min || echo "30")

    # Stateful transport pause for all active players (Spotify, AirPlay, WebRadio, Squeeze)
    /usr/bin/player_control.sh pause all >/dev/null 2>&1 || true
    duck_down_hard
    set_hw_volume "$target_vol"

    create_pid_file
    printf '{"status":"active","ringing":true,"mode":"%s","volume":%d}\n' "$alarm_mode" "$target_vol" > "$STATE_FILE"
    mqtt_pub "alarm/status" "active"

    # Volume Setup (Sharp = instant 100%, Gentle = fade-in)
    if [ "$alarm_mode" = "sharp" ] || [ "$fade_sec" -le 0 ]; then
        set_alarm_channel_vol 100
    else
        set_alarm_channel_vol 10
        (
            local cur=10
            local steps=10
            local delay=$((fade_sec / steps))
            [ "$delay" -lt 1 ] && delay=1
            local step_v=$(((100 - 10) / steps))
            local i=0
            while [ "$i" -lt "$steps" ]; do
                sleep "$delay"
                [ ! -f "$PID_FILE" ] && exit 0
                cur=$((cur + step_v))
                [ "$cur" -gt 100 ] && cur=100
                set_alarm_channel_vol "$cur"
                i=$((i + 1))
            done
            set_alarm_channel_vol 100
        ) &
    fi

    # Audio Playback Loop
    (
        local end_time=$(($(date +%s) + (duration_min * 60)))
        
        if [ "$sound_type" = "chime" ] || [ "$sound_type" = "chime_then_stream" ]; then
            local repeats=0
            while [ $(date +%s) -lt "$end_time" ] && [ -f "$PID_FILE" ]; do
                if [ -f "$sound_file" ]; then
                    aplay -q -D alarm_in "$sound_file" 2>/dev/null || aplay -q "$sound_file" 2>/dev/null || true
                else
                    aplay -q -D alarm_in /usr/share/sounds/bell.wav 2>/dev/null || true
                fi
                repeats=$((repeats + 1))
                if [ "$sound_type" = "chime_then_stream" ] && [ "$repeats" -ge 3 ]; then
                    break
                fi
                sleep 1
            done
        fi

        if [ "$sound_type" = "stream" ] || [ "$sound_type" = "chime_then_stream" ]; then
            if [ $(date +%s) -lt "$end_time" ] && [ -f "$PID_FILE" ]; then
                mpg123 -a alarm_in "$stream_url" >/dev/null 2>&1
            fi
        fi

        if [ "$sound_type" = "spotify" ]; then
            local spot_uri=$(uci -q get mcud.alarm.spotify_uri || echo "spotify:track:4cOdK2wGLETKBW3PvgPWqT")
            mqtt_pub "alarm/trigger_spotify" "{\"uri\":\"$spot_uri\",\"device\":\"Audio Pro C3\"}"
            amixer -q -c 0 sset Spotify 100% 2>/dev/null || true
            /usr/bin/player_control.sh resume spotify >/dev/null 2>&1 || true
            
            # Short intro gong while Spotify connects
            aplay -q -D alarm_in /usr/share/sounds/bell.wav 2>/dev/null || true

            local waited=0
            local is_playing=0
            while [ "$waited" -lt 6 ] && [ -f "$PID_FILE" ]; do
                sleep 1
                waited=$((waited + 1))
                if [ -f /tmp/audiopro_meta.json ] && grep -q '"source": "spotify"' /tmp/audiopro_meta.json 2>/dev/null; then
                    is_playing=1
                    break
                fi
            done

            # Reliable fallback chime if Spotify is offline or network fails
            if [ "$is_playing" -eq 0 ] && [ -f "$PID_FILE" ]; then
                while [ $(date +%s) -lt "$end_time" ] && [ -f "$PID_FILE" ]; do
                    if [ -f "$sound_file" ]; then
                        aplay -q -D alarm_in "$sound_file" 2>/dev/null || aplay -q -D alarm_in /usr/share/sounds/alarm_sharp.wav 2>/dev/null || true
                    else
                        aplay -q -D alarm_in /usr/share/sounds/alarm_sharp.wav 2>/dev/null || true
                    fi
                    sleep 1
                done
            fi
        fi

        do_stop
    ) &
}

do_sync_cron() {
    local enabled=$(uci -q get mcud.alarm.enabled || echo "0")
    local time_str=$(uci -q get mcud.alarm.time || echo "07:00")
    local days_str=$(uci -q get mcud.alarm.days || echo "1,2,3,4,5")

    local hour=$(echo "$time_str" | cut -d: -f1)
    local min=$(echo "$time_str" | cut -d: -f2)
    hour=${hour:-07}
    min=${min:-00}
    hour=$((10#$hour))
    min=$((10#$min))

    mkdir -p /etc/crontabs
    touch "$CRON_FILE"
    grep -v "smart_alarm.sh" "$CRON_FILE" > "${CRON_FILE}.tmp" || true

    if [ "$enabled" = "1" ]; then
        echo "$min $hour * * $days_str /usr/bin/smart_alarm.sh start" >> "${CRON_FILE}.tmp"
    fi

    mv "${CRON_FILE}.tmp" "$CRON_FILE"
    /etc/init.d/cron reload 2>/dev/null || /etc/init.d/cron restart 2>/dev/null || true
}

case "$1" in
    start|trigger)
        do_start 0
        ;;
    test)
        do_start 1
        ;;
    stop|dismiss)
        do_stop
        ;;
    snooze)
        do_snooze
        ;;
    sync_cron)
        do_sync_cron
        ;;
    status)
        if [ -f "$STATE_FILE" ]; then
            cat "$STATE_FILE"
        else
            echo '{"status":"idle","ringing":false}'
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|test|snooze|sync_cron|status}"
        exit 1
        ;;
esac
