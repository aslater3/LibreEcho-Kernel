import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "drivers/misc/mediatek/include/mt-plat/echo_assert_unwind.h").read_text()
FUNC = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c").read_text()
LIB = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c").read_text()
CONFIG = (ROOT / ".config").read_text()


class V173BtInnerContract(unittest.TestCase):
    def test_bt_enabled_wifi_disabled(self):
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_BT=y$")
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_WIFI=n$")

    def test_inner_marker_values(self):
        values = re.findall(r"#define ECHO_BT_INNER_B[4-9A-B] 0x([0-9A-F]+)", HEADER)
        self.assertEqual(values, ["64", "65", "66", "67", "68", "69", "6A", "6B"])

    def test_outer_bt_wrapper_markers(self):
        start = FUNC.index("INT32 wmt_func_bt_on(P_WMT_IC_OPS pOps, P_WMT_GEN_CONF pConf)\n{")
        body = FUNC[start:FUNC.index("\n}", start)]
        for marker in (
            "ECHO_BT_INNER_B4",
            "ECHO_BT_INNER_B5",
            "ECHO_BT_INNER_BB",
        ):
            self.assertIn(marker, body)
        self.assertLess(body.index("ECHO_BT_INNER_B5"), body.index("wmt_core_func_ctrl_cmd"))
        self.assertLess(body.index("wmt_core_func_ctrl_cmd"), body.index("ECHO_BT_INNER_BB"))

    def test_queue_and_wait_markers_are_bt_func_on_only(self):
        start = LIB.index("MTK_WCN_BOOL wmt_lib_put_act_op")
        body = LIB[start:LIB.index("\n}", start)]
        for marker in (
            "ECHO_BT_INNER_B6",
            "ECHO_BT_INNER_B7",
            "ECHO_BT_INNER_B8",
            "ECHO_BT_INNER_B9",
            "ECHO_BT_INNER_BA",
        ):
            self.assertIn(marker, body)
        self.assertLess(body.index("ECHO_BT_INNER_B6"), body.index("wmt_lib_put_op(&pWmtDev->rActiveOpQ"))
        self.assertLess(body.index("wmt_lib_put_op(&pWmtDev->rActiveOpQ"), body.index("ECHO_BT_INNER_B7"))
        self.assertLess(body.index("ECHO_BT_INNER_B7"), body.index("ECHO_BT_INNER_B8"))
        self.assertLess(body.index("ECHO_BT_INNER_B8"), body.index("osal_wait_for_signal_timeout"))
        self.assertLess(body.index("osal_wait_for_signal_timeout"), body.index("ECHO_BT_INNER_B9"))
        self.assertLess(body.index("ECHO_BT_INNER_B9"), body.index("ECHO_BT_INNER_BA"))
        self.assertIn("WMTDRV_TYPE_BT == pOp->op.au4OpData[0]", body)


if __name__ == "__main__":
    unittest.main()
