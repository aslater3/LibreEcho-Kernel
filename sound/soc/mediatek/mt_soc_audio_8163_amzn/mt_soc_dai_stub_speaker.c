// SPDX-License-Identifier: GPL-2.0
/* Minimal playback-only CPU DAI for the radar_puffin speaker path. */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include "AudDrv_Common.h"
#include "AudDrv_Def.h"

static struct snd_soc_dai_driver radar_speaker_dai = {
	.name = "mt-soc-dl1dai-driver",
	.playback = {
		.stream_name = "DL1 Playback",
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
		.channels_min = 1,
		.channels_max = 1,
		.rate_min = 48000,
		.rate_max = 48000,
	},
};

static const struct snd_soc_component_driver radar_speaker_dai_component = {
	.name = MT_SOC_DAI_NAME,
};

static int radar_speaker_dai_probe(struct platform_device *pdev)
{
	int ret;

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	if (pdev->dev.of_node)
		dev_set_name(&pdev->dev, "%s", MT_SOC_DAI_NAME);

	ret = snd_soc_register_component(&pdev->dev,
					 &radar_speaker_dai_component,
					 &radar_speaker_dai, 1);
	pr_info("radar speaker DAI component registration returned %d\\n", ret);
	return ret;
}

static int radar_speaker_dai_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id radar_speaker_dai_of_ids[] = {
	{ .compatible = "mediatek,mt8163-soc-dai-stub" },
	{ }
};
MODULE_DEVICE_TABLE(of, radar_speaker_dai_of_ids);

static struct platform_driver radar_speaker_dai_driver = {
	.probe = radar_speaker_dai_probe,
	.remove = radar_speaker_dai_remove,
	.driver = {
		.name = MT_SOC_DAI_NAME,
		.owner = THIS_MODULE,
		.of_match_table = radar_speaker_dai_of_ids,
	},
};
module_platform_driver(radar_speaker_dai_driver);

MODULE_DESCRIPTION("radar_puffin playback-only CPU DAI");
MODULE_LICENSE("GPL v2");
