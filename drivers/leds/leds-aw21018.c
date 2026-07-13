// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW21018 LED driver
 *
 * 18-channel I2C LED driver with 8/9/12-bit PWM brightness resolution.
 *
 * Copyright (c) 2026 Vasiliy Doylov <neko@altlinux.org>
 */

#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

/* GCR (0x20) - Global Control Register */
#define AW21018_REG_GCR			0x20
#define AW21018_GCR_APSE			BIT(7)
#define AW21018_GCR_CLKFRQ_MASK		GENMASK(6, 4)
#define AW21018_GCR_PWMRES_MASK		GENMASK(2, 1)
#define AW21018_GCR_CHIPEN			BIT(0)

/* Brightness registers: channel N low byte at 0x21+N*2, high byte at 0x22+N*2 */
#define AW21018_REG_BR_BASE		0x21
#define AW21018_BR_STRIDE			2

/* Color/scale registers: channel N at 0x46+N */
#define AW21018_REG_SL_BASE		0x46

/* Update register - write 0x00 to latch brightness/color values */
#define AW21018_REG_UPDATE			0x45

/* Global Current Control Register */
#define AW21018_REG_GCCR			0x58

/* UVCR (0x60) - Under-Voltage Control Register */
#define AW21018_REG_UVCR			0x60
#define AW21018_UVCR_UVDIS			BIT(0)
#define AW21018_UVCR_UVPD			BIT(1)

/* GCR2 (0x61) - Global Control Register 2 */
#define AW21018_REG_GCR2			0x61
#define AW21018_GCR2_RGBMD			BIT(0)
#define AW21018_GCR2_SBMD			BIT(1)

/* GCFG (0x8B) - Group Configuration Register */
#define AW21018_REG_GCFG			0x8B
#define AW21018_GCFG_GROUP_EN		GENMASK(5, 0)
#define AW21018_GCFG_GROUP_DIS		BIT(6)

/* Chip ID register */
#define AW21018_REG_CHIPID			0x70
#define AW21018_CHIPID				0x02

#define AW21018_REG_MAX			0x8B

#define AW21018_NUM_CHANNELS		18

/* GCR PWMRES field values */
#define AW21018_PWMRES_8BIT		0
#define AW21018_PWMRES_9BIT		1
#define AW21018_PWMRES_12BIT		2
#define AW21018_PWMRES_9_3BIT		3

static const unsigned int aw21018_pwmres_max[] = {
	[AW21018_PWMRES_8BIT]  = 255,
	[AW21018_PWMRES_9BIT]  = 511,
	[AW21018_PWMRES_12BIT] = 4095,
	[AW21018_PWMRES_9_3BIT] = 4095,
};

static const struct regmap_config aw21018_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW21018_REG_MAX,
};

struct aw21018;

struct aw21018_led {
	struct led_classdev cdev;
	struct aw21018 *chip;
	int channel;
};

struct aw21018 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex mutex;
	struct gpio_desc *enable;
	u8 global_current;
	u8 sl_values[AW21018_NUM_CHANNELS];
	bool has_sl_values;
	u8 osc_clk;
	u8 br_res;
	unsigned int max_brightness;
	unsigned int num_leds;
	struct aw21018_led leds[] __counted_by(num_leds);
};

static int aw21018_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct aw21018_led *led = container_of(cdev, struct aw21018_led, cdev);
	struct aw21018 *chip = led->chip;
	unsigned int br_l = AW21018_REG_BR_BASE +
			    led->channel * AW21018_BR_STRIDE;
	unsigned int br_h = br_l + 1;
	int ret;

	mutex_lock(&chip->mutex);

	ret = regmap_write(chip->regmap, br_l, brightness & 0xff);
	if (ret)
		goto unlock;

	ret = regmap_write(chip->regmap, br_h, (brightness >> 8) & 0x0f);
	if (ret)
		goto unlock;

	ret = regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);

unlock:
	mutex_unlock(&chip->mutex);

	return ret;
}

static int aw21018_chip_init(struct aw21018 *chip)
{
	int ret;
	unsigned int i;
	u8 gcr = AW21018_GCR_CHIPEN;

	/* CLKFRQ + PWMRES */
	gcr |= (chip->osc_clk << 4) & AW21018_GCR_CLKFRQ_MASK;
	gcr |= (chip->br_res << 1) & AW21018_GCR_PWMRES_MASK;

	/* CHIPEN + CLKFRQ + PWMRES */
	ret = regmap_write(chip->regmap, AW21018_REG_GCR, gcr);
	if (ret)
		return ret;

	/* GCR2: dual-byte mode, per-channel (not RGB) */
	ret = regmap_write(chip->regmap, AW21018_REG_GCR2, 0);
	if (ret)
		return ret;

	/* Global current */
	ret = regmap_write(chip->regmap, AW21018_REG_GCCR,
			   chip->global_current);
	if (ret)
		return ret;

	/* Disable under-voltage lockout */
	ret = regmap_set_bits(chip->regmap, AW21018_REG_UVCR,
			      AW21018_UVCR_UVPD | AW21018_UVCR_UVDIS);
	if (ret)
		return ret;

	/* APSE */
	ret = regmap_set_bits(chip->regmap, AW21018_REG_GCR,
			       AW21018_GCR_APSE);
	if (ret)
		return ret;

	/* GCFG: group disable for per-channel control */
	ret = regmap_write(chip->regmap, AW21018_REG_GCFG,
			    AW21018_GCFG_GROUP_DIS);
	if (ret)
		return ret;

	/* Set SL registers for per-channel calibration */
	for (i = 0; i < AW21018_NUM_CHANNELS; i++) {
		u8 sl = chip->has_sl_values ? chip->sl_values[i] : 0xff;

		ret = regmap_write(chip->regmap, AW21018_REG_SL_BASE + i, sl);
		if (ret)
			return ret;
	}

	return regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);
}

