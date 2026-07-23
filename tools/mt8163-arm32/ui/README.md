# LibreEcho-UI image integration

The web application remains in the separate `LibreEcho-UI` repository. This
directory owns the image-side build and packaging contract.

`build_ui_bundle.sh` accepts an explicit UI checkout and stages static ARM32
daemons, web assets, init scripts, and the default configuration. The image
records the UI commit, source diff identity, and a deterministic file manifest.

The daemons are packaged as default boot services after the recovery control
plane has configured loopback. They do not replace the existing kernel or
initramfs control plane:

- `networkd` attaches to the existing `wpa_supplicant` control socket.
- `audiod` uses the existing ALSA device and mixer controls.
- `ledd` uses the existing LED sysfs/I2C controls.
- `libreecho-web` exposes those adapters over the local HTTP API.

The image init script starts `logd`, `networkd`, `audiod`, `ledd`, and `web` in
that order. Each service is idempotent and can still be restarted through its
init script for development.
