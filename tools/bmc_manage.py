"""BMC-Lite 只读 SEL 管理工具，同时是只读响应层的参考实现。

本文件兼有两种身份：

  * 运维工具：``sel-info`` / ``sel-list`` / ``sel-get`` 在本地读取 SEL 文本；
    ``serve`` 在独立端口上给出与 daemon 同形状的 Redfish 资源与 ``/metrics``。
  * 参考实现：src/service.cpp 的响应要与本文件逐字节一致，
    tests/service_vectors.hpp 由 tools/gen_service_vectors.py 调用本文件生成。

因此这里的字段名、JSON 键顺序与 Prometheus 文本都不是随手写的：改动它们等于
改动 C++ 对拍用例的期望值。本文件只读取 SEL，不写入任何文件。
"""
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
    """读取并解析 SEL 文本，返回记录字典列表。

    :param path: SEL 文件路径（pathlib.Path）；文件不存在时返回空列表而不抛异常。
    :return: 按写入顺序排列的 dict 列表，最多 4096 条；每个 dict 的键为
        Id / TimestampMilliseconds / Sensor / State / Message / Value。

    一条记录占一行，用 shlex 语义切分成 6 个字段，因此带引号的传感器名或消息里
    可以含空格。deque(maxlen=4096) 与 daemon 的环形上限一致，只保留最后 4096 条。
    字段数不对或 Id/时间戳/数值非法的行只被丢弃；但文件里出现非 UTF-8 字节时，
    解码失败发生在 for 迭代处而不是 try 内，UnicodeDecodeError 会向外抛出，
    整个读取失败——这是既有行为，不要当成逐行容错。
    """
    result = deque(maxlen=4096)
    if not path.exists():
        return list(result)
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            try:
                fields = shlex.split(line)
                if len(fields) != 6:
                    continue
                # Id 先转 int 再转 str：既拒绝非数字 Id，也把 "007" 归一成 "7"。
                result.append({"Id": str(int(fields[0])), "TimestampMilliseconds": int(fields[1]),
                               "Sensor": fields[2], "State": fields[3], "Message": fields[4],
                               "Value": None if fields[5] == "null" else float(fields[5])})
            except (ValueError, UnicodeError):
                # 非数字的 Id/时间戳/数值、字段数不对的行都只丢弃当前行。
                # 注意：文件解码失败发生在上面 for 迭代处，这里捕获不到。
                continue
    return list(result)


def resource(path, entries):
    """按路径构造 Redfish 形状的响应文档。

    :param path: 已去掉查询串的 URL 路径，例如
        ``/redfish/v1/Managers/BMC/LogServices/SEL``。
    :param entries: records() 返回的记录列表，用来填 Entries 成员与单条记录。
    :return: 可直接交给 json.dumps 的 dict；未知路径返回 None，
        由调用方据此回 404（HTTP 侧映射成 ResourceNotFound）。
    """
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
        # dict(entry, **{...}) 生成副本再补 @odata.id，不修改 records() 返回的对象。
        return {"@odata.id": path, "Members@odata.count": len(entries),
                "Members": [dict(entry, **{"@odata.id": path + "/" + entry["Id"]}) for entry in entries]}
    if path.startswith(base + "/Entries/"):
        identifier = path.rsplit("/", 1)[-1]
        for entry in entries:
            if entry["Id"] == identifier:
                return dict(entry, **{"@odata.id": path})
    # 不存在的路径与不存在的 Id 一律返回 None，调用方统一当作 404。
    return None


