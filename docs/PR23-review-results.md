# PR #23 review fixes

Target: `quickplay-pr-a`, reviewed and remote head `7757cba325a1936e3eeabd6e8f7a4862bd560c0e` (checked 2026-10-16). No applicable AGENTS.md; initial working tree clean. No pushes or GitHub posts are authorized.

## Baseline

Linux Release, Qt 6.7.3, GCC. Existing suite: 1,351/1,352 pass; `PatchAllowlist.VerifyHashes` fails because the approved INI list/hash is stale. This is unrelated to the fixes. Baseline log: `/tmp/pr23-review/baseline-suite.log`.

## 1. Index worker lifecycle

- Cleanup joins independently of the session secret; re-add stops the previous worker before modifying shared state or making another add request. Callback removal defers joining to the owner.
- Source-built `netplay_index_tests`, reusable Python mock index, and CTest registration. Private ephemeral loopback sockets, memory-only configuration, child exit assertions and deadlines. No developer build paths in test sources. Root CTest discovery enabled.
- Before fix: ordinary removal passes, rejected keepalive/removal aborts with signal 6 (`commit1-before.log`).
- After fix: all eight cases pass: normal/repeated removal, rejected and malformed keepalive, destruction, failed/successful re-add, active replacement, callback removal (`commit1-after.log`, 34.45 s).
- Linux application and focused test build pass (`commit1-after-build.log`).

## 2. Listing recovery

- Keep the desired listing and phase across refused restoration, retry every three seconds, retain nonce/agreement, and report permanent errors or three transport failures through the existing result path. Cap errors retain their notice. Search/claim/setup deadlines are not renewed. Cancellation removes a listing even when it arrives during an in-flight restoration.
- A supplied monotonic clock is the only new test seam. The separate Qt Core test executable compiles the production matchmaker; headless lifecycle tests acquire no Qt requirement.
- Before fix: `waiting-cap` fails with “lost listing was discarded after its first refused restoration” (`commit2-before.log`).
- After fix: all 16 scripted cases pass (`commit2-after.log`, 94.75 s): cap/transport recovery, permanent rejection, sustained transport failure, cancellation after refusal and during a successful request, search and claim timeouts, claiming recovery, matched validation recovery/rejection/cancellation/timeout, and host-wait recovery/rejection/timeout. Fixture asserts exact listing contents, request counts and no orphan sessions.
- Linux application and focused executable builds pass (`commit2-after-build.log`, `commit2-tests-build.log`).

## 3. Exact build compatibility

- Lobby Add/List use the complete Git revision with `+ppqp2`; the old `+ppqp1` display-only namespace is deliberately isolated. Ordinary NetPlay and coordinator handshake code is unchanged; normal display version accessors remain available.
- Three source-built peers compile the real version accessors against the production `scmrev.h.in` template: equal descriptions/different 40-character revisions, and equal revision/different platform display metadata. The latter simulates metadata only, not a Windows build. Two-process tests exercise actual listing/filter requests and matching agreements. A headless ENet test uses the actual NetPlay server to check exact-version rejection and platform-independent admission.
- Before fix: different full revisions match (`commit3-before.log`); ordinary version rejection already passes.
- After fix: collision isolation, equal-revision matching with different platform metadata, legacy isolation, and ordinary handshake all pass (`commit3-after.log`, 10.47 s). Generated HTTP keys and both peers' match IDs/roles are asserted.
- Linux application and all four focused executables build (`commit3-after-build.log`). Qt test targets adopt DolphinQt's MSVC RTTI convention. No new production files require handwritten project entries.

## 4. Optional coordinator clearing

- Lobby mode treats empty/whitespace-only input as a saved empty URL. Both base and effective values use the existing settings-layer convention. Custom coordinator mode retains nonempty URL validation; unusable nonempty lobby values retain their existing treatment.
- The real settings dialog runs offscreen with the Fusion style (inherited GTK platform themes can otherwise try to connect to a display). An isolated persistent memory store verifies Save, reopen and configuration-layer reload without touching user files or making HTTP calls.
- Before fix: clearing fails the persisted-value assertion (`commit4-before.log`).
- After fix: all ten cases pass (`commit4-after.log`): empty/whitespace clearing with and without a command-line override, valid custom/fallback saving, empty/whitespace/invalid custom rejection, and existing invalid optional-input behavior. Each successful save checks base, effective, stored/reloaded values and fallback eligibility.
- Linux application and offscreen settings executable build (`commit4-after-build.log`).

## 5. Manual Wii Remote defaults

