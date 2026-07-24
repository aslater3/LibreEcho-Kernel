// SPDX-License-Identifier: GPL-2.0
/* Narrow pinctrl provider for the radar_puffin speaker build. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>

#include "mt_soc_speaker_gpio.h"

enum radar_speaker_pin {
	RADAR_PIN_I2S_IDLE = 0,
	RADAR_PIN_I2S_ACTIVE,
	RADAR_PIN_AMP_ON,
	RADAR_PIN_AMP_OFF,
	RADAR_PIN_MCLK,
	RADAR_PIN_DAC_ON,
	RADAR_PIN_DAC_OFF,
	RADAR_PIN_COUNT,
};

static const char * const radar_speaker_pin_names[RADAR_PIN_COUNT] = {
	[RADAR_PIN_I2S_IDLE] = "audi2s1-mode0",
	[RADAR_PIN_I2S_ACTIVE] = "audi2s1-mode1",
	[RADAR_PIN_AMP_ON] = "extamp-pullhigh",
	[RADAR_PIN_AMP_OFF] = "extamp-pulllow",
	[RADAR_PIN_MCLK] = "cmmclk-mclk",
	[RADAR_PIN_DAC_ON] = "extamp-dacmux-pullhigh",
	[RADAR_PIN_DAC_OFF] = "extamp-dacmux-pulllow",
};

static DEFINE_MUTEX(radar_speaker_gpio_lock);
static struct pinctrl *radar_speaker_pinctrl;
static struct pinctrl_state *radar_speaker_pins[RADAR_PIN_COUNT];
static bool radar_speaker_gpio_ready;

static int radar_speaker_select_locked(enum radar_speaker_pin pin)
{
	if (!radar_speaker_gpio_ready || !radar_speaker_pinctrl ||
	    !radar_speaker_pins[pin])
		return -ENODEV;
	return pinctrl_select_state(radar_speaker_pinctrl,
				    radar_speaker_pins[pin]);
}

static int radar_speaker_safe_locked(void)
{
	int first = 0;
	int ret;

	ret = radar_speaker_select_locked(RADAR_PIN_AMP_OFF);
	if (ret && !first)
		first = ret;
	ret = radar_speaker_select_locked(RADAR_PIN_DAC_OFF);
	if (ret && !first)
		first = ret;
	ret = radar_speaker_select_locked(RADAR_PIN_I2S_IDLE);
	if (ret && !first)
		first = ret;
	return first;
}

int radar_speaker_gpio_init(struct device *dev)
{
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins[RADAR_PIN_COUNT];
	int first = 0;
	int safe_ret;
	int i;

	if (!dev)
		return -EINVAL;
	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return PTR_ERR(pinctrl);

	for (i = 0; i < RADAR_PIN_COUNT; i++) {
		pins[i] = pinctrl_lookup_state(pinctrl,
					      radar_speaker_pin_names[i]);
		if (IS_ERR(pins[i])) {
			if (!first)
				first = PTR_ERR(pins[i]);
			pins[i] = NULL;
		}
	}

	mutex_lock(&radar_speaker_gpio_lock);
	radar_speaker_pinctrl = pinctrl;
	for (i = 0; i < RADAR_PIN_COUNT; i++)
		radar_speaker_pins[i] = pins[i];
	/* Permit safe-state attempts for every state that was found. */
	radar_speaker_gpio_ready = true;
	safe_ret = radar_speaker_safe_locked();
	if (!first)
		first = safe_ret;
	if (first)
		radar_speaker_gpio_ready = false;
	mutex_unlock(&radar_speaker_gpio_lock);

	if (first)
		dev_err(dev, "speaker pinctrl is incomplete or unsafe: %d\n",
			first);
	return first;
}

void radar_speaker_gpio_deinit(void)
{
	int i;

	mutex_lock(&radar_speaker_gpio_lock);
	if (radar_speaker_gpio_ready)
		(void)radar_speaker_safe_locked();
	radar_speaker_gpio_ready = false;
	radar_speaker_pinctrl = NULL;
	for (i = 0; i < RADAR_PIN_COUNT; i++)
		radar_speaker_pins[i] = NULL;
	mutex_unlock(&radar_speaker_gpio_lock);
}

int radar_speaker_gpio_safe(void)
{
	int ret;

	mutex_lock(&radar_speaker_gpio_lock);
	ret = radar_speaker_safe_locked();
	mutex_unlock(&radar_speaker_gpio_lock);
	return ret;
}

static int radar_speaker_select(enum radar_speaker_pin pin)
{
	int ret;

	mutex_lock(&radar_speaker_gpio_lock);
	ret = radar_speaker_select_locked(pin);
	mutex_unlock(&radar_speaker_gpio_lock);
	return ret;
}

int radar_speaker_gpio_i2s(bool enable)
{
	return radar_speaker_select(enable ? RADAR_PIN_I2S_ACTIVE :
					 RADAR_PIN_I2S_IDLE);
}

int radar_speaker_gpio_amp(bool enable)
{
	return radar_speaker_select(enable ? RADAR_PIN_AMP_ON :
					 RADAR_PIN_AMP_OFF);
}

int radar_speaker_gpio_dac_route(bool enable)
{
	return radar_speaker_select(enable ? RADAR_PIN_DAC_ON :
					 RADAR_PIN_DAC_OFF);
}

int radar_speaker_gpio_mclk(void)
{
	return radar_speaker_select(RADAR_PIN_MCLK);
}
