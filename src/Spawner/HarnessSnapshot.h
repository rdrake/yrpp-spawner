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

// Phase 0 observation verb: one CSV row per live TechnoClass at a requested
// frame. Read-only; walks TechnoClass::Array and writes. Adds no engine state.
//
// Identity is AbstractClass::UniqueID (design section 9). NOTE it is NOT a
// stable per-game salt - it is the object-ID allocator, reset to 1,000,000 at
// each scenario start - but it IS unique WITHIN a game, which is exactly what a
// session-local handle needs. See specs/notes/blackbox-spike-c-transport.md.
//
// Liveness: RemoveAllInactive runs AFTER the dispatch hook at 0x55DDA0, so dead
// objects are still in the array when we walk it. We therefore filter on the
// object's own flags, never on lookup success.
class HarnessSnapshot
{
public:
	// Bounded work: never emit more than this many rows per call, so a huge
	// battle cannot stall a render call.
	static constexpr int MaxRowsPerSnapshot = 512;

	// Appends rows for every live techno. `label` is the caller's command id,
	// so the host can group rows by the command that produced them.
	// Returns false only on file-open failure.
	static bool Write(const char* dir, int sessionId, int frame, int label);
};
