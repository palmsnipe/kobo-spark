# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the real recovery shell with an isolated reader/process simulator."""
from pathlib import Path
import os
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.app = self.root / "app"
        self.app.mkdir()
        self.lock = self.root / "lock"
        self.lock.mkdir()
        (self.lock / "reader-env").write_text("LANG=C\n")
        (self.lock / "active").touch()
        (self.lock / "heartbeat").touch()
        self.unlock = self.root / "wake_unlock"
        self.unlock.write_text("held")
        self.state = self.root / "processes"
        self.state.touch()
        self.events = self.root / "events"
        self.events.touch()
        source = (ROOT / "device/restore-reader.sh").read_text()
        source = source.replace("/mnt/onboard/.adds/kobo-spark", str(self.app))
        source = source.replace("/tmp/kobo-spark.lock", str(self.lock))
        source = source.replace("/sys/power/wake_unlock", str(self.unlock))
        source = source.replace("/tmp/nickel-hardware-status", str(self.root / "hardware-status"))
        script = self.app / "restore-reader.sh"
        script.write_text(source)
        self.script = script
        commands = self.root / "bin"
        commands.mkdir()
        stub = """#!/usr/bin/env python3
import os, sys
from pathlib import Path
name=Path(sys.argv[0]).name
state=Path(os.environ['TEST_PROCESSES'])
if name == 'pidof':
    if sys.argv[1] in state.read_text().splitlines():
        print(1234); sys.exit(0)
    sys.exit(1)
if name == 'nohup':
    child=Path(sys.argv[1]).name
    with open(os.environ['TEST_EVENTS'], 'a') as f: f.write(child+'\\n')
    if child != 'nickel' or os.environ['TEST_READER_STARTS'] == 'yes':
        with state.open('a') as f: f.write(child+'\\n')
if name == 'killall':
    names={s for s in sys.argv[1:] if not s.startswith('-')}
    state.write_text(''.join(s+'\\n' for s in state.read_text().splitlines() if s not in names))
if name == 'stat':
    print(int(Path(sys.argv[-1]).stat().st_mtime))
"""
        for name in ("pidof", "nohup", "sleep", "stat", "killall"):
            path = commands / name
            path.write_text(stub)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                        TEST_PROCESSES=str(self.state), TEST_EVENTS=str(self.events),
                        TEST_READER_STARTS="yes")

    def prepare_launcher(self, unkillable=False):
        self.state.write_text("nickel\nhindenburg\nsickel\n")
        for child in self.lock.iterdir():
            child.unlink()
        self.lock.rmdir()
        proc = self.root / "proc/1234"
        proc.mkdir(parents=True)
        (proc / "environ").write_bytes(b"LANG=C\x00")
        launch = (ROOT / "device/launch.sh").read_text()
        launch = launch.replace("/mnt/onboard/.adds/kobo-spark", str(self.app))
        launch = launch.replace("/tmp/kobo-spark.lock", str(self.lock))
        launch = launch.replace("/proc/$reader/environ", str(self.root / "proc/$reader/environ"))
        launch = launch.replace("/sys/power/wake_lock", str(self.root / "wake_lock"))
        launch = launch.replace("/tmp/nickel-hardware-status", str(self.root / "hardware-status"))
        launcher = self.app / "launch.sh"
        launcher.write_text(launch)
        self.script.chmod(0o755)
        app = self.app / "kobo-spark"
        app.write_text('#!/bin/sh\n! pidof nickel >/dev/null || exit 9\n! pidof sickel >/dev/null || exit 10\necho app >> "$TEST_EVENTS"\n')
        app.chmod(0o755)
        killer = self.root / "bin/killall"
        killer.write_text("""#!/usr/bin/env python3
import os, sys
from pathlib import Path
state=Path(os.environ['TEST_PROCESSES'])
signal='KILL' if '-KILL' in sys.argv else 'TERM'
names={s for s in sys.argv[1:] if not s.startswith('-')}
with open(os.environ['TEST_EVENTS'],'a') as f: f.write(signal+':'+','.join(sorted(names))+'\\n')
survivors=[s for s in state.read_text().splitlines() if s not in names or
    (s=='sickel' and (signal=='TERM' or os.environ['TEST_UNKILLABLE']=='yes'))]
state.write_text(''.join(s+'\\n' for s in survivors))
""")
        killer.chmod(0o755)
        self.env["TEST_UNKILLABLE"] = "yes" if unkillable else "no"
        return launcher

    def test_stubborn_watchdog_stopped_before_reader_and_app(self):
        launcher = self.prepare_launcher()
        result = subprocess.run(["sh", str(launcher)], env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        events = self.events.read_text().splitlines()
        killed = next(i for i, event in enumerate(events) if event.startswith("KILL:") and "sickel" in event)
        reader = next(i for i, event in enumerate(events) if event.startswith("TERM:") and "nickel" in event)
        self.assertLess(killed, reader)
        self.assertLess(reader, events.index("app"))
        self.assertFalse(self.lock.exists())

    def test_failure_to_stop_watchdog_keeps_reader_and_never_runs_app(self):
        launcher = self.prepare_launcher(unkillable=True)
        result = subprocess.run(["sh", str(launcher)], env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        events = self.events.read_text().splitlines()
        self.assertNotIn("app", events)
        self.assertFalse(any(event.startswith("TERM:") and "nickel" in event for event in events))
        self.assertIn("nickel", self.state.read_text().splitlines())
        self.assertFalse(self.lock.exists())

    def tearDown(self):
        self.temp.cleanup()

    def run_recovery(self):
        return subprocess.run(["sh", str(self.script)], env=self.env,
                              capture_output=True, text=True, timeout=10)

    def test_hardware_service_fifo_and_events_restored_then_idempotence(self):
        result = self.run_recovery()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        # The supervisor is backgrounded; let its simulator finish writing.
        deadline = time.monotonic() + 2
        while "hindenburg" not in self.events.read_text() and time.monotonic() < deadline:
            time.sleep(0.01)
        events=self.events.read_text().splitlines()
        self.assertCountEqual(events, ["nickel", "hindenburg", "udevadm"])
        self.assertTrue((self.root / "hardware-status").is_fifo())
        self.assertFalse(self.lock.exists())
        self.assertEqual(self.unlock.read_text().strip(), "kobo-spark")
        self.assertEqual(self.run_recovery().returncode, 0)
        self.assertEqual(self.events.read_text().splitlines(), events)

    def test_failed_start_preserves_recovery_state_and_can_retry(self):
        self.env["TEST_READER_STARTS"] = "no"
        result = self.run_recovery()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertTrue((self.lock / "active").exists())
        self.assertTrue((self.lock / "reader-env").exists())
        self.assertFalse((self.lock / "recovering").exists())
        self.assertEqual(self.unlock.read_text(), "held")
        self.env["TEST_READER_STARTS"] = "yes"
        result = self.run_recovery()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertFalse(self.lock.exists())


if __name__ == "__main__":
    unittest.main()
