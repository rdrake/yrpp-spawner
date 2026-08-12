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

#include "SyncDump.h"
#include "TarZstd.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>
#include <EventClass.h>

#include <Windows.h>
#include <cstdint>
#include <cstdio>

bool SyncDump::Enable = false;
bool SyncDump::ComputeCRC = true;
int SyncDump::MaxFrames = 5000;
bool SyncDump::Archive = true;
int SyncDump::ArchiveLevel = 3;

namespace
{
	// The per-frame sync log ring written by Game::LogFrameCRC: 256 slots of
	// 0x33C bytes at 0xAC6660, slot = frame % 256. Offset +0x8 of a slot holds
	// the value of Unsorted::CurrentFrame at the moment the slot was logged,
	// which lets us tell a fresh slot from a stale or never-written one.
	constexpr uintptr_t FrameLogBase = 0xAC6660u;
	constexpr uintptr_t FrameLogStride = 0x33Cu;
	constexpr int FrameLogSlots = 256;
	constexpr char DumpDir[] = "SYNCDUMP";
	constexpr char ArchiveName[] = "SYNCDUMP\\TRACE.tar.zst";

	int lastDumpedFrame = 0;
	int lastComputedFrame = 0;
	int dumpedCount = 0;
	bool sessionInitialized = false;

	// Finalised by SyncDump::Finish() on the paths that actually end a capture
	// (MaxFrames reached, or a new game in the same process). A process killed
	// outright never gets there, which is exactly what TarZstdWriter's periodic
	// zstd frames exist for: the archive stays readable up to the last one, so
	// nothing here depends on the destructor running at DLL unload.
	TarZstdWriter archive;

	int SlotLoggedFrame(int slot)
	{
		return *reinterpret_cast<const int*>(
			FrameLogBase + static_cast<uintptr_t>(slot) * FrameLogStride + 0x8);
	}

	// Print_CRCs_All_Players writes its file into the process CWD; the exact
	// name embeds the local slot and the frame slot. Rather than reproduce the
	// engine's naming, sweep every SYNC*.TXT out of the CWD into DumpDir under
	// a frame-stamped (strictly ordered) name.
	void CollectSyncFiles(const char* prefix, int frame)
	{
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA("SYNC*.TXT", &fd);
		if (hFind == INVALID_HANDLE_VALUE)
			return;

		do
		{
			// Forward slashes: this is an archive member path, not a Win32 one.
			// The stale sweep in InitSessionOnce runs before the archive is
			// opened, so leftovers from an earlier session take the move path
			// below without needing to be special-cased here.
			if (archive.IsOpen())
			{
				char member[MAX_PATH];
				sprintf(member, "%s/%s%07d_%s", DumpDir, prefix, frame, fd.cFileName);

				if (archive.AppendFile(member, fd.cFileName))
				{
					// The whole point: the uncompressed ~1 MB never persists.
					// `continue` in a do/while jumps to the CONDITION, i.e. to
					// FindNextFileA -- the same advance the move path takes.
					DeleteFileA(fd.cFileName);
					continue;
				}

				// Losing frames is worse than losing the space saving, so one
				// failure demotes the rest of the session to plain files
				// rather than retrying into a stream of unknown state.
				Debug::Log("[SyncDump] Archive append failed at frame %d (%s);"
					" falling back to plain files\n", frame, archive.LastError());
				archive.Close();
			}

			char dst[MAX_PATH];
			sprintf(dst, "%s\\%s%07d_%s", DumpDir, prefix, frame, fd.cFileName);
			MoveFileExA(fd.cFileName, dst, MOVEFILE_REPLACE_EXISTING);
		}
		while (FindNextFileA(hFind, &fd));

		FindClose(hFind);
	}

