# Quick Play matchmaking client

Quick Play pairs players through the Dolphin lobby or a Rust coordinator
(see [Matchmaking backends](#matchmaking-backends)). The assigned host locates Netplay Launcher,
creates an unlisted normal NetPlay room, obtains its traversal code internally, and
publishes it. The assigned client polls for the code and automatically joins the
normal NetPlay lobby. Buffer selection and game start remain manual.

Core MVP is complete; cross-platform/cross-network and manual NetPlay validation
were reported by the maintainer. See [tester instructions](TESTING.md) and
[release-prep audit](RELEASE_PREP.md). Phase 7 settings are implemented; new
Windows/Linux runtime acceptance is still pending.

## Matchmaking backends

**Matchmaking** in Quick Play Settings (`Main.NetPlay.QuickPlayMatchmaking`) picks
how players find each other. Both use the same traversal server
(`NetPlay/TraversalServer`), which only introduces the peers and never relays
gameplay, and the same host/join, digest and settings checks.

- `lobby` — **Dolphin lobby (default)**: no server of our own. Players meet
  through Dolphin's NetPlay index (`NetPlay/IndexServer`, the server browser's
  backend), `QuickPlayIndexMatchmaker`.
- `coordinator` — **Custom coordinator**: the Rust `pplus-matchmaker`
  (`QuickPlayClient`), with a fair queue and exact region matching.

The default index is the public `lobby.dolphin-emu.org`. For development, run a
local [netplay-index](https://github.com/dolphin-emu/netplay-index)
(`uv run netplay-index`, `GEOIP_DATABASE_PATH=testdata/GeoLite2-Country.mmdb`) and
`-C Dolphin.NetPlay.IndexServer=http://127.0.0.1:8000`.

If the index cannot be reached and a coordinator other than
the built-in `http://127.0.0.1:3000` placeholder is set, the search continues
through that coordinator. Otherwise it fails with an error.

### Lobby handshake

The index has no matchmaking, locks or messages, only listings that live while
their keepalive runs (every 5 s, 15 s timeout). Each searcher keeps at most one
listing. The handshake is in its name, after the readable prefix
`P+ Quick Play · PPQP1`:

| Listing | Tokens | Meaning |
| --- | --- | --- |
| `W` | region mask, preference, digest8, nonce | Waiting for an opponent |
| `C` | region mask, preference, digest8, waiter, nonce | Claims that waiter |
| `A` | host (`w`/`c`), digest8, waiter, claimer | Waiter accepted that claim |
| `H` | digest8, waiter, claimer | Host ready; `server_id` is its traversal code |

- **Search.** List `name=PPQP1&password=1&version=<exact build>`. Claim a random
  compatible `W`: same 8-character origin-digest prefix, shared region bit, not
  blocked. If there is none, list as `W`: at once with Prefer to host, after
  0–2 s and one more list with No preference, after 15 s of searching with Prefer
  to join. The Prefer to join phases are shown live (`SearchStage`): the overlay
  reads *Looking for a host in … N s*, then *Searching in …, now listed…*. The
  searching dialog explains each phase on its own line.
- **Waiting** (lists every 3–5 s). Accept the claim with the smallest nonce: replace
  `W` with `A`, which names the host by the coordinator's soft rule (the claimer
  hosts when the waiter prefers to join and the claimer does not, or the waiter has
  no preference and the claimer prefers to host). A waiter that sees a waiting
  player with a smaller nonce claims it instead, so two waiters converge.
- **Claiming** (lists every 2 s). An `A` or `H` for this pair is a match. An `A` for
  another claimer, the waiter's listing gone on two lists, or 12 s without an
  answer blocks that waiter for 60 s and searches again.
- **Matched.** Only now do both players leave their local game. After the handoff
  each checks that the other's listing is still there (three lists over ~2 s). The
  host replaces its listing with `H`; its `server_id` is the traversal code
  obfuscated with `NetPlaySession::EncryptID`, keyed by `PPQP1:<origin digest>`.
  That is not secrecy (the derivation is open source); it keeps stock browsers out
  and checks the digest once more. The guest decrypts it and joins as before.
- Every listing is removed on connect, cancel, error or the 180 s timeout.
- A Quick Play host refuses any second guest with NetPlay's "server is full".

Regions are an 11-bit mask of the listed regions (custom regions are ignored by the
lobby; at least one listed region is required). The agreed region is the first
shared one in checklist order. The index's own region field gets the first
region's continent (`NA`, `SA`, `EU`, `EA`, `OC`). `W` is listed as not in game
and the handshake listings as in game, so ordinary browsers can hide them. All are
password-locked.

Listings whose last keepalive is more than 15 s older than the newest listing have
timed out by the index's own rule, but stay listed until a later cleanup (up to
~45 s). They are skipped, so a crashed waiter costs searchers little.

