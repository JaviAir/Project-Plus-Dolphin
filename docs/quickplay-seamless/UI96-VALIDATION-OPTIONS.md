# UI96 validation options and no-bulk-scan UI trial

Archived planning decision, 2026-10-11: the user selected **Option A, no bulk scan ever**,
for the next implementation and user-operated UI experiment. **Option B,
background verification plus a cache across launches**, is documented as an
alternative only. The planning turn created the plan; it did not implement either option,
build a candidate, alter profiles or run an emulator.

The aim is to preserve the seamless same-Core lifecycle while removing the
58.906-second synchronous content-verification obstruction to UI testing. This
decision supersedes the earlier requirement to complete responsive verification
and external-save ownership work before this particular isolated UI trial. It
does not resolve those defects or establish general deterministic compatibility.

## What changed files and ordinary launches mean

The ISO and read-only SD image normally do not change just because Dolphin opens,
searches or plays another match. Replacing the ISO, updating Project+, rebuilding
the SD image, or editing its content changes those files. Config files and
launcher/checkpoint files can change when settings, software or the bundle are
updated. NAND has writable state: normal game activity can update title saves,
and emulator/system activity can update other files. The current manual profile's
autosv0.bin and autosv1.bin each differ by seven bytes from certified copies;
the exact write event and field meanings were not instrumented. See the
existing findings (workspace evidence: `planning/quickplay/phase9/ui96-manual/nand-rejection-20261011/HANDOFF.md`).

An "ordinary launch/search" in Option B means the same content and compatible
running configuration, with a usable prior verification record. It is not a
separate kind of search the player selects. Checking file metadata means reading
information such as file size, modification/change timestamps and file identity,
not reading all the bytes of that file. Such checks are usually inexpensive, but
tree enumeration and slow storage can still take time. Metadata change need not
mean content changed; a conservative cache would reverify anyway.

If an ISO or SD image needs reverification, that requires reading the entire
large file again. Option B would not limit changed-file reads to small files.
The workload observed here could still take roughly a minute; running it in the
background makes the application responsive, not the hashing intrinsically fast.
Writable saves must not be treated as immutable content that is expected to
retain its original hash forever.

Reaching CSS immediately gives background work little head start. With Option B,
a valid cache can make verification ready immediately; an empty or invalid cache
may still be working when the player presses the chord. Show a truthful waiting
indicator then, keep the GUI responsive, and support cancellation only where it
is actually implemented. Background work must not pause gameplay for its duration.
Option A has no background verifier or verification wait.

## The two options

| Option | Content policy | Search experience | Current decision |
|---|---|---|---|
| A. No bulk scan ever | Trust the bundle's declared content identity; do not recompute file/tree content hashes | Run lightweight admission checks, then search; no verification loading phase | Implement and test this trial next |
| B. Background verification plus persistent cache | Verify actual immutable content once; retain results across launches; reverify changed/missing-cache content on a worker | Immediate when verification is current; responsive waiting indicator when it is still underway | Document only; do not implement or test now |

Option A gives up independent detection of altered/corrupt external content. Two
peers agreeing on a declared manifest does not prove their actual files or saves
are identical. Pairing, gameplay or recovery problems must remain observable.
This is an explicit trial policy, not a claim that skipped checks passed.

"No bulk scan ever" applies to admission/verification in the trial at startup,
initial search, matched revalidation, restoration and repeat search. It does not
mean zero filesystem access: small manifest reads and required existence/header
checks remain, the ordinary checkpoint loader reads its savestate, and the game
continues reading assets. No substitute partial scan, tree hash, background scan
or deferred full scan is part of Option A.

## Option A implementation plan

1. **Preserve the baseline and isolate the experiment.** Retain all inherited
   source changes and current Step2b artifacts. Use a separate build directory,
   executable and dedicated trial launcher. Keep the corrected XCB/OpenGL data
   lookup. Prepare trial profiles as copies of the user's current isolated
   profiles, including their current saves; do not reset autosaves to certified
   copies. Prepare a compatible second peer for pairing when needed. Profile
   preparation may copy files, but must not introduce a content certification
   scan as a condition of launch/search. Preserve the original profiles and
   receipts. No protected research process/session or service control.

