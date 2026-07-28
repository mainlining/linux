// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based goodix_berlin_core driver
 *
 * Support is missing for:
 * - ESD Management
 * - Stylus Events
 * - Gesture Events
 * - DRM Notifier
 */

#define DEBUG

#include <linux/bitfield.h>
#include <linux/crc-ccitt.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "focaltech.h"

#define FOCALTECH_CMD_START_DELAY		12

#define FOCALTECH_CMD_START1			0x55
#define FOCALTECH_CMD_START2			0xaa
#define FOCALTECH_CMD_READ_ID			0x90

/* irq handler */
#define FOCALTECH_TOUCH_ADDR			0x01

#define FOCALTECH_EVENT_TYPE_MASK		GENMASK(7, 4)
#define FOCALTECH_TOUCH_NUM_MASK		GENMASK(3, 0)
#define FOCALTECH_TOUCH_ID_MASK			GENMASK(7, 4)
#define FOCALTECH_TOUCH_EVENT_MASK		GENMASK(7, 6)
#define FOCALTECH_TOUCH_COORD_H_MASK		GENMASK(3, 0)

#define FOCALTECH_TOUCH_E_NUM			1
#define FOCALTECH_ONE_TCH_LEN			6
#define FOCALTECH_MAX_TOUCH_BUF			4096

#define FOCALTECH_MAX_ID			0x0A
#define FOCALTECH_TOUCH_OFF_E_XH		0
#define FOCALTECH_TOUCH_OFF_XL			1
#define FOCALTECH_TOUCH_OFF_ID_YH		2
#define FOCALTECH_TOUCH_OFF_YL			3
#define FOCALTECH_TOUCH_OFF_PRE			4
#define FOCALTECH_TOUCH_OFF_AREA		5

#define FOCALTECH_TOUCH_DOWN			0
#define FOCALTECH_TOUCH_UP			1
#define FOCALTECH_TOUCH_CONTACT			2
#define FOCALTECH_EVENT_DOWN(f)			((FOCALTECH_TOUCH_DOWN == f) || \
						(FOCALTECH_TOUCH_CONTACT == f))
#define FOCALTECH_EVENT_UP(f)			(FOCALTECH_TOUCH_UP == f)

#define FOCALTECH_TOUCH_DEFAULT			0x00
#define FOCALTECH_TOUCH_EVENT_NUM		0x02
#define FOCALTECH_TOUCH_EXTRA_MSG		0x08
#define FOCALTECH_TOUCH_PEN			0x0b
#define FOCALTECH_TOUCH_GESTURE			0x80
#define FOCALTECH_TOUCH_FW_INIT			0x81
#define FOCALTECH_TOUCH_IGNORE			0xfe
#define FOCALTECH_TOUCH_ERROR			0xff

/* firmware update */
#define FOCALTECH_UPLOAD_LOOP			30

#define FOCALTECH_ROMBOOT_CMD_WRITE		0xae
#define FOCALTECH_ROMBOOT_CMD_START_APP		0x08

#define FOCALTECH_MAX_LEN_FILE			(256 * 1024)
#define FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR	0xad
#define FOCALTECH_PRAM_SADDR			0x000000
#define FOCALTECH_DRAM_SADDR			0xd00000
#define FOCALTECH_APP_INFO_OFFSET		0x100
#define FOCALTECH_MIN_FW_LEN			0x120
#define FOCALTECH_READ_BOOTID_TIMEOUT		3
#define FOCALTECH_CMD_WRITE_LEN			6
#define FOCALTECH_FLASH_PACKET_LENGTH_SPI	(32 * 1024 - 16)

#define FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN	6
#define FOCALTECH_ROMBOOT_CMD_ECC		0xcc
#define FOCALTECH_ROMBOOT_CMD_ECC_READ		0xcd

struct focaltech_core {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct touchscreen_properties props;
	struct input_dev *input_dev;
	int irq;

	u8 *touch_buf;

	const char *fw_path;
	const struct focaltech_ic_data *ic_data;
};

static const struct regulator_bulk_data focaltech_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "iovdd" },
};

static void focaltech_request_handle_reset(struct focaltech_core *cd, int sleepms)
{
	gpiod_set_value_cansleep(cd->reset_gpio, 1);
	fsleep(1000);
	gpiod_set_value_cansleep(cd->reset_gpio, 0);

	if (sleepms)
		fsleep(sleepms * 1000);
}