The index allows 5 sessions per public IP. When it refuses one
(`TOO_MANY_SESSIONS`), the search shows *Your network already has the most Dolphin
lobby rooms allowed. Waiting for a free slot.* and retries every 3 s.

## Configuration

Click the **gear button beside Quick Play** to open Quick Play Settings directly.
A toolbar separator groups Quick Play and its settings button apart from Netplay.
The compact settings gear uses the Quickplay icon's solid accent color and the
same drawn shape on every platform. Extra space before Config groups the pair
together. The main button still starts searching immediately. Opening Settings
creates no controller, attempt, HTTP request, queue ticket, NetPlay host, or client.

Quick Play is disabled while Settings is open; Save, Cancel, Escape, or closing
Settings enables it again. Settings is disabled immediately when Quick Play is
clicked and remains disabled until the attempt is dismissed, including errors.
Successful lobby handoff closes the searching dialog automatically on both host
and client, restoring settings access without closing the connected lobby.
The searching dialog has no Settings button. Below the status it shows two
columns from the attempt's snapshot: **Region:** or **Regions:** with one friendly
label per line (custom values are shown exactly), and **Host preference:**. Once
matched the first column shows the agreed region.

Save applies without restarting Dolphin or editing INI files. Cancel discards
unsaved edits. Settings reloads current config whenever reopened.

Settings use `Main.NetPlay.QuickPlayMatchmaking` (`lobby` or `coordinator`),
`Main.NetPlay.QuickPlayCoordinator`, `Main.NetPlay.QuickPlayRegions`
(comma-separated tokens) and `Main.NetPlay.QuickPlayHostPreference` (`host`,
`none` or `join`) from `Core/Config/NetplaySettings.cpp`. Defaults are `lobby`,
`http://127.0.0.1:3000`, `us-east` and `none`. The coordinator field, its
**Show** button and **Test Connection** are enabled only for Custom coordinator;
with the lobby, an invalid coordinator entry is left unsaved. While `QuickPlayRegions` is empty,
the legacy single `QuickPlayRegion` value is read instead; Save writes
`QuickPlayRegions` and removes the legacy key from the base config. Save updates effective config
and persists the base config via Dolphin's config system. If an initial value came
from a temporary `-C Dolphin.NetPlay.QuickPlayCoordinator=...` or region override,
Save explicitly replaces it for this run and persists the chosen value. Supplying
that command-line override again on a later launch still overrides the saved value.

| Display label | Saved/wire value |
| --- | --- |
| United States — East | `us-east` |
| United States — South | `us-south` |
| United States — Midwest | `us-midwest` |
| United States — West | `us-west` |
| Canada | `canada` |
| Mexico / Central America | `mexico-central-america` |
| Caribbean | `caribbean` |
| South America | `south-america` |
| Europe | `europe` |
| Asia | `asia` |
| Australia | `australia` |

Legacy mappings are `na-east` → `us-east`, `na-central` → `us-midwest`,
`na-west` → `us-west`, and `eu` → `europe`. Known values and aliases are
case/whitespace normalized when Settings loads and before queue admission, even
if Settings has never been opened. Opening/Cancel does not rewrite config; Save
persists the canonical selection. Invalid and duplicate tokens are dropped, and at
most 12 are kept. The retired `us-nationwide` region is dropped; if nothing is left,
the default `us-east` is used. Unknown custom values retain their exact spelling
and case. The canonical wire/config spelling is `caribbean`, labeled Caribbean.
Because Rust still compares exact strings, older clients sending legacy values
will not pair with clients sending the new canonical values. Use matching updated
clients; no server aliases or matching changes are introduced.

Regions are a checklist; at least one must be ticked. A saved unknown region
appears as an extra checked **Custom (preserved): value** entry and keeps its exact
spelling while ticked. Valid custom values are 1–32 printable ASCII characters
without whitespace. Two players pair when their region lists share at least one
exact token; there is no geographic inference, widening over time, or server-side
case folding.

**Host Preference** (`Prefer to host` / `No preference` / `Prefer to join`) is soft.
The coordinator honours it when the two players complement each other, otherwise
the player who queued first hosts. It never prevents or delays a match. The lobby
applies the same rule between waiter and claimer, and also uses it to decide how
long to search before listing as waiting.

Searching text names the backend: `Searching in United States — East (Dolphin
lobby)…`, `Searching in 2 regions (custom coordinator)…`.

