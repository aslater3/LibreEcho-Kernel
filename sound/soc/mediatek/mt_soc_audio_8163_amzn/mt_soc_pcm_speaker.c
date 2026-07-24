// SPDX-License-Identifier: GPL-2.0
/* Fixed-function MT8163 DL1 -> I2S speaker PCM. */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt_amzn_mclk_speaker.h"
#include "mt_soc_speaker_afe.h"
#include "mt_soc_speaker_clock.h"
#include "mt_soc_speaker_gpio.h"

#define RADAR_SPEAKER_PCM_NAME "mt-soc-i2s0dl1-pcm"
#define RADAR_SPEAKER_RATE 48000U
#define RADAR_SPEAKER_BUFFER_MAX 0x6000U
#define RADAR_SPEAKER_ALIGNMENT 64U
#define RADAR_SPEAKER_DMA_MAX 0xffffffffULL

#define AFE_DAC_CON0 0x0010U
#define AFE_DAC_CON1 0x0014U
#define AFE_CONN0 0x0020U
#define AFE_CONN1 0x0024U
#define AFE_CONN2 0x0028U
#define AFE_DL1_BASE 0x0040U
#define AFE_DL1_CUR 0x0044U
#define AFE_DL1_END 0x0048U
#define AFE_I2S_CON3 0x004cU
#define AFE_CONN_24BIT 0x006cU
#define AFE_IRQ_MCU_CON 0x03a0U
#define AFE_IRQ_MCU_STATUS 0x03a4U
#define AFE_IRQ_MCU_CLR 0x03a8U
#define AFE_IRQ_MCU_CNT1 0x03acU
#define AFE_MEMIF_PBUF_SIZE 0x03d8U

#define AFE_RATE_48K 10U
#define AFE_DL1_ENABLE (1U << 1)
#define AFE_GLOBAL_ENABLE (1U << 0)
#define AFE_IRQ1_ENABLE (1U << 0)
#define AFE_I2S_ENABLE (1U << 0)
#define AFE_OTHER_MEMIF_ENABLE_MASK 0x000007fcU
#define AFE_DL1_MONO (1U << 21)
#define AFE_DL1_FORMAT_MASK (3U << 16)
#define AFE_I2S_FORMAT_I2S (1U << 3)
#define AFE_I2S_WORD_32 (1U << 1)

/* DL1 inputs I05/I06 to the external I2S outputs O00/O01. */
#define AFE_I05_TO_O00 (1U << 5)
#define AFE_I06_TO_O01 (1U << 22)

struct radar_speaker_pcm_state {
	struct device *dev;
	void __iomem *sram;
	dma_addr_t sram_phys;
	size_t sram_size;
	struct snd_pcm_substream *substream;
	struct mutex stream_lock;
	spinlock_t irq_lock;
	int irq;
	bool running;
	bool platform_registered;
};

static struct radar_speaker_pcm_state radar_pcm;

static const struct snd_pcm_hardware radar_speaker_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RADAR_SPEAKER_RATE,
	.rate_max = RADAR_SPEAKER_RATE,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = RADAR_SPEAKER_BUFFER_MAX,
	.period_bytes_min = RADAR_SPEAKER_ALIGNMENT,
	.period_bytes_max = RADAR_SPEAKER_BUFFER_MAX / 2,
	.periods_min = 2,
	.periods_max = RADAR_SPEAKER_BUFFER_MAX / RADAR_SPEAKER_ALIGNMENT,
	.fifo_size = 0,
};

