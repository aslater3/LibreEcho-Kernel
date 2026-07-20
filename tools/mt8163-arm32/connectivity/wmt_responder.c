/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal MT8163 WMT command responder.
 *
 * The conn_soc driver publishes a command through poll/read and waits for a
 * userspace response written to the same device.  This program does only that
 * protocol.  It has no function-activation ioctl or option.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_WMT_DEVICE "/dev/stpwmt"
#define WMT_COMMAND_SIZE 256U

/* _IOW(0xa0, 13, int), pinned for the 32-bit WMT userspace ABI. */
#define WMT_IOCTL_SET_LAUNCHER_KILL 0x4004a00dUL

_Static_assert(sizeof(void *) == 4, "wmt_responder must be built for ARM32");
_Static_assert(sizeof(unsigned long) == 4,
	       "wmt_responder requires a 32-bit ioctl ABI");

struct options {
	const char *device;
	int ok;
	int once;
};

static void usage(const char *program)
{
	printf("Usage: %s [--device PATH] [--ok] [--once]\n", program);
	printf("\n");
	printf("Respond to WMT poll/read commands without activating a function.\n");
	printf("\n");
	printf("  -d, --device PATH  WMT device (default: %s)\n",
	       DEFAULT_WMT_DEVICE);
	printf("      --ok           explicitly write the success token 'ok'\n");
	printf("      --once         exit after one command/response transaction\n");
	printf("  -h, --help         show this help\n");
	printf("\n");
	printf("Without --ok the fail-closed response is 'fail'.\n");
}

static int parse_options(int argc, char **argv, struct options *options)
{
	int device_seen = 0;
	int ok_seen = 0;
	int once_seen = 0;
	int i;

	options->device = DEFAULT_WMT_DEVICE;
	options->ok = 0;
	options->once = 0;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			if (argc != 2) {
				fprintf(stderr, "--help cannot be combined with other options\n");
				return -1;
			}
			usage(argv[0]);
			return 1;
		}
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) {
			if (device_seen || i + 1 >= argc || argv[i + 1][0] != '/') {
				fprintf(stderr,
					"--device requires one absolute path\n");
				return -1;
			}
			options->device = argv[++i];
			device_seen = 1;
			continue;
		}
		if (!strcmp(argv[i], "--ok")) {
			if (ok_seen) {
				fprintf(stderr, "--ok may be supplied only once\n");
				return -1;
			}
			options->ok = 1;
			ok_seen = 1;
			continue;
		}
		if (!strcmp(argv[i], "--once")) {
			if (once_seen) {
				fprintf(stderr, "--once may be supplied only once\n");
				return -1;
			}
			options->once = 1;
			once_seen = 1;
			continue;
		}
		fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
		return -1;
	}

	return 0;
}

static int write_response(int fd, int ok)
{
	const char *response = ok ? "ok" : "fail";
	const size_t response_size = ok ? 2U : 4U;
	ssize_t written;

	written = write(fd, response, response_size);
	if (written < 0) {
		fprintf(stderr, "write response: %s\n", strerror(errno));
		return -1;
	}
	if ((size_t)written != response_size) {
		fprintf(stderr, "short response write: %zd of %zu bytes\n",
			written, response_size);
		return -1;
	}

	printf("response: %s%s\n", response, ok ? " (explicit)" : " (fail-closed)");
	fflush(stdout);
	return 0;
}

static int handle_command(int fd, const struct options *options)
{
	char command[WMT_COMMAND_SIZE];
	ssize_t command_size;

	do {
		command_size = read(fd, command, sizeof(command) - 1U);
	} while (command_size < 0 && errno == EINTR);

	if (command_size < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 1;
		fprintf(stderr, "read command: %s\n", strerror(errno));
		return -1;
	}
	if (command_size == 0) {
		fprintf(stderr, "WMT device returned EOF while readable\n");
		return -1;
	}

	/* WMT_read() returns bytes without appending a NUL terminator. */
	command[command_size] = '\0';
	printf("command: %s\n", command);
	fflush(stdout);

	return write_response(fd, options->ok);
}

int main(int argc, char **argv)
{
	struct options options;
	struct pollfd descriptor;
	int parse_result;
	int fd;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0)
		return parse_result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	fd = open(options.device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", options.device, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Arg zero tells the driver that a launcher is present, not killed. */
	if (ioctl(fd, WMT_IOCTL_SET_LAUNCHER_KILL, 0UL) < 0) {
		fprintf(stderr, "SET_LAUNCHER_KILL(0): %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	printf("wmt_responder device=%s success=%s once=%s\n", options.device,
	       options.ok ? "explicitly-enabled" : "disabled",
	       options.once ? "yes" : "no");
	fflush(stdout);

	descriptor.fd = fd;
	descriptor.events = POLLIN;
	descriptor.revents = 0;

	for (;;) {
		int poll_result;
		int command_result;

		do {
			poll_result = poll(&descriptor, 1, -1);
		} while (poll_result < 0 && errno == EINTR);

		if (poll_result < 0) {
			fprintf(stderr, "poll %s: %s\n", options.device,
				strerror(errno));
			close(fd);
			return EXIT_FAILURE;
		}
		if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "poll %s: fatal revents=0x%x\n",
				options.device, descriptor.revents);
			close(fd);
			return EXIT_FAILURE;
		}
		if (!(descriptor.revents & POLLIN))
			continue;

		command_result = handle_command(fd, &options);
		if (command_result < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
		if (command_result > 0)
			continue;
		if (options.once)
			break;
	}

	close(fd);
	return EXIT_SUCCESS;
}
