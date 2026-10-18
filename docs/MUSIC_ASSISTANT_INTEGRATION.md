# Music Assistant & Spotify Connect Integration

This document describes how Audio Pro C3 connects to Music Assistant, how the Slimproto / LMS CLI audio and control path works, and how to configure Spotify Connect for bit-perfect unity-gain playback with synchronized volume controls.

---

## 1. Architecture Overview

```
                      +---------------------------------------+
                      |       Music Assistant Server          |
                      |                                       |
                      |  +---------------------------------+  |
                      |  |     spotify_connect provider    |  |
                      |  |     (Spotify Soloist eSDK)      |  |
                      |  +---------------------------------+  |
                      |                  |                    |
                      |                  v                    |
                      |  +---------------------------------+  |
                      |  | Player Queue (02:22:6c:b8:85:ac)|  |
                      |  | volume_normalization: disabled  |  |
                      |  +---------------------------------+  |
                      |                  |                    |
                      |                  v (Slimproto :3483)  |
                      +------------------|--------------------+
                                         |
                       Wi-Fi (10.0.100.3)| LMS CLI (:9090)
                                         v
                      +---------------------------------------+
                      |         Audio Pro C3 Speaker          |
                      |                                       |
                      |  +-------------+     +-------------+  |
                      |  | squeezelite |     |    mcud     |  |
                      |  +-------------+     +-------------+  |
                      |         |                   ^         |
                      |         v                   | UART    |
                      |  ALSA (squeeze_in)          v         |
                      |  [Slimproto 0dB]     STM8S Secondary  |
                      |  [Master 0dB]        Microcontroller  |
                      |         |            (Keys / Battery) |
                      |         v                             |
                      |  TI PCM5102A DAC (I2S Master 64fs)    |
                      |         |                             |
                      |         v                             |
                      |  TI TPA3116D2 Class-D Amplifier       |
                      +---------------------------------------+
```

---

## 2. Audio Path & Headroom Configuration

By default, Music Assistant applies EBU R128 (-14.0 LUFS) volume normalization, and Spotify Connect applies internal loudness normalization. For modern masters (-7 to -9 LUFS), this cuts peak output by **7 to 10 dB** (over 50% perceived acoustic power loss).

To achieve 100% full hardware output power at 0 dBFS peak on the TI TPA3116D2 amplifier:

### 2.1. Player Queue Settings
In `/data/settings.json` (or via Web UI -> Players -> Audio Pro C3 -> Queue Settings):
```json
{
  "player_queues": {
    "02:22:6c:b8:85:ac": {
      "values": {
        "volume_normalization": "disabled"
      }
    }
  }
}
```
*Disabling `volume_normalization` prevents ffmpeg `loudnorm` and `volume=-XdB` filters from attenuating the stream.*

### 2.2. Spotify Connect Settings
In `/data/settings.json` (or Web UI -> Settings -> Providers -> Spotify Connect):
```json
{
  "providers": {
    "spotify_connect": {
      "values": {
        "volume_mode": "sync_spotify",
        "loudness_normalization": false
      }
    }
  }
}
```
- **`volume_mode: "sync_spotify"`**: Allows volume adjustment from the official Spotify app while keeping Pulse capture sink gain at unity gain (`10000 / volume` reciprocal) so the audio pipe always carries uncompressed audio.
  *(Note: Do NOT use `player_only` mode, as `player_only` intentionally rejects any Spotify app slider movement and forces it back to 100%).*
- **`loudness_normalization: false`**: Disables dynamic range compression in the Soloist eSDK.

---

## 3. Bidirectional Volume Synchronization

When volume is adjusted using physical hardware buttons on the speaker's top panel, `mcud` sends `mixer volume %+2` via LMS CLI (:9090) to Music Assistant.

To forward this player volume change back to the Spotify app slider, Music Assistant requires the `EventType.PLAYER_UPDATED` event handler in `provider.py`:

```diff
--- a/music_assistant/providers/spotify_connect/provider.py
+++ b/music_assistant/providers/spotify_connect/provider.py
@@ -628,6 +628,15 @@ class SpotifyConnectProvider(PluginProvider):
                     self._clear_active_player(daemon)
                     await self._stop_daemon(daemon)
             return
+        if event.event == EventType.PLAYER_UPDATED and event.object_id:
+            if daemon := self._daemons.get(event.object_id):
+                player = self.mass.players.get_player(event.object_id)
+                if player and player.state.volume_level is not None:
+                    if daemon.last_volume_sent != player.state.volume_level:
+                        self.mass.create_task(
+                            self._sync_player_volume_to_spotify(daemon, event.object_id)
+                        )
+            return
         if event.event == EventType.PLAYER_ADDED and event.object_id:
             # a re-registered player earns a permanently failed daemon a fresh start
             self._failed_player_ids.discard(event.object_id)
```

The patch is provided in `patches/music-assistant/spotify_connect_bidirectional_volume.patch`.
