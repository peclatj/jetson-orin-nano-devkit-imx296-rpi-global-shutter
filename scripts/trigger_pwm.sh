#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# trigger_pwm.sh - drive the IMX296 trigger pulse train on 40-pin header
# pin 32 (PWM7). Requires the clk_m clock overlay (see
# doc/external-trigger-howto.md) for camera-rate periods; run as root.
#
#   trigger_pwm.sh RATE_HZ EXPOSURE_US [inverted|normal]
#   trigger_pwm.sh park       # line held HIGH, no pulses (camera idle)
#   trigger_pwm.sh off        # PWM disabled - NOTE: with divider wiring
#                             # the line falls LOW (= XTR asserted)
#   trigger_pwm.sh status
#
# Modes (match your XTR level-shift wiring):
#   inverted (default) - low pulse of EXPOSURE_US at RATE_HZ, idle high.
#       For the plain non-inverting 1.5k/1.8k divider: the low pulse IS
#       the exposure. (duty = period - exposure; Tegra PWM cannot invert
#       in hardware.)
#   normal - high pulse of EXPOSURE_US. For an inverting MOSFET stage,
#       which turns it into the active-low pulse at the XTR pad.
#
# Achievable rates quantize to 19.2MHz/(256*n), n<=8192: 20/30/60/120 Hz
# are exact, e.g. 45 -> 44.99 Hz. Exposure quantizes to period/256
# (65.1 us at 60 Hz) - measure the real pulse width and pass it to the
# camera element's `exposure` property.
set -euo pipefail

CTRL=32e0000.pwm   # PWM7 = header pin 32

die() { echo "ERROR: $*" >&2; exit 1; }

chip=""
for c in /sys/class/pwm/pwmchip*; do
    [ "$(basename "$(readlink "$c/device")")" = "$CTRL" ] && chip=$c
done
[ -n "$chip" ] || die "$CTRL not found (is this the Orin?)"
[ -d "$chip/pwm0" ] || echo 0 > "$chip/export"
pwm=$chip/pwm0

case "${1:-}" in
status)
    grep -A2 "$CTRL" /sys/kernel/debug/pwm 2>/dev/null || cat "$pwm/period" "$pwm/duty_cycle" "$pwm/enable"
    exit 0 ;;
off)
    echo 0 > "$pwm/enable"
    echo "PWM disabled (divider wiring: line is now LOW = XTR asserted)"
    exit 0 ;;
park)
    period=$(cat "$pwm/period")
    [ "$period" -gt 0 ] || { echo 16666666 > "$pwm/period"; period=16666666; }
    echo "$period" > "$pwm/duty_cycle"
    echo 1 > "$pwm/enable"
    echo "parked: line constant HIGH (verify no 1/256 blips on a scope once)"
    exit 0 ;;
''|-h|--help)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

rate=$1
exp_us=$2
mode=${3:-inverted}
[ "$rate" -gt 0 ] 2>/dev/null || die "RATE_HZ must be a positive integer"
[ "$exp_us" -gt 0 ] 2>/dev/null || die "EXPOSURE_US must be a positive integer"

period=$((1000000000 / rate))
exp_ns=$((exp_us * 1000))
[ "$exp_ns" -lt "$period" ] || die "exposure ${exp_us}us must be shorter than the period ($((period/1000))us at ${rate}Hz)"

case "$mode" in
inverted) duty=$((period - exp_ns)) ;;
normal)   duty=$exp_ns ;;
*) die "mode must be 'inverted' or 'normal'" ;;
esac

# order matters: sysfs rejects period < current duty
echo 0 > "$pwm/duty_cycle"
echo "$period" > "$pwm/period"
echo "$duty" > "$pwm/duty_cycle"
echo 1 > "$pwm/enable"

echo "pin 32: ${rate} Hz requested, exposure ${exp_us} us, mode ${mode}"
grep -A2 "$CTRL" /sys/kernel/debug/pwm 2>/dev/null || true
