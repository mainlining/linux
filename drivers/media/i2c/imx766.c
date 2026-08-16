// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony imx766 Camera Sensor Driver
 *
 * Copyright (C) 2026 Danila Tikhonov <danila@mainlining.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>

/* Chip ID */
#define IMX766_REG_CHIP_ID			CCI_REG16(0x0016)
#define IMX766_CHIP_ID				0x0766

/* CCS standard registers */
#define IMX766_REG_MODE_SELECT			CCI_REG8(0x0100)
#define IMX766_MODE_SELECT_STANDBY		0x00
#define IMX766_MODE_SELECT_STREAMING		0x01

#define IMX766_REG_IMG_ORIENTATION_V		CCI_REG8(0x0101)
#define IMX766_IMG_ORIENTATION_V_NORMAL		0x00
#define IMX766_IMG_ORIENTATION_V_REVERSE	0x01

#define IMX766_REG_GRP_PARAM_HOLD		CCI_REG8(0x0104)
#define IMX766_GRP_PARAM_HOLD_RELEASE		0x00
#define IMX766_GRP_PARAM_HOLD			0x01

#define IMX766_REG_FAST_STANDBY_CTL		CCI_REG8(0x0106)
#define IMX766_FAST_STANDBY_CTL_AFTER_FRAMES	0x00
#define IMX766_FAST_STANDBY_CTL_IMMEDIATELY	0x01

/* Signaling mode */
#define IMX766_REG_CSI_SIG_MODE			CCI_REG8(0x0111)
#define IMX766_CSI_SIG_MODE_DPHY		0x02
#define IMX766_CSI_SIG_MODE_CPHY		0x03

/* MIPI output */
#define IMX766_REG_CSI_DT_FMT			CCI_REG16(0x0112)
#define IMX766_CSI_DT_FMT_RAW8			0x0808
#define IMX766_CSI_DT_FMT_RAW10			0x0a0a
#define IMX766_REG_CSI_LANE_MODE		CCI_REG8(0x0114)
#define IMX766_CSI_DPHY_2_LANE_MODE		0x01
#define IMX766_CSI_DPHY_4_LANE_MODE		0x03
#define IMX766_CSI_CPHY_1_TRIO_LANE_MODE	0x00
#define IMX766_CSI_CPHY_2_TRIO_LANE_MODE	0x01
#define IMX766_CSI_CPHY_3_TRIO_LANE_MODE	0x02

/* INCK */
#define IMX766_REG_INCK_FREQ			CCI_REG16(0x0136) /* [15:0] */
#define IMX766_INCK_FREQ_18MHZ			(18 * HZ_PER_MHZ)
#define IMX766_INCK_FREQ_19P2MHZ		(19200 * HZ_PER_KHZ)
#define IMX766_INCK_FREQ_24MHZ			(24 * HZ_PER_MHZ)
#define IMX766_INCK_FREQ_26MHZ			(26 * HZ_PER_MHZ)
#define IMX766_INCK_FREQ_27MHZ			(27 * HZ_PER_MHZ)

/* Integration */
#define IMX766_REG_COARSE_INTEG_TIME		CCI_REG16(0x0202) /* [15:0] */

/* Gain */
#define IMX766_REG_ANA_GAIN_GLOBAL		CCI_REG16(0x0204) /* [13:0] */
#define IMX766_REG_DIG_GAIN_GLOBAL		CCI_REG16(0x020e) /* [15:0] */
#define IMX766_REG_ST_ANA_GAIN_GLOBAL		CCI_REG16(0x0216) /* [13:0] */
#define IMX766_REG_ST_DIG_GAIN_GLOBAL		CCI_REG16(0x0218) /* [15:0] */

/* Integration */
#define IMX766_REG_ST_COARSE_INTEG_TIME		CCI_REG16(0x0224) /* [15:0] */

/* Clock */
#define IMX766_REG_IVT_PXCK_DIV			CCI_REG8(0x0301)  /* [4:0]  */
#define IMX766_REG_IVT_SYCK_DIV			CCI_REG8(0x0303)  /* [2:0]  */
#define IMX766_REG_IVT_PREPLLCK_DIV		CCI_REG8(0x0305)  /* [3:0]  */
#define IMX766_REG_IVT_PLL_MPY			CCI_REG16(0x0306) /* [10:0] */
#define IMX766_REG_IOP_SYCK_DIV			CCI_REG8(0x030b)  /* [4:0]  */
#define IMX766_REG_IOP_PREPLLCK_DIV		CCI_REG8(0x030d)  /* [4:0]  */
#define IMX766_REG_IOP_PLL_MPY			CCI_REG16(0x030e) /* [12:0] */

/* Frame Length Lines */
#define IMX766_REG_FRM_LENGTH_LINES		CCI_REG16(0x0340) /* [15:0] */

/* Line Length PCK */
#define IMX766_REG_LINE_LENGTH_PCK		CCI_REG16(0x0342) /* [15:0] */

/* ROI */
#define IMX766_REG_X_ADD_STA			CCI_REG16(0x0344) /* [13:0] */
#define IMX766_REG_Y_ADD_STA			CCI_REG16(0x0346) /* [12:0] */
#define IMX766_REG_X_ADD_END			CCI_REG16(0x0348) /* [13:0] */
#define IMX766_REG_Y_ADD_END			CCI_REG16(0x034a) /* [12:0] */

/* Output Size */
#define IMX766_REG_X_OUT_SIZE			CCI_REG16(0x034c) /* [13:0] */
#define IMX766_REG_Y_OUT_SIZE			CCI_REG16(0x034e) /* [12:0] */

#define IMX766_REG_C_VERSION			CCI_REG8(0x33f0)
#define IMX766_REG_T_VERSION			CCI_REG8(0x33f1)

/* Digital Crop & Scaling */
#define IMX766_REG_DIG_CROP_X_OFFSET		CCI_REG16(0x0408) /* [13:0] */
#define IMX766_REG_DIG_CROP_Y_OFFSET		CCI_REG16(0x040a) /* [12:0] */
#define IMX766_REG_DIG_CROP_IMAGE_WIDTH		CCI_REG16(0x040c) /* [13:0] */
#define IMX766_REG_DIG_CROP_IMAGE_HEIGHT	CCI_REG16(0x040e) /* [12:0] */

/* Mode */
#define IMX766_REG_BINNING_MODE			CCI_REG8(0x0900)  /* [0:0]  */
#define IMX766_REG_BINNING_TYPE			CCI_REG8(0x0901)  /* [7:4]&[3:0] */
#define IMX766_REG_BINNING_WEIGHTING		CCI_REG8(0x0902)  /* [7:0]  */

#define IMX766_REG_QBC_2X2OCL_MODE		CCI_REG8(0x3005)  /* [2:0]  */
#define IMX766_REG_PHASE_PIX_OUT_EN		CCI_REG8(0x30b4)  /* [2:0]  */
#define IMX766_REG_ADC_MODE			CCI_REG8(0x3120)  /* [2:0]  */
#define IMX766_REG_RST_SHORT_EN			CCI_REG8(0x3121)  /* [7:0]  */
#define IMX766_REG_BINNING_PRIORITY_H		CCI_REG8(0x3200)
#define IMX766_REG_BINNING_PRIORITY_V		CCI_REG8(0x3201)
#define IMX766_REG_QBC_RMSC_EN			CCI_REG8(0x32d6)

/* C-PHY Global Timing */
#define IMX766_REG_PHY_CTRL			CCI_REG8(0x0808)
#define IMX766_PHY_CTRL_AUTO			0x00
#define IMX766_PHY_CTRL_UI			0x01
#define IMX766_PHY_CTRL_REGISTERS		0x02

#define IMX766_REG_T3PREPARE			CCI_REG16(0x084e)
#define IMX766_REG_T3LPX			CCI_REG16(0x0850)
#define IMX766_REG_T3HSEXIT			CCI_REG16(0x0852)
#define IMX766_REG_T3PREBEGIN			CCI_REG16(0x0854)

#define IMX766_REG_T3PROGSEQ_EN			CCI_REG8(0x0856)
#define IMX766_T3PROGSEQ_DISABLE		0x00

#define IMX766_REG_T3POST			CCI_REG16(0x0858)


#define IMX766_ANA_GAIN_MIN			0
#define IMX766_ANA_GAIN_STEP			1
#define IMX766_ANA_GAIN_DEFAULT			0

#define IMX766_ANA_GAIN_MAX_24DB		15360
#define IMX766_ANA_GAIN_MAX_36DB		16128

#define IMX766_DIG_GAIN_MIN			0x0100
#define IMX766_DIG_GAIN_MAX			0x0fff
#define IMX766_DIG_GAIN_STEP			1
#define IMX766_DIG_GAIN_DEFAULT			0x0100

#define IMX766_FRM_LENGTH_MAX			65534
#define IMX766_FRM_LENGTH_STEP			2

#define IMX766_EXPOSURE_MARGIN			48

#define IMX766_NATIVE_WIDTH			8264
#define IMX766_NATIVE_HEIGHT			6256

#define IMX766_ACTIVE_LEFT			36
#define IMX766_ACTIVE_TOP			48
#define IMX766_ACTIVE_WIDTH			8192
#define IMX766_ACTIVE_HEIGHT			6144

#define IMX766_POWER_ON_TO_XCLR_US		1000
#define IMX766_XCLR_TO_STREAMING_US		17000

#define IMX766_INCK_FREQ(freq_hz) \
	((u16)(((u64)(freq_hz) * 256) / HZ_PER_MHZ))

#define to_imx766(_sd) container_of_const(_sd, struct imx766, sd)

static const struct regulator_bulk_data imx766_supplies[] = {
	{ .supply = "vana1" },
	{ .supply = "vana2" },
	{ .supply = "vdig" },
	{ .supply = "vif" },
};

/* IMX766 native and active pixel array size. */
static const struct v4l2_rect imx766_native_area = {
	.left = 0,
	.top = 0,
	.width = IMX766_NATIVE_WIDTH,
	.height = IMX766_NATIVE_HEIGHT,
};

static const struct v4l2_rect imx766_active_area = {
	.left = IMX766_ACTIVE_LEFT,
	.top = IMX766_ACTIVE_TOP,
	.width = IMX766_ACTIVE_WIDTH,
	.height = IMX766_ACTIVE_HEIGHT,
};

static const u32 imx766_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

enum imx766_inck {
	IMX766_INCK_18MHZ,
	IMX766_INCK_19P2MHZ,
	IMX766_INCK_24MHZ,
	IMX766_INCK_26MHZ,
	IMX766_INCK_27MHZ,
	IMX766_NUM_INCK,
};

struct imx766_inck_config {
	unsigned long rate;
	enum imx766_inck index;
};

static const struct imx766_inck_config imx766_inck_configs[] = {
	{ IMX766_INCK_FREQ_18MHZ,   IMX766_INCK_18MHZ,},
	{ IMX766_INCK_FREQ_19P2MHZ, IMX766_INCK_19P2MHZ,},
	{ IMX766_INCK_FREQ_24MHZ,   IMX766_INCK_24MHZ,},
	{ IMX766_INCK_FREQ_26MHZ,   IMX766_INCK_26MHZ,},
	{ IMX766_INCK_FREQ_27MHZ,   IMX766_INCK_27MHZ,},
};

struct imx766_pll_config {
	u8 ivt_prepllck_div;
	u16 ivt_pll_mpy;

	u8 iop_prepllck_div;
	u16 iop_pll_mpy;

	u64 pixel_rate;
};

static const struct imx766_pll_config imx766_pll_j[] = {
	[IMX766_INCK_18MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 312,
		.iop_prepllck_div = 9,
		.iop_pll_mpy = 1076,
		.pixel_rate = 1497600000ULL,
	},
	[IMX766_INCK_19P2MHZ] = {
		.ivt_prepllck_div = 2,
		.ivt_pll_mpy = 195,
		.iop_prepllck_div = 12,
		.iop_pll_mpy = 1345,
		.pixel_rate = 1497600000ULL,
	},
	[IMX766_INCK_24MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 312,
		.iop_prepllck_div = 3,
		.iop_pll_mpy = 269,
		.pixel_rate = 1497600000ULL,
	},
	[IMX766_INCK_26MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 288,
		.iop_prepllck_div = 13,
		.iop_pll_mpy = 1076,
		.pixel_rate = 1497600000ULL,
	},
	[IMX766_INCK_27MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 208,
		.iop_prepllck_div = 27,
		.iop_pll_mpy = 2152,
		.pixel_rate = 1497600000ULL,
	},
};

