// SPDX-License-Identifier: GPL-2.0
/* Fixed 9.6 MHz codec MCLK sourced from the MT8163 camera clock block. */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#ifdef CONFIG_MTK_SMI_VARIANT
#include "mt_smi.h"
#endif

#include "mt_amzn_mclk_speaker.h"

#define RADAR_SPEAKER_MCLK_HZ 9600000ULL
#define RADAR_SPEAKER_PLL_HZ 48000000ULL
#define RADAR_SPEAKER_LARB 2

/* Offsets inside camera1@15008000, the narrow owner of the SENINF block. */
#define SENINF_TOP_CTRL 0x0000U
#define SENINF1_CTRL 0x0100U
#define SENINF1_MUX_CTRL 0x0120U
#define SENINF_TG1_PH_CNT 0x0200U
#define SENINF_TG1_SEN_CK 0x0204U

#define SENINF1_EN_MASK (1U << 0)
#define SENINF1_MUX_EN_MASK (1U << 31)
#define SENINF12_PCLK_MASK 0x0c00U
#define SENINF12_PCLK_VALUE 0x0300U
#define SENINF_CLKFL_MASK 0x00003fU
#define SENINF_CLKRS_MASK 0x003f00U
#define SENINF_CLKCNT_MASK 0x3f0000U
#define SENINF_TGCLK_SEL_MASK 0x00000003U
#define SENINF_CLKFL_POL_MASK 0x00000004U
#define SENINF_PADCLK_INV_MASK (1U << 6)
#define SENINF_CLK_POL_MASK (1U << 28)
#define SENINF_PCEN_MASK (1U << 31)
#define SENINF_TG1_OUTPUT_ENABLE (1U << 29)

struct radar_speaker_mclk_state {
	struct platform_device *isp_pdev;
	struct platform_device *camera_pdev;
	void __iomem *camera_base;
	struct clk *sen_tg;
	struct clk *sen_cam;
	struct clk *camtg;
	struct clk *univpll_d26;
	struct clk *saved_camtg_parent;
	unsigned int users;
	bool initialized;
	bool isp_powered;
	bool sen_tg_enabled;
	bool sen_cam_enabled;
	bool camtg_parent_changed;
	bool camtg_enabled;
	bool output_enabled;
};

static DEFINE_MUTEX(radar_speaker_mclk_lock);
static struct radar_speaker_mclk_state radar_mclk;

static struct platform_device *radar_find_device(const char *compatible)
{
	struct platform_device *pdev;
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		return NULL;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	return pdev;
}

static void radar_update_bits(u32 offset, u32 mask, u32 value)
{
	u32 register_value = readl(radar_mclk.camera_base + offset);

	writel((register_value & ~mask) | (value & mask),
	       radar_mclk.camera_base + offset);
}

static int radar_speaker_mclk_program(u64 frequency)
{
	u32 divider;
	u32 falling_edge;
	u32 falling_polarity;

	if (frequency != RADAR_SPEAKER_MCLK_HZ ||
	    RADAR_SPEAKER_PLL_HZ % frequency)
		return -EINVAL;

	divider = (u32)(RADAR_SPEAKER_PLL_HZ / frequency) - 1U;
	falling_polarity = !(divider & 1U);
	falling_edge = divider > 1U ? (divider + 1U) >> 1 : 1U;

	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_PCEN_MASK,
			  SENINF_PCEN_MASK);
	radar_update_bits(SENINF_TOP_CTRL, SENINF12_PCLK_MASK,
			  SENINF12_PCLK_VALUE);
	radar_update_bits(SENINF_TG1_SEN_CK, SENINF_CLKFL_MASK,
			  falling_edge);
	radar_update_bits(SENINF_TG1_SEN_CK, SENINF_CLKRS_MASK, 0);
	radar_update_bits(SENINF_TG1_SEN_CK, SENINF_CLKCNT_MASK,
			  divider << 16);
	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_TGCLK_SEL_MASK, 1U);
	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_CLKFL_POL_MASK,
			  falling_polarity << 2);
	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_PADCLK_INV_MASK, 0);
	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_CLK_POL_MASK, 0);
	radar_update_bits(SENINF1_MUX_CTRL, SENINF1_MUX_EN_MASK,
			  SENINF1_MUX_EN_MASK);
	radar_update_bits(SENINF1_CTRL, SENINF1_EN_MASK,
			  SENINF1_EN_MASK);
	return 0;
}

