# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import time

from mock_index import Index, run, serve

PREFIX = "P+ Quick Play · PPQP1 "


def session(query):
    return dict(name=query["name"], region=query["region"], method=query["method"],
                game=query["game"], server_id=query["server_id"], version=query["version"],
                password=query["password"] == "1", in_game=query["in_game"] == "1",
                player_count=int(query["player_count"]), port=int(query["port"]))


class RecoveryIndex(Index):
    def __init__(self, scenario):
        super().__init__()
        self.scenario = scenario
        self.lost_listing = None
        self.recovery_adds = 0
        self.last_added = None

    def respond(self, path, query):
        if path == "/test/state":
            with self.lock:
                return dict(lost=self.lost_listing is not None, recovery_adds=self.recovery_adds)
        if path == "/v0/session/active" and self.active_count == 0:
            with self.lock:
                self.lost_listing = dict(self.sessions[query["secret"]])
        if path == "/v0/session/add":
            if self.lost_listing:
                self.recovery_adds += 1
                assert query == self.lost_listing, "restoration changed listing identity/agreement"
                if self.scenario == "waiting-inflight":
                    time.sleep(0.5)
                    return super().respond(path, query)
                error = None
                if self.scenario.endswith("permanent"):
                    error = "DENIED"
                elif self.scenario.endswith("unreachable") or self.scenario.endswith("transport"):
                    if self.recovery_adds == 1 or self.scenario.endswith("unreachable"):
                        # Record the failed request as well.
                        with self.lock:
                            self.requests.append((path, query))
                            self.add_count += 1
                        return None
                elif self.recovery_adds == 1 or self.scenario.endswith("timeout"):
                    error = "TOO_MANY_SESSIONS"
                if error:
                    self.add_errors[self.add_count + 1] = error
            self.last_added = query
        if path == "/v0/list":
            with self.lock:
                self.requests.append((path, query))
                entries = [session(s) for s in self.sessions.values()]
                peer = None
                if self.scenario.startswith(("claim", "host-wait")):
                    peer = "W 001 h 00000000 000000"
                    if self.scenario.startswith("host-wait") and self.last_added:
                        nonce = self.last_added["name"].split()[-1]
                        peer = f"A w 00000000 000000 {nonce}"
                elif self.scenario.startswith("validate") and self.last_added:
                    nonce = self.last_added["server_id"]
                    peer = f"C 001 n 00000000 {nonce} ffffff"
                if peer:
                    entries.append(dict(name=PREFIX + peer, region="NA", method="traversal",
                                        game="Project+", server_id="peer", version=query["version"],
                                        password=True, in_game=True, player_count=2, port=2626))
                return dict(status="OK", sessions=entries)
        return super().respond(path, query)


def recovery(binary):
    for scenario in ("waiting-cap", "waiting-transport", "waiting-permanent", "waiting-cancel",
                     "waiting-timeout", "waiting-unreachable", "waiting-inflight", "claim-cap",
                     "claim-timeout", "validate-cap", "validate-timeout", "validate-cancel",
                     "validate-permanent", "host-wait-cap", "host-wait-permanent", "host-wait-timeout"):
        index = RecoveryIndex(scenario)
        with serve(index) as url:
            run(binary, scenario, url, timeout=23)
        assert not index.sessions, (scenario, "orphan listing", index.sessions)
        expected = 1 if scenario.endswith(("permanent", "cancel", "timeout", "inflight")) else 3 if scenario.endswith("unreachable") else 2
        assert index.recovery_adds == expected, (scenario, index.recovery_adds)
        print(f"PASS recovery {scenario}", flush=True)


if __name__ == "__main__":
    recovery(sys.argv[1])
