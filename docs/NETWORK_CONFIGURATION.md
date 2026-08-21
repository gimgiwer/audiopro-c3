# Network Configuration

The speaker runs two Wi-Fi interfaces off the single MT7628 radio at the same time:
a setup access point that is always up, and an optional station link to your own
network. All values below are read from `files/etc/config/{wireless,network,dhcp,uhttpd,system}`.

## 1. Setup access point (default, always on)

Out of the box the speaker publishes its own network:

| | |
|---|---|
| SSID | `AudioPro-C3-Setup` |
| Passphrase | `setup12345` |
| Encryption | `psk2` (WPA2-PSK) |
| Address | `10.10.10.254/24` (interface `ap`) |
| DHCP pool | `10.10.10.100` – `10.10.10.199` (start 100, limit 100), 1 h leases |

Change the passphrase before putting the speaker on an untrusted network — it is a
published default, so treat it as public.

To connect:

```bash
ssh root@10.10.10.254                 # no password on a fresh image
curl http://10.10.10.254/api/status   # REST API
```

The API prefix is `/api` (`uhttpd.main.lua_prefix`), served by `/www/api.lua`.
uhttpd listens on `:80` and `:443` on both IPv4 and IPv6, so `https://` works on
every one of those addresses too — see the networking table in the README.

## 2. Joining your own Wi-Fi (station mode)

`sta_iface` ships disabled with an empty SSID so the image carries nobody's
credentials. Fill it in over SSH:

```bash
uci set wireless.sta_iface.ssid='your-network'
uci set wireless.sta_iface.encryption='psk2'
uci set wireless.sta_iface.key='your-passphrase'
uci set wireless.sta_iface.disabled='0'
uci commit wireless
/etc/init.d/network restart
```

Leave `ap_iface` enabled. Keeping the setup AP up costs one virtual interface and
is the only way back in if the station link fails, which is why `wwan` carries a
higher route metric (600) than `lan` (100) rather than the AP being torn down.

## 3. Finding the speaker afterwards

`system.@system[0].hostname` is `audiopro`, so avahi advertises:

* `http://audiopro.local` and `https://audiopro.local`
* AirPlay via shairport-sync — appears on iOS/macOS automatically
* Spotify Connect via librespot — appears in the Spotify device list
* MQTT for Home Assistant on port 1883 (plaintext; `mcud.main.mqtt_port`)

Ethernet (`eth0`) is configured as `lan` with `proto dhcp`, so if your unit exposes
a wired port it takes an address from your router — it is *not* `10.10.10.254`.
That address only ever belongs to the wireless setup AP.

## 4. Getting back in after a Wi-Fi change

The setup AP is unaffected by station-mode settings, so join `AudioPro-C3-Setup`
and reach `10.10.10.254` as in section 1. If it was disabled:

```bash
uci set wireless.ap_iface.disabled='0'
uci commit wireless && /etc/init.d/network restart
```
