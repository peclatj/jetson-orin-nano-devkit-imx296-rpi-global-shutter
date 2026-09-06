# IMX296 Driver for Jetson Orin

A Linux V4L2 sensor driver and device-tree overlays that bring the Sony
IMX296 global-shutter sensor (the **Raspberry Pi Global Shutter Camera**) to
**NVIDIA Jetson Orin** devkits, using NVIDIA's `tegracam` framework.

The register documentation in `doc/` remains the best public reference for
this sensor. Since the initial bring-up, the driver has picked up a set of
functional fixes found via adversarial review and hardware forensics,
dual-camera and 90 fps support, an experimental external-trigger mode, and a
full custom-ISP path that bypasses Argus entirely (see below) — see
[Credits](#credits) for who did what.

> **Status: working.** Probes, streams, and records on Jetson Orin NX 16GB
> (devkit carrier, JetPack 6 / L4T r36.5.0) in daily use; Also on Orin Nano (JetPack 6.2.2).

## Features

- V4L2 subdevice driver (`nv_imx296.c`) built on NVIDIA's `tegracam_core`
  framework.
- Auto-detects color (IMX296LQ) vs monochrome (IMX296LL) variants via the
  sensor's `SENSOR_INFO` register, or can be forced via a device-tree
  `compatible` string.
- Controls: gain, exposure, frame rate, group hold, fuse/sensor ID, and an
  experimental external-trigger mode.
- Two sensor modes: 1456×1088 @ 60 fps, and a centered-ROI 1280×720 @ 90 fps.
- Device-tree overlays for CAM0 (`-A`), CAM1 (`-C`), and simultaneous
  dual-camera (`-dual`) operation on the Jetson Orin devkit carrier (P3768).
- Optional vertical-flip / horizontal-mirror device-tree properties.
- A userspace ISP pipeline script (`scripts/imx296_isp_pipeline.py`) that
  applies real Raspberry Pi/libcamera tuning data (black level, CCM, gamma)
  to the raw Bayer capture, since NVIDIA's Argus ISP has no path to load a
  third-party tuning file.
- External-trigger tooling (`scripts/configure_camera_dt.py`,
  `scripts/trigger_pwm.sh`) to fire capture off an external PWM signal — see
  `doc/external-trigger-howto.md`.

See [Changelog](#changelog) for the fixes and root causes behind these.

## Repository layout

```
source/
  nvidia-oot/drivers/media/i2c/
    nv_imx296.c              Sensor driver (tegracam)
    imx296_mode_tbls.h       Register tables: mode0 1456x1088@60, mode1 1280x720@90
    Makefile                 Adds nv_imx296.o to the i2c module list
  hardware/nvidia/t23x/nv-public/overlay/
    tegra234-p3767-camera-p3768-imx296-A.dts     CAM0 (J20) single
    tegra234-p3767-camera-p3768-imx296-C.dts     CAM1 (J21) single
    tegra234-p3767-camera-p3768-imx296-dual.dts  both connectors
    tegra234-p3767-imx296-trigger-pwm7-clk.dts   clk_m reparent overlay for exact trigger-PWM rates
    Makefile                 Adds the .dtbo targets

scripts/
  install.sh                 Target-side installer (module + dtbo + boot entry)
  configure_camera_dt.py     Configures camera-layout / external-trigger DT + PWM wiring
  trigger_pwm.sh              Drives the external-trigger PWM pulse train
  imx296_isp_pipeline.py      Offline/live raw -> color ISP pipeline (see Capturing below)

doc/
  imx296_registers.typ        Register reference (typ/pdf)
  external-trigger-howto.md   External trigger wiring, arming, and verification cautions
  external_sources/           Mirrored reference material (mainline driver, tuning data, etc.)
```

The files under `source/` mirror the L4T `Linux_for_Tegra/source` layout and
copy straight on top of it.

**Companion project:** the recommended way to *consume* this driver is
[`nvimx296camerasrc`][nvimx296camerasrc] — a GStreamer source element with a
fused CUDA ISP that uses the real RPi/libcamera tuning data and replaces
`nvarguscamerasrc`/Argus entirely: no AE hunting, no TNR, correct color,
zero-copy from sensor DMA to `memory:NVMM` NV12 at 60/90 fps. It began life
in this repo (see git history through the `feat/cuda-isp` /
`feat/zero-copy-capture` merges) and is maintained as its own CMake project.

## Build

Against an extracted JetPack 6 `Linux_for_Tegra/source` tree (versions must
match the target's L4T exactly — check `cat /etc/nv_tegra_release`):

```bash
cp -r source/* /path/to/Linux_for_Tegra/source/
cd /path/to/Linux_for_Tegra/source
export KERNEL_HEADERS=/usr/src/linux-headers-$(uname -r)-ubuntu22.04_aarch64/3rdparty/canonical/linux-jammy/kernel-source
make modules   # -> nvidia-oot/drivers/media/i2c/nv_imx296.ko
make dtbs      # -> kernel-devicetree/generic-dts/dtbs/tegra234-p3767-camera-p3768-imx296-{A,C,dual}.dtbo
```

(Native build on the Jetson works with the stock `linux-headers` package, as
above; cross-building from x86 works with the upstream `scripts/build_*.sh`
after adjusting the hardcoded paths.)

## Install on the target

1. Copy the `.dtbo`(s) to `/boot/` and the `.ko` somewhere convenient.
2. Add a **non-default** extlinux label (keep your known-good default
   bootable — a bad camera boot then costs one power-cycle, nothing more):

```
LABEL imx296
    MENU LABEL IMX296 GS camera
    LINUX /boot/Image
    FDT /boot/dtb/<your-base-dtb>.dtb
    INITRD /boot/initrd
    APPEND <copy the APPEND line of your working label>
    OVERLAYS /boot/tegra234-p3767-camera-p3768-imx296-A.dtbo
```

Use `...-C.dtbo` for CAM1 or `...-dual.dtbo` for both. Do **not** co-apply
the two single overlays (they share `video0`).

3. Reboot, pick the label at the boot menu, and load the driver manually:

```bash
sudo insmod nv_imx296.ko     # keep it manual until proven on your setup;
                             # auto-loading an unproven camera driver in
                             # modules-load.d is how boards get bricked
sudo dmesg | grep imx296
```

Expected probe:

```
imx296 9-001a: probing IMX296 sensor
imx296 9-001a: IMX296LQ (color) detected (sensor_info=0x4a00)
tegra-camrtc-capture-vi tegra-capture-vi: subdev imx296 9-001a bound
imx296 9-001a: IMX296LQ sensor detected and registered
```

## Capturing

`scripts/imx296_isp_pipeline.py` needs `python3-opencv` (OpenCV for Python)
and `numpy` installed.

**Raw V4L2** (both modes; Orin's VI needs a 64-byte-aligned stride —
`preferred_stride` below):

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
         -c preferred_stride=2944
v4l2-ctl -d /dev/video0 -c exposure=8333,gain=100    # us; dB*10
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 --stream-skip=4 \
         --stream-to=frame.raw
python3 scripts/imx296_isp_pipeline.py --input frame.raw   # -> color PNG (auto AWB)
```

**Best quality / live** — use the companion
[`nvimx296camerasrc`][nvimx296camerasrc] element (see above).

**Via Argus** (`nvarguscamerasrc`) — works, but Argus has no tuning profile
for this sensor: colors are approximate and the untuned auto-exposure loop
*hunts* (~25% brightness oscillation measured). If you must use it, pin
everything:

```bash
gst-launch-1.0 nvarguscamerasrc sensor-mode=0 tnr-mode=0 ee-mode=0 \
    aelock=true awblock=true aeantibanding=0 \
    exposuretimerange="8333000 8333000" gainrange="60 60" \
    ispdigitalgainrange="1 1" \
  ! 'video/x-raw(memory:NVMM),width=1456,height=1088,framerate=60/1' ! nv3dsink
```

(8.333 ms is the only mains-flicker-immune exposure at any frame rate; gain
is dB×10, 0–480, halve exposure ⇒ +60 gain.)

## Known issues / limitations

- **Argus colors remain untuned** (NVIDIA provides no path to load a
  third-party tuning file) — by design unfixable in Argus; solved properly
  by the companion CUDA ISP element instead.
- **CFA phase mystery**: raw-domain site statistics look G-first while the
  `RG10` fourcc claims RGGB; the demosaic mapping used everywhere here is
  empirically color-correct, but the discrepancy is unexplained. Affects
  only code doing mosaic-domain math.
- Do not `rmmod` the driver while an Argus client is running/tearing down —
  NVIDIA's camera stack races a use-after-free (observed kernel panic).
  Stop `nvargus-daemon` first.
- Monochrome variant (IMX296LL) is detected but has never been
  hardware-tested; no EEPROM/OTP/HDR support.
- Dual-overlay operation is code-complete and boots, but simultaneous
  two-camera streaming awaits second-camera hardware validation.
- With `BLKLEVELAUTO` disabled (flicker fix), slow thermal black-level drift
  is uncompensated — if you see shadow lift in long sessions, use the ISP
  element's `black-offset` property.
- **External trigger mode is new and lightly tested** — read
  `doc/external-trigger-howto.md`'s wiring and arming cautions before relying
  on it.

## Changelog

Notable fixes and additions since the initial bring-up. Each landed as its
own `--no-ff` merge, one branch per fix — `git log` has the full history and
rationale, and any of them can be reverted as a single merge commit.

| Change | Details |
|---|---|
| Fixed exposure control | The sensor's 14 µs readout offset was being scaled by `exposure_factor` *before* subtraction instead of after — the math wrapped and pinned SHS1 at max, so the V4L2/Argus exposure control silently did nothing. Now correct and verified monotonic on hardware. |
| Fixed black horizontal line | Two register deltas vs the RPi *production* driver: init byte `0x30af = 0x0b` (RPi's Fast-Trigger/MIPI-FE fix, on Sony's advice) and the missing `MIPIC_AREA3W (0x4182) = height` write. Verified gone on real captures. |
| Fixed shadow/line flicker | The sensor's automatic black-level servo (`BLKLEVELAUTO`) oscillates ±1.5 counts @ 7–9 Hz (±35 @ 30 dB gain) — proven with capped-lens dark frames and a mid-stream register A/B. Disabled in favor of the fixed level the tuning data assumes (trade-off: no thermal-drift compensation; a manual trim exists in the companion ISP element). |
| Gain now latches at frame boundary | `GAINDLY` set to 1-frame mode (the RPi production value) so gain+exposure steps land atomically on one frame. |
| Added 1280×720 @ 90 fps mode (mode1) | Centered ROI crop, VMAX 750 → exactly 90.0 fps. Required making the driver's VMAX floor mode-relative — without that, the frame-rate control silently clamped the new mode back to ~60 fps. |
| Added dual-camera overlay | `tegra234-p3767-camera-p3768-imx296-dual.dtbo` (both CSI connectors at once, `video0` + `video1`), modeled on NVIDIA's imx477-dual overlay. |
| Removed overlay defects | PWDN gpio-hogs had targeted a Tegra210 address that doesn't exist on Orin (silently inert — and dangerous to "fix" in place); also removed phantom disable nodes and a `channel@1`/`reg=<0>` contradiction in the -C overlay. |
| `#define DEBUG` off by default | Previously shipped with full register dumps plus a 1 s sleep inside every stream start. |
| Rewrote ISP script (`scripts/imx296_isp_pipeline.py`) | Previous version didn't run (wrong tuning path). Now: offline/live raw → color pipeline with curve-constrained auto-AWB, argparse CLI, both sensor modes. |
| Added external trigger mode (experimental) | New `trigger_mode` V4L2 control, plus `scripts/configure_camera_dt.py` (DT/PWM wiring) and `scripts/trigger_pwm.sh` (drives the trigger pulse train). Preliminary work — wiring, arming, and verification are documented in `doc/external-trigger-howto.md`, but this path is less hardware-proven than the fixes above; read the cautions there before wiring anything up. |
| Added register provenance comments | Every deviating byte cites its source (mainline vs RPi tree vs measured) directly in the code. |

## Credits

- **Jonathan Péclat** — original bring-up, driver, overlays, and register
  documentation this driver stands on.
- **William Reed Seal-Foss** — the fixes, dual-camera/90 fps support,
  external-trigger mode, and ISP pipeline rewrite described above.
- Mainline `drivers/media/i2c/imx296.c` (Laurent Pinchart) and the Raspberry
  Pi kernel/libcamera projects — register sequences, tuning data, and the
  production reference against which the fixes here were verified.
- NVIDIA's `nv_imx185`/`gst-nvv4l2camera` sources — tegracam and NVMM
  conventions.

## License

GPL-2.0, see [LICENSE](LICENSE).

<!-- Companion-repo link: update this ONE definition when the
     nvimx296camerasrc repository is published. -->
[nvimx296camerasrc]: https://github.com/sealfoss/GStreamer-NV-IMX296-Camera-Source
