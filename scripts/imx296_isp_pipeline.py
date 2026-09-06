#!/usr/bin/env python3
"""
Minimal IMX296 ISP pipeline: raw Bayer capture -> black level -> AWB ->
debayer -> CCM -> gamma, using the real per-sensor calibration from
imx296_16mm.json (libcamera/Raspberry Pi tuning data), since NVIDIA's
Argus ISP has no way to load it.

This bypasses nvarguscamerasrc/Argus entirely and works directly on the raw
V4L2 capture, which is where the tuning data's CCM/gamma are meant to apply
(linear RGB, post-WB, pre-gamma) - not on Argus's already-processed output.

Usage:
  imx296_isp_pipeline.py                        capture + process (auto AWB)
  imx296_isp_pipeline.py --input frame.raw      process an existing capture
  imx296_isp_pipeline.py --awb tuning --ct 4560 the original fixed-CT behavior
  imx296_isp_pipeline.py --raw                  debayer only, no color science
"""

import argparse
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
# (confirmed by direct byte-level inspection). The fix is to force a
# 64-byte-aligned stride explicitly via the "preferred_stride" V4L2 control
# (in bytes, not pixels) and then strip the resulting per-line padding on
# read. The stride is derived as the smallest 64-byte-aligned value at or
# above WIDTH*2 (2912 -> 2944 for mode0; mode1's 1280*2=2560 is already
# aligned).
WIDTH, HEIGHT = 1456, 1088

# The tuning JSON ships in doc/external_sources/ in this repo; also accept a
# copy sitting next to the script (older layout) or an explicit --tuning.
TUNING_CANDIDATES = [
    Path(__file__).resolve().parent.parent / "doc" / "external_sources" / "imx296_16mm.json",
    Path(__file__).resolve().parent / "imx296_16mm.json",
]

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

# NOTE on mosaic-domain statistics: do NOT trust the fourcc to locate CFA
# sites. Measured on real captures (frame site means, 150k samples/site):
# the (0,0)/(1,1) diagonal agrees within 1% (the same-filter signature of
# the two G sites) while (0,1)/(1,0) differ by 20% - i.e. the data is
# G-first (GBRG/GRBG-like), NOT the RGGB the 'RG10' fourcc advertises.
# Rather than adding a second empirically-derived mapping that could drift
# from the demosaic one above, grey-world AWB below works on the DEMOSAICED
# linear image, inheriting whatever the validated demosaic mapping says.


def aligned_stride(width):
    return ((width * 2) + 63) // 64 * 64


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

    ct_curve = find("rpi.awb")["ct_curve"]
    triples = [ct_curve[i : i + 3] for i in range(0, len(ct_curve), 3)]

    gamma_curve = np.array(find("rpi.contrast")["gamma_curve"], dtype=np.float64)
    gamma_x = gamma_curve[0::2] / 65535.0
    gamma_y = gamma_curve[1::2] / 65535.0

    return {
        "black_level_16bit": black_level,
        "ccms": ccms,           # full per-CT table; selected later
        "ct_curve": triples,    # (ct, r_ratio, b_ratio) triples
        "gamma_x": gamma_x,
        "gamma_y": gamma_y,
    }


def select_ccm(tuning, ct):
    entry = min(tuning["ccms"], key=lambda c: abs(c["ct"] - ct))
    return np.array(entry["ccm"], dtype=np.float64).reshape(3, 3), entry["ct"]


def wb_from_tuning(tuning, target_ct):
    """The original fixed-illuminant behavior: gains from the ct_curve entry
    nearest target_ct."""
    ct, r_ratio, b_ratio = min(
        tuning["ct_curve"], key=lambda t: abs(t[0] - target_ct)
    )
    return np.array([1.0 / r_ratio, 1.0, 1.0 / b_ratio]), ct