static const struct imx766_pll_config imx766_pll_c[] = {
	[IMX766_INCK_18MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 300,	/* 1800 MHz */
		.iop_prepllck_div = 9,
		.iop_pll_mpy = 1076,
		.pixel_rate = 1440000000ULL,
	},
	[IMX766_INCK_19P2MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 282,	/* 1804.8 MHz */
		.iop_prepllck_div = 12,
		.iop_pll_mpy = 1345,
		.pixel_rate = 1443840000ULL,
	},
	[IMX766_INCK_24MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 300,	/* 1800 MHz */
		.iop_prepllck_div = 3,
		.iop_pll_mpy = 269,
		.pixel_rate = 1440000000ULL,
	},
	[IMX766_INCK_26MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 277,	/* 1800.5 MHz */
		.iop_prepllck_div = 13,
		.iop_pll_mpy = 1076,
		.pixel_rate = 1440400000ULL,
	},
	[IMX766_INCK_27MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 200,	/* 1800 MHz */
		.iop_prepllck_div = 27,
		.iop_pll_mpy = 2152,
		.pixel_rate = 1440000000ULL,
	},
};

static const struct imx766_pll_config imx766_pll_l1[] = {
	[IMX766_INCK_18MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 331,	/* 1986 MHz */
		.iop_prepllck_div = 2,
		.iop_pll_mpy = 464,	/* 4176 MHz */
		.pixel_rate = 1588800000ULL,
	},
	[IMX766_INCK_19P2MHZ] = {
		.ivt_prepllck_div = 2,
		.ivt_pll_mpy = 207,	/* 1987.2 MHz */
		.iop_prepllck_div = 2,
		.iop_pll_mpy = 435,	/* 4176 MHz */
		.pixel_rate = 1589760000ULL,
	},
	[IMX766_INCK_24MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 331,	/* 1986 MHz */
		.iop_prepllck_div = 3,
		.iop_pll_mpy = 522,	/* 4176 MHz */
		.pixel_rate = 1588800000ULL,
	},
	[IMX766_INCK_26MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 306,	/* 1989 MHz */
		.iop_prepllck_div = 13,
		.iop_pll_mpy = 2088,	/* 4176 MHz */
		.pixel_rate = 1591200000ULL,
	},
	[IMX766_INCK_27MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 221,	/* 1989 MHz */
		.iop_prepllck_div = 3,
		.iop_pll_mpy = 464,	/* 4176 MHz */
		.pixel_rate = 1591200000ULL,
	},
};

static const struct imx766_pll_config imx766_pll_m[] = {
	[IMX766_INCK_18MHZ] = {
		.ivt_prepllck_div = 3,
		.ivt_pll_mpy = 216,
		.iop_prepllck_div = 9,
		.iop_pll_mpy = 1076,
		.pixel_rate = 2073600000ULL,
	},
	[IMX766_INCK_19P2MHZ] = {
		.ivt_prepllck_div = 2,
		.ivt_pll_mpy = 135,
		.iop_prepllck_div = 12,
		.iop_pll_mpy = 1345,
		.pixel_rate = 2073600000ULL,
	},
	[IMX766_INCK_24MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 216,
		.iop_prepllck_div = 3,
		.iop_pll_mpy = 269,
		.pixel_rate = 2073600000ULL,
	},
	[IMX766_INCK_26MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 200,	/* 1300 MHz */
		.iop_prepllck_div = 13,
		.iop_pll_mpy = 1076,
		.pixel_rate = 2080000000ULL,
	},
	[IMX766_INCK_27MHZ] = {
		.ivt_prepllck_div = 4,
		.ivt_pll_mpy = 192,
		.iop_prepllck_div = 27,
		.iop_pll_mpy = 2152,
		.pixel_rate = 2073600000ULL,
	},
};

struct imx766 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	struct clk *inck;
	unsigned long inck_freq;
	enum imx766_inck inck_index;

	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwrn_gpio;

	struct v4l2_ctrl_handler ctrl_handler;

	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *digital_gain;

	struct v4l2_mbus_config_mipi_csi2 bus;
	unsigned long link_freq_bitmap;
};

enum {
	IMX766_LINK_FREQ_269MHZ,
	IMX766_LINK_FREQ_1044MHZ,
	IMX766_LINK_FREQ_1076MHZ,
};

static const s64 imx766_link_freq_menu[] = {
	[IMX766_LINK_FREQ_269MHZ]  = 269000000,
	[IMX766_LINK_FREQ_1044MHZ] = 1044000000,
	[IMX766_LINK_FREQ_1076MHZ] = 1076000000,
};

struct imx766_cphy_timing {
	u16 t3prepare;
	u16 t3lpx;
	u16 t3hsexit;
	u16 t3prebegin;
	u16 t3post;
};

static const struct imx766_cphy_timing imx766_cphy_timings[] = {
	[IMX766_LINK_FREQ_269MHZ] = {
		.t3prepare = 5,
		.t3lpx = 5,
		.t3hsexit = 9,
		.t3prebegin = 41,
		.t3post = 25,
	},
	[IMX766_LINK_FREQ_1044MHZ] = {
		.t3prepare = 19,
		.t3lpx = 17,
		.t3hsexit = 31,
		.t3prebegin = 41,
		.t3post = 25,
	},
	[IMX766_LINK_FREQ_1076MHZ] = {
		.t3prepare = 19,
		.t3lpx = 17,
		.t3hsexit = 31,
		.t3prebegin = 41,
		.t3post = 25,
	},
};

struct imx766_mode {
	u32 width;
	u32 height;

	struct v4l2_rect crop;

	u32 line_length_pck;
	u32 frame_length_lines;
	u32 min_frame_length_lines;

	u32 exposure_min;
	u32 exposure_step;
	u32 analogue_gain_max;

	const struct imx766_pll_config *pll_configs;
	u8 link_freq_index;

	const struct cci_reg_sequence *reg_list;
	unsigned int num_regs;
};

static const struct cci_reg_sequence imx766_4096x3072_regs[] = {
	/* ROI Setting */
	{ IMX766_REG_X_ADD_STA, 0x0000 },
	{ IMX766_REG_Y_ADD_STA, 0x0000 },
	{ IMX766_REG_X_ADD_END, 0x1fff },
	{ IMX766_REG_Y_ADD_END, 0x17ff },
	/* Mode Setting */
	{ IMX766_REG_BINNING_MODE, 0x01 },
	{ IMX766_REG_BINNING_TYPE, 0x22 },
	{ IMX766_REG_BINNING_WEIGHTING, 0x08 },
	{ IMX766_REG_QBC_2X2OCL_MODE, 0x00 },
	{ IMX766_REG_ADC_MODE, 0x00 },
	{ IMX766_REG_RST_SHORT_EN, 0x01 },
	{ IMX766_REG_BINNING_PRIORITY_H, 0x41 },
	{ IMX766_REG_BINNING_PRIORITY_V, 0x41 },
	{ IMX766_REG_QBC_RMSC_EN, 0x00 },
	/* Digital Crop & Scaling */
	{ IMX766_REG_DIG_CROP_X_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_Y_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_IMAGE_WIDTH, 0x1000 },
	{ IMX766_REG_DIG_CROP_IMAGE_HEIGHT, 0x0c00 },
	/* Output Size */
	{ IMX766_REG_X_OUT_SIZE, 0x1000 },
	{ IMX766_REG_Y_OUT_SIZE, 0x0c00 },
	/* Clock */
	{ IMX766_REG_IVT_PXCK_DIV, 0x05 },
	{ IMX766_REG_IVT_SYCK_DIV, 0x04 },
	{ IMX766_REG_IOP_SYCK_DIV, 0x04 },
	/* Other Setting */
	{ CCI_REG8(0x30cb), 0x00 },
	{ CCI_REG8(0x30cc), 0x10 },
	{ CCI_REG8(0x30cd), 0x00 },
	{ CCI_REG8(0x30ce), 0x03 },
	{ CCI_REG8(0x30cf), 0x00 },
	{ CCI_REG8(0x319c), 0x00 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x04 },
	{ CCI_REG8(0x3847), 0x03 },
	{ CCI_REG8(0x38b0), 0x00 },
	{ CCI_REG8(0x38b1), 0xc8 },
	{ CCI_REG8(0x38b2), 0x00 },
	{ CCI_REG8(0x38b3), 0xc8 },
	{ CCI_REG8(0x38c4), 0x00 },
	{ CCI_REG8(0x38c5), 0xaa },
	{ CCI_REG8(0x4c3a), 0x02 },
	{ CCI_REG8(0x4c3b), 0xd2 },
	{ CCI_REG8(0x4c68), 0x09 },
	{ CCI_REG8(0x4c69), 0x00 },
	{ CCI_REG8(0x4cf8), 0x07 },
	{ CCI_REG8(0x4cf9), 0x9e },
	{ CCI_REG8(0x4db8), 0x08 },
	{ CCI_REG8(0x4db9), 0x98 },
	{ CCI_REG8(0x3803), 0x00 },
	{ CCI_REG8(0x3804), 0x17 },
	{ CCI_REG8(0x3805), 0xc0 },
};

static const struct cci_reg_sequence imx766_8192x6144_regs[] = {
	/* ROI Setting */
	{ IMX766_REG_X_ADD_STA, 0x0000 },
	{ IMX766_REG_Y_ADD_STA, 0x0000 },
	{ IMX766_REG_X_ADD_END, 0x1fff },
	{ IMX766_REG_Y_ADD_END, 0x17ff },
	/* Mode Setting */
	{ IMX766_REG_BINNING_MODE, 0x00 },
	{ IMX766_REG_BINNING_TYPE, 0x11 },
	{ IMX766_REG_BINNING_WEIGHTING, 0x0a },
	{ IMX766_REG_QBC_2X2OCL_MODE, 0x00 },
	{ IMX766_REG_ADC_MODE, 0x00 },
	{ IMX766_REG_RST_SHORT_EN, 0x01 },
	/* Digital Crop & Scaling */
	{ IMX766_REG_BINNING_PRIORITY_H, 0x00 },
	{ IMX766_REG_BINNING_PRIORITY_V, 0x00 },
	{ IMX766_REG_QBC_RMSC_EN, 0x01 },
	{ IMX766_REG_DIG_CROP_X_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_Y_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_IMAGE_WIDTH, 0x2000 },
	{ IMX766_REG_DIG_CROP_IMAGE_HEIGHT, 0x1800 },
	{ IMX766_REG_X_OUT_SIZE, 0x2000 },
	{ IMX766_REG_Y_OUT_SIZE, 0x1800 },
	{ IMX766_REG_IVT_PXCK_DIV, 0x05 },
	{ IMX766_REG_IVT_SYCK_DIV, 0x04 },
	{ IMX766_REG_IOP_SYCK_DIV, 0x01 },
	/* Other Setting */
	{ CCI_REG8(0x30cb), 0x00 },
	{ CCI_REG8(0x30cc), 0x10 },
	{ CCI_REG8(0x30cd), 0x00 },
	{ CCI_REG8(0x30ce), 0x03 },
	{ CCI_REG8(0x30cf), 0x00 },
	{ CCI_REG8(0x319c), 0x00 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x04 },
	{ CCI_REG8(0x3847), 0x00 },
	{ CCI_REG8(0x38b0), 0x00 },
	{ CCI_REG8(0x38b1), 0x00 },
	{ CCI_REG8(0x38b2), 0x00 },
	{ CCI_REG8(0x38b3), 0x00 },
	{ CCI_REG8(0x38c4), 0x02 },
	{ CCI_REG8(0x38c5), 0x26 },
	{ CCI_REG8(0x4c3a), 0x02 },
	{ CCI_REG8(0x4c3b), 0xd2 },
	{ CCI_REG8(0x4c68), 0x04 },
	{ CCI_REG8(0x4c69), 0x7e },
	{ CCI_REG8(0x4cf8), 0x07 },
	{ CCI_REG8(0x4cf9), 0x9e },
	{ CCI_REG8(0x4db8), 0x08 },
	{ CCI_REG8(0x4db9), 0x98 },
	{ CCI_REG8(0x3803), 0x01 },
	{ CCI_REG8(0x3804), 0x16 },
	{ CCI_REG8(0x3805), 0xb0 },
};

