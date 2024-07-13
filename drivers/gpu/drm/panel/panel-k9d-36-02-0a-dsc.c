// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2024 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct k9d_36_02_0a_dsc {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
};

static inline
struct k9d_36_02_0a_dsc *to_k9d_36_02_0a_dsc(struct drm_panel *panel)
{
	return container_of(panel, struct k9d_36_02_0a_dsc, panel);
}

static void k9d_36_02_0a_dsc_reset(struct k9d_36_02_0a_dsc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
}

static int k9d_36_02_0a_dsc_on(struct k9d_36_02_0a_dsc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xba,
			       0x01, 0xe6, 0x00, 0x10, 0x00, 0x30, 0x00, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xb2, 0x58);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xb2, 0x0c, 0x0c);
	mipi_dsi_dcs_write_seq(dsi, 0xbe, 0x0e, 0x0b, 0x14, 0x13);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq(dsi, 0xbe, 0x8a);
	mipi_dsi_dcs_write_seq(dsi, 0xc0, 0x66);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x08);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x32);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq(dsi, 0xc0, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xc1,
			       0x30, 0x0f, 0x04, 0xc9, 0x0f, 0x81, 0xee, 0xc6,
			       0x3f, 0xfb, 0xb3, 0x6a, 0x3f, 0xf6, 0xd1, 0x42,
			       0x80, 0x00, 0xf7, 0x33, 0xb1, 0x00, 0x18, 0x00,
			       0x00, 0x8b, 0x23, 0x33, 0xc0, 0x0f, 0xb9, 0x0f,
			       0xdd, 0x8d, 0x00, 0x00, 0x00, 0x0d, 0x08, 0x00,
			       0x17, 0x23, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc2,
			       0x38, 0x0f, 0x0b, 0x64, 0x02, 0x11, 0xf6, 0x4c,
			       0x3f, 0xfa, 0xe2, 0x14, 0xff, 0xfe, 0x41, 0xa8,
			       0x00, 0x00, 0x5e, 0x26, 0x90, 0x00, 0x00, 0x24,
			       0x00, 0x17, 0x90, 0x33, 0xc0, 0x09, 0xb4, 0x0f,
			       0x94, 0xe9, 0x00, 0x00, 0x90, 0x0d, 0x3c, 0x90,
			       0x17, 0x57, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc3,
			       0x3c, 0x00, 0x04, 0xc9, 0x0f, 0x81, 0x11, 0x3a,
			       0x3f, 0xf9, 0x58, 0x7c, 0x00, 0x04, 0xf1, 0x78,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x18, 0x3c,
			       0x90, 0x8b, 0x5f, 0x33, 0x60, 0x00, 0x00, 0x0c,
			       0xdd, 0x73, 0x00, 0x00, 0x04, 0x20, 0x08, 0x04,
			       0x2a, 0x23, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc4,
			       0x3c, 0x00, 0x0b, 0x64, 0x02, 0x11, 0x09, 0xb4,
			       0x3f, 0xf6, 0xca, 0x24, 0xc0, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0xcf,
			       0x90, 0x17, 0x3b, 0x33, 0xc0, 0x00, 0x00, 0x0c,
			       0x94, 0x17, 0x00, 0x00, 0x94, 0x20, 0x3c, 0x94,
			       0x2a, 0x57, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc5,
			       0x26, 0x00, 0x04, 0xc9, 0x0f, 0x81, 0x11, 0x3a,
			       0x00, 0x00, 0x00, 0x00, 0x3f, 0xef, 0x14, 0x34,
			       0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xac, 0x00,
			       0x04, 0x1f, 0x23, 0x33, 0xc0, 0x00, 0x00, 0x03,
			       0x23, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc6,
			       0x2e, 0x00, 0x0b, 0x64, 0x02, 0x11, 0x09, 0xb4,
			       0x00, 0x03, 0x11, 0xf4, 0xff, 0xfd, 0x62, 0x7c,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x20, 0x24,
			       0x04, 0x37, 0x90, 0x33, 0xc0, 0x00, 0x00, 0x03,
			       0x6c, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc7,
			       0x2a, 0x0f, 0x04, 0xc9, 0x0f, 0x81, 0xee, 0xc6,
			       0x00, 0x02, 0x5a, 0xee, 0x00, 0x0c, 0xae, 0x86,
			       0x7f, 0xfd, 0xf9, 0xf3, 0x65, 0x93, 0xac, 0x3c,
			       0x94, 0x1f, 0x5f, 0x33, 0x6f, 0xf0, 0x47, 0x00,
			       0x23, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc8,
			       0x2a, 0x0f, 0x0b, 0x64, 0x02, 0x11, 0xf6, 0x4c,
			       0x00, 0x07, 0x29, 0xe4, 0xc0, 0x00, 0xdf, 0x2c,
			       0x7f, 0xff, 0x43, 0xb2, 0xe0, 0x84, 0x20, 0xcf,
			       0x94, 0x37, 0x3b, 0x33, 0xcf, 0xf6, 0x4c, 0x00,
			       0x6c, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc9,
			       0x27, 0x00, 0x03, 0xc1, 0x04, 0x41, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x3f, 0xfe, 0xf8, 0x42,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x24,
			       0x00, 0x84, 0x43, 0x33, 0x90, 0x00, 0x00, 0x03,
			       0x1f, 0xdf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xca,
			       0x21, 0x00, 0x03, 0xc1, 0x04, 0x00, 0x00, 0x00,
			       0x3f, 0xff, 0x0f, 0xc0, 0x3f, 0xff, 0x08, 0x00,
			       0x00, 0x00, 0x0f, 0x04, 0x00, 0x00, 0x42, 0x24,
			       0x00, 0x62, 0x43, 0x33, 0x90, 0x03, 0xe0, 0x0f,
			       0xe1, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xcb,
			       0x2d, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
			       0x3f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x44,
			       0x00, 0x62, 0x64, 0x33, 0x60, 0x00, 0x00, 0x0c,
			       0xe0, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xcc,
			       0x2b, 0x00, 0x04, 0x00, 0x04, 0x41, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0xff, 0xff, 0xee, 0xfc, 0x00, 0x00, 0x63, 0x44,
			       0x00, 0x84, 0x64, 0x33, 0x6f, 0xfb, 0xe0, 0x00,
			       0x20, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xb4, 0xc0);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xb4, 0x00, 0x80, 0x80);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xd2, 0x00, 0x00, 0x11);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq(dsi, 0xd2, 0x05);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq(dsi, 0xd2, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq(dsi, 0xd2, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xce, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x61);
	mipi_dsi_dcs_write_seq(dsi, 0xf3, 0x80);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc0, 0x46);
	mipi_dsi_dcs_write_seq(dsi, 0xbe, 0x0e, 0x0b);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq(dsi, 0xbe, 0x88);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x08);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x32);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x0b);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x33, 0x23, 0x2b);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xd1, 0x07, 0x00, 0x04);
	mipi_dsi_dcs_write_seq(dsi, 0x3b, 0x00, 0x10, 0x00, 0x30);
	mipi_dsi_dcs_write_seq(dsi, 0xd9, 0xc8);
	mipi_dsi_dcs_write_seq(dsi, 0x90, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0x91,
			       0xab, 0x28, 0x00, 0x0c, 0xc2, 0x00, 0x03, 0x1c,
			       0x01, 0x7e, 0x00, 0x0f, 0x08, 0xbb, 0x04, 0x3d,
			       0x10, 0xf0);
	mipi_dsi_dcs_write_seq(dsi, 0x03, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0x51, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x0437);
	if (ret < 0) {
		dev_err(dev, "Failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x095f);
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, 0x2f, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq(dsi, 0xfd, 0x01, 0x5a);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x04);
	mipi_dsi_dcs_write_seq(dsi, 0xfd, 0x5f);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq(dsi, 0xfd, 0x5f);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_MEMORY_START);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xca, 0x12, 0x00, 0x92, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xec, 0x80, 0x10);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xcd, 0x05, 0x31);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x10);
	mipi_dsi_dcs_write_seq(dsi, 0xd8, 0x0c);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x05);
	mipi_dsi_dcs_write_seq(dsi, 0xb3, 0x86, 0x80);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x85, 0x81);
	mipi_dsi_dcs_write_seq(dsi, 0xb7, 0x85, 0x00, 0x00, 0x81);
	mipi_dsi_dcs_write_seq(dsi, 0xb8, 0x05, 0x00, 0x00, 0x81);
	mipi_dsi_dcs_write_seq(dsi, 0xec, 0x0d, 0x11);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xec,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x08);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x32);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x0b);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x33, 0x23, 0x2b);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xce, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xc3,
			       0x94, 0x01, 0x97, 0xd0, 0x22, 0x02, 0x00);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(50);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	usleep_range(16000, 17000);

	return 0;
}

