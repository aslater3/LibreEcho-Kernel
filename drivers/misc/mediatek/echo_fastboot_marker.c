// SPDX-License-Identifier: GPL-2.0
/*
 * echo_fastboot_marker.c - Write FASTBOOT_PLEASE to expdb from kernel space.
 *
 * Spawns a thread at postcore_initcall that polls for /dev/mmcblk0p7
 * and writes the FASTBOOT_PLEASE marker as soon as the block device
 * appears.  This ensures the marker is present even if a later
 * initcall panics before userspace starts, so the next watchdog
 * reboot drops into fastboot instead of burning BCB tries.
 *
 * The userspace libreecho-init script also writes the marker (with
 * readback verification); this kernel thread is a safety net for the
 * pre-userspace window.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define EXPDB_PATH	"/dev/mmcblk0p7"
#define MARKER		"FASTBOOT_PLEASE"
#define POLL_MS		100
#define MAX_WAIT_MS	15000

static int marker_thread(void *unused)
{
	struct file *filp = ERR_PTR(-ENODEV);
	mm_segment_t old_fs;
	int elapsed = 0;
	loff_t pos = 0;

	while (elapsed < MAX_WAIT_MS && !kthread_should_stop()) {
		filp = filp_open(EXPDB_PATH, O_WRONLY, 0);
		if (!IS_ERR(filp))
			break;
		msleep(POLL_MS);
		elapsed += POLL_MS;
	}

	if (IS_ERR(filp)) {
		pr_err("echo-marker: %s not available after %d ms\n",
		       EXPDB_PATH, elapsed);
		return -ENODEV;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	vfs_write(filp, MARKER, strlen(MARKER), &pos);
	vfs_fsync(filp, 0);
	set_fs(old_fs);
	filp_close(filp, NULL);

	pr_info("echo-marker: %s written to %s after %d ms\n",
		MARKER, EXPDB_PATH, elapsed);
	return 0;
}

static int __init echo_fastboot_marker_init(void)
{
	struct task_struct *tsk;

	tsk = kthread_run(marker_thread, NULL, "echo-marker");
	if (IS_ERR(tsk)) {
		pr_err("echo-marker: failed to create thread\n");
		return PTR_ERR(tsk);
	}
	return 0;
}
postcore_initcall(echo_fastboot_marker_init);
