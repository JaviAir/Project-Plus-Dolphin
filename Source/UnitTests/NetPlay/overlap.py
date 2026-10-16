# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Reject keepalive with a directory request already in flight."""

import sys
import threading
import time

from mock_index import Index, run, serve


class OverlapIndex(Index):
    def __init__(self, scenario):
        super().__init__()
        self.scenario = scenario
        self.active_waiting = threading.Event()
        self.list_waiting = threading.Event()
        self.active_rejected = threading.Event()
        self.original = None
        self.list_count = 0
        self.recovery_adds = 0

    def respond(self, path, query):
        if path == "/test/state":
            return dict(active_waiting=self.active_waiting.is_set())
        if path == "/v0/session/active" and not self.active_rejected.is_set():
            self.active_waiting.set()
            assert self.list_waiting.wait(2), "no overlapping directory request"
            response = super().respond(path, query)
            self.active_rejected.set()
            return response
        if path == "/v0/list":
            self.list_count += 1
            if self.list_count == 2:
                self.list_waiting.set()
                assert self.active_rejected.wait(2), "keepalive did not reject during List"
                # Give the client time to consume the rejected keepalive before the list
                # failure. The assertions check diagnostics as well as request overlap.
                time.sleep(0.2)
            if self.list_count == 2 or (self.scenario == "list-outage" and self.list_count > 2):
                return dict(status="LIST_DENIED")
        if path == "/v0/session/add":
            if self.original is None:
                self.original = dict(query)
            else:
                assert query == self.original, "recovery changed listing identity"
                self.recovery_adds += 1
                if self.scenario == "transport":
                    return None
                if self.scenario == "permanent":
                    return dict(status="DENIED")
                if self.recovery_adds == 1:
                    return dict(status="TOO_MANY_SESSIONS")
        return super().respond(path, query)


def overlap(binary, scenarios):
    for scenario in scenarios:
        index = OverlapIndex(scenario)
        with serve(index) as url:
            run(binary, scenario, url, timeout=15)
        assert index.active_rejected.is_set() and index.list_waiting.is_set()
        assert index.recovery_adds == (1 if scenario == "permanent" else 3 if scenario == "transport" else 2)
        assert not index.sessions, (scenario, "orphan listing", index.sessions)
        print(f"PASS overlap {scenario}", flush=True)


if __name__ == "__main__":
    overlap(sys.argv[1], sys.argv[2:] or ("recover", "permanent", "transport", "list-outage"))
