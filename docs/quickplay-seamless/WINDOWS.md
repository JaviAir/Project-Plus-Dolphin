# Seamless Quick Play on Windows — build and first test

Branch `seamless-session-cross-platform`. Status: the source allows the persistent
in-game flow on Windows, but it has **not been built or run on Windows yet**.

## What changed for Windows

- The persistent flow accepted only the Qt `xcb` platform. It now accepts `xcb`
  (X11 or XWayland) and `windows`. It requires single-core mode and a
  separate render window (Render to Main Window off; Dual Core is forced off while
  Quick Play is prepared). Any graphics backend is allowed:
  the paused redraw only uses Dolphin's backend-independent drawing. Only OpenGL has
  been tested; Direct3D 11 on Windows is the first test of another backend. The
  backend is not part of the compared settings, so peers may use different backends.
- `DolphinQt.vcxproj` now builds `QuickPlay\PersistentSession.cpp` (before this the
  branch could not link on Windows) and defines `QUICKPLAY_NO_BULK_SCAN_TRIAL`, so
  Windows builds use the no-scan policy. The CMake option now defaults to `ON` too.
- Quick Play no longer uses a shared `QuickPlay\common-css.sav` bundle. Booting the
  Offline Launcher generates the starting state locally under a fixed recipe
  (`Core\QuickPlayRecipe.cpp`, registered in `DolphinLib.props`); see
  `Source/Core/DolphinQt/QuickPlay/PERSISTENT.md`. The recipe forces single-core and
  the other compared settings for that run. Admission logs every compared setting
  (`Persistent QuickPlay: config …`), so two logs can be diffed when the contract
  identities differ.

## Build

```bat
git fetch origin
git checkout seamless-session-cross-platform
git pull
git submodule update --init --recursive
```

Open `Source\dolphin-emu.sln` in the latest Visual Studio 2022 (Desktop C++ workload
and the latest Windows SDK). Build **Release | x64**. The result is
`Binary\x64\Dolphin.exe`, with `Sys` copied next to it.

**Both peers must run builds of the same commit.** The build revision is part of
the NetPlay version and of the Quick Play identity. Rebuild Linux from the same
commit as well, for example:

```sh
cmake -S Plus-Dolphin -B builds/plus-dolphin-cross-platform -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/joemarchy/Projects/pplus/build-tools/qt/6.7.3/gcc_64 -DLINUX_LOCAL_DEV=ON
mkdir -p builds/plus-dolphin-cross-platform/Binaries && ln -sn /home/joemarchy/Projects/pplus/Plus-Dolphin/Data/Sys builds/plus-dolphin-cross-platform/Binaries/Sys
cmake --build builds/plus-dolphin-cross-platform --target project-plus-dolphin -j 4
```

Then use `launch-scan-trial.py --build ../builds/plus-dolphin-cross-platform`.

## What must match between peers

From a Project+ maintainer, for ordinary P+ NetPlay:

> - both sides need to be on the exact same version of Dolphin (windows-Linux should work fine)
> - both sides need the exact same SD card image
> - both sides need the exact same brawl dump
>
> the reason all of that matters is because it's literally emulating the same console
> on both sides, and it's only synchronizing the controller inputs, nothing else. so it
> has to know that you're both starting from the *exact same state* to keep everything
> in sync

Seamless Quick Play follows the same rule. There is no shared savestate or
`QuickPlay` folder any more: each Dolphin boots the Offline Launcher under a fixed
recipe and generates the character-select starting state itself
(`User\QuickPlay\generated\origin.sav`, rewritten on every launch). The build forces
the emulation settings that matter, so only content and build must match:

| Must be identical | Where | Checked? |
| --- | --- | --- |
| Dolphin build (same commit) | `Dolphin.exe` / Linux binary | Yes: NetPlay version and Quick Play contract |
| Emulation settings | forced by the build while Quick Play is prepared | Yes: contract identity |
| Brawl dump | plain `.iso` | Only the 32-byte header (game ID `RSBE01`) |
| SD card image | `Wii\sd.raw` from the official P+ v3.2 Netplay download | Indirectly: origin digest |
| Launchers | `Launcher\Project+ Offline Launcher.dol`, `Project+ Netplay Launcher.dol` | Indirectly: origin digest |

Each peer logs an **origin digest** when preparation finishes and sends it when
joining. If the digests differ, the join is refused on both sides with
"Quick Play refused the join: the two games prepared different starting states"
instead of a desync. Matching digests on a Linux and a Windows machine is the main
thing this smoke test checks. Differences the character select screen does not
reveal (for example a different stage file on the SD) can still desync later, as
in ordinary P+ NetPlay, so compare hashes by hand when in doubt
(`Get-FileHash` / `sha256sum`). Reference values on the Linux test machine:

| File | SHA256 |
| --- | --- |
| `Wii\sd.raw` | `cfe5d86976092df50dfdcb21c627d19ba305aa5c9175827a18ca816fc1402d2d` |
| `Launcher\Project+ Offline Launcher.dol` | `db143a7592a7233664af19492074f560df1f5c2f21fb3fae3c19ab5702377205` |
| `Launcher\Project+ Netplay Launcher.dol` | `74a0decf1595725928c8830bddc17207669323883112640b0e25e9f2da95c0da` |
| Brawl ISO (`RSBE01` rev 1) | `ca53a7800d5dda83d219f835da47c87ad4e769152ce101f2ead6a4b000111874` |

