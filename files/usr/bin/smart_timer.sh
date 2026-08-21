#!/bin/sh
play_file() {  # $1=file $2=alsa slot; mp3 через mpg123, остальное через aplay
    case "$1" in
        *.mp3|*.MP3) mpg123 -q -a "$2" -- "$1" 2>/dev/null ;;
        *) aplay -q -D "$2" -- "$1" 2>/dev/null ;;
    esac
}
# Audio Pro C3 - Smart Countdown Timer Engine
# Handles background countdowns, MQTT state broadcasting, and ALSA alarm chime on completion

STATE_FILE="/tmp/timer_state.json"
PID_FILE="/tmp/timer.pid"
RING_PID_FILE="/tmp/timer_ring.pid"

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
    local tmp="/tmp/timer.pid.$$"
    echo "$$" > "$tmp"
    mv -f "$tmp" "$PID_FILE" 2>/dev/null || true
}

do_stop_ringing() {
    if [ -f "$RING_PID_FILE" ]; then
        local pids=$(cat "$RING_PID_FILE" 2>/dev/null)
        rm -f "$RING_PID_FILE"
        for p in $pids; do
            [ "$p" != "$$" ] && kill -9 "$p" 2>/dev/null || true
        done
    fi
    local extra_pids=$(pgrep -f "mpg123.*timer_in|aplay.*timer_in" 2>/dev/null)
    if [ -n "$extra_pids" ]; then
        for ep in $extra_pids; do
            kill -9 "$ep" 2>/dev/null || true
        done
    fi
    # Restore Alarm volume if Alarm was playing
    amixer -q -c 0 sset Alarm 100% 2>/dev/null || true
    # Only resume background players if alarm is not actively ringing
    if [ ! -f /tmp/alarm.pid ]; then
        /usr/bin/player_control.sh resume all >/dev/null 2>&1 || true
    fi
    printf '{"active":false,"ringing":false,"remaining":0,"total":0,"name":""}\n' > "$STATE_FILE"
    mqtt_pub "timer/status" '{"active":false,"ringing":false,"remaining":0}'
}

do_cancel() {
    if [ -f "$PID_FILE" ]; then
        local pids=$(cat "$PID_FILE" 2>/dev/null)
        rm -f "$PID_FILE"
        for p in $pids; do
            [ "$p" != "$$" ] && kill -9 "$p" 2>/dev/null || true
        done
    fi
    do_stop_ringing
}

do_ring() {
    local sound_file="$1"
    local volume="$2"
    local ring_duration=60 # Ring up to 60 seconds

    local tmp_ring="/tmp/timer_ring.pid.$$"
    echo "$$" > "$tmp_ring"
    mv -f "$tmp_ring" "$RING_PID_FILE" 2>/dev/null || true
    printf '{"active":false,"ringing":true,"remaining":0,"total":0,"name":"%s"}\n' "$TIMER_NAME" > "$STATE_FILE"
    mqtt_pub "timer/status" '{"active":false,"ringing":true,"remaining":0}'

    # Pause active music streams and set volume
    /usr/bin/player_control.sh pause all >/dev/null 2>&1 || true
    if [ -p /tmp/mcu_cmd_fifo ]; then
        printf "AXX+VOL+%03d\n" "$volume" > /tmp/mcu_cmd_fifo 2>/dev/null || true
    fi

    # Priority Ducking: If alarm is currently active, duck it to 25% so timer is crystal clear
    if [ -f /tmp/alarm.pid ]; then
        amixer -q -c 0 sset Alarm 25% 2>/dev/null || true
    fi
    amixer -q -c 0 sset Timer 100% 2>/dev/null || true

    local end_ring=$(($(date +%s) + ring_duration))
    while [ $(date +%s) -lt "$end_ring" ] && [ -f "$RING_PID_FILE" ]; do
        if [ -f "$sound_file" ]; then
            play_file "$sound_file" timer_in || play_file /usr/share/sounds/timer_sharp.mp3 timer_in || true
        else
            play_file /usr/share/sounds/timer_sharp.mp3 timer_in || true
        fi
        sleep 0.4
    done

    do_stop_ringing
}

do_start() {
    local total_sec=${1:-300}
    local name=${2:-"Timer"}
    local sound_file=${3:-"/usr/share/sounds/timer_sharp.mp3"}
    local volume=${4:-70}

    [ "$total_sec" -le 0 ] && exit 0
    do_cancel

    create_pid_file
    local start_ts=$(date +%s)
    local target_ts=$((start_ts + total_sec))

    while [ $(date +%s) -lt "$target_ts" ] && [ -f "$PID_FILE" ]; do
        local now=$(date +%s)
        local remaining=$((target_ts - now))
        [ "$remaining" -lt 0 ] && remaining=0

        printf '{"active":true,"ringing":false,"remaining":%d,"total":%d,"name":"%s"}\n' \
            "$remaining" "$total_sec" "$name" > "$STATE_FILE"
        
        # Publish MQTT every 5 seconds or when under 10 seconds
        if [ $((remaining % 5)) -eq 0 ] || [ "$remaining" -le 10 ]; then
            mqtt_pub "timer/status" "$(cat "$STATE_FILE")"
        fi

        sleep 1
    done

    rm -f "$PID_FILE"
    TIMER_NAME="$name"
    do_ring "$sound_file" "$volume"
}

case "$1" in
    start)
        do_start "$2" "$3" "$4" "$5" &
        ;;
    cancel|stop|dismiss)
        do_cancel
        ;;
    status)
        if [ -f "$STATE_FILE" ]; then
            cat "$STATE_FILE"
        else
            echo '{"active":false,"ringing":false,"remaining":0,"total":0,"name":""}'
        fi
        ;;
    *)
        echo "Usage: $0 {start <seconds> [name] [sound_file] [volume]|cancel|status}"
        exit 1
        ;;
esac