static int focaltech_get_ic_info(struct focaltech_core *cd)
{
	u8 id[2];
	int ret;

	ret = regmap_write(cd->regmap,
			   FOCALTECH_CMD_START1, FOCALTECH_CMD_START2);
	if (ret) {
		dev_err(cd->dev, "Start cmd write fail: %d\n", ret);
		return ret;
	}

	fsleep(FOCALTECH_CMD_START_DELAY * 1000);

	ret = regmap_raw_read(cd->regmap, FOCALTECH_CMD_READ_ID, id, 2);
	if (ret) {
		dev_err(cd->dev, "Read BootID fail: %d\n", ret);
		return ret;
	}

	if ((id[0] == 0x00 && id[1] == 0x00) ||
	    (id[0] == 0xff && id[1] == 0xff)) {
		dev_err(cd->dev, "Read BootID invalid: 0x%2phN\n", id);
		return -EIO;
	}

	dev_dbg(cd->dev, "Detected chip ID 0x%2phN\n", id);

	return 0;
}

static int focaltech_validate_firmware(struct focaltech_core *cd,
				       const struct firmware *fw)
{
	const struct focaltech_ic_settings *settings = &cd->ic_data->settings;
	u16 code_len, code_len_inv;
	size_t pram_app_size;

	if (fw->size & 1)
		return -EINVAL;

	if ((fw->size < FOCALTECH_MIN_FW_LEN) ||
	    (fw->size > settings->app2_offset)) {
		dev_err(cd->dev, "Firmware size is invalid: %zu\n", fw->size);
		return -EINVAL;
	}

	code_len = get_unaligned_be16(fw->data + FOCALTECH_APP_INFO_OFFSET);
	code_len_inv =
		   get_unaligned_be16(fw->data + FOCALTECH_APP_INFO_OFFSET + 2);
	if (code_len != (u16)(~code_len_inv)) {
		dev_err(cd->dev, "Invalid app code length: %#x/%#x\n",
			code_len, code_len_inv);
		return -EINVAL;
	}

	pram_app_size = (size_t)code_len * settings->code_length_coefficient;
	if (pram_app_size != fw->size) {
		dev_err(cd->dev,
			"Firmware size mismatch: expected %zu, got %zu\n",
			pram_app_size, fw->size);
		return -EINVAL;
	}

	return 0;
}

static int focaltech_enter_boot_mode(struct focaltech_core *cd)
{
	ssize_t init_delay_ms = cd->ic_data->settings.boot_init_delay_ms;
	int ret;

	focaltech_request_handle_reset(cd, init_delay_ms);

	ret = regmap_write(cd->regmap, FOCALTECH_CMD_START1, 0);
	if (ret)
		return ret;

	fsleep(init_delay_ms * 1000);
	return 0;
}

static int focaltech_calc_host_ecc(const u8 *data, size_t len, u16 *ecc)
{
	u16 value = 0;
	size_t i;

	for (i = 0; i < len; i += 2) {
		value = crc_ccitt_byte(value, data[i + 1]);
		value = crc_ccitt_byte(value, data[i]);
	}

	*ecc = value;
	return 0;
}

static int focaltech_calc_device_ecc(struct focaltech_core *cd, size_t start,
				     size_t len, u16 *ecc)
{
	u8 cmd[FOCALTECH_ROMBOOT_CMD_ECC_NEW_LEN];
	u8 ecc_buf[sizeof(*ecc)];
	int ret;

	put_unaligned_be24(start, cmd);
	put_unaligned_be24((u32)len, cmd + 3);

	ret = regmap_raw_write(cd->regmap, FOCALTECH_ROMBOOT_CMD_ECC,
			       cmd, sizeof(cmd));
	if (ret) {
		dev_err(cd->dev, "failed to start ECC calculation: %d\n", ret);
		return ret;
	}

	fsleep(2000);
	/* TODO: Use regmap_read_poll_timeout? */
	fsleep(cd->ic_data->settings.ecc_delay_ms * 1000);

	ret = regmap_raw_read(cd->regmap, FOCALTECH_ROMBOOT_CMD_ECC_READ,
			      ecc_buf, sizeof(ecc_buf));
	if (ret) {
		dev_err(cd->dev, "failed to read device ECC: %d\n", ret);
		return ret;
	}

	*ecc = get_unaligned_be16(ecc_buf);
	return 0;
}

