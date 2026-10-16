# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise published-host maintenance through the production controller."""

import sys
import time

from mock_index import Index, run, serve
from recovery import PREFIX, session


class HostIndex(Index):
    def __init__(self, scenario):
        super().__init__()
        self.keepalive_error = None
        self.scenario = scenario
        self.nonce = None
        self.published = None
        self.lost = False
        self.restores = 0
        self.restored = False
        self.restore_times = []
        self.errors = []

    def respond(self, path, query):
        if path == "/test/state":
            with self.lock:
                return dict(restores=self.restores, restored=self.restored)
        if path == "/v0/session/add":
            recovery = False
            with self.lock:
                if " W " in query["name"]:
                    self.nonce = query["name"].split()[-1]
                if " H " in query["name"]:
                    if self.published is None:
                        self.published = dict(query)
                    else:
                        recovery = True
                        self.restores += 1
                        self.restore_times.append(time.monotonic())
                        if query != self.published:
                            self.errors.append("restoration changed agreement or host code")
                        if self.scenario == "permanent":
                            return dict(status="DENIED")
                        if self.scenario == "timeout" or (
                                self.scenario == "recover" and self.restores == 1):
                            return dict(status="TOO_MANY_SESSIONS")
            if recovery and self.scenario.endswith("inflight"):
                time.sleep(0.7)  # Give the GUI time to cancel or receive the opponent.
            response = super().respond(path, query)
            if recovery:
                with self.lock:
                    self.restored = True
            return response
        if path == "/v0/session/active":
            with self.lock:
                listing = self.sessions.get(query["secret"])
                if listing and " H " in listing["name"] and not self.lost:
                    self.lost = True
                    del self.sessions[query["secret"]]
                    return dict(status="BAD_SESSION")
        if path == "/v0/list":
            with self.lock:
                self.requests.append((path, query))
                # Published maintenance needs no directory reads, only restoration.
                if self.published:
                    self.errors.append("host queried directory after publication")
                entries = [session(s) for s in self.sessions.values()]
                if self.nonce:
                    entries.append(dict(
                        name=PREFIX + f"C 001 n 00000000 {self.nonce} ffffff",
                        region="NA", method="traversal", game="Project+", server_id="peer",
                        version=query["version"], password=True, in_game=True,
                        player_count=2, port=2626))
                return dict(status="OK", sessions=entries)
        return super().respond(path, query)


def host_recovery(binary, scenarios):
    for scenario in scenarios:
        index = HostIndex(scenario)
        with serve(index) as url:
            run(binary, scenario, url, timeout=43)
        assert index.lost, "fixture never rejected a published listing"
        assert not index.errors, index.errors
        assert not index.sessions, (scenario, "orphan listing", index.sessions)
        expected = 2 if scenario == "recover" else 1
        if scenario == "timeout":
            assert index.restores >= 3, "timeout did not exercise repeated recovery"
        else:
            assert index.restores == expected, (scenario, index.restores)
        assert all(b - a >= 2.8 for a, b in zip(index.restore_times, index.restore_times[1:])), \
            "restoration ignored its retry cadence"
        print(f"PASS published host {scenario}", flush=True)


if __name__ == "__main__":
    host_recovery(sys.argv[1], sys.argv[2:] or (
        "recover", "permanent", "timeout", "cancel-inflight", "connect-inflight"))
