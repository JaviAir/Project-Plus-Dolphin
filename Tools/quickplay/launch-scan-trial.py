#!/usr/bin/env python3
"""Prepare/launch a fresh-profile seamless Quick Play test (Linux).

--prepare creates a new profile from the official Project+ v3.2 Netplay content only
(launchers, GameSettings, SD image) plus a minimal Dolphin.ini naming the ISO. Nothing is
copied from another profile and there is no QuickPlay folder: the build generates its CSS
origin at boot. No content verification, services or automatic inputs.
"""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess


REPO = Path(__file__).resolve().parents[2]
ROOT = REPO.parent
DEFAULT_BUILD = ROOT / "builds/plus-dolphin-cross-platform"
TRIAL_ROOT = ROOT / "planning/quickplay/portable-sync/integration1/profiles"
OFFICIAL_USER = (ROOT / "ProjectPlus-v3.2-netplay-official/extracted/Project+.v3.2.Netplay.Linux.AppImage"
                 "/Project-Plus-Dolphin.AppImage.home/.local/share/project-plus-dolphin")
DEFAULT_ISO = ROOT / "ProjectPlus-v3.2-runtime-evidence/Brawl-RSBE01-rev1.iso"
POLICY = "quickplay-declared-bundle-trial-v1"
OFFLINE = "Project+ Offline Launcher.dol"
NETPLAY = "Project+ Netplay Launcher.dol"
# Dolphin.Core.SIDevice values: SIDEVICE_GC_CONTROLLER and SIDEVICE_WIIU_ADAPTER.
STANDARD_CONTROLLER = 6
WIIU_ADAPTER = 12
# Dolphin's Pipe input device (User/Pipes/<name>), for scripted local tests only.
PIPE_PAD = """[GCPad1]
Device = Pipe/0/pad
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
Main Stick/Up = `Axis MAIN Y -`
Main Stick/Down = `Axis MAIN Y +`
Main Stick/Left = `Axis MAIN X -`
Main Stick/Right = `Axis MAIN X +`
C-Stick/Up = `Axis C Y -`
C-Stick/Down = `Axis C Y +`
C-Stick/Left = `Axis C X -`
C-Stick/Right = `Axis C X +`
Triggers/L = `Button L`
Triggers/R = `Button R`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
[GCPad2]
Device =
[GCPad3]
Device =
[GCPad4]
Device =
"""


def candidate(build):
    binary = build / "Binaries/project-plus-dolphin"
    for path in (binary, build / "Binaries/qp-trial-policy.txt", build / "CMakeCache.txt"):
        if not path.is_file():
            raise SystemExit(f"Missing build output: {path}")
    if (build / "Binaries/qp-trial-policy.txt").read_text() != POLICY + "\n":
        raise SystemExit("Build does not declare the no-scan policy.")
    cache = (build / "CMakeCache.txt").read_text()
    for setting in ("QUICKPLAY_NO_BULK_SCAN_TRIAL:BOOL=ON", "LINUX_LOCAL_DEV:BOOL=ON",
                    "CMAKE_BUILD_TYPE:STRING=Release"):
        if setting not in cache.splitlines():
            raise SystemExit(f"Required build setting missing: {setting}")
    # The temporary Quick Play NAND comes from Sys/NetplaySave and Sys/Wii.
    for sys_dir in ("GameSettings", "NetplaySave", "Wii"):
        if not (build / "Binaries/Sys" / sys_dir).is_dir():
            raise SystemExit(f"Missing adjacent Sys/{sys_dir}; create the documented Sys link.")
    return binary


def saved_port1(user):
    """The profile's saved port 1 device; Project+ Dolphin defaults to the Wii U adapter."""
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.optionxform = str
    settings.read(user / "Config/Dolphin.ini")
    value = settings.get("Core", "SIDevice0", fallback=str(WIIU_ADAPTER))
    try:
        return int(value)
    except ValueError:
        raise SystemExit(f"Unreadable saved Core.SIDevice0: {value!r}")


