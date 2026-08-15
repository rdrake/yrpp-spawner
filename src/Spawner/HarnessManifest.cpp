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

#include "HarnessManifest.h"

#include "CellDump.h"
#include "HarnessProbe.h"
#include "SyncDump.h"
// Brief's Step 2a verify-first check: the brief names this AStarDump.h /
// AStarDump::Enable, but on this branch the hook module is spelled AstarDump
// (lowercase t) - see src/Spawner/AstarDump.h. Corrected here rather than
// left dangling.
#include "AstarDump.h"

#include <Utilities/Debug.h>
#include <SessionClass.h>
#include <ScenarioClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>

void HarnessManifest::Write(const char* dir, int sessionId)
{
	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\manifest.txt", dir);

	FILE* pFile = std::fopen(path, "wt");
	if (!pFile)
	{
		Debug::Log("[HarnessManifest] Could not write %s\n", path);
		return;
	}

	std::fprintf(pFile,
		"protocol_version=%d\n"
		"session=%d\n"
		"game_mode=%d\n"
		// Game::Seed, not Unsorted::Seed as the brief had it - Seed is declared
		// on class Game in Unsorted.h (see CellDump.cpp/SyncDump.cpp, the two
		// other consumers). CI caught the wrong qualifier on the first push.
		"seed=%08X\n"
		// Whether that seed was pinned (HARNESS.Seed) or drifted in from the
		// system timer. Without this a reader cannot tell a reproducible run
		// from one that merely happened to be compared against itself.
		"seed_pinned=%d\n"
		"celldump_enabled=%d\n"
		"syncdump_enabled=%d\n"
		"syncdump_compute_crc=%d\n"
		"astardump_enabled=%d\n"
		// Ground truth for the tiberium-regrowth oracles: the archived
		// 2026-07-17/07-20 SYNC games behave as TiberiumSpreads=ON while the
		// 2026-07-21+ CELLDUMP games are field-proven OFF, and no static
		// analysis has located the toggle. special_flags is the dword the
		// engine gates on (CellClass::SpreadTiberium 0x483780 tests
		// ScenarioClass.SpecialFlags & 0x80); special_global is the Special
		// INI-read copy at 0xA8E960 for comparison.
		"special_flags=%08X\n"
		"special_global=%08X\n",
		ProtocolVersion,
		sessionId,
		static_cast<int>(SessionClass::Instance.GameMode),
		static_cast<unsigned int>(Game::Seed),
		HarnessProbe::PinnedSeed != 0 ? 1 : 0,
		CellDump::Enable ? 1 : 0,
		SyncDump::Enable ? 1 : 0,
		SyncDump::ComputeCRC ? 1 : 0,
		AstarDump::Enable ? 1 : 0,
		reinterpret_cast<unsigned int&>(ScenarioClass::Instance->SpecialFlags),
		*reinterpret_cast<unsigned int*>(0xA8E960));

	// Loaded-image bytes of CellClass::SpreadTiberium's gate block
	// (0x483780..0x483820). Every static source gate passes in the
	// 2026-07-21+ era yet the function returns false before the direction
	// draw; if these bytes diverge from gamemd-spawn.exe on disk, something
	// patches the gate at load time and the divergence names it. Read-only,
	// session-open, code pages are always readable - bounded at 0xA0 bytes.
	std::fprintf(pFile, "spreadtib_bytes=");
	const unsigned char* pGate = reinterpret_cast<const unsigned char*>(0x483780);
	for (int i = 0; i < 0xA0; ++i)
		std::fprintf(pFile, "%02X", pGate[i]);
	std::fprintf(pFile, "\n");

	std::fclose(pFile);
}