static const struct cci_reg_sequence imx766_7424x5568_regs[] = {
	/* ROI Setting */
	{ IMX766_REG_X_ADD_STA, 0x0000 },
	{ IMX766_REG_Y_ADD_STA, 0x0120 },
	{ IMX766_REG_X_ADD_END, 0x1fff },
	{ IMX766_REG_Y_ADD_END, 0x16df },
	/* Mode Setting */
	{ IMX766_REG_BINNING_MODE, 0x00 },
	{ IMX766_REG_BINNING_TYPE, 0x11 },
	{ IMX766_REG_BINNING_WEIGHTING, 0x0a },
	{ IMX766_REG_QBC_2X2OCL_MODE, 0x00 },
	{ IMX766_REG_ADC_MODE, 0x00 },
	{ IMX766_REG_RST_SHORT_EN, 0x01 },
	{ IMX766_REG_BINNING_PRIORITY_H, 0x00 },
	{ IMX766_REG_BINNING_PRIORITY_V, 0x00 },
	{ IMX766_REG_QBC_RMSC_EN, 0x01 },
	/* Digital Crop & Scaling */
	{ IMX766_REG_DIG_CROP_X_OFFSET, 0x0180 },
	{ IMX766_REG_DIG_CROP_Y_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_IMAGE_WIDTH, 0x1d00 },
	{ IMX766_REG_DIG_CROP_IMAGE_HEIGHT, 0x15c0 },
	{ IMX766_REG_X_OUT_SIZE, 0x1d00 },
	{ IMX766_REG_Y_OUT_SIZE, 0x15c0 },
	{ IMX766_REG_IVT_PXCK_DIV, 0x05 },
	{ IMX766_REG_IVT_SYCK_DIV, 0x04 },
	{ IMX766_REG_IOP_SYCK_DIV, 0x02 },
	/* Other Setting */
	{ CCI_REG8(0x30cb), 0x00 },
	{ CCI_REG8(0x30cc), 0x10 },
	{ CCI_REG8(0x30cd), 0x00 },
	{ CCI_REG8(0x30ce), 0x03 },
	{ CCI_REG8(0x30cf), 0x00 },
	{ CCI_REG8(0x319c), 0x00 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x04 },
	{ CCI_REG8(0x3847), 0x00 },
	{ CCI_REG8(0x38b0), 0x00 },
	{ CCI_REG8(0x38b1), 0x00 },
	{ CCI_REG8(0x38b2), 0x00 },
	{ CCI_REG8(0x38b3), 0x00 },
	{ CCI_REG8(0x38c4), 0x02 },
	{ CCI_REG8(0x38c5), 0x26 },
	{ CCI_REG8(0x4c3a), 0x02 },
	{ CCI_REG8(0x4c3b), 0xd2 },
	{ CCI_REG8(0x4c68), 0x04 },
	{ CCI_REG8(0x4c69), 0x7e },
	{ CCI_REG8(0x4cf8), 0x07 },
	{ CCI_REG8(0x4cf9), 0x9e },
	{ CCI_REG8(0x4db8), 0x08 },
	{ CCI_REG8(0x4db9), 0x98 },
	/* Integration Setting */
	{ CCI_REG8(0x3803), 0x01 },
	{ CCI_REG8(0x3804), 0x16 },
	{ CCI_REG8(0x3805), 0xb0 },
};

static const struct cci_reg_sequence imx766_3712x2784_regs[] = {
	/* ROI Setting */
	{ IMX766_REG_X_ADD_STA, 0x0000 },
	{ IMX766_REG_Y_ADD_STA, 0x0120 },
	{ IMX766_REG_X_ADD_END, 0x1fff },
	{ IMX766_REG_Y_ADD_END, 0x16df },
	/* Mode Setting */
	{ IMX766_REG_BINNING_MODE, 0x01 },
	{ IMX766_REG_BINNING_TYPE, 0x22 },
	{ IMX766_REG_BINNING_WEIGHTING, 0x08 },
	{ IMX766_REG_QBC_2X2OCL_MODE, 0x03 },
	{ IMX766_REG_ADC_MODE, 0x04 },
	{ IMX766_REG_RST_SHORT_EN, 0x01 },
	{ IMX766_REG_BINNING_PRIORITY_H, 0x41 },
	{ IMX766_REG_BINNING_PRIORITY_V, 0x41 },
	{ IMX766_REG_QBC_RMSC_EN, 0x00 },
	/* Digital Crop & Scaling */
	{ IMX766_REG_DIG_CROP_X_OFFSET, 0x00c0 },
	{ IMX766_REG_DIG_CROP_Y_OFFSET, 0x0000 },
	{ IMX766_REG_DIG_CROP_IMAGE_WIDTH, 0x0e80 },
	{ IMX766_REG_DIG_CROP_IMAGE_HEIGHT, 0x0ae0 },
	{ IMX766_REG_X_OUT_SIZE, 0x0e80 },
	{ IMX766_REG_Y_OUT_SIZE, 0x0ae0 },
	{ IMX766_REG_IVT_PXCK_DIV, 0x05 },
	{ IMX766_REG_IVT_SYCK_DIV, 0x02 },
	{ IMX766_REG_IOP_SYCK_DIV, 0x04 },
	/* Other Setting */
	{ CCI_REG8(0x30cb), 0x00 },
	{ CCI_REG8(0x30cc), 0x10 },
	{ CCI_REG8(0x30cd), 0x00 },
	{ CCI_REG8(0x30ce), 0x03 },
	{ CCI_REG8(0x30cf), 0x00 },
	{ CCI_REG8(0x319c), 0x01 },
	{ CCI_REG8(0x3800), 0x01 },
	{ CCI_REG8(0x3801), 0x01 },
	{ CCI_REG8(0x3802), 0x02 },
	{ CCI_REG8(0x3847), 0x03 },
	{ CCI_REG8(0x38b0), 0x00 },
	{ CCI_REG8(0x38b1), 0x64 },
	{ CCI_REG8(0x38b2), 0x00 },
	{ CCI_REG8(0x38b3), 0x64 },
	{ CCI_REG8(0x38c4), 0x00 },
	{ CCI_REG8(0x38c5), 0x64 },
	{ CCI_REG8(0x4c3a), 0x02 },
	{ CCI_REG8(0x4c3b), 0xd2 },
	{ CCI_REG8(0x4c68), 0x04 },
	{ CCI_REG8(0x4c69), 0x7e },
	{ CCI_REG8(0x4cf8), 0x0f },
	{ CCI_REG8(0x4cf9), 0x40 },
	{ CCI_REG8(0x4db8), 0x08 },
	{ CCI_REG8(0x4db9), 0x98 },
	/* Integration Setting */
	{ CCI_REG8(0x3803), 0x00 },
	{ CCI_REG8(0x3804), 0x17 },
	{ CCI_REG8(0x3805), 0xc0 },
};

static const struct imx766_mode imx766_modes[] = {
	{
		/* Sony mode J: 8192x6144, 15 fps */
		.width = 8192,
		.height = 6144,
		.crop = {
			.left = IMX766_ACTIVE_LEFT,
			.top = IMX766_ACTIVE_TOP,
			.width = 8192,
			.height = 6144,
		},
		.line_length_pck = 11552,
		.frame_length_lines = 8642,
		.min_frame_length_lines = 6300,
		.exposure_min = 13,
		.exposure_step = 1,
		.analogue_gain_max = IMX766_ANA_GAIN_MAX_24DB,
		.pll_configs = imx766_pll_j,
		.link_freq_index = IMX766_LINK_FREQ_1076MHZ,
		.reg_list = imx766_8192x6144_regs,
		.num_regs = ARRAY_SIZE(imx766_8192x6144_regs),
	},
	{
		/* Sony mode L-1: 7424x5568, 24 fps QRMSC */
		.width = 7424,
		.height = 5568,
		.crop = {
			.left = IMX766_ACTIVE_LEFT + 384,
			.top = IMX766_ACTIVE_TOP + 288,
			.width = 7424,
			.height = 5568,
		},
		.line_length_pck = 11552,
		.frame_length_lines = 5730,
		.min_frame_length_lines = 5724,
		.exposure_min = 13,
		.exposure_step = 1,
		.analogue_gain_max = IMX766_ANA_GAIN_MAX_24DB,
		.pll_configs = imx766_pll_l1,
		.link_freq_index = IMX766_LINK_FREQ_1044MHZ,
		.reg_list = imx766_7424x5568_regs,
		.num_regs = ARRAY_SIZE(imx766_7424x5568_regs),
	},
	{
		/* Sony mode C: 4096x3072, 2x2 adjacent-pixel binning */
		.width = 4096,
		.height = 3072,
		.crop = {
			.left = IMX766_ACTIVE_LEFT,
			.top = IMX766_ACTIVE_TOP,
			.width = 8192,
			.height = 6144,
		},
		.line_length_pck = 18432,
		.frame_length_lines = 3164,
		.min_frame_length_lines = 3164,
		.exposure_min = 8,
		.exposure_step = 2,
		.analogue_gain_max = IMX766_ANA_GAIN_MAX_36DB,
		.pll_configs = imx766_pll_c,
		.link_freq_index = IMX766_LINK_FREQ_269MHZ,
		.reg_list = imx766_4096x3072_regs,
		.num_regs = ARRAY_SIZE(imx766_4096x3072_regs),
	},
	{
		/* Sony mode M: 3712x2784, binned/cropped HVBIN */
		.width = 3712,
		.height = 2784,
		.crop = {
			.left = IMX766_ACTIVE_LEFT + 384,
			.top = IMX766_ACTIVE_TOP + 288,
			.width = 7424,
			.height = 5568,
		},
		.line_length_pck = 31232,
		.frame_length_lines = 2876,
		.min_frame_length_lines = 2876,
		.exposure_min = 8,
		.exposure_step = 2,
		.analogue_gain_max = IMX766_ANA_GAIN_MAX_36DB,
		.pll_configs = imx766_pll_m,
		.link_freq_index = IMX766_LINK_FREQ_269MHZ,
		.reg_list = imx766_3712x2784_regs,
		.num_regs = ARRAY_SIZE(imx766_3712x2784_regs),
	},
};

