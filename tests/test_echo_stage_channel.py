import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAM = (ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c").read_text()
GL = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_kal.c").read_text()
OPEN = (ROOT / "fs/open.c").read_text()


class EchoStageChannelContract(unittest.TestCase):
    def test_proc_exposes_vendor_fiq_and_wdt_fields(self):
        self.assertIn("LAST_RR_VAL(fiq_step)", RAM)
        self.assertIn("LAST_RRR_VAL(fiq_step)", RAM)
        self.assertIn("LAST_RRPL_VAL(wdt_status)", RAM)
        self.assertIn("raw_fiq_step=0x%08x previous=0x%08x", RAM)
        self.assertIn("raw_wdt_status=0x%08x previous_wdt=0x%08x", RAM)
        self.assertIn("stage_current=0x%08x", RAM)

    def test_proc_write_targets_actual_fiq_step(self):
        self.assertIn("LAST_RR_SET(fiq_step, value);", RAM)
        self.assertIn('proc_create("echo_stage", 0666', RAM)
        self.assertIn("kstrtou32(value_buf, 0, &value)", RAM)
        self.assertIn("if (value > 0xff)", RAM)

    def test_cleanup_writers_still_use_vendor_fiq_step(self):
        self.assertIn("aee_rr_rec_fiq_step(ucStage);", GL)
        self.assertIn("aee_rr_rec_fiq_step(step)", OPEN)


if __name__ == "__main__":
    unittest.main()
