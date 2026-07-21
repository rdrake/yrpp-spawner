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

#include "CellDump.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <CellClass.h>
#include <MapClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>

bool CellDump::Enable = false;
int CellDump::Frames[CellDump::MaxDumpFrames] = {};
int CellDump::FrameCount = 0;

namespace
{
	constexpr char DumpDir[] = "CELLDUMP";

	// Per-session bookkeeping. dumpedMask bit i is set once Frames[i] has been
	// handled (dumped, or skipped with a log) so a frame never dumps twice in
	// one session; everything resets when the frame counter runs backwards (a
	// new game started within the same process - same detection SyncDump uses).
	unsigned int dumpedMask = 0;
	int lastSeenFrame = 0;
	bool dirCreated = false;

	void WriteDump(int frame)
	{
		MapClass& map = MapClass::Instance;

		// Both compact caches are allocated at scenario/map init. A dump frame
		// that fires before that (or after teardown) has nothing coherent to
		// serialize - log and skip rather than chase null/stale pointers.
		const CellLevelPassabilityStruct* levelPass = map.LevelAndPassability;
		const LevelAndPassabilityStruct2* subzones = map.LevelAndPassabilityStruct2pointer_70;
		if (!levelPass || !subzones)
		{
			Debug::Log("[CellDump] Frame %d: pathfinding caches not allocated, skipping dump\n", frame);
			return;
		}

		// Compact-cache index base (FUN_0056d430): stride * y + x with
		// stride = MapRect.Width + MapRect.Height + 1; entries are valid on
		// [0, ValidMapCellCount) (the FUN_0056d3f0 clamp bound).
		const int stride = map.MapRect.Width + map.MapRect.Height + 1;
		const int validCells = map.ValidMapCellCount;

		if (!dirCreated)
		{
			CreateDirectoryA(DumpDir, nullptr);
			dirCreated = true;
		}

		char path[MAX_PATH];
		sprintf(path, "%s\\CELL_%d.TXT", DumpDir, frame);
		FILE* pFile = fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[CellDump] Failed to open %s for write\n", path);
			return;
		}

		fprintf(pFile, "CELLDUMP=1\n");
		fprintf(pFile, "FRAME=%d\n", frame);
		fprintf(pFile, "SEED=%08X\n", Game::Seed);
		fprintf(pFile, "MAPRECT=%d,%d,%d,%d\n",
			map.MapRect.X, map.MapRect.Y, map.MapRect.Width, map.MapRect.Height);
		fprintf(pFile, "STRIDE=%d\n", stride);
		fprintf(pFile, "VALIDCELLS=%d\n", validCells);
		// Column key for the C= lines below. occ/altocc/flags/altflags are
		// HEX (no 0x prefix - same convention as ASTARDUMP's COSTGRID flags);
		// every other column is signed decimal. cpass/clevel/czai come from
		// the 4-byte CellLevelPassabilityStruct cache; sz0/sz1/sz2 (subzone id
		// per level), szzai and szlevel from the 10-byte subzone cache. A cell
		// whose compact index falls outside [0, VALIDCELLS) emits -1 for all
		// seven cache columns.
		fprintf(pFile, "COLUMNS=x,y,pass,land,level,height,slope,tile,overlay,odata,"
			"occ,altocc,flags,altflags,cpass,clevel,czai,sz0,sz1,sz2,szzai,szlevel\n");

		int written = 0;
		for (int y = 0; y < 512; ++y)
		{
			for (int x = 0; x < 512; ++x)
			{
				CellStruct coords;
				coords.X = static_cast<short>(x);
				coords.Y = static_cast<short>(y);
				const CellClass* pCell = map.TryGetCellAt(coords);
				if (!pCell)
					continue;

				const int idx = stride * y + x;
				int cpass = -1, clevel = -1, czai = -1;
				int sz0 = -1, sz1 = -1, sz2 = -1, szzai = -1, szlevel = -1;
				if (idx >= 0 && idx < validCells)
				{
					const CellLevelPassabilityStruct& lp = levelPass[idx];
					cpass = lp.CellPassability;
					clevel = lp.CellLevel;
					czai = lp.ZoneArrayIndex;
					const LevelAndPassabilityStruct2& sz = subzones[idx];
					sz0 = sz.word_0[0];
					sz1 = sz.word_0[1];
					sz2 = sz.word_0[2];
					szzai = static_cast<unsigned short>(sz.word_0[3]);
					szlevel = sz.CellLevel;
				}

				fprintf(pFile, "C=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x,%x,%x,%x,%d,%d,%d,%d,%d,%d,%d,%d\n",
					x, y,
					static_cast<int>(pCell->Passability),
					static_cast<int>(pCell->LandType),
					static_cast<int>(pCell->Level),
					static_cast<int>(pCell->Height),
					static_cast<int>(pCell->SlopeIndex),
					pCell->IsoTileTypeIndex,
					pCell->OverlayTypeIndex,
					static_cast<int>(pCell->OverlayData),
					static_cast<unsigned int>(pCell->OccupationFlags),
					static_cast<unsigned int>(pCell->AltOccupationFlags),
					static_cast<unsigned int>(pCell->Flags),
					static_cast<unsigned int>(pCell->AltFlags),
					cpass, clevel, czai, sz0, sz1, sz2, szzai, szlevel);
				++written;
			}
		}

		fprintf(pFile, "CELLS=%d\n", written);
		fclose(pFile);
		Debug::Log("[CellDump] Frame %d: wrote %d cells to %s (stride=%d validCells=%d)\n",
			frame, written, path, stride, validCells);
	}
}

void CellDump::PerFrame()
{
	if (!Enable)
		return;

	const int currentFrame = Unsorted::CurrentFrame;
	if (currentFrame < 0)
		return;

	if (currentFrame < lastSeenFrame)
	{
		// A new game started within the same process.
		dumpedMask = 0;
		dirCreated = false;
	}
	lastSeenFrame = currentFrame;

	for (int i = 0; i < FrameCount; ++i)
	{
		if (Frames[i] != currentFrame || (dumpedMask & (1u << i)))
			continue;
		dumpedMask |= (1u << i);
		WriteDump(currentFrame);
	}
}

// Same MainLoop-after-render address SyncDump and ProtocolZero hook (Syringe
// chains multiple handlers per address). Fires once per rendered game frame.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__CellDump, 0x5)
{
	CellDump::PerFrame();
	return 0;
}
