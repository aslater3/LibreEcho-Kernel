#ifndef LIBREECHO_PLAYBACK_STATUS_H
#define LIBREECHO_PLAYBACK_STATUS_H

#include <limits.h>

#define PLAYBACK_BUS_MEDIA (1U << 0)
#define PLAYBACK_BUS_SYSTEM (1U << 1)
#define PLAYBACK_BUS_ANNOUNCEMENT (1U << 2)
#define PLAYBACK_BUS_ALARM (1U << 3)

struct playback_status {
	char path[PATH_MAX];
	char temporary_path[PATH_MAX];
	unsigned int last_mask;
	int published;
};

int playback_status_init(struct playback_status *status, const char *root);
int playback_status_publish(struct playback_status *status,
			    unsigned int bus_mask);

#endif
