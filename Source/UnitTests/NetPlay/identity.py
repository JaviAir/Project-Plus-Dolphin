# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import subprocess
import sys
import time

from mock_index import Index, serve
from recovery import PREFIX, session

REV_A = "a" * 40
REV_B = "b" * 40


class MatchingIndex(Index):
    def __init__(self, legacy=False):
        super().__init__()
        self.keepalive_error = None
        self.matched = {}
        self.keys = []
        if legacy:
            self.sessions["legacy"] = dict(name=PREFIX + "W 001 h 00000000 000000", region="NA",
                                           method="traversal", game="Project+", server_id="old",
                                           version="collision-7+ppqp1", password="1", in_game="0",
                                           player_count="1", port="2626")

    def respond(self, path, query):
        if path.startswith("/test/"):
            with self.lock:
                if path == "/test/matched":
                    self.matched[query["peer"]] = (query["id"], query["role"])
                if path == "/test/both-matched":
                    return b"yes" if len(self.matched) == 2 else b"no"
                if path == "/test/key":
                    self.keys.append(query["expected"])
                return dict(status="OK")
        if path == "/v0/list":
            with self.lock:
                self.requests.append((path, query))
                return dict(status="OK", sessions=[session(s) for s in self.sessions.values()
                            if s["version"] == query["version"]])
        return super().respond(path, query)


def pair(first, second, match, legacy=False):
    index = MatchingIndex(legacy)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    processes = []
    outputs = []
    with serve(index) as url:
        try:
            for i, (binary, revision) in enumerate((first, second)):
                if binary is None:
                    continue
                process = subprocess.Popen([binary, url, "match" if match else "isolate", str(i),
                                            revision + "+ppqp2"], env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True)
                processes.append(process)
                # Give the first peer a head start; simultaneous search is also handled by
                # the production random/yield protocol. All process waits remain bounded.
                time.sleep(0.3)
            for process in processes:
                stdout, stderr = process.communicate(timeout=15)
                outputs.append(stdout)
                assert process.returncode == 0, (process.returncode, stdout, stderr)
                assert "PASS identity" in stdout
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=3)
    if match:
        values = list(index.matched.values())
        assert len(values) == 2 and values[0][0] == values[1][0], index.matched
        assert {v[1] for v in values} == {"host", "client"}
    else:
        assert not index.matched
    # Assert the actual generated Add and List values, not just a helper's return value.
    expected_keys = {rev + "+ppqp2" for binary, rev in (first, second) if binary}
    for path in ("/v0/session/add", "/v0/list"):
        actual = {query["version"] for p, query in index.requests if p == path}
        assert actual == expected_keys, (path, actual, expected_keys)
    assert set(index.keys) == expected_keys
    assert set(index.sessions) <= {"legacy"}, index.sessions
    return outputs


if __name__ == "__main__":
    a, b, other_platform = sys.argv[1:]
    outputs = pair((a, REV_A), (b, REV_B), False)
    identities = [output.splitlines()[0].split() for output in outputs]
    assert identities[0][1] != identities[1][1] and identities[0][2] == identities[1][2], identities
    print("PASS equal display / differing full revisions isolated", flush=True)
    outputs = pair((a, REV_A), (other_platform, REV_A), True)
    identities = [output.splitlines()[0].split() for output in outputs]
    assert identities[0][1:3] == identities[1][1:3], identities
    assert identities[0][-1] != identities[1][-1], identities
    print("PASS equal revision / differing platform metadata matched", flush=True)
    pair((a, REV_A), (None, REV_A), False, legacy=True)
    print("PASS legacy display-only namespace isolated", flush=True)
