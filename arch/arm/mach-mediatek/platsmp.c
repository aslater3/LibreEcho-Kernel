/*
 * arch/arm/mach-mediatek/platsmp.c
 *
 * Copyright (c) 2014 Mediatek Inc.
 * Author: Shunli Wang <shunli.wang@mediatek.com>
 *         Yingjoe Chen <yingjoe.chen@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/threads.h>

#include <asm/memory.h>
#include <asm/psci.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>

#ifdef CONFIG_ARCH_MT8163
extern int spm_mtcmos_cpu_init(void);
extern int spm_mtcmos_ctrl_cpu(unsigned int cpu, int state,
					int chkWfiBeforePdn);
#ifdef CONFIG_HOTPLUG_CPU
extern void psci_cpu_die(unsigned int cpu);
extern int psci_cpu_kill(unsigned int cpu);
#endif

#define MT8163_CPU_POWER_DOWN	0
#define MT8163_CPU_POWER_ON	1

#ifdef CONFIG_HOTPLUG_CPU
static int mt8163_cpu_kill(unsigned int cpu)
{
	int ret;

	/*
	 * PSCI CPU_OFF stops execution, but on MT8163 the per-core power
	 * island must also be collapsed before a later CPU_ON is valid.
	 * The ARM64 MTK PSCI wrapper does this in its cpu_kill callback.
	 */
	ret = psci_cpu_kill(cpu);
	if (!ret)
		pr_warn("MT8163: CPU%u PSCI kill did not confirm shutdown\n", cpu);

	ret = spm_mtcmos_ctrl_cpu(cpu, MT8163_CPU_POWER_DOWN, 1);
	if (ret) {
		pr_err("MT8163: CPU%u MTCMOS power-down failed: %d\n", cpu, ret);
		return 0;
	}

	pr_info("MT8163: CPU%u MTCMOS power-down complete\n", cpu);
	return 1;
}
#endif
#endif

#define MTK_MAX_CPU		8
#define MTK_SMP_REG_SIZE	0x1000

struct mtk_smp_boot_info {
	unsigned long smp_base;
	unsigned int jump_reg;
	unsigned int core_keys[MTK_MAX_CPU - 1];
	unsigned int core_regs[MTK_MAX_CPU - 1];
};

static const struct mtk_smp_boot_info mtk_mt8135_tz_boot = {
	0x80002000, 0x3fc,
	{ 0x534c4131, 0x4c415332, 0x41534c33 },
	{ 0x3f8, 0x3f8, 0x3f8 },
};

static const struct mtk_smp_boot_info mtk_mt6589_boot = {
	0x10002000, 0x34,
	{ 0x534c4131, 0x4c415332, 0x41534c33 },
	{ 0x38, 0x3c, 0x40 },
};

static const struct of_device_id mtk_tz_smp_boot_infos[] __initconst = {
	{ .compatible   = "mediatek,mt8135", .data = &mtk_mt8135_tz_boot },
	{ .compatible   = "mediatek,mt8127", .data = &mtk_mt8135_tz_boot },
	{ .compatible   = "mediatek,mt2701", .data = &mtk_mt8135_tz_boot },
};

static const struct of_device_id mtk_smp_boot_infos[] __initconst = {
	{ .compatible   = "mediatek,mt6589", .data = &mtk_mt6589_boot },
};

static void __iomem *mtk_smp_base;
static const struct mtk_smp_boot_info *mtk_smp_info;

static int mtk_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	if (!mtk_smp_base)
		return -EINVAL;

	if (!mtk_smp_info->core_keys[cpu-1])
		return -EINVAL;

	writel_relaxed(mtk_smp_info->core_keys[cpu-1],
		mtk_smp_base + mtk_smp_info->core_regs[cpu-1]);

	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	return 0;
}

static void __init __mtk_smp_prepare_cpus(unsigned int max_cpus, int trustzone)
{
	int i, num;
	const struct of_device_id *infos;

	if (trustzone) {
		num = ARRAY_SIZE(mtk_tz_smp_boot_infos);
		infos = mtk_tz_smp_boot_infos;
	} else {
		num = ARRAY_SIZE(mtk_smp_boot_infos);
		infos = mtk_smp_boot_infos;
	}

	/* Find smp boot info for this SoC */
	for (i = 0; i < num; i++) {
		if (of_machine_is_compatible(infos[i].compatible)) {
			mtk_smp_info = infos[i].data;
			break;
		}
	}

	if (!mtk_smp_info) {
		pr_err("%s: Device is not supported\n", __func__);
		return;
	}

	if (trustzone) {
		if (memblock_reserve(mtk_smp_info->smp_base, MTK_SMP_REG_SIZE)) {
			pr_err("%s: Can't reserve smp memory\n", __func__);
			return;
		}
		mtk_smp_base = phys_to_virt(mtk_smp_info->smp_base);
	} else {
		mtk_smp_base = ioremap(mtk_smp_info->smp_base, MTK_SMP_REG_SIZE);
		if (!mtk_smp_base) {
			pr_err("%s: Can't remap %lx\n", __func__,
				mtk_smp_info->smp_base);
			return;
		}
	}

	/*
	 * write the address of slave startup address into the system-wide
	 * jump register
	 */
	writel_relaxed(virt_to_phys(secondary_startup_arm),
			mtk_smp_base + mtk_smp_info->jump_reg);
}

static void __init mtk_tz_smp_prepare_cpus(unsigned int max_cpus)
{
	__mtk_smp_prepare_cpus(max_cpus, 1);
}

static void __init mtk_smp_prepare_cpus(unsigned int max_cpus)
{
	__mtk_smp_prepare_cpus(max_cpus, 0);
}

static struct smp_operations mt81xx_tz_smp_ops __initdata = {
	.smp_prepare_cpus = mtk_tz_smp_prepare_cpus,
	.smp_boot_secondary = mtk_boot_secondary,
};
CPU_METHOD_OF_DECLARE(mt81xx_tz_smp, "mediatek,mt81xx-tz-smp", &mt81xx_tz_smp_ops);

static struct smp_operations mt6589_smp_ops __initdata = {
	.smp_prepare_cpus = mtk_smp_prepare_cpus,
	.smp_boot_secondary = mtk_boot_secondary,
};
CPU_METHOD_OF_DECLARE(mt6589_smp, "mediatek,mt6589-smp", &mt6589_smp_ops);

#ifdef CONFIG_ARCH_MT8163
static void __init mt8163_smp_prepare_cpus(unsigned int max_cpus)
{
	spm_mtcmos_cpu_init();
}

static int mt8163_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	int ret;

	if (!psci_ops.cpu_on)
		return -ENODEV;

	ret = psci_ops.cpu_on(cpu_logical_map(cpu), __pa(secondary_entry));
	if (ret < 0) {
		pr_err("PSCI failed to boot CPU%u: %d\n", cpu, ret);
		return ret;
	}

	return spm_mtcmos_ctrl_cpu(cpu, MT8163_CPU_POWER_ON, 1);
}

static struct smp_operations mt8163_smp_ops __initdata = {
	.smp_prepare_cpus = mt8163_smp_prepare_cpus,
	.smp_boot_secondary = mt8163_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die = psci_cpu_die,
	.cpu_kill = mt8163_cpu_kill,
#endif
};
#endif

bool __init mediatek_smp_init(void)
{
#ifdef CONFIG_ARCH_MT8163
	if (of_machine_is_compatible("mediatek,mt8163")) {
		smp_set_ops(&mt8163_smp_ops);
		pr_info("MT8163: using PSCI/MTCMOS SMP bring-up\n");
		return true;
	}
#endif

	return false;
}
