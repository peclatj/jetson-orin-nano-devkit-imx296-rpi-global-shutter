#!/usr/bin/env python3
"""
Minimal IMX296 ISP pipeline: raw Bayer capture -> debayer -> white balance
-> CCM -> gamma, using the real per-sensor calibration from imx296_16mm.json
(libcamera/Raspberry Pi tuning data), since NVIDIA's Argus ISP has no way
to load it.

This bypasses nvarguscamerasrc/Argus entirely and works directly on the raw
V4L2 capture, which is where the tuning data's CCM/gamma are meant to apply
(linear RGB, post-WB, pre-gamma) - not on Argus's already-processed output.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

VIDEO_DEVICE = "/dev/video0"  # check with: v4l2-ctl --list-devices

# mode0, full array. 1456*2=2912 bytes/line is NOT a multiple of 64, and
# Orin's VI corrupts raw capture when the line stride isn't 64-byte aligned
# (confirmed by direct byte-level inspection). Cropping to mode1's 1440 width
# avoided this by luck (1440*2=2880=45*64 is aligned), but the real fix is to
# force a 64-byte-aligned stride explicitly via the "preferred_stride" V4L2
# control (in bytes, not pixels) and then strip the resulting per-line
# padding on read - STRIDE below is the smallest 64-byte-aligned value at or
# above WIDTH*2 (2912 -> 2944). Confirmed working: padding is now a clean,
# per-line 32 bytes rather than the messy every-2-rows pattern at the
# default (unset) stride.
WIDTH, HEIGHT = 1456, 1088
STRIDE = 2944  # bytes/line requested via `-c preferred_stride=`; must stay a multiple of 64 and >= WIDTH*2
TUNING_FILE = Path(__file__).parent / "imx296_16mm.json"

# The sensor's CFA order flips depending on which pixel_phase happens to be
# flashed in the current .dtbo (we cycled through several during bring-up),
# and the V4L2 driver reports whichever one is actually active via the
# fourcc. Detecting it at runtime instead of hardcoding one avoids silently
# processing with a stale assumption after a reflash.
#
# OpenCV's Bayer-pattern naming convention is offset by one diagonal step
# from V4L2/DT's convention (confirmed empirically: V4L2 RGGB == OpenCV
# COLOR_BayerBG, not COLOR_BayerRG) - these mappings already account for
# that offset, don't "fix" them back to the naively-matching cv2 constant.
#
# Deliberately _2BGR_EA, not _2RGB_EA: OpenCV's "_2RGB" Bayer constants for
# these patterns are numerically IDENTICAL to the complementary pattern's
# "_2BGR" constant (e.g. COLOR_BayerBG2RGB_EA == COLOR_BayerRG2BGR_EA) - they
# don't independently produce true R,G,B order despite the name. Confirmed
# empirically with a synthetic frame. process_frame() reverses the channel
# axis right after demosaic to get real RGB order for the CCM/WB math below.
BAYER_CODE_BY_FOURCC = {
    "RG10": cv2.COLOR_BayerBG2BGR_EA,
    "BG10": cv2.COLOR_BayerRG2BGR_EA,
    "GR10": cv2.COLOR_BayerGB2BGR_EA,
    "GB10": cv2.COLOR_BayerGR2BGR_EA,
}

# Scene illuminant to select from the tuning file's per-CT tables. Your
# fluorescent lighting falls in rpi.awb.modes.fluorescent (lo:4000, hi:4700),
# closest to the 4640K ct_curve entry and 4560K ccm entry.
TARGET_CT = 4560


def load_tuning(path):
    with open(path) as f:
        data = json.load(f)

    def find(name):
        for algo in data["algorithms"]:
            if name in algo:
                return algo[name]
        raise KeyError(f"{name} not found in {path}")

    black_level = find("rpi.black_level")["black_level"]  # 16-bit domain

    ccms = find("rpi.ccm")["ccms"]
    ccm_entry = min(ccms, key=lambda c: abs(c["ct"] - TARGET_CT))
    ccm = np.array(ccm_entry["ccm"], dtype=np.float64).reshape(3, 3)

    ct_curve = find("rpi.awb")["ct_curve"]
    triples = [ct_curve[i : i + 3] for i in range(0, len(ct_curve), 3)]
    ct, r_ratio, b_ratio = min(triples, key=lambda t: abs(t[0] - TARGET_CT))
    wb_gains = np.array([1.0 / r_ratio, 1.0, 1.0 / b_ratio])

    gamma_curve = np.array(find("rpi.contrast")["gamma_curve"], dtype=np.float64)
    gamma_x = gamma_curve[0::2] / 65535.0
    gamma_y = gamma_curve[1::2] / 65535.0

    return {
        "black_level_16bit": black_level,
        "ccm": ccm,
        "ccm_ct": ccm_entry["ct"],
        "wb_gains": wb_gains,
        "wb_ct": ct,
        "gamma_x": gamma_x,
        "gamma_y": gamma_y,
    }


def detect_pixelformat(device, width, height):
    out = subprocess.run(
        ["v4l2-ctl", f"--device={device}", "--list-formats-ext"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout

    target_size = f"{width}x{height}"
    current_fourcc = None
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r"\[\d+\]:\s*'(\w+)'", line)
        if m:
            current_fourcc = m.group(1)
            continue
        if line.startswith("Size:") and target_size in line and current_fourcc:
            return current_fourcc

    raise RuntimeError(
        f"no pixel format on {device} advertises {target_size} - "
        f"run 'v4l2-ctl --list-formats-ext -d {device}' to see what's actually there"
    )


def capture_raw_frame(device=VIDEO_DEVICE, width=WIDTH, height=HEIGHT, stride=STRIDE):
    fourcc = detect_pixelformat(device, width, height)
    if fourcc not in BAYER_CODE_BY_FOURCC:
        raise RuntimeError(
            f"detected pixelformat {fourcc!r} on {device} has no known Bayer "
            f"mapping - add it to BAYER_CODE_BY_FOURCC"
        )

    raw_path = "/tmp/imx296_frame.raw"
    subprocess.run(
        [
            "v4l2-ctl",
            f"--device={device}",
            f"--set-fmt-video=width={width},height={height},pixelformat={fourcc}",
            "-c",
            f"preferred_stride={stride}",
            "--stream-mmap",
            "--stream-count=1",
            f"--stream-to={raw_path}",
        ],
        check=True,
    )
    raw = np.fromfile(raw_path, dtype=np.uint8)
    expected = stride * height
    if raw.size != expected:
        raise RuntimeError(
            f"expected {expected} bytes ({height} rows x {stride}-byte stride), "
            f"got {raw.size} - preferred_stride may not have taken effect, check "
            f"'v4l2-ctl --get-fmt-video -d {device}' (Bytes per Line should be {stride})"
        )
    # Strip the per-line stride padding (stride - width*2 bytes at the end of
    # each row) before reinterpreting as uint16 - viewing a byte-truncated
    # (but still last-axis-contiguous) slice as uint16 is valid in numpy
    # without an explicit copy.
    raw16 = raw.reshape(height, stride)[:, : width * 2].view(np.uint16).reshape(height, width)
    return raw16, fourcc


def process_frame(raw16, bayer_code, tuning, enhance=True):
    # Orin's VI expands each true 10-bit sample p into the 16-bit container
    # as (p << 6) | (p >> 4) - bit replication so 10-bit max maps to 16-bit
    # max, NOT zero-padding - confirmed by direct byte-level inspection.
    # A plain right-shift recovers the true 10-bit value.
    p10 = raw16 >> 6

    if not enhance:
        # Debayer/stride-fix isolation mode: no black level, WB, CCM, or
        # gamma - just the raw demosaiced 10-bit values, linearly scaled to
        # 8-bit for a viewable PNG. Colors will look flat/dull - that's
        # expected and not a bug, this is deliberately everything BEFORE
        # any color science.
        bgr = cv2.cvtColor(p10, bayer_code)
        rgb = bgr[:, :, ::-1]
        return (rgb.astype(np.float64) / 1023.0 * 255).clip(0, 255).astype(np.uint8)

    black_level_10bit = tuning["black_level_16bit"] / 64.0  # 16-bit -> 10-bit scale
    bay = np.clip(p10.astype(np.float64) - black_level_10bit, 0, None)
    # Rescale back up to the full 16-bit range before demosaicing - cv2's
    # edge-aware demosaic does internal edge-detection thresholding tuned
    # for the full bit-depth range, not a signal confined to the bottom
    # ~1.5% of a 16-bit container.
    bay16 = (bay * (65535.0 / (1023.0 - black_level_10bit))).astype(np.uint16)

    bgr16 = cv2.cvtColor(bay16, bayer_code)
    rgb = bgr16[:, :, ::-1].astype(np.float64) / 65535.0  # BGR -> RGB

    rgb = rgb * tuning["wb_gains"][np.newaxis, np.newaxis, :]
    rgb = np.clip(rgb, 0, None)

    ccm = tuning["ccm"]
    h, w, _ = rgb.shape
    rgb_flat = rgb.reshape(-1, 3)
    rgb_flat = rgb_flat @ ccm.T
    rgb = rgb_flat.reshape(h, w, 3)
    rgb = np.clip(rgb, 0, 1)

    gx, gy = tuning["gamma_x"], tuning["gamma_y"]
    for c in range(3):
        rgb[:, :, c] = np.interp(rgb[:, :, c], gx, gy)

    return (np.clip(rgb, 0, 1) * 255).astype(np.uint8)


def main():
    enhance = "--raw" not in sys.argv
    tuning = load_tuning(TUNING_FILE)
    if enhance:
        print(
            f"Using ccm ct={tuning['ccm_ct']}K, wb ct={tuning['wb_ct']}K, "
            f"wb_gains={tuning['wb_gains']}, black_level={tuning['black_level_16bit']}"
        )
    else:
        print("--raw: enhancement disabled, debayer + stride fix only")

    raw16, fourcc = capture_raw_frame()
    print(f"detected pixelformat {fourcc!r} -> {BAYER_CODE_BY_FOURCC[fourcc]}")
    p10 = raw16 >> 6  # true 10-bit value, see process_frame() for why
    print(
        f"raw10 stats (post >>6): min={p10.min()} max={p10.max()} mean={p10.mean():.1f} "
        f"pct>=1000={100 * (p10 >= 1000).mean():.1f}% "
        f"(saturation is 1023; no AE runs during this raw grab, so this reflects "
        f"whatever exposure/gain was already loaded on the sensor)"
    )
    rgb = process_frame(raw16, BAYER_CODE_BY_FOURCC[fourcc], tuning, enhance=enhance)

    out_path = "/tmp/imx296_processed.png"
    cv2.imwrite(out_path, cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    sys.exit(main())
