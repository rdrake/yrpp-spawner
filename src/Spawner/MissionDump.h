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

// Per-frame mission-state dumper. When armed (ra2md.ini [Options]
// MISSIONDUMP=yes) it emits, once per logic frame, one row per live
// TechnoClass-family object to MISSIONDUMP\MISSION_<seed>.TXT.
//
// WHY: the Ares SYNCDUMP object row prints CurrentMission and
// CurrentMissionStartTime (+0xC0) but NOT the mission timer's remaining count
// (+0xD0) and NOT MissionStatus (+0xBC). Those two fields are the whole
// question in the f10912 entry-timer episode: the 0x5b3060 dispatcher's
// per-arm epilogue overwrites an entry's TimeLeft=0 with the OUTGOING
// handler's return value, so two objects that entered the same mission on the
// same frame print IDENTICALLY while one ticks that frame and the other does
// not. +0xC0 is written only by ForceMission/NextMission and so witnesses the
// entry call; it says nothing about +0xD0. With +0xD0 printed the split is
// read straight off two rows.
//
// This is a SEPARATE dump on purpose. Adding the fields to the Ares row was
// the obvious alternative and is wrong twice over: the row is Ares's to
// format, not ours, and ratwo's `timer-recovery` gate keys on runs of
// BYTE-IDENTICAL printed row text - printing a value that changes every frame
// would collapse every static run to length 1 and destroy that gate's input.
//
// STRICTLY READ-ONLY, ZERO RNG DRAWS. The dumper walks the engine's own
// DynamicVectorClass arrays by plain Items/Count field reads and emits raw
// FIELD reads only - no virtual call, no JMP_THIS thunk, nothing that could
// reach a Randomizer entry point or write a game-state byte:
//
//   * MissionClass::CurrentMission           [this+0xAC]
//   * MissionClass::SuspendedMission         [this+0xB0]
//   * MissionClass::QueuedMission            [this+0xB4]
//   * MissionClass::MissionStatus            [this+0xBC]
//   * MissionClass::CurrentMissionStartTime  [this+0xC0]
//   * MissionClass::MissionAccumulateTime    [this+0xC4]
//   * MissionClass::UpdateTimer.StartTime    [this+0xC8]
//   * MissionClass::UpdateTimer.TimeLeft     [this+0xD0]  <- the ask
//
// The timer pair is the RAW STORED fields, deliberately NOT
// TimerStruct::GetTimeLeft(), which subtracts elapsed frames and returns a
// DERIVED remaining count. The dispatcher writes the raw field and the raw
// field is the entire point; a derived value would silently answer a
// different question.
//
// JOINING TO THE ARES ROW. `idx` is the object's position in its own engine
// array, which is the same ordinal Ares prints as `#NNNNN` in its
// `Checksums for [<Category>] (N)` listing. That ordinal is a WITHIN-FRAME key
// only - it shifts whenever a lower-indexed object dies (ratwo measured the
// Infantry pool going 50 -> 49 mid-capture on an Engineer->E1 slot reuse), so
// every row also carries the object pointer and AbstractClass::UniqueID, which
// are stable across frames. Consumers should join on (frame, cat, idx) and
// VERIFY with mission/startframe against the Ares row: this dump samples at
// the MainLoop-after-render point and Ares writes its log at its own point, so
// agreement on the two shared fields is the control that the two sampling
// points are equivalent for mission state. It is not assumed here.

class MissionDump
{
public:
	// Armed from the MISSIONDUMP config flag.
	static bool Enable;

	// Stop appending once Unsorted::CurrentFrame exceeds this (0 = unlimited),
	// non-latching - same contract as RNGDUMP.MaxFrames.
	static int MaxFrames;

	// Hard row cap - DLL-owned, never grows the game pool. On hitting it we log
	// once and stop appending.
	//
	// SIZING. One row per live object per frame, ~70 bytes. The 2026-08-04
	// capture ran 12,230 frames at ~340 objects = ~4.2M rows ~= 290 MB, so a
	// 4M cap would have truncated it silently-ish (one Debug::Log line). 8M
	// clears a full run of that shape with headroom. Bound the cost with
	// MISSIONDUMP.MaxFrames when only a window is wanted - the question this
	// dump was built for needs ~60 frames, not 12,230.
	static constexpr long MaxRows = 8000000;

	// Rows emitted in the current session (diagnostic; also drives the cap).
	static long RowCount;

	// Called once per logic frame from the shared MainLoop-after-render hook
	// (0x55DDA0 - the address SyncDump/CellDump/RngDump/AnimDump already hook;
	// Syringe chains multiple handlers per address).
	static void PerFrame();
};