static const struct cci_reg_sequence imx766_global_regs[] = {
	{ CCI_REG8(0x33d3), 0x01 },
	{ CCI_REG8(0x3892), 0x01 },
	{ CCI_REG8(0x4c14), 0x00 },
	{ CCI_REG8(0x4c15), 0x07 },
	{ CCI_REG8(0x4c16), 0x00 },
	{ CCI_REG8(0x4c17), 0x1b },
	{ CCI_REG8(0x4c1a), 0x00 },
	{ CCI_REG8(0x4c1b), 0x03 },
	{ CCI_REG8(0x4c1c), 0x00 },
	{ CCI_REG8(0x4c1d), 0x00 },
	{ CCI_REG8(0x4c1e), 0x00 },
	{ CCI_REG8(0x4c1f), 0x02 },
	{ CCI_REG8(0x4c20), 0x00 },
	{ CCI_REG8(0x4c21), 0x5f },
	{ CCI_REG8(0x4c26), 0x00 },
	{ CCI_REG8(0x4c27), 0x43 },
	{ CCI_REG8(0x4c28), 0x00 },
	{ CCI_REG8(0x4c29), 0x09 },
	{ CCI_REG8(0x4c2a), 0x00 },
	{ CCI_REG8(0x4c2b), 0x4a },
	{ CCI_REG8(0x4c2c), 0x00 },
	{ CCI_REG8(0x4c2d), 0x00 },
	{ CCI_REG8(0x4c2e), 0x00 },
	{ CCI_REG8(0x4c2f), 0x02 },
	{ CCI_REG8(0x4c30), 0x00 },
	{ CCI_REG8(0x4c31), 0xc6 },
	{ CCI_REG8(0x4c3e), 0x00 },
	{ CCI_REG8(0x4c3f), 0x55 },
	{ CCI_REG8(0x4c52), 0x00 },
	{ CCI_REG8(0x4c53), 0x97 },
	{ CCI_REG8(0x4cb4), 0x00 },
	{ CCI_REG8(0x4cb5), 0x55 },
	{ CCI_REG8(0x4cc8), 0x00 },
	{ CCI_REG8(0x4cc9), 0x97 },
	{ CCI_REG8(0x4d04), 0x00 },
	{ CCI_REG8(0x4d05), 0x4f },
	{ CCI_REG8(0x4d74), 0x00 },
	{ CCI_REG8(0x4d75), 0x55 },
	{ CCI_REG8(0x4f06), 0x00 },
	{ CCI_REG8(0x4f07), 0x5f },
	{ CCI_REG8(0x4f48), 0x00 },
	{ CCI_REG8(0x4f49), 0xc6 },
	{ CCI_REG8(0x544a), 0xff },
	{ CCI_REG8(0x544b), 0xff },
	{ CCI_REG8(0x544e), 0x01 },
	{ CCI_REG8(0x544f), 0xbd },
	{ CCI_REG8(0x5452), 0xff },
	{ CCI_REG8(0x5453), 0xff },
	{ CCI_REG8(0x5456), 0x00 },
	{ CCI_REG8(0x5457), 0xa5 },
	{ CCI_REG8(0x545a), 0xff },
	{ CCI_REG8(0x545b), 0xff },
	{ CCI_REG8(0x545e), 0x00 },
	{ CCI_REG8(0x545f), 0xa5 },
	{ CCI_REG8(0x5496), 0x00 },
	{ CCI_REG8(0x5497), 0xa2 },
	{ CCI_REG8(0x54f6), 0x01 },
	{ CCI_REG8(0x54f7), 0x55 },
	{ CCI_REG8(0x54f8), 0x01 },
	{ CCI_REG8(0x54f9), 0x61 },
	{ CCI_REG8(0x5670), 0x00 },
	{ CCI_REG8(0x5671), 0x85 },
	{ CCI_REG8(0x5672), 0x01 },
	{ CCI_REG8(0x5673), 0x77 },
	{ CCI_REG8(0x5674), 0x01 },
	{ CCI_REG8(0x5675), 0x2f },
	{ CCI_REG8(0x5676), 0x02 },
	{ CCI_REG8(0x5677), 0x55 },
	{ CCI_REG8(0x5678), 0x00 },
	{ CCI_REG8(0x5679), 0x85 },
	{ CCI_REG8(0x567a), 0x01 },
	{ CCI_REG8(0x567b), 0x77 },
	{ CCI_REG8(0x567c), 0x01 },
	{ CCI_REG8(0x567d), 0x2f },
	{ CCI_REG8(0x567e), 0x02 },
	{ CCI_REG8(0x567f), 0x55 },
	{ CCI_REG8(0x5680), 0x00 },
	{ CCI_REG8(0x5681), 0x85 },
	{ CCI_REG8(0x5682), 0x01 },
	{ CCI_REG8(0x5683), 0x77 },
	{ CCI_REG8(0x5684), 0x01 },
	{ CCI_REG8(0x5685), 0x2f },
	{ CCI_REG8(0x5686), 0x02 },
	{ CCI_REG8(0x5687), 0x55 },
	{ CCI_REG8(0x5688), 0x00 },
	{ CCI_REG8(0x5689), 0x85 },
	{ CCI_REG8(0x568a), 0x01 },
	{ CCI_REG8(0x568b), 0x77 },
	{ CCI_REG8(0x568c), 0x01 },
	{ CCI_REG8(0x568d), 0x2f },
	{ CCI_REG8(0x568e), 0x02 },
	{ CCI_REG8(0x568f), 0x55 },
	{ CCI_REG8(0x5690), 0x01 },
	{ CCI_REG8(0x5691), 0x7a },
	{ CCI_REG8(0x5692), 0x02 },
	{ CCI_REG8(0x5693), 0x6c },
	{ CCI_REG8(0x5694), 0x01 },
	{ CCI_REG8(0x5695), 0x35 },
	{ CCI_REG8(0x5696), 0x02 },
	{ CCI_REG8(0x5697), 0x5b },
	{ CCI_REG8(0x5698), 0x01 },
	{ CCI_REG8(0x5699), 0x7a },
	{ CCI_REG8(0x569a), 0x02 },
	{ CCI_REG8(0x569b), 0x6c },
	{ CCI_REG8(0x569c), 0x01 },
	{ CCI_REG8(0x569d), 0x35 },
	{ CCI_REG8(0x569e), 0x02 },
	{ CCI_REG8(0x569f), 0x5b },
	{ CCI_REG8(0x56a0), 0x01 },
	{ CCI_REG8(0x56a1), 0x7a },
	{ CCI_REG8(0x56a2), 0x02 },
	{ CCI_REG8(0x56a3), 0x6c },
	{ CCI_REG8(0x56a4), 0x01 },
	{ CCI_REG8(0x56a5), 0x35 },
	{ CCI_REG8(0x56a6), 0x02 },
	{ CCI_REG8(0x56a7), 0x5b },
	{ CCI_REG8(0x56a8), 0x01 },
	{ CCI_REG8(0x56a9), 0x80 },
	{ CCI_REG8(0x56aa), 0x02 },
	{ CCI_REG8(0x56ab), 0x72 },
	{ CCI_REG8(0x56ac), 0x01 },
	{ CCI_REG8(0x56ad), 0x2f },
	{ CCI_REG8(0x56ae), 0x02 },
	{ CCI_REG8(0x56af), 0x55 },
	{ CCI_REG8(0x5902), 0x0e },
	{ CCI_REG8(0x5a50), 0x04 },
	{ CCI_REG8(0x5a51), 0x04 },
	{ CCI_REG8(0x5a69), 0x01 },
	{ CCI_REG8(0x5c49), 0x0d },
	{ CCI_REG8(0x5d60), 0x08 },
	{ CCI_REG8(0x5d61), 0x08 },
	{ CCI_REG8(0x5d62), 0x08 },
	{ CCI_REG8(0x5d63), 0x08 },
	{ CCI_REG8(0x5d64), 0x08 },
	{ CCI_REG8(0x5d67), 0x08 },
	{ CCI_REG8(0x5d6c), 0x08 },
	{ CCI_REG8(0x5d6e), 0x08 },
	{ CCI_REG8(0x5d71), 0x08 },
	{ CCI_REG8(0x5d8e), 0x14 },
	{ CCI_REG8(0x5d90), 0x03 },
	{ CCI_REG8(0x5d91), 0x0a },
	{ CCI_REG8(0x5d92), 0x1f },
	{ CCI_REG8(0x5d93), 0x05 },
	{ CCI_REG8(0x5d97), 0x1f },
	{ CCI_REG8(0x5d9a), 0x06 },
	{ CCI_REG8(0x5d9c), 0x1f },
	{ CCI_REG8(0x5da1), 0x1f },
	{ CCI_REG8(0x5da6), 0x1f },
	{ CCI_REG8(0x5da8), 0x1f },
	{ CCI_REG8(0x5dab), 0x1f },
	{ CCI_REG8(0x5dc0), 0x06 },
	{ CCI_REG8(0x5dc1), 0x06 },
	{ CCI_REG8(0x5dc2), 0x07 },
	{ CCI_REG8(0x5dc3), 0x06 },
	{ CCI_REG8(0x5dc4), 0x07 },
	{ CCI_REG8(0x5dc7), 0x07 },
	{ CCI_REG8(0x5dcc), 0x07 },
	{ CCI_REG8(0x5dce), 0x07 },
	{ CCI_REG8(0x5dd1), 0x07 },
	{ CCI_REG8(0x5e3e), 0x00 },
	{ CCI_REG8(0x5e3f), 0x00 },
	{ CCI_REG8(0x5e41), 0x00 },
	{ CCI_REG8(0x5e48), 0x00 },
	{ CCI_REG8(0x5e49), 0x00 },
	{ CCI_REG8(0x5e4a), 0x00 },
	{ CCI_REG8(0x5e4c), 0x00 },
	{ CCI_REG8(0x5e4d), 0x00 },
	{ CCI_REG8(0x5e4e), 0x00 },
	{ CCI_REG8(0x6026), 0x03 },
	{ CCI_REG8(0x6028), 0x03 },
	{ CCI_REG8(0x602a), 0x03 },
	{ CCI_REG8(0x602c), 0x03 },
	{ CCI_REG8(0x602f), 0x03 },
	{ CCI_REG8(0x6036), 0x03 },
	{ CCI_REG8(0x6038), 0x03 },
	{ CCI_REG8(0x603a), 0x03 },
	{ CCI_REG8(0x603c), 0x03 },
	{ CCI_REG8(0x603f), 0x03 },
	{ CCI_REG8(0x6074), 0x19 },
	{ CCI_REG8(0x6076), 0x19 },
	{ CCI_REG8(0x6078), 0x19 },
	{ CCI_REG8(0x607a), 0x19 },
	{ CCI_REG8(0x607d), 0x19 },
	{ CCI_REG8(0x6084), 0x32 },
	{ CCI_REG8(0x6086), 0x32 },
	{ CCI_REG8(0x6088), 0x32 },
	{ CCI_REG8(0x608a), 0x32 },
	{ CCI_REG8(0x608d), 0x32 },
	{ CCI_REG8(0x60c2), 0x4a },
	{ CCI_REG8(0x60c4), 0x4a },
	{ CCI_REG8(0x60cb), 0x4a },
	{ CCI_REG8(0x60d2), 0x4a },
	{ CCI_REG8(0x60d4), 0x4a },
	{ CCI_REG8(0x60db), 0x4a },
	{ CCI_REG8(0x62f9), 0x14 },
	{ CCI_REG8(0x6305), 0x13 },
	{ CCI_REG8(0x6307), 0x13 },
	{ CCI_REG8(0x630a), 0x13 },
	{ CCI_REG8(0x630d), 0x0d },
	{ CCI_REG8(0x6317), 0x0d },
	{ CCI_REG8(0x632f), 0x2e },
	{ CCI_REG8(0x6333), 0x2e },
	{ CCI_REG8(0x6339), 0x2e },
	{ CCI_REG8(0x6343), 0x2e },
	{ CCI_REG8(0x6347), 0x2e },
	{ CCI_REG8(0x634d), 0x2e },
	{ CCI_REG8(0x6352), 0x00 },
	{ CCI_REG8(0x6353), 0x5f },
	{ CCI_REG8(0x6366), 0x00 },
	{ CCI_REG8(0x6367), 0x5f },
	{ CCI_REG8(0x638f), 0x95 },
	{ CCI_REG8(0x6393), 0x95 },
	{ CCI_REG8(0x6399), 0x95 },
	{ CCI_REG8(0x63a3), 0x95 },
	{ CCI_REG8(0x63a7), 0x95 },
	{ CCI_REG8(0x63ad), 0x95 },
	{ CCI_REG8(0x63b2), 0x00 },
	{ CCI_REG8(0x63b3), 0xc6 },
	{ CCI_REG8(0x63c6), 0x00 },
	{ CCI_REG8(0x63c7), 0xc6 },
	{ CCI_REG8(0x8bdb), 0x02 },
	{ CCI_REG8(0x8bde), 0x02 },
	{ CCI_REG8(0x8be1), 0x2d },
	{ CCI_REG8(0x8be4), 0x00 },
	{ CCI_REG8(0x8be5), 0x00 },
	{ CCI_REG8(0x8be6), 0x01 },
	{ CCI_REG8(0x9002), 0x14 },
	{ CCI_REG8(0x9200), 0xb5 },
	{ CCI_REG8(0x9201), 0x9e },
	{ CCI_REG8(0x9202), 0xb5 },
	{ CCI_REG8(0x9203), 0x42 },
	{ CCI_REG8(0x9204), 0xb5 },
	{ CCI_REG8(0x9205), 0x43 },
	{ CCI_REG8(0x9206), 0xbd },
	{ CCI_REG8(0x9207), 0x20 },
	{ CCI_REG8(0x9208), 0xbd },
	{ CCI_REG8(0x9209), 0x22 },
	{ CCI_REG8(0x920a), 0xbd },
	{ CCI_REG8(0x920b), 0x23 },
	{ CCI_REG8(0xb5d7), 0x10 },
	{ CCI_REG8(0xbd24), 0x00 },
	{ CCI_REG8(0xbd25), 0x00 },
	{ CCI_REG8(0xbd26), 0x00 },
	{ CCI_REG8(0xbd27), 0x00 },
	{ CCI_REG8(0xbd28), 0x00 },
	{ CCI_REG8(0xbd29), 0x00 },
	{ CCI_REG8(0xbd2a), 0x00 },
	{ CCI_REG8(0xbd2b), 0x00 },
	{ CCI_REG8(0xbd2c), 0x32 },
	{ CCI_REG8(0xbd2d), 0x70 },
	{ CCI_REG8(0xbd2e), 0x25 },
	{ CCI_REG8(0xbd2f), 0x30 },
	{ CCI_REG8(0xbd30), 0x3b },
	{ CCI_REG8(0xbd31), 0xe0 },
	{ CCI_REG8(0xbd32), 0x69 },
	{ CCI_REG8(0xbd33), 0x40 },
	{ CCI_REG8(0xbd34), 0x25 },
	{ CCI_REG8(0xbd35), 0x90 },
	{ CCI_REG8(0xbd36), 0x58 },
	{ CCI_REG8(0xbd37), 0x00 },
	{ CCI_REG8(0xbd38), 0x00 },
	{ CCI_REG8(0xbd39), 0x00 },
	{ CCI_REG8(0xbd3a), 0x00 },
	{ CCI_REG8(0xbd3b), 0x00 },
	{ CCI_REG8(0xbd3c), 0x32 },
	{ CCI_REG8(0xbd3d), 0x70 },
	{ CCI_REG8(0xbd3e), 0x25 },
	{ CCI_REG8(0xbd3f), 0x90 },
	{ CCI_REG8(0xbd40), 0x58 },
	{ CCI_REG8(0xbd41), 0x00 },
};

