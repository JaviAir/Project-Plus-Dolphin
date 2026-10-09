# Project+ Online frontend bridge

This opt-in PPON v1 EXI endpoint lets a local Project+ frontend invoke Quick Play,
read authoritative frontend status, and cancel its observed matchmaking attempt.
It does not provide synchronized game inputs.

Enable with `PPLUS_ONLINE_EXI_PROTOTYPE=1`. Attachment requires a recognized
Netplay/Offline Launcher boot ID and an empty configured Slot B. Guest support
must be installed separately. This is a development opt-in, not binary attestation.

Requests are exactly 12 bytes, big endian: magic `0x50504f4e`, version u16=1,
command u16, sequence u32. Commands are HELLO=1, START_QUICKPLAY=2, GET_STATUS=3,
and CANCEL_QUICKPLAY=4. Responses echo magic, version, result and sequence.
GET_STATUS extends the response to 24 bytes with schema u16=1, status u16,
epoch u32, reason u16 and reserved u16=0. See Protocol.h and Status.h for values.
DMA buffers must be aligned to 32 bytes and wholly within MEM1 or MEM2.

HELLO is required; sequences must increase without wrapping. Accepted START or
CANCEL acknowledges guarded host delivery, not completion of matchmaking or
teardown. GET_STATUS is observational and does not enqueue work. Cancel targets
only the attempt observed in a consumed status response; stale attempts are inert.

The endpoint copies bounded packets and queues at most one cancellable host job.
It owns no Qt objects or network connections. Reset, savestate load, destruction,
and deterministic execution invalidate it. It remains absent/inactive in the
synchronized NetPlay Core. Session/dispatch state is deliberately not serialized.

The Qt controller owns matchmaking and the stop/lobby/fresh-boot handoff.
In-game handoff currently requires an external XCB/OpenGL render window.
Recovery tears down NetPlay before booting a fresh local frontend. Desktop Quick
Play retains the normal lobby workflow. Same-window continuity does not preserve
the emulated Core.

Existing tests are ProjectPlusOnlineProtocol.*, ProjectPlusOnlineStatus.* and
ProjectPlusOnlineCancel.* in the `tests` target. Runtime and private planning
artifacts are not part of this source directory.
