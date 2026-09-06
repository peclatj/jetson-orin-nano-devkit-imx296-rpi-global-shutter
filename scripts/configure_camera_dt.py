#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# configure_camera_dt.py - configure the Jetson boot device tree for one or
# two RPi Global Shutter (IMX296) cameras, optionally enabling a 40-pin
# header PWM pin as the shared camera trigger source.
#
# Modeled on /opt/nvidia/jetson-io/jetson-io.py and its headless siblings
# (config-by-function.py etc.): like them, this never flashes anything - it
# edits /boot/extlinux/extlinux.conf (backed up first) and manages DTB
# overlays in /boot, applied by the bootloader at the next boot.
#
#   sudo ./configure_camera_dt.py --cams dual --pwm 32 --set-default
#   sudo ./configure_camera_dt.py --cams a
#   ./configure_camera_dt.py --list          # inspect, no root needed
#
# What it does:
#   1. picks the IMX296 overlay for the camera layout:
#        --cams a    -> tegra234-p3767-camera-p3768-imx296-A.dtbo  (CAM0/J20)
#        --cams c    -> tegra234-p3767-camera-p3768-imx296-C.dtbo  (CAM1/J21)
#        --cams dual -> tegra234-p3767-camera-p3768-imx296-dual.dtbo
#   2. with --pwm PIN (15, 32 and/or 33): regenerates the jetson-io header
#      overlay via NVIDIA's own config-by-function.py, which is CUMULATIVE -
#      it reads the live header configuration and adds the PWM function, so
#      existing pin assignments (GPS UART, PPS, ...) survive.
#   3. writes ONE extlinux label owned by this script (default: imx296io),
#      cloned from the current default entry with every other camera overlay
#      removed and the IMX296 overlay appended. Existing labels are never
#      edited; re-running replaces only the script's own label.
#   4. preflights the exact overlay stack with fdtoverlay against the base
#      FDT - a merge that would fail at boot aborts the script instead.
#   5. --set-default makes the new label the boot default; --reboot reboots
#      (the configuration only takes effect after a reboot, like jetson-io).
#
# Safety model (same as install.sh): existing labels are never modified,
# extlinux.conf is backed up with a timestamp, and the previous default
# label remains selectable at the boot menu.

import argparse
import datetime
import filecmp
import glob
import os
import re
import shutil
import subprocess
import sys

EXTLINUX = '/boot/extlinux/extlinux.conf'
BOOT = '/boot'
JETSON_IO_DIR = '/opt/nvidia/jetson-io'
CONFIG_BY_FUNCTION = os.path.join(JETSON_IO_DIR, 'config-by-function.py')
HDR40_OVERLAY = os.path.join(BOOT, 'jetson-io-hdr40-user-custom.dtbo')

CAM_OVERLAYS = {
    'a':    'tegra234-p3767-camera-p3768-imx296-A.dtbo',
    'c':    'tegra234-p3767-camera-p3768-imx296-C.dtbo',
    'dual': 'tegra234-p3767-camera-p3768-imx296-dual.dtbo',
}
CAM_DESC = {
    'a':    'single camera on CAM0 (J20)',
    'c':    'single camera on CAM1 (J21)',
    'dual': 'dual cameras (CAM0 + CAM1)',
}
# any other camera overlay in the cloned entry gets dropped
OTHER_CAMERA_RE = re.compile(
    r'camera-p3768-(imx219|imx477|imx296)[^,]*\.dtbo')

# 40-pin header PWM pins on the Orin Nano/NX devkit carrier (p3768), from
# jetson-io's own function list; controller mapping verified via
# /sys/class/pwm/pwmchip*/device on target.
PWM_PINS = {
    15: {'func': 'pwm1', 'pinmux': 'soc_gpio39_pn1', 'ctrl': '3280000.pwm'},
    32: {'func': 'pwm7', 'pinmux': 'soc_gpio19_pg6', 'ctrl': '32e0000.pwm'},
    33: {'func': 'pwm5', 'pinmux': 'soc_gpio21_ph0', 'ctrl': '32c0000.pwm'},
}


def say(msg):
    print('>>> {}'.format(msg))


