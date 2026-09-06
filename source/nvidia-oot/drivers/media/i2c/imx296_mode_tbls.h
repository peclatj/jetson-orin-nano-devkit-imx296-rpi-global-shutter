// SPDX-License-Identifier: GPL-2.0
/*
 * imx296_mode_tbls.h - IMX296 sensor mode tables
 *
 * Author: Jonathan Peclat <peclatj@bluewin.ch>
 * Date: 2026-03-22
 *
 * Register values derived from:
 *   - drivers/media/i2c/imx296.c by Laurent Pinchart
 *     Copyright 2019 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *   - libcamera IMX296 camera helper
 *     Copyright (C) 2020, Raspberry Pi Ltd
 */

#ifndef __IMX296_MODE_TBLS_H__
#define __IMX296_MODE_TBLS_H__

#include <media/camera_common.h>

/* ---- Sentinel values for register tables ---- */
#define IMX296_TABLE_WAIT_MS 0xFFFF /* special addr: val = delay in ms */
#define IMX296_TABLE_END 0xFFFE /* marks end of register table     */

/* Reuse kernel reg_8 struct: { u16 addr; u8 val; } */
typedef struct reg_8 imx296_reg;

/*
 * Mode indices - must match mode_table[] array order below.
 *
 * IMPORTANT: set_mode() indexes mode_table[] directly with the DT
 * mode<N> index (s_data->mode_prop_idx), so all real sensor modes MUST
 * come first and in DT order; the pseudo-mode entries (start/stop/test)
 * follow and are only ever referenced by name.
 *
 * mode0: 1456x1088 full array @ up to 60 fps
 * mode1: 1280x720 centered ROI crop @ up to 90 fps
 * Timing (VMAX/SHS1/GAIN) is set in common_regs and updated dynamically
 * by control handlers; the per-mode tables handle windowing/ROI.
 */
enum {
	IMX296_MODE_1456X1088 = 0,
	IMX296_MODE_1280X720,
	IMX296_MODE_START_STREAM,
	IMX296_MODE_STOP_STREAM,
	IMX296_MODE_TEST_PATTERN,
};

/*
 * Common initialization registers.
 *
 * WARNING: This table is extracted from vendor data that is entirely
 * undocumented. Source: mainline Linux driver imx296.c by Laurent Pinchart.
 * The first register write (0x3005) is required to activate CSI-2 output.
 * The other entries may or may not be optional.
 * Do NOT modify without Sony guidance.
 *
 * Clock configuration registers (0x3089-0x308c = INCKSEL0-3) ARE part of
 * this table (54 MHz set, matching the RPi GS camera's on-board
 * oscillator), together with the clock-dependent CTRL418C. Alternative
 * sets, should a different INCK ever be used:
 * For 37.125MHz: { 0x80, 0x0b, 0x80, 0x08 }, CTRL418C=0x74
 * For 54MHz:     { 0xb0, 0x0f, 0xb0, 0x0c }, CTRL418C=0xa8
 * For 74.25MHz:  { 0x80, 0x0f, 0x80, 0x0c }, CTRL418C=0xe8
 */
