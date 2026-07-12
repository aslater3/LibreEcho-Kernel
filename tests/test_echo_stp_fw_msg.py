#!/usr/bin/env python3
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / (
    "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c"
)


class FirmwareMessageBoundaryTests(unittest.TestCase):
    def test_full_mode_fw_message_is_bounded_and_skips_heavy_dump(self):
        text = SOURCE.read_text()
        start = text.rindex("\t\tcase MTKSTP_FW_MSG:")
        branch_end = text.index("\n\t\tdefault:", start)
        branch = text[start:branch_end]

        self.assertIn("ECHO_STP_FW_FRAME", text)
        self.assertIn("echo_stp_capture_frame", branch)
        self.assertNotIn("mtk_wcn_stp_dbg_dump_package()", branch)


if __name__ == "__main__":
    unittest.main()
