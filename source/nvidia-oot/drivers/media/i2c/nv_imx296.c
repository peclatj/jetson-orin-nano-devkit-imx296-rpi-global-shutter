// SPDX-License-Identifier: GPL-2.0
/*
 * nv_imx296.c - IMX296 sensor driver for NVIDIA Jetson
 *
 * Author: Jonathan Peclat <peclatj@bluewin.ch>
 * Date: 2026-03-22
 *
 * Based on:
 *   drivers/media/i2c/imx296.c - Copyright 2019 Laurent Pinchart
 *   nv_imx185.c - Copyright (c) 2016-2023 NVIDIA CORPORATION
 *
 * SPDX-License-Identifier: GPL-2.0
 */

/* ============================================================
 * SECTION 1 - INCLUDES
 * ============================================================ */

/* Must precede all includes: dev_dbg()'s macro expansion in
 * dev_printk.h keys off DEBUG at the point <linux/module.h> is
 * included below. */
#define DEBUG

#include <nvidia/conftest.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/version.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>
#include "imx296_mode_tbls.h"

/* ============================================================
 * SECTION 2 - DEFINES
 * ============================================================ */

/* ---- Chip identification ----
 * Register 0x3148 SENSOR_INFO:
 * bit[15]    = 1 -> monochrome (IMX296LL)
 * bit[15]    = 0 -> color     (IMX296LQ)
 * bits[14:6] = model number (296 or 297)
 *
 * NOTE: SENSOR_INFO cannot be read in standby mode.
 * Sensor must be in operating mode (CTRL00=0) to read it.
 */
#define IMX296_SENSOR_INFO_ADDR 0x3148
#define IMX296_SENSOR_INFO_MONO BIT(15)
#define IMX296_SENSOR_INFO_IMX296LQ 0x4a00
#define IMX296_SENSOR_INFO_IMX296LL 0xca00
#define IMX296_CHIP_ID 296 /* model number in bits[14:6] */

/* ---- Standby / streaming control ---- */
#define IMX296_CTRL00_ADDR 0x3000
#define IMX296_CTRL00_STANDBY BIT(0) /* 1=standby, 0=operating */
#define IMX296_CTRL0A_ADDR 0x300a
#define IMX296_CTRL0A_XMSTA BIT(0) /* 1=stop, 0=start transmission */

/* ---- Group hold (register hold) ----
 * Write REGHOLD=1 before updating registers,
 * REGHOLD=0 to apply all changes atomically at next frame boundary.
 */
#define IMX296_CTRL08_ADDR 0x3008
#define IMX296_CTRL08_REGHOLD BIT(0)

/* ---- Flip/mirror ---- */
#define IMX296_CTRL0E_ADDR 0x300e
#define IMX296_CTRL0E_VREVERSE BIT(0) /* vertical flip */
#define IMX296_CTRL0E_HREVERSE BIT(1) /* horizontal mirror */

/* ---- Frame length (VMAX) - 24-bit little-endian, 3 registers ----
 * Total lines per frame including active and blanking lines.
 * line 0 = LSB at lowest address (little-endian).
 */
#define IMX296_VMAX_ADDR 0x3010 /* VMAX[7:0]  */
#define IMX296_VMAX_ADDR_MID 0x3011 /* VMAX[15:8] */
#define IMX296_VMAX_ADDR_HIGH 0x3012 /* VMAX[22:16] only bits[2:0] used */
#define IMX296_VMAX_MAX 0x1FFFF /* 17-bit max */
#define IMX296_VMAX_MIN (IMX296_PIXEL_ARRAY_HEIGHT + 30)

/* ---- Line length (HMAX) - 16-bit little-endian, 2 registers ----
 * Fixed at 1100 in 74.25MHz clock units (from mainline driver).
 * line_period = 1100 / 74.25MHz = 14.81us
 */
#define IMX296_HMAX_ADDR 0x3014 /* HMAX[7:0]  */
#define IMX296_HMAX_ADDR_HIGH 0x3015 /* HMAX[15:8] */
#define IMX296_HMAX_DEFAULT 1100 /* in 74.25MHz clock units */

/* ---- Exposure (SHS1) - 24-bit little-endian, 3 registers ----
 * IMX296 uses inverted shutter speed convention:
 *   exposure_lines = VMAX - SHS1
 *   SHS1 = VMAX - coarse_time
 * From libcamera: exposure_us = lines * 14.81us + 14.26us
 */
#define IMX296_SHS1_ADDR 0x308d /* SHS1[7:0]  */
#define IMX296_SHS1_ADDR_MID 0x308e /* SHS1[15:8] */
#define IMX296_SHS1_ADDR_HIGH 0x308f /* SHS1[22:16] */

/* ---- Gain - 16-bit little-endian, 2 registers ----
 * Gain is dB-based: register = 20 * log10(gain) * 10
 * gain_linear = 10^(register / 200)
 * The sensor converts dB to linear gain in hardware.
 * Range: 0 (0dB=1x) to 480 (48.0dB~251x), matching mainline
 * drivers/media/i2c/imx296.c IMX296_GAIN_MAX. libcamera's AE
 * self-imposes a lower maxGainCode (239) as a noise policy choice,
 * not a hardware limit - do not confuse the two.
 */
#define IMX296_GAIN_ADDR 0x3204 /* GAIN[7:0]  */
#define IMX296_GAIN_ADDR_HIGH 0x3205 /* GAIN[15:8] */
#define IMX296_GAIN_MIN 0 /* 0 dB  = 1.0x */
#define IMX296_GAIN_MAX 480 /* 48.0dB = ~251x */

