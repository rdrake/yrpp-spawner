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

#include "AstarDump.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <FootClass.h>
#include <GeneralDefinitions.h>
#include <GeneralStructures.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>

bool AstarDump::Enable = false;
AstarDump::Mode AstarDump::CaptureMode = AstarDump::Mode::Disabled;

namespace
{
	constexpr char DumpDir[] = "ASTARDUMP";

	// DLL-owned static storage - never grows, never allocates from the game
	// pool. Zero-initialized at load time.
	AstarDump::Record records[AstarDump::MaxRecords];
	int recordCount = 0;

	// Shared "current armed record" handle. The FindPath-entry hook (Task 8)
	// arms a record via Arm(), which stashes it here; the inner core-search
	// hooks (Task 9, below) read it back through AstarDump::Current() and
	// append per-attempt data. nullptr means no FindPath is being captured.
	AstarDump::Record* currentRecord = nullptr;

	// Pending per-attempt corridor-activation flag, captured at each
	// core-search entry (FUN_00429A90 param_7). The brief wants the SUCCESSFUL
	// attempt's corridor state promoted to the record; without the FindPath
	// exit hook (Task 11) there is no reliable success signal at this layer, so
	// this task records the LAST attempt's state (each entry overwrites it and
	// the record's CorridorActive/COARSE_PATH) and leaves the promotion of the
	// successful attempt to Task 11, which can read this pending value.
	bool pendingCorridorActive = false;
}

AstarDump::Record* AstarDump::Arm(int frame, uint32_t unitId, int startX, int startY,
	int destX, int destY, const char* speedType)
{
	if (!Enable)
		return nullptr;

	if (recordCount >= MaxRecords)
	{
		// Record-buffer-full drops silently in Task 7, matching the section
		// appenders (AppendRawCode/AppendCoarsePathId/AppendCostGridCell). The
		// logged overflow marker is Task 11's scope, so all drop paths stay
		// consistent until then.
		return nullptr;
	}

	Record* record = &records[recordCount++];
	std::memset(record, 0, sizeof(Record));

	record->InUse = true;
	record->Frame = frame;
	record->UnitId = unitId;
	record->StartX = startX;
	record->StartY = startY;
	record->DestX = destX;
	record->DestY = destY;
	record->Attempts = 0;
	record->CorridorActive = false;
	record->RawCodesCount = 0;
	record->CoarsePathCount = 0;
	record->CostGridCount = 0;
	std::strncpy(record->SpeedType, speedType ? speedType : "", sizeof(record->SpeedType) - 1);
	std::strncpy(record->Result, "pending", sizeof(record->Result) - 1);

	// This freshly-armed record becomes the module's current capture target so
	// the inner core-search hooks can append to it.
	currentRecord = record;
	pendingCorridorActive = false;

	return record;
}

AstarDump::Record* AstarDump::Current()
{
	return currentRecord;
}

void AstarDump::ClearCurrent()
{
	currentRecord = nullptr;
	pendingCorridorActive = false;
}

void AstarDump::AppendRawCode(Record* record, int code)
{
	if (!record)
		return;

	if (record->RawCodesCount >= MaxRawCodes)
		return;

	record->RawCodes[record->RawCodesCount++] = code;
}

void AstarDump::AppendCoarsePathId(Record* record, int cellId)
{
	if (!record)
		return;

	if (record->CoarsePathCount >= MaxCoarsePathIds)
		return;

	record->CoarsePath[record->CoarsePathCount++] = cellId;
}

void AstarDump::AppendCostGridCell(Record* record, int x, int y, int cost, int flags)
{
	if (!record)
		return;

	if (record->CostGridCount >= MaxCostGridCells)
		return;

	CostGridCell& cell = record->CostGrid[record->CostGridCount++];
	cell.X = x;
	cell.Y = y;
	cell.Cost = cost;
	cell.Flags = flags;
}

namespace
{
	void WriteRecord(const AstarDump::Record& record)
	{
		char path[MAX_PATH];
		sprintf(path, "%s\\ASTAR_%d_%u.TXT", DumpDir, record.Frame, record.UnitId);

		FILE* pFile = fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[AstarDump] Failed to open %s for write\n", path);
			return;
		}

		fprintf(pFile, "FRAME=%d\n", record.Frame);
		fprintf(pFile, "UNITID=0x%08x\n", record.UnitId);
		fprintf(pFile, "START=%d,%d\n", record.StartX, record.StartY);
		fprintf(pFile, "DEST=%d,%d\n", record.DestX, record.DestY);
		fprintf(pFile, "SPEEDTYPE=%s\n", record.SpeedType);
		fprintf(pFile, "ATTEMPTS=%d\n", record.Attempts);
		fprintf(pFile, "CORRIDOR_ACTIVE=%d\n", record.CorridorActive ? 1 : 0);

