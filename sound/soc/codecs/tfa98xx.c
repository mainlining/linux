// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for NXP/Goodix TFA98xx (TFA2) smart speaker amplifiers.
 *
 * Copyright David Heidelberg <david@ixit.cz>
 *
 * Register sequences taken from the NXP/Goodix vendor driver:
 * Copyright NXP Semiconductors
 * Copyright GOODIX
 *
 * These amplifiers contain a CoolFlux DSP which needs a vendor-specific
 * firmware container to run. This driver keeps the DSP disabled and feeds
 * the TDM input straight to the amplifier, so the speaker protection
 * (excursion and thermal modelling) provided by the DSP is not available.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define TFA98XX_SYS_CTRL0		0x00
#define TFA98XX_SYS_CTRL0_PWDN		0	/* power down */
#define TFA98XX_SYS_CTRL0_CFE		2	/* enable CoolFlux DSP */
#define TFA98XX_SYS_CTRL0_AMPE		3	/* enable amplifier */
#define TFA98XX_SYS_CTRL0_AMPC		6	/* amplifier enabled by DSP */

#define TFA98XX_SYS_CTRL1		0x01
#define TFA98XX_SYS_CTRL1_AMPINSEL_MSK	GENMASK(1, 0)	/* amp input select */
#define TFA98XX_SYS_CTRL1_MANSCONF	BIT(2)	/* I2C settings configured */

#define TFA98XX_AUDIO_CTRL		0x02
#define TFA98XX_AUDIO_CTRL_AUDFS_MSK	GENMASK(3, 0)	/* sample rate */

#define TFA98XX_REVISION		0x03
#define TFA98XX_REVISION_ID_MSK		GENMASK(7, 0)

/* Status and live-data registers, 0x10..0x1f */
#define TFA98XX_STATUS_FIRST		0x10
#define TFA98XX_STATUS_LAST		0x1f

#define TFA98XX_KEY1			0x0f
#define TFA98XX_KEY1_UNHIDE		0x5a6b

#define TFA9872_REVISION		0x72
#define TFA9873_REVISION		0x73
#define TFA9894_REVISION		0x94

/* TDM interface fields, laid out differently per chip */
enum {
	F_TDME,		/* enable interface */
	F_NBCK,		/* BCK to FS ratio */
	F_SLLN,		/* bits per slot */
	F_SSIZE,	/* sample size */
	F_SPKE,		/* enable audio sink 0 */
	F_SPKS,		/* slot for sink 0 */
	F_NUM
};

struct tfa98xx_rev {
	unsigned int rev;
	const struct reg_sequence *init;
	unsigned int num_init;
};

struct tfa98xx_chip {
	unsigned int id;
	bool has_dsp;
	const struct tfa98xx_rev *revs;
	unsigned int num_revs;
	struct reg_field fields[F_NUM];
	const struct snd_kcontrol_new *controls;
	unsigned int num_controls;
};

struct tfa98xx {
	struct regmap *regmap;
	const struct tfa98xx_chip *chip;
	struct regmap_field *fields[F_NUM];
	u32 channel_index;
};

/* TDMSPKG, the TDM to amplifier gain: 6 dB + 1 dB per step */
static const DECLARE_TLV_DB_SCALE(tfa98xx_spkg_tlv, 600, 100, 0);

static int tfa98xx_spkg_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	int ret, err;

	/* The gain register is behind the hide key */
	ret = snd_soc_component_write(component, TFA98XX_KEY1,
				      TFA98XX_KEY1_UNHIDE);
	if (ret < 0)
		return ret;

	ret = snd_soc_put_volsw(kcontrol, ucontrol);

	err = snd_soc_component_write(component, TFA98XX_KEY1, 0);
	if (err < 0)
		return err;

	return ret;
}

#define TFA98XX_SPKG(reg, shift) \
	SOC_SINGLE_EXT_TLV("Speaker Driver Playback Volume", reg, shift, \
			   15, 0, snd_soc_get_volsw, tfa98xx_spkg_put, \
			   tfa98xx_spkg_tlv)

static const struct snd_kcontrol_new tfa9872_controls[] = {
	TFA98XX_SPKG(0x61, 6),
};

static const struct snd_kcontrol_new tfa9873_controls[] = {
	TFA98XX_SPKG(0x5f, 6),
};

static const struct snd_kcontrol_new tfa9894_controls[] = {
	TFA98XX_SPKG(0x57, 4),
};

/*
 * Correction of the power-on defaults, taken verbatim from the vendor
 * driver. The values differ per die revision.
 */
