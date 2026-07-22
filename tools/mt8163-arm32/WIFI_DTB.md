# Pinned MT8163 Wi-Fi candidate DTB

`build_wifi_dtb.py` extracts the exact EVT DTB from the pinned v184 stock
16 MiB boot envelope and adds only these properties to
`/soc/consys@18070000`:

```dts
clocks = <0x5 0x3>;
clock-names = "bus";
```

It deliberately retains the stock three-resource CONSYS tuple, including the
`0x10001000` base used by the active genpd driver's EMI-remap path.  Before
writing an output, the tool validates the source and stock hashes, confirms
both new properties were absent, applies them with `fdtput`, reads the final
values with `fdtget`, enforces the 64 KiB LK limit, and pins the complete final
DTB hash.  It refuses to overwrite an existing output.

```sh
python3 -B tools/mt8163-arm32/build_wifi_dtb.py \
  --source-boot /home/andy/workspace/echo-evidence/v184-stock32-parity/boot-v184-stock32-parity-stock.img \
  --output /tmp/giza-evt-stock-bus-clock.dtb
```

The known output from dtc tools 1.7.0 is 51,353 bytes:

```text
d5e8b62e14956fb6402c510bfbc784e2e82479daa3183c32cac1e7bc139e9f04
```

This is a raw DTB input for the recovery-image builder, not a boot image and
not a flashable artifact by itself.