static int focaltech_verify_ecc(struct focaltech_core *cd,
				const struct firmware *fw)
{
	size_t max_chunk = cd->ic_data->settings.max_ecc_len;
	size_t chunk_len;
	size_t offset = 0;
	u16 host_ecc;
	u16 device_ecc;
	int ret;

	if (!max_chunk)
		max_chunk = FOCALTECH_MAX_LEN_FILE;

	while (offset < fw->size) {
		chunk_len = min(max_chunk, fw->size - offset);

		ret = focaltech_calc_host_ecc(fw->data + offset,
					      chunk_len, &host_ecc);
		if (ret)
			return ret;

		ret = focaltech_calc_device_ecc(cd, offset,
						chunk_len, &device_ecc);
		if (ret)
			return ret;

		if (device_ecc != host_ecc) {
			dev_err(cd->dev,
				"ECC mismatch at 0x%06zx, length %zu: device=%04x host=%04x\n",
				offset, chunk_len, device_ecc, host_ecc);
			return -EBADMSG;
		}

		offset += chunk_len;
	}

	return 0;
}

static int focaltech_dpram_write(struct focaltech_core *cd,
				 const struct firmware *fw, bool pram)
{
	const size_t packet_size = FOCALTECH_FLASH_PACKET_LENGTH_SPI;
	u32 addr = pram ? FOCALTECH_PRAM_SADDR : FOCALTECH_DRAM_SADDR;
	u8 addr_buf[3];
	size_t packet_len, offset = 0;
	int ret;

	while (offset < fw->size) {
		packet_len = min(packet_size, fw->size - offset);

		put_unaligned_be24(addr + (u32)offset, addr_buf);

		ret = regmap_raw_write(
			cd->regmap, FOCALTECH_ROMBOOT_CMD_SET_PRAM_ADDR,
			addr_buf, sizeof(addr_buf));
		if (ret) {
			dev_err(cd->dev,
				"failed to set RAM address 0x%06x: %d\n",
				addr + (u32)offset, ret);
			return ret;
		}

		ret = regmap_raw_write(cd->regmap, FOCALTECH_ROMBOOT_CMD_WRITE,
				       fw->data + offset, packet_len);
		if (ret) {
			dev_err(cd->dev,
				"failed to write %zu bytes at 0x%06x: %d\n",
				packet_len, addr + (u32)offset, ret);
			return ret;
		}

		offset += packet_len;
	}

	return 0;
}

static int focaltech_upload_pram_app(struct focaltech_core *cd,
				     const struct firmware *fw)
{
	int ret;

	if (cd->ic_data->settings.spi_pe_supported) {
		dev_err(cd->dev, "SPI PE is unsupported yet :/\n");
		return -EOPNOTSUPP;
	}

	ret = focaltech_dpram_write(cd, fw, true);
	if (ret) {
		dev_err(cd->dev, "Failed to write PRAM, %d\n", ret);
		return ret;
	}

	ret = focaltech_verify_ecc(cd, fw);
	if (ret) {
		dev_err(cd->dev, "Failed to check PRAM ECC, %d\n", ret);
		return ret;
	}

	return 0;
}

static int focaltech_start_pram_app(struct focaltech_core *cd)
{
	int ret;

	ret = regmap_write(cd->regmap, FOCALTECH_ROMBOOT_CMD_START_APP, 0);
	if (ret)
		return ret;

	fsleep(10000);

	return 0;
}

static int focaltech_load_firmware(struct focaltech_core *cd)
{
	const struct firmware *fw __free(firmware) = NULL;
	int ret;

	ret = request_firmware(&fw, cd->fw_path, cd->dev);
	if (ret) {
		dev_err(cd->dev, "Failed to request firmware %s: %d\n",
			cd->fw_path, ret);
		return ret;
	}

	ret = focaltech_validate_firmware(cd, fw);
	if (ret)
		return ret;

	ret = focaltech_enter_boot_mode(cd);
	if (ret) {
		dev_err(cd->dev, "Failed to enter boot mode, %d\n", ret);
		return ret;
	}

	ret = focaltech_upload_pram_app(cd, fw);
	if (ret)
		return ret;

	ret = focaltech_start_pram_app(cd);
	if (ret) {
		dev_err(cd->dev, "Failed to start PRAM app, %d\n", ret);
		return ret;
	}

	return 0;
}

