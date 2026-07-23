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
// IMPORTANT: under the CnCNet client the game runs Ares.dll, which hooks
// 0x489235 (its "GetTotalDamage_Verses" hook) and performs the Verses[armor]
// multiply from its OWN parsed table. The stock in-struct Verses[] array at
// warhead+0xA0 is never populated from rules -- it keeps its all-1.0 constructor
// default -- so it is useless to read. We therefore do NOT dump verses; instead
// we capture two engine intermediates on the positive path and let the host
// verify the stock-computed pieces (falloff, clamp, cap) bit-exact.
//
// Read-only hook sites (see DamageDump.cpp):
//   ENTRY 0x489180 - stages inputs (damage=ECX, warhead=EDX, armor/distance on
//                    the stack, the real CellSpread/PercentAtMax the stock
//                    falloff uses, the Rules cap, the 0x00A8B230 no-damage flag).
//   0x489227       - captures ESI = the stock falloff result, pre-max(0)-clamp
//                    and pre-verses (BEFORE Ares' 0x489235 hook; sited to end
//                    at 0x48922D so it cannot overlap Phobos' sz5 hook there).
//   0x489249       - captures EAX = the post-verses value entering the cap clamp
//                    (AFTER Ares; may be unreached if Ares returns past it).
//   EXIT (3 sites) - captures the final EAX and emits the row.
// The 0x489227/0x489249 sites are only on the positive path, so heal/early-out
// rows carry them as absent ("-"). The function is non-reentrant (its only
// internal call is CRT _ftol), so a single pending slot is sound.
//
// CellSpread/PercentAtMax are written as raw f32 hex bit patterns (%08X): the
// host replays the x87 falloff and a decimal round-trip would corrupt it.
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
		float cellspread;
		float percentatmax;
		int cap;
		bool mapNoDamage;
		bool warheadNull;
		// Positive-path intermediates, captured between entry and exit. Reset by
		// StageInputs; set by the 0x489227 / 0x489249 hooks; left absent on
		// heal / early-out rows (which never reach those sites).
		bool hasPreVerses;
		int preVerses; // ESI @0x489227: stock falloff result, pre-clamp/pre-verses
		bool hasScaled;
		int scaled;    // EAX @0x489249: post-verses value entering the cap clamp
	};
	static PendingRec Pending;

	// Called by the entry hook with the function's inputs.
	static void StageInputs(int frame, int damage, int armor, int distance,
		float cellspread, float percentatmax, int cap,
		bool mapNoDamage, bool warheadNull);

	// Called by the positive-path hooks with the two engine intermediates.
	static void StagePreVerses(int preVerses);
	static void StageScaled(int scaled);

	// Called by an exit hook with the function's return value; emits one row.
	static void Emit(int output);
};
