import argparse
import json
import os
import struct
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

from test_send import require_exit, run_app


def make_report(root, build, dump_size):
    report = root / str(uuid.uuid4()).upper()
    report.mkdir()
    (report / "report.json").write_text(json.dumps({
        "schemaVersion": 1,
        "reportId": report.name,
        "buildId": build["buildId"],
        "commitSha": build["commitSha"],
        "utc": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "processId": 1,
        "threadId": 1,
        "exceptionCode": "0xC0000094",
        "dumpWritten": True,
        "logWritten": True,
        "synthetic": True,
    }), encoding="utf-8")
    text = b"Synthetic minidump for HTTPS delivery verification.\x00"
    header = struct.pack("<4sIIIIIQ", b"MDMP", 0xA793, 1, 32, 0, 0, 0)
    directory = struct.pack("<III", 10, len(text), 44)
    (report / "crash.dmp").write_bytes((header + directory + text).ljust(dump_size, b"\x00"))
    (report / "engine.log").write_bytes(b"Synthetic report. No process memory or user log.\n")
    return report


def main():
    parser = argparse.ArgumentParser(description="Submit synthetic reports to the endpoint bundled with a Win64 ZIP.")
    parser.add_argument("app", type=Path, help="Extracted app.exe beside crash-report-url.txt.")
    parser.add_argument("evidence", type=Path, help="Local directory for the report source files.")
    options = parser.parse_args()
    app = options.app.resolve(strict=True)
    root = options.evidence.resolve()
    endpoint = urlsplit((app.parent / "crash-report-url.txt").read_text(encoding="utf-8").strip())
    if endpoint.scheme != "https" or not endpoint.hostname:
        parser.error("The package must specify an HTTPS endpoint.")
    root.mkdir(parents=True, exist_ok=True)
    env = {name: value for name, value in os.environ.items() if not name.startswith("TGE_")}
    identity = run_app(app, app.parent, env, "--build-info")
    require_exit(identity, 0, "Reading build identity")
    build = json.loads(identity.stdout)

    for size in (1024, 5 * 1024 * 1024):
        report = make_report(root, build, size)
        for step in ("Sending", "Retrying"):
            sent = run_app(app, app.parent, env, "--send-crash-report", report)
            require_exit(sent, 0, f"{step} {report.name}")
            if report.name.encode() not in sent.stdout:
                raise AssertionError("The client did not print the receipt ID.")
        print(f"PASS: {report.name} build {build['buildId']} dump {size} bytes, send and retry", flush=True)
    print(f"Local files: {root}")


if __name__ == "__main__":
    main()
