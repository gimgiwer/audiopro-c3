#!/usr/bin/env bash
#
# One-shot firmware build for the Audio Pro Addon C3 (Linkplay A28 / MT7628AN).
#
#   ./build.sh              # clone OpenWrt into ./openwrt-src and build
#   ./build.sh /some/dir    # use/keep the tree at /some/dir instead
#
# Needs the usual OpenWrt build deps (build-essential, ncurses-dev, python3,
# rsync, unzip, gawk, subversion, git, wget). No Docker, no root.

set -euo pipefail

# Pinned to the exact release this firmware was built and tested against.
# Kernel 5.15.167. The audio patches in patches/ are written for that kernel;
# on a newer branch they will not apply cleanly.
OPENWRT_TAG="v23.05.5"
OPENWRT_URL="https://git.openwrt.org/openwrt/openwrt.git"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$REPO_ROOT/openwrt-src}"

say() { printf '\n=== %s ===\n' "$*"; }

if [ ! -d "$SRC/.git" ]; then
    say "cloning OpenWrt $OPENWRT_TAG"
    git clone --depth 1 --branch "$OPENWRT_TAG" "$OPENWRT_URL" "$SRC"
else
    say "reusing existing tree at $SRC"
fi

cd "$SRC"

say "feeds"
# Feed revisions are pinned by feeds.conf.default in the release tag itself,
# so this stays reproducible without extra work here.
./scripts/feeds update -a
./scripts/feeds install -a

say "grafting the C3 BSP"
"$REPO_ROOT/scripts/install_to_openwrt.sh" "$SRC"

say "expanding config"
# install_to_openwrt.sh drops the diffconfig in as .config; defconfig expands it
# and pulls in every dependency.
make defconfig

say "verifying our device got selected"
grep -q 'CONFIG_TARGET_ramips_mt76x8_DEVICE_audiopro_c3=y' .config || {
    echo "audiopro_c3 is not selected in .config - aborting" >&2
    exit 1
}

say "building (this takes a while on a first run)"
make -j"$(nproc)"

say "output"
ls -la bin/targets/ramips/mt76x8/*.bin 2>/dev/null || {
    echo "no image produced" >&2; exit 1; }

cat <<'EOF'

Flash the *-squashfs-sysupgrade.bin.

First time onto stock, or recovering a brick: use the Breed bootloader web
recovery, not sysupgrade. Read docs/FLASHING.md first - the layout in dts/
reclaims the vendor 'user'/'user2' regions for the overlay, so stock presets
and vendor certificates are lost. The 'factory' partition holding the MAC
address is left alone.
EOF