static void aw21018_chip_off(void *data)
{
	struct aw21018 *chip = data;
	unsigned int i;

	for (i = 0; i < chip->num_leds; i++)
		regmap_write(chip->regmap,
			     AW21018_REG_BR_BASE +
			     chip->leds[i].channel * AW21018_BR_STRIDE, 0);

	regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);
	regmap_clear_bits(chip->regmap, AW21018_REG_GCR, AW21018_GCR_CHIPEN);
}

static int aw21018_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw21018 *chip;
	unsigned int chipid;
	u32 val;
	int count, i = 0, ret;

	count = device_get_child_node_count(dev);
	if (!count || count > AW21018_NUM_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid number of LEDs (%d)\n", count);

	chip = devm_kzalloc(dev, struct_size(chip, leds, count), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->num_leds = count;
	chip->global_current = 0x66;
	chip->br_res = AW21018_PWMRES_8BIT;
	chip->max_brightness = 255;
	i2c_set_clientdata(client, chip);

	device_property_read_u8(dev, "awinic,global-current",
			       &chip->global_current);

	if (!device_property_read_u32(dev, "awinic,osc-clock", &val))
		chip->osc_clk = val;

	if (!device_property_read_u32(dev, "awinic,brightness-resolution", &val))
		chip->br_res = val;

	if (chip->br_res <= AW21018_PWMRES_9_3BIT)
		chip->max_brightness = aw21018_pwmres_max[chip->br_res];

	chip->has_sl_values = !device_property_read_u8_array(dev,
					"awinic,sl-values",
					chip->sl_values,
					AW21018_NUM_CHANNELS);

	chip->regmap = devm_regmap_init_i2c(client, &aw21018_regmap);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "Failed to initialize regmap\n");

	chip->enable = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chip->enable))
		return dev_err_probe(dev, PTR_ERR(chip->enable),
				     "Cannot get enable GPIO\n");

	if (chip->enable) {
		gpiod_set_value_cansleep(chip->enable, 1);
		usleep_range(2000, 2500);
	}

	ret = regmap_read(chip->regmap, AW21018_REG_CHIPID, &chipid);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");

	if (chipid != AW21018_CHIPID)
		return dev_err_probe(dev, -ENODEV,
				     "Unexpected chip ID: 0x%02x\n", chipid);

	ret = devm_mutex_init(dev, &chip->mutex);
	if (ret)
		return ret;

	ret = devm_add_action(dev, aw21018_chip_off, chip);
	if (ret)
		return ret;

	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init_data = {};
		struct aw21018_led *led;
		u32 reg;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret || reg >= AW21018_NUM_CHANNELS) {
			dev_err(dev, "Invalid reg property for node %pfw\n",
				child);
			return -EINVAL;
		}

		led = &chip->leds[i];
		led->channel = reg;
		led->chip = chip;
		led->cdev.brightness_set_blocking = aw21018_brightness_set;
		led->cdev.max_brightness = chip->max_brightness;

		init_data.fwnode = child;

		ret = devm_led_classdev_register_ext(dev, &led->cdev,
						     &init_data);
		if (ret) {
			dev_err(dev, "Failed to register LED %pfw\n", child);
			return ret;
		}

		i++;
	}

	return aw21018_chip_init(chip);
}

static const struct of_device_id aw21018_of_match[] = {
	{ .compatible = "awinic,aw21018" },
	{}
};
MODULE_DEVICE_TABLE(of, aw21018_of_match);

static const struct i2c_device_id aw21018_id[] = {
	{ "aw21018" },
	{}
};
MODULE_DEVICE_TABLE(i2c, aw21018_id);

static struct i2c_driver aw21018_driver = {
	.driver = {
		.name = "leds-aw21018",
		.of_match_table = aw21018_of_match,
	},
	.probe = aw21018_probe,
	.id_table = aw21018_id,
};
module_i2c_driver(aw21018_driver);

MODULE_AUTHOR("Vasiliy Doylov <neko@altlinux.org>");
MODULE_DESCRIPTION("Awinic AW21018 LED driver");
MODULE_LICENSE("GPL v2");
