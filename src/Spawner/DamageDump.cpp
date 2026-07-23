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

	// f32 as its raw IEEE bit pattern, so the host reconstructs the exact value
	// the engine's falloff held (a decimal round-trip would corrupt the x87 replay).
	unsigned int F32Bits(float v)
	{
		unsigned int u;
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
			"COLUMNS=frame,damage,armor,distance,cellspread,percentatmax,cap,mapflag,whnull,preverses,scaled,output\n");
		return true;
	}
}

void DamageDump::StageInputs(int frame, int damage, int armor, int distance,
	float cellspread, float percentatmax, int cap,
	bool mapNoDamage, bool warheadNull)
{
	Pending.frame = frame;
	Pending.damage = damage;
	Pending.armor = armor;
	Pending.distance = distance;
	Pending.cellspread = cellspread;
	Pending.percentatmax = percentatmax;
	Pending.cap = cap;
	Pending.mapNoDamage = mapNoDamage;
	Pending.warheadNull = warheadNull;
	// The positive-path hooks fill these in later; clear them so a heal / early-out
	// call (which never reaches those sites) emits them as absent.
	Pending.hasPreVerses = false;
	Pending.preVerses = 0;
	Pending.hasScaled = false;
	Pending.scaled = 0;
	++EntryCount;
}

void DamageDump::StagePreVerses(int preVerses)
{
	Pending.preVerses = preVerses;
	Pending.hasPreVerses = true;
}

void DamageDump::StageScaled(int scaled)
{
	Pending.scaled = scaled;
	Pending.hasScaled = true;
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

	// preVerses / scaled are absent on heal & early-out rows (the positive-path
	// hooks never fired); emit them as "-" so the host reads Option::None.
	char preBuf[16];
	char scaledBuf[16];
	if (Pending.hasPreVerses)
		std::sprintf(preBuf, "%d", Pending.preVerses);
	else
		std::strcpy(preBuf, "-");
	if (Pending.hasScaled)
		std::sprintf(scaledBuf, "%d", Pending.scaled);
	else
		std::strcpy(scaledBuf, "-");

	std::fprintf(pFile, "D=%d,%d,%d,%d,%08X,%08X,%d,%d,%d,%s,%s,%d\n",
		Pending.frame,
		Pending.damage,
		Pending.armor,
		Pending.distance,
		F32Bits(Pending.cellspread),
		F32Bits(Pending.percentatmax),
		Pending.cap,
		Pending.mapNoDamage ? 1 : 0,
		Pending.warheadNull ? 1 : 0,
		preBuf,
		scaledBuf,
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
//
// The patch window MUST be 6, not 5: the entry is
//   0x489180 sub esp,0xC   (3)
//   0x489183 push esi      (1)
//   0x489184 mov esi,ecx   (2)   <- a 5-byte window splits this instruction
// A 5-byte window ends mid-`mov esi,ecx`; Syringe copied the leftover byte
// verbatim and the trampoline executed a bare 0xF1, crashing at first combat
// (C0000005 in the trampoline). 6 steals three whole instructions, ending on
// the 0x489186 boundary. Stealing them is harmless -- they run in the
// trampoline after this hook returns 0, exactly as the original did.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x489180, MapClass_GetTotalDamage_DamageDumpEntry, 0x6)
{
	GET(int, damage, ECX);
	GET(WarheadTypeClass*, pWH, EDX);
	GET_STACK(int, armor, 0x4);
	GET_STACK(int, distance, 0x8);

	// CellSpread/PercentAtMax drive the STOCK x87 falloff (0x4891CE/0x4891D8),
	// which Ares leaves alone, so these are the real values the engine uses. Read
	// them at the engine's own offsets. We deliberately do NOT read the in-struct
	// Verses[] at +0xA0: it is dead (all-1.0 ctor default) because Ares does the
	// verses multiply from its own table -- see StagePreVerses/StageScaled and the
	// header. Offsets from the 0x489180 disassembly:
	//   CellSpread   = fld  [edi + 0x124]   (0x4891D8)
	//   PercentAtMax = fmul [edi + 0x12C]   (0x4891CE)
	float cellspread = 0.0f;
	float percentatmax = 0.0f;
	const bool warheadNull = (pWH == nullptr);
	if (!warheadNull && armor >= 0 && armor < 0xB)
	{
		const char* whBase = reinterpret_cast<const char*>(pWH);
		cellspread   = *reinterpret_cast<const float*>(whBase + 0x124);
		percentatmax = *reinterpret_cast<const float*>(whBase + 0x12C);
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
		cellspread, percentatmax, cap, mapNoDamage, warheadNull);
	return 0;
}

// POSITIVE-PATH INTERMEDIATES. Both sites are on the positive branch only, so
// heal / early-out calls never reach them and emit "-".
//
//   0x489227 (6) - ESI holds the stock falloff result (post-falloff, PRE the
//                  0x48922F/0x489233 max(0) clamp and pre-verses). Window is
//                  `xor ecx,ecx` (2) + `mov edx,[esp+0x18]` (4), ending on the
//                  0x48922D boundary where Phobos' NegativeDamageModifiers2
//                  (sz5) begins -- the previous 0x489229 sz6 site overlapped it
//                  and the paired JMPs would corrupt each other. 0x489227 is a
//                  jump target, but landing on the hook JMP itself is safe; a
//                  scan shows no live target strictly inside the window. This
//                  is BEFORE Ares' 0x489235 hook, so it is the genuine stock
//                  value the host checks bit-exact.
//   0x489249 (6) - EAX holds the post-verses value entering the cap clamp
//                  (`mov ecx,[0x8871E0]`, 6 bytes). This is AFTER Ares' verses
//                  multiply; if Ares returns past here it simply stays absent.
DEFINE_HOOK(0x489227, MapClass_GetTotalDamage_DamageDumpPreVerses, 0x6)
{
	GET(int, preVerses, ESI);
	DamageDump::StagePreVerses(preVerses);
	return 0;
}

DEFINE_HOOK(0x489249, MapClass_GetTotalDamage_DamageDumpScaled, 0x6)
{
	GET(int, scaled, EAX);
	DamageDump::StageScaled(scaled);
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