static const struct cci_reg_sequence imx766_global2_regs[] = {
	{ CCI_REG8(0x793b), 0x01 },
	{ CCI_REG8(0xacc6), 0x00 },
	{ CCI_REG8(0xacf5), 0x00 },
	{ CCI_REG8(0x793b), 0x00 },
};

static const struct cci_reg_sequence imx766_global_for_12_regs[] = {
	{ CCI_REG8(0x1f04), 0xb3 },
	{ CCI_REG8(0x1f05), 0x01 },
	{ CCI_REG8(0x1f06), 0x07 },
	{ CCI_REG8(0x1f07), 0x66 },
	{ CCI_REG8(0x1f08), 0x01 },
	{ CCI_REG8(0x4d18), 0x00 },
	{ CCI_REG8(0x4d19), 0x9d },
	{ CCI_REG8(0x4d88), 0x00 },
	{ CCI_REG8(0x4d89), 0x97 },
	{ CCI_REG8(0x5c57), 0x0a },
	{ CCI_REG8(0x5d94), 0x1f },
	{ CCI_REG8(0x5d9e), 0x1f },
	{ CCI_REG8(0x5e50), 0x23 },
	{ CCI_REG8(0x5e51), 0x20 },
	{ CCI_REG8(0x5e52), 0x07 },
	{ CCI_REG8(0x5e53), 0x20 },
	{ CCI_REG8(0x5e54), 0x07 },
	{ CCI_REG8(0x5e55), 0x27 },
	{ CCI_REG8(0x5e56), 0x0b },
	{ CCI_REG8(0x5e57), 0x24 },
	{ CCI_REG8(0x5e58), 0x0b },
	{ CCI_REG8(0x5e60), 0x24 },
	{ CCI_REG8(0x5e61), 0x24 },
	{ CCI_REG8(0x5e62), 0x1b },
	{ CCI_REG8(0x5e63), 0x23 },
	{ CCI_REG8(0x5e64), 0x1b },
	{ CCI_REG8(0x5e65), 0x28 },
	{ CCI_REG8(0x5e66), 0x22 },
	{ CCI_REG8(0x5e67), 0x28 },
	{ CCI_REG8(0x5e68), 0x23 },
	{ CCI_REG8(0x5e70), 0x25 },
	{ CCI_REG8(0x5e71), 0x24 },
	{ CCI_REG8(0x5e72), 0x20 },
	{ CCI_REG8(0x5e73), 0x24 },
	{ CCI_REG8(0x5e74), 0x20 },
	{ CCI_REG8(0x5e75), 0x28 },
	{ CCI_REG8(0x5e76), 0x27 },
	{ CCI_REG8(0x5e77), 0x29 },
	{ CCI_REG8(0x5e78), 0x24 },
	{ CCI_REG8(0x5e80), 0x25 },
	{ CCI_REG8(0x5e81), 0x25 },
	{ CCI_REG8(0x5e82), 0x24 },
	{ CCI_REG8(0x5e83), 0x25 },
	{ CCI_REG8(0x5e84), 0x23 },
	{ CCI_REG8(0x5e85), 0x2a },
	{ CCI_REG8(0x5e86), 0x28 },
	{ CCI_REG8(0x5e87), 0x2a },
	{ CCI_REG8(0x5e88), 0x28 },
	{ CCI_REG8(0x5e90), 0x24 },
	{ CCI_REG8(0x5e91), 0x24 },
	{ CCI_REG8(0x5e92), 0x28 },
	{ CCI_REG8(0x5e93), 0x29 },
	{ CCI_REG8(0x5e97), 0x25 },
	{ CCI_REG8(0x5e98), 0x25 },
	{ CCI_REG8(0x5e99), 0x2a },
	{ CCI_REG8(0x5e9a), 0x2a },
	{ CCI_REG8(0x5e9e), 0x3a },
	{ CCI_REG8(0x5e9f), 0x3f },
	{ CCI_REG8(0x5ea0), 0x17 },
	{ CCI_REG8(0x5ea1), 0x3f },
	{ CCI_REG8(0x5ea2), 0x17 },
	{ CCI_REG8(0x5ea3), 0x32 },
	{ CCI_REG8(0x5ea4), 0x10 },
	{ CCI_REG8(0x5ea5), 0x33 },
	{ CCI_REG8(0x5ea6), 0x10 },
	{ CCI_REG8(0x5eae), 0x3d },
	{ CCI_REG8(0x5eaf), 0x48 },
	{ CCI_REG8(0x5eb0), 0x3b },
	{ CCI_REG8(0x5eb1), 0x45 },
	{ CCI_REG8(0x5eb2), 0x37 },
	{ CCI_REG8(0x5eb3), 0x3a },
	{ CCI_REG8(0x5eb4), 0x31 },
	{ CCI_REG8(0x5eb5), 0x3a },
	{ CCI_REG8(0x5eb6), 0x31 },
	{ CCI_REG8(0x5ebe), 0x40 },
	{ CCI_REG8(0x5ebf), 0x48 },
	{ CCI_REG8(0x5ec0), 0x3f },
	{ CCI_REG8(0x5ec1), 0x45 },
	{ CCI_REG8(0x5ec2), 0x3f },
	{ CCI_REG8(0x5ec3), 0x3a },
	{ CCI_REG8(0x5ec4), 0x32 },
	{ CCI_REG8(0x5ec5), 0x3a },
	{ CCI_REG8(0x5ec6), 0x33 },
	{ CCI_REG8(0x5ece), 0x4b },
	{ CCI_REG8(0x5ecf), 0x4a },
	{ CCI_REG8(0x5ed0), 0x48 },
	{ CCI_REG8(0x5ed1), 0x4c },
	{ CCI_REG8(0x5ed2), 0x45 },
	{ CCI_REG8(0x5ed3), 0x3f },
	{ CCI_REG8(0x5ed4), 0x3a },
	{ CCI_REG8(0x5ed5), 0x3f },
	{ CCI_REG8(0x5ed6), 0x3a },
	{ CCI_REG8(0x5ede), 0x48 },
	{ CCI_REG8(0x5edf), 0x45 },
	{ CCI_REG8(0x5ee0), 0x3a },
	{ CCI_REG8(0x5ee1), 0x3a },
	{ CCI_REG8(0x5ee5), 0x4a },
	{ CCI_REG8(0x5ee6), 0x4c },
	{ CCI_REG8(0x5ee7), 0x3f },
	{ CCI_REG8(0x5ee8), 0x3f },
	{ CCI_REG8(0x5eec), 0x06 },
	{ CCI_REG8(0x5eed), 0x06 },
	{ CCI_REG8(0x5eee), 0x02 },
	{ CCI_REG8(0x5eef), 0x06 },
	{ CCI_REG8(0x5ef0), 0x01 },
	{ CCI_REG8(0x5ef1), 0x09 },
	{ CCI_REG8(0x5ef2), 0x05 },
	{ CCI_REG8(0x5ef3), 0x06 },
	{ CCI_REG8(0x5ef4), 0x04 },
	{ CCI_REG8(0x5efc), 0x07 },
	{ CCI_REG8(0x5efd), 0x09 },
	{ CCI_REG8(0x5efe), 0x05 },
	{ CCI_REG8(0x5eff), 0x08 },
	{ CCI_REG8(0x5f00), 0x04 },
	{ CCI_REG8(0x5f01), 0x09 },
	{ CCI_REG8(0x5f02), 0x05 },
	{ CCI_REG8(0x5f03), 0x09 },
	{ CCI_REG8(0x5f04), 0x04 },
	{ CCI_REG8(0x5f0c), 0x08 },
	{ CCI_REG8(0x5f0d), 0x09 },
	{ CCI_REG8(0x5f0e), 0x06 },
	{ CCI_REG8(0x5f0f), 0x09 },
	{ CCI_REG8(0x5f10), 0x06 },
	{ CCI_REG8(0x5f11), 0x09 },
	{ CCI_REG8(0x5f12), 0x09 },
	{ CCI_REG8(0x5f13), 0x09 },
	{ CCI_REG8(0x5f14), 0x06 },
	{ CCI_REG8(0x5f1c), 0x09 },
	{ CCI_REG8(0x5f1d), 0x09 },
	{ CCI_REG8(0x5f1e), 0x09 },
	{ CCI_REG8(0x5f1f), 0x09 },
	{ CCI_REG8(0x5f20), 0x08 },
	{ CCI_REG8(0x5f21), 0x09 },
	{ CCI_REG8(0x5f22), 0x09 },
	{ CCI_REG8(0x5f23), 0x09 },
	{ CCI_REG8(0x5f24), 0x09 },
	{ CCI_REG8(0x5f2c), 0x09 },
	{ CCI_REG8(0x5f2d), 0x09 },
	{ CCI_REG8(0x5f2e), 0x09 },
	{ CCI_REG8(0x5f2f), 0x09 },
	{ CCI_REG8(0x5f33), 0x09 },
	{ CCI_REG8(0x5f34), 0x09 },
	{ CCI_REG8(0x5f35), 0x09 },
	{ CCI_REG8(0x5f36), 0x09 },
	{ CCI_REG8(0x5f3a), 0x01 },
	{ CCI_REG8(0x5f3d), 0x07 },
	{ CCI_REG8(0x5f3f), 0x01 },
	{ CCI_REG8(0x5f4b), 0x01 },
	{ CCI_REG8(0x5f4d), 0x04 },
	{ CCI_REG8(0x5f4f), 0x02 },
	{ CCI_REG8(0x5f51), 0x02 },
	{ CCI_REG8(0x5f5a), 0x02 },
	{ CCI_REG8(0x5f5b), 0x01 },
	{ CCI_REG8(0x5f5d), 0x03 },
	{ CCI_REG8(0x5f5e), 0x07 },
	{ CCI_REG8(0x5f5f), 0x01 },
	{ CCI_REG8(0x5f60), 0x01 },
	{ CCI_REG8(0x5f61), 0x01 },
	{ CCI_REG8(0x5f6a), 0x01 },
	{ CCI_REG8(0x5f6c), 0x01 },
	{ CCI_REG8(0x5f6d), 0x01 },
	{ CCI_REG8(0x5f6e), 0x04 },
	{ CCI_REG8(0x5f70), 0x02 },
	{ CCI_REG8(0x5f72), 0x02 },
	{ CCI_REG8(0x5f7a), 0x01 },
	{ CCI_REG8(0x5f7b), 0x03 },
	{ CCI_REG8(0x5f7c), 0x01 },
	{ CCI_REG8(0x5f7d), 0x01 },
	{ CCI_REG8(0x5f82), 0x01 },
	{ CCI_REG8(0x60c6), 0x4a },
	{ CCI_REG8(0x60c8), 0x4a },
	{ CCI_REG8(0x60d6), 0x4a },
	{ CCI_REG8(0x60d8), 0x4a },
	{ CCI_REG8(0x62e4), 0x33 },
	{ CCI_REG8(0x62e9), 0x33 },
	{ CCI_REG8(0x62ee), 0x1c },
	{ CCI_REG8(0x62ef), 0x33 },
	{ CCI_REG8(0x62f3), 0x33 },
	{ CCI_REG8(0x62f6), 0x1c },
	{ CCI_REG8(0x33f2), 0x01 },
	{ CCI_REG8(0x1f04), 0xa3 },
	{ CCI_REG8(0x1f05), 0x01 },
	{ CCI_REG8(0x406e), 0x00 },
	{ CCI_REG8(0x406f), 0x08 },
	{ CCI_REG8(0x4d08), 0x00 },
	{ CCI_REG8(0x4d09), 0x2c },
	{ CCI_REG8(0x4d0e), 0x00 },
	{ CCI_REG8(0x4d0f), 0x64 },
	{ CCI_REG8(0x4d18), 0x00 },
	{ CCI_REG8(0x4d19), 0xb1 },
	{ CCI_REG8(0x4d1e), 0x00 },
	{ CCI_REG8(0x4d1f), 0xcb },
	{ CCI_REG8(0x4d3a), 0x00 },
	{ CCI_REG8(0x4d3b), 0x91 },
	{ CCI_REG8(0x4d40), 0x00 },
	{ CCI_REG8(0x4d41), 0x64 },
	{ CCI_REG8(0x4d4c), 0x00 },
	{ CCI_REG8(0x4d4d), 0xe8 },
	{ CCI_REG8(0x4d52), 0x00 },
	{ CCI_REG8(0x4d53), 0xcb },
	{ CCI_REG8(0x4d78), 0x00 },
	{ CCI_REG8(0x4d79), 0x2c },
	{ CCI_REG8(0x4d7e), 0x00 },
	{ CCI_REG8(0x4d7f), 0x64 },
	{ CCI_REG8(0x4d88), 0x00 },
	{ CCI_REG8(0x4d89), 0xab },
	{ CCI_REG8(0x4d8e), 0x00 },
	{ CCI_REG8(0x4d8f), 0xcb },
	{ CCI_REG8(0x4da6), 0x00 },
	{ CCI_REG8(0x4da7), 0xe7 },
	{ CCI_REG8(0x4dac), 0x00 },
	{ CCI_REG8(0x4dad), 0xcb },
	{ CCI_REG8(0x5b98), 0x00 },
	{ CCI_REG8(0x5c52), 0x05 },
	{ CCI_REG8(0x5c57), 0x09 },
	{ CCI_REG8(0x5d94), 0x0a },
	{ CCI_REG8(0x5d9e), 0x0a },
	{ CCI_REG8(0x5e50), 0x22 },
	{ CCI_REG8(0x5e51), 0x22 },
	{ CCI_REG8(0x5e52), 0x07 },
	{ CCI_REG8(0x5e53), 0x20 },
	{ CCI_REG8(0x5e54), 0x06 },
	{ CCI_REG8(0x5e55), 0x23 },
	{ CCI_REG8(0x5e56), 0x0a },
	{ CCI_REG8(0x5e57), 0x23 },
	{ CCI_REG8(0x5e58), 0x0a },
	{ CCI_REG8(0x5e60), 0x25 },
	{ CCI_REG8(0x5e61), 0x29 },
	{ CCI_REG8(0x5e62), 0x1c },
	{ CCI_REG8(0x5e63), 0x26 },
	{ CCI_REG8(0x5e64), 0x1c },
	{ CCI_REG8(0x5e65), 0x2d },
	{ CCI_REG8(0x5e66), 0x1e },
	{ CCI_REG8(0x5e67), 0x2a },
	{ CCI_REG8(0x5e68), 0x1e },
	{ CCI_REG8(0x5e70), 0x26 },
	{ CCI_REG8(0x5e71), 0x26 },
	{ CCI_REG8(0x5e72), 0x22 },
	{ CCI_REG8(0x5e73), 0x23 },
	{ CCI_REG8(0x5e74), 0x20 },
	{ CCI_REG8(0x5e75), 0x28 },
	{ CCI_REG8(0x5e76), 0x23 },
	{ CCI_REG8(0x5e77), 0x28 },
	{ CCI_REG8(0x5e78), 0x23 },
	{ CCI_REG8(0x5e80), 0x28 },
	{ CCI_REG8(0x5e81), 0x28 },
	{ CCI_REG8(0x5e82), 0x29 },
	{ CCI_REG8(0x5e83), 0x27 },
	{ CCI_REG8(0x5e84), 0x26 },
	{ CCI_REG8(0x5e85), 0x2a },
	{ CCI_REG8(0x5e86), 0x2d },
	{ CCI_REG8(0x5e87), 0x2a },
	{ CCI_REG8(0x5e88), 0x2a },
	{ CCI_REG8(0x5e90), 0x26 },
	{ CCI_REG8(0x5e91), 0x23 },
	{ CCI_REG8(0x5e92), 0x28 },
	{ CCI_REG8(0x5e93), 0x28 },
	{ CCI_REG8(0x5e97), 0x2f },
	{ CCI_REG8(0x5e98), 0x2e },
	{ CCI_REG8(0x5e99), 0x32 },
	{ CCI_REG8(0x5e9a), 0x32 },
	{ CCI_REG8(0x5e9e), 0x50 },
	{ CCI_REG8(0x5e9f), 0x50 },
	{ CCI_REG8(0x5ea0), 0x1e },
	{ CCI_REG8(0x5ea1), 0x50 },
	{ CCI_REG8(0x5ea2), 0x1d },
	{ CCI_REG8(0x5ea3), 0x3e },
	{ CCI_REG8(0x5ea4), 0x14 },
	{ CCI_REG8(0x5ea5), 0x3e },
	{ CCI_REG8(0x5ea6), 0x14 },
	{ CCI_REG8(0x5eae), 0x58 },
	{ CCI_REG8(0x5eaf), 0x5e },
	{ CCI_REG8(0x5eb0), 0x4b },
	{ CCI_REG8(0x5eb1), 0x5a },
	{ CCI_REG8(0x5eb2), 0x4b },
	{ CCI_REG8(0x5eb3), 0x4c },
	{ CCI_REG8(0x5eb4), 0x3a },
	{ CCI_REG8(0x5eb5), 0x4c },
	{ CCI_REG8(0x5eb6), 0x38 },
	{ CCI_REG8(0x5ebe), 0x56 },
	{ CCI_REG8(0x5ebf), 0x57 },
	{ CCI_REG8(0x5ec0), 0x50 },
	{ CCI_REG8(0x5ec1), 0x55 },
	{ CCI_REG8(0x5ec2), 0x50 },
	{ CCI_REG8(0x5ec3), 0x46 },
	{ CCI_REG8(0x5ec4), 0x3e },
	{ CCI_REG8(0x5ec5), 0x46 },
	{ CCI_REG8(0x5ec6), 0x3e },
	{ CCI_REG8(0x5ece), 0x5a },
	{ CCI_REG8(0x5ecf), 0x5f },
	{ CCI_REG8(0x5ed0), 0x5e },
	{ CCI_REG8(0x5ed1), 0x5a },
	{ CCI_REG8(0x5ed2), 0x5a },
	{ CCI_REG8(0x5ed3), 0x50 },
	{ CCI_REG8(0x5ed4), 0x4c },
	{ CCI_REG8(0x5ed5), 0x50 },
	{ CCI_REG8(0x5ed6), 0x4c },
	{ CCI_REG8(0x5ede), 0x57 },
	{ CCI_REG8(0x5edf), 0x55 },
	{ CCI_REG8(0x5ee0), 0x46 },
	{ CCI_REG8(0x5ee1), 0x46 },
	{ CCI_REG8(0x5ee5), 0x73 },
	{ CCI_REG8(0x5ee6), 0x6e },
	{ CCI_REG8(0x5ee7), 0x5f },
	{ CCI_REG8(0x5ee8), 0x5a },
	{ CCI_REG8(0x5eec), 0x0a },
	{ CCI_REG8(0x5eed), 0x0a },
	{ CCI_REG8(0x5eee), 0x0f },
	{ CCI_REG8(0x5eef), 0x0a },
	{ CCI_REG8(0x5ef0), 0x0e },
	{ CCI_REG8(0x5ef1), 0x08 },
	{ CCI_REG8(0x5ef2), 0x0c },
	{ CCI_REG8(0x5ef3), 0x0c },
	{ CCI_REG8(0x5ef4), 0x0f },
	{ CCI_REG8(0x5efc), 0x0a },
	{ CCI_REG8(0x5efd), 0x0a },
	{ CCI_REG8(0x5efe), 0x14 },
	{ CCI_REG8(0x5eff), 0x0a },
	{ CCI_REG8(0x5f00), 0x14 },
	{ CCI_REG8(0x5f01), 0x0a },
	{ CCI_REG8(0x5f02), 0x14 },
	{ CCI_REG8(0x5f03), 0x0a },
	{ CCI_REG8(0x5f04), 0x19 },
	{ CCI_REG8(0x5f0c), 0x0a },
	{ CCI_REG8(0x5f0d), 0x0a },
	{ CCI_REG8(0x5f0e), 0x0a },
	{ CCI_REG8(0x5f0f), 0x05 },
	{ CCI_REG8(0x5f10), 0x0a },
	{ CCI_REG8(0x5f11), 0x06 },
	{ CCI_REG8(0x5f12), 0x08 },
	{ CCI_REG8(0x5f13), 0x0a },
	{ CCI_REG8(0x5f14), 0x0c },
	{ CCI_REG8(0x5f1c), 0x0a },
	{ CCI_REG8(0x5f1d), 0x0a },
	{ CCI_REG8(0x5f1e), 0x0a },
	{ CCI_REG8(0x5f1f), 0x0a },
	{ CCI_REG8(0x5f20), 0x0a },
	{ CCI_REG8(0x5f21), 0x0a },
	{ CCI_REG8(0x5f22), 0x0a },
	{ CCI_REG8(0x5f23), 0x0a },
	{ CCI_REG8(0x5f24), 0x0a },
	{ CCI_REG8(0x5f2c), 0x0a },
	{ CCI_REG8(0x5f2d), 0x05 },
	{ CCI_REG8(0x5f2e), 0x06 },
	{ CCI_REG8(0x5f2f), 0x0a },
	{ CCI_REG8(0x5f33), 0x0a },
	{ CCI_REG8(0x5f34), 0x0a },
	{ CCI_REG8(0x5f35), 0x0a },
	{ CCI_REG8(0x5f36), 0x0a },
	{ CCI_REG8(0x5f3a), 0x00 },
	{ CCI_REG8(0x5f3d), 0x02 },
	{ CCI_REG8(0x5f3f), 0x0a },
	{ CCI_REG8(0x5f4a), 0x0a },
	{ CCI_REG8(0x5f4b), 0x0a },
	{ CCI_REG8(0x5f4d), 0x0f },
	{ CCI_REG8(0x5f4f), 0x00 },
	{ CCI_REG8(0x5f51), 0x00 },
	{ CCI_REG8(0x5f5a), 0x00 },
	{ CCI_REG8(0x5f5b), 0x00 },
	{ CCI_REG8(0x5f5d), 0x0a },
	{ CCI_REG8(0x5f5e), 0x02 },
	{ CCI_REG8(0x5f5f), 0x0a },
	{ CCI_REG8(0x5f60), 0x0a },
	{ CCI_REG8(0x5f61), 0x00 },
	{ CCI_REG8(0x5f6a), 0x00 },
	{ CCI_REG8(0x5f6c), 0x0a },
	{ CCI_REG8(0x5f6d), 0x06 },
	{ CCI_REG8(0x5f6e), 0x0f },
	{ CCI_REG8(0x5f70), 0x00 },
	{ CCI_REG8(0x5f72), 0x00 },
	{ CCI_REG8(0x5f7a), 0x00 },
	{ CCI_REG8(0x5f7b), 0x0a },
	{ CCI_REG8(0x5f7c), 0x0a },
	{ CCI_REG8(0x5f7d), 0x00 },
	{ CCI_REG8(0x5f82), 0x06 },
	{ CCI_REG8(0x60c6), 0x36 },
	{ CCI_REG8(0x60c8), 0x36 },
	{ CCI_REG8(0x60d6), 0x36 },
	{ CCI_REG8(0x60d8), 0x36 },
	{ CCI_REG8(0x62df), 0x56 },
	{ CCI_REG8(0x62e0), 0x52 },
	{ CCI_REG8(0x62e4), 0x38 },
	{ CCI_REG8(0x62e5), 0x51 },
	{ CCI_REG8(0x62e9), 0x35 },
	{ CCI_REG8(0x62ea), 0x54 },
	{ CCI_REG8(0x62ee), 0x1d },
	{ CCI_REG8(0x62ef), 0x38 },
	{ CCI_REG8(0x62f3), 0x33 },
	{ CCI_REG8(0x62f6), 0x26 },
	{ CCI_REG8(0x6412), 0x1e },
	{ CCI_REG8(0x6413), 0x1e },
	{ CCI_REG8(0x6414), 0x1e },
	{ CCI_REG8(0x6415), 0x1e },
	{ CCI_REG8(0x6416), 0x1e },
	{ CCI_REG8(0x6417), 0x1e },
	{ CCI_REG8(0x6418), 0x1e },
	{ CCI_REG8(0x641a), 0x1e },
	{ CCI_REG8(0x641b), 0x1e },
	{ CCI_REG8(0x641c), 0x1e },
	{ CCI_REG8(0x641d), 0x1e },
	{ CCI_REG8(0x641e), 0x1e },
	{ CCI_REG8(0x641f), 0x1e },
	{ CCI_REG8(0x6420), 0x1e },
	{ CCI_REG8(0x6421), 0x1e },
	{ CCI_REG8(0x6422), 0x1e },
	{ CCI_REG8(0x6424), 0x1e },
	{ CCI_REG8(0x6425), 0x1e },
	{ CCI_REG8(0x6426), 0x1e },
	{ CCI_REG8(0x6427), 0x1e },
	{ CCI_REG8(0x6428), 0x1e },
	{ CCI_REG8(0x6429), 0x1e },
	{ CCI_REG8(0x642a), 0x1e },
	{ CCI_REG8(0x642b), 0x1e },
	{ CCI_REG8(0x642c), 0x1e },
	{ CCI_REG8(0x642e), 0x1e },
	{ CCI_REG8(0x642f), 0x1e },
	{ CCI_REG8(0x6430), 0x1e },
	{ CCI_REG8(0x6431), 0x1e },
	{ CCI_REG8(0x6432), 0x1e },
	{ CCI_REG8(0x6433), 0x1e },
	{ CCI_REG8(0x6434), 0x1e },
	{ CCI_REG8(0x6435), 0x1e },
	{ CCI_REG8(0x6436), 0x1e },
	{ CCI_REG8(0x6438), 0x1e },
	{ CCI_REG8(0x6439), 0x1e },
	{ CCI_REG8(0x643a), 0x1e },
	{ CCI_REG8(0x643b), 0x1e },
	{ CCI_REG8(0x643d), 0x1e },
	{ CCI_REG8(0x643e), 0x1e },
	{ CCI_REG8(0x643f), 0x1e },
	{ CCI_REG8(0x6441), 0x1e },
	{ CCI_REG8(0x33f2), 0x02 },
	{ CCI_REG8(0x1f08), 0x00 },
};

