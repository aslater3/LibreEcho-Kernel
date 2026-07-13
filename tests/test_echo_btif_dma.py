#!/usr/bin/env python3
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / (
    "drivers/misc/mediatek/btif/common/btif_dma_plat.c"
)
HEADER = Path(__file__).resolve().parents[1] / (
    "drivers/misc/mediatek/btif/common/plat_inc/btif_dma_priv.h"
)


class BtifRxDmaGuardTests(unittest.TestCase):
    def test_rx_drain_uses_current_wraps_and_has_bounded_escapes(self):
        source = SOURCE.read_text()
        header = HEADER.read_text()
        start = source.index("int hal_rx_dma_irq_handler(")
        end = source.index("static int hal_tx_dma_dump_reg", start)
        handler = source[start:end]

        self.assertIn("#define BTIF_RX_IRQ_MAX_PASSES 4", header)
        self.assertIn("unsigned int passes = 0;", handler)
        self.assertIn("unsigned int total_processed = 0;", handler)
        self.assertIn("if (wpt_wrap != rpt_wrap)", handler)
        self.assertNotIn("if (wpt_wrap != p_mtk_vfifo->last_wpt_wrap)", handler)
        self.assertIn("aee_rr_rec_fiq_step(0xA1)", handler)
        self.assertIn("aee_rr_rec_fiq_step(0xA2)", handler)
        self.assertIn("passes++ >= BTIF_RX_IRQ_MAX_PASSES", handler)
        self.assertIn("total_processed >= vff_size", handler)


if __name__ == "__main__":
    unittest.main()
