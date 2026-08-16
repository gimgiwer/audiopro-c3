#!/bin/sh
# Shairport-sync metadata reader and cache writer

PIPE="/tmp/shairport-sync-meta"
META_JSON="/tmp/audiopro_meta.json"
ARTWORK_JPG="/tmp/audiopro_artwork.jpg"

[ ! -p "$PIPE" ] && mkfifo "$PIPE" 2>/dev/null

TITLE=""
ARTIST=""
ALBUM=""
PLAYING="true"

update_json() {
    local has_art=0
    [ -f "$ARTWORK_JPG" ] && [ -s "$ARTWORK_JPG" ] && has_art=1
    
    local e_title=$(printf '%s' "$TITLE" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' '  ')
    local e_artist=$(printf '%s' "$ARTIST" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' '  ')
    local e_album=$(printf '%s' "$ALBUM" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' '  ')
    
    local tmp_file="${META_JSON}.tmp.$$"
    cat << JSON_EOF > "$tmp_file"
{
  "active": true,
  "source": "airplay",
  "title": "$e_title",
  "artist": "$e_artist",
  "album": "$e_album",
  "playing": $PLAYING,
  "artwork": $([ "$has_art" -eq 1 ] && echo "true" || echo "false"),
  "updated": $(date +%s)
}
JSON_EOF
    mv -f "$tmp_file" "$META_JSON" 2>/dev/null || true
}

# Main event loop over XML pipe
while true; do
    if [ -p "$PIPE" ]; then
        while read -r line; do
            case "$line" in
                *6d696e6d*) # Title (minm)
                    read -r dline
                    DATA=$(echo "$dline" | sed -n "s/.*<data[^>]*>\(.*\)<\/data>.*/\1/p")
                    if [ -n "$DATA" ]; then
                        TITLE=$(echo "$DATA" | base64 -d 2>/dev/null)
                        update_json
                    fi
                    ;;
                *61736172*) # Artist (asar)
                    read -r dline
                    DATA=$(echo "$dline" | sed -n "s/.*<data[^>]*>\(.*\)<\/data>.*/\1/p")
                    if [ -n "$DATA" ]; then
                        ARTIST=$(echo "$DATA" | base64 -d 2>/dev/null)
                        update_json
                    fi
                    ;;
                *6173616c*) # Album (asal)
                    read -r dline
                    DATA=$(echo "$dline" | sed -n "s/.*<data[^>]*>\(.*\)<\/data>.*/\1/p")
                    if [ -n "$DATA" ]; then
                        ALBUM=$(echo "$DATA" | base64 -d 2>/dev/null)
                        update_json
                    fi
                    ;;
                *50494354*) # Artwork (PICT)
                    read -r dline
                    DATA=$(echo "$dline" | sed -n "s/.*<data[^>]*>\(.*\)<\/data>.*/\1/p")
                    if [ -n "$DATA" ]; then
                        echo "$DATA" | base64 -d > "$ARTWORK_JPG" 2>/dev/null
                        update_json
                    fi
                    ;;
                *70737474*) # Play stream status (pstt)
                    PLAYING="true"
                    /usr/bin/audio_arbiter.sh airplay &
                    update_json
                    ;;
                *70667374*) # Pause/flush stream status (pfst)
                    PLAYING="false"
                    update_json
                    ;;
            esac
        done < "$PIPE"
    fi
    sleep 1
done
