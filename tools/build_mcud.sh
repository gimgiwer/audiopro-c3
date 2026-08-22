#!/bin/sh
# rebuild mcud against the buildroot staging dir. libcares.so.2 / libjson-c.so.5
# have no SONAME symlink in staging, so ld cannot chase libmosquitto's and
# libblobmsg_json's transitive refs without $T/mcudlibs.
set -e
B=/home/gimgiwer/.gemini/antigravity/scratch/openwrt_buildroot_backup
T=/home/gimgiwer/.claude/jobs/34151320/tmp
TC=$B/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl
ST=$B/staging_dir/target-mipsel_24kc_musl
export STAGING_DIR=$B/staging_dir
SRC="${1:-$(dirname "$0")/../openwrt/package/utils/mcud/src/mcud.c}"
OUT="${2:-/tmp/mcud.built}"
$TC/bin/mipsel-openwrt-linux-gcc -Os -pipe -mno-branch-likely -mips32r2 -mtune=24kc \
  -fno-caller-saves -fno-plt -msoft-float -fstack-protector -Wall -Wextra \
  -I$ST/usr/include -o "$OUT" "$SRC" \
  -L$ST/usr/lib -L$ST/lib -L$T/mcudlibs -L$T/mcudlibs/usr/lib \
  -Wl,-znow -Wl,-zrelro -Wl,-rpath-link=$ST/usr/lib:$ST/lib:$T/mcudlibs:$T/mcudlibs/usr/lib \
  -lasound -lmosquitto -lubus -lubox -lblobmsg_json
$TC/bin/mipsel-openwrt-linux-strip "$OUT"
ls -l "$OUT"; md5sum "$OUT"