static int k9d_36_02_0a_dsc_off(struct k9d_36_02_0a_dsc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(80);

	return 0;
}

static int k9d_36_02_0a_dsc_prepare(struct drm_panel *panel)
{
	struct k9d_36_02_0a_dsc *ctx = to_k9d_36_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	k9d_36_02_0a_dsc_reset(ctx);

	ret = k9d_36_02_0a_dsc_on(ctx);
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
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	msleep(28); /* TODO: Is this panel-dependent? */

	return 0;
}

static int k9d_36_02_0a_dsc_unprepare(struct drm_panel *panel)
{
	struct k9d_36_02_0a_dsc *ctx = to_k9d_36_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = k9d_36_02_0a_dsc_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode k9d_36_02_0a_dsc_mode = {
	.clock = (1080 + 16 + 8 + 8) * (2400 + 1212 + 4 + 8) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 16,
	.hsync_end = 1080 + 16 + 8,
	.htotal = 1080 + 16 + 8 + 8,
	.vdisplay = 2400,
	.vsync_start = 2400 + 1212,
	.vsync_end = 2400 + 1212 + 4,
	.vtotal = 2400 + 1212 + 4 + 8,
	.width_mm = 68,
	.height_mm = 152,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int k9d_36_02_0a_dsc_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &k9d_36_02_0a_dsc_mode);
}

