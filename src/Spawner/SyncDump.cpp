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

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>
#include <EventClass.h>

#include <Windows.h>
#include <cstdint>
#include <cstdio>

bool SyncDump::Enable = false;
int SyncDump::MaxFrames = 5000;

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

	int lastDumpedFrame = 0;
	int dumpedCount = 0;
	bool sessionInitialized = false;

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
		// must not get mixed into this trace.
		CollectSyncFiles("stale", 0);

		char meta[MAX_PATH];
		sprintf(meta, "%s\\META.TXT", DumpDir);
		if (FILE* pFile = fopen(meta, "at"))
		{
			fprintf(pFile, "Seed=%08X StartFrame=%d MaxFrames=%d\n",
				Game::Seed, Unsorted::CurrentFrame, SyncDump::MaxFrames);
			fclose(pFile);
		}

		Debug::Log("[SyncDump] Armed: Seed=%08X StartFrame=%d MaxFrames=%d\n",
			Game::Seed, Unsorted::CurrentFrame, SyncDump::MaxFrames);
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
		// A new game started within the same process.
		lastDumpedFrame = 0;
		dumpedCount = 0;
		sessionInitialized = false;
	}

	if (MaxFrames > 0 && dumpedCount >= MaxFrames)
		return;

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
			return;
		}

		const int slot = frame % FrameLogSlots;
		if (SlotLoggedFrame(slot) != frame)
			continue;

		EventClass::Print_CRCs_All_Players(slot, nullptr);
		CollectSyncFiles("F", frame);
		lastDumpedFrame = frame;
		++dumpedCount;
	}
}

DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__SyncDump, 0x5)
{
	SyncDump::PerFrame();

	return 0;
}
