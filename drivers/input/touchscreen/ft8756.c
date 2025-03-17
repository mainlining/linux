// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Focaltech FT8756 touchscreen
 *
 * Copyright (c) 2012-2019, FocalTech Systems, Ltd.
 * Copyright (C) 2020 XiaoMi, Inc.
 * Copyright (C) 2024 Nikroks <www.github.com/N1kroks>
 *
 */

#include <linux/unaligned.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/input.h>
#include <drm/drm_panel.h>
#include <linux/pm_runtime.h>
#include <linux/devm-helpers.h>
#include <linux/regulator/consumer.h>
#include <linux/firmware.h>

/* Misc */
#define FT8756_NUM_SUPPLIES 3
#define FT8756_MAX_RETRIES	3

#define DATA_CRC_EN 0x20
#define WRITE_CMD   0x00
#define READ_CMD    (0x80 | DATA_CRC_EN)

#define CS_HIGH_DELAY 150

/* Touch info */
#define TOUCH_DEFAULT_MAX_WIDTH  1080
#define TOUCH_DEFAULT_MAX_HEIGHT 2400
#define TOUCH_MAX_FINGER_NUM	 10
#define TOUCH_MAX_PRESSURE	     255

#define FT8756_CMD_START1  0x55
#define FT8756_CMD_START2  0xAA
#define FT8756_CMD_READ_ID 0x90

#define FT8756_ROMBOOT_CMD_SET_PRAM_ADDR     0xAD
#define FT8756_ROMBOOT_CMD_SET_PRAM_ADDR_LEN 3
#define FT8756_ROMBOOT_CMD_WRITE             0xAE
#define FT8756_ROMBOOT_CMD_START_APP         0x08
#define FT8756_PRAM_SADDR                    0x000000
#define FT8756_DRAM_SADDR                    0xD00000

#define FT8756_ROMBOOT_CMD_ECC        0xCC
#define FT8756_ROMBOOT_CMD_ECC_LEN    6
#define FT8756_ECC_FINISH_TIMEOUT     100
#define FT8756_ROMBOOT_CMD_ECC_FINISH 0xCE
#define FT8756_ROMBOOT_CMD_ECC_READ   0xCD

#define FT8756_REG_POWER_MODE       0xA5
#define FT8756_REG_POWER_MODE_SLEEP 0x03

#define FT8756_APP_INFO_OFFSET 0x100

/* Point data length */
#define POINT_DATA_LEN 62

static struct drm_panel_follower_funcs ft8756_panel_follower_funcs;

struct ft8756_abs_object {
	u16 x;
	u16 y;
	u16 p;
	u8 area;
};

struct ft8756_ts {
	struct regmap *regmap;

	struct input_dev *input;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	int irq;
	struct device *dev;

	struct touchscreen_properties prop;
	struct ft8756_abs_object abs_obj;

	struct drm_panel_follower panel_follower;
};

