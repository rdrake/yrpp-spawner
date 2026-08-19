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

#include "RngDump.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Fundamentals.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

bool RngDump::Enable = false;
int RngDump::MaxFrames = 0;
long RngDump::RowCount = 0;

namespace
{
	constexpr char DumpDir[] = "RNGDUMP";
	constexpr int FlushEvery = 8192;

	// Randomizer layout (yrpp_struct_fields.csv): +0x0 bool "deterministic /
	// disabled" flag, +0x4 Next1, +0x8 Next2, +0xC u4 Table[250].
	constexpr int OffFlag = 0x0;
	constexpr int OffNext1 = 0x4;
	constexpr int OffNext2 = 0x8;

	// The two global generators seeded by Unsorted::InitRandom @0x52fc20. GenA
	// lives inside the object 0x00A8B230 points at; GenB is a plain static.
	constexpr uintptr_t GenAOwner = 0x00A8B230u;
	constexpr uintptr_t GenAOffset = 0x218u;
	constexpr uintptr_t GenB = 0x00886B88u;

	FILE* pFile = nullptr;
	unsigned int fileSeed = 0;
	bool fileOpenAttempted = false;
	int lastFrame = -1;
	bool dirCreated = false;
	bool capLogged = false;
	int sinceFlush = 0;

	const void* GenAddrA()
	{
		if (const unsigned char* pOwner = *reinterpret_cast<unsigned char**>(GenAOwner))
			return pOwner + GenAOffset;
		return nullptr;
	}

	int IndexAt(const void* self, int offset)
	{
		return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(self) + offset);
	}

	void CloseFile()
	{
		if (pFile)
		{
			Debug::Log("[RngDump] Closing RNG_%08X.TXT (%ld rows)\n", fileSeed, RngDump::RowCount);
			std::fclose(pFile);
			pFile = nullptr;
		}
		fileOpenAttempted = false;
		RngDump::RowCount = 0;
		capLogged = false;
		sinceFlush = 0;
	}

	// Open the per-session file the first time a row is emitted for a given seed.
	//
	// The seed names the file (as SyncDump/DamageDump do), but a PINNED seed
	// (HARNESS.Seed) repeats across games, and DAMAGEDUMP's silent overwrite on
	// exactly that case has already cost one capture. So never truncate an
	// existing file: probe for a free _<n> suffix instead.
	bool EnsureFile()
	{
		if (pFile)
			return true;
		// One open attempt per session: a failed open must not be retried on
		// every draw (that would hammer the disk from inside a hot hook).
		if (fileOpenAttempted)
			return false;
		fileOpenAttempted = true;

		if (!dirCreated)
		{
			CreateDirectoryA(DumpDir, nullptr);
			dirCreated = true;
		}

		fileSeed = static_cast<unsigned int>(Game::Seed);

		char path[MAX_PATH];
		std::sprintf(path, "%s\\RNG_%08X.TXT", DumpDir, fileSeed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\RNG_%08X_%d.TXT", DumpDir, fileSeed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[RngDump] Failed to open %s for write\n", path);
			return false;
		}

		const void* pA = GenAddrA();
		std::fprintf(pFile, "RNGDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", fileSeed);
		std::fprintf(pFile, "GENA=%08X\n", static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pA)));
		std::fprintf(pFile, "GENB=%08X\n", static_cast<unsigned int>(GenB));
		std::fprintf(pFile, "COLUMNS.R=frame,caller,self,next1,next2,min,max\n");
		std::fprintf(pFile, "COLUMNS.S=frame,caller,self,next1,next2\n");
		std::fprintf(pFile, "COLUMNS.C=frame,self,next1,next2,flag\n");
		Debug::Log("[RngDump] Opened %s (GenA=%08X GenB=%08X)\n", path,
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pA)),
			static_cast<unsigned int>(GenB));
		return true;
	}

	// Shared gate for every row type. Returns false when this row must be
	// dropped. Handles the two session boundaries: a re-seed (Unsorted::InitRandom
	// ran, so a new scenario is loading) and the frame counter running backwards
	// (a new game in the same process, the detection SyncDump/CellDump use).
	//
	// Rotating on the seed is what makes the SCENARIO-LOAD PREAMBLE capturable:
	// seeding happens before the load draws, so the fresh file starts exactly at
	// the boundary and menu-time draws stay behind in the previous file.
	bool BeginRow(int frame)
	{
		if (!RngDump::Enable)
			return false;

		if (pFile && static_cast<unsigned int>(Game::Seed) != fileSeed)
			CloseFile();
		else if (frame < lastFrame)
			CloseFile();
		lastFrame = frame;

		// Non-latching, so a frame counter left stale-high by the previous game
		// only suppresses rows for as long as it is actually stale.
		if (RngDump::MaxFrames > 0 && frame > RngDump::MaxFrames)
			return false;

		if (RngDump::RowCount >= RngDump::MaxRows)
		{
			if (!capLogged)
			{
				Debug::Log("[RngDump] Row cap %ld hit at frame %d; no longer appending\n",
					RngDump::MaxRows, frame);
				capLogged = true;
			}
			return false;
		}

		return EnsureFile();
	}

	void EndRow()
	{
		++RngDump::RowCount;
		if (++sinceFlush >= FlushEvery)
		{
			std::fflush(pFile);
			sinceFlush = 0;
			Debug::Log("[RngDump] %ld rows\n", RngDump::RowCount);
		}
	}
}

