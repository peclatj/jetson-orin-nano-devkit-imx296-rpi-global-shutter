#!/usr/bin/env python3
"""
IMX296 raw frame decoder - 1456x1088 mode (ROI window, 64-byte aligned stride)

Usage:
  ./raw_2_png.py <raw_file>

Capture with:
  v4l2-ctl -d /dev/video0 \
    --set-fmt-video=width=1440,height=1088,pixelformat=RG10 \
    --stream-mmap --stream-count=1 --stream-to=frame1440.raw

Verify with --get-fmt-video: expect Bytes per Line = 2944, Size Image = 3133440.
"""
import os
import sys
import numpy as np
import cv2

if len(sys.argv) < 2:
    sys.exit(f"usage: {sys.argv[0]} <raw_file>")

FNAME = sys.argv[1]
STEM = os.path.splitext(FNAME)[0]

W, H = 1456, 1088
STRIDE = 2944            # 1456 * 2 bytes; = 45 * 64 -> aligned, no more odd-line shift
BLACK_LEVEL = 60.0       # 10-bit black pedestal (BLKLEVEL)

# ---- load ------------------------------------------------------------
raw = np.fromfile(FNAME, dtype=np.uint8)
expected = STRIDE * H
if raw.size != expected:
    print(f"WARNING: file is {raw.size} bytes, expected {expected}."
          f" Check --get-fmt-video (stride/height mismatch?)")
img16 = raw[:expected].reshape(H, STRIDE)[:, :W*2].view(np.uint16).reshape(H, W)

# Orin VI stores RAW10 expanded to 16 bit: stored = (p<<6)|(p>>4).
# Shift ONCE to recover true 10-bit samples.
p10 = (img16 >> 6).astype(np.uint16)
print("p10 min/max/mean:", p10.min(), p10.max(), round(float(p10.mean()), 1))

# ---- sanity diagnostics ---------------------------------------------
cm = p10.mean(axis=0)
print("first 8 col means:", cm[:8].astype(int))
print("last  8 col means:", cm[-8:].astype(int))   # should now match neighbors

rm = p10.mean(axis=1)
r = int(rm.argmin())
print(f"darkest row: {r}  mean={rm[r]:.1f}  above={rm[r-1]:.1f} below={rm[r+1]:.1f}")
print("row == row above?", np.array_equal(p10[r], p10[r-1]))
zr = np.unique(np.where(p10 == 0)[0])
print("rows containing zero-valued pixels:", zr[:10] if zr.size else "none")

# ---- per-plane extraction (RGGB, confirmed phase) --------------------
planes = {'R': p10[0::2, 0::2], 'G1': p10[0::2, 1::2],
          'G2': p10[1::2, 0::2], 'B': p10[1::2, 1::2]}
plane_files = []
for name, p in planes.items():
    v = np.clip(p.astype(np.float32) - BLACK_LEVEL, 0, None)
    v = (v / max(v.max(), 1) * 255).astype(np.uint8)
    fname = f'{STEM}_plane_{name}.png'
    cv2.imwrite(fname, v)
    plane_files.append(fname)

# ---- full pipeline: black -> demosaic(16b, EA) -> WB -> gamma --------
bay = np.clip(p10.astype(np.float32) - BLACK_LEVEL, 0, None)
bay16 = (bay * (65535.0 / (1023.0 - BLACK_LEVEL))).astype(np.uint16)

# V4L2 "RGGB" == OpenCV COLOR_BayerBG (naming offset!)
rgb = cv2.cvtColor(bay16, cv2.COLOR_BayerBG2BGR_EA).astype(np.float32)

rgb *= rgb.mean() / (rgb.mean(axis=(0, 1)) + 1e-6)      # gray-world WB
rgb = np.clip(rgb / 65535.0, 0, 1) ** (1 / 2.2)         # display gamma
rgb_file = f'{STEM}_rgb.png'
cv2.imwrite(rgb_file, (rgb * 255).astype(np.uint8))

# 8-bit grayscale preview straight from the 16-bit container
gray_file = f'{STEM}_gray.png'
cv2.imwrite(gray_file, (img16 >> 8).astype(np.uint8))

print(f"wrote: {rgb_file}, {gray_file}, {', '.join(plane_files)}")