The coordinator field is masked like a password, since a private coordinator URL
is effectively an access secret. **Show** / **Hide** beside it toggles the mask;
reopening Settings masks it again.

URLs require absolute HTTP/HTTPS with a host and optional port/path prefix.
Surrounding whitespace and trailing slashes are removed; meaningful paths are
preserved. Credentials, queries, fragments, invalid ports and malformed/empty
URLs are rejected with inline feedback. API requests append paths to the base URL.
Redirects are not followed. HTTPS uses the existing curl TLS behavior.

**Test Connection to Coordinator Server** checks the currently entered URL, including unsaved edits,
with `GET <base>/health`. HTTP 200 with a JSON object containing `"status":"ok"`
shows **Connected**; transport/timeout, HTTP status, and invalid-response errors
are reported separately. This verifies health only, not build compatibility or
traversal. It neither saves settings nor creates a ticket or changes matchmaking
state. A separate `Common::AsyncWorkThread` runs `Common::HttpRequest` with a
five-second total timeout. The settings dialog is retained by its owning window;
closing it invalidates delivery without waiting. Editing the URL also invalidates
old results. Final owner destruction cancels and drains the bounded worker before
QObject teardown. DNS timeout bounds require an asynchronous curl resolver, as
with existing Quick Play requests.

The controller already reads current in-memory config at every Start. Each attempt
retains its own endpoint/region/preference snapshot, so subsequent config changes
cannot redirect polls, host publication, or cancellation to a different coordinator,
or change the displayed search regions.

## Exact fingerprint

| Field | Value |
| --- | --- |
| `protocol` | Integer `2`, the Quick Play API/compatibility protocol |
| `build` | Exact `Common::GetScmRevGitStr()` (`SCM_REV_STR`, generated from `git rev-parse HEAD`) |
| `pplus_version` | Exact `Common::GetScmDescStr()` (`SCM_DESC_STR`), informational Dolphin/P+ build description |
| `regions` | `QuickPlayRegions` as a JSON array, default `["us-east"]` |
| `host_preference` | `QuickPlayHostPreference`: `host`, `none` or `join` |
| `platform` | `windows`, `linux`, `macos`, or `other` |
| `player_nonce` | New `QUuid::createUuid()` per attempt, without braces |

The validated source baseline is `341e758e56c29a6c2268053ef3b0b60ca6de9cc9`.
Use the actual artifact's generated revision, not a hard-coded value. CMake derives
the informational description from `git describe --always --long --dirty`;
the Windows generator omits `--dirty`. Only protocol, build and a shared region
determine pairing.

The build key matches Dolphin's existing NetPlay revision handshake and has no
platform suffix. **Uncommitted source differences are not represented in this
key.** Test clients must use the same source changes/artifact; sharing HEAD alone
is not proof that dirty builds are compatible. Missing/whitespace-containing
build metadata fails local validation. The nonce is neither an account identity
nor an idempotency key.

## Hosting, requests, cancellation and lifetime

MainWindow injects private StartQuickPlayHost/CancelQuickPlayHost callbacks into
QuickPlayController. The start helper checks for an existing game/session, invokes
FindPPlusNetplayLauncher, and uses NetPlayHostInternal, also used by manual Host.
NetPlayJoinInternal connects the host's own loopback client and is also the shared
implementation behind normal NetPlayJoin and the private JoinQuickPlayHost bridge.
The client bridge checks for existing sessions, temporarily sets
NETPLAY_TRAVERSAL_CHOICE=traversal and NETPLAY_HOST_CODE to the validated code with
SetBaseOrCurrent, then restores both previous values on return. Traversal server,
traversal port, direct address and connect port remain unchanged. The traversal
client obtains the peer endpoint from rendezvous; the direct connect port is unused.
Manual Host/Join are guarded against reentry during Quick Play connection setup.

A nonzero immutable Quick Play attempt ID on NetPlayServer forces SetupIndex to
return for its full lifetime, including reconnects. This is a per-server override
of NETPLAY_USE_INDEX, not a temporary config toggle. Manual hosts use attempt 0
and still read the normal public-index preference. Retained rooms stay unlisted.

NetPlayServer copies GetHostID's fixed-size value on its traversal callback thread,
then NetPlayUI -> NetPlayDialog queues an owned QString + attempt ID via
QueueOnObject. HostTraversalChanged reaches the UI-thread controller. A snapshot
before the server service thread starts covers already-connected traversal.
Only connected/nonempty codes reach publication. Duplicate ready events do not
republish; a different code or failure ends the attempt. Quick Play traversal
errors use this generation-guarded path instead of the legacy modal error callback.

