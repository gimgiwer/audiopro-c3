#!/bin/sh
# Librespot player event hook script
# Receives env variables: PLAYER_EVENT, NAME, ARTISTS, ALBUM, DURATION_MS, COVERS

META_JSON="/tmp/audiopro_meta.json"
ARTWORK_JPG="/tmp/audiopro_artwork.jpg"

EVENT="${PLAYER_EVENT:-$1}"

case "$EVENT" in
    changed|playing|started|preloading)
        TITLE="${NAME:-Unknown Track}"
        ARTIST="${ARTISTS:-Unknown Artist}"
        ALBUM_NAME="${ALBUM:-}"
        
        e_title=$(echo "$TITLE" | sed "s/\"/\\\\\"/g")
        e_artist=$(echo "$ARTIST" | sed "s/\"/\\\\\"/g")
        e_album=$(echo "$ALBUM_NAME" | sed "s/\"/\\\\\"/g")
        
        if [ -n "$COVERS" ]; then
            IMG_URL=$(echo "$COVERS" | tr -d "\"[]\r\n " | tr "," "\n" | grep "^http" | head -n1)
            if [ -n "$IMG_URL" ]; then
                wget -q -O "$ARTWORK_JPG" "$IMG_URL" 2>/dev/null || true
            fi
        fi
        
        has_art=0
        [ -f "$ARTWORK_JPG" ] && [ -s "$ARTWORK_JPG" ] && has_art=1
        
        cat << JSON_EOF > "$META_JSON"
{
  "active": true,
  "source": "spotify",
  "title": "$e_title",
  "artist": "$e_artist",
  "album": "$e_album",
  "playing": true,
  "artwork": $([ "$has_art" -eq 1 ] && echo "true" || echo "false"),
  "updated": $(date +%s)
}
JSON_EOF
        ;;
        
    paused|stopped)
        if [ -f "$META_JSON" ]; then
            sed -i "s/\"playing\": true/\"playing\": false/" "$META_JSON" 2>/dev/null || true
        fi
        ;;
        
    session_disconnected|session_ended)
        rm -f "$META_JSON" "$ARTWORK_JPG" 2>/dev/null
        ;;
esac
