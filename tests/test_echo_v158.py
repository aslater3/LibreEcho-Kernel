#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GL_KAL = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_kal.c"
GL_INIT = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c"
FS_OPEN = ROOT / "fs/open.c"


class V158CleanupBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gl_kal = GL_KAL.read_text()
        cls.gl_init = GL_INIT.read_text()
        cls.fs_open = FS_OPEN.read_text()

    def test_cleanup_stage_range_is_dedicated(self):
        self.assertIn("ECHO_FW_CLEANUP_F0", self.gl_kal)
        self.assertIn("ECHO_FW_CLEANUP_F4", self.gl_kal)
        self.assertIn("ECHO_FW_CLEANUP_FD", self.gl_kal)
        self.assertIn("ECHO_FW_CLEANUP_FE", self.gl_kal)

    def test_image_unmapping_has_one_shot_operation_boundaries(self):
        start = self.gl_kal.index("VOID kalFirmwareImageUnmapping")
        end = self.gl_kal.index("\n}\n", start)
        fn = self.gl_kal[start:end]
        for marker in ("ECHO_FW_CLEANUP_F0", "ECHO_FW_CLEANUP_F1", "ECHO_FW_CLEANUP_F2", "ECHO_FW_CLEANUP_F3", "ECHO_FW_CLEANUP_FE"):
            self.assertIn(marker, fn)
        self.assertLess(fn.index("cleanup-entry"), fn.index("before-firmware-unmap"))
        self.assertLess(fn.index("after-firmware-unmap"), fn.index("before-kalFirmwareClose"))
        self.assertLess(fn.index("before-kalFirmwareClose"), fn.index("cleanup-returned"))

    def test_close_has_file_and_credential_boundaries(self):
        start = self.gl_kal.index("WLAN_STATUS kalFirmwareClose")
        end = self.gl_kal.index("/*----------------------------------------------------------------------------*/", start + 30)
        fn = self.gl_kal[start:end]
        for marker in ("ECHO_FW_CLEANUP_F4", "ECHO_FW_CLEANUP_F5", "ECHO_FW_CLEANUP_FC", "ECHO_FW_CLEANUP_FD"):
            self.assertIn(marker, fn)
        self.assertLess(fn.index("before-filp-close"), fn.index("filp-close-returned"))
        self.assertIn("override_creds", self.gl_kal)
        self.assertIn("revert_creds", self.gl_kal)
        self.assertIn("prepare_kernel_cred", self.gl_kal)
        self.assertNotIn("cred->fsuid.val =", self.gl_kal)
        self.assertNotIn("cred->fsgid.val =", self.gl_kal)

    def test_filp_close_keeps_target_identity_gate_and_order(self):
        start = self.fs_open.index("int filp_close(struct file *filp")
        end = self.fs_open.index("EXPORT_SYMBOL(filp_close)", start)
        fn = self.fs_open[start:end]
        self.assertIn("filp == READ_ONCE(echo_fw_close_target)", fn)
        markers = ("ECHO_FW_CLEANUP_F6", "ECHO_FW_CLEANUP_F7", "ECHO_FW_CLEANUP_F8", "ECHO_FW_CLEANUP_F9", "ECHO_FW_CLEANUP_FA", "ECHO_FW_CLEANUP_FB", "ECHO_FW_CLEANUP_FC")
        positions = [fn.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions))

    def test_cleanup_done_stage_is_persisted(self):
        self.assertIn("ECHO_WLAN_PERSIST_CLEANUP_DONE 0xDE", self.gl_init)
        self.assertIn("aee_rr_rec_fiq_step(ECHO_WLAN_PERSIST_CLEANUP_DONE)", self.gl_init)


if __name__ == "__main__":
    unittest.main()
