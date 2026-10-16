# Quick Play matchmaking and ordinary NetPlay handoff

Adds Quick Play settings, search UI and lobby/coordinator matchmaking. Lobby searchers exchange short-lived listings, agree on a peer and host role, then hand off to ordinary NetPlay. Temporary listing loss retries the same identity/agreement within the existing search and room-setup deadlines; cancellation attempts bounded cleanup of the listing. Index worker cleanup also handles rejected keepalives safely, and directory queries use separate error storage from listing maintenance. Quick Play index operations have three-second total transfer deadlines; polls and keepalives observe cancellation. Admission keeps its bounded response window to recover a secret for cleanup. Shutdown has a conservative nine-second worker drain budget plus scheduling with asynchronous DNS. If an admission secret never arrives or removal cannot reach the index, remote expiry remains necessary.

Lobby compatibility requires an identical full Git revision in the `+ppqp2` namespace. Older display-only Quick Play listings are intentionally isolated. Ordinary NetPlay's exact-revision handshake and the coordinator's exact-revision rule remain unchanged. Platform display metadata does not determine compatibility.

The Dolphin lobby is the default backend. A valid optional coordinator URL enables pre-match fallback; saving an empty URL clears persisted and effective settings and disables fallback. Custom-coordinator mode requires a valid nonempty URL. Rooms automatically assign GameCube ports while Wii Remote assignment remains manual, restoring Project+ Dolphin's earlier default for both ordinary and Quick Play rooms.

Validation uses source-built tests with private loopback HTTP/ENet fixtures, isolated configuration, bounded process exits, generated build identities, and the real settings dialog offscreen. The original regressions and the streaming shutdown and overlapping-error diagnostic fixtures fail before their fixes and pass afterward. The concurrent error access was established by source inspection; a targeted, partially instrumented ThreadSanitizer run of the corrected overlap paths reports no race. Linux application and headless test builds pass. The existing `PatchAllowlist.VerifyHashes` failure is tracked separately in `PR23-review-results.md`.

Windows builds/runtime tests and human GUI, traversal and gameplay checks remain pending. Loopback matching and protocol tests do not establish gameplay or Linux–Windows traversal compatibility.

This is a local description draft; it has not been posted to GitHub.