/* ---- Test pattern ---- */
#define IMX296_PGCTRL_ADDR 0x3238
#define IMX296_PGCTRL_REGEN BIT(0) /* pattern generator enable */
#define IMX296_PGCTRL_THRU BIT(1) /* bypass sensor with pattern */
#define IMX296_PGCTRL_CLKEN BIT(2) /* pattern clock enable */
#define IMX296_PGCTRL_MODE(n) ((n) << 3) /* pattern type 0-9 */

/* ---- Pixel array dimensions ---- */
#define IMX296_PIXEL_ARRAY_WIDTH 1456
#define IMX296_PIXEL_ARRAY_HEIGHT 1088

/* ---- Timing constants from libcamera cam_helper_imx296.cpp ----
 * timePerLine = 1100.0 / 74.25e6 * 1.0s = 14.81us
 * exposure_us = lines * timePerLine + 14.26us (readout offset)
 * From libcamera: frameIntegrationDiff = 4 lines
 */
#define IMX296_EXPOSURE_OFFSET_US 14 /* 14.26us fixed offset, rounded */
#define IMX296_FRAME_INTEGRATION_DIFF 4 /* lines margin from frame end */

/* ---- Fuse ID ----
 * Use SENSOR_INFO register bytes as unique identifier.
 * 2 bytes = 4 hex chars in string representation.
 */
#define IMX296_FUSE_ID_SIZE 2
#define IMX296_FUSE_ID_STR_SIZE (IMX296_FUSE_ID_SIZE * 2)

/* ---- Register count defines for register helper arrays ---- */
#define IMX296_VMAX_REG_COUNT 3 /* 24-bit frame length */
#define IMX296_SHS1_REG_COUNT 3 /* 24-bit exposure */
#define IMX296_GAIN_REG_COUNT 2 /* 16-bit gain */

/* ============================================================
 * SECTION 3 - PRIVATE DATA STRUCT
 * ============================================================ */

struct imx296 {
	struct i2c_client *i2c_client;
	struct v4l2_subdev *subdev;
	u8 fuse_id[IMX296_FUSE_ID_SIZE];
	u32 frame_length;
	bool mono; /* true = IMX296LL monochrome */
	bool v_flip;
	bool h_mirror;
	struct camera_common_data *s_data;
	struct tegracam_device *tc_dev;
};

/* ============================================================
 * SECTION 4 - REGMAP CONFIG AND CTRL LIST
 * ============================================================ */

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16, /* IMX296 uses 16-bit register addresses */
	.val_bits = 8, /* and 8-bit register values */
	/*
     * No register caching - safer during bring-up.
     * Can be changed to REGCACHE_RBTREE later for performance.
     */
	.cache_type = REGCACHE_NONE,
	.use_single_read = true,
	.use_single_write = true,
};

static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_FUSE_ID,
	/* No HDR, no EEPROM, no OTP */
	/* TEGRA_CAMERA_CID_SENSOR_MODE_ID handled by framework automatically */
};

/* ============================================================
 * SECTION 5 - OF DEVICE ID TABLE
 * ============================================================ */

static const struct of_device_id imx296_of_match[] = {
	{
		.compatible = "sony,imx296",
	}, /* auto-detect mono/color */
	{
		.compatible = "sony,imx296lq",
	}, /* force color */
	{
		.compatible = "sony,imx296ll",
	}, /* force monochrome */
	{},
};
MODULE_DEVICE_TABLE(of, imx296_of_match);

/* ============================================================
 * SECTION 6 - MODULE PARAMETER
 * ============================================================ */

static int test_mode;
module_param(test_mode, int, 0644);

/* ============================================================
 * SECTION 7 - REGISTER HELPER FUNCTIONS
 * ============================================================ */

/*
 * Frame length (VMAX) - 24-bit little-endian
 * IMX296 uses little-endian byte order (LSB at lower address).
 * This is opposite to OV9281 which used big-endian.
 */
static inline void imx296_get_frame_length_regs(imx296_reg *regs,
						u32 frame_length)
{
	regs->addr = IMX296_VMAX_ADDR;
	regs->val = frame_length & 0xff; /* LSB first */

	(regs + 1)->addr = IMX296_VMAX_ADDR_MID;
	(regs + 1)->val = (frame_length >> 8) & 0xff;

	(regs + 2)->addr = IMX296_VMAX_ADDR_HIGH;
	(regs + 2)->val = (frame_length >> 16) & 0x07; /* only bits[2:0] */
}

/*
 * Exposure via SHS1 (shutter speed) - 24-bit little-endian.
 * SHS1 is inverted: exposure_lines = VMAX - SHS1
 * Caller must pass shs1 = frame_length - coarse_time.
 *
 * From libcamera cam_helper_imx296.cpp:
 *   exposure_us = coarse_time * timePerLine + 14.26us
 *   timePerLine = 1100 / 74.25MHz = 14.81us
 *   coarse_time = (exposure_us - 14.26us) / 14.81us
 */
static inline void imx296_get_coarse_time_regs(imx296_reg *regs, u32 shs1)
{
	regs->addr = IMX296_SHS1_ADDR;
	regs->val = shs1 & 0xff; /* LSB first */

	(regs + 1)->addr = IMX296_SHS1_ADDR_MID;
	(regs + 1)->val = (shs1 >> 8) & 0xff;

	(regs + 2)->addr = IMX296_SHS1_ADDR_HIGH;
	(regs + 2)->val = (shs1 >> 16) & 0x07;
}

/*
 * Gain - 16-bit little-endian.
 * Register value = gain in dB * 10
 * Range: 0 (0dB=1x) to 239 (23.9dB=~15.8x)
 * The sensor converts dB to linear gain internally in hardware:
 *   linear_gain = 10^(register / 200)
 */
