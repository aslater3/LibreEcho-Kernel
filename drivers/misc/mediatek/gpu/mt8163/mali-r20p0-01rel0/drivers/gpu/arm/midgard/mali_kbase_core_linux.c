/*
 *
 * (C) COPYRIGHT 2010-2017 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */



#include <mali_kbase.h>
#include <mali_kbase_config_defaults.h>
#include <mali_kbase_uku.h>
#include <mali_midg_regmap.h>
#include <mali_kbase_gator.h>
#include <mali_kbase_mem_linux.h>
#ifdef CONFIG_MALI_DEVFREQ
#include <linux/devfreq.h>
#include <backend/gpu/mali_kbase_devfreq.h>
#ifdef CONFIG_DEVFREQ_THERMAL
#include <ipa/mali_kbase_ipa_debugfs.h>
#endif /* CONFIG_DEVFREQ_THERMAL */
#endif /* CONFIG_MALI_DEVFREQ */
#ifdef CONFIG_MALI_NO_MALI
#include "mali_kbase_model_linux.h"
#endif /* CONFIG_MALI_NO_MALI */
#include "mali_kbase_mem_profile_debugfs_buf_size.h"
#include "mali_kbase_debug_mem_view.h"
#include "mali_kbase_mem.h"
#include "mali_kbase_mem_pool_debugfs.h"
#if !MALI_CUSTOMER_RELEASE
#include "mali_kbase_regs_dump_debugfs.h"
#endif /* !MALI_CUSTOMER_RELEASE */
#include "mali_kbase_regs_history_debugfs.h"
#include <mali_kbase_hwaccess_backend.h>
#include <mali_kbase_hwaccess_jm.h>
#include <mali_kbase_ctx_sched.h>
#include <backend/gpu/mali_kbase_device_internal.h>
#include "mali_kbase_ioctl.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/compat.h>	/* is_compat_task */
#include <linux/mman.h>
#include <linux/version.h>
#include <mali_kbase_hw.h>
#include <platform/mali_kbase_platform_common.h>
#if defined(CONFIG_SYNC) || defined(CONFIG_SYNC_FILE)
#include <mali_kbase_sync.h>
#endif /* CONFIG_SYNC || CONFIG_SYNC_FILE */
#include <linux/clk.h>
#include <linux/delay.h>

#include <mali_kbase_config.h>


#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0))
#include <linux/pm_opp.h>
#else
#include <linux/opp.h>
#endif

#include <mali_kbase_tlstream.h>

#include <mali_kbase_as_fault_debugfs.h>

/* GPU IRQ Tags */
#define	JOB_IRQ_TAG	0
#define MMU_IRQ_TAG	1
#define GPU_IRQ_TAG	2

#if MALI_UNIT_TEST
static struct kbase_exported_test_data shared_kernel_test_data;
EXPORT_SYMBOL(shared_kernel_test_data);
#endif /* MALI_UNIT_TEST */

static int kbase_dev_nr;

static DEFINE_MUTEX(kbase_dev_list_lock);
static LIST_HEAD(kbase_dev_list);

#define KERNEL_SIDE_DDK_VERSION_STRING "K:" MALI_RELEASE_NAME "(GPL)"
static inline void __compile_time_asserts(void)
{
	CSTD_COMPILE_TIME_ASSERT(sizeof(KERNEL_SIDE_DDK_VERSION_STRING) <= KBASE_GET_VERSION_BUFFER_SIZE);
}

static int kbase_api_handshake(struct kbase_context *kctx,
		struct kbase_ioctl_version_check *version)
{
	switch (version->major) {
	case BASE_UK_VERSION_MAJOR:
		/* set minor to be the lowest common */
		version->minor = min_t(int, BASE_UK_VERSION_MINOR,
				(int)version->minor);
		break;
	default:
		/* We return our actual version regardless if it
		 * matches the version returned by userspace -
		 * userspace can bail if it can't handle this
		 * version */
		version->major = BASE_UK_VERSION_MAJOR;
		version->minor = BASE_UK_VERSION_MINOR;
		break;
	}

	/* save the proposed version number for later use */
	kctx->api_version = KBASE_API_VERSION(version->major, version->minor);

	return 0;
}

/**
 * enum mali_error - Mali error codes shared with userspace
 *
 * This is subset of those common Mali errors that can be returned to userspace.
 * Values of matching user and kernel space enumerators MUST be the same.
 * MALI_ERROR_NONE is guaranteed to be 0.
 *
 * @MALI_ERROR_NONE: Success
 * @MALI_ERROR_OUT_OF_GPU_MEMORY: Not used in the kernel driver
 * @MALI_ERROR_OUT_OF_MEMORY: Memory allocation failure
 * @MALI_ERROR_FUNCTION_FAILED: Generic error code
 */
enum mali_error {
	MALI_ERROR_NONE = 0,
	MALI_ERROR_OUT_OF_GPU_MEMORY,
	MALI_ERROR_OUT_OF_MEMORY,
	MALI_ERROR_FUNCTION_FAILED,
};

enum {
	inited_mem = (1u << 0),
	inited_js = (1u << 1),
	inited_pm_runtime_init = (1u << 2),
#ifdef CONFIG_MALI_DEVFREQ
	inited_devfreq = (1u << 3),
#endif /* CONFIG_MALI_DEVFREQ */
	inited_tlstream = (1u << 4),
	inited_backend_early = (1u << 5),
	inited_backend_late = (1u << 6),
	inited_device = (1u << 7),
	inited_vinstr = (1u << 8),

	inited_job_fault = (1u << 10),
	inited_sysfs_group = (1u << 11),
	inited_misc_register = (1u << 12),
	inited_get_device = (1u << 13),
	inited_dev_list = (1u << 14),
	inited_debugfs = (1u << 15),
	inited_gpu_device = (1u << 16),
	inited_registers_map = (1u << 17),
	inited_io_history = (1u << 18),
	inited_power_control = (1u << 19),
	inited_buslogger = (1u << 20),
	inited_protected = (1u << 21),
	inited_ctx_sched = (1u << 22)
};


#ifdef CONFIG_MALI_DEBUG
#define INACTIVE_WAIT_MS (5000)

void kbase_set_driver_inactive(struct kbase_device *kbdev, bool inactive)
{
	kbdev->driver_inactive = inactive;
	wake_up(&kbdev->driver_inactive_wait);

	/* Wait for any running IOCTLs to complete */
	if (inactive)
		msleep(INACTIVE_WAIT_MS);
}
KBASE_EXPORT_TEST_API(kbase_set_driver_inactive);
#endif /* CONFIG_MALI_DEBUG */

/**
 * kbase_legacy_dispatch - UKK dispatch function
 *
 * This is the dispatch function for the legacy UKK ioctl interface. No new
 * ioctls should be added to this function, see kbase_ioctl instead.
 *
 * @kctx: The kernel context structure
 * @args: Pointer to the data structure passed from/to user space
 * @args_size: Size of the data structure
 */
