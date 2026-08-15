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
#include <ScenarioClass.h>
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

	// LightConvert identity, interned per dump. The raw CellClass::LightConvert
	// pointer is a heap address and differs between two runs of the same game,
	// so emitting it would make byte-comparison of dumps meaningless; an index
	// assigned in scan order carries the only property the analysis needs -
	// which cells share a converter. Reset at the top of every WriteDump.
	constexpr int MaxConverters = 256;
	const void* converters[MaxConverters] = {};
	int converterCount = 0;

	// -1 = cell has no converter; -2 = the intern table overflowed (identity
	// unknown for this cell, but the columns beside it are still valid).
	int ConverterIndex(const void* p)
	{
		if (!p)
			return -1;
		for (int i = 0; i < converterCount; ++i)
		{
			if (converters[i] == p)
				return i;
		}
		if (converterCount >= MaxConverters)
			return -2;
		converters[converterCount] = p;
		return converterCount++;
	}

	void WriteLighting(FILE* pFile, const char* key, const LightingStruct& lighting)
	{
		fprintf(pFile, "%s=%d,%d,%d,%d,%d\n", key,
			lighting.Tint.Red, lighting.Tint.Green, lighting.Tint.Blue,
			lighting.Ground, lighting.Level);
	}

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

		// The lighting header block below reads the scenario singleton. Without
		// it there is no ambient level and no lighting profile to name, and the
		// per-cell lighting columns would have no constants to be checked
		// against - so skip the whole dump rather than emit an unusable one.
		const ScenarioClass* scen = ScenarioClass::Instance;
		if (!scen)
		{
			Debug::Log("[CellDump] Frame %d: no scenario instance, skipping dump\n", frame);
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

		// Scenario lighting constants, the other half of the per-cell lighting
		// columns: CellClass::UpdateCellLighting derives every cell's cached
		// intensities from AmbientCurrent, the cell's own Ambient/Level, and
		// whichever of the four profiles is active. Which one that is is NOT
		// emitted - the three gating helpers (0x53A100 / 0x53B400 / 0x53A110)
		// are unnamed and calling them by address from here would be a guess at
		// their calling convention. All four profiles are dumped instead, so
		// the consumer names the active one by which profile reproduces the
		// per-cell values.
		fprintf(pFile, "AMBIENT=%d,%d,%d,%d,%d,%d\n",
			scen->AmbientOriginal, scen->AmbientCurrent, scen->AmbientTarget,
			scen->IonAmbient, scen->NukeAmbient, scen->DominatorAmbient);
		WriteLighting(pFile, "LIGHTNORMAL", scen->NormalLighting);
		WriteLighting(pFile, "LIGHTION", scen->IonLighting);
		WriteLighting(pFile, "LIGHTNUKE", scen->NukeLighting);
		WriteLighting(pFile, "LIGHTDOM", scen->DominatorLighting);

		converterCount = 0;

		// Column key for the C= lines below. occ/altocc/flags/altflags are
		// HEX (no 0x prefix - same convention as ASTARDUMP's COSTGRID flags);
		// every other column is signed decimal. cpass/clevel/czai come from
		// the 4-byte CellLevelPassabilityStruct cache; sz0/sz1/sz2 (subzone id
		// per level), szzai and szlevel from the 10-byte subzone cache. A cell
		// whose compact index falls outside [0, VALIDCELLS) emits -1 for all
		// seven cache columns.
		//
		// The nine lighting columns are the whole CellClass +0x104..+0x116
		// block, raw and in layout order, under the YRpp field names:
		// intensity (+0x104, u4, a 16.16 multiplier - printed unsigned decimal),
		// ambient (+0x108), inorm (+0x10A), iterr (+0x10C), c1blue (+0x10E),
		// c2red/c2green/c2blue (+0x110/+0x112/+0x114). The block is dumped
		// whole rather than as the two fields the consumer asked for, because
		// the YRpp naming of +0x10A/+0x10C is itself unconfirmed and a capture
		// that carries all nine can adjudicate it. lcvt is the interned
		// LightConvert identity, not a pointer (see ConverterIndex).
		fprintf(pFile, "COLUMNS=x,y,pass,land,level,height,slope,tile,overlay,odata,"
			"occ,altocc,flags,altflags,cpass,clevel,czai,sz0,sz1,sz2,szzai,szlevel,"
			"intensity,ambient,inorm,iterr,c1blue,c2red,c2green,c2blue,lcvt\n");

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

				fprintf(pFile, "C=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x,%x,%x,%x,%d,%d,%d,%d,%d,%d,%d,%d,"
					"%u,%d,%d,%d,%d,%d,%d,%d,%d\n",
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
					cpass, clevel, czai, sz0, sz1, sz2, szzai, szlevel,
					static_cast<unsigned int>(pCell->Intensity),
					static_cast<int>(pCell->Ambient),
					static_cast<int>(pCell->Intensity_Normal),
					static_cast<int>(pCell->Intensity_Terrain),
					static_cast<int>(pCell->Color1_Blue),
					static_cast<int>(pCell->Color2_Red),
					static_cast<int>(pCell->Color2_Green),
					static_cast<int>(pCell->Color2_Blue),
					ConverterIndex(pCell->LightConvert));
				++written;
			}
		}

		fprintf(pFile, "CELLS=%d\n", written);
		fclose(pFile);
		Debug::Log("[CellDump] Frame %d: wrote %d cells to %s (stride=%d validCells=%d converters=%d)\n",
			frame, written, path, stride, validCells, converterCount);
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