static inline void imx296_get_gain_regs(imx296_reg *regs, u16 gain)
{
	regs->addr = IMX296_GAIN_ADDR;
	regs->val = gain & 0xff; /* LSB first */

	(regs + 1)->addr = IMX296_GAIN_ADDR_HIGH;
	(regs + 1)->val = (gain >> 8) & 0xff;
}

/* ============================================================
 * SECTION 8 - I2C READ/WRITE HELPERS
 * ============================================================ */

static inline int imx296_read_reg(struct camera_common_data *s_data, u16 addr,
				  u8 *val)
{
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(s_data->regmap, addr, &reg_val);
	*val = reg_val & 0xFF;

	return err;
}

static int imx296_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	int err = 0;
	struct device *dev = s_data->dev;

	err = regmap_write(s_data->regmap, addr, val);
	if (err)
		dev_err(dev, "%s: i2c write failed, 0x%x = %x\n", __func__,
			addr, val);

	return err;
}

static int imx296_write_table(struct imx296 *priv, const imx296_reg table[])
{
	struct camera_common_data *s_data = priv->s_data;

	return regmap_util_write_table_8(s_data->regmap, table, NULL, 0,
					 IMX296_TABLE_WAIT_MS,
					 IMX296_TABLE_END);
}

/*
 * Read a 16-bit register value (little-endian, LSB at lower address).
 * Used for SENSOR_INFO register (0x3148) during chip identification.
 */
static int imx296_read_reg16(struct camera_common_data *s_data, u16 addr,
			     u16 *val)
{
	u8 lo = 0, hi = 0;
	int err = 0;

	err = imx296_read_reg(s_data, addr, &lo);
	if (err)
		return err;

	err = imx296_read_reg(s_data, addr + 1, &hi);
	if (err)
		return err;

	/* IMX296 is little-endian: LSB at lower address */
	*val = ((u16)hi << 8) | lo;
	return 0;
}

/* ============================================================
 * SECTION 9 - POWER FUNCTIONS
 * ============================================================ */

static int imx296_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power on\n", __func__);
	dev_info(dev, "%s: reset_gpio = %d state = %d\n", __func__,
		 pw->reset_gpio, pw->state);

	if (pdata && pdata->power_on) {
		err = pdata->power_on(pw);
		if (!err)
			goto power_on_done;
		else
			dev_err(dev, "%s failed.\n", __func__);
		return err;
	}

	/*
     * IMX296 reset is ACTIVE LOW:
     * - Assert reset: drive GPIO LOW  (sensor held in reset)
     * - Release reset: drive GPIO HIGH (sensor starts)
     *
     * Our device tree uses GPIO_ACTIVE_HIGH so the values
     * here (0=assert, 1=release) map correctly to the
     * electrical levels via the GPIO framework.
     *
     * Power sequence from mainline imx296.c:
     * 1. Assert reset LOW
     * 2. Enable MCLK (done by camera_common_mclk_enable before power_on)
     * 3. Wait 1us
     * 4. Release reset HIGH
     * 5. Wait >= 1ms before first I2C transaction
     */
	if (pw->reset_gpio)
		gpio_set_value(pw->reset_gpio, 0); /* assert reset */

	usleep_range(1000, 2000);

	if (pw->reset_gpio)
		gpio_set_value(pw->reset_gpio, 1); /* release reset */

	dev_info(dev, "%s: gpio value after release = %d\n", __func__,
		 gpio_get_value(pw->reset_gpio));

	/* Wait for sensor internal initialization.
    * Sony sensors require long startup time after reset release.
    * IMX477 datasheet specifies 270ms (t10). Use 300ms to be safe.
    */
	usleep_range(300000, 301000); /* 300ms */

power_on_done:
	pw->state = SWITCH_ON;
	return 0;
}

static int imx296_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power off\n", __func__);

	if (pdata && pdata->power_off) {
		err = pdata->power_off(pw);
		if (!err)
			goto power_off_done;
		else
			dev_err(dev, "%s failed.\n", __func__);
		return err;
	}

	/* Assert reset LOW - sensor enters reset/standby */
	usleep_range(1, 2);
	if (pw->reset_gpio)
		gpio_set_value(pw->reset_gpio, 0);

power_off_done:
	pw->state = SWITCH_OFF;
	return 0;
}

static int imx296_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct clk *parent;
	int ret = 0;

	dev_info(dev, "%s: called\n", __func__);

	/* Sensor MCLK - optional, only fetched if the DT provides a name */
	if (pdata->mclk_name) {
		pw->mclk = devm_clk_get(dev, pdata->mclk_name);
		if (IS_ERR(pw->mclk)) {
			dev_err(dev, "unable to get clock %s\n",
				pdata->mclk_name);
			return PTR_ERR(pw->mclk);
		}

		dev_info(dev, "Got clock %s successfully\n", pdata->mclk_name);

		parent = devm_clk_get(dev, "pllp_grtba");
		if (IS_ERR(parent))
			dev_err(dev, "devm_clk_get failed for pllp_grtba\n");
		else {
			if (clk_set_parent(pw->mclk, parent) < 0)
				dev_dbg(dev, "%s failed to set parent clock\n",
					__func__);
		}
	}

	/* XSHUTDOWN GPIO from device tree reset-gpios property */
	pw->reset_gpio = pdata->reset_gpio;
	dev_info(dev, "%s: reset_gpio = %d, valid = %d\n", __func__,
		 pw->reset_gpio, gpio_is_valid(pw->reset_gpio));

	if (gpio_is_valid(pw->reset_gpio)) {
		ret = gpio_request(pw->reset_gpio, "cam_reset_gpio");
		dev_info(dev, "%s: gpio_request ret = %d\n", __func__, ret);
		if (ret < 0)
			dev_dbg(dev, "%s: can't request reset_gpio %d\n",
				__func__, ret);
		else
			/*
             * Start with GPIO LOW = reset asserted.
             * IMX296 reset is active LOW.
             */
			gpio_direction_output(pw->reset_gpio, 0);
	}

	pw->state = SWITCH_OFF;
	return 0;
}