static void radar_speaker_mclk_release_locked(void)
{
	int ret;

	if (radar_mclk.camtg_parent_changed &&
	    radar_mclk.saved_camtg_parent && radar_mclk.camtg) {
		ret = clk_set_parent(radar_mclk.camtg, radar_mclk.saved_camtg_parent);
		if (ret && radar_mclk.camera_pdev)
			dev_err(&radar_mclk.camera_pdev->dev,
				"cannot restore CAMTG parent during release: %d\n",
				ret);
	}
	if (radar_mclk.camera_base)
		iounmap(radar_mclk.camera_base);
	if (radar_mclk.univpll_d26)
		clk_put(radar_mclk.univpll_d26);
	if (radar_mclk.camtg)
		clk_put(radar_mclk.camtg);
	if (radar_mclk.sen_cam)
		clk_put(radar_mclk.sen_cam);
	if (radar_mclk.sen_tg)
		clk_put(radar_mclk.sen_tg);
	if (radar_mclk.camera_pdev)
		put_device(&radar_mclk.camera_pdev->dev);
	if (radar_mclk.isp_pdev)
		put_device(&radar_mclk.isp_pdev->dev);
	memset(&radar_mclk, 0, sizeof(radar_mclk));
}

int radar_speaker_mclk_init(struct device *dev)
{
	int ret = 0;

	if (!dev)
		return -EINVAL;
	mutex_lock(&radar_speaker_mclk_lock);
	if (radar_mclk.initialized) {
		ret = -EBUSY;
		goto out;
	}

	radar_mclk.isp_pdev = radar_find_device("mediatek,mt8163-ispsys");
	if (!radar_mclk.isp_pdev) {
		ret = -EPROBE_DEFER;
		goto fail;
	}
	radar_mclk.camera_pdev = radar_find_device("mediatek,mt8163-camera_hw");
	if (!radar_mclk.camera_pdev) {
		ret = -EPROBE_DEFER;
		goto fail;
	}
	/* All touched registers are inside camera1 reg[0] (0x15008000). */
	radar_mclk.camera_base =
		of_iomap(radar_mclk.camera_pdev->dev.of_node, 0);
	if (!radar_mclk.camera_base) {
		ret = -ENOMEM;
		goto fail;
	}

	radar_mclk.sen_tg = clk_get(&radar_mclk.isp_pdev->dev, "IMG_SEN_TG");
	if (IS_ERR(radar_mclk.sen_tg)) {
		ret = PTR_ERR(radar_mclk.sen_tg);
		radar_mclk.sen_tg = NULL;
		goto fail;
	}
	radar_mclk.sen_cam = clk_get(&radar_mclk.isp_pdev->dev,
				     "IMG_SEN_CAM");
	if (IS_ERR(radar_mclk.sen_cam)) {
		ret = PTR_ERR(radar_mclk.sen_cam);
		radar_mclk.sen_cam = NULL;
		goto fail;
	}
	radar_mclk.camtg = clk_get(&radar_mclk.camera_pdev->dev,
				   "TOP_CAMTG_SEL");
	if (IS_ERR(radar_mclk.camtg)) {
		ret = PTR_ERR(radar_mclk.camtg);
		radar_mclk.camtg = NULL;
		goto fail;
	}
	radar_mclk.univpll_d26 = clk_get(&radar_mclk.camera_pdev->dev,
					 "TOP_UNIVPLL_D26");
	if (IS_ERR(radar_mclk.univpll_d26)) {
		ret = PTR_ERR(radar_mclk.univpll_d26);
		radar_mclk.univpll_d26 = NULL;
		goto fail;
	}
	radar_mclk.initialized = true;
	goto out;

fail:
	dev_err(dev, "cannot initialize fixed speaker MCLK: %d\n", ret);
	radar_speaker_mclk_release_locked();
out:
	mutex_unlock(&radar_speaker_mclk_lock);
	return ret;
}

static int radar_speaker_isp_power_locked(void)
{
	int ret;

	if (radar_mclk.isp_powered)
		return 0;
#ifdef CONFIG_MTK_SMI_VARIANT
	ret = mtk_smi_larb_clock_on(RADAR_SPEAKER_LARB, true);
#else
	ret = pm_runtime_get_sync(&radar_mclk.isp_pdev->dev);
	if (ret < 0)
		pm_runtime_put_noidle(&radar_mclk.isp_pdev->dev);
#endif
	if (ret >= 0)
		radar_mclk.isp_powered = true;
	return ret < 0 ? ret : 0;
}

static int radar_speaker_isp_unpower_locked(void)
{
	int ret;

	if (!radar_mclk.isp_powered)
		return 0;
#ifdef CONFIG_MTK_SMI_VARIANT
	mtk_smi_larb_clock_off(RADAR_SPEAKER_LARB, true);
	ret = 0;
#else
	ret = pm_runtime_put_sync(&radar_mclk.isp_pdev->dev);
#endif
	if (ret >= 0)
		radar_mclk.isp_powered = false;
	return ret < 0 ? ret : 0;
}

