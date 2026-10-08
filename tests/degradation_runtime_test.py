"""Verify logger degradation is visible in SEL while the daemon is still running."""
import pathlib
import shlex
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="bmc-log-degradation-") as directory:
    root = pathlib.Path(directory)
    config = root / "config"
    sel = root / "sel"
    config.write_text("cpu mock 95 1 high 70 90 3 1 1 -\n")
    process = subprocess.Popen([binary, "--config", str(config), "--rules", str(root / "none"),
        "--log", "/dev/full", "--sel", str(sel), "--interval-ms", "10"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    def reports():
        if not sel.exists():
            return []
        records = [shlex.split(line) for line in sel.read_text().splitlines()]
        return [record for record in records if len(record) >= 6 and record[2:4] == ["log", "degraded"]]
    try:
        deadline = time.monotonic() + 3
        while not reports():
            assert process.poll() is None
            assert time.monotonic() < deadline, "logger degradation was not persisted"
            time.sleep(.02)
        first = reports()[0][4]
        time.sleep(.3)
        assert len(reports()) == 1, "continuous retries spammed SEL"
        deadline = time.monotonic() + 6
        while len(reports()) < 2:
            assert process.poll() is None
            assert time.monotonic() < deadline
            time.sleep(.05)
        assert reports()[1][4] != first, "updated failure counters were not reported"
        assert len(reports()) == 2
    finally:
        process.terminate()
        _, errors = process.communicate(timeout=3)
        assert process.returncode == 1 and b"log flush failed" in errors, errors
print("runtime logger degradation, retry throttling and persistent counters: passed")
