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

#define TFA98XX_TDM_CFG0		0x20
#define TFA98XX_TDM_CFG0_TDME		BIT(0)	/* enable interface */
#define TFA98XX_TDM_CFG0_TDMSPKE	BIT(1)	/* enable audio sink */

#define TFA98XX_TDM_CFG1		0x21
#define TFA98XX_TDM_CFG1_NBCK_MSK	GENMASK(3, 0)	/* BCK to FS ratio */

#define TFA98XX_TDM_CFG2		0x22
#define TFA98XX_TDM_CFG2_SLLN_MSK	GENMASK(4, 0)	/* bits per slot */
#define TFA98XX_TDM_CFG2_SSIZE_MSK	GENMASK(14, 10)	/* sample size */

#define TFA98XX_TDM_CFG3		0x23
#define TFA98XX_TDM_CFG3_SPKS_MSK	GENMASK(3, 0)	/* slot for sink 0 */

#define TFA98XX_KEY1			0x0f
#define TFA98XX_KEY1_UNHIDE		0x5a6b

#define TFA9894_REVISION		0x94

struct tfa98xx_rev {
	unsigned int rev;
	const struct reg_sequence *init;
	unsigned int num_init;
};

struct tfa98xx_chip {
	unsigned int id;
	const struct tfa98xx_rev *revs;
	unsigned int num_revs;
};

struct tfa98xx {
	struct regmap *regmap;
};

/*
 * Correction of the power-on defaults, taken verbatim from the vendor
 * driver. The values differ per die revision.
 */
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
	.revs		= tfa9894_revs,
	.num_revs	= ARRAY_SIZE(tfa9894_revs),
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

static const struct snd_soc_component_driver tfa98xx_component = {
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
	ret = snd_soc_component_update_bits(component, TFA98XX_TDM_CFG0,
					    TFA98XX_TDM_CFG0_TDME, 0);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_update_bits(component, TFA98XX_TDM_CFG1,
					    TFA98XX_TDM_CFG1_NBCK_MSK,
					    FIELD_PREP(TFA98XX_TDM_CFG1_NBCK_MSK, nbck));
	if (ret < 0)
		return ret;

	ret = snd_soc_component_update_bits(component, TFA98XX_TDM_CFG2,
					    TFA98XX_TDM_CFG2_SLLN_MSK |
					    TFA98XX_TDM_CFG2_SSIZE_MSK,
					    FIELD_PREP(TFA98XX_TDM_CFG2_SLLN_MSK, slotlen) |
					    FIELD_PREP(TFA98XX_TDM_CFG2_SSIZE_MSK, samplesize));
	if (ret < 0)
		return ret;

	return snd_soc_component_update_bits(component, TFA98XX_TDM_CFG0,
					    TFA98XX_TDM_CFG0_TDME,
					    TFA98XX_TDM_CFG0_TDME);
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

static int tfa98xx_init(struct regmap *regmap, const struct tfa98xx_rev *rev)
{
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

	/*
	 * Bypass the CoolFlux DSP: without the vendor firmware container it has
	 * nothing to run. AMPC hands control over the amplifier to the DSP and
	 * is set out of reset, so it has to be cleared as well - otherwise AMPE
	 * has no effect and the amplifier stays silent.
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

	/*
	 * Tell the hardware manager that the I2C configuration is complete,
	 * otherwise it never leaves the wait-for-settings state. The amplifier
	 * stays powered down until DAPM clears PWDN.
	 */
	ret = regmap_set_bits(regmap, TFA98XX_SYS_CTRL1,
			      TFA98XX_SYS_CTRL1_MANSCONF);
	if (ret)
		return ret;

	/* Route slot 0 of the TDM frame into the amplifier */
	ret = regmap_update_bits(regmap, TFA98XX_TDM_CFG3,
				 TFA98XX_TDM_CFG3_SPKS_MSK,
				 FIELD_PREP(TFA98XX_TDM_CFG3_SPKS_MSK, 0));
	if (ret)
		return ret;

	return regmap_set_bits(regmap, TFA98XX_TDM_CFG0,
			       TFA98XX_TDM_CFG0_TDMSPKE);
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

	tfa98xx->regmap = devm_regmap_init_i2c(i2c, &tfa98xx_regmap);
	if (IS_ERR(tfa98xx->regmap))
		return dev_err_probe(dev, PTR_ERR(tfa98xx->regmap),
				     "Failed to initialize regmap\n");

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

	ret = tfa98xx_init(tfa98xx->regmap, &chip->revs[i]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize device\n");

	return devm_snd_soc_register_component(dev, &tfa98xx_component,
					       &tfa98xx_dai, 1);
}

static const struct i2c_device_id tfa98xx_i2c_id[] = {
	{ "tfa9894", (kernel_ulong_t)&tfa9894_chip },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa98xx_i2c_id);

static const struct of_device_id tfa98xx_of_match[] = {
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
