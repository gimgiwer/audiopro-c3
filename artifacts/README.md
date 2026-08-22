# Deployed binaries that cannot fit the squashfs

## librespot (7 351 724 bytes, md5 5e967cb26ae2f5124811d92e9f2a6ff7)

Built from upstream `9c7d75615fc093bdcbdb29adbce3fed38c531852` (librespot dev)
with `patches/librespot/0001..0003` applied, rust nightly + build-std, stripped.
This is byte-identical to `/usr/bin/librespot` on the speaker.

The firmware partition is 16064k and a full image without librespot is
already ~9.1 MB, so baking this 7.0 MB binary in does not fit. Deploy it on
the overlay instead, after sysupgrade:

    ssh c3 'cat > /usr/bin/librespot' < artifacts/librespot
    ssh c3 'chmod 0755 /usr/bin/librespot; /etc/init.d/librespot restart'

Also restore the credential cache or the user has to re-link Spotify:
`/root/.cache/librespot/credentials.json` (keep OUT of this repo - it is a
login credential).
