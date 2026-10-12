# Declared-bundle Quick Play trial (Option A, no bulk scan)

Isolated, user-operated UI trial of [Option A](UI96-VALIDATION-OPTIONS.md).
Option B (background verification plus a cross-launch cache) is not implemented.
Build, test and runtime status at hand-off: **NOT RUN**.

## Selection

The ordinary CMake option `QUICKPLAY_NO_BULK_SCAN_TRIAL` (default `OFF`) defines
`QUICKPLAY_NO_BULK_SCAN_TRIAL` for `project-plus-dolphin` only. Without it the
strict validator, prepaint and "Checking game files" stage are unchanged.
`QuickPlayContent::SelectedPolicy()` is a compile-time constant; each
`PersistentQuickPlaySession` copies it at construction, so the policy cannot
change during a handoff. A trial build writes `qp-trial-policy.txt`
(`quickplay-declared-bundle-trial-v1`) next to the executable after a successful
link; the trial launcher checks that file and the build's `CMakeCache.txt`.

## What the trial checks and skips

Retained: Core running, single-core, DSP HLE, read-only SD, no movie/Bluetooth
passthrough, fixed-delay NetPlay without save/code sync, recording or
spectators, game ID RSBE01, empty EXI slots, GC controllers on SI 1/2 (local
port 1 may be a Wii U adapter port driving the standard controller) and none
on 3/4, the XCB/OpenGL external-window route, schema 1, the pinned declared
checkpoint identity, effective NetPlay configuration identity, well-formed
64-hex declarations for all eight keys, required file/directory existence,
readability and open, CSS eligibility, Core/input/room/window fences, readiness,
attach and release barriers, ordinary checkpoint loading, and restored
PC/ticks/SI device checks.

Skipped in every lifecycle stage (initial admission, matched revalidation,
restoration, repeat search): `HashFile`/`HashTree` over ISO, SD, NAND, both
launchers, user/system game settings and the checkpoint, including the
separate checkpoint hash in `PersistentSession::Restore`. Writable autosaves are
therefore never compared with certified hashes. There is no background,
deferred or partial replacement scan.

I/O audit of the remaining checks:

- Manifest: at most 64 KiB read (`ReadManifest`).
- Inserted disc: `DVDThread::GetInsertedDiscHeader` reads exactly 32 raw header
  bytes from a plain Wii ISO; the configured default ISO is read for 32 bytes
  with `QFile`. Both must match and be RSBE01 with the Wii magic. The existing
  sync hash was not reused because it reads apploader, DOL, FST and banner at
  sizes declared by the disc. Compressed formats (RVZ/WBFS) fail closed.
- Launcher identity: `FindPPlusNetplayLauncher` uses game-list metadata only.
  NetPlay advertises a declared identity derived in memory from the manifest's
  `netplay` digest, the running build and the file size. The owned trial
  `NetPlayDialog::FindGameFile` compares that copy and never calls
  `GameFile::CompareSyncIdentifier` (which hashes the DOL).
- Existence checks open and close files without reading their contents.

## Peer identity

`ContractIdentity` keeps strict's identity byte-for-byte (SHA256 of the compact
manifest). For the trial it prepends `quickplay-declared-bundle-trial-v1` and the
running build before hashing. The result uses the existing 64-character
readiness identity, so a strict peer, a different trial version or a different
trial build cannot ready together with this one. The declared launcher identity
is also build/policy-specific, so a strict peer reports a different launcher.

A matched revalidation rejects the handoff if the declared identity changed
since the initial admission of that search.

## Build/bundle compatibility

`build` is `GetScmRevGitStr() + "-persistent1"`, i.e. the raw `HEAD` revision. The
existing certified bundle declares `c5df493c211fde31791de47085df09be5bf9144b-persistent1`
(Step2b was built before the `7164321368` checkpoint commit). Strict mode still
requires the exact running build. The trial additionally accepts exactly that
archived build string (`QuickPlayContent::ARCHIVED_BUILD`); any other mismatch
still fails. The receipt is not regenerated or rewritten. Serializer
compatibility of the checkpoint is still enforced by the ordinary state loader
and the PC/ticks/device checks. Both peers must run the same trial build,
because the build is part of the trial contract identity.

## Presentation and diagnostics

Trial admission keeps the short pause and the same fences, but skips the
prepaint and does not publish "Checking game files". It completes on the next
event-loop turn, then publishes Searching. If anything is drawn in between, it
reads "Preparing session…" and is not cancellable, as before. Logs and the
NetPlay diagnostics chat say "declared bundle accepted; content verification
skipped", never "verified". Ordinary in-game text does not name the policy.

## Launcher

`Tools/quickplay/launch-scan-trial.py` is separate from `launch-xbox.py` and its
pinned Step2b hash. `--prepare --profile NAME` copies the current
`ui96-manual/xbox/User` (saves included, original receipts byte-for-byte) into
`planning/quickplay/phase9/ui96-manual/qp-seamless-scan/NAME/User`, rebases
profile paths and sets a nickname. It refuses to overwrite an existing
destination. `--check` and the default launch do bounded declaration and
existence checks only, with no content hashing and no build report. Prepare two
profiles for local pairing.

`--build DIR` selects another trial build directory (default
`builds/plus-dolphin-qp-seamless-scan`); it must pass the same policy-file,
`CMakeCache.txt` and `Sys/GameSettings` checks. The launcher forces
`SIDevice0=6` and `SIDevice1=6` on the command line; `--wiiu-adapter` instead
sets `SIDevice0=12` (GameCube Adapter for Wii U, adapter port 1) for that launch
only. Saved profile settings are not modified.

## Not established

This trial does not prove that peers' storage matches, does not fix save
ownership, and does not certify content. UI acceptance, pairing and determinism
remain open until the user-operated checks are recorded.