def wb_grey_world(rgb_linear, tuning):
    """Grey-world AWB on the demosaiced linear image: equalize the R and B
    channel means to the G mean, then estimate the scene CT as the ct_curve
    entry whose (r_ratio, b_ratio) is nearest the measured sensor ratios -
    that CT then also selects the matching CCM, keeping WB and CCM
    consistent with the same estimated illuminant.

    Robustness: means are computed over the central 2/3 of the frame with
    near-black and near-clipped pixels excluded, so a dark surround or a
    blown highlight doesn't drag the estimate.
    """
    h, w, _ = rgb_linear.shape
    cy, cx = h // 6, w // 6  # central crop bounds (keep 2/3)
    win = rgb_linear[cy : h - cy, cx : w - cx]
    lum = win.mean(axis=2)
    good = (lum > 0.016) & (lum < 0.95)  # linear domain, ~10-bit 16..972
    if good.sum() < 1000:
        good = np.ones_like(lum, dtype=bool)

    r_m, g_m, b_m = (float(win[..., c][good].mean()) for c in range(3))
    if min(r_m, g_m, b_m) <= 0:
        raise RuntimeError("grey-world stats degenerate (all-dark frame?)")

    r_ratio, b_ratio = r_m / g_m, b_m / g_m

    # Constrain to the calibrated illuminant curve: real light sources are
    # never green/magenta, but scenes often are (a frame full of foliage
    # makes raw grey-world over-correct to magenta - observed on real
    # captures). Project the measured (r, b) onto the nearest ct_curve
    # entry and take the WHITE BALANCE GAINS FROM THE CURVE, so grey-world
    # only chooses the point ALONG the calibrated locus (libcamera's
    # bayes-AWB constrains the same way).
    ct, r_c, b_c = min(
        tuning["ct_curve"],
        key=lambda t: (t[1] - r_ratio) ** 2 + (t[2] - b_ratio) ** 2,
    )
    gains = np.array([1.0 / r_c, 1.0, 1.0 / b_c])
    return gains, ct, (r_ratio, b_ratio)


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


def capture_raw_frame(device, width, height, stride, skip, raw_path="/tmp/imx296_frame.raw"):
    fourcc = detect_pixelformat(device, width, height)
    if fourcc not in BAYER_CODE_BY_FOURCC:
        raise RuntimeError(
            f"detected pixelformat {fourcc!r} on {device} has no known Bayer "
            f"mapping - add it to BAYER_CODE_BY_FOURCC"
        )

    subprocess.run(
        [
            "v4l2-ctl",
            f"--device={device}",
            f"--set-fmt-video=width={width},height={height},pixelformat={fourcc}",
            "-c",
            f"preferred_stride={stride}",
            "--stream-mmap",
            "--stream-count=1",
            # Skip the first frames: the frame emitted right after stream-on
            # still carries the mode table's default exposure/gain (control
            # values latch a frame or two later), and on this sensor gain
            # additionally latches one frame after the write (GAINDLY_1FRAME).
            f"--stream-skip={skip}",
            f"--stream-to={raw_path}",
        ],
        check=True,
    )
    return load_raw(raw_path, width, height, stride), fourcc


