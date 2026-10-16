# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Production Quick Play against responses that defeat curl's low-speed timeout."""

import json
import sys
import threading
import time

from mock_index import Index, run, serve


class DeadlineIndex(Index):
    def __init__(self, scenario):
        super().__init__()
        self.scenario = scenario
        self.keepalive_error = None
        self.fault_started = threading.Event()
        self.stop = threading.Event()
        self.faults = 0

    def stream(self, handler):
        handler.send_response(200)
        handler.send_header("Content-Length", "1000000")
        handler.end_headers()
        self.fault_started.set()
        # Four bytes every 250 ms exceeds the one-byte/second low-speed limit.
        # Explicit fixture ceiling and teardown event bound even a broken child.
        deadline = time.monotonic() + 12
        try:
            while time.monotonic() < deadline and not self.stop.is_set():
                handler.wfile.write(b"    ")
                handler.wfile.flush()
                self.stop.wait(0.25)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def delayed_add(self, handler, response):
        self.fault_started.set()
        time.sleep(0.8)  # Cancel while admitted, before the recoverable secret arrives.
        body = json.dumps(response).encode()
        handler.send_response(200)
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def respond(self, path, query):
        if path == "/test/state":
            return dict(fault_started=self.fault_started.is_set())
        response = super().respond(path, query)
        target = ("/v0/list" if self.scenario.startswith("list") or self.scenario == "retry"
                  else "/v0/session/active" if self.scenario == "keepalive-stream"
                  else "/v0/session/add")
        if self.scenario == "remove-stream":
            if path == "/v0/session/add":
                self.fault_started.set()
            if path == "/v0/session/remove":
                return self.stream
        elif path == target and self.faults == 0:
            self.faults += 1
            if self.scenario == "add-delayed":
                return lambda handler: self.delayed_add(handler, response)
            if self.scenario == "add-rejected":
                self.sessions.clear()
                return dict(status="DENIED")
            return self.stream
        return response


def deadlines(binary, scenarios):
    for scenario in scenarios:
        index = DeadlineIndex(scenario)
        try:
            with serve(index) as url:
                run(binary, scenario, url, timeout=11)
        finally:
            index.stop.set()
        if scenario == "add-timeout":
            # The protocol cannot remove an admission whose secret never arrives.
            assert len(index.sessions) == 1
            assert index.active_count == 0
        else:
            assert not index.sessions, (scenario, "orphan listing", index.sessions)
        if scenario == "retry":
            assert index.add_count == 1
        if scenario in ("add-delayed", "remove-stream", "keepalive-stream"):
            assert any(path == "/v0/session/remove" for path, _ in index.requests)
        print(f"PASS deadlines {scenario}", flush=True)


if __name__ == "__main__":
    deadlines(sys.argv[1], sys.argv[2:] or (
        "list-cancel", "list-timeout", "retry", "add-delayed", "add-timeout",
        "add-rejected", "keepalive-stream", "remove-stream"))
