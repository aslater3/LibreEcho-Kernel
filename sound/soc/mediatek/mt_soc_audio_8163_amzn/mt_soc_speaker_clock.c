// SPDX-License-Identifier: GPL-2.0
/* Explicit playback-only clock provider for MT8163 DL1/I2S. */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>

#include "mt_soc_speaker_afe.h"
#include "mt_soc_speaker_clock.h"

#define RADAR_AFE_AUDIO_TOP_CON0 0x0000U
#define RADAR_AFE_BUS_INIT (1U << 14)
#define RADAR_AFE_GATE_MASK ((1U << 2) | (1U << 25) | (1U << 26))

struct radar_speaker_clock_state {
	struct device *dev;
	struct clk *infra;
	struct clk *audio_mux;
	struct clk *audio_intbus_mux;
	bool initialized;
	bool enabled;
};

static DEFINE_MUTEX(radar_speaker_clock_lock);
static struct radar_speaker_clock_state radar_speaker_clocks;

int radar_speaker_clock_init(struct device *dev)
{
	struct clk *infra;
	struct clk *audio_mux;
	struct clk *audio_intbus_mux;

	if (!dev)
		return -EINVAL;
	infra = devm_clk_get(dev, "aud_infra_clk");
	if (IS_ERR(infra))
		return PTR_ERR(infra);
	audio_mux = devm_clk_get(dev, "top_mux_audio");
	if (IS_ERR(audio_mux))
		return PTR_ERR(audio_mux);
	audio_intbus_mux = devm_clk_get(dev, "top_mux_audio_intbus");
	if (IS_ERR(audio_intbus_mux))
		return PTR_ERR(audio_intbus_mux);

	mutex_lock(&radar_speaker_clock_lock);
	if (radar_speaker_clocks.initialized) {
		mutex_unlock(&radar_speaker_clock_lock);
		return -EBUSY;
	}
	radar_speaker_clocks.dev = dev;
	radar_speaker_clocks.infra = infra;
	radar_speaker_clocks.audio_mux = audio_mux;
	radar_speaker_clocks.audio_intbus_mux = audio_intbus_mux;
	radar_speaker_clocks.initialized = true;
	mutex_unlock(&radar_speaker_clock_lock);
	return 0;
}

int radar_speaker_clock_enable(void)
{
	int ret;

	mutex_lock(&radar_speaker_clock_lock);
	if (!radar_speaker_clocks.initialized) {
		ret = -ENODEV;
		goto out;
	}
	if (radar_speaker_clocks.enabled) {
		ret = 0;
		goto out;
	}

	ret = pm_runtime_get_sync(radar_speaker_clocks.dev);
	if (ret < 0) {
		pm_runtime_put_noidle(radar_speaker_clocks.dev);
		goto out;
	}
	ret = clk_prepare_enable(radar_speaker_clocks.infra);
	if (ret)
		goto err_runtime;
	ret = clk_prepare_enable(radar_speaker_clocks.audio_intbus_mux);
	if (ret)
		goto err_infra;
	ret = clk_prepare_enable(radar_speaker_clocks.audio_mux);
	if (ret)
		goto err_intbus;
	ret = radar_speaker_afe_update_bits(RADAR_AFE_AUDIO_TOP_CON0,
					    RADAR_AFE_BUS_INIT,
					    RADAR_AFE_BUS_INIT);
	if (ret)
		goto err_mux;
	ret = radar_speaker_afe_update_bits(RADAR_AFE_AUDIO_TOP_CON0,
					    RADAR_AFE_GATE_MASK, 0);
	if (ret)
		goto err_mux;

	radar_speaker_clocks.enabled = true;
	ret = 0;
	goto out;

err_mux:
	clk_disable_unprepare(radar_speaker_clocks.audio_mux);
err_intbus:
	clk_disable_unprepare(radar_speaker_clocks.audio_intbus_mux);
err_infra:
	clk_disable_unprepare(radar_speaker_clocks.infra);
err_runtime:
	pm_runtime_put_sync(radar_speaker_clocks.dev);
out:
	mutex_unlock(&radar_speaker_clock_lock);
	return ret;
}

int radar_speaker_clock_disable(void)
{
	int first = 0;
	int ret;

	mutex_lock(&radar_speaker_clock_lock);
	if (!radar_speaker_clocks.initialized) {
		first = -ENODEV;
		goto out;
	}
	if (!radar_speaker_clocks.enabled)
		goto out;

	ret = radar_speaker_afe_update_bits(RADAR_AFE_AUDIO_TOP_CON0,
					    RADAR_AFE_GATE_MASK,
					    RADAR_AFE_GATE_MASK);
	if (ret)
		first = ret;
	clk_disable_unprepare(radar_speaker_clocks.audio_mux);
	clk_disable_unprepare(radar_speaker_clocks.audio_intbus_mux);
	clk_disable_unprepare(radar_speaker_clocks.infra);
	ret = pm_runtime_put_sync(radar_speaker_clocks.dev);
	if (ret < 0 && !first)
		first = ret;
	radar_speaker_clocks.enabled = false;
out:
	mutex_unlock(&radar_speaker_clock_lock);
	return first;
}

void radar_speaker_clock_deinit(void)
{
	(void)radar_speaker_clock_disable();
	mutex_lock(&radar_speaker_clock_lock);
	radar_speaker_clocks.dev = NULL;
	radar_speaker_clocks.infra = NULL;
	radar_speaker_clocks.audio_mux = NULL;
	radar_speaker_clocks.audio_intbus_mux = NULL;
	radar_speaker_clocks.initialized = false;
	mutex_unlock(&radar_speaker_clock_lock);
}