	void InitSessionOnce()
	{
		if (sessionInitialized)
			return;
		sessionInitialized = true;

		CreateDirectoryA(DumpDir, nullptr);
		// Leftover SYNC files from an earlier session or a real desync dump
		// must not get mixed into this trace. Deliberately before the archive
		// opens, so they land as plain files and never enter it.
		CollectSyncFiles("stale", 0);

		if (SyncDump::Archive)
		{
			if (archive.Open(ArchiveName, SyncDump::ArchiveLevel))
			{
				Debug::Log("[SyncDump] Archiving to %s (level %d)\n",
					ArchiveName, SyncDump::ArchiveLevel);
			}
			else
			{
				Debug::Log("[SyncDump] Could not open %s (%s); writing plain files\n",
					ArchiveName, archive.LastError());
			}
		}

		char meta[MAX_PATH];
		sprintf(meta, "%s\\META.TXT", DumpDir);
		if (FILE* pFile = fopen(meta, "at"))
		{
			fprintf(pFile, "Seed=%08X StartFrame=%d MaxFrames=%d\n",
				Game::Seed, Unsorted::CurrentFrame, SyncDump::MaxFrames);
			fclose(pFile);
		}

		Debug::Log("[SyncDump] Armed: Seed=%08X StartFrame=%d MaxFrames=%d ComputeCRC=%d\n",
			Game::Seed, Unsorted::CurrentFrame, SyncDump::MaxFrames, SyncDump::ComputeCRC);
	}
}

void SyncDump::PerFrame()
{
	if (!Enable || !Game::EnableMPSyncDebug)
		return;

	const int currentFrame = Unsorted::CurrentFrame;
	if (currentFrame <= 0)
		return;

	if (currentFrame < lastDumpedFrame)
	{
		// A new game started within the same process. Finalise the previous
		// game's archive before InitSessionOnce reopens it, or its trailer is
		// never written and the last frames sit in an unflushed zstd frame.
		SyncDump::Finish();

		lastDumpedFrame = 0;
		lastComputedFrame = 0;
		dumpedCount = 0;
		sessionInitialized = false;
	}

	if (MaxFrames > 0 && dumpedCount >= MaxFrames)
	{
		SyncDump::Finish();
		return;
	}

	InitSessionOnce();

	// Catch up on every frame logged since the last dump. LogFrameCRC may run
	// slightly before or after this hook within a frame, so frames are dumped
	// only once their ring slot is confirmed fresh; unlogged frames are
	// retried until the ring wraps past them.
	int from = lastDumpedFrame + 1;
	if (currentFrame - from >= FrameLogSlots)
		from = currentFrame - FrameLogSlots + 1;

	for (int frame = from; frame <= currentFrame; ++frame)
	{
		if (MaxFrames > 0 && dumpedCount >= MaxFrames)
		{
			Debug::Log("[SyncDump] MaxFrames=%d reached at frame %d, stopping\n",
				MaxFrames, frame);
			SyncDump::Finish();
			return;
		}

		const int slot = frame % FrameLogSlots;
		if (SlotLoggedFrame(slot) != frame)
		{
			// Skirmish (GameMode 5) never reaches either retail
			// ComputeFrameCRC call site (0x647684 is network-only, 0x64731C
			// attract-only), so compute it here — mirroring what the attract
			// periodic dump does each frame. Only for the current frame (past
			// frames cannot be recomputed), at most once per logic frame, and
			// only when the engine has not already logged the slot, so
			// networked sessions are unaffected. Each compute consumes two
			// Randomizer draws (one folded by ComputeFrameCRC, one drawn by
			// LogFrameCRC) — deterministic, end-of-frame, part of the traced
			// run's contract.
			if (!ComputeCRC || frame != currentFrame || lastComputedFrame == currentFrame)
				continue;

			lastComputedFrame = currentFrame;
			Game::ComputeFrameCRC();
			// Mirror the network path's history stamp (0x6476A5) so the
			// dump's CRC[] table carries the per-frame CRC values; inert
			// otherwise (only the desync detector reads it, on peer events).
			EventClass::LatestFramesCRC[slot] = EventClass::CurrentFrameCRC;

			if (SlotLoggedFrame(slot) != frame)
				continue;
		}

		EventClass::Print_CRCs_All_Players(slot, nullptr);
		CollectSyncFiles("F", frame);
		lastDumpedFrame = frame;
		++dumpedCount;
	}
}

void SyncDump::Finish()
{
	if (!archive.IsOpen())
		return;

	const long long members = archive.MemberCount();
	if (archive.Close())
		Debug::Log("[SyncDump] Archive closed, %lld members\n", members);
	else
		Debug::Log("[SyncDump] Archive close FAILED (%s)\n", archive.LastError());
}

DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__SyncDump, 0x5)
{
	SyncDump::PerFrame();

	return 0;
}
