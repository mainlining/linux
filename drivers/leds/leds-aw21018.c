// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW21018 LED driver
 *
 * 18-channel I2C LED driver with 12-bit PWM brightness resolution.
 *
 * Copyright (c) 2026 Vasiliy Doylov <neko@altlinux.org>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

/* GCR - Global Control Register */
#define AW21018_REG_GCR			0x20

/* Brightness registers: channel N low byte at 0x21+N*2, high byte at 0x22+N*2 */
#define AW21018_REG_BR_BASE		0x21
#define AW21018_BR_STRIDE		2

/* Scale registers: channel N at 0x46+N */
#define AW21018_REG_SL_BASE		0x46

/* Update register - write 0x00 to latch brightness values */
#define AW21018_REG_UPDATE		0x45

/* Global Current Control Register */
#define AW21018_REG_GCCR		0x58

/* UVCR - Under-Voltage Control Register */
#define AW21018_REG_UVCR		0x60

/* GCR2 - Global Control Register 2 */
#define AW21018_REG_GCR2		0x61

/* GCFG - Group Configuration Register */
#define AW21018_REG_GCFG		0x8B

/* Chip ID register */
#define AW21018_REG_CHIPID		0x70
#define AW21018_CHIPID			0x02

#define AW21018_REG_MAX			0x8B

#define AW21018_NUM_CHANNELS		18

/* GCR PWMRES field: fixed 12-bit resolution */
#define AW21018_PWMRES_12BIT		2
#define AW21018_MAX_BRIGHTNESS		(BIT(12) - 1)
#define AW21018_BRIGHTNESS_LSB_MASK	GENMASK(7, 0)
#define AW21018_BRIGHTNESS_MSB_MASK	GENMASK(11, 8)

enum aw21018_field {
	F_APSE = 0,
	F_CLKFRQ,
	F_PWMRES,
	F_CHIPEN,
	F_RGBMD,
	F_SBMD,
	F_GCCR,
	F_UVDIS,
	F_UVPD,
	F_GROUP_EN,
	F_GROUP_DIS,
	F_MAX_FIELDS
};

static const struct reg_field aw21018_reg_fields[F_MAX_FIELDS] = {
	[F_APSE]	= REG_FIELD(AW21018_REG_GCR, 7, 7),
	[F_CLKFRQ]	= REG_FIELD(AW21018_REG_GCR, 4, 6),
	[F_PWMRES]	= REG_FIELD(AW21018_REG_GCR, 1, 2),
	[F_CHIPEN]	= REG_FIELD(AW21018_REG_GCR, 0, 0),
	[F_RGBMD]	= REG_FIELD(AW21018_REG_GCR2, 0, 0),
	[F_SBMD]	= REG_FIELD(AW21018_REG_GCR2, 1, 1),
	[F_GCCR]	= REG_FIELD(AW21018_REG_GCCR, 0, 7),
	[F_UVDIS]	= REG_FIELD(AW21018_REG_UVCR, 0, 0),
	[F_UVPD]	= REG_FIELD(AW21018_REG_UVCR, 1, 1),
	[F_GROUP_EN]	= REG_FIELD(AW21018_REG_GCFG, 0, 5),
	[F_GROUP_DIS]	= REG_FIELD(AW21018_REG_GCFG, 6, 6),
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
	struct regmap_field *fields[F_MAX_FIELDS];
	/* Serializes register access */
	struct mutex mutex;
	struct gpio_desc *enable;
	u8 global_current;
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

	guard(mutex)(&chip->mutex);

	ret = regmap_write(chip->regmap, br_l,
			   FIELD_GET(AW21018_BRIGHTNESS_LSB_MASK, brightness));
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, br_h,
			   FIELD_GET(AW21018_BRIGHTNESS_MSB_MASK, brightness));
	if (ret)
		return ret;

	return regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);
}

static int aw21018_chip_init(struct aw21018 *chip)
{
	int ret;

	/* CHIPEN + CLKFRQ + PWMRES */
	ret = regmap_field_write(chip->fields[F_CHIPEN], 1);
	if (ret)
		return ret;

	/* 16 MHz clock */
	ret = regmap_field_write(chip->fields[F_CLKFRQ], 0);
	if (ret)
		return ret;

	ret = regmap_field_write(chip->fields[F_PWMRES], AW21018_PWMRES_12BIT);
	if (ret)
		return ret;

	/* GCR2: dual-byte mode, per-channel (not RGB) */
	ret = regmap_field_write(chip->fields[F_RGBMD], 0);
	if (ret)
		return ret;

	ret = regmap_field_write(chip->fields[F_SBMD], 0);
	if (ret)
		return ret;

	/* Global current */
	ret = regmap_field_write(chip->fields[F_GCCR], chip->global_current);
	if (ret)
		return ret;

	/* Disable under-voltage lockout */
	ret = regmap_field_write(chip->fields[F_UVDIS], 1);
	if (ret)
		return ret;

	ret = regmap_field_write(chip->fields[F_UVPD], 1);
	if (ret)
		return ret;

	/* APSE */
	ret = regmap_field_write(chip->fields[F_APSE], 1);
	if (ret)
		return ret;

	/* GCFG: group disable for per-channel control */
	ret = regmap_field_write(chip->fields[F_GROUP_EN], 0);
	if (ret)
		return ret;

	ret = regmap_field_write(chip->fields[F_GROUP_DIS], 1);
	if (ret)
		return ret;

	/* Latch register writes */
	return regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);
}

static void aw21018_chip_off(void *data)
{
	struct aw21018 *chip = data;
	unsigned int i;

	for (i = 0; i < chip->num_leds; i++) {
		unsigned int br = AW21018_REG_BR_BASE +
				  chip->leds[i].channel * AW21018_BR_STRIDE;

		regmap_write(chip->regmap, br, 0);
		regmap_write(chip->regmap, br + 1, 0);
	}

	regmap_write(chip->regmap, AW21018_REG_UPDATE, 0);
	regmap_field_write(chip->fields[F_CHIPEN], 0);
}

static int aw21018_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw21018 *chip;
	unsigned int chipid;
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
	i2c_set_clientdata(client, chip);

	if (device_property_read_u8(dev, "awinic,global-current",
				    &chip->global_current))
		return dev_err_probe(dev, -EINVAL,
				     "Missing awinic,global-current property\n");

	chip->regmap = devm_regmap_init_i2c(client, &aw21018_regmap);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "Failed to initialize regmap\n");

	ret = devm_regmap_field_bulk_alloc(dev, chip->regmap, chip->fields,
					   aw21018_reg_fields, F_MAX_FIELDS);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to allocate regmap fields\n");

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
		u8 scale;
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
		led->cdev.max_brightness = AW21018_MAX_BRIGHTNESS;

		scale = 0xff;
		fwnode_property_read_u8(child, "led-scale", &scale);
		ret = regmap_write(chip->regmap, AW21018_REG_SL_BASE + reg, scale);
		if (ret)
			return ret;

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
MODULE_LICENSE("GPL");
