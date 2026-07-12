// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on goodix_berlin_spi driver
 */

#include <linux/crc-ccitt.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include "focaltech.h"

#define FOCALTECH_SPI_DATA_CRC_ENABLE	BIT(5)
#define FOCALTECH_SPI_READ_FLAG		BIT(7)
#define FOCALTECH_SPI_READ_CMD		(FOCALTECH_SPI_READ_FLAG | \
					 FOCALTECH_SPI_DATA_CRC_ENABLE)
#define FOCALTECH_SPI_WRITE_CMD		0x00
#define FOCALTECH_SPI_ERROR_MASK	0xa0

#define FOCALTECH_SPI_STATUS_OFFSET	3

#define FOCALTECH_SPI_COMMAND_LEN	1
#define FOCALTECH_SPI_MAX_PAYLOAD_LEN	U16_MAX
#define FOCALTECH_SPI_HEADER_LEN	4
#define FOCALTECH_SPI_DUMMY_LEN		3
#define FOCALTECH_SPI_CRC_LEN		2
#define FOCALTECH_SPI_PREFIX_LEN	(FOCALTECH_SPI_HEADER_LEN + \
					 FOCALTECH_SPI_DUMMY_LEN)
#define FOCALTECH_SPI_READ_OVERHEAD_LEN	(FOCALTECH_SPI_PREFIX_LEN + \
					 FOCALTECH_SPI_CRC_LEN)

#define FOCALTECH_TOUCH_RECORD_LEN	6
#define FOCALTECH_TOUCH_RECORD_LEN_V2	8
#define FOCALTECH_TOUCH_REPORT_LEN	(FOCALTECH_MAX_TOUCH_POINTS * \
					FOCALTECH_TOUCH_RECORD_LEN + 2)
#define FOCALTECH_TOUCH_REPORT_LEN_V2	(FOCALTECH_MAX_TOUCH_POINTS * \
					FOCALTECH_TOUCH_RECORD_LEN_V2 + 4)

static int focaltech_spi_check_crc(const u8 *data, size_t data_len)
{
	u16 calculated_crc;
	u16 received_crc;

	calculated_crc = crc_ccitt(0xffff, data, data_len);
	received_crc = get_unaligned_le16(data + data_len);

	if (calculated_crc != received_crc)
		return -EBADMSG;

	return 0;
}

/*
 * FocalTech SPI transactions start with a four-byte header:
 *
 *  byte 0:	device command
 *  byte 1:	r/w command and protocol flags
 *  bytes 2-3:	payload length, big-endian
 *
 * A read transaction is laid out as follows:
 *
 *  header | 3 dummy bytes | payload | 16-bit little-endian CRC
 *
 * A write transaction with a payload is laid out as follows:
 *
 *  header | 3 dummy bytes | payload
 *
 * Header-only write commands omit the dummy bytes.
 */
static int focaltech_spi_read(void *context, const void *cmd_buf,
			      size_t cmd_size, void *val_buf, size_t val_size)
{
	struct spi_device *spi = context;
	struct spi_transfer xfer = { };
	const u8 *cmd = cmd_buf;
	int ret;

	if (cmd_size != FOCALTECH_SPI_COMMAND_LEN)
		return -EINVAL;

	if (val_size > FOCALTECH_SPI_MAX_PAYLOAD_LEN)
		return -EMSGSIZE;

	u8 *buf __free(kfree) =
		kzalloc(FOCALTECH_SPI_READ_OVERHEAD_LEN + val_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = *cmd;
	buf[1] = FOCALTECH_SPI_READ_CMD;
	put_unaligned_be16((u16)val_size, buf + 2);

	xfer.tx_buf = buf;
	xfer.rx_buf = buf;
	xfer.len = FOCALTECH_SPI_READ_OVERHEAD_LEN + val_size;

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret) {
		dev_err_ratelimited(&spi->dev,
				    "SPI transfer failed, %d\n", ret);
		return ret;
	}

	if (buf[FOCALTECH_SPI_STATUS_OFFSET] & FOCALTECH_SPI_ERROR_MASK)
		return -EIO;

	ret = focaltech_spi_check_crc(buf + FOCALTECH_SPI_PREFIX_LEN,
				      val_size);
	if (ret) {
		dev_err_ratelimited(&spi->dev,
				    "SPI read CRC mismatch, %d\n", ret);
		return ret;
	}

	memcpy(val_buf, buf + FOCALTECH_SPI_PREFIX_LEN, val_size);
	return 0;
}

