// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple qualcomm high voltage haptics driver
 *
 * Copyright (C) 2026, Vasiliy Doylov <neko@altlinux.org>
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_address.h>

/* HAPTICS_CFG module registers */
#define HAP_CFG_EN_CTL_REG		0x46
#define HAPTICS_EN_BIT			BIT(7)

#define HAP_CFG_VMAX_REG		0x48
#define VMAX_HV_STEP_MV			50

#define HAP_CFG_DRV_WF_SEL_REG		0x49
#define DRV_WF_SEL_MASK			GENMASK(1, 0)

#define HAP_CFG_SPMI_PLAY_REG		0x4C
#define PLAY_EN_BIT			BIT(7)
#define PAT_SRC_MASK			GENMASK(2, 0)

#define HAP_CFG_BRAKE_MODE_CFG_REG	0x50
#define BRAKE_MODE_MASK			GENMASK(7, 6)
#define BRAKE_MODE_SHIFT		6

#define HAP_CFG_FAULT_CLR_REG		0x66
#define SC_CLR_BIT			BIT(2)
#define AUTO_RES_ERR_CLR_BIT		BIT(1)
#define HPWR_RDY_FAULT_CLR_BIT		BIT(0)

/* HAPTICS_PATTERN module registers */
#define HAP_PTN_DIRECT_PLAY_REG		0x26
#define DIRECT_PLAY_MAX_AMPLITUDE	0xFF

enum pattern_src {
	FIFO_SRC,
	DIRECT_PLAY,
	PATTERN1,
	PATTERN2,
};

enum brake_mode {
	OL_BRAKE,
	CL_BRAKE,
	PREDICT_BRAKE,
	AUTO_BRAKE,
};

struct qcom_hv_hap {
	struct device		*dev;
	struct regmap		*regmap;
	struct input_dev	*input;
	u32			cfg_addr;
	u32			ptn_addr;
	u32			vmax_mv;
};

static int hap_module_enable(struct qcom_hv_hap *hap, bool en)
{
	return regmap_update_bits(hap->regmap,
				  hap->cfg_addr + HAP_CFG_EN_CTL_REG,
				  HAPTICS_EN_BIT, en ? HAPTICS_EN_BIT : 0);
}

static int hap_set_play(struct qcom_hv_hap *hap, bool en, enum pattern_src src)
{
	u8 val = src & PAT_SRC_MASK;

	if (en)
		val |= PLAY_EN_BIT;

	return regmap_write(hap->regmap,
			    hap->cfg_addr + HAP_CFG_SPMI_PLAY_REG, val);
}

static void hap_clear_faults(struct qcom_hv_hap *hap)
{
	u8 val = SC_CLR_BIT | AUTO_RES_ERR_CLR_BIT | HPWR_RDY_FAULT_CLR_BIT;

	regmap_write(hap->regmap, hap->cfg_addr + HAP_CFG_FAULT_CLR_REG, val);
}

static int hap_set_direct_play(struct qcom_hv_hap *hap, u8 amp)
{
	return regmap_write(hap->regmap, hap->ptn_addr + HAP_PTN_DIRECT_PLAY_REG, amp);
}

static int qcom_hv_hap_play_effect(struct input_dev *input, void *data,
				   struct ff_effect *effect)
{
	struct qcom_hv_hap *hap = input_get_drvdata(input);
	u16 mag = effect->u.rumble.strong_magnitude;
	u8 amp;

	if (!mag) {
		hap_set_play(hap, false, DIRECT_PLAY);	
		return 0;
	}

	amp = mag * DIRECT_PLAY_MAX_AMPLITUDE / 0xffff;

	hap_clear_faults(hap);
	hap_set_direct_play(hap, (u8)amp);
	hap_set_play(hap, true, DIRECT_PLAY);

	return 0;
}

static void qcom_hv_hap_close(struct input_dev *input)
{
	struct qcom_hv_hap *hap = input_get_drvdata(input);

	hap_set_play(hap, false, FIFO_SRC);
}

