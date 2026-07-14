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

    def test_cleanup_stage_range_is_dedicated_and_not_old_ap_range(self):
        self.assertIn("#define ECHO_FW_CLEANUP_ENTER 0xD0", self.gl_kal)
        self.assertIn("#define ECHO_FW_CLEANUP_AFTER_CLOSE 0xDC", self.gl_kal)
        self.assertIn("#define ECHO_FW_CLEANUP_RETURN 0xDD", self.gl_kal)
        self.assertNotIn("0xD0", self.gl_init)

    def test_image_unmapping_has_one_shot_operation_boundaries(self):
        start = self.gl_kal.index("VOID kalFirmwareImageUnmapping")
        end = self.gl_kal.index("#endif", start)
        fn = self.gl_kal[start:end]
        for marker in (
            "ECHO_FW_CLEANUP_ENTER",
            "ECHO_FW_CLEANUP_BEFORE_VFREE",
            "ECHO_FW_CLEANUP_AFTER_VFREE",
            "ECHO_FW_CLEANUP_BEFORE_CLOSE",
            "ECHO_FW_CLEANUP_AFTER_CLOSE",
            "ECHO_FW_CLEANUP_RETURN",
        ):
            self.assertIn(marker, fn)
        self.assertLess(fn.index("before-vfree"), fn.index("after-vfree"))
        self.assertLess(fn.index("after-vfree"), fn.index("before-close"))
        self.assertLess(fn.index("before-close"), fn.index("after-close"))

    def test_close_has_file_and_task_state_boundaries(self):
        start = self.gl_kal.index("WLAN_STATUS kalFirmwareClose")
        end = self.gl_kal.index("/*----------------------------------------------------------------------------*/", start + 30)
        fn = self.gl_kal[start:end]
        for marker in (
            "ECHO_FW_CLEANUP_CLOSE_ENTER",
            "ECHO_FW_CLEANUP_BEFORE_FILP_CLOSE",
            "ECHO_FW_CLEANUP_AFTER_FILP_CLOSE",
            "ECHO_FW_CLEANUP_BEFORE_FS_RESTORE",
            "ECHO_FW_CLEANUP_AFTER_FS_RESTORE",
            "ECHO_FW_CLEANUP_BEFORE_CRED_RESTORE",
            "ECHO_FW_CLEANUP_AFTER_CRED_RESTORE",
            "ECHO_FW_CLEANUP_CLOSE_RETURN",
        ):
            self.assertIn(marker, fn)
        self.assertLess(fn.index("before-filp-close"), fn.index("after-filp-close"))
        self.assertLess(fn.index("after-filp-close"), fn.index("before-fs-restore"))
        self.assertLess(fn.index("after-fs-restore"), fn.index("before-cred-restore"))

    def test_open_and_close_log_same_task_identity_fields(self):
        open_start = self.gl_kal.index("WLAN_STATUS kalFirmwareOpen")
        close_start = self.gl_kal.index("WLAN_STATUS kalFirmwareClose")
        self.assertIn('echoFirmwareTaskMarker("OPEN")', self.gl_kal[open_start:close_start])
        self.assertIn("ECHO_FW_CLEANUP_CLOSE_ENTER", self.gl_kal[close_start:])
        helper_start = self.gl_kal.index("static VOID echoFirmwareTaskMarker")
        helper_end = self.gl_kal.index("/*----------------------------------------------------------------------------*/", helper_start)
        for text in (self.gl_kal[helper_start:helper_end],):
            self.assertIn("current->pid", text)
            self.assertIn("current->comm", text)

    def test_stage_142_is_persisted_after_unmapping(self):
        start = self.gl_init.index("kalFirmwareImageUnmapping(prGlueInfo, NULL, prFwBuffer)")
        end = self.gl_init.index("bailout:", start)
        section = self.gl_init[start:end]
        self.assertIn("ECHO_WLAN_PERSIST_CLEANUP_DONE", self.gl_init)
        self.assertIn("aee_rr_rec_fiq_step(ECHO_WLAN_PERSIST_CLEANUP_DONE)", section)


if __name__ == "__main__":
    unittest.main()
