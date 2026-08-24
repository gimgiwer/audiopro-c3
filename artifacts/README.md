# Prebuilt binaries

## librespot

Lives at `files/usr/bin/librespot` (7 351 724 bytes, md5
`5e967cb26ae2f5124811d92e9f2a6ff7`), so it is part of the image like anything
else under `files/` - no post-flash copy step. Byte-identical to
`/usr/bin/librespot` on the speaker.

It fits: `firmware` is 16064k (mtd3, 0x00fb0000 per `/proc/mtd` on the speaker)
and the built sysupgrade image is 10 486 584 bytes, leaving ~5.7 MB of headroom.

Built from upstream `9c7d75615fc093bdcbdb29adbce3fed38c531852` (librespot dev)
with `patches/librespot/0001..0003` applied, rust nightly + build-std, stripped.
`scripts/build_librespot.sh` reproduces it.

The credential cache is the one thing a flash does not carry:
`/root/.cache/librespot/credentials.json` stays OUT of this repo - it is a login
credential. Without it the user has to pick the speaker in Spotify once, which
also writes the file back.