static int radar_speaker_pcm_stop_locked(void)
{
	unsigned long flags;
	u32 dac_con0;
	int first = 0;
	int ret;

	spin_lock_irqsave(&radar_pcm.irq_lock, flags);
	radar_pcm.running = false;
	spin_unlock_irqrestore(&radar_pcm.irq_lock, flags);

	ret = radar_speaker_afe_update_bits(AFE_IRQ_MCU_CON,
					    AFE_IRQ1_ENABLE, 0);
	if (ret)
		first = ret;
	ret = radar_speaker_afe_write(AFE_IRQ_MCU_CLR, AFE_IRQ1_ENABLE);
	if (ret && !first)
		first = ret;
	ret = radar_speaker_afe_update_bits(AFE_DAC_CON0,
					    AFE_DL1_ENABLE, 0);
	if (ret && !first)
		first = ret;
	ret = radar_speaker_afe_update_bits(AFE_CONN0,
					    AFE_I05_TO_O00 | AFE_I06_TO_O01,
					    0);
	if (ret && !first)
		first = ret;
	ret = radar_speaker_afe_update_bits(AFE_I2S_CON3,
					    AFE_I2S_ENABLE, 0);
	if (ret && !first)
		first = ret;

	/* The global gate is shared by all MEMIFs; clear only our last use. */
	ret = radar_speaker_afe_read(AFE_DAC_CON0, &dac_con0);
	if (ret) {
		if (!first)
			first = ret;
	} else if (!(dac_con0 & AFE_OTHER_MEMIF_ENABLE_MASK)) {
		ret = radar_speaker_afe_update_bits(AFE_DAC_CON0,
						    AFE_GLOBAL_ENABLE, 0);
		if (ret && !first)
			first = ret;
	}
	return first;
}

static irqreturn_t radar_speaker_pcm_irq(int irq, void *data)
{
	struct radar_speaker_pcm_state *state = data;
	struct snd_pcm_substream *substream = NULL;
	unsigned long flags;
	u32 status;

	(void)irq;
	if (radar_speaker_afe_read(AFE_IRQ_MCU_STATUS, &status))
		return IRQ_NONE;
	if (!(status & AFE_IRQ1_ENABLE))
		return IRQ_NONE;
	(void)radar_speaker_afe_write(AFE_IRQ_MCU_CLR, AFE_IRQ1_ENABLE);

	spin_lock_irqsave(&state->irq_lock, flags);
	if (state->running)
		substream = state->substream;
	spin_unlock_irqrestore(&state->irq_lock, flags);
	if (substream)
		snd_pcm_period_elapsed(substream);
	return IRQ_HANDLED;
}

static int radar_speaker_pcm_open(struct snd_pcm_substream *substream)
{
	int ret;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	mutex_lock(&radar_pcm.stream_lock);
	if (radar_pcm.substream) {
		ret = -EBUSY;
		goto out;
	}
	ret = radar_speaker_clock_enable();
	if (ret)
		goto out;

	substream->runtime->hw = radar_speaker_pcm_hardware;
	substream->runtime->dma_area = (void *)radar_pcm.sram;
	substream->runtime->dma_addr = radar_pcm.sram_phys;
	substream->runtime->dma_bytes = radar_pcm.sram_size;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 RADAR_SPEAKER_ALIGNMENT);
	if (ret)
		goto disable_clock;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 RADAR_SPEAKER_ALIGNMENT);
	if (ret)
		goto disable_clock;
	radar_pcm.substream = substream;
	goto out;

disable_clock:
	(void)radar_speaker_clock_disable();
out:
	mutex_unlock(&radar_pcm.stream_lock);
	return ret;
}

static int radar_speaker_pcm_close(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	int first;
	int ret;

	mutex_lock(&radar_pcm.stream_lock);
	first = radar_speaker_pcm_stop_locked();
	synchronize_irq(radar_pcm.irq);
	spin_lock_irqsave(&radar_pcm.irq_lock, flags);
	radar_pcm.substream = NULL;
	spin_unlock_irqrestore(&radar_pcm.irq_lock, flags);
	ret = radar_speaker_clock_disable();
	if (ret && !first)
		first = ret;
	mutex_unlock(&radar_pcm.stream_lock);
	return first;
}

