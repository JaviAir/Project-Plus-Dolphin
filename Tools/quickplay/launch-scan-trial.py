#!/usr/bin/env python3
"""Prepare/launch a separate user-operated Quick Play declared-bundle trial.

No content verification, services, automatic inputs or agent build report.
Preparation copies current profile state, including saves and original receipts.
"""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import shlex
import shutil


REPO = Path(__file__).resolve().parents[2]
ROOT = REPO.parent
DEFAULT_BUILD = ROOT / "builds/plus-dolphin-qp-seamless-scan"
TRIAL_ROOT = ROOT / "planning/quickplay/phase9/ui96-manual/qp-seamless-scan"
POLICY = "quickplay-declared-bundle-trial-v1"
ARCHIVED_BUILD = "c5df493c211fde31791de47085df09be5bf9144b-persistent1"
CHECKPOINT = "5f39d05f1059b67d9c9796e3709ec0337dfa1740099d97342026105292a25ce9"
# Dolphin.Core.SIDevice values: SIDEVICE_GC_CONTROLLER and SIDEVICE_WIIU_ADAPTER.
STANDARD_CONTROLLER = 6
WIIU_ADAPTER = 12
KEYS = ("checkpoint", "offline", "netplay", "iso", "sd", "nand", "game_settings", "system_settings")


def manifest(user):
    with (user / "QuickPlay/common-css.json").open("rb") as file:
        data = file.read(65537)
    if len(data) > 65536:
        raise SystemExit("Manifest exceeds 64 KiB.")
    receipt = json.loads(data)
    if (receipt.get("schema") != 1 or receipt.get("checkpoint") != CHECKPOINT
            or not isinstance(receipt.get("config"), str) or not receipt["config"]
            or any(not re.fullmatch(r"[0-9a-f]{64}", str(receipt.get(key, ""))) for key in KEYS)):
        raise SystemExit("Unsupported/malformed declared bundle.")
    # The C++ validator enforces archived-or-exact-current build and effective config.
    if receipt.get("build") != ARCHIVED_BUILD:
        print("Bundle requires the candidate's exact SCM build; runtime admission will check it.")
    return receipt


def candidate(build):
    binary = build / "Binaries/project-plus-dolphin"
    for path in (binary, build / "Binaries/qp-trial-policy.txt", build / "CMakeCache.txt"):
        if not path.is_file():
            raise SystemExit(f"Missing manual build output: {path}")
    if (build / "Binaries/qp-trial-policy.txt").read_text() != POLICY + "\n":
        raise SystemExit("Candidate does not declare the required trial policy.")
    cache = (build / "CMakeCache.txt").read_text()
    for setting in ("QUICKPLAY_NO_BULK_SCAN_TRIAL:BOOL=ON", "LINUX_LOCAL_DEV:BOOL=ON",
                    "CMAKE_BUILD_TYPE:STRING=Release"):
        if setting not in cache.splitlines():
            raise SystemExit(f"Required candidate setting missing: {setting}")
    # Same requirement as Step2b and trial admission: the directory exists (not hashed).
    if not (build / "Binaries/Sys/GameSettings").is_dir():
        raise SystemExit("Missing adjacent Sys/GameSettings; create the documented Sys link.")
    return binary


def saved_port1(user):
    """The profile's saved port 1 device; Dolphin uses Standard Controller when absent."""
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.optionxform = str
    settings.read(user / "Config/Dolphin.ini")
    value = settings.get("Core", "SIDevice0", fallback=str(STANDARD_CONTROLLER))
    try:
        return int(value)
    except ValueError:
        raise SystemExit(f"Unreadable saved Core.SIDevice0: {value!r}")


