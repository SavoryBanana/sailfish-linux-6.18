// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2014, Sony Mobile Communications Inc.
 *
 * This driver is for the multi-block Switch-Mode Battery Charger and Boost
 * (SMBB) hardware, found in Qualcomm PM8941 PMICs.  The charger is an
 * integrated, single-cell lithium-ion battery charger.
 *
 * Sub-components:
 *  - Charger core
 *  - Buck
 *  - DC charge-path
 *  - USB charge-path
 *  - Battery interface
 *  - Boost (not implemented)
 *  - Misc
 *  - HF-Buck
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/extcon-provider.h>
#include <linux/regulator/driver.h>

#define SMBB_CHG_VMAX		0x040
#define SMBB_CHG_VSAFE		0x041
#define SMBB_CHG_CFG		0x043
#define SMBB_CHG_IMAX		0x044
#define SMBB_CHG_ISAFE		0x045
#define SMBB_CHG_VIN_MIN	0x047
#define SMBB_CHG_CTRL		0x049
#define CTRL_EN			BIT(7)
#define SMBB_CHG_VBAT_WEAK	0x052
#define SMBB_CHG_IBAT_TERM_CHG	0x05b
#define IBAT_TERM_CHG_IEOC	BIT(7)
#define IBAT_TERM_CHG_IEOC_BMS	BIT(7)
#define IBAT_TERM_CHG_IEOC_CHG	0
#define SMBB_CHG_VBAT_DET	0x05d
#define SMBB_CHG_TCHG_MAX_EN	0x060
#define TCHG_MAX_EN		BIT(7)
#define SMBB_CHG_WDOG_TIME	0x062
#define SMBB_CHG_WDOG_EN	0x065
#define WDOG_EN			BIT(7)

#define SMBB_BUCK_REG_MODE	0x174
#define BUCK_REG_MODE		BIT(0)
#define BUCK_REG_MODE_VBAT	BIT(0)
#define BUCK_REG_MODE_VSYS	0

#define SMBB_BAT_PRES_STATUS	0x208
#define PRES_STATUS_BAT_PRES	BIT(7)
#define SMBB_BAT_TEMP_STATUS	0x209
#define TEMP_STATUS_OK		BIT(7)
#define TEMP_STATUS_HOT		BIT(6)
#define SMBB_BAT_BTC_CTRL	0x249
#define BTC_CTRL_COMP_EN	BIT(7)
#define BTC_CTRL_COLD_EXT	BIT(1)
#define BTC_CTRL_HOT_EXT_N	BIT(0)

#define SMBB_USB_IMAX		0x344
#define SMBB_USB_OTG_CTL	0x348
#define OTG_CTL_EN		BIT(0)
#define SMBB_USB_ENUM_TIMER_STOP 0x34e
#define ENUM_TIMER_STOP		BIT(0)
#define SMBB_USB_SEC_ACCESS	0x3d0
#define SEC_ACCESS_MAGIC	0xa5
#define SMBB_USB_REV_BST	0x3ed
#define REV_BST_CHG_GONE	BIT(7)

#define SMBB_DC_IMAX		0x444

#define SMBB_MISC_REV2		0x601
#define SMBB_MISC_BOOT_DONE	0x642
#define BOOT_DONE		BIT(7)

/* PMI8994 / PMI8996 Switch-Mode Battery Charger (SMBCHG) registers */
#define SMBCHG_CHGR_STS			0x00e
#define SMBCHG_CHGR_TYPE_MASK		GENMASK(2, 1)
#define SMBCHG_CHGR_HOLD_OFF		BIT(3)
#define SMBCHG_CHGR_BATT_LT_VFLOAT	BIT(7)
#define SMBCHG_CHGR_RT_STS		0x010
#define SMBCHG_CHGR_INHIBIT		BIT(1)
#define SMBCHG_CHGR_COMP_SFT		BIT(3)
#define SMBCHG_CHGR_RT_TCC_REACHED	BIT(7)
#define SMBCHG_CHGR_CMD_CHG		0x042
#define SMBCHG_CMD_CHG_EN_N		BIT(1)
#define SMBCHG_CHGR_CFG2		0x0f1
#define SMBCHG_CHGR_FCC_CFG		0x0f2
#define SMBCHG_FCC_MASK			GENMASK(4, 0)
#define SMBCHG_CHGR_VFLOAT_CFG		0x0f4
#define SMBCHG_VFLOAT_MASK		GENMASK(5, 0)
#define SMBCHG_CHGR_CFG2_FC		0x0fc
#define SMBCHG_CHGR_CFG2_CHG_EN_SRC	BIT(7)
#define SMBCHG_CHGR_CFG2_CHG_EN_POL	BIT(6)
#define SMBCHG_CHGR_CFG2_I_TERM		BIT(3)
#define SMBCHG_CHGR_CFG2_AUTO_RECHG	BIT(2)
#define SMBCHG_CHGR_CFG2_CHG_INHIBIT	BIT(1)
#define SMBCHG_CHGR_SFT_CFG		0x0fd
#define SMBCHG_OTG_RT_STS		0x110
#define SMBCHG_OTG_CMD			0x140
#define SMBCHG_BAT_IF_RT_STS		0x210
#define SMBCHG_BAT_IF_CMD_CHG		0x242
#define SMBCHG_BAT_IF_BM_CFG		0x2f3
#define SMBCHG_USB_APSD_DG_STS		0x308
#define SMBCHG_TYPEC_STS_1		0x30b
#define SMBCHG_TYPEC_STS_2		0x30e
#define SMBCHG_USB_CHGPTH_CFG		0x3f1
#define SMBCHG_USB_CHGPTH_CFG_F4	0x3f4
#define SMBCHG_CFG_USB_2_3_SEL		BIT(7)
#define SMBCHG_MISC_IDEV_STS		0x608
#define SMBCHG_MISC_WDOG_CFG		0x6f5
#define SMBCHG_WDOG_EN			BIT(0)
#define SMBCHG_FG_SOC_OFF		0x3009
#define SMBCHG_FG_ADC_VBAT_OFF		0x3254
#define SMBCHG_FG_ADC_IBAT_OFF		0x3255

#define SMBCHG_HOT_BAT_HARD		BIT(0)
#define SMBCHG_HOT_BAT_SOFT		BIT(1)
#define SMBCHG_COLD_BAT_HARD		BIT(2)
#define SMBCHG_COLD_BAT_SOFT		BIT(3)
#define SMBCHG_BAT_OV			BIT(4)
#define SMBCHG_BAT_MISSING		BIT(6)

#define SMBCHG_USB_ICL_STS_1		0x307
#define SMBCHG_ICL_STS_MASK		GENMASK(4, 0)
#define SMBCHG_USB_ICL_STS_2		0x309
#define SMBCHG_USBIN_SUSPEND_STS	BIT(3)
#define SMBCHG_USBIN_RT_STS		0x310
#define SMBCHG_USBIN_UV			BIT(0)
#define SMBCHG_USBIN_OV			BIT(1)
#define SMBCHG_USB_CMD_IL		0x340
#define SMBCHG_USBIN_HC_MODE		BIT(0)
#define SMBCHG_USB51_500MA		BIT(1)
#define SMBCHG_ICL_OVERRIDE		BIT(2)
#define SMBCHG_USBIN_SUSPEND		BIT(4)
#define SMBCHG_USB_IL_CFG		0x3f2
#define SMBCHG_IL_CFG_MASK		GENMASK(4, 0)
#define SMBCHG_DC_RT_STS		0x410
#define SMBCHG_DCIN_UV			BIT(0)
#define SMBCHG_SEC_ACCESS_OFF		0x0d0

/* PMI8994/PMI8996 Fuel Gauge (FG_MEMIF at 0x4400) */
#define FG_MEMIF_BASE			0x4400
#define FG_MEMIF_DIG_MINOR		0x4400
#define FG_MEMIF_DIG_MAJOR		0x4401
#define FG_MEMIF_INT_RT_STS		0x4410
#define FG_MEM_AVAIL_BIT		BIT(0)
#define FG_MEMIF_IMA_CFG		0x4452
#define FG_MEMIF_IMA_OPR_STS		0x4454
#define FG_MEMIF_FG_BEAT_CNT		0x4457
#define FG_MEMIF_IMA_ERR_STS		0x445f
#define FG_MEMIF_IMA_BYTE_EN		0x4460
#define FG_MEM_CFG_RIF_REQ		BIT(7)
#define FG_MEM_CFG_IACS_SLCT		BIT(5)
#define FG_MEM_IMA_REQ			(FG_MEM_CFG_IACS_SLCT | FG_MEM_CFG_RIF_REQ)
#define FG_MEM_CTL_BURST		BIT(7)
#define FG_MEM_CTL_WR_EN		BIT(6)
#define FG_IMA_IACS_CLR			BIT(2)
#define FG_IMA_IACS_RDY			BIT(1)
#define FG_IMA_ERR_MASK			GENMASK(7, 5)
#define FG_SRAM_RAM_OFFSET		0x400
#define FG_SRAM_THERM_COEFF_ADDR	0x444
#define FG_SRAM_THERM_COEFF_OFF		2
#define FG_SRAM_JEITA_ADDR		0x454
#define FG_SRAM_EXT_SENSE_ADDR		0x4ac
#define FG_SRAM_EXT_SENSE_OFF		3
#define FG_BATT_TEMP_CNTRL_MASK		0x17
#define FG_TEMP_SENSE_ALWAYS_BIT	BIT(1)
#define FG_SRAM_BATT_TEMP_ADDR		0x550
#define FG_SRAM_BATT_TEMP_OFF		2
#define FG_SRAM_VBAT_ADDR		0x5cc
#define FG_SRAM_VBAT_OFF		1
#define FG_SRAM_IBAT_ADDR		0x5cc
#define FG_SRAM_IBAT_OFF		3

