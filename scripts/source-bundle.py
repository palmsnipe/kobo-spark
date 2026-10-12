#!/usr/bin/env python3
"""Package the app and its pinned dependency sources for GPL binary distribution."""
# SPDX-License-Identifier: GPL-3.0-or-later
import io
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    env = dict(line.split("=", 1) for line in (ROOT / "toolchain.env").read_text().splitlines()
               if line and not line.startswith("#"))
    fbink = ROOT / "third_party/FBInk"
    if not (fbink / "fbink.c").is_file():
        raise SystemExit("Run make build before creating the corresponding-source archive.")
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    if "src/dashboard.c" not in tracked:
        raise SystemExit("The project sources must be tracked in Git before packaging them.")
    dependencies = [ROOT / f"third_party/downloads/mbedtls-{env['MBEDTLS_VERSION']}.tar.bz2",
                    ROOT / f"third_party/downloads/curl-{env['CURL_VERSION']}.tar.xz"]
    if not all(p.is_file() for p in dependencies):
        raise SystemExit("Dependency archives are missing; run make build first.")
    output = ROOT / "dist/kobo-spark-source.tar.gz"
    output.parent.mkdir(exist_ok=True)
    with tarfile.open(output, "w:gz") as archive:
        for relative in sorted(p for p in tracked if p):
            path = ROOT / relative
            if path.is_file():
                archive.add(path, arcname="kobo-spark/" + relative, recursive=False)
        for path in dependencies:
            archive.add(path, arcname="kobo-spark/" + path.relative_to(ROOT).as_posix(), recursive=False)
        for path in sorted(fbink.rglob("*")):
            relative = path.relative_to(fbink)
            if ".git" in relative.parts or "Release" in relative.parts or not path.is_file():
                continue
            archive.add(path, arcname="kobo-spark/third_party/FBInk/" + relative.as_posix(), recursive=False)
        marker = (env["FBINK_COMMIT"] + "\n").encode()
        info = tarfile.TarInfo("kobo-spark/third_party/FBInk/.kobo-spark-commit")
        info.size = len(marker)
        info.mode = 0o644
        archive.addfile(info, io.BytesIO(marker))
    print(f"Corresponding source: {output}")


if __name__ == "__main__":
    main()
