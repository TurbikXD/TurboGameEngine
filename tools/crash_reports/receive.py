import argparse
import filecmp
import json
import os
import re
import struct
import tempfile
import time
import uuid
import zipfile
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


ROUTE = "/crash-reports"
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
ENTRY_NAMES = {"report.json", "crash.dmp", "engine.log"}
ZIP_END = struct.Struct("<4s4H2IH")
BODY_TIMEOUT = 20


class InvalidReport(ValueError):
    pass


def read_report_id(file, length):
    if length < ZIP_END.size:
        raise InvalidReport
    file.seek(-ZIP_END.size, os.SEEK_END)
    marker, disk, directory_disk, disk_entries, entries, size, offset, comment = ZIP_END.unpack(
        file.read(ZIP_END.size)
    )
    if (marker != b"PK\x05\x06" or disk or directory_disk or disk_entries != len(ENTRY_NAMES)
            or entries != len(ENTRY_NAMES) or comment or size > 4096
            or offset + size + ZIP_END.size != length):
        raise InvalidReport

    file.seek(0)
    if file.read(4) != b"PK\x03\x04":
        raise InvalidReport
    with zipfile.ZipFile(file) as archive:
        members = archive.infolist()
        if len(members) != len(ENTRY_NAMES) or {entry.orig_filename for entry in members} != ENTRY_NAMES:
            raise InvalidReport
        if any(entry.compress_type != zipfile.ZIP_STORED or entry.flag_bits & ~0x800
               or not 0 < entry.file_size == entry.compress_size <= length for entry in members):
            raise InvalidReport
        if archive.getinfo("report.json").file_size > 64 * 1024:
            raise InvalidReport
        with archive.open("crash.dmp") as dump:
            header = dump.read(32)
        if len(header) != 32:
            raise InvalidReport
        signature, version, streams, directory = struct.unpack_from("<4sIII", header)
        if (signature != b"MDMP" or version & 0xFFFF != 0xA793 or not streams or directory < 32
                or directory + 12 * streams > archive.getinfo("crash.dmp").file_size):
            raise InvalidReport
        metadata = json.loads(archive.read("report.json").decode("utf-8"))
        if (not isinstance(metadata, dict) or type(metadata.get("schemaVersion")) is not int
                or metadata["schemaVersion"] != 1):
            raise InvalidReport
        report_id = metadata.get("reportId")
        build_id = metadata.get("buildId")
        if (not isinstance(report_id, str) or str(uuid.UUID(report_id)) != report_id.lower()
                or not isinstance(build_id, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", build_id)
                or archive.testzip() is not None):
            raise InvalidReport
        return report_id


class ReportHandler(BaseHTTPRequestHandler):
    timeout = 10

    def log_message(self, *_args):
        pass

    def reply(self, status, report_id=None):
        try:
            self.send_response(status)
            self.send_header("Content-Length", "0")
            if report_id is not None:
                self.send_header("X-TGE-Report-Id", report_id)
            self.end_headers()
        except OSError:
            pass

    def do_POST(self):
        if self.path != ROUTE:
            return self.reply(HTTPStatus.NOT_FOUND)
        if self.headers.get_all("Transfer-Encoding"):
            return self.reply(HTTPStatus.BAD_REQUEST)
        content_types = self.headers.get_all("Content-Type", [])
        if len(content_types) != 1 or self.headers.get_content_type() != "application/zip":
            return self.reply(HTTPStatus.UNSUPPORTED_MEDIA_TYPE)
        lengths = self.headers.get_all("Content-Length", [])
        if not lengths:
            return self.reply(HTTPStatus.LENGTH_REQUIRED)
        if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,20}", lengths[0]):
            return self.reply(HTTPStatus.BAD_REQUEST)
        length = int(lengths[0])
        if length > MAX_ARCHIVE_BYTES:
            return self.reply(HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        try:
            status, report_id = self.receive_report(length)
        except TimeoutError:
            return self.reply(HTTPStatus.REQUEST_TIMEOUT)
        except (ValueError, zipfile.BadZipFile, EOFError, RecursionError):
            return self.reply(HTTPStatus.BAD_REQUEST)
        except OSError:
            return self.reply(HTTPStatus.INTERNAL_SERVER_ERROR)
        return self.reply(status, report_id)

    def receive_report(self, length):
        upload = None
        try:
            with tempfile.NamedTemporaryFile(dir=self.server.inbox, prefix=".upload-", delete=False) as file:
                upload = Path(file.name)
                remaining = length
                deadline = time.monotonic() + BODY_TIMEOUT
                while remaining:
                    seconds_left = deadline - time.monotonic()
                    if seconds_left <= 0:
                        raise TimeoutError
                    self.connection.settimeout(min(self.timeout, seconds_left))
                    chunk = self.rfile.read1(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise InvalidReport
                    file.write(chunk)
                    remaining -= len(chunk)
                report_id = read_report_id(file, length)
                os.fsync(file.fileno())
            destination = self.server.inbox / f"{report_id.lower()}.zip"
            try:
                os.link(upload, destination)
            except FileExistsError:
                if not destination.is_symlink() and destination.is_file() and filecmp.cmp(
                        upload, destination, shallow=False):
                    return HTTPStatus.OK, report_id
                return HTTPStatus.CONFLICT, None
            return HTTPStatus.CREATED, report_id
        finally:
            if upload is not None:
                upload.unlink(missing_ok=True)


class CrashInbox(HTTPServer):
    def __init__(self, directory, port, handler=ReportHandler):
        self.inbox = directory.resolve()
        self.inbox.mkdir(mode=0o700, parents=True, exist_ok=True)
        super().__init__(("127.0.0.1", port), handler)


def main():
    parser = argparse.ArgumentParser(description="Receive TurboGameEngine crash reports on loopback.")
    parser.add_argument("--directory", type=Path, required=True, help="Private directory for received ZIP files.")
    parser.add_argument("--port", type=int, default=8765, help="TCP port. Set 0 for an available port.")
    arguments = parser.parse_args()
    if not 0 <= arguments.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    with CrashInbox(arguments.directory, arguments.port) as inbox:
        print(f"Listening on http://127.0.0.1:{inbox.server_port}{ROUTE}", flush=True)
        try:
            inbox.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
