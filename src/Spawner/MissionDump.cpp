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

#include "MissionDump.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <AircraftClass.h>
#include <BuildingClass.h>
#include <FootClass.h>
#include <InfantryClass.h>
#include <MissionClass.h>
#include <UnitClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

bool MissionDump::Enable = false;
int MissionDump::MaxFrames = 0;
long MissionDump::RowCount = 0;

// ---------------------------------------------------------------------------
// Offset pins. ratwo's analysis names these as raw offsets off MissionClass;
// the emitter below uses the YRpp NAMED fields, and these asserts prove at
// compile time that the two agree. If YRpp ever disagrees with the analysis,
// the BUILD breaks - a wrong offset must never silently emit garbage that
// looks like data.
//
// The four ordinals (0xAC/0xB0/0xB4/0xBC) and 0xC0 are confirmed against
// reference/yrpp_struct_fields.csv; 0xC8/0xD0 are the TimerStruct pair, and
// YRpp carries its own offsetof(CDTimerClass, TimeLeft) == 0x8 assert, so the
// UpdateTimer base pin plus that one fixes +0xD0.
// ---------------------------------------------------------------------------
static_assert(offsetof(AbstractClass, UniqueID) == 0x10, "AbstractClass::UniqueID moved");
static_assert(offsetof(MissionClass, CurrentMission) == 0xAC, "MissionClass::CurrentMission moved");
static_assert(offsetof(MissionClass, SuspendedMission) == 0xB0, "MissionClass::SuspendedMission moved");
static_assert(offsetof(MissionClass, QueuedMission) == 0xB4, "MissionClass::QueuedMission moved");
static_assert(offsetof(MissionClass, MissionStatus) == 0xBC, "MissionClass::MissionStatus moved");
static_assert(offsetof(MissionClass, CurrentMissionStartTime) == 0xC0, "MissionClass::CurrentMissionStartTime moved");
static_assert(offsetof(MissionClass, MissionAccumulateTime) == 0xC4, "MissionClass::MissionAccumulateTime moved");
// UpdateTimer is a DECLARE_PROPERTY anonymous union, so this offsetof is on a
// union member of a non-standard-layout type - conditionally-supported by the
// standard, supported by MSVC, and non-standard-layout offsetof is already
// proven here (AnimDump asserts on AnimClass, which has virtuals).
static_assert(offsetof(MissionClass, UpdateTimer) == 0xC8, "MissionClass::UpdateTimer moved");
static_assert(offsetof(CDTimerClass, StartTime) == 0x0, "CDTimerClass::StartTime moved");
static_assert(offsetof(CDTimerClass, TimeLeft) == 0x8, "CDTimerClass::TimeLeft moved");

