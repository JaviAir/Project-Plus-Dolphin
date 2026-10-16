# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Bounded, portable process tests against a private ephemeral loopback index."""

import contextlib
import http.server
import json
import os
import subprocess
import sys
import threading
import urllib.parse


class Index:
    def __init__(self):
        self.requests = []
        self.sessions = {}
        self.lock = threading.Lock()
        self.add_count = 0
        self.active_count = 0
        self.keepalive_error = "BAD_SESSION"
        self.add_errors = {}

    def respond(self, path, query):
        with self.lock:
            self.requests.append((path, query))
            if path == "/v0/session/add":
                self.add_count += 1
                if self.add_count in self.add_errors:
                    return {"status": self.add_errors[self.add_count]}
                secret = str(self.add_count)
                self.sessions[secret] = dict(query)
                return {"status": "OK", "secret": secret}
            if path == "/v0/session/active":
                self.active_count += 1
                if self.active_count == 1 and self.keepalive_error:
                    self.sessions.pop(query["secret"], None)
                    if self.keepalive_error == "malformed":
                        return b"{invalid"
                    return {"status": self.keepalive_error}
            if path == "/v0/session/remove":
                assert query["secret"] in self.sessions, "removed unknown session"
                del self.sessions[query["secret"]]
            if path == "/v0/list":
                return {"status": "OK", "sessions": []}
            return {"status": "OK"}


@contextlib.contextmanager
def serve(index):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            query = dict(urllib.parse.parse_qsl(url.query))
            response = index.respond(url.path, query)
            if response is None:  # Deterministic transport failure, no public network.
                self.close_connection = True
                return
            body = response if isinstance(response, bytes) else json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        assert not thread.is_alive()


def run(binary, scenario, url, timeout=18):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    result = subprocess.run([binary, scenario, url], env=env, capture_output=True,
                            text=True, timeout=timeout)
    assert result.returncode == 0, (scenario, result.returncode, result.stdout, result.stderr)
    assert f"PASS {scenario}" in result.stdout, result.stdout


def lifecycle(binary):
    # Each termination case has its own child, so an abort cannot masquerade as success.
    for scenario in ("normal", "remove", "destroy", "failed-readd", "readd",
                     "active-readd", "callback-remove", "malformed"):
        index = Index()
        if scenario in ("normal", "active-readd"):
            index.keepalive_error = None
        if scenario == "malformed":
            index.keepalive_error = "malformed"
        if scenario == "failed-readd":
            index.add_errors[2] = "TOO_MANY_SESSIONS"
        with serve(index) as url:
            run(binary, scenario, url)
        assert index.add_count == (2 if "readd" in scenario else 1)
        assert not index.sessions, (scenario, index.sessions)
        print(f"PASS lifecycle {scenario}", flush=True)


if __name__ == "__main__":
    lifecycle(sys.argv[1])
