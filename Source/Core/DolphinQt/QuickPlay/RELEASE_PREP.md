# MVP Stabilization / Release Prep — 2026-10-07

Unnumbered gate between completed Phase 6 and planned **Phase 7 — Quick Play
Settings QOL**. No feature, networking, or release-infrastructure changes made.
Review/documentation is complete; initial coordinator commit and final artifact
packaging/verification remain release handoff actions. No deployment was performed.

## Repository evidence

Dolphin entered the gate clean on `quickplay-mvp`, tracking `origin/quickplay-mvp`,
at `341e758e56c29a6c2268053ef3b0b60ca6de9cc9` (**Add Quick Play for phase six
testing**). This is the latest relevant Quick Play commit; its parent is
`42f1332e2a590fdc5588c31214ecdfa9c688431a`. Recursive submodules match their recorded
revisions. Remotes (fetch and push):

- origin: `https://github.com/JaviAir/Project-Plus-Dolphin.git`
- upstream: `https://github.com/Project-Plus-Development-Team/Project-Plus-Dolphin.git`

After this gate only this audit, TESTING.md, and the adjacent README are changed
or new. No history rewrite, commit, push, or remote modification was performed.
Recommended Dolphin commit: **Document Quick Play MVP testing and release prep**.

The companion Rust coordinator is an initialized Git repository on unborn `main`:
no HEAD SHA, no commits, no remotes (therefore no GitHub remote). All nine files
are untracked. `git diff` is empty because there is no tracked baseline; reviewed
the complete source, manifest/lockfile, tests and README as the initial change set.
Recommended single initial commit: **Add standalone Quick Play coordinator**,
including `.gitignore`, Cargo.toml, Cargo.lock, README.md, src/ and tests/.
`target/` remains ignored. No scratch tools or credentials belong in this commit.
Record its resulting SHA with each release; do not claim Rust is committed yet.

## Diagnostics, scratch tools and privacy

Removed no production diagnostics. Retained the Phase 6 NetPlayClient traversal
stage messages: server wait, host lookup requested/succeeded/failed, peer transport
connection, handshake success/failure and stop stage. They report fixed strings or
numeric failure reasons without addresses/codes, are useful for field debugging,
and do not add per-packet payload logging. Retained QuickPlayClient/Controller
queue, role, publication, connection, cancellation and generic failure events for
the same reason. Found no temporary hooks or local-topology branches to remove.

Removed stale current-test documentation for same-machine Phase 6 acceptance and
an obsolete pre-Quick-Play build fingerprint. Historical private Phase 5 proxy
instructions are superseded. Production source/build files have no references to
`/tmp/phase5_probe.py` or Phase 5/6 scratch harnesses. Neither Rust nor testers need
them. Do not include scratch helpers, temporary profiles, logs, tickets or user
data in tester packages. Normal packaging copies build resources, not `/tmp`.

All five QuickPlayClient HTTP paths (queue admission, queue poll, match poll,
host publication, cancellation) call `SetSensitive(true)` before the transfer.
Phase 4 HttpRequest handling replaces curl-error output with a generic message
and suppresses URLs and response bodies on HTTP errors. Request headers/bodies
are not logged and curl verbose tracing is not enabled; this includes Authorization
Bearer data. Quick Play does not enable redirects. Controller/client logs never
interpolate tickets, full match IDs, traversal codes or player nonces. Traversal
client logging was also checked. No privacy fix was necessary.

Rust logs its listening address only; application validation returns fixed error
codes rather than echoing submitted secrets. There is no request/access log layer
or configured coordinator secret. Proxy/Tailscale operational logs were not
inspected: hosting operators must avoid raw capability paths, headers and bodies.
Existing normal NetPlay player-name/chat logging remains; review logs before sharing.

## Validation and known issues

Maintainer-established: core Phase 6 complete, committed same-revision Windows and
Linux builds, cross-platform/cross-network automatic pairing/host/join into the
same normal lobby, manual NetPlay regression passed, Quick Play host unlisted.
This gate does not claim another two-machine runtime run or both role directions;
record each direction and lifecycle check using [TESTING.md](TESTING.md).

Coordinator verification: `cargo test --locked --offline` compiled from Cargo.lock
and passed two unit and fifteen API tests. Its real-process restart test initially
failed to obtain a startup address in the restricted sandbox, then passed with
loopback access using `cargo test --locked --offline --test restart` (18 tests total).
Rust formatting check (`cargo fmt --all -- --check`) also passed.
No Dolphin rebuild was needed for documentation-only edits. No scratch tool used.

Pre-existing Linux Launcher Path discovery differs from Windows; manual path
configuration may be needed. Quick Play inherits it; investigate separately.
Same-machine/same-public-IP traversal also fails with manual NetPlay in the
observed topology. Successful realistic cross-network Quick Play establishes this
is not a Quick Play regression or MVP blocker. Remaining feature/polish work is
listed in TESTING.md; Phase 7 remains next and has not started.

