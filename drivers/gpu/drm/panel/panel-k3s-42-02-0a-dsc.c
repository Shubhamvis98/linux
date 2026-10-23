// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Mi 11T Pro (vili) k3s 42 02 0a DSC command mode panel
 *
 * Copyright (c) 2026 Shubham Vishwakarma <shubhamvis98@fossfrog.in>
 *
 * Generated with linux-mdss-dsi-panel-driver-generator from the vendor
 * device tree:
 *   Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/*
 * Stock sends every on-command as a DCS long write (type 0x39), even 1-2 byte
 * ones like "11 00" / "29 00". mipi_dsi_dcs_write_buffer() would turn those
 * into short writes (0x05/0x15), so build the long packet explicitly.
 */
static ssize_t k3s_dcs_long_write(struct mipi_dsi_device *dsi,
				  const u8 *data, size_t len)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_LONG_WRITE,
		.tx_buf = data,
		.tx_len = len,
	};

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	return dsi->host->ops->transfer(dsi->host, &msg);
}

/*
 * Send a long-write sequence unless an earlier command failed. *err keeps the
 * first error, so a whole init sequence can be written without checking every
 * step. Driver-local because mipi_dsi_dcs_write_seq_multi() would send 1-2
 * byte commands as short writes.
 */
#define k3s_write_seq(dsi, err, seq...)					\
	do {								\
		static const u8 d[] = { seq };				\
		int *__e = (err);					\
		ssize_t __r;						\
		if (!*__e) {						\
			__r = k3s_dcs_long_write(dsi, d, ARRAY_SIZE(d));\
			if (__r < 0)					\
				*__e = __r;				\
		}							\
	} while (0)

struct k3s_42_02_0a_dsc {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_connector *connector;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[4];
};

/* Order and loads match stock qcom,panel-supply-entries (dsi_panel_pwr_supply_k3s) */
/* avdd: 5.5V AMOLED analog supply (stock display_panel_avdd, tlmm 12) */
static const char * const k3s_supply_names[] = { "vddio", "dvdd", "vdd", "avdd" };
static const int k3s_supply_loads[] = { 60700, 60700, 10000, 0 };

static inline
struct k3s_42_02_0a_dsc *to_k3s_42_02_0a_dsc(struct drm_panel *panel)
{
	return container_of(panel, struct k3s_42_02_0a_dsc, panel);
}

static void k3s_42_02_0a_dsc_reset(struct k3s_42_02_0a_dsc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	/*
	 * Stock waits only 10ms, but here commands sent that early are
	 * silently dropped (sleep-out never sticks: 0Ah stays 0x08/0x0C).
	 * The same sequence sent later works, so give the DDIC time to
	 * finish its post-reset OTP load.
	 */
	msleep(120);
}

/*
 * Timings from the vendor DT: timing@1 (120 Hz) and timing@0 (60 Hz). Only
 * the horizontal porches differ; the panel is told the rate via DCS 0x2f.
 */
#define K3S_MODE(hfp, hbp, fps) {					\
	.clock = (1080 + (hfp) + 2 + (hbp)) * (2400 + 28 + 4 + 20) * (fps) / 1000, \
	.hdisplay = 1080,						\
	.hsync_start = 1080 + (hfp),					\
	.hsync_end = 1080 + (hfp) + 2,					\
	.htotal = 1080 + (hfp) + 2 + (hbp),				\
	.vdisplay = 2400,						\
	.vsync_start = 2400 + 28,					\
	.vsync_end = 2400 + 28 + 4,					\
	.vtotal = 2400 + 28 + 4 + 20,					\
	.width_mm = 70,							\
	.height_mm = 154,						\
	.type = DRM_MODE_TYPE_DRIVER,					\
}

static const struct {
	struct drm_display_mode mode;
	u8 rate_sel;		/* DCS 0x2f argument */
} k3s_42_02_0a_dsc_modes[] = {
	{ .mode = K3S_MODE(133, 130, 120), .rate_sel = 0x02 },	/* preferred */
	{ .mode = K3S_MODE(448, 440, 60),  .rate_sel = 0x01 },
};

/* Index of the mode the CRTC is being enabled with (0 if unknown). */
static int k3s_42_02_0a_dsc_cur_mode(struct k3s_42_02_0a_dsc *ctx)
{
	struct drm_connector *connector = ctx->connector;
	struct drm_crtc_state *crtc_state;
	int i;

	if (!connector || !connector->state || !connector->state->crtc)
		return 0;

	crtc_state = connector->state->crtc->state;
	for (i = 0; i < ARRAY_SIZE(k3s_42_02_0a_dsc_modes); i++)
		if (drm_mode_match(&crtc_state->mode,
				   &k3s_42_02_0a_dsc_modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK))
			return i;

	return 0;
}