def prepare(args, user, binary):
    source = args.source_user.expanduser().resolve()
    if not source.is_dir() or not (source / "Config/Dolphin.ini").is_file():
        raise SystemExit(f"Missing current source profile: {source}")
    manifest(source)
    # Refuse reuse/overwrite even after partial preparation. The user chooses a
    # new name for another copy. Copy actual bytes behind symlinks to isolate saves.
    if user.parent.exists():
        raise SystemExit(f"Trial destination already exists; choose a new --profile: {user.parent}")
    user.parent.mkdir(parents=True)
    # Skip named pipes/sockets (e.g. old research input pipes); no input injection.
    def special(directory, names):
        return [name for name in names
                if not (os.path.isfile(os.path.join(directory, name))
                        or os.path.isdir(os.path.join(directory, name)))]
    shutil.copytree(source, user, symlinks=False, ignore=special)
    # Rebase copied absolute profile paths. Keep external ISO paths and the
    # original manifest byte-for-byte. Do not install archived/certified saves.
    for ini in (user / "Config").glob("*.ini"):
        text = ini.read_text()
        ini.write_text(text.replace(str(source), str(user)))
    dolphin = user / "Config/Dolphin.ini"
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.optionxform = str
    settings.read(dolphin)
    settings["General"]["WiiSDCardPath"] = str(user / "Wii/sd.raw")
    settings["General"]["NANDRootPath"] = str(user / "Wii")
    settings["NetPlay"]["Nickname"] = args.nickname or f"UI96Trial-{args.profile}"
    with dolphin.open("w") as file:
        settings.write(file)
    (user.parent / "preparation.json").write_text(json.dumps({
        "policy": POLICY, "source_user": str(source), "user": str(user),
        "binary": str(binary), "saves": "copied current state; no certified-save reset",
        "content_verification": "skipped", "runtime_validation": "NOT RUN",
    }, indent=2) + "\n")
    print(f"Prepared separate profile: {user}; current saves and original receipts copied.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="xbox", help="new isolated profile name")
    parser.add_argument("--prepare", action="store_true", help="copy current profile; never launch")
    parser.add_argument("--source-user", type=Path,
                        default=ROOT / "planning/quickplay/phase9/ui96-manual/xbox/User")
    parser.add_argument("--nickname", help="nickname for the newly copied profile")
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD,
                        help=f"trial build directory (default: {DEFAULT_BUILD})")
    parser.add_argument("--wiiu-adapter", action="store_true",
                        help="override port 1 = GameCube Adapter for Wii U (adapter port 1) for "
                             "this launch; by default port 1 is the profile's saved Controllers "
                             "setting, as in a normal launch. Port 2 stays standard")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="bounded declaration/existence checks only")
    mode.add_argument("--show-command", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", args.profile):
        parser.error("--profile must be a simple name, without path separators")
    if args.prepare and (args.check or args.show_command):
        parser.error("--prepare is separate from --check/--show-command")
    if args.prepare and args.wiiu_adapter:
        parser.error("--wiiu-adapter is a launch option; --prepare never launches")
    user = TRIAL_ROOT / args.profile / "User"
    build = args.build.expanduser().resolve()
    binary = candidate(build)
    if args.prepare:
        prepare(args, user, binary)
        return
    metadata = json.loads((user.parent / "preparation.json").read_text())
    if metadata.get("policy") != POLICY or metadata.get("user") != str(user):
        raise SystemExit("Profile was not prepared for this trial.")
    manifest(user)
    launcher = user / "Launcher/Project+ Offline Launcher.dol"
    for path in (launcher, user / "Launcher/Netplay Launcher.dol",
                 user / "QuickPlay/common-css.sav", user / "Wii/sd.raw"):
        if not path.is_file():
            raise SystemExit(f"Missing required trial file: {path}")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PPLUS_")}
    environment.pop("DOLPHIN_EMU_USERPATH", None)
    environment["QT_QPA_PLATFORM"] = "xcb"
    # Skip the startup GitHub update popup, which blocks the boot until dismissed.
    environment["QUICKPLAY_DISABLE_UPDATE_CHECK"] = "1"
    environment["LD_LIBRARY_PATH"] = str(ROOT / "build-tools/qt/6.7.3/gcc_64/lib")
    arguments = [str(binary), "-u", str(user), "-e", str(launcher)]
    # Port 1 is the profile's saved device, like a normal launch (persistent
    # admission accepts Standard Controller or Wii U adapter there). --wiiu-adapter
    # overrides it for this launch only; saved controller settings are not modified.
    port1 = WIIU_ADAPTER if args.wiiu_adapter else saved_port1(user)
    if port1 not in (STANDARD_CONTROLLER, WIIU_ADAPTER):
        raise SystemExit(f"Port 1 SIDevice0={port1} is unsupported; set Standard Controller or "
                         "GameCube Adapter for Wii U in Controllers, or pass --wiiu-adapter.")
    overrides = [f"Dolphin.Core.SIDevice0={WIIU_ADAPTER}"] if args.wiiu_adapter else []
    # Retain the Step2b manual launcher route and effective graphics/input choices.
    for setting in (
        "Dolphin.Core.CPUThread=False", "Dolphin.Core.GFXBackend=OGL",
        *overrides, f"Dolphin.Core.SIDevice1={STANDARD_CONTROLLER}",
        "Dolphin.Core.SIDevice2=0", "Dolphin.Core.SIDevice3=0",
        "Dolphin.Core.BootToPause=False", "Dolphin.Interface.DebugModeEnabled=False",
        "Dolphin.Input.BackgroundInput=True", "Dolphin.Display.RenderToMain=False",
        "Dolphin.Display.Fullscreen=False", "Logger.Options.WriteToFile=True",
        "Logger.Options.WriteToConsole=True", "Logger.Options.Verbosity=4",
        "Logger.Logs.NETPLAY=True",
    ):
        arguments.extend(("-C", setting))
    if args.check:
        print(f"Declaration/existence checks complete; content verification skipped; policy {POLICY}")
        source = "--wiiu-adapter override" if args.wiiu_adapter else "profile's saved setting"
        name = "GameCube Adapter for Wii U" if port1 == WIIU_ADAPTER else "Standard Controller"
        print(f"Candidate: {binary}; profile: {user}; runtime/build tests not asserted.")
        print(f"Port 1 SIDevice0={port1} ({name}, {source}); ports 2-4 forced "
              f"{STANDARD_CONTROLLER}/0/0.")
    elif args.show_command:
        print(shlex.join(arguments))
    else:
        os.execve(str(binary), arguments, environment)


if __name__ == "__main__":
    main()
