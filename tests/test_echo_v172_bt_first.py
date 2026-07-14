import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXP = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/wmt_exp.c").read_text()
HEADER = (ROOT / "drivers/misc/mediatek/include/mt-plat/echo_assert_unwind.h").read_text()
CONFIG = (ROOT / ".config").read_text()


class V172BtFirstContract(unittest.TestCase):
    def test_bt_config_enabled_wifi_remains_enabled(self):
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_BT=y$")
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_WIFI=y$")
        self.assertNotRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_BT=n$")

    def test_markers_are_outside_existing_assertion_range(self):
        self.assertEqual(
            re.findall(r"#define ECHO_BT_FIRST_B[0-3] 0x([0-9A-F]+)", HEADER),
            ["60", "61", "62", "63"],
        )
        for marker in ("E0", "E9", "F0", "FE"):
            self.assertNotIn(f"ECHO_BT_FIRST_B{marker}", HEADER)

    def test_wifi_wrapper_orders_bt_delay_wifi_and_records_result(self):
        start = EXP.index("MTK_WCN_BOOL mtk_wcn_wmt_func_on")
        body = EXP[EXP.index("\n{", start) + 2:EXP.index("\n}", start)]
        self.assertLess(body.index("ECHO_BT_FIRST_B0"), body.index("WMTDRV_TYPE_BT, WMT_OPID_FUNC_ON"))
        self.assertLess(body.index("WMTDRV_TYPE_BT, WMT_OPID_FUNC_ON"), body.index("ECHO_BT_FIRST_B1"))
        self.assertLess(body.index("ECHO_BT_FIRST_B1"), body.index("msleep(300)"))
        self.assertLess(body.index("msleep(300)"), body.index("ECHO_BT_FIRST_B2"))
        self.assertLess(body.index("ECHO_BT_FIRST_B2"), body.index("mtk_wcn_wmt_func_ctrl(type"))
        self.assertLess(body.index("mtk_wcn_wmt_func_ctrl(type"), body.index("ECHO_BT_FIRST_B3"))
        self.assertIn("BT-first FUNC_ON failed; refusing WIFI FUNC_ON", body)


if __name__ == "__main__":
    unittest.main()
