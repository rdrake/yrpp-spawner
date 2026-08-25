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

#include "HarnessOrders.h"

#include <Utilities/Debug.h>
#include <EventClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <SessionClass.h>
#include <TargetClass.h>
#include <GeneralDefinitions.h>
#include <InfantryTypeClass.h>
#include <InfantryClass.h>
#include <TechnoTypeClass.h>
#include <MapClass.h>

namespace
{
	// Resolve a UniqueID to a LIVE techno.
	//
	// Linear over TechnoClass::Array. That is bounded by the array count and runs
	// only when a move command is actually consumed (at most MaxScanPerFrame per
	// invocation), not every frame - so it respects the bounded-work rule without
	// needing an index we would then have to keep coherent.
	TechnoClass* ResolveLive(unsigned int uid)
	{
		const int count = TechnoClass::Array.Count;
		for (int i = 0; i < count; ++i)
		{
			TechnoClass* pTechno = TechnoClass::Array.GetItem(i);
			if (!pTechno || pTechno->UniqueID != uid)
				continue;

			// Found it - but presence proves nothing here, because
			// RemoveAllInactive has not run yet this frame.
			if (!pTechno->IsAlive || pTechno->InLimbo || !pTechno->IsOnMap)
				return nullptr;

			return pTechno;
		}
		return nullptr;
	}

	bool IsSinglePlayer()
	{
		const GameMode mode = SessionClass::Instance.GameMode;
		return mode == GameMode::Campaign || mode == GameMode::Skirmish;
	}
}

OrderResult HarnessOrders::Move(unsigned int uid, int cellX, int cellY)
{
	if (!IsSinglePlayer())
	{
		Debug::Log("[HarnessOrders] Refusing move: GameMode=%d is not SP\n",
			static_cast<int>(SessionClass::Instance.GameMode));
		return OrderResult::NotSinglePlayer;
	}

	if (cellX < 0 || cellY < 0)
		return OrderResult::BadCell;

	HouseClass* pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer)
		return OrderResult::NoPlayerHouse;

	TechnoClass* pTechno = ResolveLive(uid);
	if (!pTechno)
		return OrderResult::TargetExpired;

	const CellStruct cell { static_cast<short>(cellX), static_cast<short>(cellY) };

	const TargetClass src { static_cast<AbstractClass*>(pTechno) };
	const TargetClass dest { cell };
	const TargetClass none {};

	// MegaMission: (house, src, mission, target, dest, follow).
	const EventClass event(
		pPlayer->ArrayIndex,
		src,
		Mission::Move,
		none,     // no attack target
		dest,
		none);    // no follow-up

	// Checked, never fire-and-forget - overflow is a silent drop.
	if (!EventClass::OutList.Add(event))
	{
		Debug::Log("[HarnessOrders] OutList full; move for uid=%u DROPPED\n", uid);
		return OrderResult::QueueFull;
	}

	return OrderResult::Ok;
}

const char* HarnessOrders::ResultReason(OrderResult result)
{
	switch (result)
	{
	case OrderResult::Ok:              return "move-queued";
	case OrderResult::TargetExpired:   return "target-expired";
	case OrderResult::NotSinglePlayer: return "not-single-player";
	case OrderResult::QueueFull:       return "outlist-full";
	case OrderResult::NoPlayerHouse:   return "no-player-house";
	case OrderResult::BadCell:         return "bad-cell";
	case OrderResult::AttackerExpired: return "attacker-expired";
	case OrderResult::SelfTarget:      return "self-target";
	}
	return "unknown";
}

OrderResult HarnessOrders::Attack(unsigned int uid, unsigned int targetUid)
{
	if (!IsSinglePlayer())
	{
		Debug::Log("[HarnessOrders] Refusing attack: GameMode=%d is not SP\n",
			static_cast<int>(SessionClass::Instance.GameMode));
		return OrderResult::NotSinglePlayer;
	}

	// Checked before either lookup: with uid == targetUid both ResolveLive
	// calls succeed and the miss arms below can never report it, so this
	// would otherwise queue and earn an `attack-queued` ack.
	if (uid == targetUid)
		return OrderResult::SelfTarget;

	HouseClass* pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer)
		return OrderResult::NoPlayerHouse;

	// Two resolutions, two distinct misses. ResolveLive tests the object's own
	// liveness flags rather than lookup success, because RemoveAllInactive runs
	// AFTER the dispatch hook (hazard 4 in HarnessOrders.h) - a dead object is
	// still in TechnoClass::Array and still findable this frame.
	TechnoClass* pAttacker = ResolveLive(uid);
	if (!pAttacker)
		return OrderResult::AttackerExpired;

	TechnoClass* pTarget = ResolveLive(targetUid);
	if (!pTarget)
		return OrderResult::TargetExpired;

	const TargetClass src { static_cast<AbstractClass*>(pAttacker) };
	const TargetClass target { static_cast<AbstractClass*>(pTarget) };
	const TargetClass none {};

	// MegaMission: (house, src, mission, target, dest, follow). Against Move,
	// exactly two fields move - Mission::Attack for Mission::Move, and the
	// resolved target where Move put a cell in `dest`. `dest` is empty here:
	// an attack names an object, not a destination.
	//
	// Issued for HouseClass::CurrentPlayer, never the attacker's own house:
	// an event for a house not controlled at this machine never executes AND
	// is never popped (hazard 3), so a mis-addressed attack would leak. That
	// means an attacker the local player does not own will be refused by the
	// engine after the event pops - see the ack caveat in HarnessOrders.h.
	const EventClass event(
		pPlayer->ArrayIndex,
		src,
		Mission::Attack,
		target,
		none,     // no destination cell
		none);    // no follow-up

	// Checked, never fire-and-forget - overflow is a silent drop (hazard 2).
	if (!EventClass::OutList.Add(event))
	{
		Debug::Log("[HarnessOrders] OutList full; attack uid=%u -> uid=%u DROPPED\n",
			uid, targetUid);
		return OrderResult::QueueFull;
	}

	return OrderResult::Ok;
}

