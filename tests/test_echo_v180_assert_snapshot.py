import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STP_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c"
WLAN_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/common/wlan_lib.c"
STP = STP_PATH.read_text()
WLAN = WLAN_PATH.read_text()


def body_between(source: str, start_marker: str, end_marker: str) -> str:
    start = source.index(start_marker)
    return source[start : source.index(end_marker, start)]


class V180AssertionSnapshotContract(unittest.TestCase):
    def test_snapshot_layout_is_bounded_and_complete(self):
        match = re.search(
            r"struct echo_wlan_assert_snapshot\s*\{(?P<body>.*?)\n\};",
            STP,
            re.S,
        )
        self.assertIsNotNone(match)
        assert match is not None
        fields = re.findall(r"\b(u32|u8)\s+([a-z0-9_]+)(?:\[(\d+)\])?\s*;", match.group("body"))
        self.assertEqual(
            fields,
            [
                ("u32", "magic", ""),
                ("u32", "payload_len", ""),
                ("u32", "copied_len", ""),
                ("u32", "payload_crc32", ""),
                ("u32", "cpupcr", ""),
                ("u32", "wcir", ""),
                ("u32", "wasr", ""),
                ("u32", "d2hrm0", ""),
                ("u32", "d2hrm1", ""),
                ("u8", "payload", "1024"),
            ],
        )
        self.assertIn("#define ECHO_WLAN_ASSERT_PAYLOAD_MAX 1024", STP)

    def test_first_complete_frame_is_captured_before_latch(self):
        capture = body_between(
            STP,
            "static VOID echo_capture_wlan_assert(",
            "static int echo_wlan_assert_proc_show",
        )
        required = (
            "atomic_cmpxchg(&echo_wlan_assert_snapshot_state, 0, -1)",
            "min_t(u32, payload_len, ECHO_WLAN_ASSERT_PAYLOAD_MAX)",
            "crc32_le(~0U, payload, payload_len) ^ ~0U",
            "osal_memcpy(g_echo_wlan_assert_snapshot.payload, payload, copy_len)",
            "reader(context, &g_echo_wlan_assert_snapshot.cpupcr",
            "atomic_set(&echo_wlan_assert_snapshot_state, 1)",
            "STP_SET_FW_COREDUMP_FLAG(stp_core_ctx, 1)",
            "atomic_set(&echo_fw_assert_latched, 1)",
        )
        positions = [capture.index(token) for token in required]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("smp_wmb();", capture)

    def test_both_parser_modes_capture_then_return_without_dump_drain(self):
        call = "echo_capture_wlan_assert(stp_core_ctx.rx_buf, stp_core_ctx.rx_counter);"
        self.assertEqual(STP.count(call), 2)
        start = 0
        for _ in range(2):
            start = STP.index(call, start)
            end = STP.index("return 0;", start)
            unwind = STP[start:end]
            self.assertIn("stp_change_rx_state(MTKSTP_SYNC)", unwind)
            self.assertIn("stp_core_ctx.rx_counter = 0", unwind)
            self.assertNotIn("stp_btm_notify_wmt_dmp_wq", unwind)
            self.assertNotIn("stp_btm_notify_wmt_trace_wq", unwind)
            self.assertNotIn("mtk_wcn_stp_coredump_start_ctrl", unwind)
            start = end

    def test_empty_task_frames_also_return_before_legacy_dump_handling(self):
        call = "echo_capture_wlan_assert(stp_core_ctx.rx_buf, stp_core_ctx.rx_counter);"
        self.assertEqual(STP.count(call), 2)
        regions = []
        search_from = 0
        for _ in range(2):
            call_at = STP.index(call, search_from)
            start = STP.rfind(
                "stp_core_ctx.rx_counter = stp_core_ctx.parser.length;",
                0,
                call_at,
            )
            end = STP.index("return 0;", call_at)
            regions.append(STP[start:end])
            search_from = end
        for block in regions:
            self.assertIn("if (stp_core_ctx.parser.type == STP_TASK_INDX) {", block)
            self.assertIn("if (stp_core_ctx.rx_counter != 0)", block)
            self.assertNotIn("stp_btm_notify_wmt_dmp_wq", block)
            self.assertNotIn("mtk_wcn_stp_coredump_start_ctrl", block)
        self.assertNotIn(
            "if (stp_core_ctx.parser.type == STP_TASK_INDX &&\n\t\t\t\t    stp_core_ctx.rx_counter != 0)",
            STP,
        )

    def test_proc_surface_is_read_only_and_emits_exact_payload_views(self):
        self.assertIn('#include <linux/proc_fs.h>', STP)
        self.assertIn('#include <linux/seq_file.h>', STP)
        self.assertIn('proc_create("echo_wlan_assert", 0444, NULL,', STP)
        self.assertNotIn('proc_create("echo_wlan_assert", 0666', STP)
        show = body_between(
            STP,
            "static int echo_wlan_assert_proc_show",
            "static int echo_wlan_assert_proc_open",
        )
        for field in (
            "magic=0x%08x",
            "payload_len=%u",
            "copied_len=%u",
            "payload_crc32=0x%08x",
            "cpupcr=0x%08x",
            "wcir=0x%08x",
            "wasr=0x%08x",
            "d2hrm0=0x%08x",
            "d2hrm1=0x%08x",
            "payload_hex=",
            "payload_ascii=",
        ):
            self.assertIn(field, show)
        match = re.search(
            r"static const struct file_operations echo_wlan_assert_proc_fops\s*=\s*\{(?P<body>.*?)\n\};",
            STP,
            re.S,
        )
        self.assertIsNotNone(match)
        assert match is not None
        fops = match.group("body")
        self.assertIn(".read = seq_read", fops)
        self.assertNotIn(".write", fops)

    def test_proc_creation_failure_and_deinit_are_handled(self):
        init = body_between(STP, "INT32 mtk_wcn_stp_init(", "INT32 mtk_wcn_stp_deinit(")
        self.assertIn("if (!g_echo_wlan_assert_proc) {", init)
        self.assertIn("ret = -ENOMEM;", init)
        self.assertIn("goto RETURN;", init)
        deinit = STP[STP.index("INT32 mtk_wcn_stp_deinit(") :]
        self.assertIn('remove_proc_entry("echo_wlan_assert", NULL);', deinit)
        self.assertIn("g_echo_wlan_assert_proc = NULL;", deinit)

    def test_snapshot_and_latch_are_reset_when_stp_initializes(self):
        init = body_between(STP, "INT32 mtk_wcn_stp_init(", "INT32 mtk_wcn_stp_deinit(")
        for token in (
            "atomic_set(&echo_wlan_assert_snapshot_state, 0)",
            "atomic_set(&echo_fw_assert_latched, 0)",
            "osal_memset(&g_echo_wlan_assert_snapshot, 0",
            'proc_create("echo_wlan_assert", 0444, NULL,',
        ):
            self.assertIn(token, init)
        self.assertIn("if (!g_echo_wlan_assert_proc)", init)

    def test_wlan_register_reader_records_live_hif_registers(self):
        reader = body_between(
            WLAN,
            "static VOID echoWlanAssertReadRegisters(",
            "static VOID echoWlanPersistStage",
        )
        self.assertIn("wmt_plat_read_cpupcr()", reader)
        self.assertIn("HAL_MCR_RD(prAdapter, MCR_WCIR, wcir)", reader)
        self.assertIn("HAL_MCR_RD(prAdapter, MCR_WASR, wasr)", reader)
        self.assertIn("HAL_MCR_RD(prAdapter, MCR_D2HRM0R, d2hrm0)", reader)
        self.assertIn("HAL_MCR_RD(prAdapter, MCR_D2HRM1R, d2hrm1)", reader)

        start = body_between(WLAN, "WLAN_STATUS\nwlanAdapterStart", "/* wlanAdapterStart */")
        register = "echo_wlan_assert_register_reader(echoWlanAssertReadRegisters, prAdapter)"
        unregister = "echo_wlan_assert_register_reader(NULL, NULL)"
        self.assertIn("static VOID echoWlanAssertReadRegisters(VOID *context", WLAN)
        self.assertIn("P_ADAPTER_T prAdapter = (P_ADAPTER_T)context", WLAN)
        self.assertIn("DEFINE_SPINLOCK(g_echo_wlan_assert_reg_lock)", STP)
        self.assertIn("spin_lock_irqsave(&g_echo_wlan_assert_reg_lock, flags)", STP)
        self.assertIn(register, start)
        self.assertIn(unregister, start)
        self.assertLess(start.index(register), start.index("do {"))
        self.assertLess(start.index(unregister), start.index("return u4Status"))

    def test_v179_runtime_baseline_is_unchanged_outside_observability(self):
        expected = {
            ROOT / ".config": "07e59ee076cda7f5ba25ffe7d3365e145678a50f3fb34074b07938b3f6d815fd",
            ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c": "84c4126153208740f5cecef2176af9b65c483e4d130405232fb5f2af25b2775e",
            ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_ts_wmt.c": "34cfb0c3f8c2bea2f9ca4a5873e188279308c90aa84b7831ee8997d08140fbfc",
            ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_temp_sensor_wmt.c": "f3290af5958de2a1a36ba906726c78920ac345c7b0926dcaa2fe3e9665c52170",
            ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c": "ffb89e87981803ef11c1ca2416a0372a187477b4652b75c277986b68c2cd1224",
            ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c": "a1227ae66897fdd63b6ae9d7665637a13a2b04126d1c5c540fd91ff1e240bd66",
            ROOT / "drivers/misc/mediatek/btif/common/mtk_btif.c": "4da31919151bf48208826d11243b413d01981b58ce918086f373b8716f3ddbb1",
        }
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
