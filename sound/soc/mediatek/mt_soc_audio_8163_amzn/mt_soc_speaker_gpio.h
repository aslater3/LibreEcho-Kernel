/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MT8163_SPEAKER_GPIO_H_
#define _MT8163_SPEAKER_GPIO_H_

struct device;

int radar_speaker_gpio_init(struct device *dev);
void radar_speaker_gpio_deinit(void);
int radar_speaker_gpio_i2s(bool enable);
int radar_speaker_gpio_amp(bool enable);
int radar_speaker_gpio_dac_route(bool enable);
int radar_speaker_gpio_mclk(void);
int radar_speaker_gpio_safe(void);

#endif
