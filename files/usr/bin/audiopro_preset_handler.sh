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
        killall -9 mpg123 madplay 2>/dev/null || true
        
        if [ -n "$URL" ]; then
            # Invoke Audio Arbiter
            /usr/bin/audio_arbiter.sh webradio
            
            # Update Now Playing metadata
            cat << JSON_EOF > /tmp/audiopro_meta.json
{
  "active": true,
  "source": "webradio",
  "title": "$NAME",
  "artist": "Web Radio",
  "album": "Preset $PRESET",
  "playing": true,
  "artwork": false,
  "updated": $(date +%s)
}
JSON_EOF
            
            # Launch background audio stream
            if command -v mpg123 >/dev/null 2>&1; then
                mpg123 -q -a music_in "$URL" >/dev/null 2>&1 &
            elif command -v wget >/dev/null 2>&1; then
                wget -q -O - "$URL" | aplay -D music_in -q - >/dev/null 2>&1 &
            fi
        fi
        ;;
        
    spotify)
        # Send preset command to librespot / player pipe
        echo "preset:$PRESET" > /tmp/player_cmd 2>/dev/null || true
        ;;
        
    command)
        if [ -n "$CMD" ]; then
            eval "$CMD" >/dev/null 2>&1 &
        fi
        ;;
        
    ha|*)
        # Home Assistant MQTT event is already dispatched instantly by mcud
        ;;
esac

exit 0