static const struct reg_sequence tfa9872_rev1a_init[] = {
	{ 0x00, 0x1801 }, { 0x02, 0x2dc8 }, { 0x20, 0x0890 },
	{ 0x22, 0x043c }, { 0x51, 0x0000 }, { 0x52, 0x1a1c },
	{ 0x58, 0x161c }, { 0x61, 0x0198 }, { 0x65, 0x0a8b },
	{ 0x70, 0x07f5 }, { 0x74, 0xcc84 }, { 0x82, 0x01ed },
	{ 0x83, 0x0014 }, { 0x84, 0x0021 }, { 0x85, 0x0001 },
};

static const struct reg_sequence tfa9872_rev1b_init[] = {
	{ 0x02, 0x2dc8 }, { 0x20, 0x0890 }, { 0x22, 0x043c },
	{ 0x23, 0x0001 }, { 0x51, 0x0000 }, { 0x52, 0x5a1c },
	{ 0x61, 0x0198 }, { 0x63, 0x0a9a }, { 0x65, 0x0a82 },
	{ 0x6f, 0x01e3 }, { 0x70, 0x06fd }, { 0x71, 0x307e },
	{ 0x74, 0xcc84 }, { 0x75, 0x1132 }, { 0x82, 0x01ed },
	{ 0x83, 0x001a },
};

static const struct tfa98xx_rev tfa9872_revs[] = {
	{ 0x1a72, tfa9872_rev1a_init, ARRAY_SIZE(tfa9872_rev1a_init) },
	{ 0x2a72, tfa9872_rev1a_init, ARRAY_SIZE(tfa9872_rev1a_init) },
	{ 0x1b72, tfa9872_rev1b_init, ARRAY_SIZE(tfa9872_rev1b_init) },
	{ 0x2b72, tfa9872_rev1b_init, ARRAY_SIZE(tfa9872_rev1b_init) },
	{ 0x3b72, tfa9872_rev1b_init, ARRAY_SIZE(tfa9872_rev1b_init) },
};

static const struct tfa98xx_chip tfa9872_chip = {
	.id		= TFA9872_REVISION,
	.revs		= tfa9872_revs,
	.num_revs	= ARRAY_SIZE(tfa9872_revs),
	.controls	= tfa9872_controls,
	.num_controls	= ARRAY_SIZE(tfa9872_controls),
	.fields		= {
		[F_TDME]	= REG_FIELD(0x20, 4, 4),
		[F_NBCK]	= REG_FIELD(0x20, 12, 15),
		[F_SLLN]	= REG_FIELD(0x21, 4, 8),
		[F_SSIZE]	= REG_FIELD(0x22, 2, 6),
		[F_SPKE]	= REG_FIELD(0x23, 0, 0),
		[F_SPKS]	= REG_FIELD(0x26, 0, 3),
	},
};


static const struct reg_sequence tfa9873_rev0a_init[] = {
	{ 0x02, 0x0628 }, { 0x4c, 0x00e9 }, { 0x52, 0x17d0 },
	{ 0x56, 0x0011 }, { 0x58, 0x0200 }, { 0x59, 0x0001 },
	{ 0x5f, 0x0180 }, { 0x61, 0x0183 }, { 0x63, 0x055a },
	{ 0x65, 0x0542 }, { 0x6f, 0x00a3 }, { 0x70, 0xa3fb },
	{ 0x71, 0x007e }, { 0x83, 0x009a }, { 0x84, 0x0211 },
	{ 0x85, 0x0382 }, { 0x8c, 0x0210 }, { 0xd5, 0x0000 },
};

static const struct reg_sequence tfa9873_rev0b_init[] = {
	{ 0x02, 0x0628 }, { 0x61, 0x0183 }, { 0x63, 0x005a },
	{ 0x6f, 0x0082 }, { 0x70, 0xa3eb }, { 0x73, 0x0187 },
	{ 0x83, 0x071c }, { 0x85, 0x0380 }, { 0xd5, 0x004d },
};

static const struct reg_sequence tfa9873_rev1a_init[] = {};

static const struct tfa98xx_rev tfa9873_revs[] = {
	{ 0x0a73, tfa9873_rev0a_init, ARRAY_SIZE(tfa9873_rev0a_init) },
	{ 0x0b73, tfa9873_rev0b_init, ARRAY_SIZE(tfa9873_rev0b_init) },
	{ 0x1a73, tfa9873_rev1a_init, ARRAY_SIZE(tfa9873_rev1a_init) },
};

