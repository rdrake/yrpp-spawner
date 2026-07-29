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

// Phase 1: the harness's first state-mutating verb.
//
// `move` injects a MegaMission EventClass into EventClass::OutList - the engine's
// own deterministic input path - so it is classified `stimulus-event`, not
// `setup-direct` (design section 7). Nothing here pokes object memory.
//
// Four hazards this file exists to handle, each measured or decompile-proven:
//
//  1. SINGLE-PLAYER ONLY. Any state write changes the frame CRC and desyncs
//     multiplayer (design section 11). Refuse unless GameMode is Campaign(0) or
//     Skirmish(5) - the same test the engine's own modal pumps use at 0x62312A.
//
//  2. OutList OVERFLOW IS A SILENT DROP. It is a QueueClass<EventClass,128>;
//     Add() returns false when full and does nothing else. Reporting `executed`
//     for a dropped order would be the worst available failure, so the return is
//     always checked.
//
//  3. EVENTS FOR OTHER HOUSES LEAK. An event for a house not controlled at this
//     machine never executes AND is never popped - SP never compacts DoList - so
//     they accumulate to a 16,384 cap. Always issue for
//     HouseClass::CurrentPlayer.
//
//  4. DEAD OBJECTS ARE STILL IN THE ARRAY. RemoveAllInactive runs AFTER the
//     dispatch hook at 0x55DDA0, so a deleted unit is still present and
//     findable. `target-expired` therefore tests the object's own liveness
//     flags, never lookup success.
//
// Handles are AbstractClass::UniqueID, resolved fresh on EVERY command. A raw
// pointer is never retained across frames (design section 9).

enum class OrderResult
{
	Ok,
	TargetExpired,
	NotSinglePlayer,
	QueueFull,
	NoPlayerHouse,
	BadCell,
};

// Phase 2: `spawn`, the harness's second state-mutating verb, and the first
// that creates rather than orders an object.
//
// Built strictly on the creation/ownership/placement/teardown path pinned in
// specs/notes/object-creation-teardown.md (Task 9). Two of that note's
// findings shape this directly:
//
//  1. INFANTRY ONLY. Only InfantryClass's construction chain was traced end
//     to end; UnitClass/AircraftClass have a class-specific construction-time
//     array (the note's Sec.3.3) that was never pinned. A type name that
//     resolves to a real, non-infantry TechnoTypeClass is UnsupportedType -
//     an explicit rejection, never a guess at an unpinned layout.
//
//  2. NO STABLE ENGINE UID. TechnoClass::Array's index is NOT a stable
//     identity - RemoveIndex (0x0063F000) is a shift-compaction, so every
//     deletion shifts every later index (the note's Sec.5). The label bound
//     to a spawned object is therefore AbstractClass::UniqueID: assigned
//     once per object, monotonically, by IRTTIInfo::Create_ID, and never
//     recycled - the same handle `move` already uses, and the only one the
//     note found that survives other objects' deaths.
//
// A rejected spawn (bad cell, unknown/unsupported type, allocation failure)
// leaves nothing behind. A spawn that allocates and constructs but fails to
// place (Unlimbo returns false) is NOT leaked: Limbo() is called
// unconditionally before delete (safe no-op on an object never unlimboed;
// specs/ObjectClass__Limbo_0x005f4d30.md Sec.4 step 1 - the note's Sec.6
// terminal-outcome rule, since the destructor chain was not confirmed to
// call Limbo() on its own).
enum class SpawnResult
{
	Ok,
	NotSinglePlayer,
	NoPlayerHouse,
	BadCell,
	UnknownType,
	UnsupportedType,
	AllocationFailed,
	PlacementRejected,
};

class HarnessOrders
{
public:
	// Order the unit identified by `uid` to move to map cell (cellX, cellY).
	static OrderResult Move(unsigned int uid, int cellX, int cellY);

	// Spawn an InfantryClass of `typeName` (a rules.ini ID, e.g. "E1"), owned
	// by HouseClass::CurrentPlayer, unlimboed at map cell (cellX, cellY). On
	// SpawnResult::Ok, `*outUid` receives the new object's
	// AbstractClass::UniqueID - the harness's label binding for it.
	static SpawnResult Spawn(const char* typeName, int cellX, int cellY, unsigned int* outUid);

	// Ack `reason` token for a result. Never contains a space - the host parses
	// ack lines as whitespace-separated KEY=value tokens.
	static const char* ResultReason(OrderResult result);
	static const char* ResultReason(SpawnResult result);
};