#define STATUS_USBIN_VALID	BIT(0) /* USB connection is valid */
#define STATUS_DCIN_VALID	BIT(1) /* DC connection is valid */
#define STATUS_BAT_HOT		BIT(2) /* Battery temp 1=Hot, 0=Cold */
#define STATUS_BAT_OK		BIT(3) /* Battery temp OK */
#define STATUS_BAT_PRESENT	BIT(4) /* Battery is present */
#define STATUS_CHG_DONE		BIT(5) /* Charge cycle is complete */
#define STATUS_CHG_TRKL		BIT(6) /* Trickle charging */
#define STATUS_CHG_FAST		BIT(7) /* Fast charging */
#define STATUS_CHG_GONE		BIT(8) /* No charger is connected */

enum smbb_attr {
	ATTR_BAT_ISAFE,
	ATTR_BAT_IMAX,
	ATTR_USBIN_IMAX,
	ATTR_DCIN_IMAX,
	ATTR_BAT_VSAFE,
	ATTR_BAT_VMAX,
	ATTR_BAT_VMIN,
	ATTR_CHG_VDET,
	ATTR_VIN_MIN,
	_ATTR_CNT,
};

struct smbb_charger {
	unsigned int revision;
	unsigned int addr;
	struct device *dev;
	struct extcon_dev *edev;

	bool is_smbchg;
	bool dc_disabled;
	bool jeita_ext_temp;
	unsigned long status;
	struct mutex statlock;
	struct mutex sec_lock;

	unsigned int attr[_ATTR_CNT];

	struct power_supply *usb_psy;
	struct power_supply *dc_psy;
	struct power_supply *bat_psy;
	struct regmap *regmap;

	struct regulator_desc otg_rdesc;
	struct regulator_dev *otg_reg;
	atomic_t smbchg_irq_count;
	unsigned int cmd_chg_en_val;
	struct mutex fg_mem_lock;
	u8 fg_dig_major;
	bool fg_sram_inited;
};

static const unsigned int smbb_usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_NONE,
};

static int smbb_vbat_weak_fn(unsigned int index)
{
	return 2100000 + index * 100000;
}

static int smbb_vin_fn(unsigned int index)
{
	if (index > 42)
		return 5600000 + (index - 43) * 200000;
	return 3400000 + index * 50000;
}

static int smbb_vmax_fn(unsigned int index)
{
	return 3240000 + index * 10000;
}

static int smbb_vbat_det_fn(unsigned int index)
{
	return 3240000 + index * 20000;
}

static int smbb_imax_fn(unsigned int index)
{
	if (index < 2)
		return 100000 + index * 50000;
	return index * 100000;
}

static int smbb_bat_imax_fn(unsigned int index)
{
	return index * 50000;
}

static unsigned int smbb_hw_lookup(unsigned int val, int (*fn)(unsigned int))
{
	unsigned int widx;
	unsigned int sel;

	for (widx = sel = 0; (*fn)(widx) <= val; ++widx)
		sel = widx;

	return sel;
}

static const struct smbb_charger_attr {
	const char *name;
	unsigned int reg;
	unsigned int safe_reg;
	unsigned int max;
	unsigned int min;
	unsigned int fail_ok;
	int (*hw_fn)(unsigned int);
} smbb_charger_attrs[] = {
	[ATTR_BAT_ISAFE] = {
		.name = "qcom,fast-charge-safe-current",
		.reg = SMBB_CHG_ISAFE,
		.max = 3000000,
		.min = 200000,
		.hw_fn = smbb_bat_imax_fn,
		.fail_ok = 1,
	},
	[ATTR_BAT_IMAX] = {
		.name = "qcom,fast-charge-current-limit",
		.reg = SMBB_CHG_IMAX,
		.safe_reg = SMBB_CHG_ISAFE,
		.max = 3000000,
		.min = 200000,
		.hw_fn = smbb_bat_imax_fn,
	},
	[ATTR_DCIN_IMAX] = {
		.name = "qcom,dc-current-limit",
		.reg = SMBB_DC_IMAX,
		.max = 2500000,
		.min = 100000,
		.hw_fn = smbb_imax_fn,
	},
	[ATTR_BAT_VSAFE] = {
		.name = "qcom,fast-charge-safe-voltage",
		.reg = SMBB_CHG_VSAFE,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vmax_fn,
		.fail_ok = 1,
	},
	[ATTR_BAT_VMAX] = {
		.name = "qcom,fast-charge-high-threshold-voltage",
		.reg = SMBB_CHG_VMAX,
		.safe_reg = SMBB_CHG_VSAFE,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vmax_fn,
	},
	[ATTR_BAT_VMIN] = {
		.name = "qcom,fast-charge-low-threshold-voltage",
		.reg = SMBB_CHG_VBAT_WEAK,
		.max = 3600000,
		.min = 2100000,
		.hw_fn = smbb_vbat_weak_fn,
	},
	[ATTR_CHG_VDET] = {
		.name = "qcom,auto-recharge-threshold-voltage",
		.reg = SMBB_CHG_VBAT_DET,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vbat_det_fn,
	},
	[ATTR_VIN_MIN] = {
		.name = "qcom,minimum-input-voltage",
		.reg = SMBB_CHG_VIN_MIN,
		.max = 9600000,
		.min = 4200000,
		.hw_fn = smbb_vin_fn,
	},
	[ATTR_USBIN_IMAX] = {
		.name = "usb-charge-current-limit",
		.reg = SMBB_USB_IMAX,
		.max = 2500000,
		.min = 100000,
		.hw_fn = smbb_imax_fn,
	},
};

static int smbb_charger_attr_write(struct smbb_charger *chg,
		enum smbb_attr which, unsigned int val)
{
	const struct smbb_charger_attr *prop;
	unsigned int wval;
	unsigned int out;
	int rc;

	prop = &smbb_charger_attrs[which];

	if (val > prop->max || val < prop->min) {
		dev_err(chg->dev, "value out of range for %s [%u:%u]\n",
			prop->name, prop->min, prop->max);
		return -EINVAL;
	}

	if (prop->safe_reg) {
		rc = regmap_read(chg->regmap,
				chg->addr + prop->safe_reg, &wval);
		if (rc) {
			dev_err(chg->dev,
				"unable to read safe value for '%s'\n",
				prop->name);
			return rc;
		}

		wval = prop->hw_fn(wval);

		if (val > wval) {
			dev_warn(chg->dev,
				"%s above safe value, clamping at %u\n",
				prop->name, wval);
			val = wval;
		}
	}

	wval = smbb_hw_lookup(val, prop->hw_fn);

	rc = regmap_write(chg->regmap, chg->addr + prop->reg, wval);
	if (rc) {
		dev_err(chg->dev, "unable to update %s", prop->name);
		return rc;
	}
	out = prop->hw_fn(wval);
	if (out != val) {
		dev_warn(chg->dev,
			"%s inaccurate, rounded to %u\n",
			prop->name, out);
	}

	dev_dbg(chg->dev, "%s <= %d\n", prop->name, out);

	chg->attr[which] = out;

	return 0;
}

static int smbb_charger_attr_read(struct smbb_charger *chg,
		enum smbb_attr which)
{
	const struct smbb_charger_attr *prop;
	unsigned int val;
	int rc;

	prop = &smbb_charger_attrs[which];

	rc = regmap_read(chg->regmap, chg->addr + prop->reg, &val);
	if (rc) {
		dev_err(chg->dev, "failed to read %s\n", prop->name);
		return rc;
	}
	val = prop->hw_fn(val);
	dev_dbg(chg->dev, "%s => %d\n", prop->name, val);

	chg->attr[which] = val;

	return 0;
}

static int smbb_charger_attr_parse(struct smbb_charger *chg,
		enum smbb_attr which)
{
	const struct smbb_charger_attr *prop;
	unsigned int val;
	int rc;

	prop = &smbb_charger_attrs[which];

	rc = of_property_read_u32(chg->dev->of_node, prop->name, &val);
	if (rc == 0) {
		rc = smbb_charger_attr_write(chg, which, val);
		if (!rc || !prop->fail_ok)
			return rc;
	}
	return smbb_charger_attr_read(chg, which);
}

static void smbb_set_line_flag(struct smbb_charger *chg, int irq, int flag)
{
	bool state = false;
	unsigned int val = 0;
	int ret;

	ret = irq_get_irqchip_state(irq, IRQCHIP_STATE_LINE_LEVEL, &state);
	if (ret < 0) {
		/* Fallback to reading SPMI peripheral status registers */
		if (flag == STATUS_BAT_PRESENT) {
			state = true;
		} else if (flag == STATUS_BAT_OK) {
			state = true;
		} else if (flag == STATUS_USBIN_VALID) {
			if (!regmap_read(chg->regmap, chg->addr + 0x308, &val))
				state = !!(val & BIT(7));
			else
				state = true;
		} else if (flag == STATUS_CHG_GONE) {
			if (!regmap_read(chg->regmap, chg->addr + 0x308, &val))
				state = !(val & BIT(7));
			else
				state = false;
		} else if (flag == STATUS_CHG_FAST) {
			if (!regmap_read(chg->regmap, chg->addr + 0x08, &val))
				state = !!(val & BIT(7));
		} else if (flag == STATUS_CHG_TRKL) {
			if (!regmap_read(chg->regmap, chg->addr + 0x08, &val))
				state = !!(val & BIT(6));
		} else if (flag == STATUS_CHG_DONE) {
			if (!regmap_read(chg->regmap, chg->addr + 0x08, &val))
				state = !!(val & BIT(5));
		} else if (flag == STATUS_DCIN_VALID) {
			state = false;
		}
	}

	mutex_lock(&chg->statlock);
	if (state)
		chg->status |= flag;
	else
		chg->status &= ~flag;
	mutex_unlock(&chg->statlock);

	dev_dbg(chg->dev, "status = %03lx\n", chg->status);
}