namespace
{
	constexpr char DumpDir[] = "MISSIONDUMP";
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
			Debug::Log("[MissionDump] Closing MISSION_%08X.TXT (%ld rows)\n", fileSeed, MissionDump::RowCount);
			std::fclose(pFile);
			pFile = nullptr;
		}
		fileOpenAttempted = false;
		MissionDump::RowCount = 0;
		capLogged = false;
		sinceFlush = 0;
	}

	// Open the per-session file the first time a row is emitted for a given
	// seed. The seed names the file, but a PINNED seed (HARNESS.Seed) repeats
	// across games and DAMAGEDUMP's silent overwrite on exactly that case has
	// already cost one capture - so never truncate an existing file: probe for
	// a free _<n> suffix instead (same policy as RngDump/AnimDump).
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
		std::sprintf(path, "%s\\MISSION_%08X.TXT", DumpDir, fileSeed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\MISSION_%08X_%d.TXT", DumpDir, fileSeed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[MissionDump] Failed to open %s for write\n", path);
			return false;
		}

		std::fprintf(pFile, "MISSIONDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", fileSeed);
		// Raw-offset provenance of each column, for the parser's benefit.
		// timer_start/time_left are the RAW STORED TimerStruct pair, NOT
		// GetTimeLeft() - see the header comment.
		std::fprintf(pFile,
			"OFFSETS=mission:AC,suspended:B0,queued:B4,status:BC,startframe:C0,accum:C4,"
			"timer_start:C8,time_left:D0,uid:10\n");
		// cat is I/U/A/B and idx is the object's position in that category's
		// engine array - the same ordinal Ares prints as #NNNNN. A within-frame
		// key only; ptr/uid are the cross-frame identity.
		std::fprintf(pFile,
			"COLUMNS.M=frame,cat,idx,ptr,uid,mission,queued,suspended,status,startframe,accum,"
			"timer_start,time_left\n");
		std::fprintf(pFile, "COLUMNS.F=frame,infantry,units,aircraft,buildings\n");
		Debug::Log("[MissionDump] Opened %s\n", path);
		return true;
	}

	// Shared gate for every row. Handles the two session boundaries exactly as
	// RngDump/AnimDump do: a re-seed (a new scenario is loading) and the frame
	// counter running backwards (a new game in the same process).
	bool BeginFrame(int frame)
	{
		if (!MissionDump::Enable)
			return false;

		if (pFile && static_cast<unsigned int>(Game::Seed) != fileSeed)
			CloseFile();
		else if (frame < lastFrame)
			CloseFile();
		lastFrame = frame;

		// Non-latching, so a frame counter left stale-high by the previous
		// game only suppresses rows for as long as it is actually stale.
		if (MissionDump::MaxFrames > 0 && frame > MissionDump::MaxFrames)
			return false;

		if (MissionDump::RowCount >= MissionDump::MaxRows)
		{
			if (!capLogged)
			{
				Debug::Log("[MissionDump] Row cap %ld hit at frame %d; no longer appending\n",
					MissionDump::MaxRows, frame);
				capLogged = true;
			}
			return false;
		}

		return EnsureFile();
	}

	// One object's row. Every read here is a plain field load off the
	// MissionClass sub-object; `pObj` is a TechnoClass-family pointer, and
	// MissionClass is an unambiguous single-inheritance base of all four
	// categories (mdisp 0), so the upcast is a no-op.
	void EmitRow(int frame, char cat, int idx, const MissionClass* pObj)
	{
		std::fprintf(pFile, "M=%d,%c,%d,%08X,%u,%d,%d,%d,%d,%d,%d,%d,%d\n",
			frame,
			cat,
			idx,
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pObj)),
			static_cast<unsigned int>(pObj->UniqueID),
			static_cast<int>(pObj->CurrentMission),
			static_cast<int>(pObj->QueuedMission),
			static_cast<int>(pObj->SuspendedMission),
			pObj->MissionStatus,
			pObj->CurrentMissionStartTime,
			pObj->MissionAccumulateTime,
			pObj->UpdateTimer.StartTime,
			pObj->UpdateTimer.TimeLeft);

		++MissionDump::RowCount;
		if (++sinceFlush >= FlushEvery)
		{
			std::fflush(pFile);
			sinceFlush = 0;
		}
	}

	// Iteration is the non-virtual begin()/end() pair (plain Items/Count field
	// reads) - deliberately NOT GetItem(), which is virtual and would dispatch
	// through the game-side vtable. Engine order, no sort, no copy: the array
	// position IS the ordinal Ares prints.
	template <typename T>
	int EmitArray(int frame, char cat, DynamicVectorClass<T*>& array)
	{
		int idx = 0;
		int written = 0;
		for (const T* pObj : array)
		{
			// A null slot still consumes its ordinal - Ares prints the array
			// position, so skipping one silently would shift every later row's
			// join key. Emit nothing but keep counting.
			if (pObj)
			{
				EmitRow(frame, cat, idx, pObj);
				++written;
			}
			++idx;
		}
		return written;
	}
}

void MissionDump::PerFrame()
{
	if (!Enable)
		return;

	const int frame = Unsorted::CurrentFrame;
	if (frame < 0)
		return;

	// Lazy open: the file appears the first time a frame actually carries an
	// object. Once open, every frame gets an F= heartbeat even at count 0, so
	// the parser can tell "no objects this frame" from "dump not running".
	const bool anyLive = InfantryClass::Array.Count > 0
		|| UnitClass::Array.Count > 0
		|| AircraftClass::Array.Count > 0
		|| BuildingClass::Array.Count > 0;
	if (!pFile && !anyLive)
		return;

	if (!BeginFrame(frame))
		return;

	// Category order mirrors the Ares listing order (Infantry, Units,
	// Aircraft, Buildings) so a consumer reading both in one pass sees the
	// same sequence.
	const int i = EmitArray(frame, 'I', InfantryClass::Array);
	const int u = EmitArray(frame, 'U', UnitClass::Array);
	const int a = EmitArray(frame, 'A', AircraftClass::Array);
	const int b = EmitArray(frame, 'B', BuildingClass::Array);

	std::fprintf(pFile, "F=%d,%d,%d,%d,%d\n", frame, i, u, a, b);
}

DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__MissionDump, 0x5)
{
	MissionDump::PerFrame();
	return 0;
}
