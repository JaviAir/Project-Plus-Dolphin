# Quick Play MVP tester guide

Use the supplied custom Project+ Dolphin build the same way you use current
Project+ Dolphin. No source build, Rust installation, or scratch proxy is required
on a tester's computer. The organizer runs the coordinator and supplies its URL,
region, and a matching Windows/Linux artifact pair. Buffer and game Start remain
manual once both players reach the normal NetPlay lobby.

## Install and launch

**Windows:** Extract the entire custom Windows archive into its own directory,
keeping the executable, DLLs, resources and supplied folder layout together.
Launch its Dolphin executable, not your existing shortcut to another build.
Reuse your normal Project+ user/game data and controller setup. Back up user data
before sharing it between builds; do not run two builds against the same user
folder simultaneously. For a portable setup, copy your existing User folder into
the custom portable installation. Alternatively launch with `--user "<user-folder>"`
to select an existing user folder explicitly. Keep your usual game/SD/launcher
paths; the custom emulator does not replace the Project+ game installation.

**Linux:** Use the supplied custom AppImage (`chmod +x <artifact>.AppImage`, then
run it) or the organizer's complete runnable local bundle. Keep its resources and
libraries together. Launch the custom P+ Dolphin and reuse your normal Project+
user/game data as above. `--user <user-folder>` selects an explicit profile;
portable Linux builds use `user/` beside the executable. An AppImage's user data
should stay in a writable external location, not inside its read-only image.

On both platforms confirm **Netplay Launcher** is visible before queueing. Linux
has a pre-existing Launcher Path discovery issue, independent of Quick Play;
Windows behaves better. If needed, set **Config → Paths → Launcher Path** to your
normal Project+ Launcher directory containing `Netplay Launcher.dol`, then
refresh/restart. Quick Play inherits this issue; investigation is deferred.

## Coordinator and region

Close Dolphin before editing its active `Dolphin.ini`, then restart. Add these
keys to the existing `[NetPlay]` section (do not create duplicate sections):

```ini
[NetPlay]
QuickPlayCoordinator = https://example-coordinator
QuickPlayRegion = na-east
```

Replace the example with the organizer's actual reachable endpoint. Defaults are
`http://127.0.0.1:3000` and `na-east`; loopback only reaches your own computer.

- Windows: normally `Config\Dolphin.ini` within the active user folder. Portable
  builds use `User\Config\Dolphin.ini` beside the executable; otherwise Dolphin's
  registry override, existing Documents\Dolphin Emulator, or
  `%APPDATA%\Dolphin Emulator` determines the user folder.
- Linux: normally `~/.config/project-plus-dolphin/Dolphin.ini` (or the corresponding
  `XDG_CONFIG_HOME` path). Portable mode, `DOLPHIN_EMU_USERPATH`, or a legacy
  `~/.project-plus-dolphin` profile can override the default.
- Explicit `--user <directory>`: `<directory>/Config/Dolphin.ini` on either OS.

Names are verified in `Core/Config/NetplaySettings.cpp`. URLs must be absolute
HTTP or HTTPS with a host and optional port/path prefix. Do not include credentials,
query strings, or fragments. Trailing slashes are removed before API paths are
appended. Redirects are not followed. Use HTTPS for remote private testing.

Region must be 1–32 printable ASCII characters without whitespace. There is no
region dropdown, canonicalization, or automatic geographic selection today:
`na-east` and `NA-EAST` form different queues. Both players need the same endpoint,
exact region, protocol, and committed build revision, plus compatible Project+
game data. The platform label does not split Windows/Linux queues.

The controller reads in-memory config at each new attempt, but external INI edits
are not a live settings workflow: edit while closed and restart. Manual editing is
temporary development UX. **Phase 7 — Quick Play Settings QOL** will provide URL
UI, region selection/canonicalization, live settings, and Test Connection.

## Compact regression record

Core cross-platform/cross-network flow, manual NetPlay regression, and unlisted
Quick Play hosting are maintainer-confirmed for the existing MVP. The boxes below
are a fresh artifact-pair checklist, not claims of a new test run. Record date,
Dolphin SHA, coordinator SHA, artifact checksums, OS, topology and result.

- [ ] Windows host → Linux client, on separate physical internet connections.
- [ ] Linux host → Windows client, on separate physical internet connections.
- [ ] Queue host first, then client: automatic pairing, host creation and client
  join; no code copying and no manual Join click. Both players appear in the same
  normal Project+ NetPlay lobby. Quit the lobbies and reverse queue order.
- [ ] Manual Host and manual Join work.
- [ ] Public Server Browser works; manual Show in server browser preference works
  both enabled and disabled.
- [ ] Keep that manual preference enabled: Quick Play host remains unlisted;
  subsequent manual hosting retains the preference.
- [ ] Cancel while searching, then start a fresh attempt.
- [ ] Unavailable coordinator gives a recoverable error; restore it and retry.
- [ ] Close Dolphin during an active Quick Play request: no crash/persistent hang.
- [ ] Cancel/reopen promptly: late callbacks do not change the newer attempt.

The older queued player hosts. Queue order must be deliberate when recording role
coverage. Existing synchronous NetPlay setup can briefly pause UI/cancellation.

## Known limitations and reporting

Manual NetPlay also fails in the observed same-machine/same-public-IP traversal
topology. This is pre-existing Dolphin behavior, not a Quick Play regression or an
MVP blocker; realistic cross-network Quick Play succeeds. Use separate physical
networks for acceptance. The coordinator connection is HTTP only; gameplay still
uses Dolphin traversal/P2P and is not relayed through the coordinator/Tailscale Serve.

Deferred: Linux Launcher Path investigation; Phase 7 settings; automatic buffer;
automatic first-game Start; selectable/copyable status/error text; error wording;
minor clipping/rendering polish; menu/hotkey entry points; later in-game Project+
menu bridge. No new Quick Play feature is included in this stabilization gate.

For failures record the stage, role, revision and topology. NETPLAY Info logs
include redacted connection stages. Existing normal NetPlay logs may contain
player names and chat; review before sharing. Do not post tickets, Authorization
headers, full match IDs, traversal codes, nonces, or coordinator credentials.
