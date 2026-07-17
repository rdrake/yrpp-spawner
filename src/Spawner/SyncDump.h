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

// Per-frame sync-CRC trace dumper. When armed (ra2md.ini [Options] SYNCDUMP=yes)
// it flushes the engine's in-memory per-frame sync log (the ring buffer filled by
// Game::LogFrameCRC whenever Game::EnableMPSyncDebug is set) to SYNC*.TXT via the
// retail EventClass::Print_CRCs_All_Players writer, once per logic frame, and
// collects the files under SYNCDUMP\. Read-only with respect to game state: it
// never computes a CRC, draws a random number, or mutates the simulation.
class SyncDump
{
public:
	static bool Enable;
	static int MaxFrames;

	static void PerFrame();
};