static int qcom_hv_hap_parse_dt(struct qcom_hv_hap *hap)
{
	struct device *dev = hap->dev;
	struct device_node *node = dev->of_node;
	const __be32 *addr;
	u32 tmp;

	addr = of_get_address(node, 0, NULL, NULL);
	if (!addr) {
		dev_err(dev, "missing cfg reg base\n");
		return -EINVAL;
	}
	hap->cfg_addr = be32_to_cpu(*addr);

	addr = of_get_address(node, 1, NULL, NULL);
	if (!addr) {
		dev_err(dev, "missing pattern reg base\n");
		return -EINVAL;
	}
	hap->ptn_addr = be32_to_cpu(*addr);

	hap->vmax_mv = 2000;
	if (!of_property_read_u32(node, "qcom,vmax-mv", &tmp))
		hap->vmax_mv = min_t(u32, tmp, 10000);

	return 0;
}

static int qcom_hv_hap_hw_init(struct qcom_hv_hap *hap)
{
	u32 vmax;

	hap_module_enable(hap, true);

	vmax = hap->vmax_mv / VMAX_HV_STEP_MV;
	regmap_write(hap->regmap, hap->cfg_addr + HAP_CFG_VMAX_REG, vmax);


	regmap_update_bits(hap->regmap,
				hap->cfg_addr + HAP_CFG_DRV_WF_SEL_REG,
				DRV_WF_SEL_MASK, 0x00);

	regmap_update_bits(hap->regmap,
			   hap->cfg_addr + HAP_CFG_BRAKE_MODE_CFG_REG,
			   BRAKE_MODE_MASK,
			   AUTO_BRAKE << BRAKE_MODE_SHIFT);

	hap_clear_faults(hap);
	return 0;
}

static int qcom_hv_hap_probe(struct platform_device *pdev)
{
	struct qcom_hv_hap *hap;
	struct input_dev *input;
	int rc;

	hap = devm_kzalloc(&pdev->dev, sizeof(*hap), GFP_KERNEL);
	if (!hap)
		return -ENOMEM;

	hap->dev = &pdev->dev;

	hap->regmap = dev_get_regmap(pdev->dev.parent, NULL);

	if (!hap->regmap)
		return dev_err_probe(&pdev->dev, -ENXIO, "no regmap\n");

	rc = qcom_hv_hap_parse_dt(hap);
	if (rc)
		return rc;

	rc = qcom_hv_hap_hw_init(hap);
	if (rc)
		return rc;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	input->name = "qcom-hv-haptics";
	input->close = qcom_hv_hap_close;
	input_set_drvdata(input, hap);

	input_set_capability(input, EV_FF, FF_RUMBLE);

	rc = input_ff_create_memless(input, NULL, qcom_hv_hap_play_effect);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "failed to create memless input device\n");

	rc = input_register_device(input);
	if (rc) {
		input_ff_destroy(input);
		return rc;
	}

	platform_set_drvdata(pdev, hap);
	hap->input = input;

	dev_info(&pdev->dev, "probed (vmax=%umV)\n", hap->vmax_mv);
	return 0;
}

static void qcom_hv_hap_remove(struct platform_device *pdev)
{
	struct qcom_hv_hap *hap = platform_get_drvdata(pdev);

	hap_set_play(hap, false, FIFO_SRC);
	hap_module_enable(hap, false);
}

static int __maybe_unused qcom_hv_hap_suspend(struct device *dev)
{
	struct qcom_hv_hap *hap = dev_get_drvdata(dev);

	hap_set_play(hap, false, FIFO_SRC);
	hap_module_enable(hap, false);
	return 0;
}

static int __maybe_unused qcom_hv_hap_resume(struct device *dev)
{
	struct qcom_hv_hap *hap = dev_get_drvdata(dev);

	return hap_module_enable(hap, true);
}

static SIMPLE_DEV_PM_OPS(qcom_hv_hap_pm_ops,
			  qcom_hv_hap_suspend, qcom_hv_hap_resume);

static const struct of_device_id qcom_hv_hap_of_match[] = {
	{ .compatible = "qcom,hv-haptics" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_hv_hap_of_match);

static struct platform_driver qcom_hv_hap_driver = {
	.driver = {
		.name = "qcom-hv-haptics",
		.of_match_table = qcom_hv_hap_of_match,
		.pm = &qcom_hv_hap_pm_ops,
	},
	.probe  = qcom_hv_hap_probe,
	.remove = qcom_hv_hap_remove,
};
module_platform_driver(qcom_hv_hap_driver);

MODULE_AUTHOR("Vasiliy Doylov <neko@altlinux.org>");
MODULE_DESCRIPTION("Qualcomm HV Haptics Driver");
MODULE_LICENSE("GPL v2");
