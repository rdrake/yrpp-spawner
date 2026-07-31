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

#include "AnimDump.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <AnimClass.h>
#include <AnimTypeClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

bool AnimDump::Enable = false;
int AnimDump::MaxFrames = 0;
long AnimDump::RowCount = 0;

// ---------------------------------------------------------------------------
// Offset pins. The analysis notes name raw offsets ([this+0xCC] owner,
// [+0x9C..0xA4] Location, [+0x81] limbo, [type+0x33C] area-loop bound); the
// emitter below uses the YRpp NAMED fields, and these asserts prove at compile
// time that the two agree. If YRpp ever disagrees with a note, the BUILD
// breaks - a wrong offset must never silently emit garbage that looks like
// data. All confirmed against YRpp as of this commit:
//   * AnimClass::OwnerObject      == +0xCC (note agreed)
//   * ObjectClass::Location       == +0x9C/+0xA0/+0xA4 (note agreed)
//   * ObjectClass::InLimbo        == +0x81 (note agreed)
//   * AbstractClass::UniqueID     == +0x10
//   * AnimTypeClass+0x33C is named TiberiumSpreadRadius in YRpp - the notes
//     call it only "the loop bound of Update's terminal area loop"; the two
//     readings are consistent (a spread radius bounding a nested x/y area
//     walk) but the NAME comes from YRpp, not from the note.
// ---------------------------------------------------------------------------
static_assert(offsetof(AbstractClass, UniqueID) == 0x10, "AbstractClass::UniqueID moved");
static_assert(offsetof(ObjectClass, InLimbo) == 0x81, "ObjectClass::InLimbo moved");
static_assert(offsetof(ObjectClass, Location) == 0x9C, "ObjectClass::Location moved");
static_assert(offsetof(AnimClass, Type) == 0xC8, "AnimClass::Type moved");
static_assert(offsetof(AnimClass, OwnerObject) == 0xCC, "AnimClass::OwnerObject moved");
static_assert(offsetof(AnimTypeClass, ArrayIndex) == 0x294, "AnimTypeClass::ArrayIndex moved");
static_assert(offsetof(AnimTypeClass, End) == 0x2C0, "AnimTypeClass::End moved");
static_assert(offsetof(AnimTypeClass, TiberiumSpreadRadius) == 0x33C, "AnimTypeClass+0x33C is not TiberiumSpreadRadius");

namespace
{
	constexpr char DumpDir[] = "ANIMDUMP";
	constexpr int FlushEvery = 8192;

	FILE* pFile = nullptr;
	unsigned int fileSeed = 0;
	bool fileOpenAttempted = false;
	int lastFrame = -1;
	bool dirCreated = false;
	bool capLogged = false;
	int sinceFlush = 0;

	void CloseFile()
	{
		if (pFile)
		{
			Debug::Log("[AnimDump] Closing ANIM_%08X.TXT (%ld rows)\n", fileSeed, AnimDump::RowCount);
			std::fclose(pFile);
			pFile = nullptr;
		}
		fileOpenAttempted = false;
		AnimDump::RowCount = 0;
		capLogged = false;
		sinceFlush = 0;
	}

	// Open the per-session file the first time a row is emitted for a given
	// seed. The seed names the file, but a PINNED seed (HARNESS.Seed) repeats
	// across games and DAMAGEDUMP's silent overwrite on exactly that case has
	// already cost one capture - so never truncate an existing file: probe for
	// a free _<n> suffix instead (same policy as RngDump).
	bool EnsureFile()
	{
		if (pFile)
			return true;
		// One open attempt per session: a failed open must not be retried
		// every frame.
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
		std::sprintf(path, "%s\\ANIM_%08X.TXT", DumpDir, fileSeed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\ANIM_%08X_%d.TXT", DumpDir, fileSeed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[AnimDump] Failed to open %s for write\n", path);
			return false;
		}

