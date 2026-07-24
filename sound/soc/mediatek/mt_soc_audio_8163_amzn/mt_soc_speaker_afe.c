// SPDX-License-Identifier: GPL-2.0
/* Narrow MMIO provider shared only by the MT8163 speaker clock and PCM. */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include "mt_soc_speaker_afe.h"

#define RADAR_SPEAKER_POWER_TOP_OFFSET 0x029cU
#define RADAR_SPEAKER_POWER_TOP_VALUE 0x0000000dU

static DEFINE_SPINLOCK(radar_speaker_afe_lock);
static void __iomem *radar_speaker_afe_base;
static resource_size_t radar_speaker_afe_size;

int radar_speaker_afe_init(struct device *dev)
{
	struct platform_device *pdev;
	struct resource *resource;
	struct regmap *scpsys;
	void __iomem *base;
	int ret;

	if (!dev)
		return -EINVAL;
	pdev = to_platform_device(dev);
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -ENODEV;
	base = devm_ioremap_resource(dev, resource);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/*
	 * Preserve the stock MT8163 prerequisite that makes the audio
	 * aperture accessible.  The register lives in the SCPSYS syscon at
	 * physical 0x1000629c; using the syscon keeps ownership explicit and
	 * avoids an untracked raw mapping.
	 */
	scpsys = syscon_regmap_lookup_by_compatible("mediatek,mt8163-scpsys");
	if (IS_ERR(scpsys))
		return PTR_ERR(scpsys);
	ret = regmap_write(scpsys, RADAR_SPEAKER_POWER_TOP_OFFSET,
			   RADAR_SPEAKER_POWER_TOP_VALUE);
	if (ret)
		return ret;

	spin_lock(&radar_speaker_afe_lock);
	if (radar_speaker_afe_base) {
		spin_unlock(&radar_speaker_afe_lock);
		return -EBUSY;
	}
	radar_speaker_afe_base = base;
	radar_speaker_afe_size = resource_size(resource);
	spin_unlock(&radar_speaker_afe_lock);
	return 0;
}

void radar_speaker_afe_deinit(struct device *dev)
{
	unsigned long flags;

	(void)dev;
	spin_lock_irqsave(&radar_speaker_afe_lock, flags);
	radar_speaker_afe_base = NULL;
	radar_speaker_afe_size = 0;
	spin_unlock_irqrestore(&radar_speaker_afe_lock, flags);
}

static int radar_speaker_afe_validate(u32 offset)
{
	if (!radar_speaker_afe_base)
		return -ENODEV;
	if ((resource_size_t)offset + sizeof(u32) > radar_speaker_afe_size ||
	    (offset & (sizeof(u32) - 1U)))
		return -ERANGE;
	return 0;
}

int radar_speaker_afe_read(u32 offset, u32 *value)
{
	unsigned long flags;
	int ret;

	if (!value)
		return -EINVAL;
	spin_lock_irqsave(&radar_speaker_afe_lock, flags);
	ret = radar_speaker_afe_validate(offset);
	if (!ret)
		*value = readl(radar_speaker_afe_base + offset);
	spin_unlock_irqrestore(&radar_speaker_afe_lock, flags);
	return ret;
}

int radar_speaker_afe_write(u32 offset, u32 value)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&radar_speaker_afe_lock, flags);
	ret = radar_speaker_afe_validate(offset);
	if (!ret)
		writel(value, radar_speaker_afe_base + offset);
	spin_unlock_irqrestore(&radar_speaker_afe_lock, flags);
	return ret;
}

int radar_speaker_afe_update_bits(u32 offset, u32 mask, u32 value)
{
	unsigned long flags;
	u32 register_value;
	int ret;

	spin_lock_irqsave(&radar_speaker_afe_lock, flags);
	ret = radar_speaker_afe_validate(offset);
	if (!ret) {
		register_value = readl(radar_speaker_afe_base + offset);
		writel((register_value & ~mask) | (value & mask),
		       radar_speaker_afe_base + offset);
	}
	spin_unlock_irqrestore(&radar_speaker_afe_lock, flags);
	return ret;
}