static const imx296_reg imx296_common_regs[] = {
	/* ---- Required for CSI-2 output activation ---- */
	{ 0x3005, 0xf0 }, /* required - activates CSI-2 output (undocumented) */

	/* ---- Undocumented vendor initialization sequence ----
     * Source: mainline imx296_init_table[]
     * These values are from Sony reference design.
     */
	{ 0x309e, 0x04 }, /* undocumented */
	{ 0x30a0, 0x04 }, /* undocumented */
	{ 0x30a1, 0x3c }, /* undocumented */
	{ 0x30a4, 0x5f }, /* undocumented */
	{ 0x30a8, 0x91 }, /* undocumented */
	{ 0x30ac, 0x28 }, /* undocumented */
	/*
     * 0x30af: the ONLY byte on which the two reference drivers disagree.
     * Mainline imx296.c ships 0x09, and the Raspberry Pi kernel driver
     * originally shipped 0x09 too (driver-add commit cb33db2b6ccf). RPi
     * changed it to 0x0b in commit dca33feaec31 (2024-03-04, "Updated
     * register setting to fix Fast Trigger"): per that commit, on Sony's
     * recommendation, 0x0b fixes a MIPI Frame End packet not being sent
     * at end of frame. The current RPi production driver applies 0x0b
     * unconditionally in all modes, and a missing/late FE packet is a
     * plausible contributor to per-frame VI capture artifacts (the black
     * line seen on every capture with 0x09), so adopt 0x0b — but note
     * the artifact link is a hypothesis to verify on hardware, not an
     * established cause.
     */
	{ 0x30af, 0x0b }, /* undocumented (RPi production value; mainline: 0x09) */
	{ 0x30df, 0x00 }, /* undocumented */
	{ 0x3165, 0x00 }, /* undocumented */
	{ 0x3169, 0x10 }, /* undocumented */
	{ 0x316a, 0x02 }, /* undocumented */
	{ 0x31c8, 0xf3 }, /* exposure-related (undocumented) */
	{ 0x31d0, 0xf4 }, /* exposure-related (undocumented) */
	{ 0x321a, 0x00 }, /* undocumented */
	{ 0x3226, 0x02 }, /* undocumented */
	{ 0x3256, 0x01 }, /* undocumented */
	{ 0x3541, 0x72 }, /* undocumented */
	{ 0x3516, 0x77 }, /* undocumented */
	{ 0x350b, 0x7f }, /* undocumented */
	{ 0x3758, 0xa3 }, /* undocumented */
	{ 0x3759, 0x00 }, /* undocumented */
	{ 0x375a, 0x85 }, /* undocumented */
	{ 0x375b, 0x00 }, /* undocumented */
	{ 0x3832, 0xf5 }, /* undocumented */
	{ 0x3833, 0x00 }, /* undocumented */
	{ 0x38a2, 0xf6 }, /* undocumented */
	{ 0x38a3, 0x00 }, /* undocumented */
	{ 0x3a00, 0x80 }, /* undocumented */
	{ 0x3d48, 0xa3 }, /* undocumented */
	{ 0x3d49, 0x00 }, /* undocumented */
	{ 0x3d4a, 0x85 }, /* undocumented */
	{ 0x3d4b, 0x00 }, /* undocumented */
	{ 0x400e, 0x58 }, /* undocumented */
	{ 0x4014, 0x1c }, /* undocumented */
	{ 0x4041, 0x2a }, /* undocumented */
	{ 0x40a2, 0x06 }, /* undocumented */
	{ 0x40c1, 0xf6 }, /* undocumented */
	{ 0x40c7, 0x0f }, /* undocumented */
	{ 0x40c8, 0x00 }, /* undocumented */
	{ 0x4174, 0x00 }, /* undocumented */

	/* ---- Known configuration registers ---- */

	/*
     * SYNCSEL (0x3036): XVS/XHS sync output control
     * 0xc0 = SYNCSEL_NORMAL: XVS and XHS output enabled (master mode)
     * 0xf0 = SYNCSEL_HIZ:    XVS and XHS high impedance (slave mode)
     * Using normal/master mode - sensor generates its own timing.
     * The exposed XVS/XHS pins on the camera board can be used for
     * multi-camera synchronization in the future.
     */
	{ 0x3036, 0xc0 }, /* SYNCSEL: master mode, XVS/XHS output enabled */

	/*
     * BLKLEVEL (0x3254-0x3255): black level target
     * 16-bit little-endian, default = 0x03c = 60 counts
     */
	{ 0x3254, 0x3c }, /* BLKLEVEL[7:0]  = 60 */
	{ 0x3255, 0x00 }, /* BLKLEVEL[15:8] = 0  */

	/*
     * BLKLEVELAUTO (0x3022): automatic black level calibration
     * 0x01 = ON: sensor continuously calibrates black level
     * 0xf0 = OFF: use fixed BLKLEVEL value
     *
     * OFF, deviating from mainline/RPi (which run it ON outside test
     * patterns). Measured on hardware (dark frames, 2026-07-28): the
     * auto-calibration loop OSCILLATES - +/-1.5 counts at ~7-9 Hz at
     * low gain, exploding to +/-35 counts at 38-44 Hz at gain 30 dB -
     * visible as shadow/line flicker. A mid-stream register A/B cut
     * black-level std 3.6x the moment it was disabled. The fixed
     * BLKLEVEL=60 above matches what the RPi tuning data assumes, and
     * both Argus and our ISP pipeline subtract a fixed 60. Tradeoff:
     * no compensation for slow thermal dark-current drift (DC drift,
     * not flicker) - revisit if long-session black drift appears.
     */
	{ 0x3022, 0xf0 }, /* BLKLEVELAUTO: OFF - fixed black level (see above) */

	/*
     * GAINDLY (0x3212): gain application delay
     * 0x08 = GAINDLY_NONE:   gain applied immediately (mid-frame)
     * 0x09 = GAINDLY_1FRAME: gain latched at the next frame boundary
     *
     * Use 1-frame mode, matching the RPi production driver's setup path
     * (imx296_setup writes GAINDLY_1FRAME). SHS1/exposure changes latch
     * at frame boundaries anyway, so with 1-frame mode an AE step's gain
     * and exposure land on the SAME frame instead of the gain jumping
     * mid-frame - each Argus AE adjustment produces one clean step, not
     * a partially-applied frame. (libcamera models this as
     * sensorDelays.gainDelay = 2: 1 frame register delay + 1 frame
     * pipeline.)
     */
	{ 0x3212, 0x09 }, /* GAINDLY: 1-frame (RPi production value) */

	/*
     * GAINCTRL (0x3200): gain control mode
     * 0x01 = WD_GAIN_MODE_NORMAL: standard single exposure gain
     * 0x41 = WD_GAIN_MODE_MULTI:  multi-exposure WDR gain
     * Using normal mode - no HDR.
     */
	{ 0x3200, 0x01 }, /* GAINCTRL: normal gain mode */

	/*
     * GTTABLENUM (0x4114): gamma table selection
     * 0xc5 = value from mainline driver
     */
	{ 0x4114, 0xc5 }, /* GTTABLENUM: gamma table */

	/*
     * MIPIC_AREA3W (0x4182-0x4183): MIPI output area height, 16-bit LE.
     * The Raspberry Pi kernel driver writes this even in the un-cropped
     * full-frame path (= IMX296_PIXEL_ARRAY_HEIGHT, 1088); mainline never
     * touches it, leaving the power-on default. A MIPI output-area register
     * at a stale default is the second prime suspect for the one-bad-row
     * black-line artifact. 1088 = 0x0440. Cropped modes must override this
     * with their own output height (RPi writes crop->height there).
     */
	{ 0x4182, 0x40 }, /* MIPIC_AREA3W[7:0]  = 0x40 */
	{ 0x4183, 0x04 }, /* MIPIC_AREA3W[15:8] = 0x04 -> 1088 */

	/*
     * CTRL418C: clock-dependent register
     * 37.125MHz: 116 = 0x74
     * 54MHz:     168 = 0xa8
     * 74.25MHz:  232 = 0xe8
     */
	/* CTRL418C for 54 MHz: 168 */
	{ 0x418c, 0xa8 },

	/* INCKSEL0-3 for INCK = 54 MHz (RPi GS camera onboard oscillator) */
	{ 0x3089, 0xb0 },
	{ 0x308a, 0x0f },
	{ 0x308b, 0xb0 },
	{ 0x308c, 0x0c },

	/*
     * HMAX (0x3014-0x3015): horizontal timing in 74.25MHz clock units
     * Fixed at 1100 as per mainline driver.
     * line_period = 1100 / 74.25MHz = 14.81us
     * Confirmed by libcamera: timePerLine = 550 / 37.125MHz = 14.81us
     * 0x044c = 1100
     */
	{ 0x3014, 0x4c }, /* HMAX[7:0]  = 0x4c */
	{ 0x3015, 0x04 }, /* HMAX[15:8] = 0x04 -> 0x044c = 1100 */

	/*
     * VMAX (0x3010-0x3012): vertical timing (frame length in lines)
     * Default = active_h + vblank = 1088 + 30 = 1118 lines
     * frame_time = 1118 * 14.81us = 16.56ms -> 60.4fps
     * Will be overwritten by set_frame_rate() control handler.
     * 0x045e = 1118
     */
	{ 0x3010, 0x5e }, /* VMAX[7:0]  = 0x5e */
	{ 0x3011, 0x04 }, /* VMAX[15:8] = 0x04 -> 0x045e = 1118 */
	{ 0x3012, 0x00 }, /* VMAX[22:16]= 0x00 */

	/*
     * SHS1 (0x308d-0x308f): shutter speed register (exposure)
     * exposure_lines = VMAX - SHS1
     * SHS1 = VMAX - coarse_time
     * Default: coarse_time = 1118 - 4 = 1114 lines (near max exposure)
     * SHS1 = 1118 - 1114 = 4
     * exposure_us = 1114 * 14.81us + 14.26us = 16,509us ~ 16.5ms
     * Will be overwritten by set_exposure() control handler.
     */
	{ 0x308d, 0x04 }, /* SHS1[7:0]  = 4 */
	{ 0x308e, 0x00 }, /* SHS1[15:8] = 0 */
	{ 0x308f, 0x00 }, /* SHS1[22:16]= 0 */

	/*
     * GAIN (0x3204-0x3205): analog gain in dB*10
     * Default = 0 (0dB = 1x linear gain)
     * The sensor converts dB to linear gain internally in hardware:
     * linear_gain = 10^(register / 200)
     * Will be overwritten by set_gain() control handler.
     */
	{ 0x3204, 0x00 }, /* GAIN[7:0]  = 0 (0dB = 1x) */
	{ 0x3205, 0x00 }, /* GAIN[15:8] = 0             */

	/*
     * Enter standby after init.
     * Streaming is started separately by start_streaming()
     * via CTRL00=0 (exit standby) + CTRL0A=0 (start transmission).
     */
	// Not in the original table, but we want to start in standby mode for safety.
	// TODO: Let's try without.
	// {0x3000, 0x01},  /* CTRL00: STANDBY bit = 1 */

	{ IMX296_TABLE_END, 0x00 },
};