def label(value):
    """按 Prometheus 文本格式转义标签值。

    :param value: 任意标签值，实践中是传感器名。
    :return: 转义后的字符串：反斜杠、换行、双引号依次写成 Prometheus 文本格式
        允许的 \\\\、\\n、\\" 三种转义写法。
    """
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def metrics(entries):
    """把 SEL 记录渲染成 Prometheus 文本响应（exposition version=0.0.4）。

    :param entries: records() 返回的记录列表，可以为空（此时只输出 HELP/TYPE 与 0 值）。
    :return: 完整的 /metrics 响应体，末尾带一个换行。

    只有状态名属于 states 的记录参与计算：Sensor=rule、State=active 这类非状态
    记录既不产生状态序列也不产生事件值序列，这正是 tests/metrics_test.py 断言
    ``sensor="rule"`` 不存在的原因。
    """
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
    # 后写覆盖先写，因此 latest 里每个传感器只留最后一次状态迁移。
    for entry in entries:
        if entry["State"] in states:
            latest[entry["Sensor"]] = entry
    # 四个状态全部输出，命中当前状态的为 1，同一传感器只有一条为 1。
    for sensor, entry in sorted(latest.items()):
        for state in states:
            lines.append(f'bmc_sensor_state{{sensor="{label(sensor)}",state="{state}"}} {int(entry["State"] == state)}')
    lines.extend([
        "# HELP bmc_sensor_last_event_value Sensor value at its last recorded state change; not a live sample.",
        "# TYPE bmc_sensor_last_event_value gauge",
    ])
    # 值为 null（非数值记录）或 NaN/Inf 时不输出该传感器的数值序列。
    for sensor, entry in sorted(latest.items()):
        value = entry["Value"]
        if value is not None and math.isfinite(value):
            lines.append(f'bmc_sensor_last_event_value{{sensor="{label(sensor)}"}} {value}')
    lines.extend([
        "# HELP bmc_sensor_last_event_timestamp_seconds Timestamp of the last recorded state change.",
        "# TYPE bmc_sensor_last_event_timestamp_seconds gauge",
    ])
    # 文件里存的是毫秒，Prometheus 的时间序列基数约定为秒。
    for sensor, entry in sorted(latest.items()):
        lines.append(f'bmc_sensor_last_event_timestamp_seconds{{sensor="{label(sensor)}"}} {entry["TimestampMilliseconds"] / 1000}')
    return "\n".join(lines) + "\n"


def serve(path, host, port):
    """启动有界只读 HTTP 服务，阻塞到进程被终止。

    :param path: SEL 文件路径，每个请求都重新读取，因此能看到 daemon 的新写入。
    :param host: 监听地址；只支持 IPv4 回环，main() 会拒绝 ::1。
    :param port: 监听端口。端口被占用时构造 BoundedServer 就失败，无法与 daemon 共用。
    :return: 正常情况下不返回；serve_forever() 只在异常或信号下结束。
    """
    class BoundedServer(ThreadingHTTPServer):
        daemon_threads = True
        # 内核 backlog 32 个排队连接，最多 16 个连接同时被处理。
        request_queue_size = 32
        slots = threading.BoundedSemaphore(16)

        def process_request(self, request, address):
            request.settimeout(5)
            # 非阻塞取位：并发已满就直接关连接，宁可快速失败也不无限排队。
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
                # 无论处理是否抛异常，都在这里归还并发位，保证不泄漏。
                self.slots.release()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                # 只用路径匹配路由，查询串不影响结果。
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
                # resource() 返回 None 表示未知路径，换成 Redfish 风格的错误体。
                body = resource(route, entries)
                status = 200 if body is not None else 404
                if body is None:
                    body = {"error": {"code": "ResourceNotFound", "message": "Unknown resource"}}
            except OSError:
                # 读 SEL 失败（权限、IO 错误）属于存储不可用，用 503 而不是 500。
                status = 503
                body = {"error": {"code": "StorageUnavailable", "message": "SEL unavailable"}}
            # allow_nan=False：值为 NaN/Inf 时 json.dumps 抛 ValueError，连接直接断开，
            # 不会写出非法 JSON。
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
        # 显式关闭监听套接字，便于同一进程内反复启停。
        server.server_close()


def main():
    """解析命令行并执行选中的子命令。

    :return: 无返回值；参数错误以及 sel-get 未命中都经 parser.error() 以退出码 2 结束。
    """
    parser = argparse.ArgumentParser(description="BMC-Lite read-only SEL management")
    # 默认路径与仓库/安装目录下的相对约定一致，可用 --sel 覆盖。
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
        # ::1 通过了 choices，但服务端只支持 IPv4，这里显式拒绝而不是启动后才失败。
        if args.host == "::1":
            parser.error("IPv6 serving is not supported by this server")
        serve(args.sel, args.host, args.port)
    else:
        # 三个查询子命令共用一次读取，差别只在输出哪一部分。
        entries = records(args.sel)
        if args.command == "sel-info":
            # 空文件时 LastId 为 null。
            print(json.dumps({"Entries": len(entries), "LastId": entries[-1]["Id"] if entries else None}))
        elif args.command == "sel-list":
            print(json.dumps(entries, indent=2))
        else:
            match = next((entry for entry in entries if entry["Id"] == args.id), None)
            if match is None:
                # 未命中同样走 parser.error()，退出码 2，与其它用法错误一致。
                parser.error("SEL entry not found")
            print(json.dumps(match, indent=2))


if __name__ == "__main__":
    main()
