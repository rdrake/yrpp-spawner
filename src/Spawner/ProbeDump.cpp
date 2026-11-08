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

#include "ProbeDump.h"

#include <AbstractClass.h>
#include <AbstractTypeClass.h>
#include <FootClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <ObjectClass.h>
#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

bool ProbeDump::Enable = false;
int ProbeDump::First = 0;
int ProbeDump::Last = -1;
unsigned int ProbeDump::Scanner = 0;

namespace
{
	constexpr char DumpDir[] = "PROBEDUMP";

	struct Row
	{
		int Frame;
		long Seq;
		char Site[8];
		unsigned int ScannerId;
		unsigned int CandId;
		unsigned int CandPtr;
		char Type[0x20];
		int Health;
		int Estimated;
		int OnMap;
		int InLimbo;
		int X, Y, Z;
		int A, B, C;
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
		std::sprintf(path, "%s\\PROBE_%08X.TXT", DumpDir, seed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\PROBE_%08X_%d.TXT", DumpDir, seed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[ProbeDump] Failed to open %s for write\n", path);
			return false;
		}
		std::fprintf(pFile, "PROBEDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", seed);
		std::fprintf(pFile, "WINDOW=%d..%d SCANNER=%u\n", ProbeDump::First, ProbeDump::Last, ProbeDump::Scanner);
		std::fprintf(pFile, "COLUMNS.P=frame,seq,site,scanner,cand,ptr,type,health,est,onmap,inlimbo,x,y,z,a,b,c\n");
		std::fprintf(pFile, "COLUMNS.F=frame,rows\n");
		Debug::Log("[ProbeDump] Opened %s\n", path);
		return true;
	}

	bool InWindow()
	{
		const int frame = Unsorted::CurrentFrame;
		return frame >= ProbeDump::First && frame <= ProbeDump::Last;
	}
}

void ProbeDump::Record(const char* site, const void* scanner, const void* candidate, int a, int b, int c, bool objectFields)
{
	auto const pScanner = static_cast<const ObjectClass*>(scanner);
	const unsigned int scannerId = pScanner ? pScanner->UniqueID : 0;
	if (Scanner != 0 && scannerId != Scanner)
		return;
	if (rowCount >= MaxRows)
	{
		if (!capLogged)
			Debug::Log("[ProbeDump] Row cap %ld hit; no longer recording\n", MaxRows);
		capLogged = true;
		return;
	}

	Row row {};
	row.Frame = Unsorted::CurrentFrame;
	row.Seq = rowCount++;
	std::strncpy(row.Site, site, sizeof(row.Site) - 1);
	row.ScannerId = scannerId;
	row.A = a;
	row.B = b;
	row.C = c;
	std::strcpy(row.Type, "<None>");
	if (candidate && !objectFields)
	{
		row.CandId = static_cast<const AbstractClass*>(candidate)->UniqueID;
		row.CandPtr = reinterpret_cast<unsigned int>(candidate);
	}
	else if (auto const pCand = static_cast<const ObjectClass*>(candidate))
	{
		row.CandId = pCand->UniqueID;
		row.CandPtr = reinterpret_cast<unsigned int>(pCand);
		if (auto const pType = pCand->GetType())
			std::strncpy(row.Type, pType->ID, sizeof(pType->ID) - 1);
		row.Health = pCand->Health;
		row.Estimated = pCand->EstimatedHealth;
		row.OnMap = pCand->IsOnMap ? 1 : 0;
		row.InLimbo = pCand->InLimbo ? 1 : 0;
		row.X = pCand->Location.X;
		row.Y = pCand->Location.Y;
		row.Z = pCand->Location.Z;
	}
	pending.push_back(row);
}

void ProbeDump::PerFrame()
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
		std::fprintf(pFile, "P=%d,%ld,%s,%u,%u,%08X,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			row.Frame, row.Seq, row.Site, row.ScannerId, row.CandId, row.CandPtr, row.Type,
			row.Health, row.Estimated, row.OnMap, row.InLimbo, row.X, row.Y, row.Z, row.A, row.B, row.C);
	}
	std::fprintf(pFile, "F=%d,%u\n", static_cast<int>(Unsorted::CurrentFrame), static_cast<unsigned int>(pending.size()));
	pending.clear();
	std::fflush(pFile);
}

