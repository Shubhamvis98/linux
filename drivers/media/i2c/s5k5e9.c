// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S5K5E9 5 MP image sensor driver
 *
 * The register settings come from the vendor camera stack of the Xiaomi
 * Mi 11T Pro (vili) macro camera module.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>

#define S5K5E9_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K5E9_CHIP_ID			0x559b
#define S5K5E9_REG_REVISION		CCI_REG8(0x0002)

#define S5K5E9_REG_MODE_SELECT		CCI_REG8(0x0100)
#define S5K5E9_MODE_STANDBY		0x00
#define S5K5E9_MODE_STREAMING		0x01
#define S5K5E9_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5K5E9_REG_GROUP_HOLD		CCI_REG8(0x0104)

#define S5K5E9_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K5E9_EXPOSURE_MIN		2
#define S5K5E9_EXPOSURE_STEP		1
#define S5K5E9_EXPOSURE_MARGIN		8

#define S5K5E9_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define S5K5E9_ANA_GAIN_MIN		0x20
#define S5K5E9_ANA_GAIN_MAX		0x200
#define S5K5E9_ANA_GAIN_STEP		1
#define S5K5E9_ANA_GAIN_DEFAULT		0x20

#define S5K5E9_REG_DGAIN_GR		CCI_REG16(0x020e)
#define S5K5E9_REG_DGAIN_R		CCI_REG16(0x0210)
#define S5K5E9_REG_DGAIN_B		CCI_REG16(0x0212)
#define S5K5E9_REG_DGAIN_GB		CCI_REG16(0x0214)
#define S5K5E9_DGTL_GAIN_MIN		0x100
#define S5K5E9_DGTL_GAIN_MAX		0xfff
#define S5K5E9_DGTL_GAIN_STEP		1
#define S5K5E9_DGTL_GAIN_DEFAULT	0x100

#define S5K5E9_REG_FLL			CCI_REG16(0x0340)
#define S5K5E9_FLL_MAX			0xffff

#define S5K5E9_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define S5K5E9_NATIVE_WIDTH		2608U
#define S5K5E9_NATIVE_HEIGHT		1960U

#define S5K5E9_MCLK_FREQ		19200000

/* Output PLL: 19.2 MHz / 3 * 137 = 876.8 Mbps per lane, DDR */
#define S5K5E9_LINK_FREQ		438400000ULL
#define S5K5E9_NUM_LANES		2

/* Video timing pixel clock: 19.2 MHz / 3 * 89 / 3 */
#define S5K5E9_PIXEL_RATE		189866666ULL

struct s5k5e9_mode {
	u32 width;
	u32 height;
	/* Line length in pixel clocks */
	u32 llp;
	/* Default frame length in lines */
	u32 fll_def;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const char * const s5k5e9_supply_names[] = {
	"vio",	/* I/O (1.8 V) */
	"vana",	/* Analog (2.8 V) */
	"vdig",	/* Digital core (1.2 V) */
};

static const s64 s5k5e9_link_freqs[] = {
	S5K5E9_LINK_FREQ,
};

static const char * const s5k5e9_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

static const struct cci_reg_sequence s5k5e9_init_regs[] = {
	{ CCI_REG8(0x3b45), 0x01 },
	{ CCI_REG8(0x0b05), 0x01 },
	{ CCI_REG8(0x392f), 0x01 },
	{ CCI_REG8(0x3930), 0x00 },
	{ CCI_REG8(0x3924), 0x7f },
	{ CCI_REG8(0x3925), 0xfd },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x05 },
	{ CCI_REG8(0x3c31), 0xff },
	{ CCI_REG8(0x3c32), 0xff },
	{ CCI_REG8(0x3290), 0x10 },
	{ CCI_REG8(0x3200), 0x01 },
	{ CCI_REG8(0x3074), 0x06 },
	{ CCI_REG8(0x3075), 0x2f },
	{ CCI_REG8(0x308a), 0x20 },
	{ CCI_REG8(0x308b), 0x08 },
	{ CCI_REG8(0x308c), 0x0b },
	{ CCI_REG8(0x3081), 0x07 },
	{ CCI_REG8(0x307b), 0x85 },
	{ CCI_REG8(0x307a), 0x0a },
	{ CCI_REG8(0x3079), 0x0a },
	{ CCI_REG8(0x306e), 0x71 },
	{ CCI_REG8(0x306f), 0x28 },
	{ CCI_REG8(0x301f), 0x20 },
	{ CCI_REG8(0x3012), 0x4e },
	{ CCI_REG8(0x306b), 0x9a },
	{ CCI_REG8(0x3091), 0x16 },
	{ CCI_REG8(0x30c4), 0x06 },
	{ CCI_REG8(0x306a), 0x79 },
	{ CCI_REG8(0x30b0), 0xff },
	{ CCI_REG8(0x306d), 0x08 },
	{ CCI_REG8(0x3084), 0x16 },
	{ CCI_REG8(0x3070), 0x0f },
	{ CCI_REG8(0x30c2), 0x05 },
	{ CCI_REG8(0x3069), 0x87 },
	{ CCI_REG8(0x3c0f), 0x00 },
	{ CCI_REG8(0x3083), 0x14 },
	{ CCI_REG8(0x3080), 0x08 },
	{ CCI_REG8(0x3c34), 0xea },
	{ CCI_REG8(0x3c35), 0x5c },
	{ CCI_REG8(0x3931), 0x02 },
};