static void focaltech_report_touch_frame(struct focaltech_core *cd, u8 *buf)
{
	unsigned int flag, x, y, p, area;
	u8 touch_num, base, id;

	touch_num = FIELD_GET(FOCALTECH_TOUCH_NUM_MASK,
			      buf[FOCALTECH_TOUCH_E_NUM]);
	if (touch_num > FOCALTECH_MAX_TOUCH_POINTS) {
		dev_warn(cd->dev, "Invalid touch num: %d\n", touch_num);
		return;
	}

	for (int i = 0; i < FOCALTECH_MAX_TOUCH_POINTS; i++) {
		base = FOCALTECH_ONE_TCH_LEN * i + 2; // TODO: Use report_len
		id = FIELD_GET(FOCALTECH_TOUCH_ID_MASK,
			       buf[base + FOCALTECH_TOUCH_OFF_ID_YH]);
		flag = FIELD_GET(FOCALTECH_TOUCH_EVENT_MASK,
				 buf[base + FOCALTECH_TOUCH_OFF_E_XH]);

		if (id >= FOCALTECH_MAX_ID)
			break;

		input_mt_slot(cd->input_dev, id);

		if (FOCALTECH_EVENT_DOWN(flag)) {
			x = FIELD_GET(FOCALTECH_TOUCH_COORD_H_MASK,
				      buf[base + FOCALTECH_TOUCH_OFF_E_XH]) << 8;
			x |= buf[base + FOCALTECH_TOUCH_OFF_XL];

			y = FIELD_GET(FOCALTECH_TOUCH_COORD_H_MASK,
				      buf[base + FOCALTECH_TOUCH_OFF_ID_YH]) << 8;
			y |= buf[base + FOCALTECH_TOUCH_OFF_YL];

			p = buf[base + FOCALTECH_TOUCH_OFF_PRE];
			area = buf[base + FOCALTECH_TOUCH_OFF_AREA];

			p = p ? p : 0x3f;
			area = area ? area : 0x09;

			input_mt_report_slot_state(cd->input_dev,
						   MT_TOOL_FINGER, true);

			touchscreen_report_pos(cd->input_dev, &cd->props,
					       x, y, true);
			input_report_abs(cd->input_dev, ABS_MT_PRESSURE, p);
			input_report_abs(cd->input_dev, ABS_MT_TOUCH_MAJOR, area);
		} else {
			input_mt_report_slot_state(cd->input_dev,
						   MT_TOOL_FINGER, false);
		}
	}

	input_mt_sync_frame(cd->input_dev);
	input_sync(cd->input_dev);
}

static void focaltech_dispatch_event(struct focaltech_core *cd,
				     int event_type, u8 *buf)
{
	switch (event_type) {
	case FOCALTECH_TOUCH_DEFAULT:
		focaltech_report_touch_frame(cd, buf);
		break;
	case FOCALTECH_TOUCH_EVENT_NUM:
	case FOCALTECH_TOUCH_EXTRA_MSG:
	case FOCALTECH_TOUCH_PEN:
	case FOCALTECH_TOUCH_GESTURE:
		break;
	case FOCALTECH_TOUCH_FW_INIT:
		dev_warn_ratelimited(cd->dev, "Firmware init event\n");
		break;
	case FOCALTECH_TOUCH_IGNORE:
	case FOCALTECH_TOUCH_ERROR:
		dev_dbg_ratelimited(cd->dev,
				    "Controller reported touch error\n");
		break;
	default:
		dev_warn_ratelimited(cd->dev,
				     "Unknown touchscreen event: %d\n",
				     event_type);
		break;
	}
}

static irqreturn_t focaltech_irq(int irq, void *data)
{
	struct focaltech_core *cd = data;
	int event, ret;

	ret = regmap_raw_read(cd->regmap, FOCALTECH_TOUCH_ADDR,
			      cd->touch_buf, FOCALTECH_MAX_TOUCH_BUF);
	if (ret) {
		dev_err_ratelimited(cd->dev,
				    "Failed to read touch data, %d\n", ret);
		return IRQ_HANDLED;
	}

	if (!memchr_inv(&cd->touch_buf[1], 0xef, 3)) {
		dev_err_ratelimited(cd->dev,
				   "Firmware recovery indication detected\n");
		return IRQ_HANDLED;
	}

	if (!memchr_inv(&cd->touch_buf[1], 0xff, 4)) {
		dev_warn_ratelimited(cd->dev, "Device communication failed\n");
		return IRQ_HANDLED;
	}

	event = FIELD_GET(FOCALTECH_EVENT_TYPE_MASK,
			  cd->touch_buf[FOCALTECH_TOUCH_E_NUM]);
	focaltech_dispatch_event(cd, event, cd->touch_buf);

	return IRQ_HANDLED;
}

