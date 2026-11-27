// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Qualcomm PMI8994 SPMI 2-channel Torch/Flash LED driver (0xd300).
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define PMI8994_FLASH_PERPH_TYPE	0x04
#define PMI8994_FLASH_PERPH_SUBTYPE	0x05
#define PMI8994_FLASH_LED1_CURR		0x42
#define PMI8994_FLASH_LED2_CURR		0x43
#define PMI8994_FLASH_HEADROOM		0x45
#define PMI8994_FLASH_ENABLE_CTRL	0x46
#define PMI8994_FLASH_STROBE_CTRL	0x47
#define PMI8994_FLASH_TMR_CTRL		0x48
#define PMI8994_FLASH_CLAMP_CURR	0x4a
#define PMI8994_FLASH_SEC_ACCESS	0xd0
#define PMI8994_FLASH_TORCH_CTRL	0xe4

#define PMI8994_FLASH_TYPE_VAL		0x18
#define PMI8994_FLASH_ENABLE_MOD	0x80
#define PMI8994_FLASH_ENABLE_LED1	0x40
#define PMI8994_FLASH_ENABLE_LED2	0x20
#define PMI8994_FLASH_STROBE_SW_LED1	0x80
#define PMI8994_FLASH_STROBE_SW_LED2	0x40
#define PMI8994_FLASH_SEC_MAGIC		0xa5
#define PMI8994_FLASH_TORCH_EN		0x80

/* 12.5 mA per step; cap torch at 200 mA (step 15) */
#define PMI8994_TORCH_MAX_STEP		15

struct pmi8994_flash;

struct pmi8994_torch_led {
	struct led_classdev cdev;
	struct pmi8994_flash *parent;
	unsigned int ch; /* 0 = LED1, 1 = LED2 */
	u8 brightness;
};

struct pmi8994_flash {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	struct mutex lock;
	struct pmi8994_torch_led leds[2];
};

static int pmi8994_flash_apply(struct pmi8994_flash *fl)
{
	u8 b0 = fl->leds[0].brightness;
	u8 b1 = fl->leds[1].brightness;
	u8 en = 0, strobe = 0;
	int ret;

	if (b0 == 0 && b1 == 0) {
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_STROBE_CTRL, 0x00);
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_ENABLE_CTRL, 0x00);
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_SEC_ACCESS, PMI8994_FLASH_SEC_MAGIC);
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_TORCH_CTRL, 0x00);
		return 0;
	}

	/* Configure safe torch defaults */
	regmap_write(fl->regmap, fl->base + PMI8994_FLASH_HEADROOM, 0x03);
	regmap_write(fl->regmap, fl->base + PMI8994_FLASH_TMR_CTRL, 0x00);
	regmap_write(fl->regmap, fl->base + PMI8994_FLASH_CLAMP_CURR, PMI8994_TORCH_MAX_STEP);

	if (b0 > 0) {
		u8 step = min_t(u8, b0, PMI8994_TORCH_MAX_STEP) - 1;
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_LED1_CURR, step);
		en |= PMI8994_FLASH_ENABLE_LED1;
		strobe |= PMI8994_FLASH_STROBE_SW_LED1;
	} else {
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_LED1_CURR, 0);
	}

	if (b1 > 0) {
		u8 step = min_t(u8, b1, PMI8994_TORCH_MAX_STEP) - 1;
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_LED2_CURR, step);
		en |= PMI8994_FLASH_ENABLE_LED2;
		strobe |= PMI8994_FLASH_STROBE_SW_LED2;
	} else {
		regmap_write(fl->regmap, fl->base + PMI8994_FLASH_LED2_CURR, 0);
	}

	ret = regmap_write(fl->regmap, fl->base + PMI8994_FLASH_SEC_ACCESS, PMI8994_FLASH_SEC_MAGIC);
	if (ret)
		return ret;
	ret = regmap_write(fl->regmap, fl->base + PMI8994_FLASH_TORCH_CTRL, PMI8994_FLASH_TORCH_EN);
	if (ret)
		return ret;
	ret = regmap_write(fl->regmap, fl->base + PMI8994_FLASH_ENABLE_CTRL,
			   PMI8994_FLASH_ENABLE_MOD | en);
	if (ret)
		return ret;
	return regmap_write(fl->regmap, fl->base + PMI8994_FLASH_STROBE_CTRL, strobe);
}

static int pmi8994_torch_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct pmi8994_torch_led *led = container_of(cdev, struct pmi8994_torch_led, cdev);
	struct pmi8994_flash *fl = led->parent;
	int ret;

	mutex_lock(&fl->lock);
	led->brightness = min_t(unsigned int, brightness, PMI8994_TORCH_MAX_STEP);
	ret = pmi8994_flash_apply(fl);
	mutex_unlock(&fl->lock);
	return ret;
}

static int pmi8994_flash_probe(struct platform_device *pdev)
{
	struct pmi8994_flash *fl;
	unsigned int type = 0, subtype = 0;
	u32 base = 0;
	int ret, i;
	static const char *const names[2] = { "led:torch_0", "led:torch_1" };

	fl = devm_kzalloc(&pdev->dev, sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;

	fl->dev = &pdev->dev;
	fl->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!fl->regmap)
		return -ENODEV;

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &base);
	if (ret)
		return ret;
	fl->base = base;
	mutex_init(&fl->lock);

	ret = regmap_read(fl->regmap, fl->base + PMI8994_FLASH_PERPH_TYPE, &type);
	if (ret)
		return ret;
	regmap_read(fl->regmap, fl->base + PMI8994_FLASH_PERPH_SUBTYPE, &subtype);
	if (type != PMI8994_FLASH_TYPE_VAL) {
		dev_err(&pdev->dev, "unexpected flash peripheral type 0x%02x (subtype 0x%02x)\n",
			type, subtype);
		return -ENODEV;
	}

	/* Ensure LEDs start off */
	pmi8994_flash_apply(fl);

	for (i = 0; i < 2; i++) {
		fl->leds[i].parent = fl;
		fl->leds[i].ch = i;
		fl->leds[i].cdev.name = names[i];
		fl->leds[i].cdev.max_brightness = PMI8994_TORCH_MAX_STEP;
		fl->leds[i].cdev.brightness_set_blocking = pmi8994_torch_brightness_set;
		ret = devm_led_classdev_register(&pdev->dev, &fl->leds[i].cdev);
		if (ret)
			return ret;
	}

	dev_info(&pdev->dev, "PMI8994 torch LED ready at 0x%x (type=0x%02x subtype=0x%02x)\n",
		 fl->base, type, subtype);
	return 0;
}

static const struct of_device_id pmi8994_flash_of_match[] = {
	{ .compatible = "qcom,pmi8994-flash-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmi8994_flash_of_match);

static struct platform_driver pmi8994_flash_driver = {
	.probe = pmi8994_flash_probe,
	.driver = {
		.name = "qcom-pmi8994-flash-led",
		.of_match_table = pmi8994_flash_of_match,
	},
};
module_platform_driver(pmi8994_flash_driver);

MODULE_DESCRIPTION("Qualcomm PMI8994 SPMI Torch/Flash LED Driver");
MODULE_LICENSE("GPL");