static irqreturn_t smbb_usb_valid_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_USBIN_VALID);
	extcon_set_state_sync(chg->edev, EXTCON_USB,
				chg->status & STATUS_USBIN_VALID);
	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_dc_valid_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_DCIN_VALID);
	if (!chg->dc_disabled)
		power_supply_changed(chg->dc_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_bat_temp_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;
	unsigned int val;
	int rc;

	rc = regmap_read(chg->regmap, chg->addr + SMBB_BAT_TEMP_STATUS, &val);
	if (rc)
		return IRQ_HANDLED;

	mutex_lock(&chg->statlock);
	if (val & TEMP_STATUS_OK) {
		chg->status |= STATUS_BAT_OK;
	} else {
		chg->status &= ~STATUS_BAT_OK;
		if (val & TEMP_STATUS_HOT)
			chg->status |= STATUS_BAT_HOT;
	}
	mutex_unlock(&chg->statlock);

	power_supply_changed(chg->bat_psy);
	return IRQ_HANDLED;
}

static irqreturn_t smbb_bat_present_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_BAT_PRESENT);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_done_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_DONE);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_gone_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_GONE);
	power_supply_changed(chg->bat_psy);
	power_supply_changed(chg->usb_psy);
	if (!chg->dc_disabled)
		power_supply_changed(chg->dc_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_fast_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_FAST);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_trkl_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_TRKL);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static const struct smbb_irq {
	const char *name;
	irqreturn_t (*handler)(int, void *);
} smbb_charger_irqs[] = {
	{ "chg-done", smbb_chg_done_handler },
	{ "chg-fast", smbb_chg_fast_handler },
	{ "chg-trkl", smbb_chg_trkl_handler },
	{ "bat-temp-ok", smbb_bat_temp_handler },
	{ "bat-present", smbb_bat_present_handler },
	{ "chg-gone", smbb_chg_gone_handler },
	{ "usb-valid", smbb_usb_valid_handler },
	{ "dc-valid", smbb_dc_valid_handler },
};

static const int smbchg_usb_ilim_ma_8996[] = {
	300, 400, 500, 600, 700, 800, 900, 950,
	1000, 1100, 1200, 1400, 1450, 1500, 1600, 1800,
	1850, 1880, 1910, 1930, 1950, 1970, 2000, 2050,
	2100, 2300, 2400, 2500, 3000,
};

/* PMI8994 / PMI8996 FCC_CFG (0x10F2) 5-bit step table (mA) */
static const int smbchg_fcc_ma_8996[] = {
	300, 400, 500, 600, 700, 800, 900, 1000,
	1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800,
	1900, 2000, 2100, 2200, 2300, 2400, 2500, 3000,
};

static int smbchg_sec_update_bits(struct smbb_charger *chg, unsigned int reg_off,
				  unsigned int mask, unsigned int val)
{
	unsigned int sec_addr = chg->addr + (reg_off & ~0xffU) + SMBCHG_SEC_ACCESS_OFF;
	int rc;

	mutex_lock(&chg->sec_lock);
	rc = regmap_write(chg->regmap, sec_addr, SEC_ACCESS_MAGIC);
	if (!rc)
		rc = regmap_update_bits(chg->regmap, chg->addr + reg_off, mask, val);
	mutex_unlock(&chg->sec_lock);

	return rc;
}

static bool smbchg_is_usb_present(struct smbb_charger *chg)
{
	unsigned int val = 0;

	if (regmap_read(chg->regmap, chg->addr + SMBCHG_USBIN_RT_STS, &val))
		return false;

	return !(val & (SMBCHG_USBIN_UV | SMBCHG_USBIN_OV));
}

static bool smbchg_is_dc_present(struct smbb_charger *chg)
{
	unsigned int val = 0;

	if (chg->dc_disabled ||
	    regmap_read(chg->regmap, chg->addr + SMBCHG_DC_RT_STS, &val))
		return false;

	return !(val & SMBCHG_DCIN_UV);
}

static int smbchg_get_usb_icl_ua(struct smbb_charger *chg)
{
	unsigned int sts1 = 0, sts2 = 0, idx;

	if (regmap_read(chg->regmap, chg->addr + SMBCHG_USB_ICL_STS_2, &sts2))
		return chg->attr[ATTR_USBIN_IMAX];
	if (sts2 & SMBCHG_USBIN_SUSPEND_STS)
		return 0;

	if (regmap_read(chg->regmap, chg->addr + SMBCHG_USB_ICL_STS_1, &sts1))
		return chg->attr[ATTR_USBIN_IMAX];

	idx = sts1 & SMBCHG_ICL_STS_MASK;
	if (idx < ARRAY_SIZE(smbchg_usb_ilim_ma_8996))
		return smbchg_usb_ilim_ma_8996[idx] * 1000;

	return smbchg_usb_ilim_ma_8996[ARRAY_SIZE(smbchg_usb_ilim_ma_8996) - 1] * 1000;
}

static int smbchg_set_usb_icl(struct smbb_charger *chg, int ua, bool override)
{
	int target_ma = clamp(ua / 1000, 300, 3000);
	unsigned int idx = 0, i;
	unsigned int cmd_mask = SMBCHG_USBIN_HC_MODE | SMBCHG_USB51_500MA |
				SMBCHG_USBIN_SUSPEND;
	unsigned int cmd_val = SMBCHG_USBIN_HC_MODE | SMBCHG_USB51_500MA;
	int rc;

	for (i = 0; i < ARRAY_SIZE(smbchg_usb_ilim_ma_8996); i++) {
		if (smbchg_usb_ilim_ma_8996[i] <= target_ma)
			idx = i;
		else
			break;
	}

	rc = smbchg_sec_update_bits(chg, SMBCHG_USB_IL_CFG, SMBCHG_IL_CFG_MASK, idx);
	if (rc)
		return rc;

	if (override) {
		cmd_mask |= SMBCHG_ICL_OVERRIDE;
		cmd_val |= SMBCHG_ICL_OVERRIDE;
	}

	rc = regmap_update_bits(chg->regmap, chg->addr + SMBCHG_USB_CMD_IL,
				cmd_mask, cmd_val);
	if (!rc)
		chg->attr[ATTR_USBIN_IMAX] = smbchg_usb_ilim_ma_8996[idx] * 1000;

	return rc;
}

struct fg_memif_layout {
	u16 cfg;
	u16 ctl;
	u16 addr_lsb;
	u16 rd0;
	u16 wr0;
	bool ima;
};

static const struct fg_memif_layout *fg_get_memif_layout(struct smbb_charger *chg)
{
	static const struct fg_memif_layout rev_conv = {
		.cfg = FG_MEMIF_BASE + 0x40,
		.ctl = FG_MEMIF_BASE + 0x41,
		.addr_lsb = FG_MEMIF_BASE + 0x42,
		.rd0 = FG_MEMIF_BASE + 0x4c,
		.wr0 = FG_MEMIF_BASE + 0x48,
		.ima = false,
	};
	static const struct fg_memif_layout rev_ima = {
		.cfg = FG_MEMIF_BASE + 0x50,
		.ctl = FG_MEMIF_BASE + 0x51,
		.addr_lsb = FG_MEMIF_BASE + 0x61,
		.rd0 = FG_MEMIF_BASE + 0x67,
		.wr0 = FG_MEMIF_BASE + 0x63,
		.ima = true,
	};

	if (!chg->fg_dig_major) {
		unsigned int rev = 0;

		if (!regmap_read(chg->regmap, FG_MEMIF_DIG_MAJOR, &rev))
			chg->fg_dig_major = rev & 0xff;
	}

	if (chg->fg_dig_major >= 3)
		return &rev_ima;
	if (chg->fg_dig_major >= 1)
		return &rev_conv;
	return NULL;
}

static int fg_ima_clear(struct smbb_charger *chg, const struct fg_memif_layout *l)
{
	unsigned int dummy = 0;
	int rc;

	rc = regmap_update_bits(chg->regmap, FG_MEMIF_IMA_CFG,
				FG_IMA_IACS_CLR, FG_IMA_IACS_CLR);
	if (!rc)
		rc = regmap_write(chg->regmap, l->addr_lsb + 1, 0x04);
	if (!rc)
		rc = regmap_write(chg->regmap, l->wr0 + 3, 0x00);
	if (!rc)
		rc = regmap_read(chg->regmap, l->rd0 + 3, &dummy);
	regmap_update_bits(chg->regmap, FG_MEMIF_IMA_CFG, FG_IMA_IACS_CLR, 0);
	return rc;
}

