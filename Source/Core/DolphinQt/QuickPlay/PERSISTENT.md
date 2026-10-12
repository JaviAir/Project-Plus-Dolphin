# Persistent Quick Play (first CSS lifecycle)

This path uses the existing coordinator, traversal connection, host Start Game,
and fixed-delay Dolphin NetPlay. The coordinator never carries controller inputs.
An explicit `PersistentQuickPlaySession` owns checkpoint admission and retirement;
ordinary desktop/conventional NetPlay retains its boot/stop lifecycle.

## Supported bundle

The running local game must use the validated external XCB/OpenGL window,
single-core RSBE01, DSP HLE, read-only SD, standard GC controllers on guest ports
1/2 (local port 1 may be driven by a GameCube Adapter for Wii U port; see Local
controller below), no other SI/EXI peripherals, movies, Bluetooth passthrough, save/code sync,
recording, spectators or host input authority. Keep the validated graphics
configuration. This is CSS lifecycle support, not gameplay qualification.

Install the certified bundle under the isolated profile's `User/QuickPlay/`:

- `common-css.sav`: the validated Phase 9.3 aligned common CSS capture, SHA256
  `5f39d05f1059b67d9c9796e3709ec0337dfa1740099d97342026105292a25ce9`.
- `common-css.json`: schema 1, exact `build` and effective `config` identity,
  plus SHA256 receipts named `checkpoint`, `offline`, `netplay`, `iso`, `sd`,
  `nand`, `game_settings` and `system_settings`.

`build` is the SCM revision plus `-persistent1`. Configuration identity comes from
`ConfigLoaders::RunningNetPlayConfigIdentity()`, including effective game layers
and the guest-visible SI devices (`ProjectPlusOnline::ContractSIDevice`). Tree receipts hash sorted relative filenames, newline, file
SHA256, newline. The manifest is a certification receipt for the checkpoint's
content/configuration, not a way to approve arbitrary replacement content.
Do not regenerate receipts to bypass a failed compatibility check.

The NAND content tree excludes `shared2/wc24/nwc24dl.bin` (host timer download
retry bookkeeping) and `fst.bin` (the host filesystem index rebuilt by ordinary
state loading). `/tmp` contents belong to the pinned serialized checkpoint, which
restores them through the ordinary loader, rather than the external-storage
receipt. External title data and SYSCONF remain included. This does not change
any guest state or graphics comparator policy.

The bounded Phase 9.5 tooling provisions this bundle from the verified Phase 9.1
fixture and the unchanged Phase 9.3 aligned serializer payload, and records its live configuration. Its private command surface, input
overrides and readiness hold are test instrumentation, not application APIs.
Successful run profiles contain concrete manifest examples. Retail content and
the checkpoint are not shipped in this source tree.

Both peers must use this protocol build and identical certified receipts.
Validation checks the actual inserted disc as well as the configured ISO.
The first search pauses while large files are verified, before queue admission;
unchanged files reuse verified receipts keyed by size and modification/change
times. A configured but incompatible bundle fails closed before matchmaking.
Profiles without this bundle retain conventional Quick Play behavior.

A build configured with `QUICKPLAY_NO_BULK_SCAN_TRIAL=ON` instead admits the
declared bundle without content hashing and uses a policy-tagged contract
identity; see `docs/quickplay-seamless/NO-BULK-SCAN-TRIAL.md`. Strict and trial
peers cannot ready together.

## Local controller

Local commands read one source, `Core/ProjectPlusOnline/LocalInput.h`: the logical
GC buttons of this instance's local controller 1. Port 1 may be a Standard
Controller (any mapping, including keyboard, evdev/SDL or a PC-mode adapter) or a
"GameCube Adapter for Wii U" (its port 1); other types support no local commands.
The local CSS SI branch of both device classes, the paused-setup B observer and
NetPlay local sampling all select the source by the same rule. Commands use digital
L/R (full press). Held input never becomes a fresh press across a source change,
input epoch or attempt.

The adapter is a local input source, not a different guest device. The checkpoint
serializes each SI device type and restores standard controllers on ports 1/2;
restore pins the desired types to them; a persistent NetPlay mapping requests the
standard controller for the local port; the attach/restore checks still require
it. Both peers therefore emulate identical devices and receive identical NetPlay
pad records; only the local sampler and rumble output differ. The configuration
identity records that guest device, so an adapter peer and a mapped peer with the
same receipt agree, while any other port-2/3/4 or device difference still
mismatches. The certified receipt is unchanged.

## Exercise

Boot each isolated profile's Offline Launcher into local CSS. Each player uses
their local controller 1. Release the chord, then press L + R + D-pad Up.
This invokes the existing `StartQuickPlay()` GUI sequence and coordinator search.
B cancels a pending local search after a neutral release.

Keep the effective launcher overrides used by the fixture when launching the
production binary: `-C Dolphin.Core.CPUThread=False`,
`-C Dolphin.Core.GFXBackend=OGL`, and `-C Dolphin.Core.SIDevice1=6`
(`SIDevice0` is 6, or 12 for a Wii U adapter port).
Set each profile's coordinator/traversal settings to live services and configure
its local GC controller 1 for the player's own input device. Controller bindings
and coordinator addresses are independent of the certified content receipt.
The example test services stop with their owner; the saved profiles do not imply
that those listeners remain alive. The profile must still match the receipt in
its running local CSS; application Stop can flush settings/clean temporary files.

For the bounded automated demonstration, run
`python3 planning/quickplay/phase9/lifecycle95-tooling/build.py`, then
`python3 planning/quickplay/phase9/lifecycle95-tooling/run.py` from the pplus root.
The driver creates fresh profiles/services and closes only its own verified peers.

On assignment, both Cores pause and restore the common CSS with fresh character
selections. Each validates NetPlay settings/devices and sends its contract identity.
The host sends attach only after both ready messages agree, then releases input
only after both peers acknowledge ordinary NetPlay activation. The existing pad
sampling, ENet, per-port delay FIFOs and relative timebase counter handle execution.
Play/Frame Advance cannot bypass this paused setup boundary.

Close the NetPlay room or request Stop Game from either peer to end the shared
session on both peers. Retirement wakes blocked input waits before taking CPU or
NetPlay locks, detaches runtime ownership, removes the NetPlay config layer,
fences callbacks, destroys session objects, then restores and resumes clean local
CSS. Release the chord and search again. Pre-search selections are not restored.
Application Stop/Close still stops the Core; it does not perform local CSS recovery.

Setup failure/disconnect uses the same retirement path. A failed checkpoint
restore keeps the Core paused. Generation, attempt, game and input-epoch fences
prevent stale callbacks from activating another session.

Graphics divergence, gameplay qualification and screen-text polish remain
deferred. No guest-memory patches, shader changes or comparator changes are part
of this implementation.
