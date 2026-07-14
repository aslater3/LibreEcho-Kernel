#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BTIF = (ROOT / "drivers/misc/mediatek/btif/common/mtk_btif.c").read_text()
DMA = (ROOT / "drivers/misc/mediatek/btif/common/btif_dma_plat.c").read_text()


class TestEchoBtifDmaChain(unittest.TestCase):
    def test_hal_chain_and_existing_guards(self):
        self.assertIn("aee_rr_rec_fiq_step(0xA1)", DMA)
        self.assertIn("aee_rr_rec_fiq_step(0xA2)", DMA)
        positions = [DMA.index("aee_rr_rec_fiq_step(0xD%d)" % n) for n in (2, 3, 4)]
        self.assertEqual(positions, sorted(positions))

    def test_rx_callback_and_completion_markers(self):
        self.assertIn("aee_rr_rec_fiq_step(ECHO_ASSERT_UNWIND_E4)", BTIF)
        self.assertIn("aee_rr_rec_fiq_step(ECHO_ASSERT_UNWIND_E5)", BTIF)
        self.assertIn("aee_rr_rec_fiq_step(0xD8); /* btif_rxd awakened */", BTIF)
        positions = [BTIF.index(marker) for marker in (
            "aee_rr_rec_fiq_step(ECHO_ASSERT_UNWIND_E4)",
            "aee_rr_rec_fiq_step(ECHO_ASSERT_UNWIND_E5)",
            "aee_rr_rec_fiq_step(0xDB)",
            "aee_rr_rec_fiq_step(0xDC)",
            "aee_rr_rec_fiq_step(0xDF)",
        )]
        self.assertEqual(positions, sorted(positions))


if __name__ == "__main__":
    unittest.main()