/*
 * 1456x1088 full resolution mode
 *
 * IMX296 has only one native resolution - the full pixel array.
 * Timing (VMAX/HMAX/SHS1/GAIN) is set in common_regs and updated
 * dynamically by control handlers.
 *
 * This table only handles windowing/ROI configuration.
 * Full array = no ROI needed.
 */
static const imx296_reg imx296_1456x1088_regs[] = {
	/*
     * CTRL0D (0x300d): window mode and binning control
     * bits[1:0] = WINMODE:
     *   00 = WINMODE_ALL: full array readout (no ROI)
     *   10 = WINMODE_FD_BINNING: full array with 2x2 binning
     * bit[5] = HADD_ON_BINNING: horizontal add-binning enable
     * bit[6] = SAT_CNT: saturation count enable
     * Using full array, no binning.
     */
	{ 0x300d, 0x00 }, /* CTRL0D: full array, no binning */

	/*
     * FID0_ROI (0x3300): region of interest enable
     * bit[0] = ROIH1ON: enable horizontal ROI window 1
     * bit[1] = ROIV1ON: enable vertical ROI window 1
     * 0x00 = no ROI active, full 1456x1088 array output
     */
	{ 0x3300, 0x00 }, /* FID0_ROI: no ROI, full array */

	{ IMX296_TABLE_END, 0x00 },
};