QuickPlayClient retains its existing serialized Common::HttpRequest worker and
queued result delivery. PublishHostCode POSTs {ticket, host_code} to
/v1/matches/{match_id}/host with sensitive logging. Empty HTTP 204 is success;
410 must contain state expired. Malformed/unexpected responses fail. Transport
failures, HTTP 408, and server errors get one identical-body retry; admission is
still never retried. Requests have a five-second total timeout; DELETE has three
seconds. Host setup has a separate 25-second deadline from observed assignment.
These local deadlines do not extend Rust's 30-second match lifetime.

Cancel invalidates the generation, timers, traversal events and queued HTTP
results, aborts publication when possible, and queues DELETE. The worker checks
cancellation before retry. An admission already in flight still finishes to
recover its ticket for cleanup. Missing/lost admission tickets and failed cleanup
fall back to coordinator TTL. Endpoint and ticket stay attached to the old attempt.
DNS bounds still require an asynchronous curl resolver, as documented in Phase 4.

Only the matching Quick Play server can be cleaned up. PrepareQuickPlayCancel
holds the normal OnConnect game mutex, checks that no remote player has ever
been admitted and no game is starting/running, and closes admission before
MainWindow hides the lobby and runs NetPlayQuit. Once a remote peer has joined,
Cancel leaves the normal lobby intact, even if that peer later disconnects.
Users then quit through normal NetPlay UI. The server's unlisted policy persists.
Cancel inside a modal creation failure invalidates callbacks immediately but
defers teardown until the synchronous host call returns. Starting another attempt
is refused until that call unwinds. MainWindow destroys orchestration before the
NetPlay dialog so injected callbacks cannot outlive their owner.

Hosting still includes the existing synchronous local NetPlayClient connection;
it can temporarily pause UI input (normally up to the existing five-second connect
wait plus handshake). Normal NetPlay teardown joins server/client threads; this
phase does not claim asynchronous teardown or alter NetPlay's thread model.

## Automatic client join

MatchedClient -> WaitingForHost -> Connecting -> NetPlayConnected. The host can
remain in WaitingForOpponent after the peer arrives; the normal lobby shows both
players. Connected is a success snapshot, not a new session-monitoring service.

PollMatch sends GET /v1/matches/{match_id} with Authorization: Bearer <client-ticket>.
The match ID is bound to the assigned client role and admission object; its ticket
stays on the serialized HTTP worker. First poll is immediate, subsequent polls
start one second after a waiting_for_host response. m_pending prevents overlap.
The client wait has a 25-second deadline, each request has a five-second timeout.
HTTP 410 expired, transport failures, unexpected HTTP/schema, and invalid/missing
codes end the attempt with Error. Match GET has no automatic retry on errors.
Ready accepts 1–8 ASCII alphanumeric characters (Dolphin's NETPLAY_CODE_SIZE limit,
with the coordinator's character restriction). Normal published codes are eight.
No full code or ticket is logged.

Accepting ready stops both timers and changes state before scheduling the join.
Generation, state, admission identity and match checks reject stale/duplicate
responses. Cancel/Escape/close aborts polling, invalidates the generation and
queues ticket deletion; reopening gets a fresh attempt. Start cannot reenter the
synchronous join call. The bridge uses the existing NetPlayClient construction,
IsConnected check, failure cleanup and normal lobby opening without duplicating it.
On failure Quick Play shows Error. Host departure fails through either coordinator
expiry or the existing NetPlay connection failure.

After successful join, one bounded DELETE clears both coordinator tickets/match;
there is no connected acknowledgement endpoint. Polling never resumes. The HTTP
client drops its active attempt/callback and the controller clears match/code.
The host's stable publication-success state needs no further HTTP. A later host
Cancel may issue its existing idempotent DELETE. Failed cleanup relies on Rust TTL.
Cancel never tears down the joined client lobby or an unrelated/manual session.

Connection setup retains the existing synchronous traversal wait (five seconds)
and handshake wait. UI input can pause until that call returns; queued Cancel is
then processed and leaves an already-open lobby intact. If cancellation runs in a
nested event loop during setup, the generation changes immediately and the result
is ignored when setup unwinds. This phase does not add asynchronous NetPlay setup.

## Verification and release preparation

Use [TESTING.md](TESTING.md) for installation and the compact regression checklist,
and [RELEASE_PREP.md](RELEASE_PREP.md) for maintainer build/artifact checks.
The historical Phase 5 proxy and `/tmp/phase5_probe.py` are unnecessary: clients
communicate directly with Rust or its reachable HTTPS endpoint. No scratch helper
is a runtime, build, testing, or packaging prerequisite. Use separate physical
networks for traversal acceptance; same-machine testing is not that acceptance test.