## Artifact infrastructure and handoff

Windows: `Source/dolphin-emu.sln`, MSBuild Release/x64, full `Binary/x64/` output.
The existing workflow uploads that directory. Use the repository's Windows build
prerequisites (MSVC, Windows SDK, Git and pinned submodules).

Linux: CMake local build outputs `Binaries/project-plus-dolphin` and needs Sys and
runtime libraries. The existing local cache contains that executable and generated
`SCM_REV_STR` equal to the baseline SHA above. This is supporting local-build
metadata, not a checksum/provenance claim for a packaged Windows/Linux pair.
The local Qt/ICU installation under `build-tools/qt/6.7.3/gcc_64` is a runtime
dependency: copying that executable alone is not a portable tester artifact.

AppImage: the existing workflow configures CMake/Ninja with Qt 6.7.3, installs an
AppDir, and uses linuxdeploy/Qt/AppImage tools, uploading `uploads/`. The repository
also provides `BuildLinuxAppImage.sh` for a local Ninja install/package. It uses
qmake6 from PATH and downloads continuous packaging tools; packages are not claimed
bit-for-bit reproducible. Its packaging path still needs a fresh smoke test.

**CI caveat:** `.github/workflows/ci.yml` Windows and AppImage source checkouts
explicitly select upstream `Project-Plus-Development-Team/Project-Plus-Dolphin`
`master`, not this Quick Play branch. A normal run/tag does not establish a correct
Quick Play artifact pair. The workflow also has no workflow_dispatch trigger.
Do not hand its outputs to Quick Play testers without inspecting their revision.
Infrastructure is unchanged here; use controlled local builds of an explicitly
selected clean commit. Any later CI change needs separate review.

All four Quick Play .cpp files and their headers are in DolphinQt/CMakeLists.txt
and DolphinQt.vcxproj; controller/dialog QtMoc entries are present. No new source
registration is required. A clean same-commit pair is feasible with the existing
local build systems and was previously manually validated; no new pair was
produced or certified by this gate.

Hand testers:

- Complete custom Windows Release/x64 archive, including runtime DLLs/resources.
- Custom Linux AppImage, or a complete local bundle verified on the target Linux
  environment with its libraries/resources (not the developer's bare executable).
- TESTING.md, actual coordinator URL, exact region and any tailnet access setup.
- A manifest with the exact clean Dolphin SHA shared by both artifacts, coordinator
  SHA, OS/architecture/toolchain, filenames, SHA-256 checksums and regression result.

Exclude personal User/user folders, games, logs, scratch tools and private planning.
Testers reuse their existing Project+ game/user data. Inspect archive contents and
launch on each target OS before handoff. If committing these docs produces a new
Dolphin SHA, rebuild **both** platforms at that SHA: even doc-only commits change
the matchmaking build key. Dirty source differences are not encoded in that key.

## Manual build checkpoint (maintainer only)

No code cleanup requires a rebuild now. For a fresh pair, first commit/review the
intended source and docs, select the same clean revision on both build machines,
and record `git rev-parse HEAD`, `git status --short`, and recursive submodule
status. Initialize pinned submodules with `git submodule update --init --recursive`.
Do not use mismatched old/new artifacts.

Windows, in a Visual Studio developer terminal at the selected checkout:

```bat
msbuild Source\dolphin-emu.sln /verbosity:minimal /property:Configuration=Release /property:Platform=x64
```

Linux, reusing this workspace's configured cache and installed Qt 6.7.3/ICU:

```sh
cmake --build /home/joemarchy/Projects/pplus/builds/plus-dolphin-baseline --parallel 4
```

For a separate fresh local Linux build using that same installed SDK:

```sh
cmake -S /home/joemarchy/Projects/pplus/Plus-Dolphin -B /home/joemarchy/Projects/pplus/builds/plus-dolphin-release-prep -G 'Unix Makefiles' -DLINUX_LOCAL_DEV=true -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH=/home/joemarchy/Projects/pplus/build-tools/qt/6.7.3/gcc_64
cmake --build /home/joemarchy/Projects/pplus/builds/plus-dolphin-release-prep --parallel 4
ln -s /home/joemarchy/Projects/pplus/Plus-Dolphin/Data/Sys /home/joemarchy/Projects/pplus/builds/plus-dolphin-release-prep/Binaries/Sys
```

The new directory/symlink commands assume that destination does not already exist.
Use repository Readme.md's AppImage prerequisites and Ninja packaging procedure
when producing a portable package; these local build commands alone do not bundle
Qt/ICU. Stop here for manual builds, package smoke checks and the compact regression
record. Do not begin Phase 7 as part of this gate.
