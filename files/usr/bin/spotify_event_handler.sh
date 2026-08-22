#!/bin/sh
# librespot --onevent hook. Forked once per event, so anything outside the cases
# below must stay free: only what the ui actually shows does work here.

META="/tmp/audiopro_meta.json"

write_meta() {
    e_title=$(printf '%s' "$NAME" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' '  ')
    e_album=$(printf '%s' "$ALBUM" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' '  ')
    # ARTISTS and COVERS arrive newline separated, covers largest first
    e_artist=$(printf '%s' "$ARTISTS" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\r\n' ',,' | sed 's/,*$//; s/,/, /g')
    cover=$(printf '%s' "$COVERS" | sed -n 1p | tr -d '"\\ ')

    tmp="$META.tmp.$$"
    cat > "$tmp" <<JSON
{
  "active": true,
  "source": "spotify",
  "title": "$e_title",
  "artist": "$e_artist",
  "album": "$e_album",
  "playing": $1,
  "artwork": false,
  "artwork_url": "$cover",
  "updated": $(date +%s)
}
JSON
    mv -f "$tmp" "$META" 2>/dev/null || true
}

case "$PLAYER_EVENT" in
    track_changed)
        write_meta true
        ;;
    playing|start)
        ubus call mcud set_source '{"source": "wifi"}' 2>/dev/null || true
        ubus call mcud set_mute '{"mute": 0}' 2>/dev/null || true
        sed -i 's/"playing": false/"playing": true/' "$META" 2>/dev/null || true
        ;;
    paused)
        sed -i 's/"playing": true/"playing": false/' "$META" 2>/dev/null || true
        ;;
    stopped|session_disconnected)
        # airplay owns the same cache file, so only clear our own entry
        grep -q '"source": "spotify"' "$META" 2>/dev/null && echo '{"active":false}' > "$META"
        ;;
esac
exit 0
