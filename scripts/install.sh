#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Target-side installer for the IMX296 driver + device-tree overlay on a
# Jetson Orin devkit. Run ON the Jetson, as root, with the built artifacts
# (nv_imx296.ko and tegra234-p3767-camera-p3768-imx296-{A,C,dual}.dtbo)
# either next to this script, in the current directory, or given via flags.
#
#   sudo ./install.sh [options]
#
# Options:
#   --ko PATH          path to nv_imx296.ko          (default: auto-detect)
#   --dtbo PATH        path to the overlay .dtbo     (default: auto-detect -A)
#   --variant A|C|dual which overlay to wire into the boot entry (default: A)
#   --label NAME       extlinux label to create      (default: imx296)
#   --set-default      make the new label the DEFAULT boot entry
#                      (default: label is added NON-default - you select it
#                      at the boot menu until you trust it; see SAFETY)
#   --no-autoload      skip the /etc/modules-load.d entry
#   --dry-run          print every action without executing anything
#
# What it does (all steps idempotent, extlinux.conf backed up first):
#   1. verify the .ko vermagic matches the RUNNING kernel (hard abort on
#      mismatch - a version-skewed camera module is how boards get bricked)
#   2. install the module to /lib/modules/$(uname -r)/extra/ + depmod
#   3. copy the .dtbo(s) to /boot/
#   4. add /etc/modules-load.d/nv_imx296.conf (safe even on boots without
#      the overlay: no imx296 DT node -> the module loads and stays idle)
#   5. add an extlinux label cloned from your current default entry
#      (same LINUX/INITRD/APPEND/FDT), with OVERLAYS = the default entry's
#      overlays minus any other camera overlay, plus the imx296 one -
#      so things like jetson-io header configs survive
#
# SAFETY: this script never edits your existing labels. Worst case after
# --set-default is selecting the old label at the boot menu and power-
# cycling. Do NOT --set-default on the first install; boot the label
# manually at least once first.
set -euo pipefail

VARIANT=A
LABEL=imx296
KO=""
DTBO=""
SET_DEFAULT=0
AUTOLOAD=1
DRY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --ko) KO=$2; shift 2 ;;
        --dtbo) DTBO=$2; shift 2 ;;
        --variant) VARIANT=$2; shift 2 ;;
        --label) LABEL=$2; shift 2 ;;
        --set-default) SET_DEFAULT=1; shift ;;
        --no-autoload) AUTOLOAD=0; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

