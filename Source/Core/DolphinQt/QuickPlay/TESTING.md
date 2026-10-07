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

Before starting Quick Play, use **gear button beside Quick Play**. Opening
Settings must send no HTTP request and create no matchmaking/NetPlay state. Edit
coordinator and region, Save, then click the main Quick Play area. The next attempt
uses current config without restarting; a running attempt retains its snapshot.
Defaults are `http://127.0.0.1:3000` and United States — East (`us-east`).
Loopback reaches only your computer. See [all 12 labels/values](README.md#configuration).

Legacy mappings are `na-east` → `us-east`, `na-central` → `us-midwest`,
`na-west` → `us-west`, and `eu` → `europe`. Known values and aliases are
case/whitespace normalized when Settings loads and before queue admission, even
if Settings has never been opened. Opening/Cancel does not rewrite config; Save
persists the canonical selection. Unknown custom values retain their exact spelling
and case. The canonical wire/config spelling is `caribbean`, labeled Caribbean.
Because Rust still compares exact strings, older clients sending legacy values
will not pair with clients sending the new canonical values. Use matching updated
clients; no server aliases or matching changes are introduced.

Unknown valid custom values appear as **Custom (preserved)** and survive Save
exactly. Invalid custom values require choosing a listed region.
Both players need the same endpoint, canonical (or exact custom) region, protocol,
and committed build revision, plus compatible Project+ game data. Platform does
not split Windows/Linux queues.

URLs may include a port/path prefix, but not credentials, queries or fragments.
Whitespace around the URL and trailing slashes are trimmed. Redirects are not
followed. Use HTTPS for remote private testing. **Test Connection to Coordinator Server** checks unsaved
URL text with GET `/health`, expects HTTP 200 and `{"status":"ok"}`, and reports
success, HTTP/schema failure, or unreachable/timeout (five seconds). It never
queues or saves. Settings Cancel discards edits.

## Phase 7 settings acceptance

Record the source SHA and results. Rebuild Windows from the same new Dolphin SHA
before declaring cross-platform Phase 7 acceptance; dirty builds sharing an old
SHA do not establish matching artifacts.

- [ ] Launch Dolphin without starting Quick Play. Click the toolbar gear to open Quick Play
      Settings; current coordinator and region load correctly. Confirm zero HTTP
      requests, queue tickets, attempt state, and NetPlay host/client creation.
- [ ] Change coordinator and region, Save, then use the main Quick Play control.
      Confirm immediate admission uses those values. Reopen Settings to verify them.
- [ ] Settings opens with one click; Quick Play is disabled until Save, Cancel,
      Escape, or window close. Invalid Save keeps Quick Play disabled.
- [ ] Main Quick Play disables the gear immediately. It stays disabled through
      searching, connection states, and errors until the attempt is dismissed.
- [ ] Searching has no Settings button and shows the correct friendly region label.
      A custom region displays literally; config changes do not alter the snapshot.
- [ ] Change coordinator, Save, cancel/reopen Quick Play without restarting.
      Observe the new coordinator receiving the queue POST (use local test server
      instrumentation without retaining tickets/nonces); the old one gets no new
      admission. Any prior attempt cleanup still goes to its original endpoint.
- [ ] Change region, Save, cancel/reopen again without restarting. Verify the queue
      JSON contains the canonical selected value. Exercise all 12 selections and friendly labels.
- [ ] Load profiles with each legacy alias, including ` NA-EAST `; Settings selects
      the mapped label and Save writes the canonical value (`us-east` in this case).
      Starting before opening Settings must also send the mapped region. Unknown
      valid custom values survive coordinator-only Save.
      An invalid custom value gives guidance without silently changing config.
- [ ] Cancel Settings edits, reopen, and verify original saved values remain.
      Restart Dolphin once to verify persistence (restart is not needed to apply).
- [ ] Test a working unsaved URL: Connected, only GET `/health`, no queue ticket,
      no config or active-attempt change. A path-prefixed endpoint is preserved.
- [ ] Empty URL, missing/unsupported scheme, malformed host/port, credentials,
      query and fragment are rejected with useful feedback and no request.
- [ ] Unreachable endpoint, slow response beyond five seconds, HTTP error, and
      HTTP 200 with malformed JSON/wrong status produce useful failures.
- [ ] During a slow health request, move/close Settings; UI remains responsive.
      Reopen or edit the URL before completion; no stale success message appears.
      Close Dolphin during the request; no crash or persistent hang.
- [ ] Cancel an attempt, open Settings, Save new coordinator/region, and start again.
      The new attempt uses those values without restarting.
- [ ] Run the manual Host/Join, browser/privacy, cancellation and two-client
      cross-network checks below with matching Windows/Linux Phase 7 artifacts.

### Direct settings and region display checkpoint — 2026-10-07

The toolbar uses an adjacent gear action without a dropdown menu. Its caption
row is blank so the icon aligns with Quick Play. The separator is before Quick
Play, between Netplay and Quick Play. A system settings icon is used when available,
with Dolphin's config icon as fallback. The gear is drawn at 24px inside its
32px canvas, lowered by 2px, in a 36px-wide button. Its solid strokes use the
strongest solid accent sampled directly from Quickplay, with icon shading mapped
to transparency instead of darkening that color. The toolbar label is Quickplay. Ten pixels of extra space separate the pair from the Config group. Settings and Quick Play are mutually exclusive. The searching
dialog shows the attempt's region and no longer offers Settings. The settings
help paragraph about future attempts was removed. `caribbean` is canonical;
only the four documented North America/Europe aliases are migrated.

The requested incremental Linux build passed (`cmake --build
/home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline --parallel 4`), without
fresh configuration. A temporary offscreen harness using production MainWindow
and an isolated profile passed one-click settings access, mutual disabling,
invalid Save, successful Save, Cancel, window close, and error dismissal recovery.
It verified no Settings child/button in the searching dialog and a friendly
region label that remains unchanged after external in-memory config changes.
The loopback fixture saw one intended admission with `caribbean`, polling, and
cancellation; Settings generated no requests. Saved coordinator/region were
verified on disk. Unrelated startup prompts were dismissed by the harness.

The settings/HTTP regression harness also passed all 12 exact canonical values
and friendly labels, four legacy mappings, unknown custom preservation,
Save/Cancel, effective/base config writes, health/schema/error/timeout checks,
responsive closing, stale callback suppression, and attempt snapshot behavior.
Formatting and diff whitespace checks passed with Windows CRLF accounted for.
A case-insensitive source/planning search found no remaining spelling variants.

Toolbar polish verification: the incremental Linux build and existing production
window smoke test passed again. A rendered offscreen toolbar capture confirmed
the separator between Netplay and Quick Play, the smaller color-matched gear,
extra space before Config, and aligned icon rows. Settings labels now read Coordinator Server and Test Connection to
Coordinator Server; the searching introduction uses Coordinator Server and
joins its final clauses with "and".

Manual desktop visual/keyboard checks, persistence across a real relaunch, manual
NetPlay Host/Join, actual traversal, and Windows/Linux cross-network checks remain
pending. Offscreen tests do not establish those results.

### Initial local implementation checkpoint — 2026-10-07 (before corrections)

The existing Linux baseline build completed with `cmake --build
/home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline --parallel 4` after the
Phase 7 edits. No fresh configure was used. CMake and the Windows project register
the new settings dialog; Windows has not yet been rebuilt for this change.

A temporary offscreen Qt harness linked against the production objects passed
URL validation, all four region selections, known legacy normalization, custom
preservation/invalid-custom guidance, Save/Cancel, and effective/base config writes
(including command-line overrides). Its in-memory config loader verified Save
receives the persistent values; on-disk persistence remains a manual check above.
Loopback HTTP fixtures verified unsaved/path-prefixed health requests, JSON/status
validation, HTTP errors, unreachable/timeout handling, event-loop responsiveness,
close/reopen with stale-result suppression, and destruction during a request.
Observed admissions were A/`na-east` then B/`na-west` without process restart;
the old attempt still polled and cancelled on A. Health checks created no tickets.
Formatting and diff whitespace checks passed. These checks do not establish
real traversal, manual NetPlay, or Windows/Linux cross-network acceptance.

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

Deferred: Linux Launcher Path investigation; automatic buffer;
automatic first-game Start; selectable/copyable status/error text; error wording;
minor clipping/rendering polish; menu/hotkey entry points; later in-game Project+
menu bridge. Phase 8 has not begun.

For failures record the stage, role, revision and topology. NETPLAY Info logs
include redacted connection stages. Existing normal NetPlay logs may contain
player names and chat; review before sharing. Do not post tickets, Authorization
headers, full match IDs, traversal codes, nonces, or coordinator credentials.

### Phase 7 acceptance fixes — lobby handoff and gear

The settings gear is now drawn consistently on Windows and Linux, independent of
system icon themes. Successful client join and the host lobby receiving its remote
player automatically close Quick Play and restore settings access without closing
the normal lobby. Host completion accepts a peer arriving before the publication
HTTP response; stale attempt notifications are ignored. Errors remain visible.

Recheck both host directions from the new same-SHA builds: the host must keep
Quick Play open while alone, then both search dialogs close after connection,
with both players still connected in the normal lobby. Check the gear on both OSes.
Cross-platform runtime acceptance remains pending.

Local validation: incremental Linux build passed. An offscreen Qt harness linked
against production controller/dialog objects passed host completion both before
and after publication response, stale-attempt rejection, automatic client close,
failed-join error visibility, cancellation, timer shutdown, and zero host teardown
callbacks on successful handoff. This does not establish real cross-network or
Windows runtime acceptance.
