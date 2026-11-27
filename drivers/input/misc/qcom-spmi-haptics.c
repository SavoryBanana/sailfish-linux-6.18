// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PMI8994 SPMI Haptics driver (direct LRA/ERM mode via ff-memless)
 */

#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define HAP_PERPH_TYPE_REG		0x04
#define HAP_PERPH_TYPE_HAPTICS		0x15

#define HAP_EN_CTL1_REG			0x46
#define HAP_EN_BIT			BIT(7)

#define HAP_CFG1_REG			0x4c
#define HAP_ACT_TYPE_ERM		BIT(0)

#define HAP_CFG2_REG			0x4d
#define HAP_LRA_AUTO_RES_QWD		0x30

#define HAP_SEL_REG			0x4e
#define HAP_WAVE_SHAPE_SINE		0x00

#define HAP_PLAY_MODE_REG		0x50
#define HAP_PLAY_MODE_DIRECT		0x00

#define HAP_VMAX_CFG_REG		0x51
#define HAP_VMAX_STEP_MV		116
#define HAP_VMAX_MIN_MV			1160
#define HAP_VMAX_MAX_MV			3596
#define HAP_VMAX_MASK			GENMASK(5, 1)
#define HAP_VMAX_SHIFT			1

#define HAP_ILIM_CFG_REG		0x52
#define HAP_ILIM_800MA			0x01

#define HAP_SC_DEB_CFG_REG		0x53
#define HAP_SC_DEB_8_CYCLES		0x03

#define HAP_RATE_CFG1_REG		0x54
#define HAP_RATE_CFG2_REG		0x55

#define HAP_PLAY_REG			0x70
#define HAP_PLAY_EN			BIT(7)

struct qcom_spmi_haptics {
	struct device *dev;
	struct input_dev *input;
	struct regmap *regmap;
	struct work_struct work;
	u32 base;
	u32 vmax_mv;
	u32 play_rate_us;
	u16 magnitude;
	bool active;
};

static void qcom_haptics_set_state(struct qcom_spmi_haptics *hap, u16 mag)
{
	unsigned int vmax_mv, vmax_val;

	if (mag == 0) {
		if (!hap->active)
			return;
		regmap_write(hap->regmap, hap->base + HAP_PLAY_REG, 0);
		regmap_write(hap->regmap, hap->base + HAP_EN_CTL1_REG, 0);
		hap->active = false;
		return;
	}

	vmax_mv = HAP_VMAX_MIN_MV +
		  ((u32)mag * (hap->vmax_mv - HAP_VMAX_MIN_MV)) / 0xffff;
	vmax_mv = clamp_t(unsigned int, vmax_mv, HAP_VMAX_MIN_MV, HAP_VMAX_MAX_MV);
	vmax_val = clamp_t(unsigned int, vmax_mv / HAP_VMAX_STEP_MV, 1, 31);

	regmap_update_bits(hap->regmap, hap->base + HAP_VMAX_CFG_REG,
			   HAP_VMAX_MASK, vmax_val << HAP_VMAX_SHIFT);
	regmap_write(hap->regmap, hap->base + HAP_EN_CTL1_REG, HAP_EN_BIT);
	regmap_write(hap->regmap, hap->base + HAP_PLAY_REG, HAP_PLAY_EN);
	hap->active = true;
}

static void qcom_haptics_work(struct work_struct *work)
{
	struct qcom_spmi_haptics *hap = container_of(work, struct qcom_spmi_haptics, work);

	qcom_haptics_set_state(hap, READ_ONCE(hap->magnitude));
}

static int qcom_haptics_play_effect(struct input_dev *dev, void *data,
				    struct ff_effect *effect)
{
	struct qcom_spmi_haptics *hap = input_get_drvdata(dev);
	u32 level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	WRITE_ONCE(hap->magnitude, (u16)min_t(u32, level, 0xffff));
	schedule_work(&hap->work);
	return 0;
}

static void qcom_haptics_close(struct input_dev *dev)
{
	struct qcom_spmi_haptics *hap = input_get_drvdata(dev);

	WRITE_ONCE(hap->magnitude, 0);
	cancel_work_sync(&hap->work);
	qcom_haptics_set_state(hap, 0);
}

