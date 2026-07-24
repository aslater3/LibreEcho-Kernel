// SPDX-License-Identifier: GPL-2.0
/* Minimal fail-closed ASoC card for radar_puffin speaker playback. */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt_amzn_mclk_speaker.h"
#include "mt_soc_speaker_gpio.h"

#define RADAR_SPEAKER_CODEC_COMPATIBLE "ti,tlv320aic32x4"
#define RADAR_SPEAKER_DAI_COMPATIBLE "mediatek,mt8163-soc-dai-stub"
#define RADAR_SPEAKER_MCLK_HZ 9600000U
#define RADAR_SPEAKER_RATE_HZ 48000U


static struct platform_device *radar_speaker_device;
static struct device_node *radar_speaker_codec_node;
static struct device_node *radar_speaker_dai_node;
static DEFINE_MUTEX(radar_speaker_control_lock);
static bool radar_speaker_started;
static bool radar_speaker_route_enabled;
static bool radar_speaker_amp_enabled;

static int radar_speaker_quiesce_locked(struct snd_soc_dai *codec_dai)
{
	int ret = 0;
	int rc;

	/* The codec mute asserts MFP2 before muting the DAC. */
	rc = snd_soc_dai_digital_mute(codec_dai, 1,
				      SNDRV_PCM_STREAM_PLAYBACK);
	if (rc && rc != -ENOTSUPP)
		ret = rc;

	rc = radar_speaker_gpio_amp(0);
	if (rc) {
		if (!ret)
			ret = rc;
	} else {
		radar_speaker_amp_enabled = false;
	}

	rc = radar_speaker_gpio_dac_route(0);
	if (rc) {
		if (!ret)
			ret = rc;
	} else {
		radar_speaker_route_enabled = false;
	}

	return ret;
}

static int radar_speaker_quiesce(struct snd_soc_dai *codec_dai)
{
	int ret;

	mutex_lock(&radar_speaker_control_lock);
	ret = radar_speaker_quiesce_locked(codec_dai);
	mutex_unlock(&radar_speaker_control_lock);
	return ret;
}

static int radar_speaker_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	ret = radar_speaker_quiesce(codec_dai);
	if (ret)
		return ret;

	ret = radar_speaker_gpio_i2s(1);
	if (ret)
		goto fail_closed;

	ret = radar_speaker_gpio_mclk();
	if (ret)
		goto fail_closed;

	ret = radar_speaker_mclk_enable(RADAR_SPEAKER_MCLK_HZ);
	if (ret)
		goto fail_closed;

	mutex_lock(&radar_speaker_control_lock);
	radar_speaker_started = true;
	mutex_unlock(&radar_speaker_control_lock);

	snd_soc_dapm_force_enable_pin(&rtd->card->dapm, "HPR");
	snd_soc_dapm_sync(&rtd->card->dapm);
	return 0;

fail_closed:
	radar_speaker_mclk_disable();
	radar_speaker_gpio_i2s(0);
	radar_speaker_quiesce(codec_dai);
	return ret;
}

static void radar_speaker_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;

	mutex_lock(&radar_speaker_control_lock);
	radar_speaker_started = false;
	ret = radar_speaker_quiesce_locked(rtd->codec_dai);
	mutex_unlock(&radar_speaker_control_lock);
	if (ret)
		dev_err(rtd->card->dev, "speaker quiesce failed: %d\n", ret);

	snd_soc_dapm_disable_pin(&rtd->card->dapm, "HPR");
	snd_soc_dapm_sync(&rtd->card->dapm);

	ret = radar_speaker_mclk_disable();
	if (ret)
		dev_err(rtd->card->dev, "speaker MCLK disable failed: %d\n", ret);

	ret = radar_speaker_gpio_i2s(0);
	if (ret)
		dev_err(rtd->card->dev, "speaker I2S pin disable failed: %d\n", ret);

}

