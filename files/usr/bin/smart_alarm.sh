#!/bin/sh
# Audio Pro C3 - Smart Alarm Engine
# Autonomous scheduled alarm with ALSA alarm_in routing, hardware ducking, MQTT & REST API control

PID_FILE="/tmp/alarm.pid"
STATE_FILE="/tmp/alarm_state.json"
CRON_FILE="/etc/crontabs/root"

duck_down() {
    amixer -q -c 0 sset Music 30%- 2>/dev/null || true
    amixer -q -c 0 sset Spotify 30%- 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 30%- 2>/dev/null || true
}

duck_up() {
    amixer -q -c 0 sset Music 30%+ 2>/dev/null || true
    amixer -q -c 0 sset Spotify 30%+ 2>/dev/null || true
    amixer -q -c 0 sset AirPlay 30%+ 2>/dev/null || true
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

do_stop() {
    if [ -f "$PID_FILE" ]; then
        local pids=$(cat "$PID_FILE" 2>/dev/null)
        for p in $pids; do
            kill -9 "$p" 2>/dev/null || true
        done
        rm -f "$PID_FILE"
    fi
    killall -9 mpg123_alarm 2>/dev/null || true
    duck_up
    printf '{"status":"idle"}\n' > "$STATE_FILE"
    mqtt_pub "alarm/status" "idle"
}

do_start() {
    local is_test=${1:-0}
    do_stop

    local enabled=$(uci -q get mcud.alarm.enabled || echo "1")
    if [ "$enabled" != "1" ] && [ "$is_test" != "1" ]; then
        exit 0
    fi

    local target_vol=$(uci -q get mcud.alarm.target_volume || echo "40")
    local stream_url=$(uci -q get mcud.alarm.stream_url || echo "http://icecast.vrtcdn.be/klara-high.mp3")
    local sound_file=$(uci -q get mcud.alarm.sound || echo "/usr/share/sounds/bell.wav")
    local fade_sec=$(uci -q get mcud.alarm.fade_sec || echo "60")
    local duration_min=$(uci -q get mcud.alarm.duration_min || echo "60")

    duck_down
    set_hw_volume "$target_vol"
    set_alarm_channel_vol 5

    printf '{"status":"active","volume":%d,"stream":"%s"}\n' "$target_vol" "$stream_url" > "$STATE_FILE"
    mqtt_pub "alarm/status" "active"

    # Play chime to alarm_in channel
    if [ -f "$sound_file" ]; then
        aplay -q -D alarm_in "$sound_file" 2>/dev/null || aplay -q "$sound_file" 2>/dev/null || true
    fi

    # Start audio stream
    mpg123 -a alarm_in "$stream_url" >/dev/null 2>&1 &
    local stream_pid=$!
    echo "$stream_pid $$" > "$PID_FILE"

    # Non-blocking smooth fade-in
    local steps=10
    local step_delay=$((fade_sec / steps))
    [ "$step_delay" -lt 1 ] && step_delay=1
    local vol_step=$(((target_vol - 5) / steps))
    [ "$vol_step" -lt 1 ] && vol_step=1

    local cur_vol=5
    local i=0
    while [ "$i" -lt "$steps" ]; do
        sleep "$step_delay"
        [ ! -f "$PID_FILE" ] && exit 0
        cur_vol=$((cur_vol + vol_step))
        [ "$cur_vol" -gt 100 ] && cur_vol=100
        set_alarm_channel_vol "$cur_vol"
        i=$((i + 1))
    done
    set_alarm_channel_vol 100

    # Auto-stop after duration
    local total_wait=$((duration_min * 60))
    local elapsed=0
    while [ "$elapsed" -lt "$total_wait" ]; do
        sleep 5
        [ ! -f "$PID_FILE" ] && exit 0
        elapsed=$((elapsed + 5))
    done

    do_stop
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

    # Remove existing smart_alarm lines
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
        do_start 0 &
        ;;
    test)
        do_start 1 &
        ;;
    stop|dismiss)
        do_stop
        ;;
    sync_cron)
        do_sync_cron
        ;;
    status)
        if [ -f "$STATE_FILE" ]; then
            cat "$STATE_FILE"
        else
            echo '{"status":"idle"}'
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|test|sync_cron|status}"
        exit 1
        ;;
esac
