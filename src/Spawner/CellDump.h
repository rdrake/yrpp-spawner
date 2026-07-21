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

#pragma once

// Whole-map per-cell pathfinding-state dumper. When armed (ra2md.ini
// [Options] CELLDUMP.Frames=<frame>[,<frame>...]) it serializes, once per
// listed frame, one CELLDUMP\CELL_<frame>.TXT snapshot of every valid map
// cell: the CellClass render/logic fields (Passability, LandType, Level,
// Height, SlopeIndex, tile/overlay indices, occupation flags, cell flags)
// PLUS the two compact pathfinding caches the hierarchical A* layer consumes:
//
//   * MapClass::LevelAndPassability (MapClass+0x68) - 4-byte
//     CellLevelPassabilityStruct {CellPassability i8, CellLevel i8,
//     ZoneArrayIndex u16} per cell.
//   * MapClass::LevelAndPassabilityStruct2pointer_70 (MapClass+0x70, the
//     decompile's DAT_0087f858 pointee) - 10-byte record per cell: three
//     signed 16-bit subzone ids (levels 0/1/2) at +0/+2/+4, zone-array index
//     u16 at +6, cell level i8 at +8, spare byte at +9.
//
// Both caches are indexed (MapRect.Width + MapRect.Height + 1) * y + x
// (FUN_0056d430; clamped variant FUN_0056d3f0 clamps to
// [0, ValidMapCellCount)).
//
// Read-only with respect to game state: iterates the cell grid via
// MapClass::TryGetCellAt (a pure bounds-checked pointer-grid lookup - NOT the
// stateful CellIterator, whose cursor fields live on MapClass) and reads
// engine memory only; the sole side effect is the dump file itself.
class CellDump
{
public:
	// Armed when at least one dump frame is configured.
	static bool Enable;

	// Fixed capacity for the parsed CELLDUMP.Frames list. DLL-owned static
	// storage only - never grows, never allocates from the game pool.
	static constexpr int MaxDumpFrames = 16;
	static int Frames[MaxDumpFrames];
	static int FrameCount;

	// Called once per game frame from the shared MainLoop_AfterRender hook
	// (0x55DDA0 - the same address SyncDump and ProtocolZero already hook;
	// Syringe chains multiple handlers per address). Dumps when the current
	// frame is in Frames[] and not yet dumped this session.
	static void PerFrame();
};