		fprintf(pFile, "COARSE_PATH=");
		if (record.CoarsePathCount == 0)
		{
			fprintf(pFile, "empty");
		}
		else
		{
			for (int i = 0; i < record.CoarsePathCount; ++i)
				fprintf(pFile, "%s%d", (i == 0) ? "" : ",", record.CoarsePath[i]);
		}
		fprintf(pFile, "\n");

		// COSTGRID: an empty grid MUST serialize as a bare value ("COSTGRID=") —
		// the Rust parser (astar_dump.rs) routes any non-empty string into the
		// x:y:class:flags splitter and hard-fails on the literal "empty" (unlike
		// COARSE_PATH, whose parser branch special-cases "empty"). Flags are emitted
		// in HEX: the parser reads parts[3] via from_str_radix(., 16). Cost/class
		// (parts[2]) stays decimal to match the parser's parse::<u8>().
		fprintf(pFile, "COSTGRID=");
		for (int i = 0; i < record.CostGridCount; ++i)
		{
			const AstarDump::CostGridCell& cell = record.CostGrid[i];
			fprintf(pFile, "%s%d:%d:%d:%x", (i == 0) ? "" : ";", cell.X, cell.Y, cell.Cost, cell.Flags);
		}
		fprintf(pFile, "\n");

		// RAW_CODES: an empty list is a bare value ("RAW_CODES="), NOT the literal
		// "empty" — the parser splits on ',' and would fail to parse "empty" as u8.
		fprintf(pFile, "RAW_CODES=");
		for (int i = 0; i < record.RawCodesCount; ++i)
			fprintf(pFile, "%s%d", (i == 0) ? "" : ",", record.RawCodes[i]);
		fprintf(pFile, "\n");

		fprintf(pFile, "RAW_LEN=%d\n", record.RawCodesCount);
		fprintf(pFile, "RESULT=%s\n", record.Result);

		fclose(pFile);
	}
}

void AstarDump::Flush()
{
	if (recordCount == 0)
		return;

	CreateDirectoryA(DumpDir, nullptr);

	for (int i = 0; i < recordCount; ++i)
	{
		if (records[i].InUse)
			WriteRecord(records[i]);
	}

	Reset();
}

void AstarDump::Reset()
{
	recordCount = 0;
	currentRecord = nullptr;
	pendingCorridorActive = false;
}

namespace
{
	// Task 8 gate: yes-mode arms only these three start cells (values from the
	// pinned scenario replay), all-mode arms every harvester FindPath call.
	bool IsGateStartCell(short x, short y)
	{
		return (x == 136 && y == 211)
			|| (x == 138 && y == 213)
			|| (x == 130 && y == 216);
	}

	// The moving object's type pointer (ObjectClass::GetType(), a pure RTTI/
	// static-data accessor - no simulation state is mutated or read by calling
	// it) is only meaningfully a UnitTypeClass* when the object itself is a
	// UnitClass; WhatAmI() (also a pure RTTI query) confirms that before the
	// downcast. Harvesters in the retail rules are always UnitClass instances
	// with TechnoTypeClass::Harvester set, so this is the correct all-mode
	// filter and the correct source for SPEEDTYPE.
	UnitTypeClass* GetUnitTypeIfUnit(FootClass* pFoot)
	{
		if (!pFoot || pFoot->WhatAmI() != AbstractType::Unit)
			return nullptr;

		return static_cast<UnitTypeClass*>(pFoot->GetType());
	}

	bool IsHarvesterFoot(FootClass* pFoot)
	{
		UnitTypeClass* pType = GetUnitTypeIfUnit(pFoot);
		return pType && pType->Harvester;
	}

	const char* SpeedTypeName(SpeedType speed)
	{
		switch (speed)
		{
		case SpeedType::Foot:       return "foot";
		case SpeedType::Track:      return "track";
		case SpeedType::Wheel:      return "wheel";
		case SpeedType::Hover:      return "hover";
		case SpeedType::Winged:     return "winged";
		case SpeedType::Float:      return "float";
		case SpeedType::Amphibious: return "amphibious";
		case SpeedType::FloatBeach: return "floatbeach";
		case SpeedType::None:
		default:                    return "none";
		}
	}
}