- Remove Wii auto-assignment from every admission while retaining GameCube auto-assignment. This intentionally restores Project+ Dolphin's pre-2609 default; explicit mappings, broadcasts, disconnect cleanup and the mapping UI remain available.
- Source-built ENet peers exercise the actual server in ordinary and Quick Play rooms. Four room scenarios check GC-only defaults, explicit clear followed by later admission, ordinary disconnect/rejoin and third admission, manual Wii mapping preservation across admission, broadcasts to both peers, clearing, and disconnect cleanup. Quick Play retains its existing one-opponent rule. The actual NetPlay config loader verifies all four Wii sources for every expected map.
- Before fix: player 1 receives Wii `[1,0,0,0]`, failing the empty-map assertion (`commit5-before.log`).
- After fix: both room variants and manual mapping scenarios pass (`commit5-after.log`, 8.11 s). Linux application and protocol executable build (`commit5-after-build.log`).

## Final validation

- Full Linux Release application, existing unit suite and all new source-built executables build successfully (`final-build.log`).
- Full CTest run: all six new regression groups pass; the existing suite remains 1,351/1,352, with only the baseline `PatchAllowlist.VerifyHashes` failure (`final-tests.log`, 151.77 s). CTest's overall exit is 8 because of that baseline failure. No new failures. `git diff --check` passes.
- `ENABLE_QT=OFF`: all three NetPlay regression groups build and pass (`headless-build.log`, `headless-tests.log`, 43.07 s). The mapping executable's runtime dependency list contains no Qt libraries (`headless-dependencies.log`). Qt was re-enabled for the final build.
- Windows CMake configuration was attempted with available `clang-cl`, but its compiler link check fails because there is no compatible Windows linker (`windows-configure.log`); the detected `/usr/bin/link` is the Unix utility. No Wine/runtime is available. Native Windows application/test compilation and execution remain pending.
- Existing handwritten DolphinQt project entries cover the changed production files. No production files were added, so no new project entries are required. Qt CMake test targets carry the application's MSVC RTTI convention.
- A local updated PR description is in `PR23-description-draft.md`; nothing has been posted or pushed.

## Reproduction

Configure with `ENABLE_TESTS=ON`, build `unittests` (runs CTest), or build the individual test targets and run `ctest --test-dir <build> --output-on-failure`. Python 3 is required for the private HTTP fixtures. Loopback sockets must be permitted. Qt tests, when present, use `QT_QPA_PLATFORM=offscreen` and are only built with `ENABLE_QT=ON`.

## Remaining validation

Human checks remain pending: ordinary GUI Host/Join, Quick Play matching and cancel/retry, GC-only default ports, explicit Wii assignment in the mapping UI, gameplay and a Linux–Windows Quick Play connection. The offscreen settings test exercises the actual dialog, but loopback fixtures do not establish gameplay or cross-platform traversal compatibility.

## 6. Bound Quick Play index requests (peer review R1)

- Rechecked 2026-10-16: clean local head `9e29d6de25841201c8fcfb61203edd739d053ca4`, remote PR/fork head `7757cba325a1936e3eeabd6e8f7a4862bd560c0e`, upstream master `3e15d12f405efb817813891ee886b1e3fa762a2d`. No applicable repository guidance. The original three PR commits and five fixes are preserved.
- Quick Play opts into three-second total HTTP deadlines for List, Add, keepalive and Remove. Polls and keepalives also observe cancellation; keepalive shutdown observes an atomic stop flag. Ordinary NetPlay and global HttpRequest defaults retain their previous behavior. Search (180 s), claim (12 s) and setup (25 s) deadlines are unchanged and never renewed; an in-flight request can finish up to its transfer deadline after a state deadline.
- Admission deliberately does not abort on cancellation: a response within three seconds can recover the session secret and immediately remove the admitted listing. Removal also retains its deadline. Cancellation prevents further admission after a completed poll or removal. Conservative worker drain budget is nine seconds (one active transfer, keepalive join, final removal), plus scheduling; read cancellation normally finishes much sooner. This assumes libcurl asynchronous DNS, available in the tested system curl and enabled by default in bundled curl (including Windows). A custom synchronous resolver build can exceed curl timeouts in DNS with NOSIGNAL.
- Protocol limit: if an admitted Add never returns its secret, the client cannot address removal; it sends no keepalive and the listing expires under the existing server policy. Removal remains best effort if the index itself is unavailable. No promise of remote deletion in those cases is made.
- Added source-built `quickplay_deadline_tests` and eight private loopback cases: streaming List cancellation/destruction, streaming List total timeout, immediate retry, cancelled admission with a recoverable delayed secret, admission timeout with no secret, permanent admission rejection, streaming keepalive cancellation, and streaming removal. Fixtures send four whitespace bytes every 250 ms, have bounded child/stream lifetimes, and assert admission/removal counts and orphan-free cleanup whenever a secret is recoverable.
- Before fix: the production matchmaker with the new `list-cancel` fixture hangs through the 11-second child deadline (`/tmp/pr23-remaining/r1-before.log`). After fix: all eight cases pass (`r1-after.log`); cancelled List/keepalive destruction and retry assert under two seconds, streaming List/Add complete in two to five seconds, and removal destruction asserts under five seconds. Linux application and affected source-built executables build (`r1-app-build.log`). This is real production code, with no developer-specific object paths in committed tests.