/*
 * 1280x720 @ 90fps centered ROI mode
 *
 * The IMX296 reads out a cropped window via the FID0_ROI block
 * (same mechanism the Raspberry Pi kernel driver uses for crops).
 * Line period is unchanged (HMAX stays 1100 = 14.81us), but with only
 * 720 active lines VMAX can drop to 750, giving:
 *   74.25MHz / (1100 * 750) = 90.0 fps exactly.
 *
 * Crop is centered and 4-aligned, preserving the RGGB Bayer phase:
 *   left = (1456-1280)/2 = 88,  top = (1088-720)/2 = 184
 *
 * MIPIC_AREA3W must match the cropped output height (the RPi driver
 * writes crop->height there), overriding the 1088 from common_regs.
 * VMAX=750 also overrides common_regs' 1118 default; set_frame_rate()
 * then recomputes VMAX from the requested framerate, floored at the
 * mode-relative minimum (active_h + 30 = 750 for this mode — see
 * imx296_min_frame_length(); a fixed 1118 floor would clamp 90 fps
 * back to ~60).
 */
static const imx296_reg imx296_1280x720_regs[] = {
	{ 0x300d, 0x00 }, /* CTRL0D: full readout mode, no binning */

	{ 0x3300, 0x03 }, /* FID0_ROI: ROIH1ON | ROIV1ON */
	{ 0x3310, 0x58 }, /* FID0_ROIPH1[7:0]  = 88 (crop left) */
	{ 0x3311, 0x00 }, /* FID0_ROIPH1[15:8] */
	{ 0x3312, 0xb8 }, /* FID0_ROIPV1[7:0]  = 184 (crop top) */
	{ 0x3313, 0x00 }, /* FID0_ROIPV1[15:8] */
	{ 0x3314, 0x00 }, /* FID0_ROIWH1[7:0]  = 1280 (crop width) */
	{ 0x3315, 0x05 }, /* FID0_ROIWH1[15:8] */
	{ 0x3316, 0xd0 }, /* FID0_ROIWV1[7:0]  = 720 (crop height) */
	{ 0x3317, 0x02 }, /* FID0_ROIWV1[15:8] */

	{ 0x4182, 0xd0 }, /* MIPIC_AREA3W[7:0]  = 720 (output height) */
	{ 0x4183, 0x02 }, /* MIPIC_AREA3W[15:8] */

	{ 0x3010, 0xee }, /* VMAX[7:0]   = 750 -> 90.0 fps */
	{ 0x3011, 0x02 }, /* VMAX[15:8]  */
	{ 0x3012, 0x00 }, /* VMAX[22:16] */

	{ IMX296_TABLE_END, 0x00 },
};

