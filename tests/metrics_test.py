import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location(
    "manage", pathlib.Path(__file__).resolve().parents[1] / "tools" / "bmc_manage.py")
manage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manage)


class MetricsTest(unittest.TestCase):
    def test_empty_store(self):
        output = manage.metrics([])
        self.assertIn("bmc_sel_records 0\n", output)
        self.assertIn("bmc_sel_last_id 0\n", output)

    def test_latest_state_and_escaped_labels(self):
        entries = [
            {"Id": "1", "TimestampMilliseconds": 1000, "Sensor": 'cpu"',
             "State": "critical", "Value": 95},
            {"Id": "2", "TimestampMilliseconds": 2000, "Sensor": 'cpu"',
             "State": "normal", "Value": 40},
            {"Id": "3", "TimestampMilliseconds": 3000, "Sensor": "rule",
             "State": "active", "Value": None},
        ]
        output = manage.metrics(entries)
        self.assertIn('sensor="cpu\\\"",state="normal"} 1', output)
        self.assertIn('sensor="cpu\\\"",state="critical"} 0', output)
        self.assertIn('bmc_sensor_last_event_value{sensor="cpu\\\""} 40', output)
        self.assertNotIn('sensor="rule"', output)


if __name__ == "__main__":
    unittest.main()
