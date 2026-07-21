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
#include "SyncDump.h"
// Brief's Step 2a verify-first check: the brief names this AStarDump.h /
// AStarDump::Enable, but on this branch the hook module is spelled AstarDump
// (lowercase t) - see src/Spawner/AstarDump.h. Corrected here rather than
// left dangling.
#include "AstarDump.h"

#include <Utilities/Debug.h>
#include <SessionClass.h>
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
		"seed=%08X\n"
		"celldump_enabled=%d\n"
		"syncdump_enabled=%d\n"
		"syncdump_compute_crc=%d\n"
		"astardump_enabled=%d\n",
		ProtocolVersion,
		sessionId,
		static_cast<int>(SessionClass::Instance.GameMode),
		static_cast<unsigned int>(Unsorted::Seed),
		CellDump::Enable ? 1 : 0,
		SyncDump::Enable ? 1 : 0,
		SyncDump::ComputeCRC ? 1 : 0,
		AstarDump::Enable ? 1 : 0);

	std::fclose(pFile);
}