/*
 * this function is nearly direct copy from vendor source
*/
static int ft8756_spi_write(void *dev, const void *data, size_t len)
{
	struct spi_device *spi = to_spi_device((struct device *)dev);
	int ret;
	u32 datalen = len - 1;
	u32 txlen = 0;
	u8 *txBuf = devm_kzalloc(dev, len + 9, GFP_KERNEL);
	if (!txBuf)
		return -ENOMEM;
	u8 *rxBuf = devm_kzalloc(dev, len + 9, GFP_KERNEL);
	if (!rxBuf)
		return -ENOMEM;

	txBuf[txlen++] = ((u8*)data)[0];
	txBuf[txlen++] = WRITE_CMD;
	txBuf[txlen++] = (datalen >> 8) & 0xFF;
	txBuf[txlen++] = datalen & 0xFF;

	if (datalen > 0) {
		txlen += 3; // dummy byte
		memcpy(&txBuf[txlen], data + 1, datalen);
		txlen += datalen;
	}

	struct spi_message	msg;
	struct spi_transfer xfer = {
		.tx_buf = txBuf,
		.rx_buf = rxBuf,
		.len = txlen,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for(int i = 0; i < FT8756_MAX_RETRIES; i++) {
		ret = spi_sync(spi, &msg);
		if(ret) {
			dev_err(dev, "transfer error: %d", ret);
			usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
			continue;
		}
		if ((rxBuf[3] &0xA0) == 0) {
			break;
		} else {
			dev_err(dev, "Failed to write data status: 0x%X", rxBuf[3]);
			ret = -EIO;
			usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
		}
	}

	usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
	return ret;
}

/*
 * this function is nearly direct copy from vendor source
*/
static int ft8756_spi_read(void *dev, const void *reg_buf, size_t reg_size, void *val_buf, size_t val_size)
{
	struct spi_device *spi = to_spi_device((struct device *)dev);
	int ret, i, j;
	u32 txlen = 0;
	u8 *txBuf = devm_kzalloc(dev, 4, GFP_KERNEL);
	if (!txBuf)
		return -ENOMEM;
	u8 *rxBuf = devm_kzalloc(dev, val_size + 9, GFP_KERNEL);
	if (!rxBuf)
		return -ENOMEM;
	u32 dp = 0;
	u16 crc = 0xFFFF;

	txBuf[txlen++] = ((u8*)reg_buf)[0];
	txBuf[txlen++] = READ_CMD;
	txBuf[txlen++] = (val_size >> 8) & 0xFF;
	txBuf[txlen++] = val_size & 0xFF;
	dp = txlen + 3;
	txlen = dp + val_size;
	if(txBuf[1] & DATA_CRC_EN)
		txlen += 2;

	struct spi_message	msg;
	struct spi_transfer xfer = {
		.tx_buf = txBuf,
		.rx_buf = rxBuf,
		.len = txlen,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for(i = 0; i < FT8756_MAX_RETRIES; i++) {
		ret = spi_sync(spi, &msg);
		if(ret) {
			dev_err(dev, "transfer error: %d", ret);
			usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
			continue;
		}
		if ((rxBuf[3] & 0xA0) == 0) {
			memcpy(val_buf, &rxBuf[dp], val_size);
			if(txBuf[1] & DATA_CRC_EN) {
				for(j = 0; j < (txlen - dp - 2); j++) {
					crc ^= rxBuf[dp + j];
					for(u8 k = 0; k < 8; k++)
						crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
				}
				u16 crc_read = (u16) (rxBuf[txlen - 1] << 8) + rxBuf[txlen - 2];
				if (crc != crc_read) {
					dev_err(dev, "crc error: 0x%02x expected, got 0x%02x", crc, crc_read);
					usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
					continue;
				}
			}
			break;
		} else {
			dev_err(dev, "Failed to read data status: 0x%X", rxBuf[3]);
			ret = -EIO;
			usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
		}
	}

	usleep_range(CS_HIGH_DELAY, CS_HIGH_DELAY + 100);
	return ret;
}

const struct regmap_config ft8756_regmap_config_32bit = {
	.reg_bits = 8,
	.val_bits = 8,
	.read = ft8756_spi_read,
	.write = ft8756_spi_write,

	.zero_flag_mask = true, /* this is needed to make sure addr is not write_masked */
	.cache_type = REGCACHE_NONE,
};

static void ft8756_disable_regulators(void *data)
{
	struct ft8756_ts *ts = data;

	regulator_bulk_disable(FT8756_NUM_SUPPLIES, ts->supplies);
}

static void ft8756_reset(struct ft8756_ts *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(1);
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	msleep(200);
}

static int ft8756_check_chip_id(struct ft8756_ts *ts)
{
	u16 chip_id = 0;
	u8 buf[2];
	int ret;

	ft8756_reset(ts);

	ret = regmap_raw_read(ts->regmap, 0xA3, &buf[0], 1);
	ret = regmap_raw_read(ts->regmap, 0x9F, &buf[1], 1);
	chip_id = (buf[0] << 8) | buf[1];
	if (chip_id != 0x5652 || ret) {
		dev_err(ts->dev, "Chip ID mismatch: expected 0x%x, got 0x%x", 0x5652, chip_id);
		goto fallback;
	}

	return 0;

fallback:
	ret = regmap_write(ts->regmap, FT8756_CMD_START1, FT8756_CMD_START2);
	if (ret)
		goto exit;

	msleep(15);

	ret = regmap_raw_read(ts->regmap, FT8756_CMD_READ_ID, &chip_id, 2);
	chip_id = cpu_to_be16(chip_id);
	if (chip_id != 0x8756 || ret) {
		dev_err(ts->dev, "Chip ID mismatch: expected 0x%x, got 0x%x", 0x8756, chip_id);
		ret = -ENODEV;
		goto exit;
	}

exit:
	return ret;
}

static int ft8756_input_dev_config(struct ft8756_ts *ts)
{
	struct device *dev = ts->dev;
	int ret;

	/* Allocate memory for the input device structure */
	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	/* Set the device-specific data to the allocated input device structure */
	input_set_drvdata(ts->input, ts);

	/* Set physical path for the input device */
	ts->input->phys = devm_kasprintf(dev, GFP_KERNEL,
					 "%s/input0", dev_name(dev));
	if (!ts->input->phys)
		return -ENOMEM;

	/* Set input device properties */
	ts->input->name = "Focaltech FT8756 Touchscreen";
	ts->input->dev.parent = dev;

	/* Set absolute parameters for touch events */
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	/* Set absolute parameters for touch position */
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
						 TOUCH_DEFAULT_MAX_WIDTH - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
						TOUCH_DEFAULT_MAX_HEIGHT - 1, 0, 0);

	/* Parse touchscreen properties */
	touchscreen_parse_properties(ts->input, true, &ts->prop);

	/* Check if the maximum x-coordinate is valid */
	WARN_ON(ts->prop.max_x < 1);

	/* Initialize multitouch slots for the input device */
	ret = input_mt_init_slots(ts->input, TOUCH_MAX_FINGER_NUM,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		dev_err(dev, "Cannot init MT slots (%d)\n", ret);
		return ret;
	}

	/* Register the input device */
	ret = input_register_device(ts->input);
	if (ret) {
		dev_err(dev, "Failed to register input device: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static void ft8756_report(struct ft8756_ts *ts)
{
	struct ft8756_abs_object* obj = &ts->abs_obj;
	struct input_dev *input = ts->input;
	u8 input_id = 0;
	u8 point[POINT_DATA_LEN] = { 0 };
	unsigned int base = 0;
	int ret, i;

	ret = regmap_raw_read(ts->regmap, 0x01, point, POINT_DATA_LEN);
	if (ret < 0) {
		dev_err(ts->dev,
			"Cannot read touch point data: %d\n", ret);
		goto exit;
	}

	for(i = 0; i < 6; i++) {
		if((point[i] != 0xEF && point[i] != 0xFF))
			break;
		goto exit;
	}

	for(i = 0; i < TOUCH_MAX_FINGER_NUM; i++) {
		base = 6 * i;
		input_id = point[base + 4] >> 4;
		if (input_id >= 10)
			continue;

		if ((point[base + 2] >> 6) == 0x0 || (point[base + 2] >> 6) == 0x2) {
			obj->x = ((point[base + 2] & 0xF) << 8) + (point[base + 3] & 0xFF);
			obj->y = ((point[base + 4] & 0xF) << 8) + (point[base + 5] & 0xFF);

			if ((obj->x > ts->prop.max_x) ||
				(obj->y > ts->prop.max_y))
				continue;

			obj->p = point[base + 6];
			obj->area = point[base + 7] >> 4;

			if (obj->area == 0)
				obj->area = 1;

			if (obj->p > TOUCH_MAX_PRESSURE)
				obj->p = TOUCH_MAX_PRESSURE;

			if (obj->p == 0)
				obj->p = 1;

			input_mt_slot(input, input_id);
			input_mt_report_slot_state(input,
							MT_TOOL_FINGER, true);
			touchscreen_report_pos(input, &ts->prop,
						obj->x,
						obj->y, true);

			input_report_abs(input, ABS_MT_TOUCH_MAJOR, obj->area);
			input_report_abs(input, ABS_MT_PRESSURE, obj->p);
		}
	}

	input_mt_sync_frame(input);

	input_sync(input);

exit:
	return;
}

static irqreturn_t ft8756_irq_handler(int irq, void *dev_id)
{
	struct ft8756_ts *ts = dev_id;

	disable_irq_nosync(ts->irq);

	ft8756_report(ts);

	enable_irq(ts->irq);

	return IRQ_HANDLED;
}

static int ft8756_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct regmap_config *regmap_config;
	size_t max_size;
	int ret = 0;

	dev_info(dev, "enter %s", __func__);

	/* Allocate memory for the touchscreen data structure */
	struct ft8756_ts *ts = devm_kzalloc(dev, sizeof(struct ft8756_ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	/* Set SPI mode and bits per word, and perform SPI setup */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	/* Allocate and copy the default regmap configuration */
	regmap_config = devm_kmemdup(dev, &ft8756_regmap_config_32bit,
					 sizeof(*regmap_config), GFP_KERNEL);
	if (!regmap_config) {
		dev_err(dev, "memdup regmap_config fail\n");
		return -ENOMEM;
	}

	/* Calculate the maximum raw read and write sizes based on SPI transfer size */
	max_size = spi_max_transfer_size(spi);
	regmap_config->max_raw_read = max_size - 9;
	regmap_config->max_raw_write = max_size - 9;

	/* Initialize the regmap using the provided configuration */
	ts->regmap = devm_regmap_init(dev, NULL, spi, regmap_config);
	if (IS_ERR(ts->regmap))
		return PTR_ERR(ts->regmap);

	/* Set the device-specific data to the allocated structure */
	dev_set_drvdata(dev, ts);

	ts->dev = dev;
	ts->irq = spi->irq;

	/* Allocate memory for GPIO supplies */
	ts->supplies = devm_kcalloc(dev, FT8756_NUM_SUPPLIES,
					sizeof(*ts->supplies), GFP_KERNEL);
	if (!ts->supplies)
		return -ENOMEM;

	/* Get and configure the optional reset GPIO */
	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return PTR_ERR(ts->reset_gpio);

	gpiod_set_consumer_name(ts->reset_gpio, "ft8756 reset");

	/* Get and configure the optional IRQ GPIO */
	ts->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(ts->irq_gpio))
		return PTR_ERR(ts->irq_gpio);

	/* If IRQ is not specified, try to obtain it from the IRQ GPIO */
	if (ts->irq <= 0) {
		ts->irq = gpiod_to_irq(ts->irq_gpio);
		if (ts->irq <= 0) {
			dev_err(dev, "either need irq or irq-gpio specified in devicetree node!\n");
			return -EINVAL;
		}

		dev_info(ts->dev, "Interrupts GPIO: %#x\n", ts->irq); // todo change to dbg
	}

	gpiod_set_consumer_name(ts->irq_gpio, "ft8756 irq");

	/* If the device follows a DRM panel, skip regulator initialization */
	if (drm_is_panel_follower(dev))
		goto skip_regulators;

	ts->supplies[0].supply = "vio";
	ts->supplies[1].supply = "lab";
	ts->supplies[2].supply = "ibb";
	ret = devm_regulator_bulk_get(dev,
					  FT8756_NUM_SUPPLIES,
					  ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret,
					 "Cannot get supplies: %d\n", ret);

	ret = regulator_bulk_enable(FT8756_NUM_SUPPLIES, ts->supplies);
	if (ret)
		return ret;


	/* Delay for regulators to stabilize */
	usleep_range(10000, 11000);

	ret = devm_add_action_or_reset(dev, ft8756_disable_regulators, ts);
	if (ret)
		return ret;

skip_regulators:
	ret = ft8756_check_chip_id(ts);
	if (ret)
		return ret;

	ret = ft8756_input_dev_config(ts);
	if (ret) {
		dev_err(dev, "failed set input device: %d\n", ret);
		return ret;
	}

	/* Request threaded IRQ for touch screen interrupts */
	ret = devm_request_threaded_irq(dev, ts->irq, NULL, ft8756_irq_handler,
			 IRQ_TYPE_EDGE_RISING | IRQF_ONESHOT, dev_name(dev), ts);
	if (ret) {
		dev_err(dev, "request irq failed: %d\n", ret);
		return ret;
	}

	/* If the device follows a DRM panel, configure panel follower */
	if (drm_is_panel_follower(dev)) {
		ts->panel_follower.funcs = &ft8756_panel_follower_funcs;
		devm_drm_panel_add_follower(dev, &ts->panel_follower);
	}

	dev_info(dev, "FT8756 touchscreen initialized\n");
	return 0;
}

static int __maybe_unused ft8756_internal_pm_suspend(struct device *dev)
{
	struct ft8756_ts *ts = dev_get_drvdata(dev);
	int ret = 0;

	regmap_write(ts->regmap, FT8756_REG_POWER_MODE_SLEEP, FT8756_REG_POWER_MODE_SLEEP);
	if (ret)
		dev_err(ts->dev, "Cannot enter sleep!\n");
	return 0;
}

static int __maybe_unused ft8756_pm_suspend(struct device *dev)
{
	struct ft8756_ts *ts = dev_get_drvdata(dev);
	int ret=0;

	if (drm_is_panel_follower(dev))
		return 0;

	disable_irq_nosync(ts->irq);

	ret = ft8756_internal_pm_suspend(dev);
	return ret;
}

static int __maybe_unused ft8756_pm_resume(struct device *dev)
{
	struct ft8756_ts *ts = dev_get_drvdata(dev);
	int ret=0;

	if (drm_is_panel_follower(dev))
		return 0;

	enable_irq(ts->irq);

	return ret;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(ft8756_pm_ops,
				 ft8756_pm_suspend,
				 ft8756_pm_resume);

static int panel_prepared(struct drm_panel_follower *follower)
{
	struct ft8756_ts *ts = container_of(follower, struct ft8756_ts, panel_follower);

	enable_irq(ts->irq);

	return 0;
}

static int panel_unpreparing(struct drm_panel_follower *follower)
{
	struct ft8756_ts *ts = container_of(follower, struct ft8756_ts, panel_follower);

	disable_irq(ts->irq);

	return ft8756_internal_pm_suspend(ts->dev);
}

static struct drm_panel_follower_funcs ft8756_panel_follower_funcs = {
	.panel_prepared = panel_prepared,
	.panel_unpreparing = panel_unpreparing,
};

static const struct spi_device_id ft8756_spi_ids[] = {
	{ "ft8756-spi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, ft8756_spi_ids);

static struct spi_driver ft8756_spi_driver = {
	.driver = {
		.name   = "ft8756-spi",
		.pm = pm_sleep_ptr(&ft8756_pm_ops),
	},
	.probe = ft8756_spi_probe,
	.id_table = ft8756_spi_ids,
};
module_spi_driver(ft8756_spi_driver);

MODULE_DESCRIPTION("FT8756 touchscreen driver");
MODULE_AUTHOR("Nikroks <nikroksm@mail.ru>");
MODULE_LICENSE("GPL");