const char* HarnessOrders::AttackReason(OrderResult result)
{
	// `attack-queued`, not `attack-executed`: OutList.Add succeeded and that
	// is the whole claim (HarnessOrders.h). Every other arm is shared with
	// Move and delegates, so a rejection reason exists in exactly one place.
	if (result == OrderResult::Ok)
		return "attack-queued";

	return ResultReason(result);
}

SpawnResult HarnessOrders::Spawn(const char* typeName, int cellX, int cellY, unsigned int* outUid)
{
	if (!IsSinglePlayer())
	{
		Debug::Log("[HarnessOrders] Refusing spawn: GameMode=%d is not SP\n",
			static_cast<int>(SessionClass::Instance.GameMode));
		return SpawnResult::NotSinglePlayer;
	}

	if (cellX < 0 || cellY < 0)
		return SpawnResult::BadCell;

	HouseClass* pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer)
		return SpawnResult::NoPlayerHouse;

	const CellStruct cell { static_cast<short>(cellX), static_cast<short>(cellY) };
	if (!MapClass::Instance.CellExists(cell))
		return SpawnResult::BadCell;

	// INFANTRY ONLY (HarnessOrders.h). A name that resolves to some OTHER
	// real TechnoTypeClass (a vehicle, aircraft, building ID) is a distinct,
	// explicit rejection - never a silent attempt through an unpinned
	// construction-time array.
	InfantryTypeClass* pType = InfantryTypeClass::Find(typeName);
	if (!pType)
	{
		if (TechnoTypeClass::Find(typeName))
			return SpawnResult::UnsupportedType;
		return SpawnResult::UnknownType;
	}

	// Allocation + the full construction chain: InfantryTypeClass::CreateObject
	// (pinned 0x00523B10) -> InfantryClass ctor (0x00517A50) -> FootClass
	// (0x004D31E0) -> TechnoClass (0x006F2B40), which writes Owner (+0x21C) and
	// appends unconditionally to the three construction-time arrays before
	// this call returns (object-creation-teardown.md Sec.1, Sec.3). No
	// rollback needed on failure here - allocation failure leaves no trace
	// (same note, the table's first row).
	ObjectClass* pObj = pType->CreateObject(pPlayer);
	if (!pObj)
		return SpawnResult::AllocationFailed;

	TechnoClass* pTechno = static_cast<TechnoClass*>(pObj);

	const CoordStruct coord = CellClass::Cell2Coord(cell);
	if (!pTechno->Unlimbo(coord, DirType::North))
	{
		// Constructed but never placed. Left alone, this stays in the
		// construction-time arrays "forever" (object-creation-teardown.md
		// Sec.7) - an orphan. Limbo() before delete is the note's Sec.6
		// terminal-outcome rule: a documented safe no-op on an object that
		// was never unlimboed (specs/ObjectClass__Limbo_0x005f4d30.md Sec.4
		// step 1), called unconditionally because the destructor chain was
		// not confirmed to call it on its own.
		pTechno->Limbo();
		delete pTechno;
		return SpawnResult::PlacementRejected;
	}

	// Success. UniqueID was assigned during construction (AbstractClass'
	// base ctor, reached from the TechnoClass ctor tail), so it is already
	// valid here - this is the harness's label binding (HarnessOrders.h).
	*outUid = pTechno->UniqueID;
	return SpawnResult::Ok;
}

const char* HarnessOrders::ResultReason(SpawnResult result)
{
	switch (result)
	{
	case SpawnResult::Ok:                return "spawned";
	case SpawnResult::NotSinglePlayer:   return "not-single-player";
	case SpawnResult::NoPlayerHouse:     return "no-player-house";
	case SpawnResult::BadCell:           return "bad-cell";
	case SpawnResult::UnknownType:       return "unknown-type";
	case SpawnResult::UnsupportedType:   return "unsupported-type";
	case SpawnResult::AllocationFailed:  return "allocation-failed";
	case SpawnResult::PlacementRejected: return "placement-rejected";
	}
	return "unknown";
}