/*
 * Start stream table - intentionally minimal.
 * The actual two-step start sequence is handled directly in
 * imx296_start_streaming() because it requires a 2ms delay
 * between the two register writes which cannot be expressed
 * cleanly in a register table.
 *
 * Step 1: CTRL00 = 0x00 (exit standby)
 * Step 2: wait 2ms
 * Step 3: CTRL0A = 0x00 (start MIPI transmission)
 */
static const imx296_reg imx296_start_stream[] = {
	{ IMX296_TABLE_END, 0x00 },
};

/*
 * Stop stream table - intentionally minimal.
 * The actual two-step stop sequence is handled directly in
 * imx296_stop_streaming():
 *
 * Step 1: CTRL0A = IMX296_CTRL0A_XMSTA (stop MIPI transmission)
 * Step 2: CTRL00 = IMX296_CTRL00_STANDBY (enter standby)
 */
static const imx296_reg imx296_stop_stream[] = {
	{ IMX296_TABLE_END, 0x00 },
};

/*
 * Test pattern table
 *
 * IMX296 supports 10 test patterns via PGCTRL (0x3238):
 * bit[0] = REGEN:    pattern generator enable
 * bit[1] = THRU:     bypass sensor data with pattern
 * bit[2] = CLKEN:    pattern generator clock enable
 * bits[5:3] = MODE:  pattern type:
 *   0 = Multiple pixels
 *   1 = Sequence 1
 *   2 = Sequence 2
 *   3 = Gradient      <- we use this for bring-up
 *   4 = Row
 *   5 = Column
 *   6 = Cross
 *   7 = Stripe
 *   8 = Checks
 *
 * Using gradient pattern (MODE=3) - easy to verify pixel ordering
 * and dynamic range during bring-up.
 *
 * PGCTRL = REGEN | CLKEN | MODE(3) = 0x01 | 0x04 | (3<<3)
 *        = 0x01 | 0x04 | 0x18 = 0x1d
 */
