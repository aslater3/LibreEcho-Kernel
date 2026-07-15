import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SENSOR_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_temp_sensor_wmt.c"
THERMAL_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_ts_wmt.c"
GL_INIT_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c"
WMT_LIB_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c"
WMT_DEV_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/wmt_dev.c"
WMT_WIFI_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pub/wmt_chrdev_wifi.c"

SENSOR = SENSOR_PATH.read_text()
CONFIG = (ROOT / ".config").read_bytes()


class V177SuppressWmtTempSensorContract(unittest.TestCase):
    def test_wifi_capable_config_is_byte_identical_to_v176(self):
        self.assertEqual(
            hashlib.sha256(CONFIG).hexdigest(),
            "07e59ee076cda7f5ba25ffe7d3365e145678a50f3fb34074b07938b3f6d815fd",
        )

    def test_runtime_volatile_gate_precedes_sensor_registration(self):
        self.assertIn(
            "static volatile int echo_v177_suppress_wmt_temp_sensor_registration = 1;",
            SENSOR,
        )
        init_start = SENSOR.index("static int __init mtktswmt_sensor_init(void)")
        init_end = SENSOR.index("static void __exit mtktswmt_sensor_exit(void)", init_start)
        init_body = SENSOR[init_start:init_end]
        self.assertIn("echo-v177: WMT temperature sensor registration suppressed", init_body)
        self.assertRegex(
            init_body,
            re.compile(
                r"if \(echo_v177_suppress_wmt_temp_sensor_registration\) \{.*?return 0;.*?\}",
                re.S,
            ),
        )
        guard = init_body.index("if (echo_v177_suppress_wmt_temp_sensor_registration)")
        self.assertLess(guard, init_body.index("platform_device_register(&mtktswmt_device)"))
        self.assertLess(guard, init_body.index("platform_driver_register(&mtktswmt_driver)"))

    def test_initcall_and_original_sensor_registration_remain_linked(self):
        self.assertIn("module_init(mtktswmt_sensor_init);", SENSOR)
        self.assertIn("module_exit(mtktswmt_sensor_exit);", SENSOR)
        self.assertIn("platform_device_register(&mtktswmt_device)", SENSOR)
        self.assertIn("platform_driver_register(&mtktswmt_driver)", SENSOR)
        self.assertIn("wmt_thz_hw_get_temp()", SENSOR)

    def test_exit_is_safe_after_suppressed_init(self):
        exit_start = SENSOR.index("static void __exit mtktswmt_sensor_exit(void)")
        exit_end = SENSOR.index("module_init(mtktswmt_sensor_init);", exit_start)
        exit_body = SENSOR[exit_start:exit_end]
        self.assertRegex(
            exit_body,
            re.compile(
                r"if \(echo_v177_suppress_wmt_temp_sensor_registration\) \{.*?return;.*?\}",
                re.S,
            ),
        )
        guard = exit_body.index("if (echo_v177_suppress_wmt_temp_sensor_registration)")
        self.assertLess(guard, exit_body.index("platform_device_unregister(&mtktswmt_device)"))
        self.assertLess(guard, exit_body.index("platform_driver_unregister(&mtktswmt_driver)"))

    def test_only_sensor_registration_source_changes_from_v176_surface(self):
        expected = {
            THERMAL_PATH: "15582114d69a3fd9efe85ca3f777590b621375f81169d3865aadfd477bfeec6b",
            GL_INIT_PATH: "d71202b70469d1c1825c1f406b47b226991c25b9f0ff5e4c5d1bf2b3f0f9a73a",
            WMT_LIB_PATH: "a1227ae66897fdd63b6ae9d7665637a13a2b04126d1c5c540fd91ff1e240bd66",
            WMT_DEV_PATH: "d1be8209859e3dd5d8e290f97c56c9e41406429c799422b4b4fb1dc8a4ee4633",
            WMT_WIFI_PATH: "bae93e67ee91214b70c9bb60571ac092e591d3df209c3c7c0be68647205d3908",
        }
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