run() { if [ "$DRY" = 1 ]; then echo "DRY: $*"; else "$@"; fi }
say() { echo ">>> $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "run as root (sudo $0 ...)"
grep -qi tegra /proc/device-tree/compatible 2>/dev/null || \
    die "this does not look like a Jetson (run ON the target)"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DTBO_NAME="tegra234-p3767-camera-p3768-imx296-${VARIANT}.dtbo"

# ---- locate artifacts -------------------------------------------------
find_one() {  # find_one <filename> -> first hit among script dir, cwd
    local f
    for f in "$SCRIPT_DIR/$1" "./$1"; do
        [ -f "$f" ] && { echo "$f"; return 0; }
    done
    return 1
}
[ -n "$KO" ]   || KO=$(find_one nv_imx296.ko) || die "nv_imx296.ko not found - pass --ko PATH"
[ -n "$DTBO" ] || DTBO=$(find_one "$DTBO_NAME") || die "$DTBO_NAME not found - pass --dtbo PATH"
[ -f "$KO" ] || die "no such file: $KO"
[ -f "$DTBO" ] || die "no such file: $DTBO"

# ---- 1. vermagic gate --------------------------------------------------
KREL=$(uname -r)
VERMAGIC=$(modinfo -F vermagic "$KO" | awk '{print $1}')
if [ "$VERMAGIC" != "$KREL" ]; then
    die "module vermagic '$VERMAGIC' != running kernel '$KREL' - rebuild the module against this kernel's headers. REFUSING to install."
fi
say "vermagic OK ($VERMAGIC)"

# ---- 2. module ---------------------------------------------------------
MODDIR=/lib/modules/$KREL/extra
say "installing $KO -> $MODDIR/"
run mkdir -p "$MODDIR"
run install -m 644 "$KO" "$MODDIR/nv_imx296.ko"
run depmod

# ---- 3. dtbo -----------------------------------------------------------
say "installing $DTBO -> /boot/"
run install -m 644 "$DTBO" "/boot/$DTBO_NAME"

# ---- 4. auto-load ------------------------------------------------------
if [ "$AUTOLOAD" = 1 ]; then
    say "enabling auto-load (/etc/modules-load.d/nv_imx296.conf)"
    if [ "$DRY" = 1 ]; then echo "DRY: write 'nv_imx296' to /etc/modules-load.d/nv_imx296.conf"
    else echo nv_imx296 > /etc/modules-load.d/nv_imx296.conf; fi
else
    say "skipping auto-load (--no-autoload); load manually: sudo modprobe nv_imx296"
fi

# ---- 5. extlinux label ---------------------------------------------------
EXT=/boot/extlinux/extlinux.conf
[ -f "$EXT" ] || die "$EXT not found"
STAMP=$(date +%Y%m%d-%H%M%S)
say "backing up $EXT -> $EXT.backup-$STAMP"
run cp "$EXT" "$EXT.backup-$STAMP"

if grep -q "^LABEL[[:space:]]\+$LABEL\$" "$EXT"; then
    say "label '$LABEL' already exists - leaving it untouched"
else
    DEFLBL=$(awk '$1=="DEFAULT"{print $2; exit}' "$EXT")
    [ -n "$DEFLBL" ] || die "no DEFAULT entry in $EXT"
    say "cloning boot entry from current default label '$DEFLBL'"
    # extract one field from the default label's block
    getfield() {
        awk -v lbl="$DEFLBL" -v key="$1" '
            $1=="LABEL" { inblk = ($2==lbl) ; next }
            inblk && $1==key { $1=""; sub(/^[[:space:]]+/,""); print; exit }' "$EXT"
    }
    D_LINUX=$(getfield LINUX);  D_INITRD=$(getfield INITRD)
    D_APPEND=$(getfield APPEND); D_FDT=$(getfield FDT)
    D_OVERLAYS=$(getfield OVERLAYS)
    [ -n "$D_LINUX" ] || die "could not parse LINUX from default label '$DEFLBL'"
    if [ -z "$D_FDT" ]; then
        # default label boots the UEFI-provided DT; overlays need an FDT line
        BASE_DTB=$(ls /boot/dtb/kernel_*.dtb 2>/dev/null | head -1)
        [ -n "$BASE_DTB" ] || die "default label has no FDT and no /boot/dtb/kernel_*.dtb found"
        D_FDT=$BASE_DTB
        say "default label has no FDT line; using $BASE_DTB"
    fi
    # keep the default entry's overlays (jetson-io headers etc.) but drop
    # any other camera overlay, then append ours
    NEW_OVERLAYS=$(echo "$D_OVERLAYS" | tr ',' '\n' | \
        grep -v -E 'camera-p3768-(imx219|imx477|imx296)' | paste -sd, -)
    if [ -n "$NEW_OVERLAYS" ]; then NEW_OVERLAYS="$NEW_OVERLAYS,/boot/$DTBO_NAME"
    else NEW_OVERLAYS="/boot/$DTBO_NAME"; fi

    BLOCK=$(printf '\nLABEL %s\n\tMENU LABEL IMX296 GS camera (%s overlay)\n\tLINUX %s\n\tFDT %s\n\tINITRD %s\n\tAPPEND %s\n\tOVERLAYS %s\n' \
        "$LABEL" "$VARIANT" "$D_LINUX" "$D_FDT" "$D_INITRD" "$D_APPEND" "$NEW_OVERLAYS")
    say "adding label '$LABEL' (OVERLAYS: $NEW_OVERLAYS)"
    if [ "$DRY" = 1 ]; then printf 'DRY: append to %s:%s\n' "$EXT" "$BLOCK"
    else printf '%s\n' "$BLOCK" >> "$EXT"; fi
fi

if [ "$SET_DEFAULT" = 1 ]; then
    say "setting DEFAULT $LABEL"
    run sed -i "s/^DEFAULT[[:space:]].*/DEFAULT $LABEL/" "$EXT"
else
    say "label is NON-default (select it at the boot menu; re-run with --set-default once trusted)"
fi

say "done. Reboot to use the camera boot entry."
[ "$SET_DEFAULT" = 1 ] || say "at the boot menu, pick the '$LABEL' entry number within the TIMEOUT window"