static bool imx766_mode_supported(struct imx766 *sensor,
				  const struct imx766_mode *mode)
{
	return test_bit(mode->link_freq_index,
			&sensor->link_freq_bitmap);
}

static int imx766_check_hwcfg(struct imx766 *sensor)
{
	struct device *dev = sensor->dev;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_CPHY,
	};
	struct fwnode_handle *ep;
	unsigned int i;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!ep)
		return dev_err_probe(dev, -EINVAL,
				     "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &ep_cfg);
	fwnode_handle_put(ep);
	if (ret) {
		v4l2_fwnode_endpoint_free(&ep_cfg);
		return dev_err_probe(dev, ret,
				     "failed to parse endpoint\n");
	}

	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 3) {
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported C-PHY trio count: %u\n",
				    ep_cfg.bus.mipi_csi2.num_data_lanes);
		goto out;
	}

	sensor->bus = ep_cfg.bus.mipi_csi2;

	ret = v4l2_link_freq_to_bitmap(dev,
				       ep_cfg.link_frequencies,
				       ep_cfg.nr_of_link_frequencies,
				       imx766_link_freq_menu,
				       ARRAY_SIZE(imx766_link_freq_menu),
				       &sensor->link_freq_bitmap);
	if (ret)
		goto out;

	for (i = 0; i < ARRAY_SIZE(imx766_modes); i++)
		if (imx766_mode_supported(sensor, &imx766_modes[i]))
			break;

	if (i == ARRAY_SIZE(imx766_modes))
		ret = dev_err_probe(dev, -EINVAL,
				    "no supported modes for endpoint\n");

