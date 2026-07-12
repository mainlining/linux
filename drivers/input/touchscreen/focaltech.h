/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Danila Tikhonov <danila@jiaxyga.com>
 *
 * Based on goodix_berlin driver
 */

#ifndef __FOCALTECH_H_
#define __FOCALTECH_H_

#include <linux/pm.h>
#include <linux/types.h>

#define FOCALTECH_MAX_TOUCH_POINTS	10

struct focaltech_ic_settings {
	u32 app2_offset;
	u32 max_ecc_len;
	ssize_t boot_init_delay_ms;
	ssize_t ecc_delay_ms;
	u8 code_length_coefficient;
	bool spi_pe_supported;
};

struct focaltech_ic_data {
	struct focaltech_ic_settings settings;
	size_t touch_report_len;
	bool is_in_cell;
};

struct device;
struct input_id;
struct regmap;

int focaltech_probe(struct device *dev, int irq, const struct input_id *id,
		    struct regmap *regmap,
		    const struct focaltech_ic_data *ic_data);

extern const struct dev_pm_ops focaltech_pm_ops;

#endif