static int kbase_legacy_dispatch(struct kbase_context *kctx,
		void * const args, u32 args_size)
{
	struct kbase_device *kbdev;
	union uk_header *ukh = args;
	u32 id;
	int ret = 0;

	KBASE_DEBUG_ASSERT(ukh != NULL);

	kbdev = kctx->kbdev;
	id = ukh->id;
	ukh->ret = MALI_ERROR_NONE; /* Be optimistic */

#ifdef CONFIG_MALI_DEBUG
	wait_event(kbdev->driver_inactive_wait,
			kbdev->driver_inactive == false);
#endif /* CONFIG_MALI_DEBUG */

	if (UKP_FUNC_ID_CHECK_VERSION == id) {
		struct uku_version_check_args *version_check;
		struct kbase_ioctl_version_check version;

		if (args_size != sizeof(struct uku_version_check_args)) {
			ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			return 0;
		}
		version_check = (struct uku_version_check_args *)args;
		version.minor = version_check->minor;
		version.major = version_check->major;

		kbase_api_handshake(kctx, &version);

		version_check->minor = version.minor;
		version_check->major = version.major;
		ukh->ret = MALI_ERROR_NONE;
		return 0;
	}

	/* block calls until version handshake */
	if (kctx->api_version == 0)
		return -EINVAL;

	if (!atomic_read(&kctx->setup_complete)) {
		struct kbase_uk_set_flags *kbase_set_flags;

		/* setup pending, try to signal that we'll do the setup,
		 * if setup was already in progress, err this call
		 */
		if (atomic_cmpxchg(&kctx->setup_in_progress, 0, 1) != 0)
			return -EINVAL;

		/* if unexpected call, will stay stuck in setup mode
		 * (is it the only call we accept?)
		 */
		if (id != KBASE_FUNC_SET_FLAGS)
			return -EINVAL;

		kbase_set_flags = (struct kbase_uk_set_flags *)args;

		/* if not matching the expected call, stay in setup mode */
		if (sizeof(*kbase_set_flags) != args_size)
			goto bad_size;

		/* if bad flags, will stay stuck in setup mode */
		if (kbase_context_set_create_flags(kctx,
				kbase_set_flags->create_flags) != 0)
			ukh->ret = MALI_ERROR_FUNCTION_FAILED;

		atomic_set(&kctx->setup_complete, 1);
		return 0;
	}

	/* setup complete, perform normal operation */
	switch (id) {
	case KBASE_FUNC_MEM_JIT_INIT:
		{
			struct kbase_uk_mem_jit_init *jit_init = args;

			if (sizeof(*jit_init) != args_size)
				goto bad_size;

			if (kbase_region_tracker_init_jit(kctx,
					jit_init->va_pages))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}
	case KBASE_FUNC_MEM_ALLOC:
		{
			struct kbase_uk_mem_alloc *mem = args;
			struct kbase_va_region *reg;

			if (sizeof(*mem) != args_size)
				goto bad_size;

#if defined(CONFIG_64BIT)
			if (!kbase_ctx_flag(kctx, KCTX_COMPAT)) {
				/* force SAME_VA if a 64-bit client */
				mem->flags |= BASE_MEM_SAME_VA;
			}
#endif

			reg = kbase_mem_alloc(kctx, mem->va_pages,
					mem->commit_pages, mem->extent,
					&mem->flags, &mem->gpu_va);
			mem->va_alignment = 0;

			if (!reg)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}
	case KBASE_FUNC_MEM_IMPORT: {
			struct kbase_uk_mem_import *mem_import = args;
			void __user *phandle;

			if (sizeof(*mem_import) != args_size)
				goto bad_size;
#ifdef CONFIG_COMPAT
			if (kbase_ctx_flag(kctx, KCTX_COMPAT))
				phandle = compat_ptr(mem_import->phandle);
			else
#endif
				phandle = u64_to_user_ptr(mem_import->phandle);

			if (mem_import->type == BASE_MEM_IMPORT_TYPE_INVALID) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (kbase_mem_import(kctx,
					(enum base_mem_import_type)
					mem_import->type,
					phandle,
					0,
					&mem_import->gpu_va,
					&mem_import->va_pages,
					&mem_import->flags)) {
				mem_import->type = BASE_MEM_IMPORT_TYPE_INVALID;
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			}
			break;
	}
	case KBASE_FUNC_MEM_ALIAS: {
			struct kbase_uk_mem_alias *alias = args;
			struct base_mem_aliasing_info __user *user_ai;
			struct base_mem_aliasing_info *ai;

			if (sizeof(*alias) != args_size)
				goto bad_size;

			if (alias->nents > 2048) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}
			if (!alias->nents) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

#ifdef CONFIG_COMPAT
			if (kbase_ctx_flag(kctx, KCTX_COMPAT))
				user_ai = compat_ptr(alias->ai);
			else
#endif
				user_ai = u64_to_user_ptr(alias->ai);

			ai = vmalloc(sizeof(*ai) * alias->nents);

			if (!ai) {
				ukh->ret = MALI_ERROR_OUT_OF_MEMORY;
				break;
			}

			if (copy_from_user(ai, user_ai,
					   sizeof(*ai) * alias->nents)) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				goto copy_failed;
			}

			alias->gpu_va = kbase_mem_alias(kctx, &alias->flags,
							alias->stride,
							alias->nents, ai,
							&alias->va_pages);
			if (!alias->gpu_va) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				goto no_alias;
			}