// AStarClass::FindPath entry (VA 0x42C900, __thiscall, callee-cleans
// `ret 0x1c`; see docs/superpowers/notes/astardump-offsets.md Step 2/Step 5 in
// the ratwo repo for the full ABI citation). The hook is installed at the
// very first instruction, before the function's own prologue runs, so ESP at
// hook time still points at the return address the CALL pushed and ECX still
// holds `this` - the stack-arg offsets below (+0x4.. +0xc) are exactly the
// pinned ABI's stack offsets, unshifted. Patched byte count (0x5) is chosen
// to end on an instruction boundary: `sub esp,0x20` (3 bytes) + `push ebx`
// (1 byte) + `push ebp` (1 byte) = 5 bytes, matching the confirmed raw
// disassembly of the prologue.
//
// This task (Task 8) reads only the search inputs: start cell (+0x4), dest
// cell (+0x8), and the moving object (+0xc, used for UNITID + SPEEDTYPE).
// RAW_CODES (+0x10), node budget (+0x14), zone override (+0x18) and the
// corridor-activation argument (+0x1c) are Task 9/10 scope and are not read
// here. `this` (ECX, AStarClass*) is not read: nothing this task captures
// depends on any AStarClass field, only on the call's own arguments.
//
// Read-only: no field of AStarClass/FootClass/UnitTypeClass touched here is
// written, no game-pool allocation happens, and the two RTTI accessors
// called (WhatAmI(), GetType()) are pure metadata queries with no simulation
// side effects (they do not draw from Randomizer or mutate any object).
DEFINE_HOOK(0x42C900, AStarClass_FindPath_AstarDumpEntry, 0x5)
{
	if (!AstarDump::Enable)
		return 0;

	// Start every entered FindPath with no current capture target: Arm() below
	// re-sets it only when this call is actually being traced. Without this a
	// non-armed FindPath would leave the previous call's record current, and
	// the two global inner hooks (which fire for EVERY FindPath) would append
	// this call's cells into the wrong record. Task 11's exit hook will also
	// clear it after the search returns.
	AstarDump::ClearCurrent();

	GET_STACK(CellStruct*, pStart, 0x4);
	GET_STACK(CellStruct*, pDest, 0x8);
	GET_STACK(FootClass*, pFoot, 0xc);

	if (!pStart || !pDest)
		return 0;

	bool armThis = false;
	switch (AstarDump::CaptureMode)
	{
	case AstarDump::Mode::Narrow:
		armThis = IsGateStartCell(pStart->X, pStart->Y);
		break;
	case AstarDump::Mode::All:
		armThis = IsHarvesterFoot(pFoot);
		break;
	case AstarDump::Mode::Disabled:
	default:
		armThis = false;
		break;
	}

	if (!armThis)
		return 0;

	const int frame = Unsorted::CurrentFrame;
	const uint32_t unitId = pFoot ? pFoot->UniqueID : 0u;

	SpeedType speedType = SpeedType::None;
	if (UnitTypeClass* pType = GetUnitTypeIfUnit(pFoot))
		speedType = pType->SpeedType;

	AstarDump::Arm(frame, unitId, pStart->X, pStart->Y, pDest->X, pDest->Y,
		SpeedTypeName(speedType));

	return 0;
}

// AStarClass core per-attempt search entry (VA 0x429A90, FUN_00429A90,
// __thiscall; see astardump-offsets.md Step 3). FindPath calls this once per
// hierarchical-corridor retry; ECX = param_1 = AStarClass* (this), and stack
// args param_2..param_7 sit at [esp+0x4]..[esp+0x18]. The hook is installed at
// the first instruction, before the prologue runs, so ESP still points at the
// CALL's return address ([esp+0]) and the stack-arg offsets are unshifted -
// param_7 (the corridor-activation flag) is at [esp+0x18]. Patched byte count
// (0x5) ends on an instruction boundary: `sub esp,0x4c` (3 bytes, 83ec4c) +
// `push ebx` (1, 53) + `push ebp` (1, 55) = 5 bytes (confirmed by capstone
// linear disasm of FUN_00429A90 from its 0x429A90 entry).
//
// This is a GLOBAL hook - it fires for EVERY FindPath's core search, gated or
// not - so it bails immediately when no record is armed (Current()==nullptr),
// keeping the read-only, low-overhead design.
//
// Per armed attempt: increment ATTEMPTS; read param_7 -> CORRIDOR_ACTIVE
// (stashed pending for Task 11 to promote the successful attempt, and written
// through to the record as the LAST attempt's state); and refill COARSE_PATH
// from `this`'s level-0 coarse-path buffer (AStar+0xbc, u16 ids, count at
// AStar+0xc74). Read-only: only argument/field reads, no writes, no alloc, no
// re-invocation of any engine routine.
DEFINE_HOOK(0x429A90, AStarClass_CoreSearch_AstarDumpAttempt, 0x5)
{
	if (!AstarDump::Enable)
		return 0;

	AstarDump::Record* record = AstarDump::Current();
	if (!record)
		return 0;

	// param_7 (corridor-activation flag) at [esp+0x18].
	GET_STACK(BYTE, corridorActiveByte, 0x18);
	const bool corridorActive = (corridorActiveByte != 0);

	++record->Attempts;

	// Last-attempt semantics (documented deviation - no success signal exists
	// without Task 11's exit hook): overwrite the record's corridor state and
	// coarse path each attempt. pendingCorridorActive lets Task 11 promote the
	// SUCCESSFUL attempt's value instead.
	pendingCorridorActive = corridorActive;
	record->CorridorActive = corridorActive;

	// COARSE_PATH: level-0 corridor coarse path on `this` (ECX = AStarClass*).
	// buffer = AStar + 0xbc (u16 cell ids, up to 500/level); count = AStar +
	// 0xc74 (int). Level 0 chosen per the brief's "if ambiguous, capture level
	// 0 and document" - the level-0 buffer is the top-level corridor path the
	// per-cell retries follow.
	const uintptr_t astar = R->ECX<uintptr_t>();
	record->CoarsePathCount = 0; // refill for this (latest) attempt
	int coarseCount = *reinterpret_cast<const int*>(astar + 0xc74);
	if (coarseCount < 0)
		coarseCount = 0;
	if (coarseCount > 500)
		coarseCount = 500; // per-level physical cap (500 u16 entries/level)

	const unsigned short* coarse = reinterpret_cast<const unsigned short*>(astar + 0xbc);
	for (int i = 0; i < coarseCount; ++i)
		AstarDump::AppendCoarsePathId(record, coarse[i]);

	return 0;
}

