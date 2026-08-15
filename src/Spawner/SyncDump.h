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
// collects the files under SYNCDUMP\. With SYNCDUMP.FastPrint the writer is
// SyncPrint::Build instead (yes) or both, compared (verify); every producing
// path draws from the sim RNG identically except verify, which draws twice
// per frame and is therefore not trace-valid.
//
// When Archive is set (the default) each collected frame is appended to
// SYNCDUMP\TRACE.tar.zst as it is produced and the plain ~1 MB file is deleted,
// so a full-length trace costs ~120 MB of disk instead of ~14 GB. If the
// archive cannot be opened, or any append fails, collection falls back to the
// plain MoveFileExA path for the rest of the session: a capture that is merely
// large beats a capture that is missing.
class SyncDump
{
public:
	static bool Enable;
	static bool ComputeCRC;
	static int MaxFrames;
	static bool Archive;
	static int ArchiveLevel;

	static void PerFrame();
	// Finalises the archive. Idempotent; safe when archiving is off.
	static void Finish();
};