def die(msg):
    sys.exit('ERROR: {}'.format(msg))


def run(cmd, dry, **kwargs):
    if dry:
        print('DRY: {}'.format(' '.join(cmd)))
        return None
    return subprocess.run(cmd, check=True, **kwargs)


# ---------------------------------------------------------------------------
# extlinux.conf handling
# ---------------------------------------------------------------------------

class ExtlinuxConf(object):
    """Minimal LABEL-block-preserving parser, jetson-io style: everything
    outside the managed label is kept verbatim."""

    def __init__(self, path):
        self.path = path
        with open(path) as f:
            self.text = f.read()

    def default_label(self):
        m = re.search(r'^DEFAULT\s+(\S+)', self.text, re.M)
        return m.group(1) if m else None

    def labels(self):
        return re.findall(r'^LABEL\s+(\S+)', self.text, re.M)

    def label_block(self, name):
        m = re.search(
            r'^LABEL\s+{}\s*\n(?:^[ \t]+\S.*\n?)*'.format(re.escape(name)),
            self.text, re.M)
        return m.group(0) if m else None

    def label_field(self, name, field):
        block = self.label_block(name)
        if not block:
            return None
        m = re.search(r'^[ \t]+{}\s+(.*\S)\s*$'.format(field), block, re.M)
        return m.group(1) if m else None

    def replace_or_append_label(self, name, block):
        old = self.label_block(name)
        if old:
            self.text = self.text.replace(old, block)
        else:
            self.text = self.text.rstrip('\n') + '\n\n' + block

    def set_default(self, name):
        self.text = re.sub(r'^DEFAULT\s+\S+', 'DEFAULT {}'.format(name),
                           self.text, count=1, flags=re.M)

    def save(self, dry):
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        backup = '{}.backup-{}'.format(self.path, stamp)
        if dry:
            print('DRY: backup {} -> {}'.format(self.path, backup))
            print('DRY: would write new {}'.format(self.path))
            return
        shutil.copy2(self.path, backup)
        say('backed up {} -> {}'.format(self.path, backup))
        tmp = self.path + '.tmp'
        with open(tmp, 'w') as f:
            f.write(self.text)
        os.replace(tmp, self.path)


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------

def find_cam_overlay(variant, dry):
    """The overlay must live in /boot for the bootloader to read it;
    install.sh normally puts it there. If only the on-device kernel build
    tree has it, install a copy."""
    name = CAM_OVERLAYS[variant]
    installed = os.path.join(BOOT, name)
    if os.path.isfile(installed):
        return installed
    for cand in glob.glob(os.path.expanduser(
            '~/imx296-build/Linux_for_Tegra/source/kernel-devicetree/'
            'generic-dts/dtbs/' + name)):
        say('installing {} -> {}'.format(cand, installed))
        if not dry:
            shutil.copy2(cand, installed)
        return installed
    die('{} not found in {} - run install.sh (or build_dtbo.sh) first'
        .format(name, BOOT))


def enable_pwm(pins, dry):
    """Regenerate the jetson-io 40-pin header overlay with the PWM
    function(s) added. config-by-function.py is cumulative: it reads the
    LIVE header configuration and merges, so existing assignments (GPS
    UART etc.) are preserved. The previous overlay is kept as a backup.
    extlinux.conf is snapshotted around the call so only this script ever
    edits it."""
    if not os.path.isfile(CONFIG_BY_FUNCTION):
        die('{} not found - is this a Jetson with jetson-io installed?'
            .format(CONFIG_BY_FUNCTION))
    funcs = [PWM_PINS[p]['func'] for p in pins]
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    if os.path.isfile(HDR40_OVERLAY):
        backup = '{}.backup-{}'.format(HDR40_OVERLAY, stamp)
        if dry:
            print('DRY: backup {} -> {}'.format(HDR40_OVERLAY, backup))
        else:
            shutil.copy2(HDR40_OVERLAY, backup)
            say('backed up {} -> {}'.format(HDR40_OVERLAY, backup))
    ext_snapshot = EXTLINUX + '.pre-jetson-io'
    if not dry:
        shutil.copy2(EXTLINUX, ext_snapshot)
    run([sys.executable, CONFIG_BY_FUNCTION, '-o', 'dtbo'] + funcs, dry)
    if not dry:
        if not filecmp.cmp(EXTLINUX, ext_snapshot):
            shutil.copy2(ext_snapshot, EXTLINUX)
            say('config-by-function touched extlinux.conf - restored')
        os.unlink(ext_snapshot)
        say('header overlay updated: {} (+{})'
            .format(HDR40_OVERLAY, ','.join(funcs)))
    return HDR40_OVERLAY


