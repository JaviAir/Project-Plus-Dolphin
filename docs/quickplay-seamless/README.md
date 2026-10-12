# Seamless Quick Play baseline before the no-scan trial

This checkpoint records the inherited persistent same-Core session work and
UI96 Step2a/Step2b implementation on `quickplay-seamless-session`. The planned
no-bulk-scan experiment will start on `qp-seamless-scan` after this commit.

The current implementation retains the running emulator, restores the common
CSS checkpoint, uses the existing fixed-delay NetPlay transport and readiness
barriers, and recovers to local CSS through the persistent retirement path.
Step2b includes in-game status/search/cancel presentation, hidden automatic
search/lobby dialogs, diagnostic access, and paused asynchronous setup
cancellation. Opponent names and the leave chord remain later UI work.

## Known limitations at this checkpoint

- Initial persistent admission pauses the Core and hashes actual content
  synchronously on the GUI thread. One user-operated run spent 58.906 seconds
  checking content; the ISO and SD image alone total about 10.6 GB.
- The strict NAND receipt includes writable title autosaves. Two autosave files
  in the latest isolated manual profile each differ from certified copies by
  seven bytes, explaining its current NAND rejection. No save-ownership fix or
  content-validation policy change is included in this checkpoint.
- Step2b source/build/focused checks passed in prior work, but physical runtime
  UI acceptance remains incomplete. This commit does not imply a new runtime
  test or a successful corrected Step2b pairing/search/cancel check.
- The previously documented graphics divergence and broader deterministic,
  cross-network and cross-platform qualification remain unresolved.

## Next experiment

[The archived trial plan](UI96-VALIDATION-OPTIONS.md) records exactly two options:
try declared-bundle admission without bulk scans next, and retain background
verification plus a persistent cache as an alternative only. The first option
keeps lightweight configuration/lifecycle checks while giving up independent
verification of actual external content. No no-scan implementation or emulator
test has started at this checkpoint.

Detailed reports, build artifacts, profiles and manual evidence remain in the
companion workspace under `planning/quickplay/phase9/`; those files are outside
this Git checkout. The archived plan is self-contained for implementation scope
and lists the relevant source paths and workspace evidence locations.

## Trial implementation

`qp-seamless-scan` implements Option A behind the off-by-default CMake option
`QUICKPLAY_NO_BULK_SCAN_TRIAL`; see [NO-BULK-SCAN-TRIAL.md](NO-BULK-SCAN-TRIAL.md).
Strict validation remains the default. Build/test/runtime status: NOT RUN.

UI96 Step3 makes local commands controller-agnostic (mapped Standard Controller
or a GameCube Adapter for Wii U port) under both policies; see
`Source/Core/DolphinQt/QuickPlay/PERSISTENT.md`. Build/test/runtime status: NOT RUN.
