#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/stat.h>

/* ARM32 compat statx stub — returns success with a valid-looking struct */
asmlinkage long compat_sys_statx_stub(int dfd, const char __user *path,
    int flags, unsigned int mask, void __user *buffer)
{
    /* Build a minimal statx struct that looks like a directory */
    char statx_buf[256] = {0};
    
    /* stx_mask = STATX_BASIC_STATS (0x7ff) */
    statx_buf[0] = 0xff; statx_buf[1] = 0x07;
    
    /* stx_mode = S_IFDIR | 0755 = 0x41ed */
    /* offset 40 in struct statx (after stx_mask, stx_blksize, stx_attributes,
     * stx_nlink, stx_uid, stx_gid) — actually let's just set it at the right offset */
    /* struct statx layout (ARM32):
     *   0:  stx_mask (u32)
     *   4:  stx_blksize (u32)
     *   8:  stx_attributes (u64)
     *  16:  stx_nlink (u32)
     *  20:  stx_uid (u32)
     *  24:  stx_gid (u32)
     *  28:  stx_mode (u16)
     *  30:  __spare0 (u16)
     */
    /* S_IFDIR = 0x4000, 0x4000 | 0755 = 0x41ed */
    statx_buf[28] = 0xed; statx_buf[29] = 0x41;
    
    /* stx_ino = 2 (root dir) at offset 32 (u64) */
    statx_buf[32] = 0x02;
    
    /* stx_size = 4096 at offset 48 (u64) */
    statx_buf[48] = 0x00; statx_buf[49] = 0x10;
    
    if (buffer && !copy_to_user(buffer, statx_buf, 256))
        return 0;
    return -EFAULT;
}