static int imx296_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (gpio_is_valid(pw->reset_gpio))
		gpio_free(pw->reset_gpio);

	return 0;
}

/* ============================================================
 * SECTION 10 - CONTROL HANDLERS
 * ============================================================ */

static int imx296_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	int err = 0;

	/*
     * IMX296 group hold via CTRL08 REGHOLD bit:
     * 1 = hold all register writes internally
     * 0 = apply all buffered writes atomically at next frame boundary
     */
	err = imx296_write_reg(s_data, IMX296_CTRL08_ADDR,
			       val ? IMX296_CTRL08_REGHOLD : 0);
	if (err)
		dev_dbg(dev, "%s: group hold control error\n", __func__);

	return err;
}

static int imx296_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	imx296_reg reg_list[IMX296_GAIN_REG_COUNT];
	int err = 0;
	u16 gain;
	int i;

	if (mode->control_properties.gain_factor == 0) {
		dev_err(dev, "%s: gain_factor is 0\n", __func__);
		return -EINVAL;
	}

	/*
     * Device tree: gain_factor=10
     * Framework passes val = dB * 10 * gain_factor
     * Register expects dB * 10 directly.
     * So: register = val / gain_factor
     *
     * Example: 6dB gain
     *   val = 6.0 * 10 * 10 = 600 (framework scaling)
     *   gain = 600 / 10 = 60 -> register = 60
     *
     * The sensor converts dB to linear internally:
     *   linear_gain = 10^(60/200) = 10^0.3 = 2.0x
     */
	gain = (u16)(val / mode->control_properties.gain_factor);
	gain = clamp_t(u16, gain, IMX296_GAIN_MIN, IMX296_GAIN_MAX);

	dev_dbg(dev, "%s: gain reg: %d\n", __func__, gain);

	imx296_get_gain_regs(reg_list, gain);

	for (i = 0; i < IMX296_GAIN_REG_COUNT; i++) {
		err = imx296_write_reg(s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err) {
			dev_dbg(dev, "%s: gain control error\n", __func__);
			return err;
		}
	}

	return 0;
}

static int imx296_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	struct imx296 *priv = (struct imx296 *)tc_dev->priv;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	imx296_reg reg_list[IMX296_VMAX_REG_COUNT];
	int err = 0;
	u32 frame_length;
	int i;

	if (mode->image_properties.line_length == 0 || val == 0) {
		dev_err(dev, "%s: line_length=%d frame_rate=%lld\n", __func__,
			mode->image_properties.line_length, val);
		return -EINVAL;
	}

	/*
     * frame_length (lines) = pixel_clock / (line_length * frame_rate)
     * val is in Q format scaled by framerate_factor (1000000)
     *
     * line_length = 1760 pixels at 118.8MHz from device tree
     * pix_clk_hz  = 118800000 from device tree
     */
	frame_length = mode->signal_properties.pixel_clock.val *
		       mode->control_properties.framerate_factor /
		       mode->image_properties.line_length / val;

	frame_length =
		clamp_t(u32, frame_length, IMX296_VMAX_MIN, IMX296_VMAX_MAX);

	priv->frame_length = frame_length;

	dev_dbg(dev, "%s: val=%lld frame_length=%d\n", __func__, val,
		frame_length);

	imx296_get_frame_length_regs(reg_list, frame_length);

	for (i = 0; i < IMX296_VMAX_REG_COUNT; i++) {
		err = imx296_write_reg(s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err) {
			dev_dbg(dev, "%s: frame rate control error\n",
				__func__);
			return err;
		}
	}

	return 0;
}

static int imx296_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	struct imx296 *priv = (struct imx296 *)tc_dev->priv;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	imx296_reg reg_list[IMX296_SHS1_REG_COUNT];
	int err = 0;
	u32 coarse_time;
	u32 shs1;
	int i;

	if (mode->control_properties.exposure_factor == 0 ||
	    mode->image_properties.line_length == 0) {
		dev_err(dev, "%s: line_length=%d exposure_factor=%d\n",
			__func__, mode->image_properties.line_length,
			mode->control_properties.exposure_factor);
		return -EINVAL;
	}

	/*
     * Convert exposure time to coarse integration lines.
     *
     * From libcamera cam_helper_imx296.cpp:
     *   exposure_us = lines * timePerLine + 14.26us
     *   timePerLine = 1100 / 74.25MHz = 14.81us
     *   lines = (exposure_us - 14.26us) / 14.81us
     *
     * val is in Q format scaled by exposure_factor (1000000 = microseconds).
     * Subtract the 14.26us (IMX296_EXPOSURE_OFFSET_US) offset before
     * converting to lines. Multiply first to preserve integer precision.
     */
	coarse_time =
		(u32)(mode->signal_properties.pixel_clock.val *
		      (val - IMX296_EXPOSURE_OFFSET_US *
				     mode->control_properties.exposure_factor) /
		      mode->image_properties.line_length /
		      mode->control_properties.exposure_factor);

	if (priv->frame_length == 0)
		priv->frame_length = IMX296_VMAX_MIN;

	/*
     * IMX296 exposure via SHS1 (shutter speed, inverted):
     *   exposure_lines = VMAX - SHS1
     *   SHS1 = VMAX - exposure_lines
     *
     * IMX296_FRAME_INTEGRATION_DIFF = 4 lines minimum margin
     * (from libcamera frameIntegrationDiff = 4)
     */
	coarse_time =
		clamp_t(u32, coarse_time, 1,
			priv->frame_length - IMX296_FRAME_INTEGRATION_DIFF);

	shs1 = priv->frame_length - coarse_time;

	dev_dbg(dev, "%s: val=%lld coarse_time=%d shs1=%d frame_length=%d\n",
		__func__, val, coarse_time, shs1, priv->frame_length);

	imx296_get_coarse_time_regs(reg_list, shs1);

	for (i = 0; i < IMX296_SHS1_REG_COUNT; i++) {
		err = imx296_write_reg(s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err) {
			dev_dbg(dev, "%s: exposure control error\n", __func__);
			return err;
		}
	}

	return 0;
}

