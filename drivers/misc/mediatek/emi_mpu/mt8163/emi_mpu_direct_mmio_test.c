/*
 * emi_mpu_direct_mmio_test.c — Test kernel module for direct MMIO writes
 * to EMI MPU registers, bypassing SMC calls to TEE.
 *
 * PURPOSE:
 *   Verify whether the EMI MPU hardware on MT8163 accepts direct MMIO
 *   writes from the non-secure kernel, or if only the SMC→TEE path
 *   is allowed by the hardware itself.
 *
 * BACKGROUND:
 *   The original code used emi_mpu_smc_write() which sends SMC #0
 *   (function 0x82000207) to ATF/TEE. The TEE silently rejects MPU
 *   region changes for consys (WiFi/BT) memory. However, emi_mpu.c
 *   already has EMI_BASE_ADDR ioremaped at physical 0x10203000 and
 *   the emi_reg_write() function was already patched to use
 *   mt_reg_sync_writel() (direct __raw_writel + mb()).
 *
 *   The separate emi_reg_rw.c module has mt_emi_reg_write() which
 *   still calls emi_mpu_smc_write() via SMC — but NO code in the
 *   kernel actually calls mt_emi_reg_write(). It's dead code.
 *
 * HARDWARE PROTECTION ANALYSIS:
 *   1. Device APC (DAPC): EMI_BUS_INTERFACE (device #25) is NOT marked
 *      forbidden for AP domain. The AP can read/write EMI registers.
 *   2. TZPC/TZASC: No evidence of TrustZone Protection Controller
 *      locking on the EMI register range in this codebase.
 *   3. The EMI MPU registers control DRAM access protection — they
 *      are infrastructure registers, not themselves behind MPU.
 *
 *   CONCLUSION: Direct MMIO writes should succeed. The SMC rejection
 *   is a TEE software policy decision, not a hardware block. The
 *   hardware Device APC allows AP domain access to EMI registers.
 *
 * WHAT THIS PATCH DOES:
 *   Clears EMI MPU region 15 (used for AP memory protection) using
 *   direct writel() to EMI_MPUH2, EMI_MPUL2, and EMI_MPUL2_2ND,
 *   then reads back to verify the writes took effect.
 *
 * Register layout for region 15:
 *   EMI_MPUH2   (base+0x298) = start_addr[31:16] | end_addr[15:0]
 *   EMI_MPUL2   (base+0x2B8) = permissions upper half for region 15
 *   EMI_MPUL2_2ND (base+0x2BC) = permissions upper half 2nd bank
 *
 * Region 15 shares EMI_MPUL2/EMI_MPUL2_2ND with region 14:
 *   EMI_MPUL2[15:0]    = region 14 permissions (d0-d3)
 *   EMI_MPUL2[31:16]   = region 15 permissions (d0-d3)
 *   EMI_MPUL2_2ND[15:0]  = region 14 permissions (d4-d7)
 *   EMI_MPUL2_2ND[31:16] = region 15 permissions (d4-d7)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/delay.h>

#define EMI_REG_BASE_PHYS   0x10203000
#define EMI_REG_SIZE        0x1000

/* MPU register offsets (relative to EMI base) */
#define EMI_MPUH2_OFF       0x0298   /* Region 15 start/end address */
#define EMI_MPUL2_OFF       0x02B8   /* Region 14+15 permissions (lower) */
#define EMI_MPUL2_2ND_OFF   0x02BC   /* Region 14+15 permissions (upper) */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EMI MPU Direct MMIO Test");
MODULE_DESCRIPTION("Test direct MMIO writes to EMI MPU registers on MT8163");

static void __iomem *emi_base;
static bool own_mapping = false;