static int focaltech_input_dev_config(struct focaltech_core *cd,
				      const struct input_id *id)
{
	struct input_dev *input_dev;
	int ret;

	input_dev = devm_input_allocate_device(cd->dev);
	if (!input_dev)
		return -ENOMEM;

	cd->input_dev = input_dev;
	input_set_drvdata(input_dev, cd);

	input_dev->name = "FocalTech TouchScreen";
	input_dev->phys = "input/ts";

	input_dev->id = *id;

	input_set_capability(cd->input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(cd->input_dev, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(cd->input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(cd->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(cd->input_dev, true, &cd->props);

	ret = input_mt_init_slots(cd->input_dev, FOCALTECH_MAX_TOUCH_POINTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(cd->input_dev);
	if (ret)
		return ret;

	return 0;
}

static int focaltech_power_on(struct focaltech_core *cd)
{
	int ret = regulator_bulk_enable(ARRAY_SIZE(focaltech_supplies),
					cd->supplies);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(focaltech_supplies),
				       cd->supplies);
		return ret;
	}

	gpiod_set_value_cansleep(cd->reset_gpio, 1);

	return 0;
}

static void focaltech_power_off(struct focaltech_core *cd)
{
	gpiod_set_value_cansleep(cd->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(focaltech_supplies),
			       cd->supplies);
}

static int focaltech_suspend(struct device *dev)
{
	struct focaltech_core *cd = dev_get_drvdata(dev);

	disable_irq(cd->irq);
	focaltech_power_off(cd);

	return 0;
}

static int focaltech_resume(struct device *dev)
{
	struct focaltech_core *cd = dev_get_drvdata(dev);
	int ret;

	ret = focaltech_power_on(cd);
	if (ret)
		return ret;

	ret = focaltech_load_firmware(cd);
	if (ret) {
		focaltech_power_off(cd);
		return ret;
	}

	enable_irq(cd->irq);

	return 0;
}

EXPORT_GPL_SIMPLE_DEV_PM_OPS(focaltech_pm_ops,
			     focaltech_suspend, focaltech_resume);

static void focaltech_power_off_act(void *data)
{
	struct focaltech_core *cd = data;

	focaltech_power_off(cd);
}

int focaltech_probe(struct device *dev, int irq, const struct input_id *id,
		    struct regmap *regmap,
		    const struct focaltech_ic_data *ic_data)
{
	struct focaltech_core *cd;
	int ret;

	if (irq <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "Missing interrupt number\n");

	cd = devm_kzalloc(dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->touch_buf = devm_kzalloc(dev, FOCALTECH_MAX_TOUCH_BUF, GFP_KERNEL);
	if (!cd->touch_buf)
		return -ENOMEM;

	cd->dev = dev;
	cd->regmap = regmap;
	cd->irq = irq;
	cd->ic_data = ic_data;

	cd->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(cd->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(cd->reset_gpio),
				     "Failed to request reset gpio\n");

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(focaltech_supplies),
					    focaltech_supplies, &cd->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ret = focaltech_power_on(cd);
	if (ret)
		return dev_err_probe(dev, ret, "Failed power on\n");

	ret = device_property_read_string(dev, "firmware-name", &cd->fw_path);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to read firmware-name property\n");

	ret = devm_add_action_or_reset(dev, focaltech_power_off_act, cd);
	if (ret)
		return ret;

	ret = focaltech_input_dev_config(cd, id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed set input device\n");

	if (!ic_data->is_in_cell)
		focaltech_request_handle_reset(cd, 200);

	ret = focaltech_get_ic_info(cd);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get IC information\n");

	ret = focaltech_load_firmware(cd);
	if (ret)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "Failed to load firmware\n");

	ret = devm_request_threaded_irq(dev, cd->irq, NULL, focaltech_irq,
					IRQF_ONESHOT, "focaltech", cd);
	if (ret)
		return dev_err_probe(dev, ret, "Request threaded IRQ failed\n");

	dev_set_drvdata(dev, cd);

	return 0;
}
EXPORT_SYMBOL_GPL(focaltech_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FocalTech Core Touchscreen driver");
MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