static int fg_ima_wait_iacs(struct smbb_charger *chg)
{
	unsigned int sts = 0;
	int i, rc;

	usleep_range(30, 50);
	for (i = 0; i < 60; i++) {
		rc = regmap_read(chg->regmap, FG_MEMIF_IMA_OPR_STS, &sts);
		if (rc)
			return rc;
		if (sts & FG_IMA_IACS_RDY)
			return 0;
		usleep_range(1000, 1500);
	}
	return -ETIMEDOUT;
}

static int fg_ima_check_err(struct smbb_charger *chg, const struct fg_memif_layout *l)
{
	unsigned int err = 0;
	int rc;

	rc = regmap_read(chg->regmap, FG_MEMIF_IMA_ERR_STS, &err);
	if (rc)
		return rc;
	if (err & FG_IMA_ERR_MASK) {
		fg_ima_clear(chg, l);
		return -EAGAIN;
	}
	return 0;
}

static int fg_read_bytes(struct smbb_charger *chg, u16 reg, u8 *buf, int len)
{
	unsigned int val = 0;
	int i, rc;

	for (i = 0; i < len; i++) {
		rc = regmap_read(chg->regmap, reg + i, &val);
		if (rc)
			return rc;
		buf[i] = val & 0xff;
	}
	return 0;
}

static int fg_write_bytes(struct smbb_charger *chg, u16 reg, const u8 *buf, int len)
{
	int i, rc;

	for (i = 0; i < len; i++) {
		rc = regmap_write(chg->regmap, reg + i, buf[i]);
		if (rc)
			return rc;
	}
	return 0;
}

/* Transfer 1..4 bytes within a single aligned 4-byte SRAM word over DIG_REV_3 IMA. */
static int fg_sram_word_ima(struct smbb_charger *chg, const struct fg_memif_layout *l,
			    u16 word_addr, int off, u8 *buf, int len, bool write)
{
	unsigned int val = 0, beat0 = 0, beat1 = 0;
	int i, rc;

	for (i = 0; i < 20; i++) {
		rc = regmap_read(chg->regmap, l->cfg, &val);
		if (rc)
			return rc;
		if (!(val & FG_MEM_CFG_RIF_REQ))
			break;
		usleep_range(2000, 2500);
	}
	if (val & FG_MEM_CFG_RIF_REQ)
		return -ETIMEDOUT;

	rc = regmap_update_bits(chg->regmap, l->cfg, FG_MEM_IMA_REQ, FG_MEM_IMA_REQ);
	if (rc)
		goto out;

	rc = regmap_write(chg->regmap, l->ctl, write ? FG_MEM_CTL_WR_EN : 0);
	if (rc)
		goto out;

	rc = fg_ima_wait_iacs(chg);
	if (rc) {
		fg_ima_clear(chg, l);
		rc = -EAGAIN;
		goto out;
	}

	rc = regmap_write(chg->regmap, l->addr_lsb, word_addr & 0xff);
	if (!rc)
		rc = regmap_write(chg->regmap, l->addr_lsb + 1, (word_addr >> 8) & 0xff);
	if (rc)
		goto out;

	rc = fg_ima_wait_iacs(chg);
	if (rc) {
		fg_ima_clear(chg, l);
		rc = -EAGAIN;
		goto out;
	}

	if (!write) {
		rc = regmap_read(chg->regmap, FG_MEMIF_FG_BEAT_CNT, &beat0);
		if (rc)
			goto out;
		rc = fg_read_bytes(chg, l->rd0 + off, buf, len);
		if (rc)
			goto out;
		rc = fg_ima_wait_iacs(chg);
		if (rc) {
			fg_ima_clear(chg, l);
			rc = -EAGAIN;
			goto out;
		}
		rc = fg_ima_check_err(chg, l);
		if (rc)
			goto out;
		rc = regmap_read(chg->regmap, FG_MEMIF_FG_BEAT_CNT, &beat1);
		if (!rc && ((beat0 ^ beat1) & 0x0f))
			rc = -EAGAIN;
	} else {
		u8 byte_en = ((1U << len) - 1U) << off;

		rc = regmap_write(chg->regmap, FG_MEMIF_IMA_BYTE_EN, byte_en);
		if (rc)
			goto out;
		rc = fg_write_bytes(chg, l->wr0 + off, buf, len);
		if (rc)
			goto out;
		/* Writing WR_DATA3 triggers the IMA SRAM write when bit 3 isn't written above. */
		if (!(byte_en & BIT(3))) {
			rc = regmap_write(chg->regmap, l->wr0 + 3, 0x00);
			if (rc)
				goto out;
		}
		rc = fg_ima_wait_iacs(chg);
		if (rc) {
			fg_ima_clear(chg, l);
			rc = -EAGAIN;
			goto out;
		}
		rc = fg_ima_check_err(chg, l);
	}

out:
	regmap_update_bits(chg->regmap, l->cfg, FG_MEM_IMA_REQ, 0);
	return rc;
}

/* Transfer 1..4 bytes within a 4-byte SRAM word on pre-IMA DIG_REV_1/2 hardware. */
static int fg_sram_word_conv(struct smbb_charger *chg, const struct fg_memif_layout *l,
			     u16 word_addr, int off, u8 *buf, int len, bool write)
{
	u8 word[4];
	unsigned int val = 0;
	int i, rc;

	rc = regmap_update_bits(chg->regmap, l->cfg, FG_MEM_CFG_RIF_REQ, FG_MEM_CFG_RIF_REQ);
	if (rc)
		return rc;

	for (i = 0; i < 50; i++) {
		rc = regmap_read(chg->regmap, FG_MEMIF_INT_RT_STS, &val);
		if (!rc && (val & FG_MEM_AVAIL_BIT))
			break;
		usleep_range(2000, 2500);
	}
	if (!(val & FG_MEM_AVAIL_BIT)) {
		rc = -ETIMEDOUT;
		goto out;
	}

	if (!write || len < 4) {
		rc = regmap_write(chg->regmap, l->ctl, 0);
		if (!rc)
			rc = regmap_write(chg->regmap, l->addr_lsb, word_addr & 0xff);
		if (!rc)
			rc = regmap_write(chg->regmap, l->addr_lsb + 1, (word_addr >> 8) & 0xff);
		if (!rc)
			rc = fg_read_bytes(chg, l->rd0, word, 4);
		if (rc)
			goto out;
	}

	if (!write) {
		memcpy(buf, word + off, len);
	} else {
		memcpy(word + off, buf, len);
		rc = regmap_write(chg->regmap, l->ctl, FG_MEM_CTL_WR_EN);
		if (!rc)
			rc = regmap_write(chg->regmap, l->addr_lsb, word_addr & 0xff);
		if (!rc)
			rc = regmap_write(chg->regmap, l->addr_lsb + 1, (word_addr >> 8) & 0xff);
		if (!rc)
			rc = fg_write_bytes(chg, l->wr0, word, 4);
		if (rc)
			goto out;
	}

out:
	regmap_update_bits(chg->regmap, l->cfg, FG_MEM_CFG_RIF_REQ, 0);
	return rc;
}

/**
 * fg_sram_xfer - Read or write arbitrary bytes in PMI8994/PMI8996 FG SRAM.
 * @chg: charger driver state
 * @address: starting 4-byte-aligned or byte SRAM address (>= FG_SRAM_RAM_OFFSET)
 * @offset: byte offset added to @address
 * @buf: source or destination buffer
 * @len: byte count to transfer
 * @write: true for write, false for read
 */
static int fg_sram_xfer(struct smbb_charger *chg, u16 address, int offset,
			u8 *buf, int len, bool write)
{
	const struct fg_memif_layout *l;
	unsigned int cur = address + offset;
	int rem = len, rc = 0;

	if (cur < FG_SRAM_RAM_OFFSET || len <= 0)
		return -EINVAL;

	mutex_lock(&chg->fg_mem_lock);
	l = fg_get_memif_layout(chg);
	if (!l) {
		mutex_unlock(&chg->fg_mem_lock);
		return -ENODEV;
	}

	while (rem > 0) {
		u16 word_addr = cur & ~0x3U;
		int word_off = cur & 0x3;
		int chunk = min(rem, 4 - word_off);
		int tries;

		for (tries = 0; tries < 3; tries++) {
			rc = l->ima ? fg_sram_word_ima(chg, l, word_addr, word_off, buf, chunk, write)
				    : fg_sram_word_conv(chg, l, word_addr, word_off, buf, chunk, write);
			if (rc != -EAGAIN)
				break;
		}
		if (rc)
			break;

		cur += chunk;
		buf += chunk;
		rem -= chunk;
	}
	mutex_unlock(&chg->fg_mem_lock);
	return rc;
}

static int fg_sram_read(struct smbb_charger *chg, u16 address, int offset, u8 *buf, int len)
{
	return fg_sram_xfer(chg, address, offset, buf, len, false);
}

static int fg_sram_write(struct smbb_charger *chg, u16 address, int offset, const u8 *buf, int len)
{
	return fg_sram_xfer(chg, address, offset, (u8 *)buf, len, true);
}

static int fg_read_batt_temp_decidegc(struct smbb_charger *chg, int *temp_decidegc)
{
	u8 reg[2];
	u16 raw;
	int rc;

	rc = fg_sram_read(chg, FG_SRAM_BATT_TEMP_ADDR, FG_SRAM_BATT_TEMP_OFF, reg, 2);
	if (rc)
		return rc;
	raw = reg[0] | (reg[1] << 8);
	*temp_decidegc = (int)(((s64)raw * 625) / 1000) - 2730;
	return 0;
}