static int qcom_haptics_hw_init(struct qcom_spmi_haptics *hap)
{
	unsigned int perph_type = 0;
	u32 rate_code;
	int ret;

	ret = regmap_read(hap->regmap, hap->base + HAP_PERPH_TYPE_REG, &perph_type);
	if (ret)
		return ret;

	if (perph_type != HAP_PERPH_TYPE_HAPTICS) {
		dev_err(hap->dev, "Unexpected peripheral type 0x%02x (expected 0x%02x)\n",
			perph_type, HAP_PERPH_TYPE_HAPTICS);
		return -ENODEV;
	}

	/* Ensure stopped before configuring */
	regmap_write(hap->regmap, hap->base + HAP_PLAY_REG, 0);
	regmap_write(hap->regmap, hap->base + HAP_EN_CTL1_REG, 0);

	/* LRA, sine wave, QWD auto-resonance, direct play mode */
	regmap_write(hap->regmap, hap->base + HAP_CFG1_REG, 0x00);
	regmap_write(hap->regmap, hap->base + HAP_CFG2_REG, HAP_LRA_AUTO_RES_QWD);
	regmap_write(hap->regmap, hap->base + HAP_SEL_REG, HAP_WAVE_SHAPE_SINE);
	regmap_write(hap->regmap, hap->base + HAP_PLAY_MODE_REG, HAP_PLAY_MODE_DIRECT);
	regmap_write(hap->regmap, hap->base + HAP_ILIM_CFG_REG, HAP_ILIM_800MA);
	regmap_write(hap->regmap, hap->base + HAP_SC_DEB_CFG_REG, HAP_SC_DEB_8_CYCLES);

	rate_code = clamp_t(u32, hap->play_rate_us / 5, 1, 0xfff);
	regmap_write(hap->regmap, hap->base + HAP_RATE_CFG1_REG, rate_code & 0xff);
	regmap_write(hap->regmap, hap->base + HAP_RATE_CFG2_REG, (rate_code >> 8) & 0x0f);

	return 0;
}

static int qcom_haptics_probe(struct platform_device *pdev)
{
	struct qcom_spmi_haptics *hap;
	int ret;

	hap = devm_kzalloc(&pdev->dev, sizeof(*hap), GFP_KERNEL);
	if (!hap)
		return -ENOMEM;

	hap->dev = &pdev->dev;
	hap->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!hap->regmap)
		return -ENODEV;

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &hap->base);
	if (ret)
		return ret;

	hap->vmax_mv = 2552;
	of_property_read_u32(pdev->dev.of_node, "qcom,vmax-mv", &hap->vmax_mv);
	hap->vmax_mv = clamp_t(u32, hap->vmax_mv, HAP_VMAX_MIN_MV, HAP_VMAX_MAX_MV);

	hap->play_rate_us = 5000;
	of_property_read_u32(pdev->dev.of_node, "qcom,wave-play-rate-us", &hap->play_rate_us);

	ret = qcom_haptics_hw_init(hap);
	if (ret)
		return ret;

	INIT_WORK(&hap->work, qcom_haptics_work);

	hap->input = devm_input_allocate_device(&pdev->dev);
	if (!hap->input)
		return -ENOMEM;

	hap->input->name = "qcom-spmi-haptics";
	hap->input->id.bustype = BUS_HOST;
	hap->input->close = qcom_haptics_close;
	input_set_drvdata(hap->input, hap);
	input_set_capability(hap->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(hap->input, NULL, qcom_haptics_play_effect);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, hap);

	ret = input_register_device(hap->input);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "PMI8994 SPMI haptics ready (LRA %u us, vmax %u mV)\n",
		 hap->play_rate_us, hap->vmax_mv);
	return 0;
}

static void qcom_haptics_remove(struct platform_device *pdev)
{
	struct qcom_spmi_haptics *hap = platform_get_drvdata(pdev);

	WRITE_ONCE(hap->magnitude, 0);
	cancel_work_sync(&hap->work);
	qcom_haptics_set_state(hap, 0);
}

static int qcom_haptics_suspend(struct device *dev)
{
	struct qcom_spmi_haptics *hap = dev_get_drvdata(dev);

	WRITE_ONCE(hap->magnitude, 0);
	cancel_work_sync(&hap->work);
	qcom_haptics_set_state(hap, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qcom_haptics_pm_ops, qcom_haptics_suspend, NULL);

static const struct of_device_id qcom_haptics_of_match[] = {
	{ .compatible = "qcom,pmi8994-haptics" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_haptics_of_match);

static struct platform_driver qcom_haptics_driver = {
	.probe = qcom_haptics_probe,
	.remove = qcom_haptics_remove,
	.driver = {
		.name = "qcom-spmi-haptics",
		.pm = pm_sleep_ptr(&qcom_haptics_pm_ops),
		.of_match_table = qcom_haptics_of_match,
	},
};
module_platform_driver(qcom_haptics_driver);

MODULE_DESCRIPTION("Qualcomm PMI8994 SPMI Haptics driver");
MODULE_LICENSE("GPL");