2. **Make the policy explicit and contained.** Introduce a named host-side trial
   policy selected by the dedicated candidate/launcher, with the existing strict
   path remaining the default outside the trial. Choose the smallest ordinary
   production setting or build option during implementation; do not revive
   private research environment flags or input overrides. Capture the policy
   with the attempt/session so it cannot change halfway through a handoff.
   Record it in diagnostics and the handoff; normal game guidance does not need
   implementation terminology.

3. **Replace actual-content comparison with declared-bundle admission in the
   trial.** Read/validate the small common-css.json structure, required declared
   identities, schema, supported build, pinned declared checkpoint identity and
   effective running configuration. Retain required-file existence/readability
   and bounded identification checks. Retain the actual inserted-disc/configured
   game check where it uses the existing bounded sync identifier; audit its I/O
   so it cannot hide a full-file scan. Retain CSS eligibility, single-core,
   DSP/SD policy, SI/EXI devices, movie and NetPlay restrictions.

   Skip HashFile/HashTree content comparison for ISO, SD, launchers, settings,
   checkpoint and NAND in trial admission. In particular, natural autosave drift
   must not fail a comparison that this policy deliberately does not perform.
   Do not change the certified JSON receipts or redefine strict NAND exclusions.
   Hashing a small in-memory identity representation is permitted; rereading
   file contents to establish their hashes is not.

4. **Use the policy consistently through the lifecycle.** Apply it at initial
   admission and matched revalidation, not just at chord dispatch. Audit Restore
   too: its separate checkpoint HashFile must not trigger an extra verification
   pass in this trial. Keep ordinary State::LoadForPersistentSession handling,
   actual loader failures, PC/ticks/device checks, CPU/GPU ownership and existing
   recovery behavior. The checkpoint is loaded as required by the seamless
   lifecycle, with no additional checksum preread. Do not modify guest memory,
   pad transport, graphics settings or comparators to make the trial succeed.

5. **Preserve peer compatibility and honest status.** Derive the ready-contract
   identity from the declared bundle/configuration and an explicit policy/version
   tag. Both trial peers must agree; a strict peer must not interpret a trial's
   declaration as verified content. Use the existing 64-character identity and
   readiness barrier if sufficient; do not add a second input transport.
   Log "declared bundle accepted; content verification skipped" rather than
   "content verified". Audit earlier success logs for the same distinction.

   Bypass the long paused Checking-game-files/prepaint stage for this policy.
   Run lightweight admission on its proper owner, preserve its generation and
   ownership checks, and publish Searching promptly after admission succeeds.
   Keep Connecting/setup presentation, necessary short pause boundaries,
   readiness/attach/release, B availability and retained first errors truthful.
   Preserve strict-mode validation/prepaint behavior independently.

6. **Check all entry points and the launcher.** Cover InGame and explicit Desktop
   entry when the trial policy is selected, pairing, retirement and fresh search
   after recovery. The dedicated launcher must identify the correct trial
   executable/profile/policy without reintroducing game-content tree hashing.
   The old launcher pins a specific Step2b executable hash and hashes system
   settings; provide a separate trial launcher rather than overwrite that
   historical launch contract. A bounded binary provenance check is allowed,
   but no ISO/SD/NAND certification sweep at launch or --check.

7. **Build and verify the candidate.** Run affected build targets and meaningful
   focused tests. Verify policy selection/defaults; valid and malformed bundle
   admission; rejection of unsupported config/devices or missing required files;
   trial never calling content file/tree hash routines at initial or matched
   admission/restoration; policy-tag identity agreement/mismatch; stale
   completion, cancellation and Stop/Close behavior. Where tests need file
   fixtures, use disposable fixtures, not live user profiles. Audit the actual
   call graph, launcher and restore path in addition to policy-unit tests.

   Preserve existing ownership/presentation checks and report the affected suite
   results. Separate builds and unit tests are allowed in the future implementation
   scope; emulator launch/control and physical inputs remain user-operated.

## User-operated test order and acceptance

First test the UI already present in Step2b. Opponent names and the leave chord
have not been implemented; they remain UI96 Steps3/4. A successful trial enables
their later implementation/testing rather than inventing an "all UI passed"
result now.

1. **Fresh process, first search:** close/relaunch the trial Dolphin, reach local
   CSS, release then press L+R+Up. Record admission duration and visible search
   response. Target local admission/Searching presentation within one second on
   this machine, excluding actual network assignment; measure rather than assume.
   There must be no minute-long verification pause or background bulk scan.
