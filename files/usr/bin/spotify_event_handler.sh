#!/bin/sh
case "$PLAYER_EVENT" in
    playing|start)
        ubus call mcud set_source '{"source": "wifi"}' 2>/dev/null || true
        ubus call mcud set_mute '{"mute": 0}' 2>/dev/null || true
        ;;
esac
