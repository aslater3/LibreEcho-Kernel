/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MT8163_SPEAKER_CLOCK_H_
#define _MT8163_SPEAKER_CLOCK_H_

struct device;

int radar_speaker_clock_init(struct device *dev);
void radar_speaker_clock_deinit(void);
int radar_speaker_clock_enable(void);
int radar_speaker_clock_disable(void);

#endif
