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
	// core-search entry (FUN_00429A90 param_7). Each attempt overwrites it (and
	// the record's CorridorActive/COARSE_PATH) with the LAST attempt's state.
	// Task 11's FindPath-exit hooks (below) promote this to the record's final
	// CorridorActive exactly once, at return: since FindPath returns as soon as
	// a core-search attempt succeeds (no further retries follow a success),
	// "last attempt" and "successful attempt" coincide for a successful search;
	// for a failed search there is no successful attempt to prefer, so the
	// last-tried state is kept as the most meaningful value available.
	bool pendingCorridorActive = false;
}

AstarDump::Record* AstarDump::Arm(int frame, uint32_t unitId, int startX, int startY,
	int destX, int destY, const char* speedType)
{
	if (!Enable)
		return nullptr;

	if (recordCount >= MaxRecords)
	{
		// Task 11: record-buffer-full is a rare, low-frequency event (at most
		// once per FindPath call while all MaxRecords slots are InUse), so it's
		// safe to log unconditionally - no anti-spam flag needed here, unlike
		// the section appenders below.
		Debug::Log("[AstarDump] dropped (buffer full): record buffer full (%d armed), frame=%d unit=0x%08x\n",
			MaxRecords, frame, unitId);
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
	{
		// Task 11: log once per record (see Record::RawCodesOverflowLogged);
		// RAW_CODES can refill multiple times per FindPath (one per
		// reconstruction), so this can still legitimately fire more than once
		// across a record's lifetime, just not thousands of times per fire.
		if (!record->RawCodesOverflowLogged)
		{
			Debug::Log("[AstarDump] dropped (buffer full): RAW_CODES capped at %d for unit 0x%08x\n",
				MaxRawCodes, record->UnitId);
			record->RawCodesOverflowLogged = true;
		}
		return;
	}

	record->RawCodes[record->RawCodesCount++] = code;
}

void AstarDump::AppendCoarsePathId(Record* record, int cellId)
{
	if (!record)
		return;

	if (record->CoarsePathCount >= MaxCoarsePathIds)
	{
		if (!record->CoarsePathOverflowLogged)
		{
			Debug::Log("[AstarDump] dropped (buffer full): COARSE_PATH capped at %d for unit 0x%08x\n",
				MaxCoarsePathIds, record->UnitId);
			record->CoarsePathOverflowLogged = true;
		}
		return;
	}

	record->CoarsePath[record->CoarsePathCount++] = cellId;
}

void AstarDump::AppendCostGridCell(Record* record, int x, int y, int cost, int flags)
{
	if (!record)
		return;

	if (record->CostGridCount >= MaxCostGridCells)
	{
		// Fires per-neighbor once the cap is hit (thousands of times/search
		// without the anti-spam flag) - this is exactly the "if cheap" case the
		// per-record bool exists for.
		if (!record->CostGridOverflowLogged)
		{
			Debug::Log("[AstarDump] dropped (buffer full): COSTGRID capped at %d for unit 0x%08x\n",
				MaxCostGridCells, record->UnitId);
			record->CostGridOverflowLogged = true;
		}
		return;
	}

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
		// Fix I1: per-search cost-class clamp condition (bVar10 @ FUN_00429A90).
		// The COSTGRID class field is the RAW, PRE-CLAMP class; the replay applies
		// `if (CLAMP_ACTIVE && class < 7) class = 0` per cell to get the engine's
		// effective class. New backward-compatible key - the Rust parser ignores
		// unknown keys via fields.get(), so this is safe until ratwo consumes it.
		fprintf(pFile, "CLAMP_ACTIVE=%d\n", record.ClampActive ? 1 : 0);

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
		// Fix I2: harvester-gate Narrow mode too. The 3 gate cells ARE harvester
		// start cells, so the intent is harvester-only - but arming purely on the
		// start cell would also catch a non-harvester (infantry/MCV) that happens
		// to run FindPath from an exact gate cell. That record would carry a
		// foot/hover/none SPEEDTYPE, which the Rust parser (astar_dump.rs) HARD-
		// REJECTS (only track/wheel are accepted), corrupting the trace. ANDing
		// IsHarvesterFoot (the same helper All mode already uses) makes both modes
		// harvester-gated, so SPEEDTYPE is always track/wheel.
		armThis = IsGateStartCell(pStart->X, pStart->Y) && IsHarvesterFoot(pFoot);
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
	// Task 11 fix: MaxCoarsePathIds (256) is the actual storage cap - smaller
	// than the 500-entry physical buffer bound above. Clamping only the append
	// (not the loop) meant ids 256..499 were read and individually dropped by
	// AppendCoarsePathId every attempt; clamp the loop bound itself instead so
	// nothing past the real cap is read or iterated over. Pure bound fix: no
	// behavior change for coarseCount <= MaxCoarsePathIds.
	if (coarseCount > AstarDump::MaxCoarsePathIds)
		coarseCount = AstarDump::MaxCoarsePathIds;

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
// COST-CLASS CLAMP (Fix I1): the captured class (EAX here) is PRE-CLAMP. The
// engine clamps it immediately after this hook window (capstone, linear from
// 0x429A90):
//   0x429f5a: mov ebx,eax             ; ebx <- raw cost class (this hook: EAX)
//   0x429f5c: mov al,[esp+0x12]       ; al  <- bVar10 (clamp-condition byte)
//   0x429f60: test al,al
//   0x429f62: je   0x429f6b           ; cond==0 -> no clamp
//   0x429f64: cmp  ebx,7
//   0x429f67: jge  0x429f6b           ; class>=7 -> no clamp
//   0x429f69: xor  ebx,ebx            ; else class = 0   (decompile line 216:
//                                     ;   `if (bVar10 && iVar17 < 7) iVar17 = 0`)
// The clamped class in EBX is what the engine then feeds into the cost math
// (pushed at 0x429F84 into the 0x429830 cost call). `[esp+0x12]` (bVar10) is a
// PER-SEARCH CONSTANT: set once at FUN_00429A90 lines 98-101 from a vtable-33
// result field (+0xc94) BEFORE the neighbor loop, never reassigned in it, so
// it is identical for every neighbor of this search. We read it here (it is the
// second instruction of this very 6-byte patch window - ESP is unchanged
// between 0x429F5A and the engine's own read at 0x429F5C, so GET_STACK at
// offset 0x12 reads the identical byte) and stash it as record->ClampActive
// (each cell writes the same value). REPLAY MUST reproduce the clamp per cell:
//   effective_class = (CLAMP_ACTIVE && raw_class < 7) ? 0 : raw_class
// Capturing raw class + CLAMP_ACTIVE keeps the 4-field COSTGRID cell format
// (x:y:class:flags) unchanged while carrying enough to rebuild the engine's
// effective class.
//
// GLOBAL hook, fires per-neighbor (thousands/search): bails when no record is
// armed. The 4096-cell cap is enforced by AppendCostGridCell. Read-only:
// direct register + cached-stack/field reads only, no getter re-invocation, no
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

	// Fix I1: capture the per-search cost-class clamp condition (bVar10). This is
	// the exact byte the engine reads one instruction later (`mov al,[esp+0x12]`
	// at 0x429F5C); ESP is unchanged from the hook site, so GET_STACK offset 0x12
	// reads the identical slot. Per-search constant, so writing it on every cell
	// is harmless. See the block comment above for the full clamp semantics the
	// replay must apply.
	GET_STACK(BYTE, clampByte, 0x12);
	record->ClampActive = (clampByte != 0);

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

// RAW_CODES capture (VA 0x42A40F, inside FUN_00429A90, immediately after the
// reconstruction call `call 0x42AA90` at 0x42A406 returns; see
// astardump-offsets.md Step 4 "Reconstruction caller site - RAW_CODES post-fill,
// pre-smoothing"). This is the PRE-SMOOTHING direction-code path - the primary
// data the whole tool compares against the Rust port.
//
// TIMING (critical, same pattern as the Task 8/9 hooks): Syringe restores the
// patched original bytes AFTER the hook body runs, so at hook entry the patched
// instruction `mov edi,eax` (0x42A40F) has NOT executed yet. EDI therefore still
// holds its stale prior value; the reconstruction return value is still live in
// EAX (the engine copies it into EDI here precisely so it survives the two
// following smoothing calls). We MUST read the pointer from EAX, not EDI.
// Capstone (linear from 0x429A90):
//   0x42a406: call 0x42aa90               ; ret value (path-data struct ptr) -> EAX
//   0x42a40b: mov  ebx,[esp+0x68]  8b5c2468; reloads param_4, leaves EAX intact
//   0x42a40f: mov  edi,eax         8bf8    ; <-- hook site (patched)
//   0x42a411: push ebx             53
//   0x42a412: push edi             57
//   0x42a413: mov  ecx,esi         8bce
//   0x42a415: call 0x42b210               ; smoothing pass 1 - mutates buffer IN PLACE
//   0x42a41e: call 0x42b7f0               ; smoothing pass 2 - mutates buffer IN PLACE
// Patched byte count (0x6) ends on an instruction boundary: mov edi,eax (2) +
// push ebx (1) + push edi (1) + mov ecx,esi (2) = 6 bytes, landing exactly at
// 0x42A415 - BEFORE either smoothing call. Copying the buffer in this body
// therefore captures the raw, pre-smoothing codes; both smoothing passes read
// the same buffer (via struct+0xc) and overwrite it in place afterwards, so a
// hook placed at or after 0x42A415 would see smoothed data.
//
// EAX is the reconstruction return value = &DAT_0089a2d8, a fixed global
// path-data struct (FUN_0042aa90 `return &DAT_0089a2d8;`). LENGTH source
// (resolved from the decompile, not guessed): FUN_0042aa90 stores the path node
// count (`param_1[3]`) at struct+0x8 (_DAT_0089a2e0) and the direction-code
// output buffer pointer (`param_2`) at struct+0xc (_DAT_0089a2e4). The codes are
// 4-byte DWORD entries (undefined4*, values 0..8; delta->code LUT DAT_00818760),
// count = nodeCount - 1, with a 0xffffffff terminator written into the final
// slot (FUN_0042aa90 line 63). The 0x42a3f8 `cmp [eax+0xc],2 / jl` guard means
// reconstruction only runs for nodeCount >= 2, so codeCount >= 1 whenever this
// hook fires; the empty/zero-length branch below is defensive only.
//
// Fires per successful reconstruction (a failed FindPath never reaches 0x42A40F,
// so its record keeps the initialized RESULT="pending" - Task 11's exit hook
// finalizes pending->none). Like the COARSE_PATH hook, RawCodesCount is reset
// each fire so the record holds the LATEST reconstruction. Read-only: reads EAX
// + the path-data struct + the code buffer, no writes, no alloc, no re-invocation
// of the reconstruction or smoothing passes.
DEFINE_HOOK(0x42A40F, AStarClass_CoreSearch_AstarDumpRawCodes, 0x6)
{
	if (!AstarDump::Enable)
		return 0;

	AstarDump::Record* record = AstarDump::Current();
	if (!record)
		return 0;

	// Read the reconstruction return value from EAX (NOT EDI - see timing note).
	const uintptr_t pathData = R->EAX<uintptr_t>();
	if (!pathData)
		return 0;

	const int nodeCount = *reinterpret_cast<const int*>(pathData + 0x8);
	const int* buffer = *reinterpret_cast<const int* const*>(pathData + 0xc);

	// Refill for this (latest) reconstruction. RESULT=ok even for a zero-length
	// path -> a bare empty RAW_CODES= (RawCodesCount stays 0), which is what the
	// Rust parser expects (it splits on ',' and rejects the literal "empty").
	record->RawCodesCount = 0;

	int codeCount = nodeCount - 1;
	if (codeCount < 0)
		codeCount = 0;
	if (codeCount > MaxRawCodes)
		codeCount = MaxRawCodes; // never over-read past the capped copy

	if (buffer)
	{
		for (int i = 0; i < codeCount; ++i)
		{
			const int code = buffer[i];
			if (code == -1)
				break; // in-band 0xffffffff terminator (defensive; count is primary)

			AstarDump::AppendRawCode(record, code);
		}
	}

	std::strncpy(record->Result, "ok", sizeof(record->Result) - 1);

	return 0;
}

namespace
{
	// Task 11 finalization, shared by both FindPath-exit hook sites below.
	// Precondition: AstarDump::Enable is true and AstarDump::Current() is
	// non-null (both hook bodies check this before calling in).
	void FinalizeAndFlushCurrent()
	{
		AstarDump::Record* record = AstarDump::Current();

		// Promote the staged per-attempt corridor state (Task 9's
		// pendingCorridorActive, last written by whichever core-search attempt
		// ran most recently) to the record. For a successful search, the LAST
		// attempt IS the successful one - FindPath returns as soon as a
		// core-search attempt (FUN_00429A90) succeeds, with no further retries
		// after that, so "last attempt" and "successful attempt" coincide here.
		// For a failed search there is no successful attempt to prefer, so the
		// last-tried corridor state is still the most meaningful value to keep.
		record->CorridorActive = pendingCorridorActive;

		// RESULT finalization: the RAW_CODES hook (0x42A40F) only fires when
		// FUN_0042aa90's reconstruction actually runs, which only happens for a
		// successful core-search attempt. If Result is still "pending" here,
		// FindPath returned without ever reaching that hook - i.e. every
		// attempt failed (or none ran) - so the search result is "none". If
		// RESULT is already "ok", leave it: RAW_CODES already fired and filled
		// the record.
		if (std::strcmp(record->Result, "pending") == 0)
			std::strncpy(record->Result, "none", sizeof(record->Result) - 1);

		// Flush() is the module's ONLY disk-write trigger (see WriteRecord/
		// Flush above - grep confirms no other call site in this file invokes
		// Flush()): writing here, exactly once per FindPath return, guarantees
		// nothing is written to disk mid-search. Flush() also calls Reset(),
		// which already clears currentRecord/pendingCorridorActive, so the
		// explicit ClearCurrent() below is technically redundant with Reset()
		// but is kept as the documented, task-mandated exit-hook contract (and
		// stays correct if a future change makes Flush() skip Reset() on an
		// empty buffer, or skip clearing Current() specifically).
		AstarDump::Flush();
		AstarDump::ClearCurrent();
	}
}

// AStarClass::FindPath exit, early-out path (VA 0x42CB36..0x42CB3F; see
// docs/superpowers/notes/astardump-offsets.md "FindPath exit/return sites").
// This is the zone-type-mismatch `return NULL` (decompile line 117) - a FAILED
// search that does NOT fall through to the function's shared epilogue; it has
// its own physical `ret 0x1c` and must be hooked independently of the
// fall-through exit below.
//
// Capstone-verified linear disasm from 0x42cb36 (re-confirmed against the
// note, same run used to pin FindPath's stack ABI):
//   0x42cb36: pop edi        5f
//   0x42cb37: pop esi        5e
//   0x42cb38: pop ebp        5d
//   0x42cb39: xor eax,eax    33c0
//   0x42cb3b: pop ebx        5b
//   0x42cb3c: add esp,0x20   83c420
//   0x42cb3f: ret 0x1c       c21c00
// Patched byte count (0x5) ends on an instruction boundary: pop edi (1) + pop
// esi (1) + pop ebp (1) + xor eax,eax (2) = 5 bytes, landing exactly at
// 0x42cb3b (before `pop ebx`). Syringe re-executes these 5 patched bytes after
// the hook body returns, so all four instructions (including the
// caller-invisible `xor eax,eax` that produces this path's NULL return value)
// still run normally, followed by the untouched `pop ebx; add esp,0x20; ret
// 0x1c`. The hook body touches no register/flag this epilogue depends on, so
// this is safe regardless of where exactly inside the 5 bytes it's placed.
//
// No FindPath registers are read here (by design - see work item 1): the hook
// operates purely on AstarDump's own module state via Current(), which is
// valid for exactly the FindPath call currently unwinding, independent of
// which registers that call's own epilogue is mid-way through restoring.
DEFINE_HOOK(0x42CB36, AStarClass_FindPath_AstarDumpExitEarly, 0x5)
{
	if (!AstarDump::Enable)
		return 0;

	if (!AstarDump::Current())
		return 0;

	FinalizeAndFlushCurrent();

	return 0;
}

// AStarClass::FindPath exit, shared/fall-through epilogue (VA
// 0x42CCC4..0x42CCCB; see astardump-offsets.md). This is the function's single
// physical `ret 0x1c` reached by every OTHER return in FindPath (decompile
// lines 149/153/156, both the "search succeeded" and "search exhausted every
// corridor level and gave up" paths converge here via jumps/fallthrough) - it
// is a separate site from the early-out above, which never reaches this
// epilogue.
//
// Capstone-verified linear disasm from 0x42ccc4 (same continuous run as
// above, per the note):
//   0x42ccc4: pop edi        5f
//   0x42ccc5: pop esi        5e
//   0x42ccc6: pop ebp        5d
//   0x42ccc7: pop ebx        5b
//   0x42ccc8: add esp,0x20   83c420
//   0x42cccb: ret 0x1c       c21c00
// Patched byte count (0x7) ends on an instruction boundary: pop edi (1) + pop
// esi (1) + pop ebp (1) + pop ebx (1) + add esp,0x20 (3) = 7 bytes, landing
// exactly at 0x42cccb (the bare `ret 0x1c` itself, left untouched). A 5-byte
// patch would end mid-`add esp,0x20` (at cumulative offset 4, one byte short
// of that 3-byte instruction's boundary at 7), so 7 is the smallest hookable
// size that both fits the 5-byte jmp and lands cleanly - matching the note's
// "more bytes than the bare 3-byte ret" guidance. All five original
// instructions re-execute after the hook body returns, unaffected by it.
//
// Like the early-out hook, no FindPath registers are read; only the module's
// own Current()/pendingCorridorActive/record state is touched.
DEFINE_HOOK(0x42CCC4, AStarClass_FindPath_AstarDumpExitNormal, 0x7)
{
	if (!AstarDump::Enable)
		return 0;

	if (!AstarDump::Current())
		return 0;

	FinalizeAndFlushCurrent();

	return 0;
}

// ---------------------------------------------------------------------------
// Task 11 read-only audit (work item 4)
//
// This enumerates EVERY read the module's five engine hooks perform, to
// confirm the Global Constraint (direct field/register reads only; no virtual
// getter re-invoked for data; no RNG; no game-state writes) holds for the
// whole module, not just the hook it was originally documented in.
//
// AStarClass_FindPath_AstarDumpEntry (0x42C900):
//   - GET_STACK reads of the CALL's own pushed args: pStart [esp+0x4]
//     (CellStruct*), pDest [esp+0x8] (CellStruct*), pFoot [esp+0xc]
//     (FootClass*). Plain stack reads, no indirection through a vtable.
//   - pStart->X, pStart->Y, pDest->X, pDest->Y: direct CellStruct field
//     reads (POD struct, no accessor).
//   - pFoot->WhatAmI(): virtual call, but a pure RTTI/AbstractType query -
//     returns a compile-time-fixed enum tag per class, touches no simulation
//     state, and is not "data" in the sense the constraint means (it's the
//     same value every time for a given C++ class, computed with no reads of
//     mutable object state).
//   - pFoot->GetType(): virtual call, but ObjectClass::GetType() is a pure
//     accessor returning the object's (immutable, set-once-at-construction)
//     TechnoTypeClass*/UnitTypeClass* pointer - not a "getter that computes
//     over live data", just a stored-pointer accessor with no side effects.
//   - pType->Harvester, pType->SpeedType: direct field reads on the returned
//     UnitTypeClass* (rules-data, immutable at runtime).
//   - pFoot->UniqueID: direct field read.
//   - Unsorted::CurrentFrame: direct global read (existing SyncDump-adjacent
//     convention, not gated by this module).
//   No writes anywhere in this hook except the module's own AstarDump::Arm()
//   internal state and AstarDump::ClearCurrent().
//
// AStarClass_CoreSearch_AstarDumpAttempt (0x429A90):
//   - GET_STACK corridorActiveByte [esp+0x18]: plain stack read (param_7).
//   - R->ECX (AStarClass* this): plain register read.
//   - *(astar + 0xc74) (coarse-path count) and *(astar + 0xbc) (coarse-path
//     u16 buffer): direct field reads at fixed, already-pinned offsets on
//     `this` - no getter, no vtable dispatch.
//   No writes to AStarClass or any engine object; only AstarDump::Record
//   fields and the module-local pendingCorridorActive are written.
//
// AStarClass_CoreSearch_AstarDumpCostGrid (0x429F5A):
//   - R->EAX (cost class, the vtable-107 getter's ALREADY-COMPUTED return
//     value - read, not re-invoked) and R->EBX (CellClass*): plain register
//     reads. The getter itself is not called a second time; its result is
//     captured off the register it was about to be moved into.
//   - *(cell + 0x24), *(cell + 0x26) (CellClass::MapCoords.X/Y), *(cell +
//     0x140) (CellClass::Flags): direct field reads at fixed offsets, no
//     getter.
//   - GET_STACK [esp+0x12] (bVar10, the cost-class clamp condition, Fix I1):
//     plain stack read of a value the engine itself computed once per search -
//     not re-invoked here, just observed off the stack slot it already lives in.
//   No writes to CellClass or AStarClass; only AstarDump::Record fields.
//
// AStarClass_CoreSearch_AstarDumpRawCodes (0x42A40F):
//   - R->EAX (reconstruction's already-computed return value, &DAT_0089a2d8):
//     plain register read; the reconstruction function itself is NOT called
//     again.
//   - *(pathData + 0x8) (node count), *(pathData + 0xc) (code buffer
//     pointer), buffer[i] (int codes): direct field/array reads off the
//     already-populated, fixed global path-data struct.
//   No writes to the path-data struct or the code buffer (the two smoothing
//   calls that mutate it in place have NOT run yet at this hook's timing, and
//   this hook doesn't call them or touch the buffer itself); only
//   AstarDump::Record fields are written.
//
// AStarClass_FindPath_AstarDumpExitEarly / ...ExitNormal (0x42CB36, 0x42CCC4,
// this task):
//   - No FindPath register or stack read at all (by design, work item 1):
//     these hooks read only the module's own Current()/pendingCorridorActive
//     module-local state and Record fields already written by the other four
//     hooks. Result-string comparison (std::strcmp) and Flush()'s file I/O
//     are the only "action" here, and Flush() only touches AstarDump's own
//     DLL-owned static `records[]` buffer and the filesystem (ASTARDUMP\*.TXT)
//     - never the game/simulation state.
//
// Cross-cutting: no hook above calls a Randomizer/RNG routine, no hook writes
// any engine object field (AStarClass/FootClass/UnitTypeClass/CellClass), and
// every "getter" invoked (WhatAmI, GetType, the vtable-107 cost-class call
// upstream of the COSTGRID hook) is either a pure RTTI/static-data accessor or
// was already-called by the engine itself (its result captured off a
// register/struct field, never re-invoked by this module).
// ---------------------------------------------------------------------------