static int k3s_42_02_0a_dsc_on(struct k3s_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	u8 rate_sel = k3s_42_02_0a_dsc_modes[k3s_42_02_0a_dsc_cur_mode(ctx)].rate_sel;
	int err = 0;

	k3s_write_seq(dsi, &err, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	k3s_write_seq(dsi, &err, 0x6f, 0x05);
	k3s_write_seq(dsi, &err, 0xbe, 0x08);
	/*
	 * Novatek DSC setup. 0x90: bit0 DSC on, bit1 two slices per line
	 * (stock k3s 1x1080 = 0x01; downstream m20 2x540 = 0x03).
	 * 0x91 mirrors the RC parameters (stock bytes match drm_dsc for a
	 * 1080px slice exactly), so derive it from the config the msm DSI
	 * host computed.
	 */
	if (!err) {
		const struct drm_dsc_config *d = &ctx->dsc;
		u8 c90[] = { 0x90, d->slice_count > 1 ? 0x03 : 0x01 };
		u8 c91[] = { 0x91,
			(d->bits_per_component << 4) | d->line_buf_depth,
			d->bits_per_component == 10 ? 0xa8 : 0x28,
			d->slice_height >> 8, d->slice_height & 0xff,
			0xc2, 0x00,
			d->initial_dec_delay >> 8, d->initial_dec_delay & 0xff,
			d->scale_increment_interval >> 8,
			d->scale_increment_interval & 0xff,
			d->scale_decrement_interval >> 8,
			d->scale_decrement_interval & 0xff,
			d->nfl_bpg_offset >> 8, d->nfl_bpg_offset & 0xff,
			d->slice_bpg_offset >> 8, d->slice_bpg_offset & 0xff,
			d->final_offset >> 8, d->final_offset & 0xff };
		ssize_t r;

		r = k3s_dcs_long_write(dsi, c90, sizeof(c90));
		if (r >= 0)
			r = k3s_dcs_long_write(dsi, c91, sizeof(c91));
		if (r < 0)
			err = r;
	}
	k3s_write_seq(dsi, &err, 0x03, 0x01);
	k3s_write_seq(dsi, &err, MIPI_DCS_WRITE_MEMORY_START, 0x00);
	k3s_write_seq(dsi, &err, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x07);
	k3s_write_seq(dsi, &err, 0xc0, 0x01, 0x02);
	if (!err)
		err = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (!err)
		err = mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x0437);
	if (!err)
		err = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x095f);
	k3s_write_seq(dsi, &err, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x28);
	k3s_write_seq(dsi, &err, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	k3s_write_seq(dsi, &err, 0x6f, 0x02);
	k3s_write_seq(dsi, &err, 0xb2, 0x08, 0x08);
	/* frame rate select: 0x01 = 60 Hz, 0x02 = 120 Hz */
	if (rate_sel == 0x02)
		k3s_write_seq(dsi, &err, 0x2f, 0x02);
	else
		k3s_write_seq(dsi, &err, 0x2f, 0x01);
	k3s_write_seq(dsi, &err, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	k3s_write_seq(dsi, &err, 0xc3, 0x9b, 0x01, 0x94, 0xd0, 0x22, 0x02, 0x00);
	k3s_write_seq(dsi, &err, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	k3s_write_seq(dsi, &err, 0x6f, 0x06);
	k3s_write_seq(dsi, &err, 0xb5, 0x2b, 0x0c, 0x32);
	k3s_write_seq(dsi, &err, 0x11, 0x00);
	if (!err)
		msleep(120);
	k3s_write_seq(dsi, &err, 0x29, 0x00);

	return err;
}

static int k3s_42_02_0a_dsc_off(struct k3s_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	int err = 0;

	k3s_write_seq(dsi, &err, 0x28, 0x00);
	k3s_write_seq(dsi, &err, 0x10, 0x00);
	if (!err)
		msleep(100);

	return err;
}

static int k3s_42_02_0a_dsc_prepare(struct drm_panel *panel)
{
	struct k3s_42_02_0a_dsc *ctx = to_k3s_42_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret, i;

	/* Stock: each supply has qcom,supply-post-on-sleep = 10ms */
	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_enable(ctx->supplies[i].consumer);
		if (ret < 0) {
			dev_err(dev, "Failed to enable %s: %d\n",
				ctx->supplies[i].supply, ret);
			while (--i >= 0)
				regulator_disable(ctx->supplies[i].consumer);
			return ret;
		}
		usleep_range(10000, 11000);
	}

	k3s_42_02_0a_dsc_reset(ctx);

	ret = k3s_42_02_0a_dsc_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}

	/* Stock has no qcom,mdss-pps-delay-ms, i.e. no delay after the PPS */

	/* Stock normal-mode max is 0x7ff (hbm-off: 51 07 FF), sent in LP */
	{
		static const u8 bl_init[] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					      0x07, 0xff };
		ret = k3s_dcs_long_write(ctx->dsi, bl_init, sizeof(bl_init));
		if (ret < 0)
			dev_err(dev, "initial brightness failed: %d\n", ret);
	}

	return 0;
}

