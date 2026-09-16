# Quick Play coordinator client (Phase 6)

Quick Play queues and pairs through Rust. The assigned host locates Netplay Launcher,
creates an unlisted normal NetPlay room, obtains its traversal code internally, and
publishes it. The assigned client polls for the code and automatically joins the
normal NetPlay lobby. Buffer selection and game start remain manual.

## Configuration

In the active **Dolphin.ini**, add to the existing `[NetPlay]` section:

```ini
[NetPlay]
QuickPlayCoordinator = http://127.0.0.1:3000
QuickPlayRegion = na-east
```

These are the defaults, defined once in `Core/Config/NetplaySettings.cpp`. Edit
while Dolphin is closed, then restart. Normal Linux configuration is
`~/.config/project-plus-dolphin/Dolphin.ini`; with `--user <directory>`, use
`<directory>/Config/Dolphin.ini`. No settings UI is added in this phase.

Temporary command-line overrides are also supported by Dolphin's existing loader:

```sh
project-plus-dolphin -C Main.NetPlay.QuickPlayCoordinator=http://192.168.1.20:3000 -C Main.NetPlay.QuickPlayRegion=na-east
```

The client snapshots settings at each attempt. It accepts absolute HTTP/HTTPS
URLs, including an optional path prefix, strips trailing slashes, and appends
`/v1/queue`. Credentials, queries, fragments, empty hosts and other schemes are
rejected. Redirects are not followed. HTTPS uses the existing curl TLS behavior.
Region must be 1–32 printable ASCII characters without whitespace; comparison is
case-sensitive, with no geographic inference or fallback.

## Exact fingerprint

| Field | Value |
| --- | --- |
| `protocol` | Integer `1`, the Quick Play API/compatibility protocol |
| `build` | Exact `Common::GetScmRevGitStr()` (`SCM_REV_STR`, generated from `git rev-parse HEAD`) |
| `pplus_version` | Exact `Common::GetScmDescStr()` (`SCM_DESC_STR`), informational Dolphin/P+ build description |
| `region` | `QuickPlayRegion`, default `na-east` |
| `platform` | `windows`, `linux`, `macos`, or `other` |
| `player_nonce` | New `QUuid::createUuid()` per attempt, without braces |

For the current checkout, `build` is
`42f1332e2a590fdc5588c31214ecdfa9c688431a`. The current Linux generated description
is `v3.2.0-2606a-dirty`. CMake derives the description from
`git describe --always --long --dirty`, removing the hash/zero-count suffix;
the Windows generator omits `--dirty`. Send each generator's exact description,
not a hard-coded `v3.2.0`. It is informational and is not an assertion about the
installed game files. Only protocol/build/region determine pairing.

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

## Manual Phase 6 build/runtime checkpoint

Run the incremental build manually, using the existing cache:

```sh
cmake --build /home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline --parallel 4
```

1. Start Rust directly in a terminal:

   ```sh
   cd ~/Projects/pplus/pplus-matchmaker
   cargo run --locked
   ```

2. Launch two instances of the same newly built executable, each in its own terminal:

   ```sh
   /home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline/Binaries/project-plus-dolphin --user /tmp/pplus-quickplay-a -C Main.NetPlay.QuickPlayCoordinator=http://127.0.0.1:3000
   ```

   ```sh
   /home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline/Binaries/project-plus-dolphin --user /tmp/pplus-quickplay-b -C Main.NetPlay.QuickPlayCoordinator=http://127.0.0.1:3000
   ```

3. In both profiles set Config -> Paths -> Launcher Path to
   `/home/joemarchy/Projects/pplus/Plus-Dolphin/Data/user/Launcher/`; restart/refresh
   until Netplay Launcher appears. Use equal regions (default na-east). Enable
   NETPLAY Info logging. Internet traversal access is required even with local Rust.

4. Click Quick Play on A, then B. A hosts automatically and reaches Room ready.
   B waits for host, shows Connecting then Connected, and opens the same normal
   lobby. Verify both player names in both lobby lists. Copy no code and click no
   manual Join. Manual Start is expected; no automatic buffer/start is implemented.

5. Dismiss Quick Play and quit both normal lobbies. Repeat with B queued first:
   B must host and A must join. Before a further pair, enable Show in server browser
   in manual hosting with a recognizable room name/valid index region, and verify
   that manual room is listed. Quit it, pair through Quick Play with that checkbox
   still enabled, and verify the Quick Play room is absent from the public browser.
   After quitting, manual hosting must retain its previous index preference.

6. For a longer host wait, quit both sessions and relaunch the intended host with
   the additional override `-C Main.NetPlay.TraversalServer=192.0.2.1`. Queue it
   first, then the normal client. Cancel the client while Waiting for host; leave
   it open beyond 30 seconds and verify no delayed join. Repeat Cancel/reopen,
   Escape and window-close, then restore the host's normal traversal configuration
   and confirm a new pair succeeds. Also click Cancel as Connecting appears:
   if the synchronous join already opened a lobby, that lobby must remain usable.

7. Repeat the delayed-host setup, but stop Rust with Ctrl-C while the client waits.
   Expect controlled Error within the current request deadline, no retry loop or
   later join. Restart Rust and pair again. To exercise HTTP 410 directly, restart
   Rust while both clients are waiting for host setup: if the next GET reaches the
   restarted server it sees expired (a GET during downtime instead sees transport
   Error). Closing/cancelling the host before join must similarly produce expiry
   or the ordinary connection failure. Remove the traversal override afterward.

8. Verify ordinary manual Host, Join by traversal code, and joining through Server
   Browser still work; Quick Play must refuse to replace an existing session.
   Close/reopen Quick Play rapidly and quit Dolphin during waiting/connection;
   verify no stale join, crash or persistent hang. Windows build/moc and the same
   two-machine test remain pending. Malformed/missing/overlong/non-alphanumeric
   ready payloads require a separate controlled HTTP fixture for runtime testing;
   source checks alone do not establish those runtime results.

For two machines, start Rust with `MATCHMAKER_BIND=0.0.0.0:3000 cargo run --locked`
and point both same-build clients at `http://<coordinator-LAN-IP>:3000`.

The Phase 5 scratch proxy did not print VERIFIED in the user's test, although the
host reached its post-publication state. That helper is deferred test-infrastructure
cleanup; Phase 6 neither uses nor modifies it. Successful automatic join against
Rust directly is the definitive code-delivery acceptance test.

**Stop after Phase 6 at the manual build/runtime checkpoint.**
