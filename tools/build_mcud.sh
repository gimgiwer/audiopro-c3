#!/bin/sh
# rebuild mcud against the buildroot staging dir. libcares.so.2 / libjson-c.so.5
# used to have no SONAME symlink in staging, so ld could not chase libmosquitto's
# and libblobmsg_json's transitive refs - $T/mcudlibs carries stand-ins for
# whichever of them is still missing.
set -e
B=/home/gimgiwer/.gemini/antigravity/scratch/openwrt_buildroot_backup
T=/tmp/mcud-build
TC=$B/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl
ST=$B/staging_dir/target-mipsel_24kc_musl
export STAGING_DIR=$B/staging_dir
SRC="${1:-$(dirname "$0")/../openwrt/package/utils/mcud/src/mcud.c}"
OUT="${2:-/tmp/mcud.built}"
mkdir -p "$T/mcudlibs/usr/lib"
for so in libcares.so.2 libjson-c.so.5; do
	[ -e "$T/mcudlibs/$so" ] && continue
	real=$(ls "$ST/usr/lib/$so".* 2>/dev/null | head -1) || true
	[ -n "$real" ] && ln -sf "$real" "$T/mcudlibs/$so"
done
$TC/bin/mipsel-openwrt-linux-gcc -Os -pipe -mno-branch-likely -mips32r2 -mtune=24kc \
  -fno-caller-saves -fno-plt -msoft-float -fstack-protector -Wall -Wextra \
  -I$ST/usr/include -o "$OUT" "$SRC" \
  -L$ST/usr/lib -L$ST/lib -L$T/mcudlibs -L$T/mcudlibs/usr/lib \
  -Wl,-znow -Wl,-zrelro -Wl,-rpath-link=$ST/usr/lib:$ST/lib:$T/mcudlibs:$T/mcudlibs/usr/lib \
  -lasound -lmosquitto -lubus -lubox -lblobmsg_json
$TC/bin/mipsel-openwrt-linux-strip "$OUT"
ls -l "$OUT"; md5sum "$OUT"
