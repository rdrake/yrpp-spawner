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

// Phase 1: the harness's ONLY state-mutating verb.
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

class HarnessOrders
{
public:
	// Order the unit identified by `uid` to move to map cell (cellX, cellY).
	static OrderResult Move(unsigned int uid, int cellX, int cellY);

	// Ack `reason` token for a result. Never contains a space - the host parses
	// ack lines as whitespace-separated KEY=value tokens.
	static const char* ResultReason(OrderResult result);
};
