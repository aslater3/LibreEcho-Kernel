#define _GNU_SOURCE

#include "playback_status.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t length)
{
	size_t written = 0;

	while (written < length) {
		ssize_t result = write(fd, data + written, length - written);

		if (result > 0) {
			written += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

int playback_status_init(struct playback_status *status, const char *root)
{
	int length;

	if (!status || !root) {
		errno = EINVAL;
		return -1;
	}
	memset(status, 0, sizeof(*status));
	length = snprintf(status->path, sizeof(status->path),
			  "%s/status.json", root);
	if (length < 0 || (size_t)length >= sizeof(status->path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	length = snprintf(status->temporary_path,
			  sizeof(status->temporary_path),
			  "%s/.status.json.tmp", root);
	if (length < 0 ||
	    (size_t)length >= sizeof(status->temporary_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/*
 * Publish only bus-state transitions.  rename(2) makes every host reader see
 * either the previous complete JSON object or the next one; it can never see
 * a partially-written status file.
 */
int playback_status_publish(struct playback_status *status,
			    unsigned int bus_mask)
{
	const char *state;
	const char *active;
	char document[256];
	int fd;
	int length;
	int saved_errno;

	if (!status || status->path[0] == '\0') {
		errno = EINVAL;
		return -1;
	}
	if (status->published && status->last_mask == bus_mask)
		return 0;
	if (bus_mask & PLAYBACK_BUS_ALARM) {
		state = "alarm";
		active = "\"alarm\"";
	} else if (bus_mask & PLAYBACK_BUS_ANNOUNCEMENT) {
		state = "announcing";
		active = "\"announcement\"";
	} else if (bus_mask & PLAYBACK_BUS_SYSTEM) {
		state = "system";
		active = "\"system\"";
	} else if (bus_mask & PLAYBACK_BUS_MEDIA) {
		state = "playing";
		active = "\"media\"";
	} else {
		state = "idle";
		active = "null";
	}
	length = snprintf(document, sizeof(document),
		"{\"state\":\"%s\",\"active\":%s,\"buses\":{"
		"\"media\":%s,\"system\":%s,\"announcement\":%s,"
		"\"alarm\":%s}}\n",
		state, active,
		bus_mask & PLAYBACK_BUS_MEDIA ? "true" : "false",
		bus_mask & PLAYBACK_BUS_SYSTEM ? "true" : "false",
		bus_mask & PLAYBACK_BUS_ANNOUNCEMENT ? "true" : "false",
		bus_mask & PLAYBACK_BUS_ALARM ? "true" : "false");
	if (length < 0 || (size_t)length >= sizeof(document)) {
		errno = EOVERFLOW;
		return -1;
	}

	fd = open(status->temporary_path,
		  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
		  0644);
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0644) < 0 ||
	    write_all(fd, document, (size_t)length) < 0 ||
	    fsync(fd) < 0) {
		saved_errno = errno;
		(void)close(fd);
		(void)unlink(status->temporary_path);
		errno = saved_errno;
		return -1;
	}
	if (close(fd) < 0) {
		saved_errno = errno;
		(void)unlink(status->temporary_path);
		errno = saved_errno;
		return -1;
	}
	if (rename(status->temporary_path, status->path) < 0) {
		saved_errno = errno;
		(void)unlink(status->temporary_path);
		errno = saved_errno;
		return -1;
	}
	status->last_mask = bus_mask;
	status->published = 1;
	return 0;
}