static int imx296_fill_string_ctrl(struct tegracam_device *tc_dev,
				   struct v4l2_ctrl *ctrl)
{
	struct imx296 *priv = tc_dev->priv;
	int i, ret;

	switch (ctrl->id) {
	case TEGRA_CAMERA_CID_FUSE_ID:
		for (i = 0; i < IMX296_FUSE_ID_SIZE; i++) {
			ret = sprintf(&ctrl->p_new.p_char[i * 2], "%02x",
				      priv->fuse_id[i]);
			if (ret < 0)
				return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	ctrl->p_cur.p_char = ctrl->p_new.p_char;
	return 0;
}

/* ============================================================
 * SECTION 11 - TEGRACAM CTRL OPS
 * Must appear AFTER all control handler functions it references.
 * ============================================================ */

static struct tegracam_ctrl_ops imx296_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	/* string_ctrl_size slots: [0]=EEPROM, [1]=FUSE_ID, [2]=OTP */
	.string_ctrl_size = { 0, IMX296_FUSE_ID_STR_SIZE, 0 },
	.set_gain = imx296_set_gain,
	.set_exposure = imx296_set_exposure,
	.set_frame_rate = imx296_set_frame_rate,
	.set_group_hold = imx296_set_group_hold,
	.fill_string_ctrl = imx296_fill_string_ctrl,
};

/* ============================================================
 * SECTION 12 - STREAM FUNCTIONS
 * ============================================================ */

#ifdef DEBUG
static void imx296_dump_registers(struct imx296 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct device *dev = s_data->dev;
	u8 val = 0;

	dev_info(dev, "=== IMX296 Register Dump ===\n");

	/* ---- Streaming control ---- */
	imx296_read_reg(s_data, 0x3000, &val);
	dev_info(
		dev,
		"CTRL00       [0x3000] = 0x%02x (0x00=streaming, 0x01=standby)\n",
		val);
	imx296_read_reg(s_data, 0x3008, &val);
	dev_info(dev, "CTRL08       [0x3008] = 0x%02x (bit0=reghold)\n", val);
	imx296_read_reg(s_data, 0x300a, &val);
	dev_info(
		dev,
		"CTRL0A       [0x300a] = 0x%02x (0x00=MIPI on, 0x01=MIPI stop)\n",
		val);
	imx296_read_reg(s_data, 0x300d, &val);
	dev_info(dev, "CTRL0D       [0x300d] = 0x%02x (winmode/binning)\n",
		 val);
	imx296_read_reg(s_data, 0x300e, &val);
	dev_info(dev,
		 "CTRL0E       [0x300e] = 0x%02x (bit0=vflip, bit1=hmirror)\n",
		 val);

	/* ---- PLL / Clock ---- */
	imx296_read_reg(s_data, 0x3005, &val);
	dev_info(
		dev,
		"CLK_CTRL     [0x3005] = 0x%02x (CSI2 activation, should be 0xf0)\n",
		val);
	imx296_read_reg(s_data, 0x3089, &val);
	dev_info(dev,
		 "INCKSEL0     [0x3089] = 0x%02x (should be 0xb0 for 54MHz)\n",
		 val);
	imx296_read_reg(s_data, 0x308a, &val);
	dev_info(dev,
		 "INCKSEL1     [0x308a] = 0x%02x (should be 0x0f for 54MHz)\n",
		 val);
	imx296_read_reg(s_data, 0x308b, &val);
	dev_info(dev,
		 "INCKSEL2     [0x308b] = 0x%02x (should be 0xb0 for 54MHz)\n",
		 val);
	imx296_read_reg(s_data, 0x308c, &val);
	dev_info(dev,
		 "INCKSEL3     [0x308c] = 0x%02x (should be 0x0c for 54MHz)\n",
		 val);
	imx296_read_reg(s_data, 0x418c, &val);
	dev_info(
		dev,
		"CTRL418C     [0x418c] = 0x%02x (should be 0xa8 for 75.25MHz)\n",
		val);

	/* ---- HMAX / VMAX ---- */
	{
		u8 lo, hi, high;
		imx296_read_reg(s_data, 0x3014, &lo);
		imx296_read_reg(s_data, 0x3015, &hi);
		dev_info(
			dev,
			"HMAX         [0x3014-15] = 0x%04x = %d (should be 1100)\n",
			((u16)hi << 8) | lo, ((u16)hi << 8) | lo);

		imx296_read_reg(s_data, 0x3010, &lo);
		imx296_read_reg(s_data, 0x3011, &hi);
		imx296_read_reg(s_data, 0x3012, &high);
		dev_info(dev, "VMAX         [0x3010-12] = 0x%06x = %d\n",
			 ((u32)high << 16) | ((u32)hi << 8) | lo,
			 ((u32)high << 16) | ((u32)hi << 8) | lo);
	}

	/* ---- SHS1 (exposure) ---- */
	{
		u8 lo, hi, high;
		imx296_read_reg(s_data, 0x308d, &lo);
		imx296_read_reg(s_data, 0x308e, &hi);
		imx296_read_reg(s_data, 0x308f, &high);
		dev_info(dev, "SHS1         [0x308d-8f] = 0x%06x = %d lines\n",
			 ((u32)high << 16) | ((u32)hi << 8) | lo,
			 ((u32)high << 16) | ((u32)hi << 8) | lo);
	}

	/* ---- Gain ---- */
	{
		u8 lo, hi;
		imx296_read_reg(s_data, 0x3204, &lo);
		imx296_read_reg(s_data, 0x3205, &hi);
		dev_info(dev,
			 "GAIN         [0x3204-05] = 0x%04x = %d (dB*10)\n",
			 ((u16)hi << 8) | lo, ((u16)hi << 8) | lo);
	}

	/* ---- Gain control ---- */
	imx296_read_reg(s_data, 0x3200, &val);
	dev_info(dev, "GAINCTRL     [0x3200] = 0x%02x (0x01=normal)\n", val);
	imx296_read_reg(s_data, 0x3212, &val);
	dev_info(dev, "GAINDLY      [0x3212] = 0x%02x (0x08=no delay)\n", val);

	/* ---- Black level ---- */
	{
		u8 lo, hi;
		imx296_read_reg(s_data, 0x3254, &lo);
		imx296_read_reg(s_data, 0x3255, &hi);
		dev_info(
			dev,
			"BLKLEVEL     [0x3254-55] = 0x%04x = %d (should be 60=0x3c)\n",
			((u16)hi << 8) | lo, ((u16)hi << 8) | lo);
	}
	imx296_read_reg(s_data, 0x3022, &val);
	dev_info(dev, "BLKLEVELAUTO [0x3022] = 0x%02x (0x01=auto on)\n", val);

	/* ---- Sync / master mode ---- */
	imx296_read_reg(s_data, 0x3036, &val);
	dev_info(dev,
		 "SYNCSEL      [0x3036] = 0x%02x (0xc0=master, 0xf0=slave)\n",
		 val);

	/* ---- ROI ---- */
	imx296_read_reg(s_data, 0x3300, &val);
	dev_info(dev, "FID0_ROI     [0x3300] = 0x%02x (0x00=no ROI)\n", val);

	/* ---- Test pattern ---- */
	imx296_read_reg(s_data, 0x3238, &val);
	dev_info(dev, "PGCTRL       [0x3238] = 0x%02x (0x00=disabled)\n", val);

	/* ---- Sensor info ---- */
	{
		u8 lo, hi;
		imx296_read_reg(s_data, 0x3148, &lo);
		imx296_read_reg(s_data, 0x3149, &hi);
		dev_info(dev, "SENSOR_INFO  [0x3148-49] = 0x%04x\n",
			 ((u16)hi << 8) | lo);
	}

	imx296_read_reg(s_data, 0x300b, &val);
	dev_info(dev, "CTRL0B       [0x300b] = 0x%02x (bit0=trigger enable)\n",
		 val);
	imx296_read_reg(s_data, 0x3024, &val);
	dev_info(dev, "SST          [0x3024] = 0x%02x (bit0=SST enable)\n",
		 val);
	imx296_read_reg(s_data, 0x3021, &val);
	dev_info(dev, "WDSEL        [0x3021] = 0x%02x (0x00=normal)\n", val);
	imx296_read_reg(s_data, 0x300b, &val);
	dev_info(dev, "CTRL0B_TRIG  [0x300b] = 0x%02x (0x00=free run)\n", val);
	imx296_read_reg(s_data, 0x3026, &val);
	dev_info(dev, "CTRLTOUT     [0x3026] = 0x%02x\n", val);
	imx296_read_reg(s_data, 0x3029, &val);
	dev_info(dev, "CTRLTRIG     [0x3029] = 0x%02x\n", val);

	dev_info(dev, "=== End Register Dump ===\n");
}
#endif

static int imx296_set_mode(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	int err = 0;

#ifdef DEBUG
	dev_info(dev, "--- Register state BEFORE set_mode ---\n");
	imx296_dump_registers(priv);
#endif

	dev_info(tc_dev->dev, "%s: called, mode=%d\n", __func__,
		 tc_dev->s_data->mode_prop_idx);

	if (s_data->mode_prop_idx < 0)
		return -EINVAL;

	dev_dbg(dev, "%s: mode %d\n", __func__, s_data->mode_prop_idx);

	/* Write common initialization registers first */
	err = imx296_write_table(priv, imx296_common_regs);
	if (err)
		return err;

	/* Then write mode specific registers */
	err = imx296_write_table(priv, mode_table[s_data->mode_prop_idx]);

#ifdef DEBUG
	dev_info(dev, "--- Register state AFTER set_mode ---\n");
	imx296_dump_registers(priv);
#endif

	return err;
}

static int imx296_start_streaming(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	int err = 0;
	u8 val;

	dev_info(tc_dev->dev, "%s: called\n", __func__);

	dev_dbg(dev, "%s:\n", __func__);

	/* Apply flip/mirror settings via read-modify-write */
	err = imx296_read_reg(s_data, IMX296_CTRL0E_ADDR, &val);
	if (err)
		goto fail;

	if (priv->v_flip)
		val |= IMX296_CTRL0E_VREVERSE;
	else
		val &= ~IMX296_CTRL0E_VREVERSE;

	if (priv->h_mirror)
		val |= IMX296_CTRL0E_HREVERSE;
	else
		val &= ~IMX296_CTRL0E_HREVERSE;

	err = imx296_write_reg(s_data, IMX296_CTRL0E_ADDR, val);
	if (err)
		goto fail;

	/* Optionally enable test pattern for bring-up validation */
	if (test_mode) {
		err = imx296_write_table(priv,
					 mode_table[IMX296_MODE_TEST_PATTERN]);
		if (err)
			goto fail;
	}

#ifdef DEBUG
	dev_info(dev, "--- Register state BEFORE streaming ---\n");
	imx296_dump_registers(priv);
#endif

	/*
     * IMX296 two-step start sequence (from mainline imx296_stream_on):
     * Step 1: Exit standby - clear CTRL00_STANDBY bit
     * Step 2: Wait 2ms - sensor needs time to stabilize
     * Step 3: Start MIPI - clear CTRL0A_XMSTA bit
     *
     * The 2ms delay is critical - from mainline driver:
     * "500us results in I2C failures, 1000us seems enough, be conservative"
     */
	err = imx296_write_reg(s_data, IMX296_CTRL00_ADDR, 0);
	if (err)
		goto fail;

	//usleep_range(2000, 2500);

	// Test with a bigger sleep
	/* Wait for sensor internal initialization.
    * Sony sensors require long startup time after reset release.
    * IMX477 datasheet specifies 270ms (t10). Use 300ms to be safe.
    */
	usleep_range(300000, 301000); /* 300ms */

	err = imx296_write_reg(s_data, IMX296_CTRL0A_ADDR, 0);
	if (err)
		goto fail;

#ifdef DEBUG
	dev_info(dev, "--- Register state AFTER streaming start ---\n");
	imx296_dump_registers(priv);

	/* Wait 1 second then verify sensor is still in operating mode */
	msleep(1000);
	{
		u8 ctrl00 = 0xff, ctrl0a = 0xff;
		imx296_read_reg(s_data, IMX296_CTRL00_ADDR, &ctrl00);
		imx296_read_reg(s_data, IMX296_CTRL0A_ADDR, &ctrl0a);
		dev_info(dev, "%s: 1s later CTRL00=0x%02x CTRL0A=0x%02x\n",
			 __func__, ctrl00, ctrl0a);
	}
#endif

	return 0;

fail:
	dev_err(dev, "%s: error starting stream\n", __func__);
	return err;
}

static int imx296_stop_streaming(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = s_data->dev;
	int err = 0;

	dev_dbg(dev, "%s:\n", __func__);

	/*
     * IMX296 two-step stop sequence (from mainline imx296_stream_off):
     * Step 1: Stop MIPI transmission - set CTRL0A_XMSTA
     * Step 2: Enter standby - set CTRL00_STANDBY
     *
     * Order matters: must stop transmission before entering standby.
     * Wait one frame period after stopping for clean shutdown.
     * frame_time = frame_length * 14.81us
     * Use frame_length * 15us as safe approximation.
     */
	err = imx296_write_reg(s_data, IMX296_CTRL0A_ADDR, IMX296_CTRL0A_XMSTA);
	if (err)
		goto fail;

	err = imx296_write_reg(s_data, IMX296_CTRL00_ADDR,
			       IMX296_CTRL00_STANDBY);
	if (err)
		goto fail;

	usleep_range(priv->frame_length * 15, priv->frame_length * 15 + 1000);

	return 0;

fail:
	dev_err(dev, "%s: error stopping stream\n", __func__);
	return err;
}

/* ============================================================
 * SECTION 13 - V4L2 SUBDEV INTERNAL OPS
 * ============================================================ */

static int imx296_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);
	return 0;
}