no_alias:
copy_failed:
			vfree(ai);
			break;
		}
	case KBASE_FUNC_MEM_COMMIT:
		{
			struct kbase_uk_mem_commit *commit = args;
			int ret;

			if (sizeof(*commit) != args_size)
				goto bad_size;

			ret = kbase_mem_commit(kctx, commit->gpu_addr,
					commit->pages);

			ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			commit->result_subcode =
				BASE_BACKING_THRESHOLD_ERROR_INVALID_ARGUMENTS;

			if (ret == 0) {
				ukh->ret = MALI_ERROR_NONE;
				commit->result_subcode =
					BASE_BACKING_THRESHOLD_OK;
			} else if (ret == -ENOMEM) {
				commit->result_subcode =
					BASE_BACKING_THRESHOLD_ERROR_OOM;
			}

			break;
		}

	case KBASE_FUNC_MEM_QUERY:
		{
			struct kbase_uk_mem_query *query = args;

			if (sizeof(*query) != args_size)
				goto bad_size;

			if (kbase_mem_query(kctx, query->gpu_addr,
					query->query, &query->value) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				ukh->ret = MALI_ERROR_NONE;
			break;
		}
		break;

	case KBASE_FUNC_MEM_FLAGS_CHANGE:
		{
			struct kbase_uk_mem_flags_change *fc = args;

			if (sizeof(*fc) != args_size)
				goto bad_size;

			if (kbase_mem_flags_change(kctx, fc->gpu_va,
					fc->flags, fc->mask) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;

			break;
		}
	case KBASE_FUNC_MEM_FREE:
		{
			struct kbase_uk_mem_free *mem = args;

			if (sizeof(*mem) != args_size)
				goto bad_size;

			if (kbase_mem_free(kctx, mem->gpu_addr) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_JOB_SUBMIT:
		{
			struct kbase_uk_job_submit *job = args;
			char __user *user_buf;

			if (sizeof(*job) != args_size)
				goto bad_size;

#ifdef CONFIG_COMPAT
			if (kbase_ctx_flag(kctx, KCTX_COMPAT))
				user_buf = compat_ptr(job->addr);
			else
#endif
				user_buf = u64_to_user_ptr(job->addr);

			if (kbase_jd_submit(kctx, user_buf,
						job->nr_atoms,
						job->stride,
						false) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_SYNC:
		{
			struct kbase_uk_sync_now *sn = args;

			if (sizeof(*sn) != args_size)
				goto bad_size;

			if (kbase_sync_now(kctx, &sn->sset.basep_sset) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_DISJOINT_QUERY:
		{
			struct kbase_uk_disjoint_query *dquery = args;

			if (sizeof(*dquery) != args_size)
				goto bad_size;

			/* Get the disjointness counter value. */
			dquery->counter = kbase_disjoint_event_get(kctx->kbdev);
			break;
		}

	case KBASE_FUNC_POST_TERM:
		{
			kbase_event_close(kctx);
			break;
		}

	case KBASE_FUNC_HWCNT_SETUP:
		{
			struct kbase_uk_hwcnt_setup *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			mutex_lock(&kctx->vinstr_cli_lock);
			if (kbase_vinstr_legacy_hwc_setup(kbdev->vinstr_ctx,
					&kctx->vinstr_cli, setup) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			mutex_unlock(&kctx->vinstr_cli_lock);
			break;
		}

	case KBASE_FUNC_HWCNT_DUMP:
		{
			/* args ignored */
			mutex_lock(&kctx->vinstr_cli_lock);
			if (kbase_vinstr_hwc_dump(kctx->vinstr_cli,
					BASE_HWCNT_READER_EVENT_MANUAL) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			mutex_unlock(&kctx->vinstr_cli_lock);
			break;
		}

	case KBASE_FUNC_HWCNT_CLEAR:
		{
			/* args ignored */
			mutex_lock(&kctx->vinstr_cli_lock);
			if (kbase_vinstr_hwc_clear(kctx->vinstr_cli) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			mutex_unlock(&kctx->vinstr_cli_lock);
			break;
		}

	case KBASE_FUNC_HWCNT_READER_SETUP:
		{
			struct kbase_uk_hwcnt_reader_setup *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			mutex_lock(&kctx->vinstr_cli_lock);
			if (kbase_vinstr_hwcnt_reader_setup(kbdev->vinstr_ctx,
					setup) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			mutex_unlock(&kctx->vinstr_cli_lock);
			break;
		}

	case KBASE_FUNC_GPU_PROPS_REG_DUMP:
		{
			struct kbase_uk_gpuprops *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			if (kbase_gpuprops_uk_get_props(kctx, setup) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}
	case KBASE_FUNC_FIND_CPU_OFFSET:
		{
			struct kbase_uk_find_cpu_offset *find = args;

			if (sizeof(*find) != args_size)
				goto bad_size;

			if (find->gpu_addr & ~PAGE_MASK) {
				dev_warn(kbdev->dev, "kbase_legacy_dispatch case KBASE_FUNC_FIND_CPU_OFFSET: find->gpu_addr: passed parameter is invalid");
				goto out_bad;
			}

			if (find->size > SIZE_MAX || find->cpu_addr > ULONG_MAX) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			} else {
				int err;

				err = kbasep_find_enclosing_cpu_mapping_offset(
						kctx,
						find->cpu_addr,
						find->size,
						&find->offset);

				if (err)
					ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			}
			break;
		}
	case KBASE_FUNC_GET_VERSION:
		{
			struct kbase_uk_get_ddk_version *get_version = (struct kbase_uk_get_ddk_version *)args;

			if (sizeof(*get_version) != args_size)
				goto bad_size;

			/* version buffer size check is made in compile time assert */
			memcpy(get_version->version_buffer, KERNEL_SIDE_DDK_VERSION_STRING, sizeof(KERNEL_SIDE_DDK_VERSION_STRING));
			get_version->version_string_size = sizeof(KERNEL_SIDE_DDK_VERSION_STRING);
			break;
		}

	case KBASE_FUNC_STREAM_CREATE:
		{
#if defined(CONFIG_SYNC) || defined(CONFIG_SYNC_FILE)
			struct kbase_uk_stream_create *screate = (struct kbase_uk_stream_create *)args;

			if (sizeof(*screate) != args_size)
				goto bad_size;

			if (strnlen(screate->name, sizeof(screate->name)) >= sizeof(screate->name)) {
				/* not NULL terminated */
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (kbase_sync_fence_stream_create(screate->name,
							   &screate->fd) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				ukh->ret = MALI_ERROR_NONE;
#else /* CONFIG_SYNC || CONFIG_SYNC_FILE */
			ukh->ret = MALI_ERROR_FUNCTION_FAILED;
#endif /* CONFIG_SYNC || CONFIG_SYNC_FILE */
			break;
		}
	case KBASE_FUNC_FENCE_VALIDATE:
		{
#if defined(CONFIG_SYNC) || defined(CONFIG_SYNC_FILE)
			struct kbase_uk_fence_validate *fence_validate = (struct kbase_uk_fence_validate *)args;

			if (sizeof(*fence_validate) != args_size)
				goto bad_size;

			if (kbase_sync_fence_validate(fence_validate->fd) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				ukh->ret = MALI_ERROR_NONE;
#endif /* CONFIG_SYNC || CONFIG_SYNC_FILE */
			break;
		}

	case KBASE_FUNC_SET_TEST_DATA:
		{
#if MALI_UNIT_TEST
			struct kbase_uk_set_test_data *set_data = args;

			shared_kernel_test_data = set_data->test_data;
			shared_kernel_test_data.kctx = (uintptr_t)kctx;
			shared_kernel_test_data.mm = (uintptr_t)current->mm;
			ukh->ret = MALI_ERROR_NONE;
#endif /* MALI_UNIT_TEST */
			break;
		}

	case KBASE_FUNC_INJECT_ERROR:
		{
#ifdef CONFIG_MALI_ERROR_INJECT
			unsigned long flags;
			struct kbase_error_params params = ((struct kbase_uk_error_params *)args)->params;

			/*mutex lock */
			spin_lock_irqsave(&kbdev->reg_op_lock, flags);
			if (job_atom_inject_error(&params) != 0)
				ukh->ret = MALI_ERROR_OUT_OF_MEMORY;
			else
				ukh->ret = MALI_ERROR_NONE;
			spin_unlock_irqrestore(&kbdev->reg_op_lock, flags);
			/*mutex unlock */
#endif /* CONFIG_MALI_ERROR_INJECT */
			break;
		}

	case KBASE_FUNC_MODEL_CONTROL:
		{
#ifdef CONFIG_MALI_NO_MALI
			unsigned long flags;
			struct kbase_model_control_params params =
					((struct kbase_uk_model_control_params *)args)->params;

			/*mutex lock */
			spin_lock_irqsave(&kbdev->reg_op_lock, flags);
			if (gpu_model_control(kbdev->model, &params) != 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				ukh->ret = MALI_ERROR_NONE;
			spin_unlock_irqrestore(&kbdev->reg_op_lock, flags);
			/*mutex unlock */
#endif /* CONFIG_MALI_NO_MALI */
			break;
		}

	case KBASE_FUNC_GET_PROFILING_CONTROLS:
		{
			struct kbase_uk_profiling_controls *controls =
					(struct kbase_uk_profiling_controls *)args;
			u32 i;

			if (sizeof(*controls) != args_size)
				goto bad_size;

			for (i = FBDUMP_CONTROL_MIN; i < FBDUMP_CONTROL_MAX; i++)
				controls->profiling_controls[i] =
					kbdev->kbase_profiling_controls[i];

			break;
		}

	/* used only for testing purposes; these controls are to be set by gator through gator API */
	case KBASE_FUNC_SET_PROFILING_CONTROLS:
		{
			struct kbase_uk_profiling_controls *controls =
					(struct kbase_uk_profiling_controls *)args;
			u32 i;

			if (sizeof(*controls) != args_size)
				goto bad_size;

			for (i = FBDUMP_CONTROL_MIN; i < FBDUMP_CONTROL_MAX; i++)
				_mali_profiling_control(i, controls->profiling_controls[i]);

			break;
		}

	case KBASE_FUNC_DEBUGFS_MEM_PROFILE_ADD:
		{
			struct kbase_uk_debugfs_mem_profile_add *add_data =
					(struct kbase_uk_debugfs_mem_profile_add *)args;
			char *buf;
			char __user *user_buf;

			if (sizeof(*add_data) != args_size)
				goto bad_size;

			if (add_data->len > KBASE_MEM_PROFILE_MAX_BUF_SIZE) {
				dev_err(kbdev->dev, "buffer too big\n");
				goto out_bad;
			}

#ifdef CONFIG_COMPAT
			if (kbase_ctx_flag(kctx, KCTX_COMPAT))
				user_buf = compat_ptr(add_data->buf);
			else
#endif
				user_buf = u64_to_user_ptr(add_data->buf);

			buf = kmalloc(add_data->len, GFP_KERNEL);
			if (ZERO_OR_NULL_PTR(buf))
				goto out_bad;

			if (0 != copy_from_user(buf, user_buf, add_data->len)) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				kfree(buf);
				goto out_bad;
			}

			if (kbasep_mem_profile_debugfs_insert(kctx, buf,
							add_data->len)) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				goto out_bad;
			}

			break;
		}

#ifdef CONFIG_MALI_NO_MALI
	case KBASE_FUNC_SET_PRFCNT_VALUES:
		{

			struct kbase_uk_prfcnt_values *params =
			  ((struct kbase_uk_prfcnt_values *)args);
			gpu_model_set_dummy_prfcnt_sample(params->data,
					params->size);

			break;
		}
#endif /* CONFIG_MALI_NO_MALI */
#ifdef BASE_LEGACY_UK10_4_SUPPORT
	case KBASE_FUNC_TLSTREAM_ACQUIRE_V10_4:
		{
			struct kbase_uk_tlstream_acquire_v10_4 *tlstream_acquire
					= args;
			int ret;

			if (sizeof(*tlstream_acquire) != args_size)
				goto bad_size;

			ret = kbase_tlstream_acquire(
						kctx, 0);
			if (ret < 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				tlstream_acquire->fd = ret;
			break;
		}
#endif /* BASE_LEGACY_UK10_4_SUPPORT */
	case KBASE_FUNC_TLSTREAM_ACQUIRE:
		{
			struct kbase_uk_tlstream_acquire *tlstream_acquire =
				args;
			int ret;

			if (sizeof(*tlstream_acquire) != args_size)
				goto bad_size;

			if (tlstream_acquire->flags & ~BASE_TLSTREAM_FLAGS_MASK)
				goto out_bad;

			ret = kbase_tlstream_acquire(
					kctx, tlstream_acquire->flags);
			if (ret < 0)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			else
				tlstream_acquire->fd = ret;
			break;
		}
	case KBASE_FUNC_TLSTREAM_FLUSH:
		{
			struct kbase_uk_tlstream_flush *tlstream_flush =
				args;

			if (sizeof(*tlstream_flush) != args_size)
				goto bad_size;

			kbase_tlstream_flush_streams();
			break;
		}
#if MALI_UNIT_TEST
	case KBASE_FUNC_TLSTREAM_TEST:
		{
			struct kbase_uk_tlstream_test *tlstream_test = args;

			if (sizeof(*tlstream_test) != args_size)
				goto bad_size;

			kbase_tlstream_test(
					tlstream_test->tpw_count,
					tlstream_test->msg_delay,
					tlstream_test->msg_count,
					tlstream_test->aux_msg);
			break;
		}
	case KBASE_FUNC_TLSTREAM_STATS:
		{
			struct kbase_uk_tlstream_stats *tlstream_stats = args;

			if (sizeof(*tlstream_stats) != args_size)
				goto bad_size;

			kbase_tlstream_stats(
					&tlstream_stats->bytes_collected,
					&tlstream_stats->bytes_generated);
			break;
		}
#endif /* MALI_UNIT_TEST */

	case KBASE_FUNC_GET_CONTEXT_ID:
		{
			struct kbase_uk_context_id *info = args;

			info->id = kctx->id;
			break;
		}

	case KBASE_FUNC_SOFT_EVENT_UPDATE:
		{
			struct kbase_uk_soft_event_update *update = args;

			if (sizeof(*update) != args_size)
				goto bad_size;

			if (((update->new_status != BASE_JD_SOFT_EVENT_SET) &&
			    (update->new_status != BASE_JD_SOFT_EVENT_RESET)) ||
			    (update->flags != 0))
				goto out_bad;

			if (kbase_soft_event_update(kctx, update->evt,
						update->new_status))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;

			break;
		}

	default:
		dev_err(kbdev->dev, "unknown ioctl %u\n", id);
		goto out_bad;
	}

	return ret;

 bad_size:
	dev_err(kbdev->dev, "Wrong syscall size (%d) for %08x\n", args_size, id);
 out_bad:
	return -EINVAL;
}

static struct kbase_device *to_kbase_device(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static int assign_irqs(struct platform_device *pdev)
{
	struct kbase_device *kbdev = to_kbase_device(&pdev->dev);
	int i;

	if (!kbdev)
		return -ENODEV;

	/* 3 IRQ resources */
	for (i = 0; i < 3; i++) {
		struct resource *irq_res;
		int irqtag;

		irq_res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!irq_res) {
			dev_err(kbdev->dev, "No IRQ resource at index %d\n", i);
			return -ENOENT;
		}

#ifdef CONFIG_OF
		if (!strncmp(irq_res->name, "JOB", 4)) {
			irqtag = JOB_IRQ_TAG;
		} else if (!strncmp(irq_res->name, "MMU", 4)) {
			irqtag = MMU_IRQ_TAG;
		} else if (!strncmp(irq_res->name, "GPU", 4)) {
			irqtag = GPU_IRQ_TAG;
		} else {
			dev_err(&pdev->dev, "Invalid irq res name: '%s'\n",
				irq_res->name);
			return -EINVAL;
		}
#else
		irqtag = i;
#endif /* CONFIG_OF */
		kbdev->irqs[irqtag].irq = irq_res->start;
		kbdev->irqs[irqtag].flags = irq_res->flags & IRQF_TRIGGER_MASK;
	}

	return 0;
}

/*
 * API to acquire device list mutex and
 * return pointer to the device list head
 */
const struct list_head *kbase_dev_list_get(void)
{
	mutex_lock(&kbase_dev_list_lock);
	return &kbase_dev_list;
}
KBASE_EXPORT_TEST_API(kbase_dev_list_get);

/* API to release the device list mutex */
void kbase_dev_list_put(const struct list_head *dev_list)
{
	mutex_unlock(&kbase_dev_list_lock);
}
KBASE_EXPORT_TEST_API(kbase_dev_list_put);

/* Find a particular kbase device (as specified by minor number), or find the "first" device if -1 is specified */
struct kbase_device *kbase_find_device(int minor)
{
	struct kbase_device *kbdev = NULL;
	struct list_head *entry;
	const struct list_head *dev_list = kbase_dev_list_get();

	list_for_each(entry, dev_list) {
		struct kbase_device *tmp;

		tmp = list_entry(entry, struct kbase_device, entry);
		if (tmp->mdev.minor == minor || minor == -1) {
			kbdev = tmp;
			get_device(kbdev->dev);
			break;
		}
	}
	kbase_dev_list_put(dev_list);

	return kbdev;
}
EXPORT_SYMBOL(kbase_find_device);

void kbase_release_device(struct kbase_device *kbdev)
{
	put_device(kbdev->dev);
}
EXPORT_SYMBOL(kbase_release_device);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0) && \
		!(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 28) && \
		LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0))
/*
 * Older versions, before v4.6, of the kernel doesn't have
 * kstrtobool_from_user(), except longterm 4.4.y which had it added in 4.4.28
 */
static int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[32];

	count = min(sizeof(buf), count);

	if (copy_from_user(buf, s, count))
		return -EFAULT;
	buf[count] = '\0';

	return strtobool(buf, res);
}
#endif

static ssize_t write_ctx_infinite_cache(struct file *f, const char __user *ubuf, size_t size, loff_t *off)
{
	struct kbase_context *kctx = f->private_data;
	int err;
	bool value;

	err = kstrtobool_from_user(ubuf, size, &value);
	if (err)
		return err;

	if (value)
		kbase_ctx_flag_set(kctx, KCTX_INFINITE_CACHE);
	else
		kbase_ctx_flag_clear(kctx, KCTX_INFINITE_CACHE);

	return size;
}

static ssize_t read_ctx_infinite_cache(struct file *f, char __user *ubuf, size_t size, loff_t *off)
{
	struct kbase_context *kctx = f->private_data;
	char buf[32];
	int count;
	bool value;

	value = kbase_ctx_flag(kctx, KCTX_INFINITE_CACHE);

	count = scnprintf(buf, sizeof(buf), "%s\n", value ? "Y" : "N");

	return simple_read_from_buffer(ubuf, size, off, buf, count);
}

static const struct file_operations kbase_infinite_cache_fops = {
	.open = simple_open,
	.write = write_ctx_infinite_cache,
	.read = read_ctx_infinite_cache,
};

static int kbase_open(struct inode *inode, struct file *filp)
{
	struct kbase_device *kbdev = NULL;
	struct kbase_context *kctx;
	int ret = 0;
#ifdef CONFIG_DEBUG_FS
	char kctx_name[64];
#endif

	kbdev = kbase_find_device(iminor(inode));

	if (!kbdev)
		return -ENODEV;

	kctx = kbase_create_context(kbdev, is_compat_task());
	if (!kctx) {
		ret = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&kctx->event_queue);
	filp->private_data = kctx;
	kctx->filp = filp;

	if (kbdev->infinite_cache_active_default)
		kbase_ctx_flag_set(kctx, KCTX_INFINITE_CACHE);

#ifdef CONFIG_DEBUG_FS
	snprintf(kctx_name, 64, "%d_%d", kctx->tgid, kctx->id);

	kctx->kctx_dentry = debugfs_create_dir(kctx_name,
			kbdev->debugfs_ctx_directory);

	if (IS_ERR_OR_NULL(kctx->kctx_dentry)) {
		ret = -ENOMEM;
		goto out;
	}

	debugfs_create_file("infinite_cache", 0644, kctx->kctx_dentry,
			    kctx, &kbase_infinite_cache_fops);

	mutex_init(&kctx->mem_profile_lock);

	kbasep_jd_debugfs_ctx_init(kctx);
	kbase_debug_mem_view_init(filp);

	kbase_debug_job_fault_context_init(kctx);

	kbase_mem_pool_debugfs_init(kctx->kctx_dentry, &kctx->mem_pool, &kctx->lp_mem_pool);

	kbase_jit_debugfs_init(kctx);
#endif /* CONFIG_DEBUG_FS */

	dev_dbg(kbdev->dev, "created base context\n");

	{
		struct kbasep_kctx_list_element *element;

		element = kzalloc(sizeof(*element), GFP_KERNEL);
		if (element) {
			mutex_lock(&kbdev->kctx_list_lock);
			element->kctx = kctx;
			list_add(&element->link, &kbdev->kctx_list);
			KBASE_TLSTREAM_TL_NEW_CTX(
					element->kctx,
					element->kctx->id,
					(u32)(element->kctx->tgid));
			mutex_unlock(&kbdev->kctx_list_lock);
		} else {
			/* we don't treat this as a fail - just warn about it */
			dev_warn(kbdev->dev, "couldn't add kctx to kctx_list\n");
		}
	}
	return 0;

 out:
	kbase_release_device(kbdev);
	return ret;
}

static int kbase_release(struct inode *inode, struct file *filp)
{
	struct kbase_context *kctx = filp->private_data;
	struct kbase_device *kbdev = kctx->kbdev;
	struct kbasep_kctx_list_element *element, *tmp;
	bool found_element = false;

	KBASE_TLSTREAM_TL_DEL_CTX(kctx);

#ifdef CONFIG_DEBUG_FS
	kbasep_mem_profile_debugfs_remove(kctx);
	kbase_debug_job_fault_context_term(kctx);
#endif

	mutex_lock(&kbdev->kctx_list_lock);
	list_for_each_entry_safe(element, tmp, &kbdev->kctx_list, link) {
		if (element->kctx == kctx) {
			list_del(&element->link);
			kfree(element);
			found_element = true;
		}
	}
	mutex_unlock(&kbdev->kctx_list_lock);
	if (!found_element)
		dev_warn(kbdev->dev, "kctx not in kctx_list\n");

	filp->private_data = NULL;

	mutex_lock(&kctx->vinstr_cli_lock);
	/* If this client was performing hwcnt dumping and did not explicitly
	 * detach itself, remove it from the vinstr core now */
	if (kctx->vinstr_cli) {
		struct kbase_uk_hwcnt_setup setup;

		setup.dump_buffer = 0llu;
		kbase_vinstr_legacy_hwc_setup(
				kbdev->vinstr_ctx, &kctx->vinstr_cli, &setup);
	}
	mutex_unlock(&kctx->vinstr_cli_lock);

	kbase_destroy_context(kctx);

	dev_dbg(kbdev->dev, "deleted base context\n");
	kbase_release_device(kbdev);
	return 0;
}

#define CALL_MAX_SIZE 536

static long kbase_legacy_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	u64 msg[(CALL_MAX_SIZE + 7) >> 3] = { 0xdeadbeefdeadbeefull };	/* alignment fixup */
	u32 size = _IOC_SIZE(cmd);
	struct kbase_context *kctx = filp->private_data;

	if (size > CALL_MAX_SIZE)
		return -ENOTTY;

	if (0 != copy_from_user(&msg, (void __user *)arg, size)) {
		dev_err(kctx->kbdev->dev, "failed to copy ioctl argument into kernel space\n");
		return -EFAULT;
	}

	if (kbase_legacy_dispatch(kctx, &msg, size) != 0)
		return -EFAULT;

	if (0 != copy_to_user((void __user *)arg, &msg, size)) {
		dev_err(kctx->kbdev->dev, "failed to copy results of UK call back to user space\n");
		return -EFAULT;
	}
	return 0;
}

static int kbase_api_set_flags(struct kbase_context *kctx,
		struct kbase_ioctl_set_flags *flags)
{
	int err;

	/* setup pending, try to signal that we'll do the setup,
	 * if setup was already in progress, err this call
	 */
	if (atomic_cmpxchg(&kctx->setup_in_progress, 0, 1) != 0)
		return -EINVAL;

	err = kbase_context_set_create_flags(kctx, flags->create_flags);
	/* if bad flags, will stay stuck in setup mode */
	if (err)
		return err;

	atomic_set(&kctx->setup_complete, 1);
	return 0;
}

static int kbase_api_job_submit(struct kbase_context *kctx,
		struct kbase_ioctl_job_submit *submit)
{
	return kbase_jd_submit(kctx, u64_to_user_ptr(submit->addr),
			submit->nr_atoms,
			submit->stride, false);
}

static int kbase_api_get_gpuprops(struct kbase_context *kctx,
		struct kbase_ioctl_get_gpuprops *get_props)
{
	struct kbase_gpu_props *kprops = &kctx->kbdev->gpu_props;
	int err;

	if (get_props->flags != 0) {
		dev_err(kctx->kbdev->dev, "Unsupported flags to get_gpuprops");
		return -EINVAL;
	}

	if (get_props->size == 0)
		return kprops->prop_buffer_size;
	if (get_props->size < kprops->prop_buffer_size)
		return -EINVAL;

	err = copy_to_user(u64_to_user_ptr(get_props->buffer),
			kprops->prop_buffer,
			kprops->prop_buffer_size);
	if (err)
		return -EFAULT;
	return kprops->prop_buffer_size;
}

static int kbase_api_post_term(struct kbase_context *kctx)
{
	kbase_event_close(kctx);
	return 0;
}

static int kbase_api_mem_alloc(struct kbase_context *kctx,
		union kbase_ioctl_mem_alloc *alloc)
{
	struct kbase_va_region *reg;
	u64 flags = alloc->in.flags;
	u64 gpu_va;

#if defined(CONFIG_64BIT)
	if (!kbase_ctx_flag(kctx, KCTX_COMPAT)) {
		/* force SAME_VA if a 64-bit client */
		flags |= BASE_MEM_SAME_VA;
	}
#endif

	reg = kbase_mem_alloc(kctx, alloc->in.va_pages,
			alloc->in.commit_pages,
			alloc->in.extent,
			&flags, &gpu_va);

	if (!reg)
		return -ENOMEM;

	alloc->out.flags = flags;
	alloc->out.gpu_va = gpu_va;

	return 0;
}

static int kbase_api_mem_query(struct kbase_context *kctx,
		union kbase_ioctl_mem_query *query)
{
	return kbase_mem_query(kctx, query->in.gpu_addr,
			query->in.query, &query->out.value);
}

static int kbase_api_mem_free(struct kbase_context *kctx,
		struct kbase_ioctl_mem_free *free)
{
	return kbase_mem_free(kctx, free->gpu_addr);
}

static int kbase_api_hwcnt_reader_setup(struct kbase_context *kctx,
		struct kbase_ioctl_hwcnt_reader_setup *setup)
{
	int ret;
	struct kbase_uk_hwcnt_reader_setup args = {
		.buffer_count = setup->buffer_count,
		.jm_bm = setup->jm_bm,
		.shader_bm = setup->shader_bm,
		.tiler_bm = setup->tiler_bm,
		.mmu_l2_bm = setup->mmu_l2_bm
	};

	mutex_lock(&kctx->vinstr_cli_lock);
	ret = kbase_vinstr_hwcnt_reader_setup(kctx->kbdev->vinstr_ctx, &args);
	mutex_unlock(&kctx->vinstr_cli_lock);

	if (ret)
		return ret;
	return args.fd;
}

static int kbase_api_hwcnt_enable(struct kbase_context *kctx,
		struct kbase_ioctl_hwcnt_enable *enable)
{
	int ret;
	struct kbase_uk_hwcnt_setup args = {
		.dump_buffer = enable->dump_buffer,
		.jm_bm = enable->jm_bm,
		.shader_bm = enable->shader_bm,
		.tiler_bm = enable->tiler_bm,
		.mmu_l2_bm = enable->mmu_l2_bm
	};

	mutex_lock(&kctx->vinstr_cli_lock);
	ret = kbase_vinstr_legacy_hwc_setup(kctx->kbdev->vinstr_ctx,
			&kctx->vinstr_cli, &args);
	mutex_unlock(&kctx->vinstr_cli_lock);

	return ret;
}

static int kbase_api_hwcnt_dump(struct kbase_context *kctx)
{
	int ret;

	mutex_lock(&kctx->vinstr_cli_lock);
	ret = kbase_vinstr_hwc_dump(kctx->vinstr_cli,
			BASE_HWCNT_READER_EVENT_MANUAL);
	mutex_unlock(&kctx->vinstr_cli_lock);

	return ret;
}

static int kbase_api_hwcnt_clear(struct kbase_context *kctx)
{
	int ret;

	mutex_lock(&kctx->vinstr_cli_lock);
	ret = kbase_vinstr_hwc_clear(kctx->vinstr_cli);
	mutex_unlock(&kctx->vinstr_cli_lock);

	return ret;
}

static int kbase_api_disjoint_query(struct kbase_context *kctx,
		struct kbase_ioctl_disjoint_query *query)
{
	query->counter = kbase_disjoint_event_get(kctx->kbdev);

	return 0;
}

static int kbase_api_get_ddk_version(struct kbase_context *kctx,
		struct kbase_ioctl_get_ddk_version *version)
{
	int ret;
	int len = sizeof(KERNEL_SIDE_DDK_VERSION_STRING);

	if (version->version_buffer == 0)
		return len;

	if (version->size < len)
		return -EOVERFLOW;

	ret = copy_to_user(u64_to_user_ptr(version->version_buffer),
			KERNEL_SIDE_DDK_VERSION_STRING,
			sizeof(KERNEL_SIDE_DDK_VERSION_STRING));

	if (ret)
		return -EFAULT;

	return len;
}

static int kbase_api_mem_jit_init(struct kbase_context *kctx,
		struct kbase_ioctl_mem_jit_init *jit_init)
{
	return kbase_region_tracker_init_jit(kctx, jit_init->va_pages);
}

static int kbase_api_mem_sync(struct kbase_context *kctx,
		struct kbase_ioctl_mem_sync *sync)
{
	struct basep_syncset sset = {
		.mem_handle.basep.handle = sync->handle,
		.user_addr = sync->user_addr,
		.size = sync->size,
		.type = sync->type
	};

	return kbase_sync_now(kctx, &sset);
}

static int kbase_api_mem_find_cpu_offset(struct kbase_context *kctx,
		union kbase_ioctl_mem_find_cpu_offset *find)
{
	return kbasep_find_enclosing_cpu_mapping_offset(
			kctx,
			find->in.cpu_addr,
			find->in.size,
			&find->out.offset);
}

static int kbase_api_get_context_id(struct kbase_context *kctx,
		struct kbase_ioctl_get_context_id *info)
{
	info->id = kctx->id;

	return 0;
}

static int kbase_api_tlstream_acquire(struct kbase_context *kctx,
		struct kbase_ioctl_tlstream_acquire *acquire)
{
	return kbase_tlstream_acquire(kctx, acquire->flags);
}

static int kbase_api_tlstream_flush(struct kbase_context *kctx)
{
	kbase_tlstream_flush_streams();

	return 0;
}

static int kbase_api_mem_commit(struct kbase_context *kctx,
		struct kbase_ioctl_mem_commit *commit)
{
	return kbase_mem_commit(kctx, commit->gpu_addr, commit->pages);
}

static int kbase_api_mem_alias(struct kbase_context *kctx,
		union kbase_ioctl_mem_alias *alias)
{
	struct base_mem_aliasing_info *ai;
	u64 flags;
	int err;

	if (alias->in.nents == 0 || alias->in.nents > 2048)
		return -EINVAL;

	ai = vmalloc(sizeof(*ai) * alias->in.nents);
	if (!ai)
		return -ENOMEM;

	err = copy_from_user(ai,
			u64_to_user_ptr(alias->in.aliasing_info),
			sizeof(*ai) * alias->in.nents);
	if (err) {
		vfree(ai);
		return -EFAULT;
	}

	flags = alias->in.flags;

	alias->out.gpu_va = kbase_mem_alias(kctx, &flags,
			alias->in.stride, alias->in.nents,
			ai, &alias->out.va_pages);

	alias->out.flags = flags;

	vfree(ai);

	if (alias->out.gpu_va == 0)
		return -ENOMEM;

	return 0;
}

static int kbase_api_mem_import(struct kbase_context *kctx,
		union kbase_ioctl_mem_import *import)
{
	int ret;
	u64 flags = import->in.flags;

	ret = kbase_mem_import(kctx,
			import->in.type,
			u64_to_user_ptr(import->in.phandle),
			import->in.padding,
			&import->out.gpu_va,
			&import->out.va_pages,
			&flags);

	import->out.flags = flags;

	return ret;
}

static int kbase_api_mem_flags_change(struct kbase_context *kctx,
		struct kbase_ioctl_mem_flags_change *change)
{
	return kbase_mem_flags_change(kctx, change->gpu_va,
			change->flags, change->mask);
}

static int kbase_api_stream_create(struct kbase_context *kctx,
		struct kbase_ioctl_stream_create *stream)
{
#if defined(CONFIG_SYNC) || defined(CONFIG_SYNC_FILE)
	int fd, ret;

	/* Name must be NULL-terminated and padded with NULLs, so check last
	 * character is NULL
	 */
	if (stream->name[sizeof(stream->name)-1] != 0)
		return -EINVAL;

	ret = kbase_sync_fence_stream_create(stream->name, &fd);

	if (ret)
		return ret;
	return fd;
#else
	return -ENOENT;
#endif
}

static int kbase_api_fence_validate(struct kbase_context *kctx,
		struct kbase_ioctl_fence_validate *validate)
{
#if defined(CONFIG_SYNC) || defined(CONFIG_SYNC_FILE)
	return kbase_sync_fence_validate(validate->fd);
#else
	return -ENOENT;
#endif
}

static int kbase_api_get_profiling_controls(struct kbase_context *kctx,
		struct kbase_ioctl_get_profiling_controls *controls)
{
	int ret;

	if (controls->count > (FBDUMP_CONTROL_MAX - FBDUMP_CONTROL_MIN))
		return -EINVAL;

	ret = copy_to_user(u64_to_user_ptr(controls->buffer),
			&kctx->kbdev->kbase_profiling_controls[
				FBDUMP_CONTROL_MIN],
			controls->count * sizeof(u32));

	if (ret)
		return -EFAULT;
	return 0;
}

static int kbase_api_mem_profile_add(struct kbase_context *kctx,
		struct kbase_ioctl_mem_profile_add *data)
{
	char *buf;
	int err;

	if (data->len > KBASE_MEM_PROFILE_MAX_BUF_SIZE) {
		dev_err(kctx->kbdev->dev, "mem_profile_add: buffer too big\n");
		return -EINVAL;
	}

	buf = kmalloc(data->len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	err = copy_from_user(buf, u64_to_user_ptr(data->buffer),
			data->len);
	if (err) {
		kfree(buf);
		return -EFAULT;
	}

	return kbasep_mem_profile_debugfs_insert(kctx, buf, data->len);
}

static int kbase_api_soft_event_update(struct kbase_context *kctx,
		struct kbase_ioctl_soft_event_update *update)
{
	if (update->flags != 0)
		return -EINVAL;

	return kbase_soft_event_update(kctx, update->event, update->new_status);
}

#if MALI_UNIT_TEST
static int kbase_api_tlstream_test(struct kbase_context *kctx,
		struct kbase_ioctl_tlstream_test *test)
{
	kbase_tlstream_test(
			test->tpw_count,
			test->msg_delay,
			test->msg_count,
			test->aux_msg);

	return 0;
}

static int kbase_api_tlstream_stats(struct kbase_context *kctx,
		struct kbase_ioctl_tlstream_stats *stats)
{
	kbase_tlstream_stats(
			&stats->bytes_collected,
			&stats->bytes_generated);

	return 0;
}
#endif /* MALI_UNIT_TEST */

#define KBASE_HANDLE_IOCTL(cmd, function)                          \
	case cmd:                                                  \
	do {                                                       \
		if (_IOC_DIR(cmd) != _IOC_NONE) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }          \
		return function(kctx);                             \
	} while (0)

#define KBASE_HANDLE_IOCTL_IN(cmd, function, type)                 \
	case cmd:                                                  \
	do {                                                       \
		type param;                                        \
		int err;                                           \
		if (_IOC_DIR(cmd) != _IOC_WRITE) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }         \
		if (sizeof(param) != _IOC_SIZE(cmd)) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }     \
		err = copy_from_user(&param, uarg, sizeof(param)); \
		if (err)                                           \
			return -EFAULT;                            \
		return function(kctx, &param);                     \
	} while (0)

#define KBASE_HANDLE_IOCTL_OUT(cmd, function, type)                \
	case cmd:                                                  \
	do {                                                       \
		type param;                                        \
		int ret, err;                                      \
		if (_IOC_DIR(cmd) != _IOC_READ) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }          \
		if (sizeof(param) != _IOC_SIZE(cmd)) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }     \
		ret = function(kctx, &param);                      \
		err = copy_to_user(uarg, &param, sizeof(param));   \
		if (err)                                           \
			return -EFAULT;                            \
		return ret;                                        \
	} while (0)

#define KBASE_HANDLE_IOCTL_INOUT(cmd, function, type)                  \
	case cmd:                                                      \
	do {                                                           \
		type param;                                            \
		int ret, err;                                          \
		if (_IOC_DIR(cmd) != (_IOC_WRITE|_IOC_READ)) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); } \
		if (sizeof(param) != _IOC_SIZE(cmd)) { pr_warn("stub: BUG_ON avoided in %s\n", __func__); }         \
		err = copy_from_user(&param, uarg, sizeof(param));     \
		if (err)                                               \
			return -EFAULT;                                \
		ret = function(kctx, &param);                          \
		err = copy_to_user(uarg, &param, sizeof(param));       \
		if (err)                                               \
			return -EFAULT;                                \
		return ret;                                            \
	} while (0)

static long kbase_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct kbase_context *kctx = filp->private_data;
	struct kbase_device *kbdev = kctx->kbdev;
	void __user *uarg = (void __user *)arg;

	/* The UK ioctl values overflow the cmd field causing the type to be
	 * incremented
	 */
	if (_IOC_TYPE(cmd) == LINUX_UK_BASE_MAGIC+2)
		return kbase_legacy_ioctl(filp, cmd, arg);

	/* The UK version check IOCTL doesn't overflow the cmd field, so is
	 * handled separately here
	 */
	if (cmd == _IOC(_IOC_READ|_IOC_WRITE, LINUX_UK_BASE_MAGIC,
				UKP_FUNC_ID_CHECK_VERSION,
				sizeof(struct uku_version_check_args)))
		return kbase_legacy_ioctl(filp, cmd, arg);

	/* Only these ioctls are available until setup is complete */
	switch (cmd) {
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_VERSION_CHECK,
				kbase_api_handshake,
				struct kbase_ioctl_version_check);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_SET_FLAGS,
				kbase_api_set_flags,
				struct kbase_ioctl_set_flags);
	}

	/* Block call until version handshake and setup is complete */
	if (kctx->api_version == 0 || !atomic_read(&kctx->setup_complete))
		return -EINVAL;

	/* Normal ioctls */
	switch (cmd) {
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_JOB_SUBMIT,
				kbase_api_job_submit,
				struct kbase_ioctl_job_submit);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_GET_GPUPROPS,
				kbase_api_get_gpuprops,
				struct kbase_ioctl_get_gpuprops);
		KBASE_HANDLE_IOCTL(KBASE_IOCTL_POST_TERM,
				kbase_api_post_term);
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_MEM_ALLOC,
				kbase_api_mem_alloc,
				union kbase_ioctl_mem_alloc);
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_MEM_QUERY,
				kbase_api_mem_query,
				union kbase_ioctl_mem_query);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_FREE,
				kbase_api_mem_free,
				struct kbase_ioctl_mem_free);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_HWCNT_READER_SETUP,
				kbase_api_hwcnt_reader_setup,
				struct kbase_ioctl_hwcnt_reader_setup);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_HWCNT_ENABLE,
				kbase_api_hwcnt_enable,
				struct kbase_ioctl_hwcnt_enable);
		KBASE_HANDLE_IOCTL(KBASE_IOCTL_HWCNT_DUMP,
				kbase_api_hwcnt_dump);
		KBASE_HANDLE_IOCTL(KBASE_IOCTL_HWCNT_CLEAR,
				kbase_api_hwcnt_clear);
		KBASE_HANDLE_IOCTL_OUT(KBASE_IOCTL_DISJOINT_QUERY,
				kbase_api_disjoint_query,
				struct kbase_ioctl_disjoint_query);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_GET_DDK_VERSION,
				kbase_api_get_ddk_version,
				struct kbase_ioctl_get_ddk_version);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_JIT_INIT,
				kbase_api_mem_jit_init,
				struct kbase_ioctl_mem_jit_init);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_SYNC,
				kbase_api_mem_sync,
				struct kbase_ioctl_mem_sync);
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_MEM_FIND_CPU_OFFSET,
				kbase_api_mem_find_cpu_offset,
				union kbase_ioctl_mem_find_cpu_offset);
		KBASE_HANDLE_IOCTL_OUT(KBASE_IOCTL_GET_CONTEXT_ID,
				kbase_api_get_context_id,
				struct kbase_ioctl_get_context_id);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_TLSTREAM_ACQUIRE,
				kbase_api_tlstream_acquire,
				struct kbase_ioctl_tlstream_acquire);
		KBASE_HANDLE_IOCTL(KBASE_IOCTL_TLSTREAM_FLUSH,
				kbase_api_tlstream_flush);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_COMMIT,
				kbase_api_mem_commit,
				struct kbase_ioctl_mem_commit);
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_MEM_ALIAS,
				kbase_api_mem_alias,
				union kbase_ioctl_mem_alias);
		KBASE_HANDLE_IOCTL_INOUT(KBASE_IOCTL_MEM_IMPORT,
				kbase_api_mem_import,
				union kbase_ioctl_mem_import);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_FLAGS_CHANGE,
				kbase_api_mem_flags_change,
				struct kbase_ioctl_mem_flags_change);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_STREAM_CREATE,
				kbase_api_stream_create,
				struct kbase_ioctl_stream_create);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_FENCE_VALIDATE,
				kbase_api_fence_validate,
				struct kbase_ioctl_fence_validate);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_GET_PROFILING_CONTROLS,
				kbase_api_get_profiling_controls,
				struct kbase_ioctl_get_profiling_controls);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_MEM_PROFILE_ADD,
				kbase_api_mem_profile_add,
				struct kbase_ioctl_mem_profile_add);
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_SOFT_EVENT_UPDATE,
				kbase_api_soft_event_update,
				struct kbase_ioctl_soft_event_update);

