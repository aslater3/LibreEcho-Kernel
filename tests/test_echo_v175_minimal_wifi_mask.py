import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAM = (ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c").read_text()
RAM_HEADER = (ROOT / "drivers/misc/mediatek/include/mt-plat/mtk_ram_console.h").read_text()
FUNC = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c").read_text()
LIB = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c").read_text()
CONFIG = (ROOT / ".config").read_text()


class V175MinimalWifiBtMaskContract(unittest.TestCase):
    def test_normalized_wifi_config(self):
        for symbol in (
            "MTK_COMBO_BT",
            "MTK_COMBO_WIFI",
            "WIRELESS_EXT",
            "WEXT_PRIV",
            "MTK_WAPI_SUPPORT",
            "MTK_WIFI_MCC_SUPPORT",
        ):
            self.assertRegex(CONFIG, rf"(?m)^CONFIG_{symbol}=y$")

    def test_proc_exposes_cumulative_mask(self):
        self.assertIn("static atomic_t echo_bt_stage_mask = ATOMIC_INIT(0);", RAM)
        self.assertIn('"bt_stage_mask=0x%08x\\n"', RAM)
        self.assertIn("atomic_read(&echo_bt_stage_mask)", RAM)
        self.assertIn("extern void aee_rr_rec_bt_stage(u8 stage);", RAM_HEADER)
        self.assertIn("static inline void aee_rr_rec_bt_stage(u8 stage)", RAM_HEADER)

    def test_mask_maps_b4_through_bb_to_bits_zero_through_seven(self):
        start = RAM.index("void aee_rr_rec_bt_stage(u8 stage)")
        body = RAM[start:RAM.index("\n}", start)]
        self.assertIn("stage < 0x64 || stage > 0x6b", body)
        self.assertIn("atomic_or(1U << (stage - 0x64), &echo_bt_stage_mask);", body)
        self.assertNotIn("atomic_set", body)
        self.assertNotIn("atomic_and", body)

    def test_every_inner_fiq_marker_sets_the_matching_mask_bit(self):
        combined = FUNC + LIB
        for suffix in ("B4", "B5", "B6", "B7", "B8", "B9", "BA", "BB"):
            marker = f"ECHO_BT_INNER_{suffix}"
            pair = re.compile(
                rf"aee_rr_rec_fiq_step\({marker}\);\s+"
                rf"aee_rr_rec_bt_stage\({marker}\);"
            )
            self.assertRegex(combined, pair)
            self.assertEqual(combined.count(f"aee_rr_rec_bt_stage({marker});"), 1)

    def test_mask_has_no_runtime_clear(self):
        self.assertEqual(RAM.count("echo_bt_stage_mask = ATOMIC_INIT(0)"), 1)
        self.assertEqual(len(re.findall(r"echo_bt_stage_mask\s*=", RAM)), 1)
        self.assertNotRegex(RAM, re.compile(r"atomic_set\s*\(\s*&echo_bt_stage_mask"))


if __name__ == "__main__":
    unittest.main()