static int fg_read_vbat_uv(struct smbb_charger *chg, int *uv)
{
	u8 reg[2];
	u16 raw;
	int rc;

	rc = fg_sram_read(chg, FG_SRAM_VBAT_ADDR, FG_SRAM_VBAT_OFF, reg, 2);
	if (rc)
		return rc;
	raw = reg[0] | (reg[1] << 8);
	*uv = (int)(((u64)raw * 152587ULL) / 1000ULL);
	return 0;
}

static int fg_read_ibat_ua(struct smbb_charger *chg, int *ua)
{
	u8 reg[2];
	s16 raw;
	int rc;

	rc = fg_sram_read(chg, FG_SRAM_IBAT_ADDR, FG_SRAM_IBAT_OFF, reg, 2);
	if (rc)
		return rc;
	raw = (s16)(reg[0] | (reg[1] << 8));
	*ua = (int)(((s64)raw * 152587) / 1000);
	return 0;
}

/* Apply Pixel (sailfish) stock NTC beta coefficients and JEITA limits to FG SRAM. */
static void smbchg_init_fg_sram(struct smbb_charger *chg)
{
	static const u8 sailfish_coeff[6] = { 0x97, 0x85, 0x62, 0x49, 0x5a, 0x35 };
	/* (decidegC / 10) + 30 for cool=10C, warm=47C, cold=0C, hot=55C */
	static const u8 sailfish_jeita[4] = { 40, 77, 30, 85 };
	u8 cur_coeff[6] = { 0 }, cur_jeita[4] = { 0 }, sense = 0, new_sense;
	unsigned int dig_minor = 0, fg_bat_rt = 0, bat_rt = 0;
	int temp = 0, temp_rc, rc;

	fg_get_memif_layout(chg);
	regmap_read(chg->regmap, FG_MEMIF_DIG_MINOR, &dig_minor);
	if (!chg->fg_dig_major) {
		dev_warn(chg->dev, "fg_sram: FG_MEMIF unavailable\n");
		return;
	}

	rc = fg_sram_read(chg, FG_SRAM_THERM_COEFF_ADDR, FG_SRAM_THERM_COEFF_OFF, cur_coeff, 6);
	rc |= fg_sram_read(chg, FG_SRAM_JEITA_ADDR, 0, cur_jeita, 4);
	rc |= fg_sram_read(chg, FG_SRAM_EXT_SENSE_ADDR, FG_SRAM_EXT_SENSE_OFF, &sense, 1);
	temp_rc = fg_read_batt_temp_decidegc(chg, &temp);
	regmap_read(chg->regmap, chg->addr + 0x3110, &fg_bat_rt);
	regmap_read(chg->regmap, chg->addr + SMBCHG_BAT_IF_RT_STS, &bat_rt);

	dev_info(chg->dev,
		 "fg_sram: pre-init rev=%u.%u rc=%d T=%d(rc=%d) coeff=%02x%02x%02x%02x%02x%02x jeita=%u,%u,%u,%u sense=0x%02x fg_rt=0x%02x bat_rt=0x%02x\n",
		 chg->fg_dig_major, dig_minor & 0xff, rc, temp, temp_rc,
		 cur_coeff[0], cur_coeff[1], cur_coeff[2],
		 cur_coeff[3], cur_coeff[4], cur_coeff[5],
		 cur_jeita[0], cur_jeita[1], cur_jeita[2], cur_jeita[3],
		 sense, fg_bat_rt, bat_rt);

	rc = fg_sram_write(chg, FG_SRAM_THERM_COEFF_ADDR, FG_SRAM_THERM_COEFF_OFF, sailfish_coeff, 6);
	if (rc)
		dev_warn(chg->dev, "fg_sram: failed to write therm coeff (%d)\n", rc);

	rc = fg_sram_write(chg, FG_SRAM_JEITA_ADDR, 0, sailfish_jeita, 4);
	if (rc)
		dev_warn(chg->dev, "fg_sram: failed to write JEITA limits (%d)\n", rc);

	new_sense = (sense & ~FG_BATT_TEMP_CNTRL_MASK) | FG_TEMP_SENSE_ALWAYS_BIT;
	if (new_sense != sense) {
		rc = fg_sram_write(chg, FG_SRAM_EXT_SENSE_ADDR, FG_SRAM_EXT_SENSE_OFF, &new_sense, 1);
		if (rc)
			dev_warn(chg->dev, "fg_sram: failed to enable therm bias (%d)\n", rc);
	}

	chg->fg_sram_inited = true;
}

static int smbchg_enable_charging(struct smbb_charger *chg)
{
	unsigned int chgr_sts = 0, i;
	int rc;

	/* Disable SMBCHG hardware watchdog so expiry cannot halt charging */
	rc = smbchg_sec_update_bits(chg, SMBCHG_MISC_WDOG_CFG,
				    SMBCHG_WDOG_EN, 0);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to disable WDOG_CFG (%d)\n", rc);

	rc = smbchg_sec_update_bits(chg, SMBCHG_USB_CHGPTH_CFG_F4,
				    SMBCHG_CFG_USB_2_3_SEL,
				    SMBCHG_CFG_USB_2_3_SEL);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to configure CHGPTH_CFG_F4 (%d)\n", rc);

	rc = smbchg_set_usb_icl(chg, chg->attr[ATTR_USBIN_IMAX], true);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to configure USB ICL (%d)\n", rc);

	/*
	 * Set software active-low charge enable in CHGR_CFG2_FC (0x10fc) while
	 * preserving the bootloader's lower bits [5:0] (e.g. 0x07 -> 0x47).
	 */
	rc = smbchg_sec_update_bits(chg, SMBCHG_CHGR_CFG2_FC,
				    SMBCHG_CHGR_CFG2_CHG_EN_SRC |
				    SMBCHG_CHGR_CFG2_CHG_EN_POL,
				    SMBCHG_CHGR_CFG2_CHG_EN_POL);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to configure CHGR_CFG2_FC (%d)\n", rc);

	chg->cmd_chg_en_val = 0;
	rc = regmap_update_bits(chg->regmap, chg->addr + SMBCHG_BAT_IF_CMD_CHG,
				SMBCHG_CMD_CHG_EN_N, 0);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to enable BAT_IF charging (%d)\n", rc);

	rc = regmap_update_bits(chg->regmap, chg->addr + SMBCHG_CHGR_CMD_CHG,
				SMBCHG_CMD_CHG_EN_N, 0);
	if (rc)
		dev_warn(chg->dev, "smbchg: failed to enable CHGR charging (%d)\n", rc);

	/* Allow the hardware startup hold-off timer to complete */
	for (i = 0; i < 6; i++) {
		if (regmap_read(chg->regmap, chg->addr + SMBCHG_CHGR_STS, &chgr_sts))
			break;
		if ((chgr_sts & SMBCHG_CHGR_TYPE_MASK) != 0 ||
		    !(chgr_sts & SMBCHG_CHGR_HOLD_OFF))
			break;
		msleep(100);
	}

	dev_info(chg->dev, "smbchg: enable complete (chgr_sts=0x%02x)\n", chgr_sts);
	return rc;
}

static int smbchg_get_batt_health(struct smbb_charger *chg)
{
	/*
	 * PMI8996 BAT_IF_RT_STS (0x1210) can assert HOT_BAT_HARD falsely during
	 * bring-up; never report OVERHEAT so Android BatteryService does not shut down.
	 */
	return POWER_SUPPLY_HEALTH_GOOD;
}