def prepare(args, user):
    official = args.official_user.expanduser().resolve()
    iso = args.iso.expanduser().resolve()
    for path in (official / "Launcher" / OFFLINE, official / "Launcher" / NETPLAY,
                 official / "Wii/sd.raw", official / "GameSettings", iso):
        if not path.exists():
            raise SystemExit(f"Missing official content: {path}")
    if user.parent.exists():
        raise SystemExit(f"Profile already exists; choose a new --profile: {user.parent}")
    (user / "Config").mkdir(parents=True)
    (user / "Wii").mkdir()
    shutil.copytree(official / "Launcher", user / "Launcher")
    shutil.copytree(official / "GameSettings", user / "GameSettings")
    # A reflink where the filesystem supports it; otherwise a plain 2 GB copy.
    subprocess.run(["cp", "--reflink=auto", str(official / "Wii/sd.raw"), str(user / "Wii/sd.raw")],
                   check=True)
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.optionxform = str
    settings["General"] = {"ISOPaths": "1", "ISOPath0": str(user / "Launcher")}
    settings["Core"] = {"DefaultISO": str(iso)}
    if args.pipe_input:
        # Project+ Dolphin defaults every port to the Wii U adapter; the pipe drives a
        # Standard Controller.
        settings["Core"]["SIDevice0"] = str(STANDARD_CONTROLLER)
    settings["Analytics"] = {"Enabled": "False", "PermissionAsked": "True"}
    settings["NetPlay"] = {
        "Nickname": args.nickname or f"QP-{args.profile}",
        "QuickPlayCoordinator": args.coordinator,
        "QuickPlayRegion": args.region,
        "TraversalChoice": "traversal",
        "TraversalServer": args.traversal,
        "TraversalPort": "6262",
        "TraversalPortAlt": "6226",
    }
    if args.pipe_input:
        # Scripted input and the Take Screenshot hotkey work while the window is unfocused.
        settings["Input"] = {"BackgroundInput": "True"}
        settings["General"]["HotkeysRequireFocus"] = "False"
        (user / "Pipes").mkdir()
        os.mkfifo(user / "Pipes/pad")
        os.mkfifo(user / "Pipes/hk")
        (user / "Config/GCPadNew.ini").write_text(PIPE_PAD)
        (user / "Config/Hotkeys.ini").write_text(
            "[Hotkeys]\nDevice = Pipe/0/hk\nGeneral/Take Screenshot = `Button A`\n")
    with (user / "Config/Dolphin.ini").open("w") as file:
        settings.write(file)
    (user.parent / "preparation.json").write_text(json.dumps({
        "kind": "fresh official-content profile", "official_user": str(official),
        "iso": str(iso), "user": str(user), "pipe_input": args.pipe_input,
        "quickplay_folder": "none (generated at boot)",
    }, indent=2) + "\n")
    print(f"Prepared fresh profile: {user}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="a", help="profile name (a new folder per --prepare)")
    parser.add_argument("--prepare", action="store_true", help="create a fresh profile; never launch")
    parser.add_argument("--official-user", type=Path, default=OFFICIAL_USER,
                        help="official P+ v3.2 Netplay user folder (Launcher, GameSettings, Wii/sd.raw)")
    parser.add_argument("--iso", type=Path, default=DEFAULT_ISO, help="plain Brawl RSBE01 .iso")
    parser.add_argument("--nickname", help="NetPlay nickname for the new profile")
    parser.add_argument("--coordinator", default="http://127.0.0.1:30997")
    parser.add_argument("--region", default="na-east")
    parser.add_argument("--traversal", default="127.0.0.1")
    parser.add_argument("--pipe-input", action="store_true",
                        help="map controller 1 to the User/Pipes/pad FIFO and Take Screenshot to "
                             "User/Pipes/hk (scripted tests)")
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD,
                        help=f"build directory (default: {DEFAULT_BUILD})")
    parser.add_argument("--backend", default="OGL", help="graphics backend for this launch")
    parser.add_argument("--wiiu-adapter", action="store_true",
                        help="override port 1 = GameCube Adapter for Wii U (adapter port 1) for "
                             "this launch; by default port 1 is the profile's saved setting")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="bounded existence checks only")
    mode.add_argument("--show-command", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", args.profile):
        parser.error("--profile must be a simple name, without path separators")
    if args.prepare and (args.check or args.show_command or args.wiiu_adapter):
        parser.error("--prepare never launches")
    user = TRIAL_ROOT / args.profile / "User"
    if args.prepare:
        prepare(args, user)
        return
    binary = candidate(args.build.expanduser().resolve())
    launcher = user / "Launcher" / OFFLINE
    for path in (launcher, user / "Launcher" / NETPLAY, user / "Wii/sd.raw"):
        if not path.is_file():
            raise SystemExit(f"Missing profile file: {path}")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PPLUS_")}
    environment.pop("DOLPHIN_EMU_USERPATH", None)
    environment["QT_QPA_PLATFORM"] = "xcb"
    # Skip the startup GitHub update popup, which blocks the boot until dismissed.
    environment["QUICKPLAY_DISABLE_UPDATE_CHECK"] = "1"
    environment["LD_LIBRARY_PATH"] = str(ROOT / "build-tools/qt/6.7.3/gcc_64/lib")
    arguments = [str(binary), "-u", str(user), "-e", str(launcher)]
    port1 = WIIU_ADAPTER if args.wiiu_adapter else saved_port1(user)
    if port1 not in (STANDARD_CONTROLLER, WIIU_ADAPTER):
        raise SystemExit(f"Port 1 SIDevice0={port1} is unsupported; set Standard Controller or "
                         "GameCube Adapter for Wii U in Controllers, or pass --wiiu-adapter.")
    overrides = [f"Dolphin.Core.SIDevice0={WIIU_ADAPTER}"] if args.wiiu_adapter else []
    # Quick Play recipe mode forces the emulation settings; these are host/window choices.
    for setting in (
        f"Dolphin.Core.GFXBackend={args.backend}", *overrides,
        "Dolphin.Core.BootToPause=False", "Dolphin.Display.RenderToMain=False",
        "Dolphin.Display.Fullscreen=False", "Logger.Options.WriteToFile=True",
        "Logger.Options.WriteToConsole=True", "Logger.Options.Verbosity=4",
        "Logger.Logs.NETPLAY=True",
    ):
        arguments.extend(("-C", setting))
    if args.check:
        print(f"Existence checks complete; content verification skipped; policy {POLICY}")
        print(f"Build: {binary}; profile: {user}; port 1 SIDevice0={port1}.")
    elif args.show_command:
        print(shlex.join(arguments))
    else:
        os.execve(str(binary), arguments, environment)


if __name__ == "__main__":
    main()