static int radar_speaker_mclk_disable_locked(void)
{
	int first = 0;
	int ret;

	if (radar_mclk.output_enabled) {
		radar_update_bits(SENINF_TG1_PH_CNT,
				  SENINF_TG1_OUTPUT_ENABLE, 0);
		radar_mclk.output_enabled = false;
	}
	if (radar_mclk.camtg_enabled) {
		clk_disable_unprepare(radar_mclk.camtg);
		radar_mclk.camtg_enabled = false;
	}
	if (radar_mclk.camtg_parent_changed) {
		ret = clk_set_parent(radar_mclk.camtg, radar_mclk.saved_camtg_parent);
		if (ret)
			first = ret;
		else
			radar_mclk.camtg_parent_changed = false;
	}
	if (radar_mclk.sen_cam_enabled) {
		clk_disable_unprepare(radar_mclk.sen_cam);
		radar_mclk.sen_cam_enabled = false;
	}
	if (radar_mclk.sen_tg_enabled) {
		clk_disable_unprepare(radar_mclk.sen_tg);
		radar_mclk.sen_tg_enabled = false;
	}
	if (radar_mclk.isp_powered) {
		ret = radar_speaker_isp_unpower_locked();
		if (ret && !first)
			first = ret;
	}
	radar_mclk.users = 0;
	return first;
}

int radar_speaker_mclk_enable(u64 frequency)
{
	int ret;

	mutex_lock(&radar_speaker_mclk_lock);
	if (!radar_mclk.initialized) {
		ret = -ENODEV;
		goto out;
	}
	if (frequency != RADAR_SPEAKER_MCLK_HZ) {
		ret = -EINVAL;
		goto out;
	}
	if (radar_mclk.users) {
		radar_mclk.users++;
		ret = 0;
		goto out;
	}

	ret = radar_speaker_isp_power_locked();
	if (ret)
		goto out;
	ret = clk_prepare_enable(radar_mclk.sen_tg);
	if (ret)
		goto fail;
	radar_mclk.sen_tg_enabled = true;
	ret = clk_prepare_enable(radar_mclk.sen_cam);
	if (ret)
		goto fail;
	radar_mclk.sen_cam_enabled = true;

	radar_mclk.saved_camtg_parent = clk_get_parent(radar_mclk.camtg);
	if (IS_ERR_OR_NULL(radar_mclk.saved_camtg_parent)) {
		ret = radar_mclk.saved_camtg_parent ?
			PTR_ERR(radar_mclk.saved_camtg_parent) : -ENODEV;
		radar_mclk.saved_camtg_parent = NULL;
		goto fail;
	}
	if (radar_mclk.saved_camtg_parent != radar_mclk.univpll_d26) {
		ret = clk_set_parent(radar_mclk.camtg, radar_mclk.univpll_d26);
		if (ret)
			goto fail;
		radar_mclk.camtg_parent_changed = true;
	}
	ret = clk_prepare_enable(radar_mclk.camtg);
	if (ret)
		goto fail;
	radar_mclk.camtg_enabled = true;
	ret = radar_speaker_mclk_program(frequency);
	if (ret)
		goto fail;
	radar_update_bits(SENINF_TG1_PH_CNT, SENINF_TG1_OUTPUT_ENABLE,
			  SENINF_TG1_OUTPUT_ENABLE);
	radar_mclk.output_enabled = true;
	radar_mclk.users++;
	ret = 0;
	goto out;

fail:
	(void)radar_speaker_mclk_disable_locked();
out:
	mutex_unlock(&radar_speaker_mclk_lock);
	return ret;
}

int radar_speaker_mclk_disable(void)
{
	int ret = 0;

	mutex_lock(&radar_speaker_mclk_lock);
	if (!radar_mclk.initialized) {
		ret = -ENODEV;
		goto out;
	}
	if (!radar_mclk.users)
		goto out;
	radar_mclk.users--;
	if (!radar_mclk.users)
		ret = radar_speaker_mclk_disable_locked();
out:
	mutex_unlock(&radar_speaker_mclk_lock);
	return ret;
}

void radar_speaker_mclk_deinit(void)
{
	int ret;

	mutex_lock(&radar_speaker_mclk_lock);
	if (radar_mclk.initialized) {
		ret = radar_speaker_mclk_disable_locked();
		if (ret && radar_mclk.camera_pdev)
			dev_err(&radar_mclk.camera_pdev->dev,
				"cannot fully disable speaker MCLK: %d\n", ret);
	}
	radar_speaker_mclk_release_locked();
	mutex_unlock(&radar_speaker_mclk_lock);
}