static int __init emi_mpu_test_init(void)
{
    u32 val_before, val_after;
    u32 mpul2_before, mpul2_after;
    u32 mpul2_2nd_before, mpul2_2nd_after;
    struct device_node *node;

    pr_info("[EMI_MPU_TEST] === Direct MMIO write test starting ===\n");

    /* Get EMI base address — try to reuse existing mapping */
    node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-emi");
    if (node) {
        emi_base = of_iomap(node, 0);
        if (!emi_base) {
            pr_err("[EMI_MPU_TEST] of_iomap failed, trying ioremap\n");
            emi_base = ioremap(EMI_REG_BASE_PHYS, EMI_REG_SIZE);
            if (!emi_base) {
                pr_err("[EMI_MPU_TEST] ioremap failed\n");
                return -ENOMEM;
            }
            own_mapping = true;
        }
    } else {
        pr_warn("[EMI_MPU_TEST] DT node not found, using ioremap\n");
        emi_base = ioremap(EMI_REG_BASE_PHYS, EMI_REG_SIZE);
        if (!emi_base) {
            pr_err("[EMI_MPU_TEST] ioremap failed\n");
            return -ENOMEM;
        }
        own_mapping = true;
    }

    pr_info("[EMI_MPU_TEST] EMI base mapped at %p (phys 0x%x)\n",
            emi_base, EMI_REG_BASE_PHYS);

    /* Step 1: Read current values */
    val_before = readl(emi_base + EMI_MPUH2_OFF);
    mpul2_before = readl(emi_base + EMI_MPUL2_OFF);
    mpul2_2nd_before = readl(emi_base + EMI_MPUL2_2ND_OFF);

    pr_info("[EMI_MPU_TEST] BEFORE:\n");
    pr_info("  EMI_MPUH2     = 0x%08x (region 15 addr range)\n", val_before);
    pr_info("  EMI_MPUL2     = 0x%08x (region 14+15 perms low)\n", mpul2_before);
    pr_info("  EMI_MPUL2_2ND = 0x%08x (region 14+15 perms high)\n", mpul2_2nd_before);
    pr_info("  Region 15 perms (d0-d3): 0x%04x\n", (mpul2_before >> 16) & 0xFFFF);
    pr_info("  Region 15 perms (d4-d7): 0x%04x\n", (mpul2_2nd_before >> 16) & 0xFFFF);

    /* Step 2: Test read — if reads return 0xFFFFFFFF or all-zero, the
     * hardware is blocking even reads from non-secure world */
    if (val_before == 0xFFFFFFFF) {
        pr_err("[EMI_MPU_TEST] FAIL: Read returns 0xFFFFFFFF — hardware blocks non-secure reads\n");
        goto cleanup;
    }

    /* Step 3: Clear region 15 address range (write 0 to EMI_MPUH2) */
    pr_info("[EMI_MPU_TEST] Writing 0x00000000 to EMI_MPUH2 (clear region 15 addr)...\n");
    __raw_writel(0x00000000, emi_base + EMI_MPUH2_OFF);
    mb();
    udelay(10);

    val_after = readl(emi_base + EMI_MPUH2_OFF);
    pr_info("[EMI_MPU_TEST] EMI_MPUH2 after write: 0x%08x (expected 0x00000000)\n", val_after);

    if (val_after == 0x00000000) {
        pr_info("[EMI_MPU_TEST] SUCCESS: Direct MMIO write to EMI_MPUH2 WORKED!\n");
    } else if (val_after == val_before) {
        pr_err("[EMI_MPU_TEST] FAIL: Write was IGNORED — value unchanged (0x%08x)\n", val_after);
        pr_err("[EMI_MPU_TEST] Hardware blocks non-secure MMIO writes to MPU registers\n");
    } else {
        pr_warn("[EMI_MPU_TEST] UNEXPECTED: Value changed to 0x%08x (neither old nor expected)\n", val_after);
    }

    /* Step 4: Clear region 15 permissions (upper half of MPUL2/MPUL2_2ND)
     * Must preserve region 14 permissions in lower half */
    {
        u32 mpul2_cleared, mpul2_2nd_cleared;

        /* Preserve region 14 (lower 16 bits), clear region 15 (upper 16 bits) */
        mpul2_cleared = mpul2_before & 0x0000FFFF;
        mpul2_2nd_cleared = mpul2_2nd_before & 0x0000FFFF;

        pr_info("[EMI_MPU_TEST] Clearing region 15 permissions...\n");
        pr_info("  Writing EMI_MPUL2     = 0x%08x (preserve r14 lower)\n", mpul2_cleared);
        pr_info("  Writing EMI_MPUL2_2ND = 0x%08x (preserve r14 upper)\n", mpul2_2nd_cleared);

        __raw_writel(mpul2_cleared, emi_base + EMI_MPUL2_OFF);
        mb();
        __raw_writel(mpul2_2nd_cleared, emi_base + EMI_MPUL2_2ND_OFF);
        mb();
        udelay(10);

        mpul2_after = readl(emi_base + EMI_MPUL2_OFF);
        mpul2_2nd_after = readl(emi_base + EMI_MPUL2_2ND_OFF);

        pr_info("[EMI_MPU_TEST] AFTER permission clear:\n");
        pr_info("  EMI_MPUL2     = 0x%08x (expected 0x%08x)\n", mpul2_after, mpul2_cleared);
        pr_info("  EMI_MPUL2_2ND = 0x%08x (expected 0x%08x)\n", mpul2_2nd_after, mpul2_2nd_cleared);

        if ((mpul2_after & 0xFFFF0000) == 0 && (mpul2_2nd_after & 0xFFFF0000) == 0) {
            pr_info("[EMI_MPU_TEST] SUCCESS: Region 15 permissions cleared via direct MMIO!\n");
        } else {
            pr_err("[EMI_MPU_TEST] FAIL: Region 15 permissions NOT cleared\n");
        }
    }

    /* Step 5: Summary */
    pr_info("[EMI_MPU_TEST] === Final register state ===\n");
    pr_info("  EMI_MPUH2     = 0x%08x\n", readl(emi_base + EMI_MPUH2_OFF));
    pr_info("  EMI_MPUL2     = 0x%08x\n", readl(emi_base + EMI_MPUL2_OFF));
    pr_info("  EMI_MPUL2_2ND = 0x%08x\n", readl(emi_base + EMI_MPUL2_2ND_OFF));
    pr_info("[EMI_MPU_TEST] === Test complete ===\n");

cleanup:
    /* Note: We don't iounmap here because EMI_BASE_ADDR is shared with
     * the emi_mpu driver. If we created our own mapping, clean it up. */
    if (own_mapping && emi_base) {
        iounmap(emi_base);
        emi_base = NULL;
    }

    return 0;  /* Always return 0 so module loads and prints results */
}

static void __exit emi_mpu_test_exit(void)
{
    pr_info("[EMI_MPU_TEST] Module unloaded\n");
}

module_init(emi_mpu_test_init);
module_exit(emi_mpu_test_exit);
