/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MT8163_SPEAKER_AFE_H_
#define _MT8163_SPEAKER_AFE_H_

#include <linux/types.h>

struct device;
struct snd_pcm_substream;

int radar_speaker_afe_init(struct device *dev);
void radar_speaker_afe_deinit(struct device *dev);
int radar_speaker_afe_read(u32 offset, u32 *value);
int radar_speaker_afe_write(u32 offset, u32 value);
int radar_speaker_afe_update_bits(u32 offset, u32 mask, u32 value);

#endif
