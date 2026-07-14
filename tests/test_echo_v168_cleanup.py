#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GL_KAL = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_kal.c"
FS_OPEN = ROOT / "fs/open.c"
HEADER = ROOT / "drivers/misc/mediatek/include/mt-plat/echo_assert_unwind.h"


class V168CleanupLocalizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gl_kal = GL_KAL.read_text()
        cls.fs_open = FS_OPEN.read_text()
        cls.header = HEADER.read_text()

    def test_f0_to_fe_contract_is_defined(self):
        for marker in ("F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "FA", "FB", "FC", "FD", "FE"):
            self.assertIn(f"ECHO_FW_CLEANUP_{marker}", self.header)

    def test_cleanup_outer_order(self):
        start = self.gl_kal.index("VOID kalFirmwareImageUnmapping")
        end = self.gl_kal.index("#endif", start)
        fn = self.gl_kal[start:end]
        order = [
            "ECHO_FW_CLEANUP_F0", "ECHO_FW_CLEANUP_F1", "ECHO_FW_CLEANUP_F2",
            "ECHO_FW_CLEANUP_F3", "ECHO_FW_CLEANUP_FE",
        ]
        positions = [fn.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions))

    def test_close_order_and_credential_restore(self):
        start = self.gl_kal.index("WLAN_STATUS kalFirmwareClose")
        end = self.gl_kal.index("/*----------------------------------------------------------------------------*/", start + 30)
        fn = self.gl_kal[start:end]
        for marker in ("ECHO_FW_CLEANUP_F4", "ECHO_FW_CLEANUP_F5", "ECHO_FW_CLEANUP_FC", "ECHO_FW_CLEANUP_FD"):
            self.assertIn(marker, fn)
        self.assertIn("override_creds", self.gl_kal)
        self.assertIn("revert_creds", self.gl_kal)
        self.assertIn("prepare_kernel_cred", self.gl_kal)
        self.assertNotIn("(struct cred *)get_current_cred()", self.gl_kal)
        self.assertNotIn("cred->fsuid.val =", self.gl_kal)
        self.assertNotIn("cred->fsgid.val =", self.gl_kal)

    def test_filp_close_keeps_target_identity_gate_and_order(self):
        start = self.fs_open.index("int filp_close(struct file *filp")
        end = self.fs_open.index("EXPORT_SYMBOL(filp_close)", start)
        fn = self.fs_open[start:end]
        self.assertIn("filp == READ_ONCE(echo_fw_close_target)", fn)
        for marker in ("ECHO_FW_CLEANUP_F6", "ECHO_FW_CLEANUP_F7", "ECHO_FW_CLEANUP_F8", "ECHO_FW_CLEANUP_F9", "ECHO_FW_CLEANUP_FA", "ECHO_FW_CLEANUP_FB", "ECHO_FW_CLEANUP_FC"):
            self.assertIn(marker, fn)
        positions = [fn.index(marker) for marker in ("ECHO_FW_CLEANUP_F6", "ECHO_FW_CLEANUP_F7", "ECHO_FW_CLEANUP_F8", "ECHO_FW_CLEANUP_F9", "ECHO_FW_CLEANUP_FA", "ECHO_FW_CLEANUP_FB", "ECHO_FW_CLEANUP_FC")]
        self.assertEqual(positions, sorted(positions))

    def test_no_uart_cleanup_marker(self):
        start = self.gl_kal.index("static VOID echoFirmwareCleanupMarker")
        end = self.gl_kal.index("/*----------------------------------------------------------------------------*/", start)
        self.assertNotIn("pr_err", self.gl_kal[start:end])

    def test_assertion_unwind_contract_is_unchanged(self):
        for path, markers in [
            (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c", ("ECHO_ASSERT_UNWIND_E0", "ECHO_ASSERT_UNWIND_E1", "ECHO_ASSERT_UNWIND_E2", "ECHO_ASSERT_UNWIND_E3")),
            (ROOT / "drivers/misc/mediatek/btif/common/mtk_btif.c", ("ECHO_ASSERT_UNWIND_E4", "ECHO_ASSERT_UNWIND_E5")),
            (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/common/wlan_lib.c", ("ECHO_ASSERT_UNWIND_E6", "ECHO_ASSERT_UNWIND_E7", "ECHO_ASSERT_UNWIND_E8", "ECHO_ASSERT_UNWIND_E9")),
        ]:
            text = path.read_text()
            for marker in markers:
                self.assertIn(marker, text)


if __name__ == "__main__":
    unittest.main()