static const struct cci_reg_sequence s5k5e9_mode_2592x1944_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x59 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0x89 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x6c },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x3929), 0x0f },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x27 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x9f },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x98 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x00 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xee },
	{ CCI_REG8(0x0342), 0x0c },
	{ CCI_REG8(0x0343), 0x28 },
	{ CCI_REG8(0x0200), 0x0b },
	{ CCI_REG8(0x0201), 0x9c },
	{ CCI_REG8(0x30b8), 0x2e },
	{ CCI_REG8(0x30ba), 0x36 },
};

static const struct s5k5e9_mode s5k5e9_modes[] = {
	{
		.width = 2592,
		.height = 1944,
		.llp = 3112,
		.fll_def = 2030,
		.crop = {
			.left = 8,
			.top = 8,
			.width = 2592,
			.height = 1944,
		},
		.regs = s5k5e9_mode_2592x1944_regs,
		.num_regs = ARRAY_SIZE(s5k5e9_mode_2592x1944_regs),
	},
};

struct s5k5e9 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(s5k5e9_supply_names)];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	const struct s5k5e9_mode *cur_mode;
};

static inline struct s5k5e9 *to_s5k5e9(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k5e9, sd);
}

static u32 s5k5e9_get_format_code(struct s5k5e9 *s5k5e9)
{
	return MEDIA_BUS_FMT_SGRBG10_1X10;
}