out:
	v4l2_fwnode_endpoint_free(&ep_cfg);

	return ret;
}

static int imx766_identify_module(struct imx766 *sensor)
{
	int ret;
	u64 val;

	ret = cci_read(sensor->regmap, IMX766_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to read chip id\n");

	if (val != IMX766_CHIP_ID)
		return dev_err_probe(sensor->dev, -EIO,
				     "chip id mismatch: %x!=%llx\n",
				     IMX766_CHIP_ID, val);

	return 0;
}

static const struct imx766_pll_config *
imx766_get_pll_config(const struct imx766 *sensor,
		      const struct imx766_mode *mode)
{
	return &mode->pll_configs[sensor->inck_index];
}

static u64 imx766_get_pixel_rate(const struct imx766 *sensor,
				 const struct imx766_mode *mode)
{
	return imx766_get_pll_config(sensor, mode)->pixel_rate;
}

static int imx766_configure_pll(struct imx766 *sensor,
				const struct imx766_mode *mode)
{
	const struct imx766_pll_config *pll =
		imx766_get_pll_config(sensor, mode);
	int ret = 0;

	cci_write(sensor->regmap, IMX766_REG_IVT_PREPLLCK_DIV,
		  pll->ivt_prepllck_div, &ret);
	cci_write(sensor->regmap, IMX766_REG_IVT_PLL_MPY,
		  pll->ivt_pll_mpy, &ret);

	cci_write(sensor->regmap, IMX766_REG_IOP_PREPLLCK_DIV,
		  pll->iop_prepllck_div, &ret);
	cci_write(sensor->regmap, IMX766_REG_IOP_PLL_MPY,
		  pll->iop_pll_mpy, &ret);

	return ret;
}

static int imx766_configure_cphy_timing(struct imx766 *sensor,
					const struct imx766_mode *mode)
{
	const struct imx766_cphy_timing *timing;
	int ret = 0;

	if (mode->link_freq_index >= ARRAY_SIZE(imx766_cphy_timings))
		return -EINVAL;

	timing = &imx766_cphy_timings[mode->link_freq_index];

	cci_write(sensor->regmap, IMX766_REG_T3PREPARE,
		  timing->t3prepare, &ret);
	cci_write(sensor->regmap, IMX766_REG_T3LPX,
		  timing->t3lpx, &ret);
	cci_write(sensor->regmap, IMX766_REG_T3HSEXIT,
		  timing->t3hsexit, &ret);
	cci_write(sensor->regmap, IMX766_REG_T3PREBEGIN,
		  timing->t3prebegin, &ret);
	cci_write(sensor->regmap, IMX766_REG_T3PROGSEQ_EN,
		  IMX766_T3PROGSEQ_DISABLE, &ret);
	cci_write(sensor->regmap, IMX766_REG_T3POST,
		  timing->t3post, &ret);

	/*
	 * Select register-controlled Global Timing only after all
	 * timing parameters have been programmed.
	 */
	cci_write(sensor->regmap, IMX766_REG_PHY_CTRL,
		  IMX766_PHY_CTRL_REGISTERS, &ret);

	return ret;
}

static const struct imx766_mode *
imx766_get_mode(struct imx766 *sensor, u32 width, u32 height)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx766_modes); i++) {
		const struct imx766_mode *mode = &imx766_modes[i];

		if (!imx766_mode_supported(sensor, mode))
			continue;

		if (mode->width == width && mode->height == height)
			return mode;
	}

	return NULL;
}

static const struct imx766_mode *
imx766_get_default_mode(struct imx766 *sensor)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx766_modes); i++)
		if (imx766_mode_supported(sensor, &imx766_modes[i]))
			return &imx766_modes[i];

	return NULL;
}

static int imx766_update_exposure_range(struct imx766 *sensor,
					const struct imx766_mode *mode,
					u32 vblank)
{
	u32 exposure_max = mode->height + vblank -
			   IMX766_EXPOSURE_MARGIN;
	u32 exposure_def = clamp_t(u32, mode->frame_length_lines / 2,
			   mode->exposure_min, exposure_max);

	return __v4l2_ctrl_modify_range(sensor->exposure,
					mode->exposure_min,
					exposure_max,
					mode->exposure_step,
					exposure_def);
}

static int imx766_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx766 *sensor =
		container_of(ctrl->handler, struct imx766, ctrl_handler);
	struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *fmt;
	const struct imx766_mode *mode;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&sensor->sd);
	fmt = v4l2_subdev_state_get_format(state, 0);

	mode = imx766_get_mode(sensor, fmt->width, fmt->height);
	if (!mode)
		return -EINVAL;

	/*
	 * VBLANK changes the maximum permissible coarse integration time.
	 * Do this even while the sensor is powered down.
	 */
	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = imx766_update_exposure_range(sensor, mode,
						   ctrl->val);
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->regmap, IMX766_REG_COARSE_INTEG_TIME,
			  ctrl->val, &ret);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, IMX766_REG_ANA_GAIN_GLOBAL,
			  ctrl->val, &ret);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		cci_write(sensor->regmap, IMX766_REG_DIG_GAIN_GLOBAL,
			  ctrl->val, &ret);
		break;

	case V4L2_CID_VBLANK:
		cci_write(sensor->regmap, IMX766_REG_FRM_LENGTH_LINES,
			  mode->height + ctrl->val, &ret);
		break;

	default:
		break;
	}

	pm_runtime_put(sensor->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx766_ctrl_ops = {
	.s_ctrl = imx766_set_ctrl,
};

static int imx766_init_controls(struct imx766 *sensor)
{
	struct v4l2_fwnode_device_properties props;
	const struct imx766_mode *mode;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	u64 pixel_rate;
	u32 hblank;
	u32 vblank_min;
	u32 vblank_def;
	u32 vblank_max;
	u32 exposure_max;
	u32 exposure_def;
	int ret;

	mode = imx766_get_default_mode(sensor);
	if (!mode)
		return -EINVAL;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "failed to parse fwnode properties\n");

	hblank = mode->line_length_pck - mode->width;

	vblank_min = mode->min_frame_length_lines - mode->height;
	vblank_def = mode->frame_length_lines - mode->height;
	vblank_max = IMX766_FRM_LENGTH_MAX - mode->height;

	exposure_max = mode->frame_length_lines - IMX766_EXPOSURE_MARGIN;

	exposure_def = clamp_t(u32, exposure_max / 2,
			       mode->exposure_min, exposure_max);

	ret = v4l2_ctrl_handler_init(hdl, 8);
	if (ret)
		return ret;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &imx766_ctrl_ops,
					      &props);
	if (ret) {
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	pixel_rate = imx766_get_pixel_rate(sensor, mode);

	sensor->pixel_rate = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       pixel_rate,
					       pixel_rate,
					       1, pixel_rate);

	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->hblank = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
					  V4L2_CID_HBLANK,
					  hblank, hblank, 1, hblank);

	if (sensor->hblank)
		sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->vblank = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
					V4L2_CID_VBLANK,
					vblank_min,
					vblank_max,
					IMX766_FRM_LENGTH_STEP,
					vblank_def);

	sensor->exposure = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     mode->exposure_min,
					     exposure_max,
					     mode->exposure_step,
					     exposure_def);

	sensor->analogue_gain = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
						  V4L2_CID_ANALOGUE_GAIN,
						  IMX766_ANA_GAIN_MIN,
						  mode->analogue_gain_max,
						  IMX766_ANA_GAIN_STEP,
						  IMX766_ANA_GAIN_DEFAULT);

	sensor->digital_gain = v4l2_ctrl_new_std(hdl, &imx766_ctrl_ops,
						 V4L2_CID_DIGITAL_GAIN,
						 IMX766_DIG_GAIN_MIN,
						 IMX766_DIG_GAIN_MAX,
						 IMX766_DIG_GAIN_STEP,
						 IMX766_DIG_GAIN_DEFAULT);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	sensor->sd.ctrl_handler = hdl;

	return 0;
}

