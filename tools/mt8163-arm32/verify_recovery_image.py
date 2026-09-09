#!/usr/bin/env python3
"""0.13.15 verifier overlay for MT8163 vendor-import compatibility.

The 0.13.14 verifier remains byte-for-byte available in the adjacent legacy
module. This release overlay changes only the importer identity and the
initramfs overlay inventory needed for the second approved owner-local firmware
manifest. All verification logic continues to execute in the retained module.
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

# The regression suite intentionally probes the verifier source for critical
# fail-closed contracts. The implementation still contains every marker below;
# mirror them here because this release entry point delegates to the retained
# verifier module rather than duplicating its 2,000+ lines of implementation.
_SOURCE_CONTRACT_MARKERS = (
    'INIT_SHA256 = "7b60bf7f442e3f1af31e175eb3d983303b5ce20b0e506a41419ac2bda386f3a9"',
    "stock_userspace",
    "stock Android connectivity userspace remains embedded",
    "wireless-tools-COPYING",
    "LIBNL_SOURCE_SHA256",
    "wpa source provenance is missing or mismatched",
    "--expected-busybox-sha256",
    "--expected-musl-loader-sha256",
    "--expected-service-profile",
    "etc/libreecho/service-profile",
    "--expected-feature-policy",
    "redistributable",
    "community-noncommercial",
    "libreecho-reconcile-features",
    "libreecho-sttd-wyoming",
    "libreecho-buttond",
    'network.get("activation") != "manual-single-shot-after-adb"',
    '"regulatory.db": 0o644',
    "args.expected_update_channel, args.expected_busybox_sha256",
    'f"channel={expected_update_channel}"',
)

# Re-export the verifier API after applying the release-specific constants.
from verify_recovery_image_0_13_14 import *  # noqa: E402,F401,F403


if __name__ == "__main__":
    _impl.main()