static const imx296_reg imx296_test_pattern[] = {
	/* Pattern position and size */
	{ 0x3239, 0x08 }, /* PGHPOS[7:0]:  horizontal start = 8 */
	{ 0x323a, 0x00 }, /* PGHPOS[15:8]: high byte */
	{ 0x323b, 0x08 }, /* PGVPOS[7:0]:  vertical start = 8 */
	{ 0x323c, 0x00 }, /* PGVPOS[15:8]: high byte */
	{ 0x323e, 0x08 }, /* PGHPSTEP: horizontal step size = 8 */
	{ 0x323f, 0x08 }, /* PGVPSTEP: vertical step size = 8 */
	{ 0x3240, 0x64 }, /* PGHPNUM: horizontal repeat count = 100 */
	{ 0x3241, 0x64 }, /* PGVPNUM: vertical repeat count = 100 */

	/* Pattern data values */
	{ 0x3244, 0x00 }, /* PGDATA1[7:0]:  = 0x0300 */
	{ 0x3245, 0x03 }, /* PGDATA1[15:8]: */
	{ 0x3246, 0x00 }, /* PGDATA2[7:0]:  = 0x0100 */
	{ 0x3247, 0x01 }, /* PGDATA2[15:8]: */
	{ 0x3249, 0x00 }, /* PGHGSTEP: horizontal gray step = 0 */

	/* Disable black level auto-calibration during test pattern */
	{ 0x3254, 0x00 }, /* BLKLEVEL[7:0]  = 0 */
	{ 0x3255, 0x00 }, /* BLKLEVEL[15:8] = 0 */
	{ 0x3022, 0xf0 }, /* BLKLEVELAUTO: OFF during test pattern */

	/* Enable gradient test pattern generator */
	{ 0x3238, 0x1d }, /* PGCTRL: REGEN|CLKEN|MODE(3=gradient) = 0x1d */

	{ IMX296_TABLE_END, 0x00 },
};

/*
 * mode_table - indexed by IMX296_MODE_* enum values
 * Used by set_mode() and start/stop_streaming() via mode_table[idx]
 */
static const imx296_reg *const mode_table[] = {
	[IMX296_MODE_1456X1088] = imx296_1456x1088_regs,
	[IMX296_MODE_1280X720] = imx296_1280x720_regs,
	[IMX296_MODE_START_STREAM] = imx296_start_stream,
	[IMX296_MODE_STOP_STREAM] = imx296_stop_stream,
	[IMX296_MODE_TEST_PATTERN] = imx296_test_pattern,
};

/*
 * Supported frame rates
 * IMX296 max = 60fps at 1456x1088 with 37.125MHz MCLK and VMAX=1118
 * Higher rates possible with reduced VMAX (more blanking removed)
 * but 60fps is the practical maximum for this clock configuration.
 */
static const int imx296_60fps[] = { 60 };
static const int imx296_90fps[] = { 90 };

/*
 * imx296_frmfmt - frame format table
 * Consumed by camera_common framework for V4L2 format enumeration.
 * MUST stay in sync with the DT mode<N> nodes: entry order matches the
 * mode enum / DT mode index (frmfmt count == number of DT modes).
 */
static const struct camera_common_frmfmt imx296_frmfmt[] = {
	{
		.size = { 1456, 1088 },
		.framerates = imx296_60fps,
		.num_framerates = 1,
		.hdr_en = false,
		.mode = IMX296_MODE_1456X1088,
	},
	{
		.size = { 1280, 720 },
		.framerates = imx296_90fps,
		.num_framerates = 1,
		.hdr_en = false,
		.mode = IMX296_MODE_1280X720,
	},
};

#endif /* __IMX296_MODE_TBLS_H__ */