def build_overlay_list(conf, src_label, cam_overlay, want_hdr40):
    overlays = conf.label_field(src_label, 'OVERLAYS') or ''
    keep = [o for o in overlays.split(',')
            if o.strip() and not OTHER_CAMERA_RE.search(o)]
    if want_hdr40 and HDR40_OVERLAY not in keep:
        keep.insert(0, HDR40_OVERLAY)
    keep.append(cam_overlay)
    return keep


def preflight_merge(base_fdt, overlays, dry):
    """Refuse to write a boot entry whose overlay stack does not merge -
    the same check the bootloader will do, done now instead of at boot."""
    if dry:
        print('DRY: fdtoverlay -i {} {}'.format(base_fdt, ' '.join(overlays)))
        return
    if not shutil.which('fdtoverlay'):
        say('WARNING: fdtoverlay not available - skipping merge preflight')
        return
    out = '/tmp/configure_camera_dt.preflight.dtb'
    r = subprocess.run(['fdtoverlay', '-i', base_fdt, '-o', out] + overlays,
                       capture_output=True, text=True)
    if r.returncode != 0:
        die('overlay preflight merge FAILED - not touching extlinux.conf:\n'
            + r.stderr.strip())
    os.unlink(out)
    say('preflight OK: base FDT + {} overlay(s) merge cleanly'
        .format(len(overlays)))


def make_label_block(name, conf, src_label, overlays, variant, pwm_pins):
    fields = {f: conf.label_field(src_label, f)
              for f in ('LINUX', 'FDT', 'INITRD', 'APPEND')}
    if not fields['LINUX']:
        die("cannot parse LINUX from label '{}'".format(src_label))
    if not fields['FDT']:
        cands = sorted(glob.glob(os.path.join(BOOT, 'dtb', 'kernel_*.dtb')))
        if not cands:
            die("label '{}' has no FDT and no /boot/dtb/kernel_*.dtb exists"
                .format(src_label))
        fields['FDT'] = cands[0]
        say('source label has no FDT line; using {}'.format(fields['FDT']))
    desc = CAM_DESC[variant]
    if pwm_pins:
        desc += ' + trigger PWM on pin {}'.format(
            '/'.join(str(p) for p in pwm_pins))
    lines = ['LABEL {}'.format(name),
             '\tMENU LABEL IMX296 {}'.format(desc)]
    for f in ('LINUX', 'FDT', 'INITRD', 'APPEND'):
        if fields[f]:
            lines.append('\t{} {}'.format(f, fields[f]))
    lines.append('\tOVERLAYS {}'.format(','.join(overlays)))
    return '\n'.join(lines) + '\n'


