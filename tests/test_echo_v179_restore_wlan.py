import hashlib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GL_INIT_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c"
AHB_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/hif/ahb/ahb.c"
WMT_EXP_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pri/wmt_exp.c"
WMT_FUNC_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c"
WMT_LIB_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c"
RAM_CONSOLE_PATH = ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c"
WMT_THERMAL_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_ts_wmt.c"
WMT_SENSOR_PATH = ROOT / "drivers/misc/mediatek/thermal/mt8163/mtk_temp_sensor_wmt.c"

GL_INIT = GL_INIT_PATH.read_text()
AHB = AHB_PATH.read_text()
WMT_EXP = WMT_EXP_PATH.read_text()
WMT_THERMAL = WMT_THERMAL_PATH.read_text()
WMT_SENSOR = WMT_SENSOR_PATH.read_text()
CONFIG = (ROOT / ".config").read_bytes()


def function_body(source: str, declaration: str, end_marker: str) -> str:
    start = source.index(declaration)
    end = source.index(end_marker, start)
    return source[start:end]


class V179RestoreWlanContract(unittest.TestCase):
    def test_wifi_capable_config_is_unchanged_from_v178(self):
        self.assertEqual(
            hashlib.sha256(CONFIG).hexdigest(),
            "07e59ee076cda7f5ba25ffe7d3365e145678a50f3fb34074b07938b3f6d815fd",
        )

    def test_v176_runtime_gate_is_completely_removed(self):
        self.assertNotIn("echo_v176_suppress_wlan_boot_registration", GL_INIT)
        self.assertNotIn("echo-v176: WLAN boot registration/probe suppressed", GL_INIT)
        self.assertNotIn("echo-v176: WLAN exit skipped", GL_INIT)

    def test_initwlan_executes_registration_path(self):
        body = function_body(GL_INIT, "static int initWlan(void)", "/* end of initWlan() */")
        required_in_order = (
            "spin_lock_init(&g_p2p_lock)",
            "kalInitIOBuffer()",
            "procInitFs()",
            "createWirelessDevice()",
            "glRegisterBus(wlanProbe, wlanRemove)",
            "register_set_dbg_level_handler(set_dbg_level_handler)",
            "glRegisterPlatformDev()",
        )
        positions = [body.index(token) for token in required_in_order]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("return 0;", body[: body.index("spin_lock_init(&g_p2p_lock)")])
        self.assertTrue(body.rstrip().endswith("return ret;\n}"))
        self.assertIn("module_init(initWlan);", GL_INIT)

    def test_exitwlan_executes_symmetric_teardown(self):
        body = function_body(GL_INIT, "static VOID exitWlan(void)", "/* end of exitWlan() */")
        required_in_order = (
            "glUnregisterPlatformDev()",
            "register_set_dbg_level_handler(NULL)",
            "destroyWirelessDevice()",
            "glP2pDestroyWirelessDevice()",
            "glUnregisterBus(wlanRemove)",
            "kalUninitIOBuffer()",
            "procUninitProcFs()",
        )
        positions = [body.index(token) for token in required_in_order]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("return;", body[: body.index("glUnregisterPlatformDev()")])
        self.assertIn("module_exit(exitWlan);", GL_INIT)

    def test_bus_registration_populates_wmt_probe_callback(self):
        self.assertIn("WLAN_STATUS glRegisterBus(probe_card pfProbe, remove_card pfRemove)", AHB)
        self.assertIn("platform_driver_register(&MtkPltmAhbDriver)", AHB)
        self.assertIn("WmtCb.wlan_probe_cb = HifAhbProbe;", AHB)
        self.assertIn("WmtCb.wlan_remove_cb = HifAhbRemove;", AHB)
        self.assertIn("i4WmtRet = mtk_wcn_wmt_wlan_reg(&WmtCb);", AHB)
        self.assertIn("mtk_wcn_wlan_probe = pWmtWlanCbInfo->wlan_probe_cb;", WMT_EXP)
        self.assertIn("mtk_wcn_wlan_remove = pWmtWlanCbInfo->wlan_remove_cb;", WMT_EXP)

    def test_wlan_probe_contains_cfg80211_and_netdev_creation_paths(self):
        self.assertIn("wiphy_register(prWiphy)", GL_INIT)
        self.assertIn("register_netdev(prWdev->netdev)", GL_INIT)
        self.assertIn("static INT_32 wlanProbe(PVOID pvData)", GL_INIT)
        self.assertIn("wlanNetCreate", GL_INIT)
        self.assertIn("wlanAdapterStart", GL_INIT)

    def test_both_thermal_suppressions_remain_active(self):
        self.assertIn("static volatile int echo_v178_suppress_wmt_thermal_registration = 1;", WMT_THERMAL)
        self.assertIn("echo-v178: WMT thermal runtime registration suppressed", WMT_THERMAL)
        self.assertIn("static volatile int echo_v177_suppress_wmt_temp_sensor_registration = 1;", WMT_SENSOR)
        self.assertIn("echo-v177: WMT temperature sensor registration suppressed", WMT_SENSOR)

    def test_non_wlan_discriminator_sources_are_byte_identical_to_v178(self):
        expected = {
            WMT_THERMAL_PATH: "34cfb0c3f8c2bea2f9ca4a5873e188279308c90aa84b7831ee8997d08140fbfc",
            WMT_SENSOR_PATH: "f3290af5958de2a1a36ba906726c78920ac345c7b0926dcaa2fe3e9665c52170",
            WMT_EXP_PATH: "ae762737e982e0ff808f8f2b6dfc16cc2fc605a68f27416660d84f7a85e88fcd",
            WMT_FUNC_PATH: "ffb89e87981803ef11c1ca2416a0372a187477b4652b75c277986b68c2cd1224",
            WMT_LIB_PATH: "a1227ae66897fdd63b6ae9d7665637a13a2b04126d1c5c540fd91ff1e240bd66",
            RAM_CONSOLE_PATH: "325569a50987205522ce4a40518db8603e011a7a17a84af2ef774cfc67bf8925",
            AHB_PATH: "1518702bba826b0bcec56b9c28e95c0d77865aa69847e5b5d24b78dfe18e750e",
        }
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
