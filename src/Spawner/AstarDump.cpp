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

	return record;
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
