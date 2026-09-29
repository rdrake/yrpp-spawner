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

#include "UidDump.h"
#include "WatchEngine.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>
#include <vector>

bool UidDump::Enable = false;
int UidDump::MaxFrames = 0;

namespace
{
	constexpr char DumpDir[] = "UIDDUMP";
	constexpr unsigned int AssignerReturn = 0x41024B;

	struct Row
	{
		int Frame;
		unsigned int Id;
		unsigned int Path;
		unsigned int Caller;
		unsigned int Object;
		unsigned int Vtable;
		unsigned int Candidates[WatchEngine::MaxCandidates];
	};

	std::vector<Row> pending;
	FILE* pFile = nullptr;
	bool fileOpenAttempted = false;
	long rowCount = 0;
	bool capLogged = false;

	bool EnsureFile()
	{
		if (pFile)
			return true;
		if (fileOpenAttempted)
			return false;
		fileOpenAttempted = true;

		CreateDirectoryA(DumpDir, nullptr);
		const unsigned int seed = static_cast<unsigned int>(Game::Seed);
		char path[MAX_PATH];
		std::sprintf(path, "%s\\UID_%08X.TXT", DumpDir, seed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\UID_%08X_%d.TXT", DumpDir, seed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[UidDump] Failed to open %s for write\n", path);
			return false;
		}
		std::fprintf(pFile, "UIDDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", seed);
		// path is the allocator's return address: 0041024B = the object
		// assigner (caller exact, object and vtable filled), anything else =
		// another caller (caller/object/vtable 0). c1..c4 are deeper stack
		// candidates, nearest first.
		std::fprintf(pFile, "COLUMNS.U=frame,id,path,caller,object,vtable,c1,c2,c3,c4\n");
		std::fprintf(pFile, "COLUMNS.F=frame,rows\n");
		Debug::Log("[UidDump] Opened %s\n", path);
		return true;
	}
}

void UidDump::Record(unsigned int esp, unsigned int newId)
{
	const int frame = Unsorted::CurrentFrame;
	if (MaxFrames > 0 && frame > MaxFrames)
		return;
	if (rowCount >= MaxRows)
	{
		if (!capLogged)
			Debug::Log("[UidDump] Row cap %ld hit; no longer recording\n", MaxRows);
		capLogged = true;
		return;
	}
	++rowCount;

	const unsigned int* stack = reinterpret_cast<const unsigned int*>(esp);
	Row row {};
	row.Frame = frame;
	row.Id = newId;
	row.Path = stack[0];
	unsigned int scanFrom = esp + 4;
	if (row.Path == AssignerReturn)
	{
		row.Caller = stack[1];
		row.Object = stack[2] - 4;
		row.Vtable = *reinterpret_cast<const unsigned int*>(row.Object);
		scanFrom = esp + 12;
	}
	WatchEngine::ReturnCandidates(scanFrom, row.Candidates);
	pending.push_back(row);
}

void UidDump::PerFrame()
{
	if (!Enable || (pending.empty() && !pFile))
		return;
	if (!EnsureFile())
	{
		pending.clear();
		return;
	}
	for (const Row& row : pending)
	{
		std::fprintf(pFile, "U=%d,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
			row.Frame, row.Id, row.Path, row.Caller, row.Object, row.Vtable,
			row.Candidates[0], row.Candidates[1], row.Candidates[2], row.Candidates[3]);
	}
	std::fprintf(pFile, "F=%d,%u\n", static_cast<int>(Unsorted::CurrentFrame), static_cast<unsigned int>(pending.size()));
	pending.clear();
	std::fflush(pFile);
}

// The allocator's first instruction, `mov eax, [ecx+0x214]` (6 bytes, no
// relative operand, so Syringe can relocate it).
DEFINE_HOOK(0x68BCB0, ScenarioClass_NewUniqueID_UidDump, 0x6)
{
	if (!UidDump::Enable)
		return 0;

	GET(const unsigned char*, pScenario, ECX);
	const unsigned int current = *reinterpret_cast<const unsigned int*>(pScenario + 0x214);
	UidDump::Record(R->ESP(), current + 1);
	return 0;
}

// Chained with the other dumps on the MainLoop-after-render point.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__UidDump, 0x5)
{
	UidDump::PerFrame();
	return 0;
}
