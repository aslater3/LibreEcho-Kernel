/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deliberately narrow MT8163 BT-only gate.
 *
 * There is one activation operation in this file: one ioctl with the pinned
 * ARM32 request and BT argument.  There is no generic function selector,
 * retry loop, or Wi-Fi path.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_WMT_DEVICE "/dev/stpwmt"

/* Exact ARM32 conn_soc ABI: _IOW(0xa0, 6, int), BT type 0, on bit set. */
#define WMT_BT_ONLY_REQUEST  0x4004a006UL
#define WMT_BT_ONLY_ARGUMENT 0x80000000UL

_Static_assert(sizeof(void *) == 4, "wmt_bt_on must be built for ARM32");
_Static_assert(sizeof(unsigned long) == 4,
	       "wmt_bt_on requires a 32-bit ioctl ABI");

struct options {
	const char *device;
	int execute;
};

static void usage(const char *program)
{
	printf("Usage: %s --execute-bt-only-once [--device PATH]\n", program);
	printf("\n");
	printf("Issue exactly one pinned MT8163 BT-only ioctl, with no retry.\n");
	printf("\n");
	printf("      --execute-bt-only-once  required explicit execution gate\n");
	printf("  -d, --device PATH          WMT device (default: %s)\n",
	       DEFAULT_WMT_DEVICE);
	printf("  -h, --help                 show this help\n");
}

static int parse_options(int argc, char **argv, struct options *options)
{
	int device_seen = 0;
	int execute_seen = 0;
	int i;

	options->device = DEFAULT_WMT_DEVICE;
	options->execute = 0;

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
		if (!strcmp(argv[i], "--execute-bt-only-once")) {
			if (execute_seen) {
				fprintf(stderr,
					"--execute-bt-only-once may be supplied only once\n");
				return -1;
			}
			options->execute = 1;
			execute_seen = 1;
			continue;
		}
		fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
		return -1;
	}

	if (!options->execute) {
		fprintf(stderr,
			"refusing to issue BT ioctl without --execute-bt-only-once\n");
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct options options;
	int parse_result;
	int result;
	int fd;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0)
		return parse_result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	fd = open(options.device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", options.device, strerror(errno));
		return EXIT_FAILURE;
	}

	printf("bt_only_ioctl device=%s request=0x%08lx arg=0x%08lx calls=1 retries=0\n",
	       options.device, WMT_BT_ONLY_REQUEST, WMT_BT_ONLY_ARGUMENT);
	fflush(stdout);

	result = ioctl(fd, WMT_BT_ONLY_REQUEST, WMT_BT_ONLY_ARGUMENT);
	if (result < 0) {
		fprintf(stderr, "BT-only ioctl failed: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	printf("bt_only_ioctl_ok ret=%d\n", result);
	return EXIT_SUCCESS;
}