static int radar_speaker_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	mutex_lock(&radar_speaker_control_lock);
	ret = radar_speaker_started ? 0 : -EIO;
	mutex_unlock(&radar_speaker_control_lock);
	if (ret)
		return -EIO;
	if (params_rate(params) != RADAR_SPEAKER_RATE_HZ ||
	    params_channels(params) != 1 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS);
	if (ret) {
		radar_speaker_quiesce(codec_dai);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, RADAR_SPEAKER_MCLK_HZ,
				     SND_SOC_CLOCK_IN);
	if (ret) {
		radar_speaker_quiesce(codec_dai);
		return ret;
	}

	return 0;
}

static int radar_speaker_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	return radar_speaker_quiesce(rtd->codec_dai);
}

static int radar_speaker_route_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	(void)kcontrol;
	mutex_lock(&radar_speaker_control_lock);
	ucontrol->value.integer.value[0] = radar_speaker_route_enabled;
	mutex_unlock(&radar_speaker_control_lock);
	return 0;
}

static int radar_speaker_route_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	long requested = ucontrol->value.integer.value[0];
	bool enable;
	int ret;

	(void)kcontrol;
	if (requested != 0 && requested != 1)
		return -EINVAL;
	enable = requested != 0;

	mutex_lock(&radar_speaker_control_lock);
	if (enable == radar_speaker_route_enabled) {
		ret = 0;
		goto out;
	}
	if (enable) {
		if (!radar_speaker_started) {
			ret = -EPERM;
			goto out;
		}
		ret = radar_speaker_gpio_dac_route(1);
		if (!ret)
			radar_speaker_route_enabled = true;
	} else {
		if (radar_speaker_amp_enabled) {
			ret = -EBUSY;
			goto out;
		}
		ret = radar_speaker_gpio_dac_route(0);
		if (!ret)
			radar_speaker_route_enabled = false;
	}
	if (!ret)
		ret = 1;
out:
	mutex_unlock(&radar_speaker_control_lock);
	return ret;
}

static int radar_speaker_amp_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	(void)kcontrol;
	mutex_lock(&radar_speaker_control_lock);
	ucontrol->value.integer.value[0] = radar_speaker_amp_enabled;
	mutex_unlock(&radar_speaker_control_lock);
	return 0;
}

static int radar_speaker_amp_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = &card->rtd[0];
	long requested = ucontrol->value.integer.value[0];
	bool enable;
	int ret;
	int rc;

	if (requested != 0 && requested != 1)
		return -EINVAL;
	enable = requested != 0;

	mutex_lock(&radar_speaker_control_lock);
	if (enable == radar_speaker_amp_enabled) {
		ret = 0;
		goto out;
	}
	if (enable) {
		if (!radar_speaker_started || !radar_speaker_route_enabled) {
			ret = -EPERM;
			goto out;
		}
		ret = snd_soc_dai_digital_mute(rtd->codec_dai, 0,
					       SNDRV_PCM_STREAM_PLAYBACK);
		if (ret && ret != -ENOTSUPP) {
			radar_speaker_quiesce_locked(rtd->codec_dai);
			goto out;
		}
		ret = radar_speaker_gpio_amp(1);
		if (ret) {
			radar_speaker_quiesce_locked(rtd->codec_dai);
			goto out;
		}
		radar_speaker_amp_enabled = true;
		ret = 1;
	} else {
		ret = 0;
		rc = snd_soc_dai_digital_mute(rtd->codec_dai, 1,
					      SNDRV_PCM_STREAM_PLAYBACK);
		if (rc && rc != -ENOTSUPP)
			ret = rc;
		rc = radar_speaker_gpio_amp(0);
		if (rc) {
			if (!ret)
				ret = rc;
		} else {
			radar_speaker_amp_enabled = false;
			if (!ret)
				ret = 1;
		}
	}
out:
	mutex_unlock(&radar_speaker_control_lock);
	return ret;
}

