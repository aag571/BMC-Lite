import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = str(pathlib.Path(sys.argv.pop(1)).resolve())


class FaultInjectionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="bmc-fault-")
        self.root = pathlib.Path(self.directory.name)
        self.config = self.root / "sensors.conf"
        self.log = self.root / "faults.jsonl"
        self.sel = self.root / "sel.db"
        self.config.write_text("cpu mock 95,95,95,err,err,err,40,40,40 1 high 70 90 3 3 3 -\n")

    def tearDown(self):
        self.directory.cleanup()

    def run_service(self, *options):
        return subprocess.run(
            [EXECUTABLE, "--config", str(self.config), "--log", str(self.log),
             "--sel", str(self.sel), "--rules", str(self.root / "no-rules"),
             "--interval-ms", "1", "--ticks", "9", *options],
            cwd=self.root, capture_output=True, text=True, timeout=10)

    def test_read_failures_transition_and_recover(self):
        result = self.run_service()
        self.assertEqual(result.returncode, 0, result.stderr)
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        transitions = [event["state"] for event in events if "state" in event]
        self.assertEqual(transitions, ["critical", "unavailable", "normal"])
        self.assertTrue(any(event.get("reason") == "read_failure" for event in events))

    def test_corrupt_configuration_refuses_start(self):
        self.config.write_text("corrupt configuration\n")
        result = self.run_service()
        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid config", result.stderr)

    def test_log_device_full_exits_cleanly(self):
        result = self.run_service("--log", "/dev/full")
        self.assertEqual(result.returncode, 1)
        self.assertIn("bmc-lite:", result.stderr)

    def test_sel_directory_is_not_writable_file(self):
        result = self.run_service("--sel", str(self.root))
        self.assertEqual(result.returncode, 1)
        self.assertIn("bmc-lite:", result.stderr)

    def test_missing_sensor_remains_running_until_tick_limit(self):
        self.config.write_text(
            f"cpu sysfs {self.root / 'missing-sensor'} 1 high 70 90 3 1 2 -\n")
        result = self.run_service()
        self.assertEqual(result.returncode, 0, result.stderr)
        events = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([event["state"] for event in events if "state" in event], ["unavailable"])
        self.assertEqual(events[-1]["action"], "stopped")


if __name__ == "__main__":
    unittest.main()