static int focaltech_spi_write(void *context, const void *data, size_t count)
{
	struct spi_device *spi = context;
	struct spi_transfer xfer = { };
	size_t payload_len = count - 1;
	const u8 *cmd = data;
	int ret;

	if (count < FOCALTECH_SPI_COMMAND_LEN)
		return -EINVAL;

	if (payload_len > FOCALTECH_SPI_MAX_PAYLOAD_LEN)
		return -EMSGSIZE;

	u8 *buf __free(kfree) =
		kzalloc(FOCALTECH_SPI_PREFIX_LEN + payload_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = *cmd;
	buf[1] = FOCALTECH_SPI_WRITE_CMD;
	put_unaligned_be16((u16)payload_len, buf + 2);
	if (payload_len)
		memcpy(buf + FOCALTECH_SPI_PREFIX_LEN,
		       cmd + FOCALTECH_SPI_COMMAND_LEN, payload_len);

	xfer.tx_buf = buf;
	xfer.rx_buf = buf;
	xfer.len = payload_len ?
		   FOCALTECH_SPI_PREFIX_LEN + payload_len :
		   FOCALTECH_SPI_HEADER_LEN;

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret) {
		dev_err_ratelimited(&spi->dev,
				    "SPI transfer failed, %d\n", ret);
		return ret;
	}

	if (buf[FOCALTECH_SPI_STATUS_OFFSET] & FOCALTECH_SPI_ERROR_MASK)
		return -EIO;

	return 0;
}

/* Regmap's register address represents the one-byte command selector. */
static const struct regmap_config focaltech_spi_regmap_conf = {
	.reg_bits = 8,
	.val_bits = 8,
	.read = focaltech_spi_read,
	.write = focaltech_spi_write,
};

static const struct input_id focaltech_spi_input_id = {
	.bustype = BUS_SPI,
};

static int focaltech_spi_probe(struct spi_device *spi)
{
	const struct focaltech_ic_data *ic_data = spi_get_device_match_data(spi);
	struct regmap_config regmap_config = focaltech_spi_regmap_conf;
	struct regmap *regmap;
	size_t max_size;
	int ret = 0;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	max_size = spi_max_transfer_size(spi);

	regmap_config.max_raw_read = max_size - FOCALTECH_SPI_READ_OVERHEAD_LEN;
	regmap_config.max_raw_write = max_size - FOCALTECH_SPI_PREFIX_LEN;

	regmap = devm_regmap_init(&spi->dev, NULL, spi, &regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return focaltech_probe(&spi->dev, spi->irq,
			       &focaltech_spi_input_id, regmap, ic_data);
}

static const struct focaltech_ic_data ft3680_data = {
	.settings = {
		.app2_offset			= SZ_128K,
		.max_ecc_len			= SZ_128K,
		.boot_init_delay_ms		= 8,
		.ecc_delay_ms			= 5,
		.code_length_coefficient	= 4,
		.spi_pe_supported		= false,
	},
	.touch_report_len	= FOCALTECH_TOUCH_REPORT_LEN,
	.is_in_cell		= false,
};

static const struct focaltech_ic_data ft3683g_data = {
	.settings = { /* FIXME */
		.app2_offset			= SZ_128K,
		.max_ecc_len			= SZ_128K,
		.boot_init_delay_ms		= 8,
		.ecc_delay_ms			= 5,
		.code_length_coefficient	= 4,
		.spi_pe_supported		= false,
	},
	.touch_report_len	= FOCALTECH_TOUCH_REPORT_LEN_V2,
	.is_in_cell		= false,
};

static const struct spi_device_id focaltech_spi_ids[] = {
	{ .name = "ft3680", .driver_data = (long)&ft3680_data },
	{ .name = "ft3683g", .driver_data = (long)&ft3683g_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(spi, focaltech_spi_ids);

static const struct of_device_id focaltech_spi_of_match[] = {
	{ .compatible = "focaltech,ft3680", .data = &ft3680_data },
	{ .compatible = "focaltech,ft3683g", .data = &ft3683g_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, focaltech_spi_of_match);

static struct spi_driver focaltech_spi_driver = {
	.driver = {
		.name = "focaltech-spi",
		.of_match_table = focaltech_spi_of_match,
		.pm = pm_sleep_ptr(&focaltech_pm_ops),
	},
	.probe = focaltech_spi_probe,
	.id_table = focaltech_spi_ids,
};
module_spi_driver(focaltech_spi_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FocalTech SPI Touchscreen driver");
MODULE_AUTHOR("Danila Tikhonov <danila@jiaxyga.com>");