static void smbchg_get_batt_status(struct smbb_charger *chg,
				   int *status, int *charge_type)
{
	unsigned int chgr_sts = 0, chgr_rt = 0, ctype;
	int health;

	*charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;

	if (!smbchg_is_usb_present(chg) && !smbchg_is_dc_present(chg)) {
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		return;
	}

	if (regmap_read(chg->regmap, chg->addr + SMBCHG_CHGR_STS, &chgr_sts)) {
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		return;
	}

	health = smbchg_get_batt_health(chg);
	ctype = (chgr_sts & SMBCHG_CHGR_TYPE_MASK) >> 1;

	switch (ctype) {
	case 1: /* pre-charge */
		*status = POWER_SUPPLY_STATUS_CHARGING;
		*charge_type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		return;
	case 2: /* fast-charge */
	case 3: /* taper-charge */
		*status = POWER_SUPPLY_STATUS_CHARGING;
		*charge_type = POWER_SUPPLY_CHARGE_TYPE_FAST;
		return;
	default:
		break;
	}

	if (!regmap_read(chg->regmap, chg->addr + SMBCHG_CHGR_RT_STS, &chgr_rt) &&
	    (chgr_rt & (SMBCHG_CHGR_RT_TCC_REACHED |
			SMBCHG_CHGR_COMP_SFT |
			SMBCHG_CHGR_INHIBIT)) &&
	    health == POWER_SUPPLY_HEALTH_GOOD) {
		*status = POWER_SUPPLY_STATUS_FULL;
		return;
	}

	*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int smbchg_append_reg_range(struct smbb_charger *chg, char *buf,
				   size_t size, int pos, const char *label,
				   unsigned int base_off)
{
	unsigned int i;

	if (pos >= (int)size)
		return pos;

	pos += scnprintf(buf + pos, size - pos, "%s", label);
	for (i = 0; i < 0x10 && pos < (int)size; i++) {
		unsigned int val = 0;
		int rc = regmap_read(chg->regmap, chg->addr + base_off + i, &val);

		if (rc)
			pos += scnprintf(buf + pos, size - pos, "err ");
		else
			pos += scnprintf(buf + pos, size - pos, "%02x ", val);
	}

	return pos;
}

static int smbchg_format_regs(struct smbb_charger *chg, char *buf, size_t size)
{
	static const struct {
		const char *name;
		unsigned int off;
	} regs[] = {
		{ "chgr_sts", SMBCHG_CHGR_STS },
		{ "chgr_rt", SMBCHG_CHGR_RT_STS },
		{ "cmd_chg", SMBCHG_CHGR_CMD_CHG },
		{ "bat_cmd_chg", SMBCHG_BAT_IF_CMD_CHG },
		{ "chgr_cfg2", SMBCHG_CHGR_CFG2 },
		{ "cfg2_fc", SMBCHG_CHGR_CFG2_FC },
		{ "sft_cfg", SMBCHG_CHGR_SFT_CFG },
		{ "fcc_cfg", SMBCHG_CHGR_FCC_CFG },
		{ "vfloat_cfg", SMBCHG_CHGR_VFLOAT_CFG },
		{ "otg_rt", SMBCHG_OTG_RT_STS },
		{ "otg_cmd", SMBCHG_OTG_CMD },
		{ "bat_rt", SMBCHG_BAT_IF_RT_STS },
		{ "bm_cfg", SMBCHG_BAT_IF_BM_CFG },
		{ "icl_sts1", SMBCHG_USB_ICL_STS_1 },
		{ "apsd", SMBCHG_USB_APSD_DG_STS },
		{ "icl_sts2", SMBCHG_USB_ICL_STS_2 },
		{ "typec1", SMBCHG_TYPEC_STS_1 },
		{ "typec2", SMBCHG_TYPEC_STS_2 },
		{ "usb_rt", SMBCHG_USBIN_RT_STS },
		{ "cmd_il", SMBCHG_USB_CMD_IL },
		{ "chgpth_cfg", SMBCHG_USB_CHGPTH_CFG },
		{ "chgpth_f4", SMBCHG_USB_CHGPTH_CFG_F4 },
		{ "il_cfg", SMBCHG_USB_IL_CFG },
		{ "idev_sts", SMBCHG_MISC_IDEV_STS },
		{ "wdog_cfg", SMBCHG_MISC_WDOG_CFG },
		{ "fg_soc", SMBCHG_FG_SOC_OFF },
		{ "fg_vbat", SMBCHG_FG_ADC_VBAT_OFF },
		{ "fg_ibat", SMBCHG_FG_ADC_IBAT_OFF },
	};
	int pos = 0;
	unsigned int i, fg_bat_rt = 0;
	int rc;

	for (i = 0; i < ARRAY_SIZE(regs) && pos < (int)size; i++) {
		unsigned int val = 0;

		rc = regmap_read(chg->regmap, chg->addr + regs[i].off, &val);
		if (rc)
			pos += scnprintf(buf + pos, size - pos, "%s=err(%d) ", regs[i].name, rc);
		else
			pos += scnprintf(buf + pos, size - pos, "%s=0x%02x ", regs[i].name, val);
	}
	if (pos < (int)size)
		pos += scnprintf(buf + pos, size - pos, "irqs=%d\n",
				 atomic_read(&chg->smbchg_irq_count));

	pos = smbchg_append_reg_range(chg, buf, size, pos, "chgr_cfg[f0..ff]: ", 0x0f0);
	if (pos < (int)size)
		pos += scnprintf(buf + pos, size - pos, "\n");

	pos = smbchg_append_reg_range(chg, buf, size, pos, "bat_cfg[f0..ff]: ", 0x2f0);
	if (pos < (int)size) {
		rc = regmap_read(chg->regmap, chg->addr + 0x3110, &fg_bat_rt);
		if (rc)
			pos += scnprintf(buf + pos, size - pos, "fg_bat_rt=err(%d)\n", rc);
		else
			pos += scnprintf(buf + pos, size - pos, "fg_bat_rt=0x%02x\n", fg_bat_rt);
	}

	if (pos < (int)size) {
		u8 coeff[6] = { 0 }, jeita[4] = { 0 }, sense = 0;
		unsigned int dig_minor = 0;
		int temp = 0, uv = 0, ua = 0, sram_rc;

		regmap_read(chg->regmap, FG_MEMIF_DIG_MINOR, &dig_minor);
		sram_rc = fg_sram_read(chg, FG_SRAM_THERM_COEFF_ADDR,
				       FG_SRAM_THERM_COEFF_OFF, coeff, 6);
		if (!sram_rc)
			sram_rc = fg_sram_read(chg, FG_SRAM_JEITA_ADDR, 0, jeita, 4);
		if (!sram_rc)
			sram_rc = fg_sram_read(chg, FG_SRAM_EXT_SENSE_ADDR,
					       FG_SRAM_EXT_SENSE_OFF, &sense, 1);
		if (!sram_rc)
			sram_rc = fg_read_batt_temp_decidegc(chg, &temp);
		if (!sram_rc)
			fg_read_vbat_uv(chg, &uv);
		if (!sram_rc)
			fg_read_ibat_ua(chg, &ua);

		pos += scnprintf(buf + pos, size - pos,
				 "fg_sram: rc=%d rev=%u.%u T=%d.%dC V=%dmV I=%dmA coeff=%02x%02x%02x%02x%02x%02x jeita=%u,%u,%u,%u sense=0x%02x\n",
				 sram_rc, chg->fg_dig_major, dig_minor & 0xff,
				 temp / 10, abs(temp % 10), uv / 1000, ua / 1000,
				 coeff[0], coeff[1], coeff[2],
				 coeff[3], coeff[4], coeff[5],
				 jeita[0], jeita[1], jeita[2], jeita[3], sense);
	}

	return pos;
}

static ssize_t smbchg_regs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct smbb_charger *chg = dev_get_drvdata(dev);

	return smbchg_format_regs(chg, buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(smbchg_regs);

static irqreturn_t smbchg_irq_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;
	bool usb_present = smbchg_is_usb_present(chg);

	atomic_inc(&chg->smbchg_irq_count);
	if (usb_present) {
		smbchg_set_usb_icl(chg, chg->attr[ATTR_USBIN_IMAX], true);
		regmap_update_bits(chg->regmap, chg->addr + SMBCHG_BAT_IF_CMD_CHG,
				   SMBCHG_CMD_CHG_EN_N, chg->cmd_chg_en_val);
		regmap_update_bits(chg->regmap, chg->addr + SMBCHG_CHGR_CMD_CHG,
				   SMBCHG_CMD_CHG_EN_N, chg->cmd_chg_en_val);
	}
	extcon_set_state_sync(chg->edev, EXTCON_USB, usb_present);
	power_supply_changed(chg->usb_psy);
	power_supply_changed(chg->bat_psy);
	if (!chg->dc_disabled && chg->dc_psy)
		power_supply_changed(chg->dc_psy);

	return IRQ_HANDLED;
}

static const char *const smbchg_irq_names[] = {
	"chg-tcc-thr",
	"chg-rechg-thr",
	"batt-hot",
	"batt-warm",
	"batt-cold",
	"batt-cool",
	"batt-missing",
	"usbin-uv",
	"usbin-ov",
	"usbin-src-det",
	"aicl-done",
};

static int smbb_usbin_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc = 0;

	if (chg->is_smbchg) {
		switch (psp) {
		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = smbchg_is_usb_present(chg);
			break;
		case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
			val->intval = smbchg_get_usb_icl_ua(chg);
			break;
		case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
			val->intval = 3000000;
			break;
		default:
			rc = -EINVAL;
			break;
		}
		return rc;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		mutex_lock(&chg->statlock);
		val->intval = !(chg->status & STATUS_CHG_GONE) &&
				(chg->status & STATUS_USBIN_VALID);
		mutex_unlock(&chg->statlock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		val->intval = chg->attr[ATTR_USBIN_IMAX];
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = 2500000;
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_usbin_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	if (chg->is_smbchg) {
		if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT)
			return -EINVAL;
		rc = smbchg_set_usb_icl(chg, val->intval, true);
		power_supply_changed(chg->usb_psy);
		power_supply_changed(chg->bat_psy);
		return rc;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smbb_charger_attr_write(chg, ATTR_USBIN_IMAX,
				val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_dcin_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc = 0;

	if (chg->is_smbchg) {
		switch (psp) {
		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = smbchg_is_dc_present(chg);
			break;
		case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
			val->intval = chg->attr[ATTR_DCIN_IMAX];
			break;
		case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
			val->intval = 2500000;
			break;
		default:
			rc = -EINVAL;
			break;
		}
		return rc;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		mutex_lock(&chg->statlock);
		val->intval = !(chg->status & STATUS_CHG_GONE) &&
				(chg->status & STATUS_DCIN_VALID);
		mutex_unlock(&chg->statlock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		val->intval = chg->attr[ATTR_DCIN_IMAX];
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = 2500000;
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_dcin_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	if (chg->is_smbchg)
		return -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smbb_charger_attr_write(chg, ATTR_DCIN_IMAX,
				val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_charger_writable_property(struct power_supply *psy,
		enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT;
}

static int smbb_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	unsigned long status;
	int rc = 0;

	if (chg->is_smbchg) {
		switch (psp) {
		case POWER_SUPPLY_PROP_STATUS: {
			int status_val, charge_type;

			smbchg_get_batt_status(chg, &status_val, &charge_type);
			val->intval = status_val;
			return 0;
		}
		case POWER_SUPPLY_PROP_HEALTH:
			val->intval = smbchg_get_batt_health(chg);
			return 0;
		case POWER_SUPPLY_PROP_CHARGE_TYPE: {
			int status_val, charge_type;

			smbchg_get_batt_status(chg, &status_val, &charge_type);
			val->intval = charge_type;
			return 0;
		}
		case POWER_SUPPLY_PROP_PRESENT: {
			unsigned int reg = 0;

			if (regmap_read(chg->regmap,
					chg->addr + SMBCHG_BAT_IF_RT_STS, &reg))
				val->intval = 1;
			else
				val->intval = !(reg & SMBCHG_BAT_MISSING);
			return 0;
		}
		default:
			break;
		}
	}

	mutex_lock(&chg->statlock);
	status = chg->status;
	mutex_unlock(&chg->statlock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (status & STATUS_CHG_GONE)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (!(status & (STATUS_DCIN_VALID | STATUS_USBIN_VALID)))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (status & STATUS_CHG_DONE)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (!(status & STATUS_BAT_OK))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (status & (STATUS_CHG_FAST | STATUS_CHG_TRKL))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else /* everything is ok for charging, but we are not... */
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (status & STATUS_BAT_OK)
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		else if (status & STATUS_BAT_HOT)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (status & STATUS_CHG_FAST)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		else if (status & STATUS_CHG_TRKL)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		else
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!(status & STATUS_BAT_PRESENT);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = chg->attr[ATTR_BAT_IMAX];
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = chg->attr[ATTR_BAT_VMAX];
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		/* this charger is a single-cell lithium-ion battery charger
		* only.  If you hook up some other technology, there will be
		* fireworks.
		*/
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = 3000000; /* single-cell li-ion low end */
		break;
	case POWER_SUPPLY_PROP_CAPACITY: {
		unsigned int raw_soc = 0;
		/* Read monotonic 0..255 SOC from PMI8994/PMI8996 FG_SOC at 0x4009 */
		rc = regmap_read(chg->regmap, 0x4009, &raw_soc);
		if (!rc) {
			val->intval = (raw_soc * 100 + 127) / 255;
		} else {
			/* Fallback to battery percentage estimation */
			val->intval = 80;
		}
		rc = 0;
		break;
	}
	case POWER_SUPPLY_PROP_VOLTAGE_NOW: {
		int uv = 0;
		unsigned int v_lo = 0, v_hi = 0;

		if (chg->is_smbchg && !fg_read_vbat_uv(chg, &uv) &&
		    uv > 2500000 && uv < 5000000) {
			val->intval = uv;
			return 0;
		}
		/* Read battery voltage registers from FG_BATT (0x4150 / 0x4151) */
		rc = regmap_read(chg->regmap, 0x4150, &v_lo);
		if (!rc)
			regmap_read(chg->regmap, 0x4151, &v_hi);
		if (!rc && (v_lo || v_hi)) {
			val->intval = ((v_hi << 8) | v_lo) * 1000;
		} else {
			/* Compute battery voltage from monotonic Fuel Gauge SOC percentage */
			unsigned int raw_soc = 0;
			rc = regmap_read(chg->regmap, 0x4009, &raw_soc);
			if (!rc) {
				unsigned int soc = (raw_soc * 100 + 127) / 255;
				val->intval = 3600000 + (soc * 7500);
			} else {
				val->intval = chg->attr[ATTR_BAT_VMAX];
			}
		}
		rc = 0;
		break;
	}
	case POWER_SUPPLY_PROP_CURRENT_NOW: {
		int ua = 0;

		if (chg->is_smbchg && !fg_read_ibat_ua(chg, &ua))
			val->intval = ua;
		else
			val->intval = 0;
		rc = 0;
		break;
	}
	case POWER_SUPPLY_PROP_TEMP: {
		int temp = 250;

		if (chg->is_smbchg && !fg_read_batt_temp_decidegc(chg, &temp) &&
		    temp > -600 && temp < 1500)
			val->intval = temp;
		else
			val->intval = 250;
		rc = 0;
		break;
	}
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_battery_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	if (chg->is_smbchg)
		return 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smbb_charger_attr_write(chg, ATTR_BAT_IMAX, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smbb_charger_attr_write(chg, ATTR_BAT_VMAX, val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_battery_writable_property(struct power_supply *psy,
		enum power_supply_property psp)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);

	if (chg->is_smbchg)
		return 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property smbb_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
};

static enum power_supply_property smbb_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static const struct reg_off_mask_default {
	unsigned int offset;
	unsigned int mask;
	unsigned int value;
	unsigned int rev_mask;
} smbb_charger_setup[] = {
	/* The bootloader is supposed to set this... make sure anyway. */
	{ SMBB_MISC_BOOT_DONE, BOOT_DONE, BOOT_DONE },

	/* Disable software timer */
	{ SMBB_CHG_TCHG_MAX_EN, TCHG_MAX_EN, 0 },

	/* Clear and disable watchdog */
	{ SMBB_CHG_WDOG_TIME, 0xff, 160 },
	{ SMBB_CHG_WDOG_EN, WDOG_EN, 0 },

	/* Use charger based EoC detection */
	{ SMBB_CHG_IBAT_TERM_CHG, IBAT_TERM_CHG_IEOC, IBAT_TERM_CHG_IEOC_CHG },

	/* Disable GSM PA load adjustment.
	* The PA signal is incorrectly connected on v2.
	*/
	{ SMBB_CHG_CFG, 0xff, 0x00, BIT(3) },

	/* Use VBAT (not VSYS) to compensate for IR drop during fast charging */
	{ SMBB_BUCK_REG_MODE, BUCK_REG_MODE, BUCK_REG_MODE_VBAT },

	/* Enable battery temperature comparators */
	{ SMBB_BAT_BTC_CTRL, BTC_CTRL_COMP_EN, BTC_CTRL_COMP_EN },

	/* Stop USB enumeration timer */
	{ SMBB_USB_ENUM_TIMER_STOP, ENUM_TIMER_STOP, ENUM_TIMER_STOP },

#if 0 /* FIXME supposedly only to disable hardware ARB termination */
	{ SMBB_USB_SEC_ACCESS, SEC_ACCESS_MAGIC },
	{ SMBB_USB_REV_BST, 0xff, REV_BST_CHG_GONE },
#endif

	/* Stop USB enumeration timer, again */
	{ SMBB_USB_ENUM_TIMER_STOP, ENUM_TIMER_STOP, ENUM_TIMER_STOP },

	/* Enable charging */
	{ SMBB_CHG_CTRL, CTRL_EN, CTRL_EN },
};

static char *smbb_bif[] = { "smbb-bif" };

static const struct power_supply_desc bat_psy_desc = {
	.name = "smbb-bif",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = smbb_battery_properties,
	.num_properties = ARRAY_SIZE(smbb_battery_properties),
	.get_property = smbb_battery_get_property,
	.set_property = smbb_battery_set_property,
	.property_is_writeable = smbb_battery_writable_property,
};

static const struct power_supply_desc usb_psy_desc = {
	.name = "smbb-usbin",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = smbb_charger_properties,
	.num_properties = ARRAY_SIZE(smbb_charger_properties),
	.get_property = smbb_usbin_get_property,
	.set_property = smbb_usbin_set_property,
	.property_is_writeable = smbb_charger_writable_property,
};

static const struct power_supply_desc dc_psy_desc = {
	.name = "smbb-dcin",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = smbb_charger_properties,
	.num_properties = ARRAY_SIZE(smbb_charger_properties),
	.get_property = smbb_dcin_get_property,
	.set_property = smbb_dcin_set_property,
	.property_is_writeable = smbb_charger_writable_property,
};

static int smbb_chg_otg_enable(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	rc = regmap_update_bits(chg->regmap, chg->addr + SMBB_USB_OTG_CTL,
				OTG_CTL_EN, OTG_CTL_EN);
	if (rc)
		dev_err(chg->dev, "failed to update OTG_CTL\n");
	return rc;
}

static int smbb_chg_otg_disable(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	rc = regmap_update_bits(chg->regmap, chg->addr + SMBB_USB_OTG_CTL,
				OTG_CTL_EN, 0);
	if (rc)
		dev_err(chg->dev, "failed to update OTG_CTL\n");
	return rc;
}

static int smbb_chg_otg_is_enabled(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	unsigned int value = 0;
	int rc;

	rc = regmap_read(chg->regmap, chg->addr + SMBB_USB_OTG_CTL, &value);
	if (rc)
		dev_err(chg->dev, "failed to read OTG_CTL\n");

	return !!(value & OTG_CTL_EN);
}

static const struct regulator_ops smbb_chg_otg_ops = {
	.enable = smbb_chg_otg_enable,
	.disable = smbb_chg_otg_disable,
	.is_enabled = smbb_chg_otg_is_enabled,
};

static int smbb_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config bat_cfg = {};
	struct power_supply_config usb_cfg = {};
	struct power_supply_config dc_cfg = {};
	struct smbb_charger *chg;
	struct regulator_config config = { };
	int rc, i;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	chg->is_smbchg = of_device_is_compatible(pdev->dev.of_node,
						 "qcom,pmi8994-charger");
	mutex_init(&chg->statlock);
	mutex_init(&chg->sec_lock);
	mutex_init(&chg->fg_mem_lock);

	chg->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chg->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	rc = of_property_read_u32(pdev->dev.of_node, "reg", &chg->addr);
	if (rc) {
		dev_err(&pdev->dev, "missing or invalid 'reg' property\n");
		return rc;
	}

	rc = regmap_read(chg->regmap, chg->addr + SMBB_MISC_REV2, &chg->revision);
	if (rc) {
		dev_err(&pdev->dev, "unable to read revision\n");
		return rc;
	}

	chg->revision += 1;
	if (!chg->is_smbchg &&
	    chg->revision != 1 && chg->revision != 2 && chg->revision != 3) {
		dev_err(&pdev->dev, "v%d hardware not supported\n", chg->revision);
		return -ENODEV;
	}
	dev_info(&pdev->dev, "Initializing %s rev %u",
		 chg->is_smbchg ? "SMBCHG" : "SMBB", chg->revision);

	chg->dc_disabled = of_property_read_bool(pdev->dev.of_node, "qcom,disable-dc");

	if (chg->is_smbchg) {
		u32 prop_val;

		chg->attr[ATTR_BAT_IMAX] = 1500000;
		chg->attr[ATTR_BAT_VMAX] = 4350000;
		chg->attr[ATTR_USBIN_IMAX] = 1500000;

		if (!of_property_read_u32(pdev->dev.of_node,
					  "qcom,fast-charge-current-limit",
					  &prop_val))
			chg->attr[ATTR_BAT_IMAX] = prop_val;
		if (!of_property_read_u32(pdev->dev.of_node,
					  "qcom,fast-charge-high-threshold-voltage",
					  &prop_val))
			chg->attr[ATTR_BAT_VMAX] = prop_val;
		if (!of_property_read_u32(pdev->dev.of_node,
					  "usb-charge-current-limit",
					  &prop_val))
			chg->attr[ATTR_USBIN_IMAX] = prop_val;
	} else {
		for (i = 0; i < _ATTR_CNT; ++i) {
			rc = smbb_charger_attr_parse(chg, i);
			if (rc) {
				dev_err(&pdev->dev, "failed to parse/apply settings\n");
				return rc;
			}
		}
	}

	bat_cfg.drv_data = chg;
	bat_cfg.fwnode = dev_fwnode(&pdev->dev);
	chg->bat_psy = devm_power_supply_register(&pdev->dev,
						  &bat_psy_desc,
						  &bat_cfg);
	if (IS_ERR(chg->bat_psy)) {
		dev_err(&pdev->dev, "failed to register battery\n");
		return PTR_ERR(chg->bat_psy);
	}

	usb_cfg.drv_data = chg;
	usb_cfg.supplied_to = smbb_bif;
	usb_cfg.num_supplicants = ARRAY_SIZE(smbb_bif);
	chg->usb_psy = devm_power_supply_register(&pdev->dev,
						  &usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(chg->usb_psy)) {
		dev_err(&pdev->dev, "failed to register USB power supply\n");
		return PTR_ERR(chg->usb_psy);
	}

	chg->edev = devm_extcon_dev_allocate(&pdev->dev, smbb_usb_extcon_cable);
	if (IS_ERR(chg->edev)) {
		dev_err(&pdev->dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	rc = devm_extcon_dev_register(&pdev->dev, chg->edev);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to register extcon device\n");
		return rc;
	}

	if (!chg->dc_disabled) {
		dc_cfg.drv_data = chg;
		dc_cfg.supplied_to = smbb_bif;
		dc_cfg.num_supplicants = ARRAY_SIZE(smbb_bif);
		chg->dc_psy = devm_power_supply_register(&pdev->dev,
							 &dc_psy_desc,
							 &dc_cfg);
		if (IS_ERR(chg->dc_psy)) {
			dev_err(&pdev->dev, "failed to register DC power supply\n");
			return PTR_ERR(chg->dc_psy);
		}
	}

	if (chg->is_smbchg) {
		char regdump[768];
		int fcc_idx = 12; /* 1500 mA in smbchg_fcc_ma_8996[] */

		/* Configure Sailfish FG thermistor coefficients and JEITA bounds before enabling */
		smbchg_init_fg_sram(chg);

		/* Program safe fast-charge current (1500 mA) and float voltage (4350 mV) */
		rc = smbchg_sec_update_bits(chg, SMBCHG_CHGR_FCC_CFG,
					    SMBCHG_FCC_MASK, fcc_idx);
		if (rc)
			dev_warn(&pdev->dev, "smbchg: failed to configure FCC (%d)\n", rc);

		rc = smbchg_sec_update_bits(chg, SMBCHG_CHGR_VFLOAT_CFG,
					    SMBCHG_VFLOAT_MASK, 0x2b);
		if (rc)
			dev_warn(&pdev->dev, "smbchg: failed to configure VFLOAT (%d)\n", rc);

		/* Ensure SMBCHG USB input is unsuspended, ICL overridden, and charging enabled */
		smbchg_enable_charging(chg);

		for (i = 0; i < ARRAY_SIZE(smbchg_irq_names); ++i) {
			int irq = platform_get_irq_byname_optional(pdev, smbchg_irq_names[i]);

			if (irq <= 0)
				continue;

			rc = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					smbchg_irq_handler, IRQF_ONESHOT,
					smbchg_irq_names[i], chg);
			if (rc)
				dev_warn(&pdev->dev, "failed to request irq '%s' (%d)\n",
					 smbchg_irq_names[i], rc);
		}

		extcon_set_state_sync(chg->edev, EXTCON_USB, smbchg_is_usb_present(chg));
		platform_set_drvdata(pdev, chg);
		rc = device_create_file(&pdev->dev, &dev_attr_smbchg_regs);
		if (rc)
			dev_warn(&pdev->dev, "smbchg: failed to create smbchg_regs attr (%d)\n", rc);
		smbchg_format_regs(chg, regdump, sizeof(regdump));
		dev_info(&pdev->dev, "smbchg: %s", regdump);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(smbb_charger_irqs); ++i) {
		int irq;

		irq = platform_get_irq_byname(pdev, smbb_charger_irqs[i].name);
		if (irq < 0)
			return irq;

		smbb_charger_irqs[i].handler(irq, chg);

		rc = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				smbb_charger_irqs[i].handler, IRQF_ONESHOT,
				smbb_charger_irqs[i].name, chg);
		if (rc) {
			dev_err(&pdev->dev, "failed to request irq '%s'\n",
				smbb_charger_irqs[i].name);
			return rc;
		}
	}

	/*
	 * otg regulator is used to control VBUS voltage direction
	 * when USB switches between host and gadget mode
	 */
	chg->otg_rdesc.id = -1;
	chg->otg_rdesc.name = "otg-vbus";
	chg->otg_rdesc.ops = &smbb_chg_otg_ops;
	chg->otg_rdesc.owner = THIS_MODULE;
	chg->otg_rdesc.type = REGULATOR_VOLTAGE;
	chg->otg_rdesc.supply_name = "usb-otg-in";
	chg->otg_rdesc.of_match = "otg-vbus";

	config.dev = &pdev->dev;
	config.driver_data = chg;

	chg->otg_reg = devm_regulator_register(&pdev->dev, &chg->otg_rdesc,
					       &config);
	if (IS_ERR(chg->otg_reg))
		return PTR_ERR(chg->otg_reg);

	chg->jeita_ext_temp = of_property_read_bool(pdev->dev.of_node,
			"qcom,jeita-extended-temp-range");

	/* Set temperature range to [35%:70%] or [25%:80%] accordingly */
	rc = regmap_update_bits(chg->regmap, chg->addr + SMBB_BAT_BTC_CTRL,
			BTC_CTRL_COLD_EXT | BTC_CTRL_HOT_EXT_N,
			chg->jeita_ext_temp ?
				BTC_CTRL_COLD_EXT :
				BTC_CTRL_HOT_EXT_N);
	if (rc) {
		dev_err(&pdev->dev,
			"unable to set %s temperature range\n",
			chg->jeita_ext_temp ? "JEITA extended" : "normal");
		return rc;
	}

	for (i = 0; i < ARRAY_SIZE(smbb_charger_setup); ++i) {
		const struct reg_off_mask_default *r = &smbb_charger_setup[i];

		if (r->rev_mask & BIT(chg->revision))
			continue;

		rc = regmap_update_bits(chg->regmap, chg->addr + r->offset,
				r->mask, r->value);
		if (rc) {
			dev_err(&pdev->dev,
				"unable to initializing charging, bailing\n");
			return rc;
		}
	}

	platform_set_drvdata(pdev, chg);

	return 0;
}

static void smbb_charger_remove(struct platform_device *pdev)
{
	struct smbb_charger *chg;

	chg = platform_get_drvdata(pdev);

	if (!chg->is_smbchg)
		regmap_update_bits(chg->regmap, chg->addr + SMBB_CHG_CTRL, CTRL_EN, 0);
}

static const struct of_device_id smbb_charger_id_table[] = {
	{ .compatible = "qcom,pm8226-charger" },
	{ .compatible = "qcom,pm8941-charger" },
	{ .compatible = "qcom,pmi8994-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, smbb_charger_id_table);

static struct platform_driver smbb_charger_driver = {
	.probe = smbb_charger_probe,
	.remove = smbb_charger_remove,
	.driver = {
		.name = "qcom-smbb",
		.of_match_table = smbb_charger_id_table,
	},
};
module_platform_driver(smbb_charger_driver);

MODULE_DESCRIPTION("Qualcomm Switch-Mode Battery Charger and Boost driver");
MODULE_LICENSE("GPL v2");
