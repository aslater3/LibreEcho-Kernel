#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STP = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c"
WLAN = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/common/wlan_lib.c"
NIC_TX = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/nic/nic_tx.c"


class V157DiagnosticShapeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stp = STP.read_text()
        cls.wlan = WLAN.read_text()
        cls.nic_tx = NIC_TX.read_text()

    def test_frame_capture_is_bounded(self):
        self.assertIn("#define ECHO_STP_FW_FRAME_LIMIT 4", self.stp)
        self.assertIn("#define ECHO_STP_FW_PAYLOAD_LIMIT 128", self.stp)
        self.assertIn("ECHO_STP_FW_FRAME", self.stp)

    def test_clearing_assert_state_resets_capture_budget(self):
        self.assertIn("static atomic_t echo_fw_assert_latched = ATOMIC_INIT(0)", self.stp)
        start = self.stp.index("INT32 mtk_wcn_stp_coredump_start_ctrl")
        end = self.stp.index("mtk_wcn_stp_coredump_start_get", start)
        control = self.stp[start:end]
        self.assertIn("atomic_set(&echo_fw_assert_latched, !!value)", control)
        self.assertIn("g_echo_fw_capture_active = MTK_WCN_BOOL_FALSE", control)
        self.assertIn("g_echo_fw_frame_count = 0", control)
        self.assertIn("g_echo_fw_payload_bytes = 0", control)
        get_start = self.stp.index("INT32 mtk_wcn_stp_coredump_start_get")
        get_end = self.stp.index("mtk_wcn_stp_set_wmt_last_close", get_start)
        self.assertIn("return echo_fw_asserted();", self.stp[get_start:get_end])

    def test_capture_runs_only_at_complete_frame_boundaries(self):
        crc_start = self.stp.index("if (stp_check_crc(")
        crc_end = self.stp.index("} else {", crc_start)
        self.assertIn("echo_stp_capture_frame", self.stp[crc_start:crc_end])
        fw_start = self.stp.rindex("\t\tcase MTKSTP_FW_MSG:")
        complete_start = self.stp.index("\t\t\tif (i >= remain_length) {", fw_start)
        complete_end = self.stp.index("\n\t\t\t} else", complete_start)
        self.assertIn("echo_stp_capture_frame", self.stp[complete_start:complete_end])

    def test_firmware_assert_marks_failure_without_starting_reset_worker(self):
        start = self.stp.rindex("/* Diagnostic mode: preserve controller state")
        end = self.stp.index("/*discard CRC */", start)
        normal = self.stp[start:end]
        self.assertIn("mtk_wcn_stp_coredump_start_ctrl(1)", normal)
        self.assertNotIn("stp_btm_notify_wmt_rst_wq", normal)
        self.assertNotIn("osal_dbg_assert_aee", normal)

    def test_start_command_bytes_are_logged_once(self):
        start = self.nic_tx.index("WLAN_STATUS nicTxInitCmd")
        end = self.nic_tx.index("WLAN_STATUS nicTxInitResetResource", start)
        self.assertIn("ECHO_FW_START_CMD", self.nic_tx[start:end])

    def test_ready_loop_aborts_on_complete_stp_firmware_assert_snapshot(self):
        start = self.wlan.index("/* 4 <5> check Wi-Fi FW asserts ready bit */")
        end = self.wlan.index("echoWlanCpuHistDump();", start)
        ready_loop = self.wlan[start:end]
        self.assertIn("echo_wlan_assert_snapshot_complete()", ready_loop)
        self.assertNotIn("echo_fw_asserted()", ready_loop)
        self.assertIn("ECHO_WLAN_FW_ASSERT", ready_loop)
        self.assertIn("u4Status = WLAN_STATUS_FAILURE", ready_loop)

    def test_assert_snapshot_contains_full_compact_register_set(self):
        start = self.wlan.index("static VOID echoWlanHifSnapshot")
        end = self.wlan.index("#endif", start)
        snapshot = self.wlan[start:end]
        self.assertIn("MCR_H2DSM1R", snapshot)
        self.assertIn("FWOWN=", snapshot)


if __name__ == "__main__":
    unittest.main()
