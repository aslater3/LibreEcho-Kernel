#!/usr/bin/env python3
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / (
    "drivers/misc/mediatek/btif/common/mtk_btif.c"
)


class TestEchoBtifSched(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()
        start = cls.source.index("static int _btif_rx_btm_sched(p_mtk_btif p_btif)\n{")
        end = cls.source.index("static int _btif_rx_btm_deinit", start)
        cls.function = cls.source[start:end]

    def test_thread_context_markers_order(self):
        f = self.function
        positions = [f.index("aee_rr_rec_fiq_step(0x%02X)" % n) for n in (0xDB, 0xDC, 0xDF)]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(f.index("0xDB"), f.index("complete(&p_btif->rx_comp)"))
        self.assertLess(f.index("complete(&p_btif->rx_comp)"), f.index("0xDC"))

    def test_hard_irq_logging_removed_from_thread_context(self):
        f = self.function
        self.assertNotIn("btif_ftrace_print", f)
        self.assertNotIn('BTIF_DBG_FUNC("schedule btif_rx_thread', f)

    def test_other_context_paths_remain(self):
        f = self.function
        self.assertIn("queue_work(p_btif->p_rx_wq", f)
        self.assertIn("tasklet_schedule(&(p_btif->rx_tasklet))", f)


if __name__ == "__main__":
    unittest.main()
