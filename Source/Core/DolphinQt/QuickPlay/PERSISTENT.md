# Persistent Quick Play (first CSS lifecycle)

This path uses the existing coordinator, traversal connection, host Start Game,
and fixed-delay Dolphin NetPlay. The coordinator never carries controller inputs.
An explicit `PersistentQuickPlaySession` owns origin admission and retirement;
ordinary desktop/conventional NetPlay retains its boot/stop lifecycle.

## Generated origin (no shared bundle)

There is no `QuickPlay/common-css.sav` or `common-css.json` to install or copy.
Each Dolphin generates the shared CSS origin itself, under a fixed recipe, during
its own boot (`Core/QuickPlayRecipe.{h,cpp}`, recipe `portable-css-ab/3`: `/2` from the
portable-sync research, with a backend-independent digest). On join, each peer loads
its *own* generated origin.

**When.** DolphinQt boots a DOL whose game ID is `ID-Project+ Offline Launcher`
(official content) or `ID-Offline Launcher`, outside NetPlay and movies, with
Quick Play settings > "Prepare in-game Quick Play when the Offline Launcher
starts" on (`[NetPlay] QuickPlaySeamless`, default on). Other boots, the nogui
frontend and ordinary NetPlay are unchanged.

**Recipe.** For the whole Core lifetime:

- Clock: EXI IPL RTC, `/dev/dolphin` time and memcard format time use
  `0x6955b900 + floor(ticks / ticks_per_second)` (2026‑01‑01). Custom RTC is on
  with the same value, so NetPlay continues the same clock after a join. The
  epoch must stay at least 71 s after 2000‑01‑01 (guest system time is the time
  base plus the −2³² bias Wii HLE presets; a negative value stalls SI detection).
- Storage: a private temporary Wii NAND (`User/WiiSession`) from build-owned
  `Sys/NetplaySave` + `Sys/Wii`, the NAND serial fallback `123456789`, the temporary
  NAND's own SYSCONF (BootCore writes no user Wii settings into it), read-only SD.
  The temporary NAND is never copied back to the configured NAND. **The game's own
  saves, tags and records from the configured NAND are not used or kept** in this
  mode; turn the setting off for ordinary offline play with saves.
- Settings forced in the CurrentRun layer (above game INIs; never saved):
  everything the NetPlay contract compares or the guest can observe — single-core
  JIT (platform default core), DSP HLE, CPU accuracy flags, no overclock/RAM
  override, empty EXI slots, SI standard/standard/none/none, no Wii remotes,
  Bluetooth/USB passthrough or emulated USB devices, SD on/read-only/no folder sync,
  cheats on, GC language 0, region override off, custom RTC = epoch, fixed
  WirelessMac `00:17:ab:0a:5a:79`, synchronous shader compilation, the
  research recipe's EFB/XFB hacks, no custom textures or graphics mods, and the
  persistent NetPlay options (fixed delay, no save/code sync, recording or
  spectating). Configured Wii system settings (SYSCONF) are pinned to their
  defaults for the run, since the guest does not see them.
- Left to the player: controller mapping and device, graphics backend and
  adapter, the settings ordinary NetPlay only syncs with Strict Settings Sync
  (internal resolution, MSAA/SSAA, anisotropy, texture filtering, widescreen hack,
  per-pixel lighting, fog, true color, copy filter, ...; they are not in the
  contract identity and can change mid-session), window/fullscreen, audio backend and volume, emulation speed, OSD,
  hotkeys and coordinator/traversal/nickname settings.
- Preparation only: unlimited speed and muted audio until the origin, then the
  player's own speed and volume.

**Controllers.** Until the origin, guest ports 1/2 read a connected neutral
standard controller and no physical input is sampled. A GameCube Adapter for
Wii U on local port 1 keeps its `SIDevice0 = 12` setting: SI init in recipe mode
creates the standard controller `ContractSIDevice` names, so the generated
origin has standard controllers on both ports on every peer. After capture the
standard controller samples the adapter port (as below), and the restore's
device pinning keeps it. Guest ports 2-4 then get their configured devices back
(`QuickPlayRecipe::ReleaseExtraPorts`), after capture and after a match returns to
the local CSS, so an unconfigured second port shows no hand outside a match. A
match restores the origin, which brings back its standard controller on port 2.
The game image is covered (black, Quick Play status on top) while the origin is
prepared and after each release until the CSS has dropped the port 2 hand for 3
fields (about 0.3 s; 1.5 s at most), so that hand is never shown locally.

