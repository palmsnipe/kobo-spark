#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch pinned, hash-checked HTTPS dependencies from their official sources."""
from pathlib import Path
import hashlib
import subprocess
import tarfile

root = Path(__file__).resolve().parents[1]
env = dict(line.split("=", 1) for line in (root / "toolchain.env").read_text().splitlines()
           if line and not line.startswith("#"))
downloads = root / "third_party/downloads"
downloads.mkdir(parents=True, exist_ok=True)
items = [
    (f"mbedtls-{env['MBEDTLS_VERSION']}.tar.bz2",
     f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{env['MBEDTLS_VERSION']}/mbedtls-{env['MBEDTLS_VERSION']}.tar.bz2",
     env["MBEDTLS_SHA256"]),
    (f"curl-{env['CURL_VERSION']}.tar.xz",
     f"https://curl.se/download/curl-{env['CURL_VERSION']}.tar.xz", env["CURL_SHA256"]),
]
assert hashlib.sha256((root / "device/cacert.pem").read_bytes()).hexdigest() == env["CA_BUNDLE_SHA256"]
for name, url, expected in items:
    file = downloads / name
    if not file.exists():
        temporary = file.with_suffix(file.suffix + ".download")
        subprocess.run(["curl", "-fLsS", "--connect-timeout", "10", "--max-time", "120",
                        url, "-o", str(temporary)], check=True)
        assert hashlib.sha256(temporary.read_bytes()).hexdigest() == expected, name
        temporary.replace(file)
    assert hashlib.sha256(file.read_bytes()).hexdigest() == expected, name
    if name != "cacert.pem":
        folder = name.split(".tar.")[0]
        if not (root / "third_party" / folder).exists():
            with tarfile.open(file) as archive:
                archive.extractall(root / "third_party", filter="data")
print("HTTPS dependency archives and CA bundle verified")
