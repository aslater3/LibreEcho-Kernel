import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "drivers/misc/mediatek/include/mt-plat/echo_assert_unwind.h").read_text()
CONFIG = (ROOT / ".config").read_text()


class V174BtThenWifiContract(unittest.TestCase):
    def test_combo_config_is_reenabled(self):
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_BT=y$")
        self.assertRegex(CONFIG, r"(?m)^CONFIG_MTK_COMBO_WIFI=y$")

    def test_transition_markers_are_non_colliding(self):
        values = re.findall(r"#define ECHO_BT_WIFI_TRANSITION_(7[0-3]) 0x([0-9A-F]+)", HEADER)
        self.assertEqual(values, [("70", "70"), ("71", "71"), ("72", "72"), ("73", "73")])
        self.assertEqual(len({value for _, value in values}), 4)


if __name__ == "__main__":
    unittest.main()
