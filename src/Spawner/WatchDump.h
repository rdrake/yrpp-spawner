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

// Write-watch dumper. When armed (ra2md.ini [Options] WATCHDUMP=yes plus
// WATCHDUMP.Targets) it records every instruction that stores into a watched
// range, from the moment settings load (before the scenario exists) to
// WATCHDUMP.MaxFrames, to WATCHDUMP\WATCH_<seed>.TXT.
//
// WHY: "who writes this field" is the one question no other instrument
// answers. find_refs misses stores whose displacement is computed, and
// every per-frame dump sees only the value after the frame, not the writer.
// Each row carries the storing EIP, the value before and after, and up to
// four stack dwords that look like return addresses (heuristic: inside
// .text and preceded by a call).
//
// Targets: `ADDR:LEN` for a global, `[SLOT]+OFF:LEN` for a field of the
// object whose pointer lives at SLOT (re-resolved whenever SLOT is written),
// e.g. `A8B29C:32` = AISlots.Countries. A `[SLOT]` target on a heap page
// can hang the game (WatchEngine.h, LIMITS).
//
// Mechanism and limits: Spawner/WatchEngine.h. The game's simulation is not
// touched - no RNG draw, no game allocation, no game-state write - but every
// store to a watched PAGE costs a fault and a single-step (about 100 us under
// Wine on Apple Silicon), so keep targets off hot pages or bound MaxFrames.
class WatchDump
{
public:
	// Arms the engine; logs and does nothing when the spec or the self-test
	// fails (an attached Syringe fails the self-test).
	static void Arm(const char* targets, int maxFrames);

	// Called once per logic frame from the shared MainLoop-after-render hook
	// (0x55DDA0). Writes the rows recorded since the last call; disarms after
	// MaxFrames.
	static void PerFrame();
};
