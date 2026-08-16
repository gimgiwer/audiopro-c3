#!/bin/sh
ACTION="${1:-status}"
HA_SERVER="${2:-192.168.1.100}"
PORT="${3:-5000}"

case "$ACTION" in
    enable|start)
        insmod snd-aloop 2>/dev/null || true
        cat > /etc/asound.conf << 'ASOUND_EOF'
pcm.dmixer {
    type dmix
    ipc_key 1024
    ipc_perm 0666
    slave {
        pcm "hw:Loopback,0,0"
        mmap_emulation 1
        period_time 0
        period_size 1024
        buffer_size 4096
        rate 44100
        channels 2
        format S16_LE
    }
    bindings { 0 0  1 1 }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Spotify" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control { name "AirPlay" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Music" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default { type plug; slave.pcm "music_in"; }
ctl.!default { type hw; card 0; }
ASOUND_EOF
        sync
        sleep 0.3
        killall -9 aec_bridge 2>/dev/null || true
        
        # Start native C daemon (handles hardware DAC bridge and TCP stream with ~0.2% CPU)
        /usr/bin/aec_bridge "$HA_SERVER" "$PORT" >/dev/null 2>&1 &
        
        # Restart audio daemons to bind to new asound.conf
        /etc/init.d/librespot restart 2>/dev/null || true
        /etc/init.d/shairport-sync restart 2>/dev/null || true
        /etc/init.d/squeezelite restart 2>/dev/null || true
        
        echo "AEC Loopback Tap ENABLED (via aec_bridge C daemon) -> streaming to $HA_SERVER:$PORT"
        ;;
    disable|stop)
        cat > /etc/asound.conf << 'ASOUND_DIRECT'
pcm.dmixer {
    type dmix
    ipc_key 1024
    ipc_perm 0666
    slave {
        pcm "hw:0,0"
        mmap_emulation 1
        period_time 0
        period_size 1024
        buffer_size 4096
        rate 44100
        channels 2
        format S16_LE
    }
    bindings { 0 0  1 1 }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Spotify" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control { name "AirPlay" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Music" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default { type plug; slave.pcm "music_in"; }
ctl.!default { type hw; card 0; }
ASOUND_DIRECT
        sync
        sleep 0.3
        killall -9 aec_bridge 2>/dev/null || true
        
        # Restart audio daemons to bind to restored direct DAC asound.conf
        /etc/init.d/librespot restart 2>/dev/null || true
        /etc/init.d/shairport-sync restart 2>/dev/null || true
        /etc/init.d/squeezelite restart 2>/dev/null || true
        
        echo "AEC Loopback Tap DISABLED -> restored direct zero-latency DAC mode"
        ;;
    status)
        if pgrep -x aec_bridge >/dev/null; then
            echo "AEC Tap Mode: ACTIVE (Native aec_bridge C daemon running)"
        else
            echo "AEC Tap Mode: INACTIVE (Direct DAC hw:0,0)"
        fi
        ;;
    *)
        echo "Usage: $0 {enable [HA_IP] [PORT] | disable | status}"
        ;;
esac