static const struct v4l2_subdev_internal_ops imx296_subdev_internal_ops = {
	.open = imx296_open,
};

/* ============================================================
 * SECTION 14 - FUSE ID SETUP
 * Must appear BEFORE board_setup which calls it.
 * ============================================================ */

static int imx296_fuse_id_setup(struct imx296 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct device *dev = s_data->dev;
	u16 sensor_info;
	u16 model;
	int err = 0;

	/*
     * Read SENSOR_INFO register (0x3148) - 16-bit little-endian.
     *
     * CRITICAL: From mainline driver imx296.c:
     * "While most registers can be read when the sensor is in standby,
     * this is not the case of the sensor info register."
     * Sensor must be in operating mode (CTRL00=0) to read this register.
     *
     * bit[15]    = 1 -> monochrome (IMX296LL)
     *            = 0 -> color     (IMX296LQ)
     * bits[14:6] = model number (should be 296)
     */

	/* Exit standby before reading SENSOR_INFO */
	err = imx296_write_reg(s_data, IMX296_CTRL00_ADDR, 0);
	if (err) {
		dev_err(dev, "Failed to exit standby for sensor info read\n");
		return err;
	}
	usleep_range(2000, 2500); /* wait for sensor to be ready */

	err = imx296_read_reg16(s_data, IMX296_SENSOR_INFO_ADDR, &sensor_info);
	if (err) {
		dev_err(dev, "Failed to read SENSOR_INFO\n");
		goto standby;
	}

	/* Extract and verify model number from bits[14:6] */
	model = (sensor_info >> 6) & 0x1ff;
	if (model != IMX296_CHIP_ID) {
		dev_err(dev, "Unexpected model %u (expected %u)\n", model,
			IMX296_CHIP_ID);
		err = -ENODEV;
		goto standby;
	}

	/* Detect color vs monochrome variant */
	priv->mono = !!(sensor_info & IMX296_SENSOR_INFO_MONO);

	dev_info(dev, "IMX296%s detected (sensor_info=0x%04x)\n",
		 priv->mono ? "LL (monochrome)" : "LQ (color)", sensor_info);

	/* Store sensor_info bytes as fuse ID */
	priv->fuse_id[0] = (sensor_info >> 8) & 0xff;
	priv->fuse_id[1] = sensor_info & 0xff;

standby:
	/* Return to standby regardless of error */
	imx296_write_reg(s_data, IMX296_CTRL00_ADDR, IMX296_CTRL00_STANDBY);
	return err;
}