void RngDump::RecordRanged(unsigned int caller, const void* self, int nMin, int nMax)
{
	if (!self)
		return;
	const int frame = Unsorted::CurrentFrame;
	if (!BeginRow(frame))
		return;

	std::fprintf(pFile, "R=%d,%08X,%08X,%d,%d,%d,%d\n",
		frame,
		caller,
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(self)),
		IndexAt(self, OffNext1),
		IndexAt(self, OffNext2),
		nMin,
		nMax);
	EndRow();
}

void RngDump::RecordRaw(unsigned int caller, const void* self)
{
	if (!self)
		return;
	const int frame = Unsorted::CurrentFrame;
	if (!BeginRow(frame))
		return;

	std::fprintf(pFile, "S=%d,%08X,%08X,%d,%d\n",
		frame,
		caller,
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(self)),
		IndexAt(self, OffNext1),
		IndexAt(self, OffNext2));
	EndRow();
}

void RngDump::PerFrame()
{
	// Checkpoints only bracket an existing trace; they must never be the thing
	// that opens a file (that would fill RNGDUMP\ with menu-time noise).
	if (!Enable || !pFile)
		return;

	const int frame = Unsorted::CurrentFrame;
	const void* gens[2] = { GenAddrA(), reinterpret_cast<const void*>(GenB) };
	for (const void* self : gens)
	{
		if (!self)
			continue;
		if (!BeginRow(frame))
			return;
		std::fprintf(pFile, "C=%d,%08X,%d,%d,%d\n",
			frame,
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(self)),
			IndexAt(self, OffNext1),
			IndexAt(self, OffNext2),
			*reinterpret_cast<const unsigned char*>(reinterpret_cast<const char*>(self) + OffFlag) ? 1 : 0);
		EndRow();
	}
}

// ---------------------------------------------------------------------------
// Hooks. All addresses are gamemd-spawn.exe (the build the DLL injects into).
// Both generator entry points are __thiscall, so `this` is in ECX and the stack
// is at entry state: [ESP+0] = the caller's return address, i.e. the caller EIP
// this hook exists to record.
//
// Randomizer::RandomRanged @0x65c7e0, window 6:
//   0x65c7e0 mov eax,[esp+4]   (4)   <- nMin
//   0x65c7e4 mov edx,ecx       (2)
//   0x65c7e6 mov ecx,[esp+8]         <- nMax; first instruction NOT stolen
// 6 steals two whole instructions and ends on the 0x65c7e6 boundary. The only
// inbound control flow is the function entry itself; the body's internal jumps
// target 0x65c7fb and later, so nothing lands strictly inside the window.
//
// Randomizer::Random @0x65c780, window 5:
//   0x65c780 cmp byte [ecx],0  (3)
//   0x65c783 je 0x65c788       (2)
// 5 ends on the 0x65c785 boundary. 0x65c788 (the je target) is outside it.
//
// Phobos hooks 0x65c7d0 and 0x65c88a in this same pair of functions. 0x65c7d0
// is Random's `ret`, followed by 15 bytes of NOP padding before 0x65c7e0, so no
// window sited there can reach RandomRanged's entry; 0x65c88a is in
// RandomRanged's tail, far past our window. Verified byte-for-byte against the
// instruction stream, the lesson of the DamageDump/Phobos overlap.
//
// Read-only: the handlers read registers, the stack and the generator's index
// fields, and write a file. They never touch the Table, the indices, or any
// other game state, so the frame CRC is unchanged.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x65c7e0, Randomizer_RandomRanged_RngDump, 0x6)
{
	if (!RngDump::Enable)
		return 0;

	GET(const void*, self, ECX);
	GET_STACK(unsigned int, caller, 0x0);
	GET_STACK(int, nMin, 0x4);
	GET_STACK(int, nMax, 0x8);

	RngDump::RecordRanged(caller, self, nMin, nMax);
	return 0;
}

DEFINE_HOOK(0x65c780, Randomizer_Random_RngDump, 0x5)
{
	if (!RngDump::Enable)
		return 0;

	GET(const void*, self, ECX);
	GET_STACK(unsigned int, caller, 0x0);

	RngDump::RecordRaw(caller, self);
	return 0;
}

// Chained with SyncDump/CellDump/ProtocolZero on the MainLoop-after-render point.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__RngDump, 0x5)
{
	RngDump::PerFrame();

	return 0;
}
