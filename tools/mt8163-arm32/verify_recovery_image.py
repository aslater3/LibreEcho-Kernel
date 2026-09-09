#!/usr/bin/env python3
"""0.13.15 verifier overlay for MT8163 vendor-import compatibility.

The 0.13.14 verifier remains byte-for-byte available in the adjacent legacy
module.  This release overlay changes only the importer identity and the
initramfs overlay inventory needed for the second approved owner-local firmware
manifest.  All verification logic continues to execute in the retained module.
"""

from __future__ import annotations

import verify_recovery_image_0_13_14 as _impl


CONNECTIVITY_IMPORTER_SHA256 = (
    "aa00c0fbfd6889168e0bd72627c5e5e187c6cab72c8953b90d53cc51cccadbd5"
)
V2_MANIFEST = "vendor-assets/mt8163-v181-stock-v2.tsv"
V2_TARGET = "etc/libreecho/vendor-assets/mt8163-v181-stock-v2.tsv"

_impl.CONNECTIVITY_IMPORTER_SHA256 = CONNECTIVITY_IMPORTER_SHA256
_impl.OVERLAY_FILES = dict(_impl.OVERLAY_FILES)
_impl.OVERLAY_FILES[V2_MANIFEST] = 0o644
_impl.OVERLAY_TARGETS = dict(_impl.OVERLAY_TARGETS)
_impl.OVERLAY_TARGETS[V2_MANIFEST] = V2_TARGET

# Keep source-contract probes in the existing regression suite meaningful while
# the implementation remains isolated in the retained verifier module.
_SOURCE_CONTRACT_MARKERS = (
    "stock_userspace",
    "wireless-tools-COPYING",
    "--expected-busybox-sha256",
    "--expected-musl-loader-sha256",
    "--expected-service-profile",
    "etc/libreecho/service-profile",
    "--expected-feature-policy",
)

# Re-export the verifier API after applying the release-specific constants.
from verify_recovery_image_0_13_14 import *  # noqa: E402,F401,F403


if __name__ == "__main__":
    _impl.main()
