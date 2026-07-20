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
		Debug::Log("[AstarDump] Record buffer full (MaxRecords=%d), dropping frame=%d unit=0x%08x\n",
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

		fprintf(pFile, "COSTGRID=");
		if (record.CostGridCount == 0)
		{
			fprintf(pFile, "empty");
		}
		else
		{
			for (int i = 0; i < record.CostGridCount; ++i)
			{
				const AstarDump::CostGridCell& cell = record.CostGrid[i];
				fprintf(pFile, "%s%d:%d:%d:%d", (i == 0) ? "" : ";", cell.X, cell.Y, cell.Cost, cell.Flags);
			}
		}
		fprintf(pFile, "\n");

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
