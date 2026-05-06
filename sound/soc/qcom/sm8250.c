// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "usb_offload_utils.h"
#include "sdw.h"

#define MI2S_BCLK_RATE		1536000
#define TDM_BCLK_RATE		6144000
#define NUM_TDM_SLOTS		8

static unsigned int tdm_slot_offset[8] = {0, 4, 8, 12, 16, 20, 24, 28};

struct sm8250_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	uint32_t clk_count[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack usb_offload_jack;
	bool usb_offload_jack_setup;
	struct snd_soc_jack dp_jack;
	bool jack_setup;
};

static int sm8250_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX:
		return qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);
	case USB_RX:
		return qcom_snd_usb_offload_jack_setup(rtd, &data->usb_offload_jack,
						       &data->usb_offload_jack_setup);
	default:
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
	}
}

static void sm8250_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == USB_RX)
		qcom_snd_usb_offload_jack_remove(rtd,
						 &data->usb_offload_jack_setup);

}

static int sm8250_tdm_snd_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	unsigned int rx_mask;
	int ret = 0, j;
	int channels, slot_width;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		slot_width = 16;
		break;
	default:
		dev_err(rtd->dev, "%s: invalid param format 0x%x\n",
				__func__, params_format(params));
		return -EINVAL;
	}

	channels = params_channels(params);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
				channels, tdm_slot_offset);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0xf, 0,
				8, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set tdm slot, err:%d\n",
					__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, channels,
				tdm_slot_offset, 0, NULL);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map, err:%d\n",
					__func__, ret);
			goto end;
		}
	}

	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		if (strstr(codec_dai->component->name_prefix, "PL")) {
			rx_mask = BIT(0);
		} else if (strstr(codec_dai->component->name_prefix, "PR")) {
			rx_mask = BIT(1);
		} else if (strstr(codec_dai->component->name_prefix, "SL")) {
			rx_mask = BIT(2);
		} else if (strstr(codec_dai->component->name_prefix, "SR")) {
			rx_mask = BIT(3);
		} else {
			rx_mask = 0;
			dev_warn(rtd->dev, "%s: codec DAI name '%s' not recognized\n",
				__func__, codec_dai->component->name_prefix);
		}
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0, rx_mask,
				NUM_TDM_SLOTS, slot_width);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set TDM slot, err:%d\n",
				__func__, ret);
			goto end;
		}
	}

end:
	return ret;
}

static int sm8250_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8250_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int j;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		if (++(data->clk_count[PRIMARY_MI2S_RX]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		if (++(data->clk_count[SECONDARY_MI2S_RX]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		if (++(data->clk_count[TERTIARY_MI2S_RX]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case QUINARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		if (++(data->clk_count[QUINARY_MI2S_RX]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case LPI_MI2S_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case LPI_MI2S_TX_3:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SENARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		if (++(data->clk_count[SENARY_MI2S_RX]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEN_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case PRIMARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;
		if (++(data->clk_count[PRIMARY_TDM_RX_0]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;
		if (++(data->clk_count[SECONDARY_TDM_RX_0]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;
		if (++(data->clk_count[TERTIARY_TDM_RX_0]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case QUATERNARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;
		if (++(data->clk_count[QUATERNARY_TDM_RX_0]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case QUINARY_TDM_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_B;
		if (++(data->clk_count[QUINARY_TDM_RX_0]) == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUIN_TDM_IBIT,
				TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		for_each_rtd_codec_dais(rtd, j, codec_dai)
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	default:
		break;
	}

	return qcom_snd_sdw_startup(substream);
}

static void sm8250_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		if (--(data->clk_count[PRIMARY_MI2S_RX]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case SECONDARY_MI2S_RX:
		if (--(data->clk_count[SECONDARY_MI2S_RX]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case TERTIARY_MI2S_RX:
		if (--(data->clk_count[TERTIARY_MI2S_RX]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case QUINARY_MI2S_RX:
		if (--(data->clk_count[QUINARY_MI2S_RX]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case SENARY_MI2S_RX:
		if (--(data->clk_count[SENARY_MI2S_RX]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEN_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case PRIMARY_TDM_RX_0:
		if (--(data->clk_count[PRIMARY_TDM_RX_0]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case SECONDARY_TDM_RX_0:
		if (--(data->clk_count[SECONDARY_TDM_RX_0]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case TERTIARY_TDM_RX_0:
		if (--(data->clk_count[TERTIARY_TDM_RX_0]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case QUATERNARY_TDM_RX_0:
		if (--(data->clk_count[QUATERNARY_TDM_RX_0]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUAD_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	case QUINARY_TDM_RX_0:
		if (--(data->clk_count[QUINARY_TDM_RX_0]) == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_QUIN_TDM_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	default:
		break;
	}

	qcom_snd_sdw_shutdown(substream);
}

static int sm8250_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;

	switch (cpu_dai->id) {
	case PRIMARY_TDM_RX_0...QUINARY_TDM_TX_7:
		ret = sm8250_tdm_snd_hw_params(substream, params);
		break;
	default:
		break;
	}

	return ret;
}

static const struct snd_soc_ops sm8250_be_ops = {
	.startup = sm8250_snd_startup,
	.shutdown = sm8250_snd_shutdown,
	.hw_free = sm8250_snd_hw_free,
	.hw_params = sm8250_snd_hw_params,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = &sm8250_be_ops;
		}
	}
}

static int sm8250_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8250_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	sm8250_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sm8250_dt_match[] = {
	{ .compatible = "fairphone,fp4-sndcard", .data = "sm7225" },
	{ .compatible = "fairphone,fp5-sndcard", .data = "qcm6490" },
	{ .compatible = "qcom,qrb2210-sndcard", .data = "qcm2290" },
	{ .compatible = "qcom,qrb4210-rb2-sndcard", .data = "sm4250" },
	{ .compatible = "qcom,qrb5165-rb5-sndcard", .data = "sm8250" },
	{ .compatible = "qcom,sdm660-sndcard", .data = "sdm660" },
	{ .compatible = "qcom,sm8250-sndcard", .data = "sm8250" },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8250_dt_match);

static struct platform_driver snd_sm8250_driver = {
	.probe  = sm8250_platform_probe,
	.driver = {
		.name = "snd-sm8250",
		.of_match_table = snd_sm8250_dt_match,
	},
};
module_platform_driver(snd_sm8250_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SM8250 ASoC Machine Driver");
MODULE_LICENSE("GPL");