static int imx766_update_controls(struct imx766 *sensor,
				  const struct imx766_mode *mode)
{
	u32 hblank = mode->line_length_pck - mode->width;
	u32 vblank_min = mode->min_frame_length_lines - mode->height;
	u32 vblank_def = mode->frame_length_lines - mode->height;
	u32 vblank_max = IMX766_FRM_LENGTH_MAX - mode->height;
	u64 pixel_rate = imx766_get_pll_config(sensor, mode)->pixel_rate;
	int ret;

	ret = __v4l2_ctrl_modify_range(sensor->hblank,
				       hblank, hblank, 1, hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->vblank,
				       vblank_min, vblank_max,
				       IMX766_FRM_LENGTH_STEP,
				       vblank_def);
	if (ret)
		return ret;

	/*
	 * Reset the frame interval to the default of the selected mode.
	 * imx766_set_ctrl() also updates the exposure range.
	 */
	ret = __v4l2_ctrl_s_ctrl(sensor->vblank, vblank_def);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->analogue_gain,
				       IMX766_ANA_GAIN_MIN,
				       mode->analogue_gain_max,
				       IMX766_ANA_GAIN_STEP,
				       IMX766_ANA_GAIN_DEFAULT);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(sensor->pixel_rate,
					pixel_rate, pixel_rate,
					1, pixel_rate);
}

static int imx766_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(imx766_mbus_codes))
		return -EINVAL;

	code->code = imx766_mbus_codes[code->index];

	return 0;
}

static const struct imx766_mode *
imx766_find_nearest_mode(struct imx766 *sensor,
			 u32 width, u32 height)
{
	const struct imx766_mode *best = NULL;
	unsigned int best_error = UINT_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx766_modes); i++) {
		const struct imx766_mode *mode = &imx766_modes[i];
		unsigned int error;

		if (!imx766_mode_supported(sensor, mode))
			continue;

		error = abs((int)mode->width - (int)width) +
			abs((int)mode->height - (int)height);

		if (error < best_error) {
			best = mode;
			best_error = error;
		}
	}

	return best;
}

static int imx766_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx766 *sensor = to_imx766(sd);
	struct v4l2_mbus_framefmt *state_fmt;
	struct v4l2_rect *crop;
	const struct imx766_mode *mode;
	int ret = 0;

	if (fmt->pad)
		return -EINVAL;

	switch (fmt->format.code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		break;
	default:
		fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
		break;
	}

	mode = imx766_find_nearest_mode(sensor,
				       fmt->format.width,
				       fmt->format.height);
	if (!mode)
		return -EINVAL;

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	state_fmt = v4l2_subdev_state_get_format(state, fmt->pad);
	*state_fmt = fmt->format;

	crop = v4l2_subdev_state_get_crop(state, fmt->pad);
	*crop = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ret = imx766_update_controls(sensor, mode);

	return ret;
}

static int imx766_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, sel->pad);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = imx766_native_area;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = imx766_active_area;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx766_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx766 *sensor = to_imx766(sd);
	const struct imx766_mode *mode;
	unsigned int index = 0;
	unsigned int i;

	switch (fse->code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(imx766_modes); i++) {
		mode = &imx766_modes[i];

		if (!imx766_mode_supported(sensor, mode))
			continue;

		if (index++ != fse->index)
			continue;

		fse->min_width = mode->width;
		fse->max_width = mode->width;
		fse->min_height = mode->height;
		fse->max_height = mode->height;

		return 0;
	}

	return -EINVAL;
}

static int imx766_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx766 *sensor = to_imx766(sd);
	const struct imx766_mode *mode;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	};

	mode = imx766_get_default_mode(sensor);
	if (!mode)
		return -EINVAL;

	fmt.format.width = mode->width;
	fmt.format.height = mode->height;

	return imx766_set_pad_format(sd, state, &fmt);
}

static int imx766_start_streaming(struct imx766 *sensor,
				  const struct imx766_mode *mode,
				  u32 code)
{
	u16 csi_dt_fmt;
	int ret = 0;

	switch (code) {
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		csi_dt_fmt = IMX766_CSI_DT_FMT_RAW10;
		break;
	default:
		return -EINVAL;
	}

	cci_write(sensor->regmap, IMX766_REG_INCK_FREQ,
		  IMX766_INCK_FREQ(sensor->inck_freq), &ret);
	cci_write(sensor->regmap, IMX766_REG_C_VERSION, 0x05, &ret);
	cci_write(sensor->regmap, IMX766_REG_T_VERSION, 0x08, &ret);
	cci_write(sensor->regmap, IMX766_REG_CSI_SIG_MODE,
		  IMX766_CSI_SIG_MODE_CPHY, &ret);

	cci_multi_reg_write(sensor->regmap, imx766_global_regs,
			    ARRAY_SIZE(imx766_global_regs), &ret);
	cci_multi_reg_write(sensor->regmap, imx766_global2_regs,
			    ARRAY_SIZE(imx766_global2_regs), &ret);
	cci_multi_reg_write(sensor->regmap, imx766_global_for_12_regs,
			    ARRAY_SIZE(imx766_global_for_12_regs), &ret);

	cci_write(sensor->regmap, IMX766_REG_CSI_DT_FMT,
		csi_dt_fmt, &ret);
	cci_write(sensor->regmap, IMX766_REG_CSI_LANE_MODE,
		IMX766_CSI_CPHY_3_TRIO_LANE_MODE, &ret);
	cci_write(sensor->regmap, IMX766_REG_LINE_LENGTH_PCK,
		  mode->line_length_pck, &ret);

	if (!ret)
		ret = imx766_configure_pll(sensor, mode);

	/* No phase-pixel stream is represented by this driver. */
	cci_write(sensor->regmap, IMX766_REG_PHASE_PIX_OUT_EN, 0, &ret);

	cci_multi_reg_write(sensor->regmap, mode->reg_list,
			    mode->num_regs, &ret);

	if (!ret)
		ret = imx766_configure_cphy_timing(sensor, mode);

	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	if (ret)
		return ret;

	return cci_write(sensor->regmap, IMX766_REG_MODE_SELECT,
			IMX766_MODE_SELECT_STREAMING, NULL);
}

static int imx766_get_mbus_config(struct v4l2_subdev *sd,
				  unsigned int pad,
				  struct v4l2_mbus_config *config)
{
	struct imx766 *sensor = to_imx766(sd);
	const struct v4l2_mbus_framefmt *fmt;
	const struct imx766_mode *mode;
	struct v4l2_subdev_state *state;
	int ret = 0;

	if (pad)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	fmt = v4l2_subdev_state_get_format(state, pad);
	mode = imx766_get_mode(sensor, fmt->width, fmt->height);
	if (!mode) {
		ret = -EINVAL;
		goto unlock;
	}

	config->type = V4L2_MBUS_CSI2_CPHY;
	config->link_freq =
		imx766_link_freq_menu[mode->link_freq_index];
	config->bus.mipi_csi2 = sensor->bus;

unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int imx766_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct imx766 *sensor = to_imx766(sd);
	const struct v4l2_mbus_framefmt *fmt;
	const struct imx766_mode *mode;
	int ret;

	if (pad || streams_mask != BIT_ULL(0))
		return -EINVAL;

	fmt = v4l2_subdev_state_get_format(state, pad);

	mode = imx766_get_mode(sensor, fmt->width, fmt->height);
	if (!mode)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	ret = imx766_start_streaming(sensor, mode, fmt->code);
	if (ret) {
		pm_runtime_put(sensor->dev);
		return ret;
	}

	return 0;
}

static int imx766_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct imx766 *sensor = to_imx766(sd);
	int ret;

	if (pad != 0 || streams_mask != BIT_ULL(0))
		return -EINVAL;

	ret = cci_write(sensor->regmap, IMX766_REG_MODE_SELECT,
			IMX766_MODE_SELECT_STANDBY, NULL);

	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops imx766_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx766_pad_ops = {
	.enum_mbus_code = imx766_enum_mbus_code,
	.enum_frame_size = imx766_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx766_set_pad_format,
	.get_selection = imx766_get_selection,
	.get_mbus_config = imx766_get_mbus_config,
	.enable_streams = imx766_enable_streams,
	.disable_streams = imx766_disable_streams,
};

static const struct v4l2_subdev_ops imx766_subdev_ops = {
	.video = &imx766_video_ops,
	.pad = &imx766_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx766_internal_ops = {
	.init_state = imx766_init_state,
};

static int imx766_get_inck(struct imx766 *sensor)
{
	const struct imx766_inck_config *config;
	unsigned long rate;
	unsigned int i;

	sensor->inck = devm_v4l2_sensor_clk_get(sensor->dev, NULL);
	if (IS_ERR(sensor->inck))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->inck),
				     "failed to get input clock\n");

	rate = clk_get_rate(sensor->inck);

	for (i = 0; i < ARRAY_SIZE(imx766_inck_configs); i++) {
		config = &imx766_inck_configs[i];

		if (config->rate != rate)
			continue;

		sensor->inck_freq = rate;
		sensor->inck_index = config->index;

		return 0;
	}

	return dev_err_probe(sensor->dev, -EINVAL,
			     "unsupported input clock frequency: %lu Hz\n",
			     rate);
}

static int imx766_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx766 *sensor = to_imx766(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx766_supplies),
				    sensor->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(sensor->inck);
	if (ret < 0) {
		dev_err(dev, "failed to enable input clock: %d\n", ret);
		goto error_disable_regulators;
	}

	fsleep(IMX766_POWER_ON_TO_XCLR_US);

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	gpiod_set_value_cansleep(sensor->pwrn_gpio, 0);

	/*
	 * Also covers the 1.2 ms XCLR-to-CCI requirement.
	 * Streaming may be started after this returns.
	 */
	fsleep(IMX766_XCLR_TO_STREAMING_US);

	return 0;

error_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(imx766_supplies), sensor->supplies);
	return ret;
}

static int imx766_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx766 *sensor = to_imx766(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	gpiod_set_value_cansleep(sensor->pwrn_gpio, 1);
	clk_disable_unprepare(sensor->inck);
	regulator_bulk_disable(ARRAY_SIZE(imx766_supplies),
			       sensor->supplies);

	return 0;
}

static int imx766_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx766 *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(imx766_supplies),
					    imx766_supplies, &sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get regulators\n");

	sensor->reset_gpio = devm_gpiod_get(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");

	sensor->pwrn_gpio = devm_gpiod_get(dev, "pwrn",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwrn_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->pwrn_gpio),
				     "failed to get pwrn GPIO\n");

	ret = imx766_get_inck(sensor);
	if (ret)
		return ret;

	ret = imx766_check_hwcfg(sensor);
	if (ret)
		return ret;

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init CCI\n");

	v4l2_i2c_subdev_init(&sensor->sd, client, &imx766_subdev_ops);

	sensor->sd.internal_ops = &imx766_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to init media entity\n");

	ret = imx766_init_controls(sensor);
	if (ret)
		goto error_media_entity;

	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		dev_err_probe(dev, ret,
			      "failed to initialize subdev state\n");
		goto error_ctrl_handler;
	}

	ret = imx766_power_on(dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to power on\n");
		goto error_subdev_cleanup;
	}

	ret = imx766_identify_module(sensor);
	if (ret)
		goto error_power_off;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		dev_err_probe(dev, ret,
			      "failed to register sensor\n");
		goto error_runtime_pm;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

error_runtime_pm:
	pm_runtime_disable(dev);
	imx766_power_off(dev);
	pm_runtime_set_suspended(dev);
	goto error_subdev_cleanup;

error_power_off:
	imx766_power_off(dev);

error_subdev_cleanup:
	v4l2_subdev_cleanup(&sensor->sd);

error_ctrl_handler:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);

error_media_entity:
	media_entity_cleanup(&sensor->sd.entity);

	return ret;
}

static void imx766_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx766 *sensor = to_imx766(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);

	pm_runtime_disable(dev);

	if (!pm_runtime_status_suspended(dev)) {
		imx766_power_off(dev);
		pm_runtime_set_suspended(dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx766_pm_ops,
				 imx766_power_off, imx766_power_on, NULL);

static const struct of_device_id imx766_of_match[] = {
	{ .compatible = "sony,imx766" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx766_of_match);

static struct i2c_driver imx766_i2c_driver = {
	.driver = {
		.name = "imx766",
		.of_match_table = imx766_of_match,
		.pm = pm_ptr(&imx766_pm_ops),
	},
	.probe = imx766_probe,
	.remove = imx766_remove,
};
module_i2c_driver(imx766_i2c_driver);

MODULE_AUTHOR("Danila Tikhonov <danila@mainlining.org>");
MODULE_DESCRIPTION("Sony imx766 sensor driver");
MODULE_LICENSE("GPL");
