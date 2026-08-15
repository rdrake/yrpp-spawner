/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// Read-only evidence probe for the black-box driving-harness spikes B and C.
// Armed with ra2md.ini [Options] HARNESS.Probe=yes.
//
// STRICTLY READ-ONLY with respect to game state. It reads engine memory and
// the staged command inbox, and writes only its own log/ack files. It executes
// NO game mutation and queues NO events - the command verbs it understands are
// deliberately no-ops or pure reads. Mutation is Phase 1+ work and is out of
// scope for the spikes.
//
// It answers three questions that the static RE left open or could only assert:
//
//   Spike B (confirmatory) - does this dispatch point (0x55DDA0) fire while the
//     game is paused? ANSWERED 2026-07-21: no. But the mechanism is not the one
//     originally proposed - Scenario->unknown_62C was never nonzero across
//     23,658 records, so the 0x55D877 early-return is never exercised in
//     skirmish. No player-reachable skirmish action pauses via +0x62C at all
//     (PauseGame @0x684060 has four callers, all movie/map-trigger paths). The
//     Esc options screen goes quiet because it runs OUTSIDE MainLoop entirely.
//
//   Spike A - is MainLoop re-entrancy reachable? ANSWERED 2026-07-21: no, in any
//     game mode. 0xABCD58 is a real re-entry lock - MainLoop sets it at entry and
//     clears it at all three exits, and all five secondary call sites test it via
//     the accessor at 0x55CBF0 before calling. 0x55D360 is also never
//     address-taken, so there is no indirect route in.
//     The `esp` column stays because it is the only observable that COULD show
//     re-entrancy: it cannot be measured by nesting our own handler (a pump
//     entered earlier in the outer MainLoop has not yet reached this hook and one
//     entered later has already left it, so a depth counter reads 1 by
//     construction). A nested activation would run at a markedly lower ESP.
//     Observed: one constant band across 23,658 records, as predicted.
//
//   Spike C - does the transactional transport contract hold end to end?
//     Session nonce, monotonic command IDs, atomic publication, exactly-once
//     execution, terminal acks with the actual frame, stale-session rejection
//     after a restart, fixed caps, and a heartbeat the controller can time out
//     on.
//
// Transport contract (host side writes, probe side reads):
//
//   <dir>\inbox\%06d.cmd   one immutable file per command, published by
//                          write-to-.tmp-then-MoveFile (rename is atomic on
//                          NTFS, so an openable file is always complete).
//                          Body is line-oriented KEY=value:
//                            session=<nonce>   must equal Scenario->UniqueID
//                            id=<n>            must equal the file's number
//                            frame=<n>         0 = as soon as possible
//                            verb=<name>       noop | count-technos |
//                                              fail-test | end
//                          Values may be padded with spaces/tabs; a value may
//                          not itself contain a space.
//   <dir>\acks.txt         append-only, one terminal ack per command:
//                            id= status= reason= requested_frame= actual_frame=
//                            session=
//                          status is accepted-then-executed collapsed into a
//                          single terminal record, or rejected, or failed.
//   <dir>\status.txt       rewritten periodically; the controller's liveness
//                          signal and engine-exit detector. Also carries the
//                          error counters (obs_dropped, flush_failures,
//                          ack_write_failures, status_write_failures,
//                          duplicates_suppressed) so a bundle can never look
//                          clean while silently missing records.
//   <dir>\frames.txt       the observation log; every line carries S=<nonce> so
//                          two capture passes never merge indistinguishably.
//
// A command whose ack cannot be written is deliberately NOT consumed - it is
// retried - because consuming without a terminal ack would strand the
// controller forever, the one failure the transport contract forbids outright.
//
// A command that DOES reach a terminal ack is renamed .cmd -> .done, so a
// rescan can never re-ack it. See the session-identity note below for why that
// matters; the 2026-07-21 capture produced 978 terminal acks for 7 commands
// without it.
//
// SESSION IDENTITY - minted here, not borrowed from the engine.
//
// An earlier version used ScenarioClass::Instance->UniqueID as the session
// nonce, on the strength of the YRpp comment calling it a "random salt for this
// game's communications". That is wrong, and the 2026-07-21 capture disproved it
// directly. UniqueID is the AbstractClass object-ID allocator: 37 object
// constructors reach ++Scenario->UniqueID via Get_New_UniqueID @0x68BCB0. It is
// reset to exactly 1,000,000 at each scenario start (0x683633), so its value is
// essentially the map-load object count - a function of map dimensions. Measured:
// it drifted 305 WITHIN one game while moving only +2/+3 BETWEEN games, and both
// new games collided with values an earlier game had held. It cannot identify a
// session, pinned or otherwise; the dangerous failure is a stale command being
// falsely ACCEPTED into a later game.
//
// So identity is minted (GetTickCount at session open) and only the BOUNDARY is
// read from the engine, using two independent signals, either sufficient alone:
//   - UniqueID decreases. It only ever increments within a game and is reset
//     downward at scenario start, so any decrease is a proven boundary.
//     (Ignore negative readings: 0x4D7F42 and 0x4AD05F park -3 in the field
//     temporarily and restore it.)
//   - CurrentFrame decreases. The classic heuristic, kept as belt and braces.
//
// Command IDs are a flat PER-PROCESS namespace and deliberately do NOT rewind on
// a new session - rewinding the scanner is what made the old build rescan and
// re-ack a stale inbox.
class HarnessProbe
{
public:
	// Armed by HARNESS.Probe=yes.
	static bool Enable;

	// Working directory, relative to the game dir (HARNESS.Dir, default
	// "HARNESS"). DLL-owned fixed storage; never grows.
	static constexpr int MaxDirLen = 64;
	static char Dir[MaxDirLen];

	// HARNESS.Seed, or 0 for "leave the engine's stock behaviour alone".
	// Recorded here purely so the manifest can state whether a run's seed was
	// pinned or drifted in from the system timer; the pin itself is applied
	// once at config time (Main.Config.cpp) by storing to 0xA8ED98.
	static int PinnedSeed;

	// Hard caps. Bounded work per invocation is a review requirement: this
	// runs on a render call and must never poll or parse without a ceiling.
	static constexpr int MaxScanPerFrame = 4;      // inbox probes per invocation

	// Command IDs tracked. This is a PER-PROCESS budget, not per-session: the id
	// namespace no longer rewinds at a game boundary (see the session-identity
	// note above), so a long multi-game run draws from one pool. Sized well above
	// any plausible spike or Phase-1 run; seenMask costs MaxCommands/8 bytes.
	// Exhaustion is loud (Debug::Log + ended=1 in status.txt), never silent.
	static constexpr int MaxCommands = 4096;
	static constexpr int MaxObsRecords = 2048;     // buffered frame observations
	static constexpr int MaxCmdFileBytes = 512;    // refuse anything larger

	// Flush/heartbeat cadence. BOTH a frame budget and a wall-clock budget:
	// frame-only gating goes silent during a modal dialog (where the frame does
	// not advance), which is one of the very windows being measured - and a
	// frozen status.txt reads as a dead engine to the controller.
	static constexpr int ObsFlushInterval = 240;      // frames between flushes
	static constexpr unsigned int ObsFlushMs = 2000;  // or this long, whichever first
	static constexpr int StatusInterval = 60;         // frames between heartbeats
	static constexpr unsigned int StatusMs = 500;     // or this long, whichever first

	// Called from the shared MainLoop dispatch hook at 0x55DDA0.
	static void PerFrame();
};