/* ============================================================
 * SECTION 15 - PARSE DT
 * Must appear BEFORE board_setup and camera_common_sensor_ops.
 * ============================================================ */

static struct camera_common_pdata *
imx296_parse_dt(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *board_priv_pdata;
	struct camera_common_pdata *ret = NULL;
	int gpio;

	if (!np)
		return NULL;

	board_priv_pdata =
		devm_kzalloc(dev, sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	/* Read mclk name - optional, only present if MCLK is host-supplied */
	if (of_property_read_string(np, "mclk", &board_priv_pdata->mclk_name))
		dev_dbg(dev,
			"mclk not in DT, assume sensor driven externally\n");

	/* Read XSHUTDOWN GPIO - required */
	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio < 0) {
		if (gpio == -EPROBE_DEFER)
			ret = ERR_PTR(-EPROBE_DEFER);
		dev_err(dev, "reset-gpios not found: %d\n", gpio);
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	/* Read optional flip/mirror properties */
	board_priv_pdata->v_flip = of_property_read_bool(np, "vertical-flip");
	board_priv_pdata->h_mirror =
		of_property_read_bool(np, "horizontal-mirror");

	return board_priv_pdata;

error:
	return ret;
}

/* ============================================================
 * SECTION 16 - BOARD SETUP
 * Calls fuse_id_setup and parse_dt - must appear after both.
 * ============================================================ */

static int imx296_board_setup(struct imx296 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;
	int err = 0;

	dev_dbg(dev, "%s++\n", __func__);

	/* Store flip/mirror preferences from device tree */
	priv->v_flip = pdata->v_flip;
	priv->h_mirror = pdata->h_mirror;

	/*
     * Enable MCLK if host-supplied - must be stable before any I2C
     * communication. Skipped when the sensor has its own oscillator.
     */
	if (pdata->mclk_name) {
		err = camera_common_mclk_enable(s_data);
		if (err) {
			dev_err(dev, "Error %d turning on mclk\n", err);
			return err;
		}

		dev_info(dev, "MCLK enabled successfully\n");
	}

	/* Release reset - exit hardware standby */
	err = imx296_power_on(s_data);
	if (err) {
		dev_err(dev, "Error %d during power on\n", err);
		goto disable_mclk;
	}

	/*
     * Read SENSOR_INFO - verifies I2C communication and detects
     * color vs monochrome variant.
     * imx296_fuse_id_setup handles standby/operating transition internally.
     */
	err = imx296_fuse_id_setup(priv);
	if (err)
		goto power_off;

power_off:
	imx296_power_off(s_data);
disable_mclk:
	if (pdata->mclk_name) {
		camera_common_mclk_disable(s_data);
	}
	return err;
}

/* ============================================================
 * SECTION 17 - CAMERA COMMON SENSOR OPS
 * Must appear after all functions it references.
 * ============================================================ */

static struct camera_common_sensor_ops imx296_common_ops = {
	.numfrmfmts = ARRAY_SIZE(imx296_frmfmt),
	.frmfmt_table = imx296_frmfmt,
	.power_on = imx296_power_on,
	.power_off = imx296_power_off,
	.write_reg = imx296_write_reg,
	.read_reg = imx296_read_reg,
	.parse_dt = imx296_parse_dt,
	.power_get = imx296_power_get,
	.power_put = imx296_power_put,
	.set_mode = imx296_set_mode,
	.start_streaming = imx296_start_streaming,
	.stop_streaming = imx296_stop_streaming,
};

/* ============================================================
 * SECTION 18 - PROBE AND REMOVE
 * ============================================================ */

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG)
static int imx296_probe(struct i2c_client *client)
#else
static int imx296_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct imx296 *priv;
	int err = 0;

	dev_info(dev, "probing IMX296 sensor\n");

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	/* Allocate private data */
	priv = devm_kzalloc(dev, sizeof(struct imx296), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Allocate tegracam device */
	tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	/* Initialize tegracam device */
	priv->i2c_client = tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, "imx296", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &sensor_regmap_config;
	tc_dev->sensor_ops = &imx296_common_ops;
	tc_dev->v4l2sd_internal_ops = &imx296_subdev_internal_ops;
	tc_dev->tcctrl_ops = &imx296_ctrl_ops;

	/* Register with tegracam framework */
	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}

	/* Link private data */
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	priv->subdev = &tc_dev->s_data->subdev;
	tegracam_set_privdata(tc_dev, (void *)priv);

	/* Run board setup - verifies chip ID and detects color/mono */
	err = imx296_board_setup(priv);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "board setup failed\n");
		return err;
	}

	/* Register V4L2 subdevice */
	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		dev_err(dev, "tegra camera subdev registration failed\n");
		tegracam_device_unregister(tc_dev);
		return err;
	}

	dev_info(dev, "IMX296%s sensor detected and registered\n",
		 priv->mono ? "LL" : "LQ");
	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT)
static int imx296_remove(struct i2c_client *client)
#else
static void imx296_remove(struct i2c_client *client)
#endif
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct imx296 *priv;

	if (!s_data)
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT)
		return -EINVAL;
#else
		return;
#endif

	priv = (struct imx296 *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT)
	return 0;
#endif
}

/* ============================================================
 * SECTION 19 - DRIVER REGISTRATION
 * ============================================================ */

static const struct i2c_device_id imx296_id[] = { { "imx296", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, imx296_id);

static struct i2c_driver imx296_i2c_driver = {
    .driver = {
        .name           = "imx296",
        .owner          = THIS_MODULE,
        .of_match_table = of_match_ptr(imx296_of_match),
    },
    .probe    = imx296_probe,
    .remove   = imx296_remove,
    .id_table = imx296_id,
};

module_i2c_driver(imx296_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for Sony IMX296");
MODULE_AUTHOR("Jonathan Peclat");
MODULE_LICENSE("GPL v2");