def show_current(conf):
    print('DEFAULT: {}'.format(conf.default_label()))
    for name in conf.labels():
        marker = ' *' if name == conf.default_label() else ''
        print('\nLABEL {}{}'.format(name, marker))
        for f in ('FDT', 'OVERLAYS'):
            v = conf.label_field(name, f)
            if v:
                for part in ([v] if f != 'OVERLAYS' else v.split(',')):
                    print('    {}: {}'.format(f, part))
    print('\nPWM pins (40-pin header): ' + ', '.join(
        'pin {} = {} ({})'.format(p, d['func'], d['ctrl'])
        for p, d in sorted(PWM_PINS.items())))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Configure the Jetson device tree for RPi GS (IMX296) '
                    'cameras and an optional trigger-PWM header pin. '
                    'Changes take effect at the NEXT BOOT.')
    ap.add_argument('--cams', choices=sorted(CAM_OVERLAYS),
                    help='camera layout: a = CAM0/J20, c = CAM1/J21, '
                         'dual = both connectors')
    ap.add_argument('--pwm', type=int, action='append', default=[],
                    choices=sorted(PWM_PINS), metavar='PIN',
                    help='enable a 40-pin header PWM pin for the camera '
                         'trigger line (15=pwm1, 32=pwm7, 33=pwm5); '
                         'repeatable')
    ap.add_argument('--extra-overlay', action='append', default=[],
                    metavar='DTBO',
                    help='additional overlay to include in the boot entry '
                         '(repeatable), e.g. /boot/tegra234-p3767-imx296-'
                         'trigger-pwm7-clk.dtbo for exact-60Hz trigger PWM')
    ap.add_argument('--label', default='imx296io',
                    help='extlinux label owned by this script '
                         '(default: %(default)s)')
    ap.add_argument('--set-default', action='store_true',
                    help='make the label the boot default')
    ap.add_argument('--reboot', action='store_true',
                    help='reboot when done (config only applies at boot)')
    ap.add_argument('--dry-run', action='store_true',
                    help='print every action without executing')
    ap.add_argument('--list', action='store_true',
                    help='show current boot entries and exit')
    args = ap.parse_args()

    if not os.path.isfile(EXTLINUX):
        die('{} not found (run this ON the Jetson)'.format(EXTLINUX))
    conf = ExtlinuxConf(EXTLINUX)

    if args.list:
        show_current(conf)
        return
    if not args.cams:
        ap.error('--cams is required (or use --list)')
    if os.geteuid() != 0 and not args.dry_run:
        die('run as root (sudo {})'.format(' '.join(sys.argv)))

    src_label = conf.default_label()
    if not src_label:
        die('no DEFAULT entry in {}'.format(EXTLINUX))
    # Cloning the script's own label (after a prior --set-default) is fine
    # and intended: it is exactly what boots today, with the full overlay
    # stack; build_overlay_list strips camera overlays before re-adding the
    # requested one, so re-runs are idempotent. Redirecting to another
    # label here would silently drop non-camera overlays (PPS, header).

    cam_overlay = find_cam_overlay(args.cams, args.dry_run)
    say('camera layout: {} ({})'.format(CAM_DESC[args.cams], cam_overlay))

    pwm_pins = sorted(set(args.pwm))
    if pwm_pins:
        for p in pwm_pins:
            d = PWM_PINS[p]
            say('trigger PWM: header pin {} ({}, pinmux {}, /sys chip for '
                '{})'.format(p, d['func'], d['pinmux'], d['ctrl']))
        enable_pwm(pwm_pins, args.dry_run)

    overlays = build_overlay_list(conf, src_label, cam_overlay,
                                  want_hdr40=bool(pwm_pins))
    for extra in args.extra_overlay:
        if not os.path.isfile(extra):
            die('extra overlay not found: {}'.format(extra))
        if extra not in overlays:
            overlays.append(extra)
    base_fdt = conf.label_field(src_label, 'FDT')
    if not base_fdt:
        cands = sorted(glob.glob(os.path.join(BOOT, 'dtb', 'kernel_*.dtb')))
        base_fdt = cands[0] if cands else None
    if base_fdt:
        preflight_merge(base_fdt, overlays, args.dry_run)

    block = make_label_block(args.label, conf, src_label, overlays,
                             args.cams, pwm_pins)
    conf.replace_or_append_label(args.label, block)
    if args.set_default:
        conf.set_default(args.label)
    conf.save(args.dry_run)

    say("label '{}' {} (cloned from '{}')".format(
        args.label,
        'written' if not args.dry_run else 'planned',
        src_label))
    say('OVERLAYS: {}'.format(','.join(overlays)))
    if args.set_default:
        say("DEFAULT set to '{}'".format(args.label))
    else:
        say("label is NON-default - select it at the boot menu, or re-run "
            "with --set-default")
    say('REBOOT REQUIRED for the device tree change to take effect')

    if args.reboot:
        say('rebooting now...')
        run(['reboot'], args.dry_run)


if __name__ == '__main__':
    main()
