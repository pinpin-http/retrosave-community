#!/usr/bin/env python3
"""Align and sign an Android release with the maintainer's persistent key."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sdk = Path(os.environ["ANDROID_HOME"])
build_tools = sdk / "build-tools" / "35.0.0"
source = root / "android/app/build/outputs/apk/release/app-release-unsigned.apk"
version = (root / "VERSION").read_text().strip()
destination = root / "dist" / f"RetroSave-Community-{version}-android.apk"
destination.parent.mkdir(exist_ok=True)
aligned = destination.with_suffix(".aligned.apk")
try:
    subprocess.run(
        [str(build_tools / "zipalign"), "-f", "-P", "16", "4", str(source), str(aligned)],
        check=True,
    )
    subprocess.run(
        [
            str(build_tools / "apksigner"),
            "sign",
            "--ks",
            os.environ["RSC_KEYSTORE"],
            "--ks-key-alias",
            "community",
            "--ks-pass",
            "env:RSC_KEYSTORE_PASSWORD",
            "--key-pass",
            "env:RSC_KEYSTORE_PASSWORD",
            "--out",
            str(destination),
            str(aligned),
        ],
        check=True,
    )
    subprocess.run(
        [str(build_tools / "apksigner"), "verify", "--verbose", "--print-certs", str(destination)],
        check=True,
    )
    subprocess.run(
        [str(build_tools / "zipalign"), "-c", "-P", "16", "4", str(destination)], check=True
    )
finally:
    aligned.unlink(missing_ok=True)
