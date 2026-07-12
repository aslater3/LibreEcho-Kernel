#!/usr/bin/env python3
"""Host-side regression tests for the MT8163 STP/BTIF TX boundary.

The tests compile the real function bodies from the vendor sources with a
minimal host shim.  This exercises the code under test without requiring the
MT8163 hardware or a booted kernel.
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STP_CORE = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/stp_core.c"
STP_BTIF = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/stp_btif.c"
WMT_CTRL = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_ctrl.c"
WMT_CORE = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_core.c"
WMT_DEV = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/wmt_dev.c"


def extract_function(source: str, name: str) -> str:
    match = re.search(
        rf"(?m)^[^\n;]*\b{re.escape(name)}\s*\([^;]*?\)\s*\n(?:#[^\n]*\n)*\{{",
        source,
    )
    if not match:
        raise AssertionError(f"function {name} not found")

    start = match.start()
    brace = source.find("{", match.start())
    depth = 0
    state = "code"
    i = brace
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif ch == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        elif state == "block_comment" and ch == "*" and nxt == "/":
            state = "code"
            i += 1
        elif state == "line_comment" and ch == "\n":
            state = "code"
        elif state in {"string", "char"}:
            if ch == "\\":
                i += 1
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1

    raise AssertionError(f"unterminated function {name}")


def compile_and_run(source: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="stp-transport-test-") as temp:
        c_path = Path(temp) / "test.c"
        exe_path = Path(temp) / "test"
        c_path.write_text(source)
        compile_result = subprocess.run(
            ["gcc", "-std=gnu99", "-Wall", "-Wextra", str(c_path), "-o", str(exe_path)],
            text=True,
            capture_output=True,
        )
        if compile_result.returncode:
            raise AssertionError(f"host harness failed to compile:\n{compile_result.stderr}")
        return subprocess.run([str(exe_path)], text=True, capture_output=True)


class BtifExactWriteTests(unittest.TestCase):
    def test_btif_rejects_zero_negative_and_partial_progress(self) -> None:
        function = extract_function(STP_BTIF.read_text(), "mtk_wcn_consys_stp_btif_tx")
        harness = textwrap.dedent(
            f"""
            #include <errno.h>
            #include <stdint.h>
            #include <stdio.h>
            typedef int INT32;
            typedef unsigned int UINT32;
            typedef unsigned char UINT8;
            #define STP_BTIF_TX_RTY_LMT 10
            #define STP_BTIF_TX_RTY_DLY 1
            #define STP_MAX_PACKAGE_ALLOWED 2000
            #define WMT_WARN_FUNC(...) do {{ }} while (0)
            #define WMT_INFO_FUNC(...) do {{ }} while (0)
            #define WMT_ERR_FUNC(...) do {{ }} while (0)
            #define osal_ftrace_print(...) do {{ }} while (0)
            struct fake_task {{ int pid; const char *comm; }};
            static struct fake_task fake_current = {{ 1, "test" }};
            #define current (&fake_current)
            static unsigned long stpBtifId = 1;
            static UINT32 stp_btif_tx_failure_count;
            static int stp_btif_diag_should_log(UINT32 count) {{
                return count <= 4 || !(count & (count - 1));
            }}
            static int results[16];
            static int result_count;
            static int result_index;
            static int write_calls;
            static void set_results(const int *values, int count) {{
                int i;
                for (i = 0; i < count; i++) results[i] = values[i];
                result_count = count;
                result_index = 0;
                write_calls = 0;
            }}
            static int mtk_wcn_btif_write(unsigned long id, const UINT8 *buf, UINT32 len) {{
                (void)id; (void)buf; (void)len;
                write_calls++;
                if (result_index >= result_count) return results[result_count - 1];
                return results[result_index++];
            }}
            static void osal_sleep_ms(int ms) {{ (void)ms; }}
            {function}
            static int require(int condition, const char *message) {{
                if (!condition) {{ fprintf(stderr, "%s\\n", message); return 1; }}
                return 0;
            }}
            int main(void) {{
                UINT8 data[16] = {{0}};
                UINT32 written = 99;
                int rc;
                int zero[] = {{0}};
                int partial[] = {{3, 7}};
                int negative[] = {{-5}};
                int complete[] = {{10}};

                set_results(zero, 1);
                rc = mtk_wcn_consys_stp_btif_tx(data, 10, &written);
                if (require(rc == -EIO, "zero progress must return -EIO") ||
                    require(written == 0, "zero progress must report zero bytes") ||
                    require(write_calls == 1, "zero progress must not be retried")) return 1;

                set_results(partial, 2);
                written = 99;
                rc = mtk_wcn_consys_stp_btif_tx(data, 10, &written);
                if (require(rc == -EIO, "partial progress must return -EIO") ||
                    require(written == 3, "partial progress must be reported") ||
                    require(write_calls == 1, "a partial frame must not retry its suffix")) return 1;

                set_results(negative, 1);
                written = 99;
                rc = mtk_wcn_consys_stp_btif_tx(data, 10, &written);
                if (require(rc == -5, "negative BTIF errors must be preserved") ||
                    require(written == 0, "negative errors must report zero bytes")) return 1;

                set_results(complete, 1);
                written = 0;
                rc = mtk_wcn_consys_stp_btif_tx(data, 10, &written);
                if (require(rc == 10, "complete write must return its length") ||
                    require(written == 10, "complete write must report its length") ||
                    require(write_calls == 1, "complete write must use one call")) return 1;

                set_results(complete, 1);
                written = 99;
                rc = mtk_wcn_consys_stp_btif_tx(data, 0, &written);
                if (require(rc == -EINVAL, "zero-length callbacks must be rejected") ||
                    require(write_calls == 0, "zero-length callbacks must not reach BTIF")) return 1;
                return 0;
            }}
            """
        )
        result = compile_and_run(harness)
        self.assertEqual(result.returncode, 0, result.stderr)


class StpRingBoundaryTests(unittest.TestCase):
    def test_exact_end_and_wrap_use_one_exact_callback(self) -> None:
        source = STP_CORE.read_text()
        update = extract_function(source, "stp_update_tx_queue")
        exact = extract_function(source, "stp_if_tx_exact")
        send = extract_function(source, "stp_send_tx_queue")
        harness = textwrap.dedent(
            f"""
            #include <errno.h>
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>
            typedef int INT32;
            typedef unsigned int UINT32;
            typedef unsigned char UINT8;
            typedef void VOID;
            #define MTKSTP_BUFFER_SIZE 32
            #define MTKSTP_SEQ_SIZE 8
            #define STP_ERR_FUNC(...) do {{ }} while (0)
            #define STP_INFO_FUNC(...) do {{ }} while (0)
            #define osal_assert(x) do {{ (void)(x); }} while (0)
            #define osal_memcpy memcpy
            static UINT32 stp_tx_wrap_count;
            static UINT32 stp_tx_failure_count;
            static int stp_diag_should_log(UINT32 count) {{
                return count <= 4 || !(count & (count - 1));
            }}
            struct sequence_context {{ UINT8 txack; }};
            struct context {{
                UINT8 tx_buf[MTKSTP_BUFFER_SIZE];
                UINT32 tx_start_addr[MTKSTP_SEQ_SIZE];
                UINT32 tx_length[MTKSTP_SEQ_SIZE];
                UINT8 *tx_linear_buf;
                UINT32 tx_linear_buf_size;
                struct sequence_context sequence;
            }};
            static struct context stp_core_ctx;
            typedef INT32 (*IF_TX)(const UINT8 *, const UINT32, UINT32 *);
            static IF_TX sys_if_tx;
            static int callback_calls;
            static int callback_status;
            static int callback_short;
            static UINT32 callback_len;
            static UINT8 callback_data[MTKSTP_BUFFER_SIZE];
            static INT32 fake_tx(const UINT8 *data, const UINT32 len, UINT32 *written) {{
                callback_calls++;
                callback_len = len;
                memcpy(callback_data, data, len);
                *written = callback_short ? len - 1 : len;
                return callback_status ? callback_status : (INT32)len;
            }}
            {update}
            {exact}
            {send}
            static void prepare(UINT32 start, UINT32 len, UINT8 *linear) {{
                UINT32 i;
                memset(&stp_core_ctx, 0, sizeof(stp_core_ctx));
                stp_core_ctx.tx_linear_buf = linear;
                stp_core_ctx.tx_linear_buf_size = MTKSTP_BUFFER_SIZE;
                stp_core_ctx.tx_start_addr[0] = start;
                stp_core_ctx.tx_length[0] = len;
                stp_core_ctx.sequence.txack = 3;
                for (i = 0; i < len; i++)
                    stp_core_ctx.tx_buf[(start + i) % MTKSTP_BUFFER_SIZE] = (UINT8)(0x80 + i);
                callback_calls = 0;
                callback_status = 0;
                callback_short = 0;
                callback_len = 0;
                sys_if_tx = fake_tx;
            }}
            static int verify_frame(UINT32 start, UINT32 len) {{
                UINT32 i;
                if (callback_calls != 1 || callback_len != len) return 1;
                for (i = 0; i < len; i++)
                    if (callback_data[i] != stp_core_ctx.tx_buf[(start + i) % MTKSTP_BUFFER_SIZE]) return 1;
                return 0;
            }}
            int main(void) {{
                UINT8 linear[MTKSTP_BUFFER_SIZE];
                int rc;
                prepare(22, 10, linear);
                rc = stp_send_tx_queue(0);
                if (rc || verify_frame(22, 10)) {{ fprintf(stderr, "exact-end frame was not one exact callback\\n"); return 1; }}

                prepare(27, 10, linear);
                rc = stp_send_tx_queue(0);
                if (rc || verify_frame(27, 10)) {{ fprintf(stderr, "wrapped frame was not linearized once\\n"); return 1; }}

                prepare(27, 10, linear);
                callback_short = 1;
                rc = stp_send_tx_queue(0);
                if (rc != -EIO || callback_calls != 1) {{ fprintf(stderr, "short callback was not propagated\\n"); return 1; }}

                prepare(27, 10, linear);
                callback_status = -ENODEV;
                rc = stp_send_tx_queue(0);
                if (rc != -ENODEV || callback_calls != 1) {{ fprintf(stderr, "callback error was not propagated\\n"); return 1; }}
                return 0;
            }}
            """
        )
        result = compile_and_run(harness)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_send_state_commits_only_after_transport_success(self) -> None:
        source = STP_CORE.read_text()
        for name in ("stp_send_data_no_ps", "mtk_wcn_stp_send_data"):
            body = extract_function(source, name)
            send_pos = body.find("stp_send_tx_queue")
            check = re.search(r"if\s*\([^)]*tx_ret[^)]*\)", body)
            commit_pos = body.find("INDEX_INC", send_pos)
            self.assertGreaterEqual(send_pos, 0, f"{name} must call stp_send_tx_queue")
            if check is None:
                self.fail(f"{name} must check the transport result")
            self.assertLess(check.start(), commit_pos, f"{name} must check failure before committing txseq")

    def test_linear_buffer_has_init_and_teardown_lifecycle(self) -> None:
        source = STP_CORE.read_text()
        init = extract_function(source, "mtk_wcn_stp_init")
        deinit = extract_function(source, "mtk_wcn_stp_deinit")
        self.assertRegex(init, r"tx_linear_buf\s*=\s*osal_malloc")
        self.assertRegex(deinit, r"osal_free\s*\(\s*stp_core_ctx\.tx_linear_buf\s*\)")


class WmtTransportPropagationTests(unittest.TestCase):
    def test_wmt_ctrl_preserves_stp_failures(self) -> None:
        function = extract_function(WMT_CTRL.read_text(), "wmt_ctrl_tx_ex")
        harness = textwrap.dedent(
            f"""
            #include <errno.h>
            #include <stdint.h>
            #include <stdio.h>
            typedef int INT32;
            typedef unsigned int UINT32;
            typedef unsigned char UINT8;
            typedef UINT8 *PUINT8;
            typedef UINT32 *PUINT32;
            typedef int MTK_WCN_BOOL;
            #define WMT_TASK_INDX 0
            #define WMT_STAT_STP_OPEN 0
            #define WMT_STAT_STP_EN 1
            #define WMT_WARN_FUNC(...) do {{ }} while (0)
            #define WMT_ERR_FUNC(...) do {{ }} while (0)
            #define osal_assert(x) do {{ (void)(x); }} while (0)
            struct dev_wmt {{ unsigned long state; }};
            typedef struct dev_wmt *P_DEV_WMT;
            static struct dev_wmt gDevWmt = {{ 3 }};
            static INT32 fake_stp_result;
            static unsigned int notify_count;
            static void wmt_dev_stp_error_notify(void) {{ notify_count++; }}
            static void wmt_dev_stp_error_arm(void) {{ }}
            static int osal_test_bit(int bit, unsigned long *state) {{ return !!(*state & (1UL << bit)); }}
            static void mtk_wcn_stp_flush_rx_queue(int type) {{ (void)type; }}
            static INT32 mtk_wcn_stp_send_data_raw(const UINT8 *data, UINT32 size, UINT8 type) {{
                (void)data; (void)size; (void)type; return fake_stp_result;
            }}
            static INT32 mtk_wcn_stp_send_data(const UINT8 *data, UINT32 size, UINT8 type) {{
                (void)data; (void)size; (void)type; return fake_stp_result;
            }}
            {function}
            int main(void) {{
                UINT8 data[10] = {{0}};
                UINT32 written;
                INT32 rc;

                fake_stp_result = -EIO;
                written = 99;
                rc = wmt_ctrl_tx_ex(data, sizeof(data), &written, 0);
                if (rc != -EIO || written != 0) {{ fprintf(stderr, "negative STP error was masked\\n"); return 1; }}

                fake_stp_result = 0;
                written = 99;
                rc = wmt_ctrl_tx_ex(data, sizeof(data), &written, 0);
                if (rc != -EAGAIN || written != 0) {{ fprintf(stderr, "no-window result was not retryable\\n"); return 1; }}

                fake_stp_result = 5;
                written = 99;
                rc = wmt_ctrl_tx_ex(data, sizeof(data), &written, 0);
                if (rc != -EIO || written != 5) {{ fprintf(stderr, "partial STP result was masked\\n"); return 1; }}

                fake_stp_result = 10;
                written = 0;
                rc = wmt_ctrl_tx_ex(data, sizeof(data), &written, 0);
                if (rc || written != 10) {{ fprintf(stderr, "complete STP result failed\\n"); return 1; }}
                if (notify_count != 2) {{ fprintf(stderr, "unexpected stream-error notifications: %u\\n", notify_count); return 1; }}
                return 0;
            }}
            """
        )
        result = compile_and_run(harness)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wmt_core_retries_only_no_window(self) -> None:
        function = extract_function(WMT_CORE.read_text(), "wmt_core_tx")
        harness = textwrap.dedent(
            f"""
            #include <errno.h>
            #include <stdint.h>
            #include <stdio.h>
            typedef int INT32;
            typedef unsigned int UINT32;
            typedef unsigned char UINT8;
            typedef UINT8 *PUINT8;
            typedef UINT32 *PUINT32;
            typedef int MTK_WCN_BOOL;
            #define WMT_WARN_FUNC(...) do {{ }} while (0)
            #define WMT_ERR_FUNC(...) do {{ }} while (0)
            #define osal_assert(x) do {{ (void)(x); }} while (0)
            static INT32 results[8];
            static int result_count;
            static int result_index;
            static int tx_calls;
            static void set_results(const INT32 *values, int count) {{
                int i;
                for (i = 0; i < count; i++) results[i] = values[i];
                result_count = count; result_index = 0; tx_calls = 0;
            }}
            static INT32 wmt_ctrl_tx_ex(const UINT8 *data, UINT32 size, UINT32 *written, MTK_WCN_BOOL raw) {{
                INT32 rc;
                (void)data; (void)raw;
                tx_calls++;
                rc = results[result_index < result_count ? result_index++ : result_count - 1];
                *written = rc == 0 ? size : 0;
                return rc;
            }}
            static void osal_sleep_ms(int ms) {{ (void)ms; }}
            {function}
            int main(void) {{
                UINT8 data[10] = {{0}};
                UINT32 written = 0;
                INT32 rc;
                INT32 hard_error[] = {{-EIO}};
                INT32 retry_then_ok[] = {{-EAGAIN, 0}};

                set_results(hard_error, 1);
                rc = wmt_core_tx(data, sizeof(data), &written, 0);
                if (rc != -EIO || tx_calls != 1) {{ fprintf(stderr, "hard transport error was retried\\n"); return 1; }}

                set_results(retry_then_ok, 2);
                written = 0;
                rc = wmt_core_tx(data, sizeof(data), &written, 0);
                if (rc || written != sizeof(data) || tx_calls != 2) {{ fprintf(stderr, "no-window result was not retried once\\n"); return 1; }}
                return 0;
            }}
            """
        )
        result = compile_and_run(harness)
        self.assertEqual(result.returncode, 0, result.stderr)


class CrcWakeupTests(unittest.TestCase):
    def test_protocol_error_wakeup_does_not_touch_rx_accounting(self) -> None:
        source = WMT_DEV.read_text()
        notify = extract_function(source, "wmt_dev_stp_error_notify")
        self.assertIn("atomic_inc", notify)
        self.assertIn("wake_up_interruptible", notify)
        self.assertNotIn("gRxCount", notify)
        self.assertNotIn("u4RxFlag", notify)
        timeout = extract_function(source, "wmt_dev_rx_timeout")
        self.assertNotIn("gpRxEvent = NULL", timeout)

    def test_wait_condition_returns_badmsg_for_new_generation(self) -> None:
        source = WMT_DEV.read_text()
        timeout = extract_function(source, "wmt_dev_rx_timeout")
        self.assertIn("stp_error_generation", timeout)
        self.assertIn("-EBADMSG", timeout)
        self.assertRegex(timeout, r"gRxCount\).*?>\s*0|atomic_read\s*\(\s*&gRxCount\s*\)\s*>\s*0")

    def test_crc_failure_notifies_wmt_and_wmt_propagates_badmsg(self) -> None:
        stp = extract_function(STP_CORE.read_text(), "stp_parser_data_in_full_mode")
        tx = extract_function(WMT_CTRL.read_text(), "wmt_ctrl_tx_ex")
        ctrl = extract_function(WMT_CTRL.read_text(), "wmt_ctrl_rx")
        self.assertIn("sys_protocol_error", stp)
        arm_pos = tx.find("wmt_dev_stp_error_arm")
        send_pos = tx.find("iRet = mtk_wcn_stp_send_data")
        self.assertGreaterEqual(arm_pos, 0)
        self.assertLess(arm_pos, send_pos)
        self.assertIn("wmt_dev_stp_error_snapshot", ctrl)
        self.assertRegex(ctrl, r"waitRet\s*==\s*-EBADMSG")
        self.assertRegex(ctrl, r"return\s+waitRet")

    def test_hard_tx_errors_mark_stream_and_init_retries_once(self) -> None:
        ctrl_tx = extract_function(WMT_CTRL.read_text(), "wmt_ctrl_tx_ex")
        stp_init = extract_function(WMT_CORE.read_text(), "wmt_core_stp_init")
        pwr_on = extract_function(WMT_CORE.read_text(), "opfunc_pwr_on")
        self.assertGreaterEqual(ctrl_tx.count("wmt_dev_stp_error_notify"), 2)
        self.assertNotIn("recovery_active", stp_init)
        self.assertIn("wmt_core_stp_deinit()", pwr_on)
        self.assertIn("opfunc_pwr_off", pwr_on)
        self.assertIn("goto pwr_on_rty", pwr_on)
        self.assertIn("stream_recovery_active", pwr_on)
        self.assertIn("wmt_dev_stp_error_generation", pwr_on)
        self.assertRegex(pwr_on, r"if\s*\(\s*stream_recovery_active\s*\)")
        self.assertRegex(pwr_on, r"if\s*\(\s*0\s*<\s*retry--\s*\)")

    def test_transport_diagnostics_are_counted_and_rate_limited(self) -> None:
        stp = STP_CORE.read_text()
        btif = STP_BTIF.read_text()
        for counter in (
            "stp_tx_wrap_count",
            "stp_tx_failure_count",
            "stp_crc_failure_count",
        ):
            self.assertIn(counter, stp)
        self.assertIn("stp_btif_tx_failure_count", btif)
        self.assertIn("stp_diag_should_log", stp)
        self.assertIn("stp_btif_diag_should_log", btif)


if __name__ == "__main__":
    unittest.main(verbosity=2)
