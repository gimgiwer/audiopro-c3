#!/bin/sh
# Universal Preset Action Dispatcher for Audio Pro C3
PRESET="$1"
[ -z "$PRESET" ] && exit 0

. /usr/lib/audiopro/kill_audio.sh

CFG_FILE="/etc/config/audiopro_presets"
[ ! -f "$CFG_FILE" ] && exit 0

MODE=$(uci -q get audiopro_presets."$PRESET".mode 2>/dev/null || echo "ha")
URL=$(uci -q get audiopro_presets."$PRESET".url 2>/dev/null || echo "")
NAME=$(uci -q get audiopro_presets."$PRESET".name 2>/dev/null || echo "Preset $PRESET")
CMD=$(uci -q get audiopro_presets."$PRESET".command 2>/dev/null || echo "")

case "$MODE" in
    radio|stream)
        # Kill any active standalone stream players
        kill_stream
        
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
            
            /usr/bin/play_stream.sh "$URL" music_in >/dev/null 2>&1 &
        fi
        ;;
        
    command)
        # The api has no authentication, so this is root on the lan: the mode is not
        # offered in the web preset editor and the flag has to be set over ssh. The old
        # auth_enabled check that used to sit here made the whole branch dead - nothing
        # on the image can verify a password.
        if [ -z "$CMD" ]; then
            :
        elif [ "$(uci -q get mcud.main.allow_custom_commands || echo 0)" = "1" ]; then
            eval "$CMD" >/dev/null 2>&1 &
        else
            logger -t audiopro_preset "preset $PRESET command blocked: allow_custom_commands is 0"
        fi
        ;;
        
    ha|*)
        # Nothing to do: mcud publishes the mqtt trigger before it spawns us, for the
        # front-panel key and for the web's ubus trigger_preset alike.
        ;;
esac

exit 0
