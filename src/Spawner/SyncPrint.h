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

#include <cstddef>

// In-memory rebuild of the exact bytes Ares.dll 21.352.1218's LogFrame (its
// replacement for EventClass::Print_CRCs_All_Players) writes to
// SYNC%01d_%03d.TXT, mirrored from the shipped DLL's instruction stream where
// it diverges from the public 0.A source. Byte-identical includes the \r\n
// every "wt"-mode \n becomes on disk.
namespace SyncPrint
{
	enum class Mode { Off, Verify, Fast };
	extern Mode PrintMode;

	// Draws exactly ONE number from the scenario RNG, at the same point in
	// the sequence Ares does -- part of the traced contract. Returns an
	// internal static buffer, valid until the next Build(). frameSlot names
	// the output file only; the content never depends on it.
	const char* Build(int frameSlot, size_t& outLen);

	// Drops the cached AbstractTypes block. Call on the new-game reset.
	void InvalidateTypeCache();
}
