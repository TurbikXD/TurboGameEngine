import argparse
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import zipfile
from pathlib import Path

from receive import CrashInbox, ENTRY_NAMES, ROUTE, ReportHandler


PROJECT = Path(__file__).resolve().parents[2] / "demos" / "lab5-beacon" / "Lab5Beacon.tgeproj"


def run_app(app, cwd, environment, *arguments):
    return subprocess.run(
        [str(app), *map(str, arguments)],
        cwd=cwd,
        env=environment,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
        timeout=90,
    )


def require_exit(result, expected, step):
    code = result.returncode & 0xFFFFFFFF
    if code != expected:
        excerpt = (result.stderr.strip() or result.stdout.strip())[-512:].decode("utf-8", errors="replace")
        raise AssertionError(f"{step} returned 0x{code:08X}, expected 0x{expected:08X}\n{excerpt}")


def check_delivery(app, cwd, reports, inbox, receipts):
    env = {name: value for name, value in os.environ.items() if not name.startswith("TGE_")}
    env["TGE_CRASH_REPORT_DIR"] = str(reports)
    env["TGE_CRASH_REPORT_URL"] = f"http://127.0.0.1:{inbox.server_port}{ROUTE}"
    identity = run_app(app, cwd, env, "--build-info")
    require_exit(identity, 0, "Reading build identity")
    build = json.loads(identity.stdout)
    crash = run_app(app, cwd, env, "--lab5", PROJECT, "--verify", "divide-by-zero")
    require_exit(crash, 0xC0000094, "Lab 5 native probe")
    assert receipts == [], "The engine posted a report before explicit send"
    assert not list(inbox.inbox.iterdir()), "The inbox changed before explicit send"

    [report] = [path for path in reports.iterdir() if path.is_dir()]
    files = {name: (report / name).read_bytes() for name in ENTRY_NAMES}
    metadata = json.loads(files["report.json"])
    assert metadata["reportId"] == report.name
    assert metadata["buildId"] == build["buildId"]
    assert metadata["commitSha"] == build["commitSha"]
    assert metadata["dumpWritten"] and files["crash.dmp"].startswith(b"MDMP")

    sent = run_app(app, cwd, env, "--send-crash-report", report)
    require_exit(sent, 0, "Sending the report")
    assert receipts == [(201, report.name)], "The inbox did not confirm creation"
    archive_path = inbox.inbox / f"{report.name.lower()}.zip"
    archive_bytes = archive_path.read_bytes()
    with zipfile.ZipFile(archive_path) as archive:
        assert sorted(archive.namelist()) == sorted(files)
        for name, original in files.items():
            assert archive.read(name) == original, f"Received {name} differs from the report"

    retried = run_app(app, cwd, env, "--send-crash-report", report)
    require_exit(retried, 0, "Retrying the report")
    assert receipts == [(201, report.name), (200, report.name)], "An identical archive was not idempotent"
    assert list(inbox.inbox.iterdir()) == [archive_path]
    assert archive_path.read_bytes() == archive_bytes, "The retry replaced archive bytes"

    with socket.socket() as closed_port:
        closed_port.bind(("127.0.0.1", 0))
        offline_url = f"http://127.0.0.1:{closed_port.getsockname()[1]}{ROUTE}"
        unreachable = run_app(app, cwd, dict(env, TGE_CRASH_REPORT_URL=offline_url), "--send-crash-report", report)
        assert unreachable.returncode != 0, "Sending to an unavailable inbox reported success"

    incomplete = reports.parent / "incomplete" / report.name
    incomplete.mkdir(parents=True)
    for name in ENTRY_NAMES - {"crash.dmp"}:
        (incomplete / name).write_bytes(files[name])
    missing_dump = run_app(app, cwd, env, "--send-crash-report", incomplete)
    assert missing_dump.returncode != 0, "Sending a report without a dump reported success"
    assert receipts == [(201, report.name), (200, report.name)], "An incomplete report was uploaded"
    for name, original in files.items():
        assert (report / name).read_bytes() == original, f"Local {name} changed during send"
        if name != "crash.dmp":
            assert (incomplete / name).read_bytes() == original, f"Incomplete {name} changed during send"


def main():
    parser = argparse.ArgumentParser(description="Test delivery of a real Lab 5 crash report on Windows.")
    parser.add_argument("app", type=Path, help="Path to built app.exe with its runtime assets.")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The Lab 5 delivery test requires Windows")
    app = args.app.resolve(strict=True)
    receipts = []

    class RecordingHandler(ReportHandler):
        def reply(self, status, report_id=None):
            receipts.append((status, report_id))
            super().reply(status, report_id)

    with tempfile.TemporaryDirectory(prefix="tge-crash-send-") as temporary:
        sandbox = Path(temporary)
        cwd = sandbox / "runtime"
        react_dir = Path("assets") / "scripts" / "react-luau"
        shutil.copytree(app.parent / react_dir, cwd / react_dir)
        with CrashInbox(sandbox / "inbox", 0, RecordingHandler) as inbox:
            worker = threading.Thread(target=inbox.serve_forever, daemon=True)
            worker.start()
            try:
                check_delivery(app, cwd, sandbox / "reports", inbox, receipts)
            finally:
                inbox.shutdown()
                worker.join()
    print("PASS: explicit crash report send, byte-for-byte ZIP, repeat, offline inbox and missing dump.")


if __name__ == "__main__":
    main()