#if MALI_UNIT_TEST
		KBASE_HANDLE_IOCTL_IN(KBASE_IOCTL_TLSTREAM_TEST,
				kbase_api_tlstream_test,
				struct kbase_ioctl_tlstream_test);
		KBASE_HANDLE_IOCTL_OUT(KBASE_IOCTL_TLSTREAM_STATS,
				kbase_api_tlstream_stats,
				struct kbase_ioctl_tlstream_stats);
#endif
	}

	dev_warn(kbdev->dev, "Unknown ioctl 0x%x nr:%d", cmd, _IOC_NR(cmd));

	return -ENOIOCTLCMD;
}

static ssize_t kbase_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct kbase_context *kctx = filp->private_data;
	struct base_jd_event_v2 uevent;
	int out_count = 0;

	if (count < sizeof(uevent))
		return -ENOBUFS;

	do {
		while (kbase_event_dequeue(kctx, &uevent)) {
			if (out_count > 0)
				goto out;

			if (filp->f_flags & O_NONBLOCK)
				return -EAGAIN;

			if (wait_event_interruptible(kctx->event_queue,
					kbase_event_pending(kctx)) != 0)
				return -ERESTARTSYS;
		}
		if (uevent.event_code == BASE_JD_EVENT_DRV_TERMINATED) {
			if (out_count == 0)
				return -EPIPE;
			goto out;
		}

		if (copy_to_user(buf, &uevent, sizeof(uevent)) != 0)
			return -EFAULT;

		buf += sizeof(uevent);
		out_count++;
		count -= sizeof(uevent);
	} while (count >= sizeof(uevent));

 out:
	return out_count * sizeof(uevent);
}

