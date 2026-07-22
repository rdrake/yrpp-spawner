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

#include "DamageDump.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Fundamentals.h>
#include <RulesClass.h>
#include <Unsorted.h>
#include <WarheadTypeClass.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>

bool DamageDump::Enable = false;
long DamageDump::EntryCount = 0;
long DamageDump::ExitCount = 0;
DamageDump::PendingRec DamageDump::Pending = {};

namespace
{
	constexpr char DumpDir[] = "DAMAGEDUMP";
	constexpr int FlushEvery = 4096;

	FILE* pFile = nullptr;
	unsigned int fileSeed = 0;
	int lastFrame = -1;
	bool dirCreated = false;
	bool capLogged = false;
	int sinceFlush = 0;

	// f32/f64 as their raw IEEE bit patterns, so the host reconstructs the exact
	// value the engine held (a decimal round-trip would corrupt the x87 replay).
	unsigned int F32Bits(float v)
	{
		unsigned int u;
		std::memcpy(&u, &v, sizeof u);
		return u;
	}
	unsigned long long F64Bits(double v)
	{
		unsigned long long u;
		std::memcpy(&u, &v, sizeof u);
		return u;
	}

	void CloseFile()
	{
		if (pFile)
		{
			Debug::Log("[DamageDump] Closing DAMAGE_%08X.TXT (entry=%ld exit=%ld)\n",
				fileSeed, DamageDump::EntryCount, DamageDump::ExitCount);
			std::fclose(pFile);
			pFile = nullptr;
		}
	}

	// Open (truncating) the per-session file the first time a row is emitted in
	// a session, named by the game seed so a new game lands in a new file.
	bool EnsureFile()
	{
		if (pFile)
			return true;

		if (!dirCreated)
		{
			CreateDirectoryA(DumpDir, nullptr);
			dirCreated = true;
		}

		fileSeed = static_cast<unsigned int>(Game::Seed);
		char path[MAX_PATH];
		std::sprintf(path, "%s\\DAMAGE_%08X.TXT", DumpDir, fileSeed);
		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[DamageDump] Failed to open %s for write\n", path);
			return false;
		}
		std::fprintf(pFile, "DAMAGEDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", fileSeed);
		std::fprintf(pFile,
			"COLUMNS=frame,damage,armor,distance,verses,cellspread,percentatmax,cap,mapflag,whnull,output\n");
		return true;
	}
}

void DamageDump::StageInputs(int frame, int damage, int armor, int distance,
	double verses, float cellspread, float percentatmax, int cap,
	bool mapNoDamage, bool warheadNull)
{
	Pending.frame = frame;
	Pending.damage = damage;
	Pending.armor = armor;
	Pending.distance = distance;
	Pending.verses = verses;
	Pending.cellspread = cellspread;
	Pending.percentatmax = percentatmax;
	Pending.cap = cap;
	Pending.mapNoDamage = mapNoDamage;
	Pending.warheadNull = warheadNull;
	++EntryCount;
}

void DamageDump::Emit(int output)
{
	// New game within the same process: the frame counter runs backwards. Start
	// a fresh file and reset the invariant counters (same detection SyncDump and
	// CellDump use).
	if (Pending.frame < lastFrame)
	{
		CloseFile();
		EntryCount = 1; // the StageInputs for THIS call already ran
		ExitCount = 0;
		capLogged = false;
		sinceFlush = 0;
	}
	lastFrame = Pending.frame;

	++ExitCount;

	if (!Enable)
		return;

	if (ExitCount > MaxRows)
	{
		if (!capLogged)
		{
			Debug::Log("[DamageDump] Row cap %d hit; no longer appending\n", MaxRows);
			capLogged = true;
		}
		return;
	}

	if (!EnsureFile())
		return;

	std::fprintf(pFile, "D=%d,%d,%d,%d,%016llX,%08X,%08X,%d,%d,%d,%d\n",
		Pending.frame,
		Pending.damage,
		Pending.armor,
		Pending.distance,
		F64Bits(Pending.verses),
		F32Bits(Pending.cellspread),
		F32Bits(Pending.percentatmax),
		Pending.cap,
		Pending.mapNoDamage ? 1 : 0,
		Pending.warheadNull ? 1 : 0,
		output);

	if (++sinceFlush >= FlushEvery)
	{
		std::fflush(pFile);
		sinceFlush = 0;
		Debug::Log("[DamageDump] %ld rows (entry=%ld exit=%ld)\n",
			ExitCount, EntryCount, ExitCount);
	}
}