// COSTGRID capture (VA 0x429F5A, inside FUN_00429A90's neighbor-eval loop; see
// astardump-offsets.md Step 4 "Neighbor-eval cost-class site"). The engine
// instruction at this address is `mov ebx,eax` (bytes 8b d8), copying the
// vtable-107 cost-class getter's return value into EBX. Syringe restores the
// patched original bytes AFTER the hook body runs (the same pre-instruction
// register semantics the Task 8 entry hook relies on to read unshifted stack
// args), so at hook entry the `mov ebx,eax` has NOT executed yet. Therefore:
//   * EAX = the vtable-107 return value = the cost class (the value about to
//     be moved into EBX; == decompile iVar17 before its `<7 -> 0` clamp).
//   * EBX = the candidate CellClass* (decompile iVar16), still live - it is
//     the pointer this loop dereferenced at the corridor gate ([ebx+0x122])
//     and pushed as the vtable-107 call's first argument at 0x429F51
//     (`push ebx`), and it is callee-preserved across the 0x429F54 call.
//
// This RESOLVES the note's open sub-pin (which register holds the CellClass*
// at 0x429F5A): capstone shows it is EBX, and consequently the class must be
// read from EAX rather than EBX at this exact hook instant. Reading EBX here
// would yield the cell pointer, not the class; reading the class from EAX and
// the cell from EBX captures both with no re-invocation of the getter.
//
// Patched byte count (0x6) ends on an instruction boundary: `mov ebx,eax`
// (2 bytes, 8bd8) + `mov al,[esp+0x12]` (4 bytes, 8a442412) = 6 bytes (a 5-byte
// jmp cannot fit within the 2-byte `mov ebx,eax` alone, so the next whole
// instruction is included; both re-execute normally after the hook returns).
//
// GLOBAL hook, fires per-neighbor (thousands/search): bails when no record is
// armed. The 4096-cell cap is enforced by AppendCostGridCell. Read-only:
// direct register + cached-field reads only, no getter re-invocation, no
// writes.
DEFINE_HOOK(0x429F5A, AStarClass_CoreSearch_AstarDumpCostGrid, 0x6)
{
	if (!AstarDump::Enable)
		return 0;

	AstarDump::Record* record = AstarDump::Current();
	if (!record)
		return 0;

	const int costClass = R->EAX<int>();
	const uintptr_t cell = R->EBX<uintptr_t>();
	if (!cell)
		return 0;

	// CellClass field reads (confirmed against reference/yrpp_struct_fields.csv
	// and YRpp/CellClass.h): MapCoords (CellStruct{short X; short Y}) at +0x24,
	// so X = +0x24 (u16), Y = +0x26 (u16); Flags (u32) at +0x140 (the 0x40000
	// layer bit lives here). Emitted as the raw u32 (serializer writes hex).
	const int x = *reinterpret_cast<const unsigned short*>(cell + 0x24);
	const int y = *reinterpret_cast<const unsigned short*>(cell + 0x26);
	const int flags = static_cast<int>(*reinterpret_cast<const uint32_t*>(cell + 0x140));

	AstarDump::AppendCostGridCell(record, x, y, costClass, flags);

	return 0;
}