static unsigned int kbase_poll(struct file *filp, poll_table *wait)
{
	struct kbase_context *kctx = filp->private_data;

	poll_wait(filp, &kctx->event_queue, wait);
	if (kbase_event_pending(kctx))
		return POLLIN | POLLRDNORM;

	return 0;
}

void kbase_event_wakeup(struct kbase_context *kctx)
{
	KBASE_DEBUG_ASSERT(kctx);

	wake_up_interruptible(&kctx->event_queue);
}

KBASE_EXPORT_TEST_API(kbase_event_wakeup);

static int kbase_check_flags(int flags)
{
	/* Enforce that the driver keeps the O_CLOEXEC flag so that execve() always
	 * closes the file descriptor in a child process.
	 */
	if (0 == (flags & O_CLOEXEC))
		return -EINVAL;

	return 0;
}


/**
 * align_and_check - Align the specified pointer to the provided alignment and
 *                   check that it is still in range.
 * @gap_end:        Highest possible start address for allocation (end of gap in
 *                  address space)
 * @gap_start:      Start address of current memory area / gap in address space
 * @info:           vm_unmapped_area_info structure passed to caller, containing
 *                  alignment, length and limits for the allocation
 * @is_shader_code: True if the allocation is for shader code (which has
 *                  additional alignment requirements)
 *
 * Return: true if gap_end is now aligned correctly and is still in range,
 *         false otherwise
 */
