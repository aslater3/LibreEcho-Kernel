import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RAM = (ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c").read_text()
RAM_H = (ROOT / "drivers/misc/mediatek/include/mt-plat/mtk_ram_console.h").read_text()
GL = (ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_kal.c").read_text()
OPEN = (ROOT / "fs/open.c").read_text()


class EchoStageChannelContract(unittest.TestCase):
    def test_persistent_stage_fields_and_proc_node(self):
        self.assertIn("uint32_t echo_stage;", RAM)
        self.assertIn("uint32_t echo_stage_selftest;", RAM)
        self.assertIn('proc_create("echo_stage", 0444', RAM)
        self.assertIn("current=0x%08x previous=0x%08x selftest=0x%08x", RAM)

    def test_boot_selftest_has_two_patterns_and_runs_before_activation(self):
        self.assertIn("ECHO_STAGE_SELFTEST_A 0x13579BDF", RAM)
        self.assertIn("ECHO_STAGE_SELFTEST_B 0x2468ACE0", RAM)
        init_start = RAM.index("static int __init ram_console_init(")
        init_end = RAM.index("\n}\n", init_start)
        init_body = RAM[init_start:init_end]
        self.assertLess(init_body.index("ram_console_init_done = 1;"), init_body.index("echo_stage_selftest();"))
        self.assertLess(init_body.index("echo_stage_selftest();"), init_body.index("return 0;"))

    def test_api_is_available_with_and_without_ram_console(self):
        for symbol in ("echo_stage_set", "echo_stage_current", "echo_stage_previous"):
            self.assertIn(symbol, RAM_H)

    def test_cleanup_paths_write_readable_stage(self):
        self.assertIn("echo_stage_set(ucStage);", GL)
        self.assertIn("echo_stage_set(step);", OPEN)
        self.assertIn("aee_rr_rec_fiq_step(step);", OPEN)


if __name__ == "__main__":
    unittest.main()
