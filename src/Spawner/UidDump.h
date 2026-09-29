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

// UniqueID draw dumper. When armed (ra2md.ini [Options] UIDDUMP=yes) it
// records every call to the UniqueID allocator @0x68BCB0 - from settings load,
// before the scenario exists - to UIDDUMP\UID_<seed>.TXT.
//
// WHY: the engine's absolute UniqueID base (about 1,031,100 on the probe maps)
// is a count of every draw before the first map object, and a per-install
// remainder of about 2,100 draws has no traced source. The counter lives in
// the heap-allocated ScenarioClass (+0x214), where a WATCHDUMP page watch hangs
// the game, so this hooks the allocator instead.
//
// The allocator is a leaf: `mov eax,[ecx+0x214]; inc eax; mov [ecx+0x214],eax;
// ret`, with two callers. Via the object assigner @0x410230 (a leaf taking the
// object's +4 interface pointer and storing the id at its +0xC), [ESP+4] at
// allocator entry is the exact return into the drawing constructor and
// [ESP+8]-4 is the object, whose first dword is its vtable as of the draw.
// The other caller is ConnectionPointClass::Advise @0x4A0654. Rows also carry
// up to four deeper return-address candidates (WatchEngine::ReturnCandidates).
//
// Read-only: reads registers and stack, draws no RNG, allocates only its own
// row buffer (rows before the first frame are held until the file opens).
class UidDump
{
public:
	static bool Enable;

	// Stop recording once Unsorted::CurrentFrame exceeds this (0 = unlimited).
	static int MaxFrames;

	static constexpr long MaxRows = 2000000;

	static void Record(unsigned int esp, unsigned int newId);

	// Flushes buffered rows; called from the MainLoop-after-render hook.
	static void PerFrame();
};