// GreatestThreat tracker loop body, `mov ecx, [esi+0x21c]` (6 bytes).
DEFINE_HOOK(0x6F91EC, TechnoClass_GreatestThreat_ProbeTracker, 0x6)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(int, flags, 0x70);
	ProbeDump::Record("TRK", R->ESI<const void*>(), R->EBP<const void*>(), flags, 0);
	return 0;
}

// `mov al, [ebp+0x74]; test al, al` (5 bytes).
DEFINE_HOOK(0x6F9251, TechnoClass_GreatestThreat_ProbeOnMap, 0x5)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	ProbeDump::Record("ONMAP", R->ESI<const void*>(), R->EBP<const void*>(), 0, 0);
	return 0;
}

// `mov al, [esp+0x78]; test al, al` (6 bytes); EAX still holds InWhichLayer's return.
DEFINE_HOOK(0x6F926D, TechnoClass_GreatestThreat_ProbeLayer, 0x6)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(unsigned char, houseFilter, 0x78);
	ProbeDump::Record("LAYER", R->ESI<const void*>(), R->EBP<const void*>(), R->EAX<int>(), houseFilter);
	return 0;
}

// `mov ecx, [esp+0x2c]; mov edx, [esp+0x70]` (8 bytes).
DEFINE_HOOK(0x6F9292, TechnoClass_GreatestThreat_ProbeCall, 0x8)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	ProbeDump::Record("CALL", R->ESI<const void*>(), R->EBP<const void*>(), 0, 0);
	return 0;
}

// `mov eax, [esp+0x74]; mov ecx, [esp+0x1c]` (8 bytes).
DEFINE_HOOK(0x6F9363, TechnoClass_GreatestThreat_ProbeScore, 0x8)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(int, score, 0x74);
	GET_STACK(int, best, 0x1C);
	ProbeDump::Record("SCORE", R->ESI<const void*>(), R->EBP<const void*>(), score, best);
	return 0;
}

// The argument setup before CanAutoTargetObject's GetFireError call,
// `mov edx, [edi]; push ebx; push esi; mov ecx, edi` (6 bytes, ending at 0x6F7CE8).
DEFINE_HOOK(0x6F7CE2, TechnoClass_CanAutoTargetObject_ProbeFireError, 0x6)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(int, caller, 0x3C);
	ProbeDump::Record("FERR", R->EDI<const void*>(), R->ESI<const void*>(), 0, caller);
	return 0;
}

// `mov eax, [esi+0x70]; mov ebx, 2` (8 bytes).
DEFINE_HOOK(0x6F7CF7, TechnoClass_CanAutoTargetObject_ProbeEstimated, 0x8)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(int, caller, 0x3C);
	int vhpScan = -1;
	if (auto const pScanner = R->EDI<const ObjectClass*>())
		if (auto const pType = pScanner->GetTechnoType())
			vhpScan = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pType) + 0x394);
	ProbeDump::Record("EST", R->EDI<const void*>(), R->ESI<const void*>(), R->EAX<int>(), caller, vhpScan);
	return 0;
}

// GreatestThreat's shared return, `mov eax, [esp+0x20]; pop edi` (5 bytes).
DEFINE_HOOK(0x6F9DA1, TechnoClass_GreatestThreat_ProbeReturn, 0x5)
{
	if (!ProbeDump::Enable || !InWindow())
		return 0;
	GET_STACK(const void*, result, 0x20);
	GET_STACK(int, best, 0x1C);
	ProbeDump::Record("RET", R->ESI<const void*>(), result, best, 0, 0, false);
	return 0;
}

// Chained with the other dumps on the MainLoop-after-render point.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__ProbeDump, 0x5)
{
	ProbeDump::PerFrame();
	return 0;
}
