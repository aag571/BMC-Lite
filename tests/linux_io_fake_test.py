import os
import tempfile
import unittest

class FakeLinuxIoTest(unittest.TestCase):
    def test_open_failure_is_observable(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = os.path.join(directory, "missing")
            self.assertFalse(os.path.exists(missing))
            with self.assertRaises(FileNotFoundError):
                os.open(missing, os.O_RDONLY)

    def test_descriptor_lifecycle(self):
        with tempfile.NamedTemporaryFile() as file:
            descriptor = os.open(file.name, os.O_RDONLY)
            os.close(descriptor)
            with self.assertRaises(OSError):
                os.close(descriptor)

if __name__ == "__main__":
    unittest.main()
