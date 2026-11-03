#!/usr/bin/env python3
"""Exercise real HTTP multipart uploads, failure handling, cancellation and TLS."""

import email.parser
import email.policy
import http.server
import json
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time

binary = pathlib.Path(sys.argv[1]).resolve()
records = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        if self.path.startswith("/slow"):
            time.sleep(2)
            return
        body = self.rfile.read(int(self.headers["Content-Length"]))
        message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
            (
                "Content-Type: "
                + self.headers["Content-Type"]
                + "\r\nMIME-Version: 1.0\r\n\r\n"
            ).encode()
            + body
        )
        fields = {
            p.get_param("name", header="content-disposition"): p
            for p in message.iter_parts()
        }
        records.append((self.path, dict(self.headers), fields))
        if self.path.startswith("/reject"):
            self.send_response(403)
            self.end_headers()
            self.wfile.write(b'{"error":"test-key-do-not-log"}')
            return
        if self.path.startswith("/redirect"):
            self.send_response(302)
            self.send_header("Location", "/should-not-follow")
            self.end_headers()
            return
        response = (
            b"<html>login</html>"
            if self.path.startswith("/html")
            else json.dumps(
                {"files": {"local": {"name": "uploaded.gcode"}}}
                if "/api/files/local" in self.path
                else {
                    "item": {"path": "uploaded.gcode", "root": "gcodes"},
                    "print_started": False,
                }
            ).encode()
        )
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass


with tempfile.TemporaryDirectory(prefix="orca-upload-tests-") as directory:
    tmp = pathlib.Path(directory)
    file = tmp / "source.gcode"
    content = b";test slice\nG21\nG1 X10 Y20 E1\n"
    file.write_bytes(content)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"

    def run(protocol, url, mode="normal", ca=None, source=file):
        args = [str(binary), protocol, url, str(source), mode] + (
            [str(ca)] if ca else []
        )
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=20, check=True
        )
        assert "test-key-do-not-log" not in result.stdout + result.stderr
        return json.loads(result.stdout)

    try:
        for protocol, path in [
            ("octoprint", "/proxy/api/files/local"),
            ("moonraker", "/proxy/server/files/upload"),
        ]:
            result = run(protocol, base + "/proxy/")
            assert result["success"] and result["percent"] == 100, result
            location, headers, fields = records[-1]
            assert location == path
            assert headers["X-Api-Key"] == "test-key-do-not-log"
            assert fields["file"].get_payload(decode=True) == content
            assert fields["file"].get_filename() == "Český cube & space.gcode"
            assert fields["print"].get_content() == "false"
            assert fields["path"].get_content() == "folder with spaces"
            assert fields[
                "select" if protocol == "octoprint" else "root"
            ].get_content() == ("false" if protocol == "octoprint" else "gcodes")
        for path in ["/reject", "/redirect", "/html"]:
            before = len(records)
            result = run("octoprint", base + path)
            assert not result["success"], result
            assert len(records) == before + 1
        for url in [
            "file:///tmp/printer",
            base + "?key=secret",
            base + "/#fragment",
            "http://user:secret@localhost",
        ]:
            before = len(records)
            assert not run("octoprint", url)["success"]
            assert len(records) == before
        large = tmp / "large.gcode"
        large.write_bytes(content * 200000)
        result = run("moonraker", base + "/slow", mode="cancel", source=large)
        assert result["cancelled"] and not result["success"], result
        cert = tmp / "cert.pem"
        key = tmp / "key.pem"
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-addext",
                "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout",
                str(key),
                "-out",
                str(cert),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        secure = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        tls_thread = threading.Thread(target=secure.serve_forever, daemon=True)
        tls_thread.start()
        try:
            url = f"https://127.0.0.1:{secure.server_port}"
            assert not run("moonraker", url)["success"]
            result = run("moonraker", url, ca=cert)
            assert result["success"], result
        finally:
            secure.shutdown()
            secure.server_close()
            tls_thread.join()
        if len(sys.argv) > 2:
            before = len(records)
            subprocess.run(
                sys.argv[2:],
                env={**os.environ, "ORCA_TEST_UPLOAD_URL": base},
                check=True,
                timeout=240,
            )
            uploads = records[before:]
            assert len(uploads) == 2
            assert (
                uploads[0][0] == "/api/files/local"
                and uploads[1][0] == "/server/files/upload"
            )
            first = uploads[0][2]["file"].get_payload(decode=True)
            assert b"OrcaSlicer" in first and len(first) > 1000
            assert first == uploads[1][2]["file"].get_payload(decode=True)
            assert all(row[2]["print"].get_content() == "false" for row in uploads)
        print(
            "PASS OctoPrint and Moonraker multipart bytes/auth/paths, upload-only, HTTP failures, redirect refusal, URL validation, cancellation and TLS verification"
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
