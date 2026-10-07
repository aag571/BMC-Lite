import argparse
import json
import pathlib
import shlex
import math
import threading
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


def records(path):
    result = deque(maxlen=4096)
    if not path.exists():
        return list(result)
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            try:
                fields = shlex.split(line)
                if len(fields) != 6:
                    continue
                result.append({"Id": str(int(fields[0])), "TimestampMilliseconds": int(fields[1]),
                               "Sensor": fields[2], "State": fields[3], "Message": fields[4],
                               "Value": None if fields[5] == "null" else float(fields[5])})
            except (ValueError, UnicodeError):
                continue
    return list(result)


def resource(path, entries):
    base = "/redfish/v1/Managers/BMC/LogServices/SEL"
    if path == "/redfish/v1/":
        return {"@odata.id": path, "Id": "RootService", "Name": "BMC-Lite management",
                "Managers": {"@odata.id": "/redfish/v1/Managers"}}
    if path == "/redfish/v1/Managers":
        return {"@odata.id": path, "Members@odata.count": 1,
                "Members": [{"@odata.id": "/redfish/v1/Managers/BMC"}]}
    if path == "/redfish/v1/Managers/BMC":
        return {"@odata.id": path, "Id": "BMC", "Name": "BMC-Lite",
                "LogServices": {"@odata.id": "/redfish/v1/Managers/BMC/LogServices"}}
    if path == "/redfish/v1/Managers/BMC/LogServices":
        return {"@odata.id": path, "Members@odata.count": 1, "Members": [{"@odata.id": base}]}
    if path == base:
        return {"@odata.id": path, "Id": "SEL", "Name": "System event log",
                "Entries": {"@odata.id": base + "/Entries"}}
    if path == base + "/Entries":
        return {"@odata.id": path, "Members@odata.count": len(entries),
                "Members": [dict(entry, **{"@odata.id": path + "/" + entry["Id"]}) for entry in entries]}
    if path.startswith(base + "/Entries/"):
        identifier = path.rsplit("/", 1)[-1]
        for entry in entries:
            if entry["Id"] == identifier:
                return dict(entry, **{"@odata.id": path})
    return None


def label(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def metrics(entries):
    lines = [
        "# HELP bmc_sel_records Number of readable records in the SEL file.",
        "# TYPE bmc_sel_records gauge",
        f"bmc_sel_records {len(entries)}",
        "# HELP bmc_sel_last_id Highest readable SEL record identifier.",
        "# TYPE bmc_sel_last_id gauge",
        f"bmc_sel_last_id {max((int(entry['Id']) for entry in entries), default=0)}",
        "# HELP bmc_sensor_state Last recorded sensor state; exactly one state is 1.",
        "# TYPE bmc_sensor_state gauge",
    ]
    states = ("normal", "warning", "critical", "unavailable")
    latest = {}
    for entry in entries:
        if entry["State"] in states:
            latest[entry["Sensor"]] = entry
    for sensor, entry in sorted(latest.items()):
        for state in states:
            lines.append(f'bmc_sensor_state{{sensor="{label(sensor)}",state="{state}"}} {int(entry["State"] == state)}')
    lines.extend([
        "# HELP bmc_sensor_last_event_value Sensor value at its last recorded state change; not a live sample.",
        "# TYPE bmc_sensor_last_event_value gauge",
    ])
    for sensor, entry in sorted(latest.items()):
        value = entry["Value"]
        if value is not None and math.isfinite(value):
            lines.append(f'bmc_sensor_last_event_value{{sensor="{label(sensor)}"}} {value}')
    lines.extend([
        "# HELP bmc_sensor_last_event_timestamp_seconds Timestamp of the last recorded state change.",
        "# TYPE bmc_sensor_last_event_timestamp_seconds gauge",
    ])
    for sensor, entry in sorted(latest.items()):
        lines.append(f'bmc_sensor_last_event_timestamp_seconds{{sensor="{label(sensor)}"}} {entry["TimestampMilliseconds"] / 1000}')
    return "\n".join(lines) + "\n"


def serve(path, host, port):
    class BoundedServer(ThreadingHTTPServer):
        daemon_threads = True
        request_queue_size = 32
        slots = threading.BoundedSemaphore(16)

        def process_request(self, request, address):
            request.settimeout(5)
            if not self.slots.acquire(blocking=False):
                self.shutdown_request(request)
                return
            try:
                super().process_request(request, address)
            except Exception:
                self.slots.release()
                raise

        def process_request_thread(self, request, address):
            try:
                super().process_request_thread(request, address)
            finally:
                self.slots.release()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                route = urlsplit(self.path).path
                entries = records(path)
                if route == "/metrics":
                    payload = metrics(entries).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(payload)
                    return
                body = resource(route, entries)
                status = 200 if body is not None else 404
                if body is None:
                    body = {"error": {"code": "ResourceNotFound", "message": "Unknown resource"}}
            except OSError:
                status = 503
                body = {"error": {"code": "StorageUnavailable", "message": "SEL unavailable"}}
            payload = json.dumps(body, allow_nan=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
    server = BoundedServer((host, port), Handler)
    try:
        server.serve_forever()
    finally:
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description="BMC-Lite read-only SEL management")
    parser.add_argument("--sel", type=pathlib.Path, default=pathlib.Path("var/sel.db"))
    commands = parser.add_subparsers(dest="command", required=True)
    web = commands.add_parser("serve")
    web.add_argument("--host", choices=["127.0.0.1", "::1"], default="127.0.0.1")
    web.add_argument("--port", type=int, default=8000)
    commands.add_parser("sel-info")
    commands.add_parser("sel-list")
    get = commands.add_parser("sel-get")
    get.add_argument("id")
    args = parser.parse_args()
    if args.command == "serve":
        if args.host == "::1":
            parser.error("IPv6 serving is not supported by this server")
        serve(args.sel, args.host, args.port)
    else:
        entries = records(args.sel)
        if args.command == "sel-info":
            print(json.dumps({"Entries": len(entries), "LastId": entries[-1]["Id"] if entries else None}))
        elif args.command == "sel-list":
            print(json.dumps(entries, indent=2))
        else:
            match = next((entry for entry in entries if entry["Id"] == args.id), None)
            if match is None:
                parser.error("SEL entry not found")
            print(json.dumps(match, indent=2))


if __name__ == "__main__":
    main()
