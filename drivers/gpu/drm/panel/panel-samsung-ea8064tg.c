// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung EA8064TG 5.0" 1080x1920 AMOLED DSI Command-Mode Panel Driver
 * Found on Google Pixel 1st Gen (sailfish).
 *
 * Copyright (c) 2026 SavoryBanana / Google Pixel Mainline Project
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct ea8064tg_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
};

static inline struct ea8064tg_panel *to_ea8064tg_panel(struct drm_panel *panel)
{
	return container_of(panel, struct ea8064tg_panel, panel);
}

static void ea8064tg_reset(struct ea8064tg_panel *ctx)
{
	/* Hardware reset sequence: High 20ms, Low 20ms, High 20ms */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
}

static int ea8064tg_on(struct ea8064tg_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* CMD2 Page 4: VESA DSC setting */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x03);

	/* CMD2 Page 7: SubPixel Rendering (SPR) Mode: YYG Rainbow-RGB */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xef, 0x01);

	/* CMD2 Page 0: Display Output Control: SRAM Enable */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	/* Data Compression Mode: None (0x00) & VESA DSC Off (0x00) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x03, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4, 0x01);

	/* Set Tear Effect ON (mode 0 = VBLANK) */
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	/* CMD2 Page 1: Novatek manufacturer key sequence */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x00);

	/* 0x11: Exit sleep mode, delay 120ms */
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	/* 0x29: Display ON, delay 120ms */
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	/* 0x53: Write control display (0x20 = brightness via DCS) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	/* 0x51: Write initial display brightness (180/255) */
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 180);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	if (dsi_ctx.accum_err)
		pr_err("panel-samsung-ea8064tg: ea8064tg_on failed: %d\n", dsi_ctx.accum_err);
	else
		pr_info("panel-samsung-ea8064tg: ea8064tg_on success\n");

	return dsi_ctx.accum_err;
}

static int ea8064tg_off(struct ea8064tg_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* 0x28: Display off, delay 120ms */
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	/* 0x10: Enter sleep mode, delay 120ms */
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return dsi_ctx.accum_err;
}

static int ea8064tg_prepare(struct drm_panel *panel)
{
	struct ea8064tg_panel *ctx = to_ea8064tg_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable supplies: %d\n", ret);
		return ret;
	}

	ea8064tg_reset(ctx);

	ret = ea8064tg_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	return 0;
}

static int ea8064tg_unprepare(struct drm_panel *panel)
{
	struct ea8064tg_panel *ctx = to_ea8064tg_panel(panel);

	ea8064tg_off(ctx);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode ea8064tg_panel_mode = {
	.clock = 130932,
	.hdisplay = 1080,
	.hsync_start = 1080 + 18,
	.hsync_end = 1080 + 18 + 10,
	.htotal = 1080 + 18 + 10 + 18,
	.vdisplay = 1920,
	.vsync_start = 1920 + 5,
	.vsync_end = 1920 + 5 + 2,
	.vtotal = 1920 + 5 + 2 + 11,
	.width_mm = 62,
	.height_mm = 110,
	.flags = 0,
};

static int ea8064tg_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ea8064tg_panel_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ea8064tg_panel_funcs = {
	.prepare = ea8064tg_prepare,
	.unprepare = ea8064tg_unprepare,
	.get_modes = ea8064tg_get_modes,
};

static int ea8064tg_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u8 brightness = (u8)backlight_get_brightness(bl);

	return mipi_dsi_dcs_set_display_brightness(dsi, brightness);
}

static const struct backlight_ops ea8064tg_bl_ops = {
	.update_status = ea8064tg_bl_update_status,
};

static struct backlight_device *ea8064tg_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = 180,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &ea8064tg_bl_ops, &props);
}

static int ea8064tg_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ea8064tg_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ea8064tg_panel, panel,
				   &ea8064tg_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get power supplies\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	/* DCS commands in prepare() need the DSI host powered up first */
	ctx->panel.prepare_prev_first = true;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.backlight = ea8064tg_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight device\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	dev_info(dev, "Samsung EA8064TG AMOLED panel registered\n");
	return 0;
}

static void ea8064tg_remove(struct mipi_dsi_device *dsi)
{
	struct ea8064tg_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ea8064tg_of_match[] = {
	{ .compatible = "samsung,ea8064tg" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ea8064tg_of_match);

static struct mipi_dsi_driver ea8064tg_driver = {
	.probe = ea8064tg_probe,
	.remove = ea8064tg_remove,
	.driver = {
		.name = "panel-samsung-ea8064tg",
		.of_match_table = ea8064tg_of_match,
	},
};

module_mipi_dsi_driver(ea8064tg_driver);

MODULE_AUTHOR("SavoryBanana / Google Pixel Mainline Project");
MODULE_DESCRIPTION("DRM Panel Driver for Samsung EA8064TG AMOLED (Google Pixel 1 / sailfish)");
MODULE_LICENSE("GPL v2");

