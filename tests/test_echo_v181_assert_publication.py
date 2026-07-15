import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STP_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c"
WLAN_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/common/wlan_lib.c"
BTIF_PATH = ROOT / "drivers/misc/mediatek/btif/common/mtk_btif.c"
STP = STP_PATH.read_text()
WLAN = WLAN_PATH.read_text()
BTIF = BTIF_PATH.read_text()


def body_between(source: str, start_marker: str, end_marker: str) -> str:
    start = source.index(start_marker)
    return source[start : source.index(end_marker, start)]


class V181AssertionPublicationContract(unittest.TestCase):
    def test_generic_predicate_preserves_the_legacy_coredump_latch(self):
        predicate = body_between(
            STP,
            "bool echo_fw_asserted(void)",
            "EXPORT_SYMBOL(echo_fw_asserted);",
        )
        self.assertIn("atomic_read(&echo_fw_assert_latched) != 0", predicate)
        self.assertNotIn("echo_wlan_assert_snapshot_state", predicate)

    def test_wlan_predicate_requires_a_complete_published_snapshot(self):
        predicate = body_between(
            STP,
            "bool echo_wlan_assert_snapshot_complete(void)",
            "EXPORT_SYMBOL(echo_wlan_assert_snapshot_complete);",
        )
        self.assertIn(
            "atomic_read(&echo_wlan_assert_snapshot_state) == 1",
            predicate,
        )
        self.assertNotIn("echo_fw_assert_latched", predicate)

    def test_generic_coredump_control_and_get_contract_is_preserved(self):
        control = body_between(
            STP,
            "INT32 mtk_wcn_stp_coredump_start_ctrl(UINT32 value)",
            "INT32 mtk_wcn_stp_coredump_start_get",
        )
        getter = body_between(
            STP,
            "INT32 mtk_wcn_stp_coredump_start_get(VOID)",
            "/* mtk_wcn_stp_set_wmt_last_close",
        )
        self.assertIn("atomic_set(&echo_fw_assert_latched, !!value)", control)
        self.assertIn("return echo_fw_asserted();", getter)
        self.assertNotIn("echo_wlan_assert_snapshot_state", control)
        self.assertNotIn("echo_capture_wlan_assert", control)

    def test_complete_snapshot_is_published_before_wlan_can_observe_it(self):
        capture = body_between(
            STP,
            "static VOID echo_capture_wlan_assert(",
            "static int echo_wlan_assert_proc_show",
        )
        publish = capture.index("atomic_set(&echo_wlan_assert_snapshot_state, 1)")
        latch = capture.index("atomic_set(&echo_fw_assert_latched, 1)")
        self.assertLess(publish, latch)
        self.assertIn("smp_wmb();", capture[:publish])

    def test_only_wlan_ready_path_uses_snapshot_specific_predicate(self):
        ready = body_between(
            WLAN,
            "\t\twhile (1) {",
            "\t\t/* OID timeout timer initialize */",
        )
        self.assertEqual(3, ready.count("echo_wlan_assert_snapshot_complete()"))
        self.assertNotIn("echo_fw_asserted()", ready)
        self.assertIn("aborting startup", ready)
        self.assertIn("echo_fw_asserted()", BTIF)


if __name__ == "__main__":
    unittest.main()