// ---------------------------------------------------------------------------
// Hooks. GetTotalDamage @0x489180 is __fastcall(int damage /*ECX*/,
// WarheadTypeClass* pWH /*EDX*/, Armor armor /*[ESP+4]*/, int distance
// /*[ESP+8]*/) and returns EAX. All addresses are gamemd-spawn.exe (the build
// the DLL injects into; gamemd.exe has a different layout at 0x489180).
//
// ENTRY: Syringe fires before the first instruction, so ECX/EDX hold the args
// and the stack is at entry state ([ESP+0]=retaddr, [ESP+4]=armor,
// [ESP+8]=distance -- confirmed from the disassembly's post-prologue 0x18/0x1c
// reads).
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x489180, MapClass_GetTotalDamage_DamageDumpEntry, 0x5)
{
	GET(int, damage, ECX);
	GET(WarheadTypeClass*, pWH, EDX);
	GET_STACK(int, armor, 0x4);
	GET_STACK(int, distance, 0x8);

	double verses = 0.0;
	float cellspread = 0.0f;
	float percentatmax = 0.0f;
	const bool warheadNull = (pWH == nullptr);
	if (!warheadNull && armor >= 0 && armor < 0xB)
	{
		verses = pWH->Verses[armor];
		cellspread = pWH->CellSpread;
		percentatmax = pWH->PercentAtMax;
	}

	// The function's own cap: RulesClass::Instance->MaxDamage (+0x16C8). Guarded
	// against a null singleton, though combat only runs after rules load.
	const int cap = RulesClass::Instance ? RulesClass::Instance->MaxDamage : 0;

	// The 0x00A8B230 "no damage / editor" flag: that address holds a pointer,
	// and the function tests bit 0x20 of the byte it points at (disassembly
	// 0x489195: mov eax,[0xA8B230]; 0x48919A: test byte [eax],0x20).
	bool mapNoDamage = false;
	if (const unsigned char* pFlag = *reinterpret_cast<unsigned char**>(0x00A8B230))
		mapNoDamage = (*pFlag & 0x20) != 0;

	DamageDump::StageInputs(Unsorted::CurrentFrame, damage, armor, distance,
		verses, cellspread, percentatmax, cap, mapNoDamage, warheadNull);
	return 0;
}

// EXIT: three return points, each hooked at a site where EAX already holds the
// result and the 5+-byte patch window ends on an instruction boundary with no
// inbound jump (the ret-adjacent instructions are 1-3 bytes, so two sites need a
// 7-byte window). All share one handler.
//
//   0x48926A (5) - early-outs (EAX=0 via the 0x489263 xor) and the UNCAPPED
//                  positive result (reached by 0x489257 jl 0x489265).
//   0x48925C (7) - CAPPED positive result (EAX=MaxDamage, set by 0x48925A
//                  mov eax,ecx); window 0x48925C..0x489263.
//   0x4891BF (7) - the heal branch (damage<0; EAX set by 0x4891BD and eax,esi);
//                  window 0x4891BF..0x4891C6.
//
// The unsafe naive sites (the rets at 0x489260 and 0x4891C3) are NOT hooked:
// their 5-byte windows contain the live jump targets 0x489263 and 0x4891C6.
DEFINE_HOOK(0x48926A, MapClass_GetTotalDamage_DamageDumpExitMain, 0x5)
{
	GET(int, output, EAX);
	DamageDump::Emit(output);
	return 0;
}

DEFINE_HOOK(0x48925C, MapClass_GetTotalDamage_DamageDumpExitCapped, 0x7)
{
	GET(int, output, EAX);
	DamageDump::Emit(output);
	return 0;
}

DEFINE_HOOK(0x4891BF, MapClass_GetTotalDamage_DamageDumpExitHeal, 0x7)
{
	GET(int, output, EAX);
	DamageDump::Emit(output);
	return 0;
}
