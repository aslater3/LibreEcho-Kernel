import hashlib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THERMAL_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_ts_wmt.c"
SENSOR_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_temp_sensor_wmt.c"
WMT_LIB_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c"
WMT_FUNC_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c"
WMT_DEV_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/wmt_dev.c"
WMT_WIFI_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pub/wmt_chrdev_wifi.c"
RAM_CONSOLE_PATH = ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c"

THERMAL = THERMAL_PATH.read_text()
CONFIG = (ROOT / ".config").read_bytes()


class V178SuppressWmtThermalContract(unittest.TestCase):
    def test_wifi_capable_config_is_byte_identical_to_v177(self):
        self.assertEqual(
            hashlib.sha256(CONFIG).hexdigest(),
            "07e59ee076cda7f5ba25ffe7d3365e145678a50f3fb34074b07938b3f6d815fd",
        )

    def test_volatile_guard_precedes_every_wmt_thermal_init_side_effect(self):
        self.assertIn(
            "static volatile int echo_v178_suppress_wmt_thermal_registration = 1;",
            THERMAL,
        )
        init_start = THERMAL.index("static int __init wmt_tm_init(void)")
        init_end = THERMAL.index("static void __exit wmt_tm_deinit(void)", init_start)
        init_body = THERMAL[init_start:init_end]
        guard_body = (
            "if (echo_v178_suppress_wmt_thermal_registration) {\n"
            '\t\tpr_info("echo-v178: WMT thermal runtime registration suppressed\\n");\n'
            "\t\treturn 0;\n"
            "\t}"
        )
        self.assertIn(guard_body, init_body)
        guard_end = init_body.index(guard_body) + len(guard_body)
        for side_effect in (
            'wmt_tm_printk("[wmt_tm_init] start -->\\n")',
            "wmt_tm_proc_register()",
            "wmt_stats_info.pre_time = 0",
            "init_timer(&wmt_stats_timer)",
            "add_timer(&wmt_stats_timer)",
            "wmt_tm_thz_cl_register()",
        ):
            with self.subTest(side_effect=side_effect):
                self.assertLess(guard_end, init_body.index(side_effect))

    def test_deinit_is_safe_after_suppressed_init(self):
        exit_start = THERMAL.index("static void __exit wmt_tm_deinit(void)")
        exit_end = THERMAL.index("module_init(wmt_tm_init);", exit_start)
        exit_body = THERMAL[exit_start:exit_end]
        guard_body = (
            "if (echo_v178_suppress_wmt_thermal_registration) {\n"
            "\t\treturn;\n"
            "\t}"
        )
        self.assertIn(guard_body, exit_body)
        guard_end = exit_body.index(guard_body) + len(guard_body)
        for side_effect in (
            'wmt_tm_printk("[%s]\\n", __func__)',
            "wmt_tm_thz_cl_unregister()",
            "wmt_tm_proc_unregister()",
            "del_timer(&wmt_stats_timer)",
        ):
            with self.subTest(side_effect=side_effect):
                self.assertLess(guard_end, exit_body.index(side_effect))

    def test_complete_dormant_thermal_implementation_remains_linked(self):
        for token in (
            "module_init(wmt_tm_init);",
            "module_exit(wmt_tm_deinit);",
            "static int wmt_tm_proc_register(void)",
            "static int wmt_tm_proc_unregister(void)",
            "static int wmt_tm_thz_cl_register(void)",
            "static int wmt_tm_thz_cl_unregister(void)",
            "static int wmt_cal_stats(unsigned long data)",
            "init_timer(&wmt_stats_timer)",
            "add_timer(&wmt_stats_timer)",
            "del_timer(&wmt_stats_timer)",
            'mtk_thermal_zone_device_register("mtktswmt"',
            "static int wmt_thz_get_temp(struct thermal_zone_device *thz_dev, unsigned long *pv)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, THERMAL)

        ops_start = THERMAL.index("static struct thermal_zone_device_ops wmt_thz_dev_ops")
        ops_end = THERMAL.index("};", ops_start)
        self.assertIn(".get_temp = wmt_thz_get_temp", THERMAL[ops_start:ops_end])

        get_temp_start = THERMAL.index(
            "static int wmt_thz_get_temp(struct thermal_zone_device *thz_dev, unsigned long *pv)"
        )
        get_temp_end = THERMAL.index("static int wmt_thz_get_mode", get_temp_start)
        self.assertIn("temp = mtk_wcn_cmb_stub_query_ctrl();", THERMAL[get_temp_start:get_temp_end])

    def test_v177_sensor_suppression_and_shared_paths_are_unchanged(self):
        expected = {
            SENSOR_PATH: "f3290af5958de2a1a36ba906726c78920ac345c7b0926dcaa2fe3e9665c52170",
            WMT_LIB_PATH: "a1227ae66897fdd63b6ae9d7665637a13a2b04126d1c5c540fd91ff1e240bd66",
            WMT_FUNC_PATH: "ffb89e87981803ef11c1ca2416a0372a187477b4652b75c277986b68c2cd1224",
            WMT_DEV_PATH: "d1be8209859e3dd5d8e290f97c56c9e41406429c799422b4b4fb1dc8a4ee4633",
            WMT_WIFI_PATH: "bae93e67ee91214b70c9bb60571ac092e591d3df209c3c7c0be68647205d3908",
            RAM_CONSOLE_PATH: "325569a50987205522ce4a40518db8603e011a7a17a84af2ef774cfc67bf8925",
        }
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