static int s5k5e9_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k5e9 *s5k5e9 = container_of(ctrl->handler, struct s5k5e9,
					     ctrl_handler);
	const struct s5k5e9_mode *mode = s5k5e9->cur_mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = mode->height + ctrl->val - S5K5E9_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(s5k5e9->exposure,
					 s5k5e9->exposure->minimum, max,
					 s5k5e9->exposure->step,
					 min(s5k5e9->exposure->default_value,
					     max));
	}

	if (!pm_runtime_get_if_in_use(s5k5e9->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5k5e9->regmap, S5K5E9_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(s5k5e9->regmap, S5K5E9_REG_DGAIN_GR, ctrl->val, &ret);
		cci_write(s5k5e9->regmap, S5K5E9_REG_DGAIN_R, ctrl->val, &ret);
		cci_write(s5k5e9->regmap, S5K5E9_REG_DGAIN_B, ctrl->val, &ret);
		cci_write(s5k5e9->regmap, S5K5E9_REG_DGAIN_GB, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5k5e9->regmap, S5K5E9_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5k5e9->regmap, S5K5E9_REG_FLL,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5k5e9->regmap, S5K5E9_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s5k5e9->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k5e9_ctrl_ops = {
	.s_ctrl = s5k5e9_set_ctrl,
};

static int s5k5e9_init_controls(struct s5k5e9 *s5k5e9)
{
	struct v4l2_ctrl_handler *hdl = &s5k5e9->ctrl_handler;
	const struct s5k5e9_mode *mode = s5k5e9->cur_mode;
	struct v4l2_fwnode_device_properties props;
	s64 vblank_def, exposure_max, hblank;
	int ret;

	v4l2_ctrl_handler_init(hdl, 12);

	s5k5e9->link_freq = v4l2_ctrl_new_int_menu(hdl, &s5k5e9_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(s5k5e9_link_freqs) - 1,
						   0, s5k5e9_link_freqs);
	if (s5k5e9->link_freq)
		s5k5e9->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k5e9->pixel_rate = v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       S5K5E9_PIXEL_RATE,
					       S5K5E9_PIXEL_RATE, 1,
					       S5K5E9_PIXEL_RATE);

	vblank_def = mode->fll_def - mode->height;
	s5k5e9->vblank = v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_def,
					   S5K5E9_FLL_MAX - mode->height, 1,
					   vblank_def);

	hblank = mode->llp - mode->width;
	s5k5e9->hblank = v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (s5k5e9->hblank)
		s5k5e9->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exposure_max = mode->fll_def - S5K5E9_EXPOSURE_MARGIN;
	s5k5e9->exposure = v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K5E9_EXPOSURE_MIN, exposure_max,
					     S5K5E9_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K5E9_ANA_GAIN_MIN, S5K5E9_ANA_GAIN_MAX,
			  S5K5E9_ANA_GAIN_STEP, S5K5E9_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(hdl, &s5k5e9_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  S5K5E9_DGTL_GAIN_MIN, S5K5E9_DGTL_GAIN_MAX,
			  S5K5E9_DGTL_GAIN_STEP, S5K5E9_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(hdl, &s5k5e9_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k5e9_test_pattern_menu) - 1,
				     0, 0, s5k5e9_test_pattern_menu);

	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(s5k5e9->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &s5k5e9_ctrl_ops, &props);
	if (ret)
		goto err_free;

	s5k5e9->sd.ctrl_handler = hdl;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void s5k5e9_update_pad_format(struct s5k5e9 *s5k5e9,
				     const struct s5k5e9_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = s5k5e9_get_format_code(s5k5e9);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k5e9_start_streaming(struct s5k5e9 *s5k5e9)
{
	const struct s5k5e9_mode *mode = s5k5e9->cur_mode;
	int ret;

	ret = pm_runtime_resume_and_get(s5k5e9->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(s5k5e9->regmap, s5k5e9_init_regs,
				  ARRAY_SIZE(s5k5e9_init_regs), NULL);
	if (ret) {
		dev_err(s5k5e9->dev, "failed to write init settings: %d\n",
			ret);
		goto err_rpm_put;
	}

	ret = cci_multi_reg_write(s5k5e9->regmap, mode->regs, mode->num_regs,
				  NULL);
	if (ret) {
		dev_err(s5k5e9->dev, "failed to set mode: %d\n", ret);
		goto err_rpm_put;
	}

	ret = __v4l2_ctrl_handler_setup(s5k5e9->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(s5k5e9->regmap, S5K5E9_REG_MODE_SELECT,
			S5K5E9_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put(s5k5e9->dev);
	return ret;
}

static int s5k5e9_stop_streaming(struct s5k5e9 *s5k5e9)
{
	int ret;

	ret = cci_write(s5k5e9->regmap, S5K5E9_REG_MODE_SELECT,
			S5K5E9_MODE_STANDBY, NULL);
	if (ret)
		dev_err(s5k5e9->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put(s5k5e9->dev);

	return ret;
}

static int s5k5e9_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable)
		ret = s5k5e9_start_streaming(s5k5e9);
	else
		ret = s5k5e9_stop_streaming(s5k5e9);

	v4l2_subdev_unlock_state(state);

	return ret;
}

static int s5k5e9_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = s5k5e9_get_format_code(s5k5e9);

	return 0;
}

static int s5k5e9_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	if (fse->index >= ARRAY_SIZE(s5k5e9_modes))
		return -EINVAL;

	if (fse->code != s5k5e9_get_format_code(s5k5e9))
		return -EINVAL;

	fse->min_width = s5k5e9_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k5e9_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k5e9_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	const struct s5k5e9_mode *mode;
	s64 hblank, vblank_def, exposure_max;

	mode = v4l2_find_nearest_size(s5k5e9_modes, ARRAY_SIZE(s5k5e9_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);

	s5k5e9_update_pad_format(s5k5e9, mode, &fmt->format);
	*v4l2_subdev_state_get_format(sd_state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(sd_state, 0) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	s5k5e9->cur_mode = mode;

	hblank = mode->llp - mode->width;
	__v4l2_ctrl_modify_range(s5k5e9->hblank, hblank, hblank, 1, hblank);

	vblank_def = mode->fll_def - mode->height;
	__v4l2_ctrl_modify_range(s5k5e9->vblank, vblank_def,
				 S5K5E9_FLL_MAX - mode->height, 1,
				 vblank_def);
	__v4l2_ctrl_s_ctrl(s5k5e9->vblank, vblank_def);

	exposure_max = mode->fll_def - S5K5E9_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(s5k5e9->exposure, S5K5E9_EXPOSURE_MIN,
				 exposure_max, S5K5E9_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int s5k5e9_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K5E9_NATIVE_WIDTH;
		sel->r.height = S5K5E9_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = s5k5e9_modes[0].crop;
		return 0;
	}

	return -EINVAL;
}

static int s5k5e9_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5k5e9_modes[0].width,
			.height = s5k5e9_modes[0].height,
		},
	};

	return s5k5e9_set_pad_format(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k5e9_video_ops = {
	.s_stream = s5k5e9_set_stream,
};

static const struct v4l2_subdev_pad_ops s5k5e9_pad_ops = {
	.enum_mbus_code = s5k5e9_enum_mbus_code,
	.enum_frame_size = s5k5e9_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k5e9_set_pad_format,
	.get_selection = s5k5e9_get_selection,
};

static const struct v4l2_subdev_core_ops s5k5e9_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops s5k5e9_subdev_ops = {
	.core = &s5k5e9_core_ops,
	.video = &s5k5e9_video_ops,
	.pad = &s5k5e9_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k5e9_internal_ops = {
	.init_state = s5k5e9_init_state,
};

static int s5k5e9_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(s5k5e9->supplies),
				    s5k5e9->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}
	usleep_range(1000, 1100);

	ret = clk_prepare_enable(s5k5e9->mclk);
	if (ret) {
		dev_err(dev, "failed to enable mclk: %d\n", ret);
		goto err_disable_regulators;
	}
	usleep_range(1000, 1100);

	gpiod_set_value_cansleep(s5k5e9->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;

err_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(s5k5e9->supplies), s5k5e9->supplies);
	return ret;
}

static int s5k5e9_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	gpiod_set_value_cansleep(s5k5e9->reset_gpio, 1);
	clk_disable_unprepare(s5k5e9->mclk);
	regulator_bulk_disable(ARRAY_SIZE(s5k5e9->supplies), s5k5e9->supplies);

	return 0;
}

static int s5k5e9_identify(struct s5k5e9 *s5k5e9)
{
	u64 id, rev;
	int ret;

	ret = cci_read(s5k5e9->regmap, S5K5E9_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(s5k5e9->dev, ret,
				     "failed to read chip id\n");

	if (id != S5K5E9_CHIP_ID)
		return dev_err_probe(s5k5e9->dev, -ENODEV,
				     "unexpected chip id 0x%04llx\n", id);

	ret = cci_read(s5k5e9->regmap, S5K5E9_REG_REVISION, &rev, NULL);
	if (ret)
		return ret;

	dev_info(s5k5e9->dev, "S5K5E9 revision 0x%02llx\n", rev);

	return 0;
}

static int s5k5e9_check_hwcfg(struct s5k5e9 *s5k5e9)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned long freq_bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(s5k5e9->dev), NULL);
	if (!ep)
		return dev_err_probe(s5k5e9->dev, -ENXIO,
				     "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(s5k5e9->dev, ret,
				     "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K5E9_NUM_LANES) {
		ret = dev_err_probe(s5k5e9->dev, -EINVAL,
				    "only %u data lanes are supported\n",
				    S5K5E9_NUM_LANES);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(s5k5e9->dev,
				       bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k5e9_link_freqs,
				       ARRAY_SIZE(s5k5e9_link_freqs),
				       &freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int s5k5e9_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k5e9 *s5k5e9;
	unsigned int i;
	int ret;

	s5k5e9 = devm_kzalloc(dev, sizeof(*s5k5e9), GFP_KERNEL);
	if (!s5k5e9)
		return -ENOMEM;

	s5k5e9->dev = dev;

	ret = s5k5e9_check_hwcfg(s5k5e9);
	if (ret)
		return ret;

	s5k5e9->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k5e9->regmap))
		return dev_err_probe(dev, PTR_ERR(s5k5e9->regmap),
				     "failed to init CCI\n");

	s5k5e9->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s5k5e9->mclk))
		return dev_err_probe(dev, PTR_ERR(s5k5e9->mclk),
				     "failed to get mclk\n");

	ret = clk_set_rate(s5k5e9->mclk, S5K5E9_MCLK_FREQ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set mclk rate\n");

	s5k5e9->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s5k5e9->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s5k5e9->reset_gpio),
				     "failed to get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(s5k5e9_supply_names); i++)
		s5k5e9->supplies[i].supply = s5k5e9_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s5k5e9->supplies),
				      s5k5e9->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	v4l2_i2c_subdev_init(&s5k5e9->sd, client, &s5k5e9_subdev_ops);

	ret = s5k5e9_power_on(dev);
	if (ret)
		return ret;

	ret = s5k5e9_identify(s5k5e9);
	if (ret)
		goto err_power_off;

	s5k5e9->cur_mode = &s5k5e9_modes[0];

	ret = s5k5e9_init_controls(s5k5e9);
	if (ret)
		goto err_power_off;

	s5k5e9->sd.internal_ops = &s5k5e9_internal_ops;
	s5k5e9->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	s5k5e9->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k5e9->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k5e9->sd.entity, 1, &s5k5e9->pad);
	if (ret)
		goto err_free_ctrls;

	s5k5e9->sd.state_lock = s5k5e9->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s5k5e9->sd);
	if (ret)
		goto err_entity_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&s5k5e9->sd);
	if (ret)
		goto err_rpm_disable;

	pm_runtime_idle(dev);

	return 0;

err_rpm_disable:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&s5k5e9->sd);
err_entity_cleanup:
	media_entity_cleanup(&s5k5e9->sd.entity);
err_free_ctrls:
	v4l2_ctrl_handler_free(&s5k5e9->ctrl_handler);
err_power_off:
	s5k5e9_power_off(dev);
	return ret;
}

static void s5k5e9_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k5e9 *s5k5e9 = to_s5k5e9(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5k5e9->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		s5k5e9_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5k5e9_pm_ops, s5k5e9_power_off,
				 s5k5e9_power_on, NULL);

static const struct of_device_id s5k5e9_of_match[] = {
	{ .compatible = "samsung,s5k5e9" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k5e9_of_match);

static struct i2c_driver s5k5e9_i2c_driver = {
	.driver = {
		.name = "s5k5e9",
		.of_match_table = s5k5e9_of_match,
		.pm = pm_ptr(&s5k5e9_pm_ops),
	},
	.probe = s5k5e9_probe,
	.remove = s5k5e9_remove,
};
module_i2c_driver(s5k5e9_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K5E9 image sensor driver");
MODULE_LICENSE("GPL");
