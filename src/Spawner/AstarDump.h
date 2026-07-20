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

#include <cstdint>

// Per-FindPath A* pathfinding trace dumper. When armed (ra2md.ini [Options]
// ASTARDUMP=yes|all) it collects a fixed-capacity, DLL-owned snapshot of a
// harvester's pathfinding attempt (start/dest, raw direction codes, coarse
// cell path, cost-grid samples) into AstarRecord slots and, on Flush(),
// serializes each armed record to ASTARDUMP\ASTAR_<frame>_<unit>.TXT in the
// shared KEY=value record format. Read-only with respect to game state: it
// never mutates the simulation or the pathfinder's own working set.
//
// This module is a skeleton only (Task 7): no engine hooks arm records yet.
// Later tasks (8-11) call Arm()/the per-section setters from FindPath hooks;
// Task 13 adds a SETTLE section (capacity reserved below, storage deferred).
class AstarDump
{
public:
	// ASTARDUMP=no (default): disabled.
	// ASTARDUMP=yes: enabled, narrow gate applied by a later task (Task 8).
	// ASTARDUMP=all: enabled, captures every harvester FindPath.
	enum class Mode
	{
		Disabled = 0,
		Narrow = 1,
		All = 2,
	};

	static bool Enable;
	static Mode CaptureMode;

	// Fixed-capacity dimensions. DLL-owned static storage only - never grows,
	// never allocates from the game pool. Overflow drops the offending
	// section/record (a later task adds the logged marker, Task 11).
	static constexpr int MaxRecords = 64;
	static constexpr int MaxRawCodes = 256;
	static constexpr int MaxCostGridCells = 4096;
	static constexpr int MaxCoarsePathIds = 256;
	// Reserved for Task 13's SETTLE section; no storage allocated yet.
	static constexpr int MaxSettleEntries = 8192;

	struct CostGridCell
	{
		int X;
		int Y;
		int Cost;
		int Flags;
	};

	struct Record
	{
		bool InUse;

		int Frame;
		uint32_t UnitId;
		int StartX;
		int StartY;
		int DestX;
		int DestY;
		char SpeedType[16];
		int Attempts;
		bool CorridorActive;

		int RawCodes[MaxRawCodes];
		int RawCodesCount;

		int CoarsePath[MaxCoarsePathIds];
		int CoarsePathCount;

		CostGridCell CostGrid[MaxCostGridCells];
		int CostGridCount;

		char Result[16];

		// Task 11: per-section overflow-marker anti-spam. Each Append* function
		// logs an [AstarDump] "dropped (buffer full)" line the FIRST time its
		// section's cap is hit, then sets this flag so later drops in the same
		// record (there can be thousands for COSTGRID) stay silent. Zeroed by
		// Arm()'s memset, so every freshly-armed record can log once per section.
		bool RawCodesOverflowLogged;
		bool CoarsePathOverflowLogged;
		bool CostGridOverflowLogged;
	};

	// Reserves the next free record slot and fills its header fields
	// (frame/unit/start/dest/speed type). Returns nullptr if AstarDump is
	// disabled or the buffer is full (MaxRecords already armed) - the caller
	// must treat a null return as "this attempt is not being traced".
	static Record* Arm(int frame, uint32_t unitId, int startX, int startY,
		int destX, int destY, const char* speedType);

	// Appends one code to record->RawCodes, dropping it (record is left
	// intact, just short) once MaxRawCodes is reached.
	static void AppendRawCode(Record* record, int code);

	// Appends one coarse-path cell id, dropping it once MaxCoarsePathIds is
	// reached.
	static void AppendCoarsePathId(Record* record, int cellId);

	// Appends one cost-grid sample, dropping it once MaxCostGridCells is
	// reached.
	static void AppendCostGridCell(Record* record, int x, int y, int cost, int flags);

	// Serializes every armed record to ASTARDUMP\ASTAR_<frame>_<unit>.TXT and
	// clears the buffer.
	static void Flush();

	// Clears the buffer without writing anything (e.g. on a new game start).
	static void Reset();

	// Returns the record most recently armed by Arm() (the "current" FindPath
	// being captured), or nullptr when no capture is in flight. The inner
	// core-search hooks (Task 9+) append per-attempt data to this record; a
	// null return means "this FindPath is not being traced" and the hook must
	// no-op cheaply. Set by Arm() on a successful arm; cleared at FindPath
	// entry (Task 8 hook), on Flush()/Reset(), and by Task 11's exit hook.
	static Record* Current();

	// Clears the current-record handle (and the pending per-attempt state)
	// without touching the record buffer. Task 11's FindPath-exit hook calls
	// this once the search returns; the FindPath-entry hook calls it before
	// deciding whether to arm, so a non-captured FindPath never inherits a
	// stale current record.
	static void ClearCurrent();
};
