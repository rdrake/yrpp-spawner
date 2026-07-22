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

// GetTotalDamage trace dumper. When armed (ra2md.ini [Options] DAMAGEDUMP=yes)
// it records, for every MapClass::GetTotalDamage @0x489180 call, the function's
// own inputs and its return value to DAMAGEDUMP\DAMAGE_<seed>.TXT - so the host
// can verify total_damage(inputs) == output exactly, without inferring anything
// from HP deltas.
//
// Two read-only hook sites (see DamageDump.cpp): an ENTRY hook at 0x489180
// stages the inputs (damage=ECX, warhead=EDX, armor/distance on the stack, plus
// the warhead's Verses[armor]/CellSpread/PercentAtMax, the Rules cap, and the
// 0x00A8B230 no-damage flag), and one of three EXIT hooks captures EAX and emits
// the row. The function is non-reentrant (its only internal call is CRT _ftol,
// which does not re-enter), so a single pending slot is sound.
//
// Floats are written as raw hex bit patterns (f32 %08X, f64 %016llX): the host
// replays the x87 arithmetic and a decimal round-trip would corrupt it.
class DamageDump
{
public:
	// Armed from the DAMAGEDUMP config flag.
	static bool Enable;

	// Hard row cap - DLL-owned, never grows the game pool. Coverage saturates
	// long before this; on hitting it we log once and stop appending.
	static constexpr int MaxRows = 200000;

	// Entry/exit call counters. They must stay equal (every staged call must
	// reach exactly one hooked return); a divergence means a return path was
	// missed and is logged for the drop-check.
	static long EntryCount;
	static long ExitCount;

	// The inputs staged by the entry hook, consumed by whichever exit hook
	// fires. One slot only (non-reentrant).
	struct PendingRec
	{
		int frame;
		int damage;
		int armor;
		int distance;
		double verses;
		float cellspread;
		float percentatmax;
		int cap;
		bool mapNoDamage;
		bool warheadNull;
	};
	static PendingRec Pending;

	// Called by the entry hook with the function's inputs.
	static void StageInputs(int frame, int damage, int armor, int distance,
		double verses, float cellspread, float percentatmax, int cap,
		bool mapNoDamage, bool warheadNull);

	// Called by an exit hook with the function's return value; emits one row.
	static void Emit(int output);
};
