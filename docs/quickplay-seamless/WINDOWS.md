# Seamless Quick Play on Windows — build and first test

Branch `seamless-session-cross-platform`. Status: the source allows the persistent
in-game flow on Windows, but it has **not been built or run on Windows yet**.

## What changed for Windows

- The persistent flow accepted only the Qt `xcb` platform. It now accepts `xcb`
  (X11 or XWayland) and `windows`. It still requires OpenGL, single-core mode and a
  separate render window (not Render to Main Window).
- `DolphinQt.vcxproj` now builds `QuickPlay\PersistentSession.cpp` (before this the
  branch could not link on Windows) and defines `QUICKPLAY_NO_BULK_SCAN_TRIAL`, so
  Windows builds use the no-scan policy. The CMake option now defaults to `ON` too.
- If admission rejects the bundle's build or configuration, the log now lists each
  differing setting (`Persistent QuickPlay: bundle expects …` / `running config has …`).

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

Seamless Quick Play relies on the same rule, with a few more inputs because it starts
from a saved state instead of a fresh boot:

| Must be identical | Where | Checked by the no-scan build? |
| --- | --- | --- |
| Dolphin build (same commit) | `Dolphin.exe` / Linux binary | Yes: NetPlay version and Quick Play identity |
| Brawl dump | plain `.iso` | Only the 32-byte header (game ID `RSBE01`); compare `Get-FileHash` / `sha256sum` with the manifest's `iso` by hand |
| SD card image | `Wii\sd.raw` | No (it must exist) |
| CSS checkpoint and manifest | `QuickPlay\common-css.sav`, `common-css.json` | Checkpoint version, PC and ticks only; the manifest is part of the identity |
| NAND | `Wii\` | No |
| Launchers | `Launcher\` | Declared identity plus file size |
| Dolphin settings that affect emulation | `Config\`, `GameSettings\` | Yes: the configuration identity |

Anything marked "No" is trusted, the same way ordinary P+ NetPlay trusts the SD card.
A difference there does not produce an error; it shows up later as a desync. Copying one
working `User` folder and one ISO to every machine (below) is what keeps them identical.

## Set up the Windows user folder

1. Copy a working Linux trial profile's whole `User` folder to Windows (for example
   `planning/quickplay/phase9/ui96-manual/qp-seamless-scan/b/User` to
   `C:\pplus\User`). It contains everything that must match: `QuickPlay\`
   (`common-css.sav`, `common-css.json`), `Wii\` (NAND and `sd.raw`), `Launcher\`,
   `GameSettings\` and `Config\`.
2. Copy the **same ISO file** (a plain `.iso`, not RVZ/WBFS). `common-css.json`'s
   `iso` value is the SHA256 of the original. `Get-FileHash <iso>` must print the same
   hash (Windows prints it in uppercase).
3. Edit `C:\pplus\User\Config\Dolphin.ini` and change only these lines to Windows paths:
   `WiiSDCardPath`, `NANDRootPath`, `ISOPath0` (the `Launcher` folder) and `DefaultISO`.
   Set `Nickname` and the `[NetPlay]` network lines (see below). Leave everything else
   alone: the settings must match the bundle's `config` exactly.
4. In Dolphin, map **Controller 1** to your controller as a Standard Controller (an
   Xbox pad, or a GC adapter in PC mode). Leave Controller 2 unmapped. A "GameCube
   Adapter for Wii U" port 1 also works, but Windows needs the WinUSB driver for it
   (Zadig).
5. To capture logs: View > Show Log Configuration, verbosity **Info**, enable
   **Write to File** and the **NetPlay** log type. The log is `User\Logs\dolphin.log`.

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

1. The game boots in a separate window and reaches the character select screen.
   Is the `L + R + D-pad Up — Search` hint shown?
2. Search: does `Searching in <region>…` appear immediately? Then press B to cancel.
3. Pair Windows with Linux. Do both reach the shared CSS, a full match and the
   results screen? Try both host directions.
4. Leave with L+R+D-pad Right (hold 1 s) from each side. Do both return to local CSS
   and can both search again?
5. Note any error text, and send the `Persistent QuickPlay:` lines from both logs.
   A Windows-only crash, black screen or frozen window during the handoff points
   at the OpenGL/WGL restore and redraw path, which is the main unverified part.
