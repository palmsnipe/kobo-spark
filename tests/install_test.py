# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("install_usb", Path(__file__).resolve().parents[1] / "scripts/install-usb.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def test_install_update_preserves_config_data_and_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            volume, bundle = root / "volume", root / "bundle"
            (volume / ".kobo").mkdir(parents=True)
            (volume / ".kobo/version").write_text("serial,release,4.45.23697,390")
            (volume / ".kobo/KoboRoot.tgz").write_bytes(b"pending firmware")
            (volume / ".adds/nm").mkdir(parents=True)
            (volume / ".adds/nm/other-app").write_text("other menu item")
            bundle.mkdir()
            (bundle / "kobo-spark").write_bytes(b"app v1")
            (bundle / "nickelmenu.conf").write_text("Kobo Spark menu item")
            config = root / "dashboard.conf"
            config.write_text("http://example.local:5555\n")
            installer.install(bundle, volume, config, dry_run=True)
            app = volume / ".adds/kobo-spark"
            self.assertFalse(app.exists())
            installer.install(bundle, volume, config)
            self.assertEqual((app / "dashboard.conf").read_bytes(), config.read_bytes())
            (app / "app.log").write_text("keep logs")
            (app / "rates.csv").write_text("keep history")
            config.write_text("http://different.local:5555\n")
            (bundle / "kobo-spark").write_bytes(b"app v2")
            installer.install(bundle, volume, config)
            self.assertEqual((app / "kobo-spark").read_bytes(), b"app v2")
            self.assertEqual((app / "dashboard.conf").read_text(), "http://example.local:5555\n")
            self.assertEqual((app / "app.log").read_text(), "keep logs")
            self.assertEqual((app / "rates.csv").read_text(), "keep history")
            self.assertEqual((volume / ".kobo/KoboRoot.tgz").read_bytes(), b"pending firmware")
            self.assertEqual((volume / ".adds/nm/other-app").read_text(), "other menu item")

    def test_reject_unmounted_device(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "not mounted"):
                installer.install(root, root, root / "config")


if __name__ == "__main__":
    unittest.main()
