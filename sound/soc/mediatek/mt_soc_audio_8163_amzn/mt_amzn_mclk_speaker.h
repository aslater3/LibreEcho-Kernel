/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT_AMZN_MCLK_SPEAKER_H__
#define __MT_AMZN_MCLK_SPEAKER_H__

#include <linux/types.h>

struct device;

int radar_speaker_mclk_init(struct device *dev);
int radar_speaker_mclk_enable(u64 frequency);
int radar_speaker_mclk_disable(void);
void radar_speaker_mclk_deinit(void);

#endif /* __MT_AMZN_MCLK_SPEAKER_H__ */