2. **Search cancellation:** release then press B, confirm cancellation and local
   CSS hint. Exercise held B/chord, release/rearm, repeated search and focus
   changes; prevent Back leakage and duplicate coordinator requests.
3. **Pairing and setup:** use a second compatible trial peer. Confirm hidden
   automatic dialogs, no focus theft, truthful Connecting and B guidance,
   paused asynchronous cancellation, automatic readiness/activation and shared
   CSS. Confirm no bulk scan at matched handoff. Exercise both player roles.
4. **Errors and diagnostics:** review visible connection/restore failures when
   encountered and access Tools > Quick Play Diagnostics. Use isolated fixtures
   for intentional incompatibility checks; do not damage user content. Record
   failures separately from UI successes.
5. **Recovery and repeated operation:** use the existing diagnostics Quit/room
   exit until the leave chord exists. Confirm return to usable independent CSS,
   repeat search, a complete match/results where feasible, and Stop/Close.
   Repeat the first-search check after another full Dolphin restart; no warm-cache
   assumption may explain success. Natural save writes remain intact.
6. **Complete the remaining UI work in its existing order:** after the Step2 gate,
   implement/test leave, opponent names and final layout under UI96 Steps3/4,
   then the full local flow in Step5. Check fullscreen/window sizes, chat/OSD,
   CSS/stage/gameplay/results placement and both roles.

Deliver measured first-search and matched-admission times, user-observed physical
behavior, candidate identity/policy, focused check results and observed failures.
Do not describe bypassed NAND validation as a save-ownership fix, or successful UI
testing as proof of content equality/determinism. If attachment, gameplay or
recovery fails with natural saves, investigate that demonstrated failure without
silently restoring certified saves or claiming the policy resolved it.

## Option B retained alternative — no work on it now

If the trial's tradeoff is later rejected, implement background immutable-content
verification plus an on-disk cache together. Scope would include:

- Persistent records tied to actual file identity/path, size, modification/change
  metadata, verifier version and bundle/build/configuration as relevant. Missing,
  stale or corrupt records cause reverification, not assumed success. Metadata
  checks reduce repeated reads; they are not cryptographic proof against every
  possible undetected modification.
- Worker-owned file hashing, stable-file checks before/after reads, copied owner
  inputs/results and fencing for Core/profile/content/attempt changes. No guest
  memory reads on a worker. Keep local gameplay running while verifying files.
- Proactive startup verification, prompt use of valid cached results, truthful
  waiting indication when CSS/search arrives first, and real cancellation with
  defined behavior for work still running. Large changed files still require
  large reads. No simulated progress or GUI blocking.
- An explicit storage policy for writable title saves and system state; immutable
  cache rules alone do not solve the NAND/autosave defect.
- Tests for cold/warm launch, metadata changes/replacement, concurrent writes,
  cache corruption, cancellation, Stop/Close, rejoin and both peer roles.

Earlier rough estimate for Option B: 1–2 engineering days for background
verification, another half to one day for persistent caching, roughly 2–3 days
combined including focused checks. This excludes external-save ownership work
and full user-operated runtime acceptance. No implementation estimate is a
completion guarantee. Option B is not a prerequisite for the selected trial.

## Source references and limits

- Current trigger/admission/prepaint: MainWindow.cpp StartQuickPlayFrom and
  ValidateQuickPlayContent; SI_DeviceGCController.cpp GetPadStatus.
- Current full-file/tree hashing and extra restore hash:
  DolphinQt/QuickPlay/PersistentSession.cpp Validate, HashFile, HashTree, Restore.
- Existing peer barrier: NetPlayClient.cpp PreparePersistentSession /
  ValidatePersistentAttach; NetPlayServer.cpp PersistentReady handling.
- Existing user launch: ui96-manual/launch-xbox.py and its README.
- Existing UI gates: UI96-PLAN.md and UI96-STEP2B.md.

No receipt regeneration, save replacement, automated emulator smoke/private
input injection, protected-session advancement, packaging/distribution,
cross-network/platform acceptance, commit, push or deployment is part of this
plan-only turn. Preserve historical reports, including the unresolved graphics
difference. The selected trial is a new explicit policy; historical evidence
continues to describe the original strict policy.
