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
//     game is paused, and is it 1:1 with logic frames? Static RE says NO to the
//     first (MainLoop returns at 0x55D877, before this address, whenever
//     Scenario->unknown_62C != 0) and "1:1 except during modal UI" to the
//     second. Each invocation records the frame, the pause counter, the
//     IsGamePaused flag, the four modal-dialog gate bytes, and a wall clock, so
//     a pause shows up as a large time gap with no frames and a modal dialog
//     shows up as repeated records at one frame.
//
//   Spike A - is MainLoop re-entrancy from modal UI pumps reachable in normal
//     play? Note this CANNOT be measured by nesting our own handler: a pump
//     entered earlier in the outer MainLoop has not yet reached this hook and
//     one entered later has already left it, so the handler is never on the
//     stack twice and a naive depth counter is pinned at 1 by construction.
//     The observable that does work is the STACK POINTER: a nested MainLoop
//     activation runs at a markedly lower ESP, so re-entrancy appears as
//     records at one frame in a distinctly different `esp` band.
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
// The session nonce is ScenarioClass::Instance->UniqueID (YRpp: "random salt
// for this game's communications"), which is a strictly better new-game
// discriminator than the frame-counter-runs-backwards heuristic the other dump
// hooks use: it cannot false-negative when a new game happens to start at a
// higher frame.
class HarnessProbe
{
public:
	// Armed by HARNESS.Probe=yes.
	static bool Enable;

	// Working directory, relative to the game dir (HARNESS.Dir, default
	// "HARNESS"). DLL-owned fixed storage; never grows.
	static constexpr int MaxDirLen = 64;
	static char Dir[MaxDirLen];

	// Hard caps. Bounded work per invocation is a review requirement: this
	// runs on a render call and must never poll or parse without a ceiling.
	static constexpr int MaxScanPerFrame = 4;      // inbox probes per invocation
	static constexpr int MaxCommands = 512;        // command IDs tracked per session
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
