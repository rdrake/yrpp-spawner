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
	}
	return "unknown";
}