def load_raw(raw_path, width, height, stride):
    raw = np.fromfile(raw_path, dtype=np.uint8)
    expected = stride * height
    if raw.size != expected:
        raise RuntimeError(
            f"{raw_path}: expected {expected} bytes ({height} rows x {stride}-byte "
            f"stride), got {raw.size} - wrong --width/--height, or preferred_stride "
            f"did not take effect (Bytes per Line should be {stride})"
        )
    # Reinterpret as uint16 FIRST (the flat buffer is contiguous - always
    # legal), THEN slice off the per-line stride padding. Slicing bytes
    # before .view() leaves a non-contiguous array, which older numpy
    # releases (e.g. the Orin's) reject with "must be C-contiguous".
    return raw.view(np.uint16).reshape(height, stride // 2)[:, :width]


def process_frame(raw16, fourcc, tuning, awb="auto", target_ct=4560, enhance=True):
    bayer_code = BAYER_CODE_BY_FOURCC[fourcc]

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
        return (rgb.astype(np.float64) / 1023.0 * 255).clip(0, 255).astype(np.uint8), None

    black_level_10bit = tuning["black_level_16bit"] / 64.0  # 16-bit -> 10-bit scale

    bay = np.clip(p10.astype(np.float64) - black_level_10bit, 0, None)
    # Rescale back up to the full 16-bit range before demosaicing - cv2's
    # edge-aware demosaic does internal edge-detection thresholding tuned
    # for the full bit-depth range, not a signal confined to the bottom
    # ~1.5% of a 16-bit container.
    bay16 = (bay * (65535.0 / (1023.0 - black_level_10bit))).astype(np.uint16)

    bgr16 = cv2.cvtColor(bay16, bayer_code)
    rgb = bgr16[:, :, ::-1].astype(np.float64) / 65535.0  # BGR -> RGB

    if awb == "auto":
        wb_gains, ct, ratios = wb_grey_world(rgb, tuning)
        wb_desc = (
            f"grey-world gains={np.round(wb_gains, 3)} measured r/g={ratios[0]:.3f} "
            f"b/g={ratios[1]:.3f} -> estimated ct={ct}K"
        )
    else:
        wb_gains, ct = wb_from_tuning(tuning, target_ct)
        wb_desc = f"tuning ct_curve @ {ct}K gains={np.round(wb_gains, 3)}"

    ccm, ccm_ct = select_ccm(tuning, ct)

    rgb = rgb * wb_gains[np.newaxis, np.newaxis, :]
    rgb = np.clip(rgb, 0, None)

    h, w, _ = rgb.shape
    rgb = (rgb.reshape(-1, 3) @ ccm.T).reshape(h, w, 3)
    rgb = np.clip(rgb, 0, 1)

    gx, gy = tuning["gamma_x"], tuning["gamma_y"]
    for c in range(3):
        rgb[:, :, c] = np.interp(rgb[:, :, c], gx, gy)

    return (np.clip(rgb, 0, 1) * 255).astype(np.uint8), f"{wb_desc}; ccm @ {ccm_ct}K"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", help="process an existing raw capture instead of grabbing one")
    ap.add_argument("-o", "--output", default="/tmp/imx296_processed.png")
    ap.add_argument("--device", default=VIDEO_DEVICE)
    ap.add_argument("--width", type=int, default=WIDTH)
    ap.add_argument("--height", type=int, default=HEIGHT)
    ap.add_argument("--skip", type=int, default=4, help="frames to skip after stream-on (default 4)")
    ap.add_argument("--tuning", help="path to imx296_16mm.json (default: auto-locate in repo)")
    ap.add_argument("--awb", choices=["auto", "tuning"], default="auto",
                    help="auto = grey-world + CT estimate (default); tuning = fixed --ct entry")
    ap.add_argument("--ct", type=int, default=4560,
                    help="illuminant CT for --awb tuning (default 4560, the original behavior)")
    ap.add_argument("--fourcc", default=None,
                    help="override fourcc for --input files (default: query the device)")
    ap.add_argument("--raw", action="store_true", help="debayer + stride fix only, no color science")
    args = ap.parse_args()

    if args.tuning:
        tuning_path = Path(args.tuning)
    else:
        tuning_path = next((p for p in TUNING_CANDIDATES if p.exists()), None)
        if tuning_path is None:
            sys.exit(f"tuning file not found in: {[str(p) for p in TUNING_CANDIDATES]} - pass --tuning")
    tuning = load_tuning(tuning_path)

    stride = aligned_stride(args.width)
    if args.input:
        fourcc = args.fourcc or detect_pixelformat(args.device, args.width, args.height)
        raw16 = load_raw(args.input, args.width, args.height, stride)
    else:
        raw16, fourcc = capture_raw_frame(args.device, args.width, args.height, stride, args.skip)

    p10 = raw16 >> 6  # true 10-bit value, see process_frame() for why
    print(
        f"raw10 stats (post >>6): min={p10.min()} max={p10.max()} mean={p10.mean():.1f} "
        f"pct>=1000={100 * (p10 >= 1000).mean():.1f}% (saturation is 1023)"
    )
    rgb, desc = process_frame(
        raw16, fourcc, tuning, awb=args.awb, target_ct=args.ct, enhance=not args.raw
    )
    if desc:
        print(f"color: {desc}")
    else:
        print("--raw: enhancement disabled, debayer + stride fix only")

    cv2.imwrite(args.output, cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
