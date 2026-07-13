#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BTIF = ROOT / "drivers/misc/mediatek/btif/common/mtk_btif.c"
DMA = ROOT / "drivers/misc/mediatek/btif/common/btif_dma_plat.c"


class TestEchoBtifDmaChain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.btif = BTIF.read_text()
        cls.dma = DMA.read_text()

    def test_hal_chain_and_existing_guards(self):
        self.assertIn("aee_rr_rec_fiq_step(0xA1)", self.dma)
        self.assertIn("aee_rr_rec_fiq_step(0xA2)", self.dma)
        positions = [self.dma.index("aee_rr_rec_fiq_step(0xD%d)" % n) for n in (2, 3, 4)]
        self.assertEqual(positions, sorted(positions))

    def test_irq_chain_order(self):
        positions = [self.btif.index("aee_rr_rec_fiq_step(0x%02X)" % n) for n in (0xD0, 0xD1, 0xD5, 0xD6, 0xD7)]
        self.assertEqual(positions, sorted(positions))

    def test_thread_and_callback_markers(self):
        self.assertIn("aee_rr_rec_fiq_step(0xD8); /* btif_rxd awakened */", self.btif)
        self.assertEqual(self.btif.count("aee_rr_rec_fiq_step(0xD9)"), 3)
        self.assertEqual(self.btif.count("aee_rr_rec_fiq_step(0xDA)"), 3)


if __name__ == "__main__":
    unittest.main()