static const struct tfa98xx_chip tfa9873_chip = {
	.id		= TFA9873_REVISION,
	.revs		= tfa9873_revs,
	.num_revs	= ARRAY_SIZE(tfa9873_revs),
	.controls	= tfa9873_controls,
	.num_controls	= ARRAY_SIZE(tfa9873_controls),
	.fields		= {
		[F_TDME]	= REG_FIELD(0x20, 0, 0),
		[F_NBCK]	= REG_FIELD(0x20, 12, 15),
		[F_SLLN]	= REG_FIELD(0x21, 4, 8),
		[F_SSIZE]	= REG_FIELD(0x22, 2, 6),
		[F_SPKE]	= REG_FIELD(0x23, 0, 0),
		[F_SPKS]	= REG_FIELD(0x26, 0, 3),
	},
};

static const struct reg_sequence tfa9894_rev0a_init[] = {
	{ 0x00, 0xa245 }, { 0x02, 0x51e8 }, { 0x52, 0xbe17 },
	{ 0x57, 0x0344 }, { 0x61, 0x0033 }, { 0x71, 0x00cf },
	{ 0x72, 0x34a9 }, { 0x73, 0x3808 }, { 0x76, 0x0067 },
	{ 0x80, 0x0000 }, { 0x81, 0x5715 }, { 0x82, 0x0104 },
};

static const struct reg_sequence tfa9894_rev1a_init[] = {
	{ 0x00, 0xa245 }, { 0x01, 0x15da }, { 0x02, 0x5288 },
	{ 0x52, 0xbe17 }, { 0x53, 0x0dbe }, { 0x56, 0x05c3 },
	{ 0x57, 0x0344 }, { 0x61, 0x0032 }, { 0x71, 0x00cf },
	{ 0x72, 0x34a9 }, { 0x73, 0x38c8 }, { 0x76, 0x0067 },
	{ 0x80, 0x0000 }, { 0x81, 0x5799 }, { 0x82, 0x0104 },
};

static const struct reg_sequence tfa9894_rev2a_init[] = {
	{ 0x01, 0x15da }, { 0x02, 0x51e8 }, { 0x04, 0x0200 },
	{ 0x52, 0xbe17 }, { 0x53, 0x0dbe }, { 0x57, 0x0344 },
	{ 0x61, 0x0032 }, { 0x71, 0x6ecf }, { 0x72, 0xb4a9 },
	{ 0x73, 0x38c8 }, { 0x76, 0x0067 }, { 0x80, 0x0000 },
	{ 0x81, 0x5799 }, { 0x82, 0x0104 },
};

static const struct tfa98xx_rev tfa9894_revs[] = {
	{ 0x0a94, tfa9894_rev0a_init, ARRAY_SIZE(tfa9894_rev0a_init) },
	{ 0x1a94, tfa9894_rev1a_init, ARRAY_SIZE(tfa9894_rev1a_init) },
	{ 0x2a94, tfa9894_rev2a_init, ARRAY_SIZE(tfa9894_rev2a_init) },
	{ 0x3a94, tfa9894_rev2a_init, ARRAY_SIZE(tfa9894_rev2a_init) },
};

static const struct tfa98xx_chip tfa9894_chip = {
	.id		= TFA9894_REVISION,
	.has_dsp	= true,
	.revs		= tfa9894_revs,
	.num_revs	= ARRAY_SIZE(tfa9894_revs),
	.controls	= tfa9894_controls,
	.num_controls	= ARRAY_SIZE(tfa9894_controls),
	.fields		= {
		[F_TDME]	= REG_FIELD(0x20, 0, 0),
		[F_SPKE]	= REG_FIELD(0x20, 1, 1),
		[F_NBCK]	= REG_FIELD(0x21, 0, 3),
		[F_SLLN]	= REG_FIELD(0x22, 0, 4),
		[F_SSIZE]	= REG_FIELD(0x22, 10, 14),
		[F_SPKS]	= REG_FIELD(0x23, 0, 3),
	},
};

static bool tfa98xx_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg >= TFA98XX_STATUS_FIRST && reg <= TFA98XX_STATUS_LAST;
}

static bool tfa98xx_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg != TFA98XX_REVISION && !tfa98xx_volatile_reg(dev, reg);
}

static const struct regmap_config tfa98xx_regmap = {
	.reg_bits	= 8,
	.val_bits	= 16,

	.max_register	= 0xff,
	.writeable_reg	= tfa98xx_writeable_reg,
	.volatile_reg	= tfa98xx_volatile_reg,
	.cache_type	= REGCACHE_MAPLE,
};