static int radar_speaker_pcm_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	size_t bytes = params_buffer_bytes(params);

	if (params_rate(params) != RADAR_SPEAKER_RATE ||
	    params_channels(params) != 1 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE ||
	    !bytes || bytes > radar_pcm.sram_size ||
	    bytes > RADAR_SPEAKER_BUFFER_MAX ||
	    (bytes & (RADAR_SPEAKER_ALIGNMENT - 1U)))
		return -EINVAL;

	substream->runtime->dma_area = (void *)radar_pcm.sram;
	substream->runtime->dma_addr = radar_pcm.sram_phys;
	substream->runtime->dma_bytes = bytes;
	if (radar_speaker_afe_write(AFE_DL1_BASE, (u32)radar_pcm.sram_phys))
		return -EIO;
	if (radar_speaker_afe_write(AFE_DL1_END,
				    (u32)(radar_pcm.sram_phys + bytes - 1U)))
		return -EIO;
	return 0;
}

static int radar_speaker_pcm_prepare(struct snd_pcm_substream *substream)
{
	u32 i2s = (AFE_RATE_48K << 8) | AFE_I2S_FORMAT_I2S |
		  AFE_I2S_WORD_32;
	int ret;

	memset_io(radar_pcm.sram, 0, substream->runtime->dma_bytes);
	ret = radar_speaker_afe_update_bits(AFE_MEMIF_PBUF_SIZE,
					    AFE_DL1_FORMAT_MASK, 0);
	if (ret)
		return ret;
	ret = radar_speaker_afe_update_bits(AFE_DAC_CON1, 0x0fU,
					    AFE_RATE_48K);
	if (ret)
		return ret;
	ret = radar_speaker_afe_update_bits(AFE_DAC_CON1,
					    AFE_DL1_MONO, AFE_DL1_MONO);
	if (ret)
		return ret;
	ret = radar_speaker_afe_update_bits(AFE_CONN_24BIT,
					    (1U << 0) | (1U << 1), 0);
	if (ret)
		return ret;
	ret = radar_speaker_afe_write(AFE_I2S_CON3, i2s);
	if (ret)
		return ret;
	ret = radar_speaker_afe_write(AFE_IRQ_MCU_CNT1,
				    (u32)substream->runtime->period_size);
	if (ret)
		return ret;
	ret = radar_speaker_afe_update_bits(AFE_IRQ_MCU_CON, 0x0fU << 4,
					    AFE_RATE_48K << 4);
	if (ret)
		return ret;
	return radar_speaker_pcm_stop_locked();
}

static int radar_speaker_pcm_trigger(struct snd_pcm_substream *substream,
				     int command)
{
	unsigned long flags;
	int ret;

	(void)substream;
	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		ret = radar_speaker_afe_update_bits(AFE_CONN0,
						    AFE_I05_TO_O00 |
						    AFE_I06_TO_O01,
						    AFE_I05_TO_O00 |
						    AFE_I06_TO_O01);
		if (ret)
			return ret;
		ret = radar_speaker_afe_write(AFE_IRQ_MCU_CLR,
					      AFE_IRQ1_ENABLE);
		if (ret)
			goto fail;
		ret = radar_speaker_afe_update_bits(AFE_I2S_CON3,
						    AFE_I2S_ENABLE,
						    AFE_I2S_ENABLE);
		if (ret)
			goto fail;
		ret = radar_speaker_afe_update_bits(AFE_DAC_CON0,
						    AFE_DL1_ENABLE,
						    AFE_DL1_ENABLE);
		if (ret)
			goto fail;
		spin_lock_irqsave(&radar_pcm.irq_lock, flags);
		radar_pcm.running = true;
		spin_unlock_irqrestore(&radar_pcm.irq_lock, flags);
		ret = radar_speaker_afe_update_bits(AFE_IRQ_MCU_CON,
						    AFE_IRQ1_ENABLE,
						    AFE_IRQ1_ENABLE);
		if (ret)
			goto fail;
		ret = radar_speaker_afe_update_bits(AFE_DAC_CON0,
						    AFE_GLOBAL_ENABLE,
						    AFE_GLOBAL_ENABLE);
		if (ret)
			goto fail;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return radar_speaker_pcm_stop_locked();
	default:
		return -EINVAL;
	}

fail:
	(void)radar_speaker_pcm_stop_locked();
	return ret;
}