static bool align_and_check(unsigned long *gap_end, unsigned long gap_start,
		struct vm_unmapped_area_info *info, bool is_shader_code)
{
	/* Compute highest gap address at the desired alignment */
	(*gap_end) -= info->length;
	(*gap_end) -= (*gap_end - info->align_offset) & info->align_mask;

	if (is_shader_code) {
		/* Check for 4GB boundary */
		if (0 == (*gap_end & BASE_MEM_MASK_4GB))
			(*gap_end) -= (info->align_offset ? info->align_offset :
					info->length);
		if (0 == ((*gap_end + info->length) & BASE_MEM_MASK_4GB))
			(*gap_end) -= (info->align_offset ? info->align_offset :
					info->length);

		if (!(*gap_end & BASE_MEM_MASK_4GB) || !((*gap_end +
				info->length) & BASE_MEM_MASK_4GB))
			return false;
	}


	if ((*gap_end < info->low_limit) || (*gap_end < gap_start))
		return false;


	return true;
}

/* The following function is taken from the kernel and just
 * renamed. As it's not exported to modules we must copy-paste it here.
 */

static unsigned long kbase_unmapped_area_topdown(struct vm_unmapped_area_info
		*info, bool is_shader_code)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long length, low_limit, high_limit, gap_start, gap_end;

	/* Adjust search length to account for worst case alignment overhead */
	length = info->length + info->align_mask;
	if (length < info->length)
		return -ENOMEM;

	/*
	 * Adjust search limits by the desired length.
	 * See implementation comment at top of unmapped_area().
	 */
	gap_end = info->high_limit;
	if (gap_end < length)
		return -ENOMEM;
	high_limit = gap_end - length;

	if (info->low_limit > high_limit)
		return -ENOMEM;
	low_limit = info->low_limit + length;

	/* Check highest gap, which does not precede any rbtree node */
	gap_start = mm->highest_vm_end;
	if (gap_start <= high_limit) {
		if (align_and_check(&gap_end, gap_start, info, is_shader_code))
			return gap_end;
	}

	/* Check if rbtree root looks promising */
	if (RB_EMPTY_ROOT(&mm->mm_rb))
		return -ENOMEM;
	vma = rb_entry(mm->mm_rb.rb_node, struct vm_area_struct, vm_rb);
	if (vma->rb_subtree_gap < length)
		return -ENOMEM;

	while (true) {
		/* Visit right subtree if it looks promising */
		gap_start = vma->vm_prev ? vma->vm_prev->vm_end : 0;
		if (gap_start <= high_limit && vma->vm_rb.rb_right) {
			struct vm_area_struct *right =
				rb_entry(vma->vm_rb.rb_right,
					 struct vm_area_struct, vm_rb);
			if (right->rb_subtree_gap >= length) {
				vma = right;
				continue;
			}
		}

check_current:
		/* Check if current node has a suitable gap */
		gap_end = vma->vm_start;
		if (gap_end < low_limit)
			return -ENOMEM;
		if (gap_start <= high_limit && gap_end - gap_start >= length) {
			/* We found a suitable gap. Clip it with the original
			 * high_limit. */