static const struct drm_panel_funcs k9d_36_02_0a_dsc_panel_funcs = {
	.prepare = k9d_36_02_0a_dsc_prepare,
	.unprepare = k9d_36_02_0a_dsc_unprepare,
	.get_modes = k9d_36_02_0a_dsc_get_modes,
};

static int k9d_36_02_0a_dsc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
// correct values. If not, remove this function.
static int k9d_36_02_0a_dsc_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops k9d_36_02_0a_dsc_bl_ops = {
	.update_status = k9d_36_02_0a_dsc_bl_update_status,
	.get_brightness = k9d_36_02_0a_dsc_bl_get_brightness,
};

static struct backlight_device *
k9d_36_02_0a_dsc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4095,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &k9d_36_02_0a_dsc_bl_ops, &props);
}

static int k9d_36_02_0a_dsc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct k9d_36_02_0a_dsc *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supplies[0].supply = "vdd";
	ctx->supplies[1].supply = "vddio";
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
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &k9d_36_02_0a_dsc_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = k9d_36_02_0a_dsc_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 1 */
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 1080;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1080 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1080 / ctx->dsc.slice_width;
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

static void k9d_36_02_0a_dsc_remove(struct mipi_dsi_device *dsi)
{
	struct k9d_36_02_0a_dsc *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id k9d_36_02_0a_dsc_of_match[] = {
	{ .compatible = "mdss,k9d-36-02-0a-dsc" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, k9d_36_02_0a_dsc_of_match);

static struct mipi_dsi_driver k9d_36_02_0a_dsc_driver = {
	.probe = k9d_36_02_0a_dsc_probe,
	.remove = k9d_36_02_0a_dsc_remove,
	.driver = {
		.name = "panel-k9d-36-02-0a-dsc",
		.of_match_table = k9d_36_02_0a_dsc_of_match,
	},
};
module_mipi_dsi_driver(k9d_36_02_0a_dsc_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for xiaomi 36 02 0a cmd mode dsc dsi panel");
MODULE_LICENSE("GPL");