**Leave.** A persistent session's NetPlay client disconnects without waiting for
the server's acknowledgement: the stop is already agreed, and the host's own
server (or a leaving host's server) is gone, so the graceful wait only froze that
player for its 3 s timeout.

**Capture.** The origin is the third consecutive CSS update entry (module 10
text + 0x4e10, word `9421ffe0`) where predicate `css-init-v1` holds (published
CSS task settled, both standard controllers detected with their areas active,
ports 3/4 absent, only neutral input consumed, no guest I/O in flight or issued
since the previous entry). The entry is watched with a `BreakPoints` capture hook
that Jit64, JitArm64 and the cached interpreter check without debug mode;
`DebugModeEnabled` is not used. The CPU stops before the entry executes; a CPU
job computes the origin digest, saves `User/QuickPlay/generated/origin.sav`
(`State::SaveForPersistentSession`: synchronous, no OSD, no undo backup) and
`origin.json`, removes the hook, and the host resumes the same Core. Search stays
unavailable ("Preparing Quick Play…") until then. If the predicate is not met
within 20 s of emulated time, or the CSS code is not the supported build,
preparation fails: the overlay says why, local play continues and Quick Play
admission reports the reason.

**Identity.** The origin digest is XXH3 over the MEM1, MEM2 and CPU register
digests, PC, ticks and the digest of the EFB/XFB copy ranges at capture. Memory
written by EFB/XFB copies during preparation is left out of the MEM1/MEM2 digests:
graphics backends round those pixels differently (OpenGL and Vulkan differed in
one 640×480 RGBA8 copy), and the game only displays them, as in ordinary NetPlay
between peers on different backends. Admission computes the contract identity
`SHA256(policy, build revision, recipe id, RunningNetPlayConfigIdentity())` and
sends identity, origin digest and the hashed contract text in `PersistentReady`.
The host compares both; on a difference it sends `PersistentMismatch` (both
digests and up to 8 differing contract lines with both values) before `StopGame`,
so both peers log `join refused … origin digest local X peer Y` and show the
refusal, naming the differing settings, instead of an opponent leave. A setting
the host's NetPlay settings carry is checked earlier on the joining peer
(`ValidateRunningNetPlayConfig`); that peer names it ("yours X, host's Y") and
backs out, and the host sees it leave. `DSPThread` is no longer part of the
configuration identity (Core resets it from the host core count; DSP HLE
ignores it).

Restore loads the private `origin.sav` and accepts it only if the loaded PC,
ticks and recomputed digest equal the capture. Leave restores the same origin
(local recovery). The file is regenerated on every launch; there is no cache.

## Supported profile

The running local game must use an external render window on X11 (including
XWayland) or Windows, game ID RSBE01 after the launcher, standard GC controllers
on guest ports 1/2 (local port 1 may be driven by a GameCube Adapter for Wii U
port; see Local controller below) and no movies or recording. Recipe mode
provides the rest. Content follows ordinary P+ NetPlay's trust model: both
players use the same Dolphin build, the official Project+ v3.2 Netplay SD image
and launchers, and the same plain Brawl ISO. The no-scan build identifies the
inserted disc by its 32-byte header only. A content difference shows up as an
origin digest mismatch at join (refused) or, if the CSS does not reveal it, as a
later desync.

The `Launcher` folder must be one of the game list paths, so the NetPlay room can
name the Netplay launcher (`Project+ Netplay Launcher.dol`, or
`Netplay Launcher.dol` from this repository's `Data/user`). The no-scan build
declares it from the build, recipe, file name and size without reading it.

Both peers must use this protocol build. Profiles without recipe mode retain
conventional Quick Play behavior.

## Local controller

Local commands read one source, `Core/ProjectPlusOnline/LocalInput.h`: the logical
GC buttons of this instance's local controller 1. Port 1 may be a Standard
Controller (any mapping, including keyboard, evdev/SDL or a PC-mode adapter) or a
"GameCube Adapter for Wii U" (its port 1); other types support no local commands.
The local CSS SI branch of both device classes, the paused-setup B observer and
NetPlay local sampling all select the source by the same rule. Commands use digital
L/R (full press). Held input never becomes a fresh press across a source change,
input epoch or attempt.

The adapter is a local input source, not a different guest device. The origin
serializes each SI device type with standard controllers on ports 1/2; restore
pins the desired types to them; a persistent NetPlay mapping requests the
standard controller for the local port; the attach/restore checks still require
it. Both peers therefore emulate identical devices and receive identical NetPlay
pad records; only the local sampler and rumble output differ. The configuration
identity records that guest device, so an adapter peer and a mapped peer agree,
while any other port-2/3/4 or device difference still mismatches.

## Exercise

Boot each profile's Offline Launcher. "Preparing Quick Play…" shows for a few
seconds while the origin is generated; then the CSS hint appears. Each player
uses their local controller 1. Release the chord, then press L + R + D-pad Up.
This invokes the existing `StartQuickPlay()` GUI sequence and coordinator search.
B cancels a pending local search after a neutral release.

`Tools/quickplay/launch-scan-trial.py --prepare` creates a fresh profile from the
official content (Linux); `docs/quickplay-seamless/WINDOWS.md` has the Windows
steps. Set each profile's coordinator/traversal settings to live services and
configure its local GC controller 1 for the player's own input device.

On assignment, both Cores pause and restore their own generated origin with fresh
character selections. Each validates NetPlay settings/devices and sends its
contract identity and origin digest. The host sends attach only after both ready
messages agree, then releases input only after both peers acknowledge ordinary
NetPlay activation. The existing pad sampling, ENet, per-port delay FIFOs and
relative timebase counter handle execution. Play/Frame Advance cannot bypass this
paused setup boundary.

Close the NetPlay room or request Stop Game from either peer to end the shared
session on both peers. Retirement wakes blocked input waits before taking CPU or
NetPlay locks, detaches runtime ownership, removes the NetPlay config layer,
fences callbacks, destroys session objects, then restores and resumes clean local
CSS. Release the chord and search again. Pre-search selections are not restored.
Application Stop/Close still stops the Core; it does not perform local CSS recovery.

Setup failure/disconnect uses the same retirement path. A failed origin restore
keeps the Core paused. Generation, attempt, game and input-epoch fences prevent
stale callbacks from activating another session.

## Known limits

- Cross-machine and cross-platform origin equality is not yet proven; the
  logged origin digests are the check.
- Local history that writes NAND before a join (tags, saves) is not undone by
  restoring the origin's RAM; arbitrary local history in general is unproven.
- After a load, the post-load redraw can show one green frame (cosmetic).
- Graphics divergence, gameplay qualification and screen-text polish remain
  deferred.