static int k3s_42_02_0a_dsc_unprepare(struct drm_panel *panel)
{
	struct k3s_42_02_0a_dsc *ctx = to_k3s_42_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = k3s_42_02_0a_dsc_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int k3s_42_02_0a_dsc_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	struct k3s_42_02_0a_dsc *ctx = to_k3s_42_02_0a_dsc(panel);
	struct drm_display_mode *mode;
	int i;

	for (i = 0; i < ARRAY_SIZE(k3s_42_02_0a_dsc_modes); i++) {
		mode = drm_mode_duplicate(connector->dev,
					  &k3s_42_02_0a_dsc_modes[i].mode);
		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = k3s_42_02_0a_dsc_modes[0].mode.width_mm;
	connector->display_info.height_mm = k3s_42_02_0a_dsc_modes[0].mode.height_mm;
	ctx->connector = connector;

	return i;
}

static const struct drm_panel_funcs k3s_42_02_0a_dsc_panel_funcs = {
	.prepare = k3s_42_02_0a_dsc_prepare,
	.unprepare = k3s_42_02_0a_dsc_unprepare,
	.get_modes = k3s_42_02_0a_dsc_get_modes,
};

static int k3s_42_02_0a_dsc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	u8 buf[3] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
		      brightness >> 8, brightness & 0xff };
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_LONG_WRITE,
		.tx_buf = buf,
		.tx_len = sizeof(buf),
	};
	ssize_t ret;

	/*
	 * Big-endian DBV (stock bl-inverted-dbv). Stock sends brightness
	 * updates in HS mode (no qcom,mdss-dsi-bl-lp-mode); the panel ignores
	 * them in LP mode while it is displaying.
	 */
	ret = dsi->host->ops->transfer(dsi->host, &msg);

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops k3s_42_02_0a_dsc_bl_ops = {
	.update_status = k3s_42_02_0a_dsc_bl_update_status,
};

static struct backlight_device *
k3s_42_02_0a_dsc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2047,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &k3s_42_02_0a_dsc_bl_ops, &props);
}

static int k3s_42_02_0a_dsc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct k3s_42_02_0a_dsc *ctx;
	int ret, i;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ctx->supplies[i].supply = k3s_supply_names[i];
		ctx->supplies[i].init_load_uA = k3s_supply_loads[i];
	}
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	/* Command mode panel (stock: dsi_cmd_mode), driven with DSC */
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &k3s_42_02_0a_dsc_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = k3s_42_02_0a_dsc_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 2;

	/*
	 * Stock uses slice_per_pkt = 2, but with a single 1080px slice per
	 * line that is clamped to 1 anyway; leave dsi->slice_per_pkt at 1.
	 */
	ctx->dsc.slice_height = 12;
	/*
	 * Stock: one slice spanning the full line (single DSC, 1:1:1
	 * topology). All modes share the same active area.
	 */
	ctx->dsc.slice_width = k3s_42_02_0a_dsc_modes[0].mode.hdisplay;
	ctx->dsc.slice_count = k3s_42_02_0a_dsc_modes[0].mode.hdisplay /
			       ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void k3s_42_02_0a_dsc_remove(struct mipi_dsi_device *dsi)
{
	struct k3s_42_02_0a_dsc *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id k3s_42_02_0a_dsc_of_match[] = {
	{ .compatible = "xiaomi,k3s-42-02-0a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, k3s_42_02_0a_dsc_of_match);

static struct mipi_dsi_driver k3s_42_02_0a_dsc_driver = {
	.probe = k3s_42_02_0a_dsc_probe,
	.remove = k3s_42_02_0a_dsc_remove,
	.driver = {
		.name = "panel-k3s-42-02-0a-dsc",
		.of_match_table = k3s_42_02_0a_dsc_of_match,
	},
};
module_mipi_dsi_driver(k3s_42_02_0a_dsc_driver);

MODULE_AUTHOR("Shubham Vishwakarma <shubhamvis98@fossfrog.in>");
MODULE_DESCRIPTION("DRM driver for Xiaomi k3s 42 02 0a DSC command mode panel");
MODULE_LICENSE("GPL");