static const struct snd_soc_dapm_widget tfa98xx_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_SUPPLY("POWER", TFA98XX_SYS_CTRL0,
			    TFA98XX_SYS_CTRL0_PWDN, 1, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("AMPE", TFA98XX_SYS_CTRL0,
			     TFA98XX_SYS_CTRL0_AMPE, 0, NULL, 0),

	SND_SOC_DAPM_AIF_IN("AIFIN", "HiFi Playback", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route tfa98xx_dapm_routes[] = {
	{ "OUT", NULL, "AMPE" },
	{ "AMPE", NULL, "POWER" },
	{ "AMPE", NULL, "AIFIN" },
};

static int tfa98xx_component_probe(struct snd_soc_component *component)
{
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);

	return snd_soc_add_component_controls(component,
					      tfa98xx->chip->controls,
					      tfa98xx->chip->num_controls);
}

static const struct snd_soc_component_driver tfa98xx_component = {
	.probe			= tfa98xx_component_probe,
	.dapm_widgets		= tfa98xx_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa98xx_dapm_widgets),
	.dapm_routes		= tfa98xx_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa98xx_dapm_routes),
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

/* Indexed by the AUDFS field value */
static const unsigned int tfa98xx_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static int tfa98xx_find_sample_rate(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tfa98xx_rates); ++i)
		if (tfa98xx_rates[i] == rate)
			return i;

	return -EINVAL;
}

static int tfa98xx_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tfa98xx *tfa98xx = snd_soc_component_get_drvdata(component);
	unsigned int nbck, slotlen, samplesize;
	int sr, ret;

	sr = tfa98xx_find_sample_rate(params_rate(params));
	if (sr < 0)
		return sr;

	switch (params_width(params)) {
	case 16:
		/* 16-bit sample in a 16-bit slot, 32 BCK per frame */
		nbck = 0;
		slotlen = 16 - 1;
		samplesize = 16 - 1;
		break;
	case 24:
	case 32:
		/* 24-bit sample in a 32-bit slot, 64 BCK per frame */
		nbck = 2;
		slotlen = 32 - 1;
		samplesize = 24 - 1;
		break;
	default:
		return -EINVAL;
	}

	ret = snd_soc_component_update_bits(component, TFA98XX_AUDIO_CTRL,
					    TFA98XX_AUDIO_CTRL_AUDFS_MSK,
					    FIELD_PREP(TFA98XX_AUDIO_CTRL_AUDFS_MSK, sr));
	if (ret < 0)
		return ret;

	/* The interface must be disabled while its framing is reprogrammed */
	ret = regmap_field_write(tfa98xx->fields[F_TDME], 0);
	if (ret)
		return ret;

	ret = regmap_field_write(tfa98xx->fields[F_NBCK], nbck);
	if (ret)
		return ret;

	ret = regmap_field_write(tfa98xx->fields[F_SLLN], slotlen);
	if (ret)
		return ret;

	ret = regmap_field_write(tfa98xx->fields[F_SSIZE], samplesize);
	if (ret)
		return ret;

	return regmap_field_write(tfa98xx->fields[F_TDME], 1);
}

static int tfa98xx_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_DSP_A:
		break;
	default:
		return -EINVAL;
	}

	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops tfa98xx_dai_ops = {
	.hw_params	= tfa98xx_hw_params,
	.set_fmt	= tfa98xx_set_fmt,
};

static struct snd_soc_dai_driver tfa98xx_dai = {
	.name = "tfa98xx-hifi",
	.playback = {
		.stream_name	= "HiFi Playback",
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.rate_min	= 8000,
		.rate_max	= 48000,
		.channels_min	= 1,
		.channels_max	= 2,
	},
	.ops = &tfa98xx_dai_ops,
};

