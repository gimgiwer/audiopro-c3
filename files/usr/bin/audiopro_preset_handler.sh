#!/bin/sh
# Universal Preset Action Dispatcher for Audio Pro C3
PRESET="$1"
[ -z "$PRESET" ] && exit 0

CFG_FILE="/etc/config/audiopro_presets"
[ ! -f "$CFG_FILE" ] && exit 0

MODE=$(uci -q get audiopro_presets."$PRESET".mode 2>/dev/null || echo "ha")
URL=$(uci -q get audiopro_presets."$PRESET".url 2>/dev/null || echo "")
NAME=$(uci -q get audiopro_presets."$PRESET".name 2>/dev/null || echo "Preset $PRESET")
CMD=$(uci -q get audiopro_presets."$PRESET".command 2>/dev/null || echo "")

case "$MODE" in
    radio|stream)
        # Kill any active standalone stream players
        # TERM, not -9: a hard-killed alsa client leaves the dmix segment in a
        # state where every later open on default blocks forever.
        killall -TERM mpg123 madplay 2>/dev/null || true
        sleep 1
        killall -KILL mpg123 madplay 2>/dev/null || true
        
        if [ -n "$URL" ]; then
            # Validate URL protocol scheme
            case "$URL" in
                http://*|https://*|rtsp://*) ;;
                *) logger -t audiopro_preset "Blocked invalid URL scheme: $URL"; exit 1 ;;
            esac

            # Invoke Audio Arbiter
            /usr/bin/audio_arbiter.sh webradio
            
            # Escape quotes to prevent JSON injection
            E_NAME=$(echo "$NAME" | sed 's/"/\\"/g')
            
            # Update Now Playing metadata
            cat << JSON_EOF > /tmp/audiopro_meta.json
{
  "active": true,
  "source": "webradio",
  "title": "$E_NAME",
  "artist": "Web Radio",
  "album": "Preset $PRESET",
  "playing": true,
  "artwork": false,
  "updated": $(date +%s)
}
JSON_EOF
            
            # Launch background audio stream with argument injection protection
            if command -v mpg123 >/dev/null 2>&1; then
                mpg123 -q -a music_in -- "$URL" >/dev/null 2>&1 &
            elif command -v wget >/dev/null 2>&1; then
                wget -q -O - -- "$URL" | aplay -D music_in -q - >/dev/null 2>&1 &
            fi
        fi
        ;;
        
    spotify)
        # Send preset command to librespot / player pipe
        echo "preset:$PRESET" > /tmp/player_cmd 2>/dev/null || true
        ;;
        
    command)
        if [ -n "$CMD" ]; then
            # SECURITY CHECK: Block execution if auth is disabled or commands are explicitly forbidden
            ALLOW_CMD=$(uci -q get mcud.main.allow_custom_commands || echo "0")
            AUTH_EN=$(uci -q get mcud.main.auth_enabled || echo "0")
            
            if [ "$AUTH_EN" = "0" ]; then
                echo "SECURITY ALERT: Preset command blocked! Cannot run scripts in passwordless (AUTH_ENABLED=0) mode." > /dev/console
            elif [ "${ALLOW_CMD:-0}" != "1" ]; then
                echo "SECURITY ALERT: Preset command blocked! ALLOW_CUSTOM_COMMANDS is 0." > /dev/console
            else
                eval "$CMD" >/dev/null 2>&1 &
            fi
        fi
        ;;
        
    ha|*)
        # Home Assistant MQTT event is already dispatched instantly by mcud
        ;;
esac

exit 0