static int radar_speaker_link_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;

	ret = radar_speaker_quiesce(rtd->codec_dai);
	if (ret)
		return ret;

	return radar_speaker_gpio_i2s(0);
}

static struct snd_soc_ops radar_speaker_ops = {
	.startup = radar_speaker_startup,
	.shutdown = radar_speaker_shutdown,
	.hw_params = radar_speaker_hw_params,
	.hw_free = radar_speaker_hw_free,
};

static const struct snd_kcontrol_new radar_speaker_controls[] = {
	SOC_SINGLE_BOOL_EXT("Speaker DAC Route", 0,
			    radar_speaker_route_get, radar_speaker_route_put),
	SOC_SINGLE_BOOL_EXT("Speaker Amp Enable", 0,
			    radar_speaker_amp_get, radar_speaker_amp_put),
};

static struct snd_soc_dai_link radar_speaker_links[] = {
	{
		.name = "TI_DAC_Playback",
		.stream_name = "TLV320AIC3204 Playback",
		.cpu_dai_name = NULL,
		.platform_name = "mt-soc-i2s0dl1-pcm",
		.codec_dai_name = "tlv320aic32x4-hifi",
		.playback_only = 1,
		.init = radar_speaker_link_init,
		.ops = &radar_speaker_ops,
		.ignore_pmdown_time = 1,
	},
};

static struct snd_soc_card radar_speaker_card = {
	.name = "mt-snd-card",
	.owner = THIS_MODULE,
	.dai_link = radar_speaker_links,
	.num_links = ARRAY_SIZE(radar_speaker_links),
	.controls = radar_speaker_controls,
	.num_controls = ARRAY_SIZE(radar_speaker_controls),
};

static int __init radar_speaker_init(void)
{
	int ret;

	radar_speaker_codec_node =
		of_find_compatible_node(NULL, NULL,
					RADAR_SPEAKER_CODEC_COMPATIBLE);
	if (!radar_speaker_codec_node) {
		pr_info("radar speaker codec is absent; card remains disabled\n");
		return -ENODEV;
	}

	radar_speaker_dai_node =
		of_find_compatible_node(NULL, NULL,
					RADAR_SPEAKER_DAI_COMPATIBLE);
	if (!radar_speaker_dai_node) {
		pr_info("radar speaker CPU DAI is absent; card remains disabled\n");
		ret = -ENODEV;
		goto put_codec_node;
	}

	radar_speaker_links[0].codec_of_node = radar_speaker_codec_node;
	radar_speaker_links[0].cpu_of_node = radar_speaker_dai_node;
	radar_speaker_device = platform_device_alloc("soc-audio", -1);
	if (!radar_speaker_device) {
		ret = -ENOMEM;
		goto put_codec_node;
	}

	platform_set_drvdata(radar_speaker_device, &radar_speaker_card);
	ret = platform_device_add(radar_speaker_device);
	if (ret)
		goto put_device;

	return 0;

put_device:
	platform_device_put(radar_speaker_device);
	radar_speaker_device = NULL;
put_codec_node:
	of_node_put(radar_speaker_codec_node);
	radar_speaker_codec_node = NULL;
	radar_speaker_links[0].codec_of_node = NULL;
	of_node_put(radar_speaker_dai_node);
	radar_speaker_dai_node = NULL;
	radar_speaker_links[0].cpu_of_node = NULL;
	return ret;
}
module_init(radar_speaker_init);

static void __exit radar_speaker_exit(void)
{
	if (radar_speaker_device)
		platform_device_unregister(radar_speaker_device);
	radar_speaker_links[0].codec_of_node = NULL;
	radar_speaker_links[0].cpu_of_node = NULL;
	of_node_put(radar_speaker_codec_node);
	radar_speaker_codec_node = NULL;
	of_node_put(radar_speaker_dai_node);
	radar_speaker_dai_node = NULL;
}
module_exit(radar_speaker_exit);

MODULE_DESCRIPTION("radar_puffin speaker-only ASoC card");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mt-snd-card");