		std::fprintf(pFile, "ANIMDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", fileSeed);
		// Raw-offset provenance of each column, for the parser's benefit.
		// ptr/uid are the stable instance identity; owner is the raw
		// [this+0xCC] pointer (00000000 = detached, and then ox/oy/oz are
		// meaningless and emitted as 0); lx/ly/lz are the RAW stored Location
		// [+0x9C..0xA4], NOT GetCoords(); limbo is [+0x81]; stage is
		// Animation.Value; tibrad is AnimTypeClass+0x33C.
		std::fprintf(pFile, "OFFSETS=owner:CC,location:9C.A0.A4,limbo:81,uid:10,type:C8,tibrad:33C,end:2C0\n");
		std::fprintf(pFile, "COLUMNS.A=frame,ptr,uid,typeidx,type,lx,ly,lz,owner,ox,oy,oz,limbo,stage,tibrad,end,loop\n");
		std::fprintf(pFile, "COLUMNS.F=frame,count\n");
		Debug::Log("[AnimDump] Opened %s\n", path);
		return true;
	}

	// Shared gate for every row. Handles the two session boundaries exactly as
	// RngDump does: a re-seed (a new scenario is loading) and the frame counter
	// running backwards (a new game in the same process).
	bool BeginRow(int frame)
	{
		if (!AnimDump::Enable)
			return false;

		if (pFile && static_cast<unsigned int>(Game::Seed) != fileSeed)
			CloseFile();
		else if (frame < lastFrame)
			CloseFile();
		lastFrame = frame;

		// Non-latching, so a frame counter left stale-high by the previous
		// game only suppresses rows for as long as it is actually stale.
		if (AnimDump::MaxFrames > 0 && frame > AnimDump::MaxFrames)
			return false;

		if (AnimDump::RowCount >= AnimDump::MaxRows)
		{
			if (!capLogged)
			{
				Debug::Log("[AnimDump] Row cap %ld hit at frame %d; no longer appending\n",
					AnimDump::MaxRows, frame);
				capLogged = true;
			}
			return false;
		}

		return EnsureFile();
	}

	void EndRow()
	{
		++AnimDump::RowCount;
		if (++sinceFlush >= FlushEvery)
		{
			std::fflush(pFile);
			sinceFlush = 0;
		}
	}
}

void AnimDump::PerFrame()
{
	if (!Enable)
		return;

	const int frame = Unsorted::CurrentFrame;
	if (frame < 0)
		return;

	// Lazy open: the file appears the first time a frame actually carries an
	// anim, so a session that never spawns one leaves no file behind. Once
	// open, every frame gets an F= heartbeat even at count 0, so the parser
	// can tell "no anims this frame" from "dump not running".
	//
	// Iteration is the non-virtual begin()/end() pair (plain Items/Count field
	// reads) - deliberately NOT GetItem(), which is virtual and would dispatch
	// through the game-side vtable. Engine order, no sort, no copy.
	auto& array = AnimClass::Array;
	if (!pFile && array.Count <= 0)
		return;

	int written = 0;
	for (const AnimClass* pAnim : array)
	{
		if (!pAnim)
			continue;
		if (!BeginRow(frame))
			return;

		// All reads below are plain field loads - no virtual call, no inline
		// JMP_THIS thunk, nothing that could reach either Randomizer entry
		// point or write a game-state byte.
		const AnimTypeClass* pType = pAnim->Type;
		const ObjectClass* pOwner = pAnim->OwnerObject;
		// OwnerObject is kept valid by the engine's pointer-expiry protocol
		// (AnimClass::PointerExpired @0x425150 nulls it when the owner dies),
		// so at the after-render point it is either null or live.
		const CoordStruct ownerLoc = pOwner ? pOwner->Location : CoordStruct { 0, 0, 0 };

		std::fprintf(pFile, "A=%d,%08X,%u,%d,%.24s,%d,%d,%d,%08X,%d,%d,%d,%d,%d,%d,%d,%d\n",
			frame,
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pAnim)),
			static_cast<unsigned int>(pAnim->UniqueID),
			pType ? pType->ArrayIndex : -1,
			pType ? pType->ID : "-",
			pAnim->Location.X,
			pAnim->Location.Y,
			pAnim->Location.Z,
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pOwner)),
			ownerLoc.X,
			ownerLoc.Y,
			ownerLoc.Z,
			pAnim->InLimbo ? 1 : 0,
			pAnim->Animation.Value,
			pType ? pType->TiberiumSpreadRadius : -1,
			pType ? pType->End : -1,
			pType ? pType->LoopCount : -1);
		EndRow();
		++written;
	}

	// Per-frame closing bracket (only once the file exists - it must never be
	// the thing that opens one, same rule as RngDump's checkpoint).
	if (pFile && BeginRow(frame))
	{
		std::fprintf(pFile, "F=%d,%d\n", frame, written);
		EndRow();
	}
}

// Chained with SyncDump/CellDump/RngDump/ProtocolZero on the MainLoop-after-
// render point; fires once per logic frame.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__AnimDump, 0x5)
{
	AnimDump::PerFrame();
	return 0;
}
