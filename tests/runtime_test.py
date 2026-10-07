import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError("service exited unexpectedly")
        time.sleep(0.02)
    raise AssertionError("timed out waiting for runtime event")


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="bmc-runtime-") as directory:
        root = pathlib.Path(directory)
        config = root / "sensors.conf"
        log = root / "events.jsonl"
        config.write_text("cpu mock 40 1 high 70 90 3 1 3 -\n")
        process = subprocess.Popen(
            [executable, "--config", str(config), "--log", str(log),
             "--interval-ms", "10"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        def events():
            if not log.exists():
                return []
            return [json.loads(line) for line in log.read_text().splitlines()]
        try:
            wait_for(lambda: any(event.get("action") == "started" for event in events()), process)
            config.write_text("cpu mock 95 1 high 70 90 3 1 3 -\n")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: any(event.get("state") == "critical" for event in events()), process)
            config.write_text("invalid configuration\n")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: any("reload rejected" in event.get("action", "") for event in events()), process)
            assert process.poll() is None
            config.write_text("cpu mock 40 1 high 70 90 3 1 3 -\n")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: any(event.get("action") == "validated generation 3" for event in events()), process)
            process.send_signal(signal.SIGTERM)
            _, errors = process.communicate(timeout=5)
            assert process.returncode == 0, errors.decode()
            assert events()[-1]["action"] == "stopped"
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
    print("runtime reload, rejection, and shutdown: passed")


if __name__ == "__main__":
    main()