## Set up a fresh Windows user folder

Start from an empty user folder; do not copy a `User` folder or `QuickPlay` files
from another machine. Without `-u`, a Windows build picks, in order: `User\` next to
the exe if `portable.txt` is there, the registry `UserConfigPath`, `Documents\Dolphin
Emulator`, then `AppData\Roaming\Dolphin Emulator`. The steps below use
`C:\pplus\User` with `-u`; File > Open User Folder shows which folder a build uses.

1. From the official **Project+ v3.2 Netplay** download for Windows, copy into
   `C:\pplus\User`:
   - `Launcher\` (both `Project+ … Launcher.dol` files with their `.png`/`.xml`)
   - `GameSettings\`
   - `Wii\sd.raw`
2. Put a **plain `.iso`** Brawl dump somewhere (not RVZ/WBFS; see "To do" below).
3. Launch once (below), close Dolphin, then in Dolphin set:
   - Config > Paths: add `C:\pplus\User\Launcher` as a game folder, and set
     Default ISO to the Brawl `.iso`. The SD card path defaults to
     `User\Wii\sd.raw`; leave it.
   - Tools > Quick Play Settings: coordinator URL and region (same on both
     machines). "Prepare in-game Quick Play when the Offline Launcher starts" must
     be checked (default).
   - NetPlay nickname, and the traversal settings below.
   - Controllers: Project+ Dolphin sets every port to "GameCube Adapter for Wii U" by
     default; without an adapter, controller 1 then reads as unplugged in game. Set
     **Controller 1** = Standard Controller mapped to your pad (an Xbox
     pad, or a GC adapter in PC mode), or "GameCube Adapter for Wii U" (needs the
     WinUSB driver via Zadig). Leave the other ports alone; the build forces
     port 2 = Standard Controller and ports 3/4 empty while Quick Play is prepared.
   You do not need to change emulation or graphics settings: the build forces the
   ones peers must share for that run without saving them.
4. To capture logs: View > Show Log Configuration, verbosity **Info**, enable
   **Write to File** and the **NetPlay** log type. The log is `User\Logs\dolphin.log`.

The Linux equivalent is `python3 Plus-Dolphin/Tools/quickplay/launch-scan-trial.py
--prepare --profile <name>` (from the pplus root), which creates a fresh profile the
same way.

While Quick Play preparation is on, the Offline Launcher runs on a temporary Wii
NAND: Brawl's own saves and tags are not loaded or kept. Uncheck the setting for
ordinary offline play with saves.

## Network for a same-house test

Dolphin's public traversal server cannot connect two machines behind the same
public IP. For Linux and Windows machines on the same LAN, use the local traversal
server on the Linux machine (`builds/plus-dolphin-baseline/Binaries/traversal_server`,
UDP 6262/6226):

- On **both** machines set `TraversalServer = <Linux LAN IP>`, `TraversalPort = 6262`,
  `TraversalPortAlt = 6226`. Do not use `127.0.0.1` on the Linux peer, because the
  server would give that address to the Windows peer.
- Allow UDP 6262/6226 and the NetPlay port through the Linux firewall.
- Set `QuickPlayCoordinator` to the Tailscale Serve URL on both machines and use
  the same region. Check `<URL>/health` from both.

Across separate networks, use the normal traversal server instead.

## Launch (update check off)

```powershell
$env:QUICKPLAY_DISABLE_UPDATE_CHECK = "1"
& "<repo>\Binary\x64\Dolphin.exe" -u "C:\pplus\User" -e "C:\pplus\User\Launcher\Project+ Offline Launcher.dol"
```

## First Windows checks — report each result

1. The game boots in a separate window. "Preparing Quick Play…" shows in the top
   right for a few seconds (the boot runs fast and muted), then the character
   select screen shows the `L + R + D-pad Up — Search` hint. Note how long the
   preparation took, and send the log line
   `Quick Play recipe portable-css-ab/2: origin digest …` from both machines.
   **The two origin digests must be equal.** If preparation fails, the overlay
   says why and local play continues; send the `Quick Play recipe:` lines.
2. Search: does `Searching in <region>…` appear immediately? Then press B to cancel.
3. Pair Windows with Linux. Do both reach the shared CSS, a full match and the
   results screen? Try both host directions. A digest mismatch shows "Quick Play
   refused the join…" on both sides; send both logs.
4. Leave with L+R+D-pad Right (hold 1 s) from each side. Do both return to local CSS
   and can both search again?
5. Note any error text, and send the `Persistent QuickPlay:` and `Quick Play recipe:`
   lines from both logs. A Windows-only crash, black screen or frozen window during
   the handoff points at the graphics restore and redraw path (Direct3D 11 or
   OpenGL/WGL), which is the main unverified part.

## To do (nice to have)

- **Accept RVZ/WBFS dumps.** This is not a scan. The no-scan build reads only the
  disc's first 32 bytes to confirm it is Brawl (`RSBE01`). It reads them straight from
  the file, and in an RVZ or WBFS file those bytes are a container header, so those
  dumps are refused even when the game data is identical. Reading the same 32 bytes
  through Dolphin's disc reader (`DiscIO`), which decodes RVZ/WBFS, would fix it.
- **Render to Main Window.** Quick Play currently needs a separate render window.