static int tfa98xx_init(struct tfa98xx *tfa98xx,
			const struct tfa98xx_chip *chip,
			const struct tfa98xx_rev *rev)
{
	struct regmap *regmap = tfa98xx->regmap;
	int ret;

	/* The correction sequences touch registers behind the hide key */
	ret = regmap_write(regmap, TFA98XX_KEY1, TFA98XX_KEY1_UNHIDE);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(regmap, rev->init, rev->num_init);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TFA98XX_KEY1, 0);
	if (ret)
		return ret;

	if (chip->has_dsp) {
		/*
		 * Bypass the CoolFlux DSP: without the vendor firmware
		 * container it has nothing to run. AMPC hands control over the
		 * amplifier to the DSP and is set out of reset, so it has to
		 * be cleared as well - otherwise AMPE has no effect and the
		 * amplifier stays silent.
		 */
		ret = regmap_clear_bits(regmap, TFA98XX_SYS_CTRL0,
					BIT(TFA98XX_SYS_CTRL0_CFE) |
					BIT(TFA98XX_SYS_CTRL0_AMPC));
		if (ret)
			return ret;

		/* Take the amplifier input straight from the TDM interface */
		ret = regmap_update_bits(regmap, TFA98XX_SYS_CTRL1,
					 TFA98XX_SYS_CTRL1_AMPINSEL_MSK, 0);
		if (ret)
			return ret;
	}

	/*
	 * Tell the hardware manager that the I2C configuration is complete,
	 * otherwise it never leaves the wait-for-settings state. The amplifier
	 * stays powered down until DAPM clears PWDN.
	 */
	ret = regmap_set_bits(regmap, TFA98XX_SYS_CTRL1,
			      TFA98XX_SYS_CTRL1_MANSCONF);
	if (ret)
		return ret;

	/* Route slot taken from device tree of the TDM frame into the amplifier */
	ret = regmap_field_write(tfa98xx->fields[F_SPKS], tfa98xx->channel_index);
	if (ret)
		return ret;

	return regmap_field_write(tfa98xx->fields[F_SPKE], 1);
}

static int tfa98xx_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	const struct tfa98xx_chip *chip;
	struct gpio_desc *reset_gpiod;
	struct tfa98xx *tfa98xx;
	unsigned int rev, i;
	int ret;

	chip = i2c_get_match_data(i2c);
	if (!chip)
		return -EINVAL;

	tfa98xx = devm_kzalloc(dev, sizeof(*tfa98xx), GFP_KERNEL);
	if (!tfa98xx)
		return -ENOMEM;

	tfa98xx->chip = chip;

	tfa98xx->regmap = devm_regmap_init_i2c(i2c, &tfa98xx_regmap);
	if (IS_ERR(tfa98xx->regmap))
		return dev_err_probe(dev, PTR_ERR(tfa98xx->regmap),
				     "Failed to initialize regmap\n");

	ret = devm_regmap_field_bulk_alloc(dev, tfa98xx->regmap,
					   tfa98xx->fields, chip->fields,
					   F_NUM);
	if (ret)
		return ret;

	i2c_set_clientdata(i2c, tfa98xx);

	reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpiod))
		return dev_err_probe(dev, PTR_ERR(reset_gpiod),
				     "Failed to get reset GPIO\n");

	if (reset_gpiod) {
		fsleep(1000);
		gpiod_set_value_cansleep(reset_gpiod, 0);
		fsleep(1000);
	}

	ret = regmap_read(tfa98xx->regmap, TFA98XX_REVISION, &rev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to read revision register\n");

	if (FIELD_GET(TFA98XX_REVISION_ID_MSK, rev) != chip->id)
		return dev_err_probe(dev, -ENODEV,
				     "Unexpected device revision 0x%04x\n", rev);

	for (i = 0; i < chip->num_revs; i++)
		if (chip->revs[i].rev == rev)
			break;

	if (i == chip->num_revs)
		return dev_err_probe(dev, -ENODEV,
				     "Unsupported die revision 0x%04x\n", rev);

	if (!of_property_read_u32(dev->of_node, "sound-channel", &tfa98xx->channel_index))
		tfa98xx->channel_index = 0;

	ret = tfa98xx_init(tfa98xx, chip, &chip->revs[i]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize device\n");

	return devm_snd_soc_register_component(dev, &tfa98xx_component,
					       &tfa98xx_dai, 1);
}

static const struct i2c_device_id tfa98xx_i2c_id[] = {
	{ "tfa9872", (kernel_ulong_t)&tfa9872_chip },
	{ "tfa9873", (kernel_ulong_t)&tfa9873_chip },
	{ "tfa9894", (kernel_ulong_t)&tfa9894_chip },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa98xx_i2c_id);

static const struct of_device_id tfa98xx_of_match[] = {
	{ .compatible = "nxp,tfa9872", .data = &tfa9872_chip },
	{ .compatible = "nxp,tfa9873", .data = &tfa9873_chip },
	{ .compatible = "nxp,tfa9894", .data = &tfa9894_chip },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa98xx_of_match);

static struct i2c_driver tfa98xx_i2c_driver = {
	.driver = {
		.name = "tfa98xx",
		.of_match_table = tfa98xx_of_match,
	},
	.probe = tfa98xx_i2c_probe,
	.id_table = tfa98xx_i2c_id,
};
module_i2c_driver(tfa98xx_i2c_driver);

MODULE_DESCRIPTION("ASoC NXP/Goodix TFA98xx (TFA2) amplifier driver");
MODULE_AUTHOR("David Heidelberg <david@ixit.cz>");
MODULE_LICENSE("GPL");