static snd_pcm_uframes_t radar_speaker_pcm_pointer(
		struct snd_pcm_substream *substream)
{
	u32 hardware_pointer;
	u32 base = (u32)radar_pcm.sram_phys;
	u32 bytes = (u32)substream->runtime->dma_bytes;

	if (radar_speaker_afe_read(AFE_DL1_CUR, &hardware_pointer) ||
	    hardware_pointer < base || hardware_pointer >= base + bytes)
		return 0;
	return bytes_to_frames(substream->runtime, hardware_pointer - base);
}

static int radar_speaker_pcm_copy(struct snd_pcm_substream *substream,
				  int channel, snd_pcm_uframes_t pos,
				  void __user *source,
				  snd_pcm_uframes_t count)
{
	size_t offset = frames_to_bytes(substream->runtime, pos);
	size_t bytes = frames_to_bytes(substream->runtime, count);
	size_t first;
	void *buffer;

	(void)channel;
	if (!bytes || offset >= substream->runtime->dma_bytes ||
	    bytes > substream->runtime->dma_bytes)
		return -EINVAL;
	buffer = memdup_user(source, bytes);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	first = min(bytes, substream->runtime->dma_bytes - offset);
	memcpy_toio(radar_pcm.sram + offset, buffer, first);
	if (first < bytes)
		memcpy_toio(radar_pcm.sram, buffer + first, bytes - first);
	kfree(buffer);
	return 0;
}

static int radar_speaker_pcm_silence(struct snd_pcm_substream *substream,
				     int channel, snd_pcm_uframes_t pos,
				     snd_pcm_uframes_t count)
{
	size_t offset = frames_to_bytes(substream->runtime, pos);
	size_t bytes = frames_to_bytes(substream->runtime, count);
	size_t first;

	(void)channel;
	if (offset >= substream->runtime->dma_bytes ||
	    bytes > substream->runtime->dma_bytes)
		return -EINVAL;
	first = min(bytes, substream->runtime->dma_bytes - offset);
	memset_io(radar_pcm.sram + offset, 0, first);
	if (first < bytes)
		memset_io(radar_pcm.sram, 0, bytes - first);
	return 0;
}

static const struct snd_pcm_ops radar_speaker_pcm_ops = {
	.open = radar_speaker_pcm_open,
	.close = radar_speaker_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = radar_speaker_pcm_hw_params,
	.prepare = radar_speaker_pcm_prepare,
	.trigger = radar_speaker_pcm_trigger,
	.pointer = radar_speaker_pcm_pointer,
	.copy = radar_speaker_pcm_copy,
	.silence = radar_speaker_pcm_silence,
};

static struct snd_soc_platform_driver radar_speaker_platform = {
	.ops = &radar_speaker_pcm_ops,
};

static int radar_speaker_pcm_probe(struct platform_device *pdev)
{
	struct resource *sram_resource;
	int ret;

	memset(&radar_pcm, 0, sizeof(radar_pcm));
	mutex_init(&radar_pcm.stream_lock);
	spin_lock_init(&radar_pcm.irq_lock);
	radar_pcm.dev = &pdev->dev;

	ret = radar_speaker_afe_init(&pdev->dev);
	if (ret)
		return ret;
	sram_resource = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!sram_resource) {
		ret = -ENODEV;
		goto deinit_afe;
	}
	if (resource_size(sram_resource) < RADAR_SPEAKER_ALIGNMENT) {
		ret = -ENOSPC;
		goto deinit_afe;
	}
	radar_pcm.sram_size = min_t(size_t, resource_size(sram_resource),
				      RADAR_SPEAKER_BUFFER_MAX);
	if ((u64)sram_resource->start >
	    RADAR_SPEAKER_DMA_MAX - (radar_pcm.sram_size - 1U)) {
		ret = -EOVERFLOW;
		goto deinit_afe;
	}
	radar_pcm.sram = devm_ioremap_resource(&pdev->dev, sram_resource);
	if (IS_ERR(radar_pcm.sram)) {
		ret = PTR_ERR(radar_pcm.sram);
		radar_pcm.sram = NULL;
		goto deinit_afe;
	}
	radar_pcm.sram_phys = sram_resource->start;

	pm_runtime_enable(&pdev->dev);
	ret = radar_speaker_clock_init(&pdev->dev);
	if (ret)
		goto disable_runtime;
	ret = radar_speaker_gpio_init(&pdev->dev);
	if (ret)
		goto deinit_clock;
	ret = radar_speaker_mclk_init(&pdev->dev);
	if (ret)
		goto deinit_gpio;

	radar_pcm.irq = platform_get_irq(pdev, 0);
	if (radar_pcm.irq < 0) {
		ret = radar_pcm.irq;
		goto deinit_mclk;
	}
	ret = devm_request_irq(&pdev->dev, radar_pcm.irq,
			       radar_speaker_pcm_irq, 0,
			       "radar-speaker-afe", &radar_pcm);
	if (ret)
		goto deinit_mclk;

	dev_set_name(&pdev->dev, RADAR_SPEAKER_PCM_NAME);
	ret = snd_soc_register_platform(&pdev->dev, &radar_speaker_platform);
	if (ret)
		goto free_irq;
	radar_pcm.platform_registered = true;
	platform_set_drvdata(pdev, &radar_pcm);
	return 0;

