#!/usr/bin/env python3
"""Install or update Kobo Spark on a mounted Libra Colour, leaving its firmware alone."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
from pathlib import Path
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def replace_verified(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".kobo-spark-", delete=False) as f:
        temporary = Path(f.name)
    try:
        shutil.copyfile(source, temporary)
        if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(temporary.read_bytes()).digest():
            raise RuntimeError(f"Copy verification failed: {target.name}")
        temporary.chmod(0o755 if source.name in {"kobo-spark", "launch.sh", "restore-reader.sh"} else 0o644)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)


def install(bundle, volume, config, dry_run=False):
    version_path = volume / ".kobo/version"
    if not version_path.is_file():
        raise RuntimeError("Kobo is not mounted. Connect USB and tap Connect on the reader.")
    version = version_path.read_text().strip().split(",")
    if len(version) < 3 or not version[-1].endswith("390"):
        raise RuntimeError("This app currently supports only the Kobo Libra Colour (device 390).")
    if not (bundle / "kobo-spark").is_file() or not (bundle / "nickelmenu.conf").is_file():
        raise RuntimeError("App bundle missing; run make build first.")
    if not (volume / ".adds/nm").is_dir():
        raise RuntimeError("Install NickelMenu first: https://pgaskin.net/NickelMenu/")
    app = volume / ".adds/kobo-spark"
    if not (app / "dashboard.conf").is_file() and not config.is_file():
        raise RuntimeError("Copy device/dashboard.conf.example to device/dashboard.conf and set your sparkDash URL first.")
    if config.is_file():
        address = config.read_text().strip()
        if not address.startswith(("http://", "https://")) or any(c.isspace() for c in address):
            raise RuntimeError("dashboard.conf must contain one HTTP or HTTPS base URL.")
    sources = sorted(p for p in bundle.rglob("*") if p.is_file()
                     and p.relative_to(bundle).as_posix() not in {"dashboard.conf", "nickelmenu.conf"})
    if dry_run:
        print(f"Would install {len(sources)} app files in {app}, preserving existing configuration and data.")
        return
    for source in sources:
        replace_verified(source, app / source.relative_to(bundle))
    if not (app / "dashboard.conf").is_file():
        replace_verified(config, app / "dashboard.conf")
    replace_verified(bundle / "nickelmenu.conf", volume / ".adds/nm/kobo-spark")
    print(f"Installed and SHA-256 verified {len(sources)} app files in {app}.")
    print("Existing configuration, logs and history retained. Eject safely, unplug, then open NickelMenu → Kobo Spark.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--volume", type=Path, default=Path("/Volumes/KOBOeReader"))
    parser.add_argument("--config", type=Path, default=ROOT / "device/dashboard.conf")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        install(ROOT / "dist/kobo-spark", args.volume, args.config, args.dry_run)
    except (OSError, RuntimeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
