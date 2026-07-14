import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GL_INIT_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c"
WMT_WIFI_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/linux/pub/wmt_chrdev_wifi.c"
WMT_FUNC_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_func.c"
WMT_LIB_PATH = ROOT / "drivers/misc/mediatek/connectivity/conn_soc/common/core/wmt_lib.c"
RAM_CONSOLE_PATH = ROOT / "drivers/misc/mediatek/ram_console/mtk_ram_console.c"

GL_INIT = GL_INIT_PATH.read_text()
WMT_WIFI = WMT_WIFI_PATH.read_text()
CONFIG = (ROOT / ".config").read_bytes()


class V176SuppressWlanProbeContract(unittest.TestCase):
    def test_normalized_wifi_config_is_unchanged_from_v175(self):
        self.assertEqual(
            hashlib.sha256(CONFIG).hexdigest(),
            "07e59ee076cda7f5ba25ffe7d3365e145678a50f3fb34074b07938b3f6d815fd",
        )

    def test_only_wlan_boot_init_is_suppressed_before_side_effects(self):
        init_start = GL_INIT.index("static int initWlan(void)")
        init_end = GL_INIT.index("/* end of initWlan() */", init_start)
        init_body = GL_INIT[init_start:init_end]

        self.assertIn(
            "static volatile int echo_v176_suppress_wlan_boot_registration = 1;",
            GL_INIT,
        )
        self.assertIn("echo-v176: WLAN boot registration/probe suppressed", init_body)
        self.assertRegex(
            init_body,
            re.compile(
                r"if \(echo_v176_suppress_wlan_boot_registration\) \{.*?return 0;.*?\}",
                re.S,
            ),
        )
        guard = init_body.index("if (echo_v176_suppress_wlan_boot_registration)")
        for first_side_effect in (
            "spin_lock_init(&g_p2p_lock)",
            "kalInitIOBuffer()",
            "procInitFs()",
            "createWirelessDevice()",
            "glRegisterBus(wlanProbe, wlanRemove)",
            "glRegisterPlatformDev()",
        ):
            self.assertLess(guard, init_body.index(first_side_effect))

    def test_boot_initcall_stays_linked_but_returns_without_registering(self):
        self.assertIn("module_init(initWlan);", GL_INIT)
        self.assertIn("module_exit(exitWlan);", GL_INIT)
        self.assertIn("if (echo_v176_suppress_wlan_boot_registration)", GL_INIT)

    def test_module_exit_is_safe_after_suppressed_init(self):
        exit_start = GL_INIT.index("static VOID exitWlan(void)")
        exit_end = GL_INIT.index("/* end of exitWlan() */", exit_start)
        exit_body = GL_INIT[exit_start:exit_end]
        self.assertRegex(
            exit_body,
            re.compile(
                r"if \(echo_v176_suppress_wlan_boot_registration\) \{.*?return;.*?\}",
                re.S,
            ),
        )
        self.assertLess(
            exit_body.index("if (echo_v176_suppress_wlan_boot_registration)"),
            exit_body.index("glUnregisterPlatformDev()"),
        )

    def test_wmt_wifi_bridge_remains_built_and_registered(self):
        self.assertIn("module_init(WIFI_init);", WMT_WIFI)
        self.assertIn("return WIFI_init();", WMT_WIFI)
        self.assertIn("mtk_wcn_wmt_wifi_soc_init", WMT_WIFI)

    def test_bt_path_and_stage_channel_are_byte_identical_to_v175(self):
        expected = {
            WMT_FUNC_PATH: "ffb89e87981803ef11c1ca2416a0372a187477b4652b75c277986b68c2cd1224",
            WMT_LIB_PATH: "a1227ae66897fdd63b6ae9d7665637a13a2b04126d1c5c540fd91ff1e240bd66",
            RAM_CONSOLE_PATH: "325569a50987205522ce4a40518db8603e011a7a17a84af2ef774cfc67bf8925",
            WMT_WIFI_PATH: "bae93e67ee91214b70c9bb60571ac092e591d3df209c3c7c0be68647205d3908",
        }
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