free_irq:
	devm_free_irq(&pdev->dev, radar_pcm.irq, &radar_pcm);
deinit_mclk:
	radar_speaker_mclk_deinit();
deinit_gpio:
	radar_speaker_gpio_deinit();
deinit_clock:
	radar_speaker_clock_deinit();
disable_runtime:
	pm_runtime_disable(&pdev->dev);
deinit_afe:
	radar_speaker_afe_deinit(&pdev->dev);
	return ret;
}

static int radar_speaker_pcm_remove(struct platform_device *pdev)
{
	unsigned long flags;
	int ret = 0;

	/* Stop the hardware and drain any in-flight handler before unregister. */
	mutex_lock(&radar_pcm.stream_lock);
	if (radar_pcm.substream)
		ret = radar_speaker_pcm_stop_locked();
	else {
		spin_lock_irqsave(&radar_pcm.irq_lock, flags);
		radar_pcm.running = false;
		spin_unlock_irqrestore(&radar_pcm.irq_lock, flags);
	}
	mutex_unlock(&radar_pcm.stream_lock);
	devm_free_irq(&pdev->dev, radar_pcm.irq, &radar_pcm);

	if (radar_pcm.platform_registered) {
		snd_soc_unregister_platform(&pdev->dev);
		radar_pcm.platform_registered = false;
	}
	/* ALSA close may have run during unregister; no handler may retain it. */
	spin_lock_irqsave(&radar_pcm.irq_lock, flags);
	radar_pcm.running = false;
	radar_pcm.substream = NULL;
	spin_unlock_irqrestore(&radar_pcm.irq_lock, flags);
	if (ret)
		dev_err(&pdev->dev, "failed to stop speaker PCM during remove: %d\n",
			ret);

	(void)radar_speaker_gpio_safe();
	radar_speaker_mclk_deinit();
	radar_speaker_gpio_deinit();
	radar_speaker_clock_deinit();
	pm_runtime_disable(&pdev->dev);
	radar_speaker_afe_deinit(&pdev->dev);
	memset(&radar_pcm, 0, sizeof(radar_pcm));
	return 0;
}

static const struct of_device_id radar_speaker_pcm_of_match[] = {
	{ .compatible = "mediatek,mt8163-soc-pcm-dl1" },
	{ }
};
MODULE_DEVICE_TABLE(of, radar_speaker_pcm_of_match);

static struct platform_driver radar_speaker_pcm_driver = {
	.probe = radar_speaker_pcm_probe,
	.remove = radar_speaker_pcm_remove,
	.driver = {
		.name = RADAR_SPEAKER_PCM_NAME,
		.owner = THIS_MODULE,
		.of_match_table = radar_speaker_pcm_of_match,
	},
};
module_platform_driver(radar_speaker_pcm_driver);

MODULE_DESCRIPTION("MT8163 fixed-function speaker PCM");
MODULE_LICENSE("GPL v2");
