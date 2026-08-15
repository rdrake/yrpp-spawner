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
#include "SyncPrint.h"
#include "TarZstd.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>
#include <EventClass.h>
#include <HouseClass.h>

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool SyncDump::Enable = false;
bool SyncDump::ComputeCRC = true;
int SyncDump::MaxFrames = 5000;
bool SyncDump::Archive = true;
int SyncDump::ArchiveLevel = TarZstdWriter::DefaultLevel;

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
	bool verifyWarned = false;

	// Finalised by SyncDump::Finish() on the paths that actually end a capture
	// (MaxFrames reached, or a new game in the same process). A process killed
	// outright never gets there, which is exactly what TarZstdWriter's periodic
	// zstd frames exist for: the archive stays readable up to the last one, so
	// nothing here depends on the destructor running at DLL unload.
	TarZstdWriter archive;

	// Phase timers: where inside this hook the frame time goes. The 2026-08-15
	// overhead matrix priced the whole hook at 20.6 ms/frame but cannot split
	// engine-CRC vs engine-print vs collect/compress; these accumulators can.
	long long hookTicks = 0;
	long long crcTicks = 0;
	long long printTicks = 0;
	long long collectTicks = 0;
	long long appendTicks = 0;
	constexpr int PhaseLogEvery = 500;

	long long Ticks()
	{
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return li.QuadPart;
	}

	long long TicksToUs(long long ticks)
	{
		static long long freq = 0;
		if (!freq)
		{
			LARGE_INTEGER li;
			freq = QueryPerformanceFrequency(&li) ? li.QuadPart : 1;
		}
		return ticks * 1000000ll / freq;
	}

	// Chrome Trace Event stream (Perfetto/chrome://tracing), JSON Array
	// Format: the spec makes the closing ] optional (the Object Format has no
	// such clause), and both importers additionally strip a trailing comma,
	// so a killed process leaves a loadable trace; fflush per frame bounds
	// the loss to one CRT buffer.
	FILE* pTrace = nullptr;
	long long traceT0 = 0;

	void EmitPhase(const char* name, long long t0, long long dur, int frame)
	{
		// t0 < traceT0: a hook slice that began before this session's trace
		// opened (first frame, or the new-game transition) -- init cost, not
		// phase cost, and a negative ts renders before trace start.
		if (!pTrace || dur <= 0 || t0 < traceT0)
			return;
		fprintf(pTrace,
			"{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"name\":\"%s\",\"ts\":%lld,"
			"\"dur\":%lld,\"args\":{\"frame\":%d}},\n",
			name, TicksToUs(t0 - traceT0), TicksToUs(dur), frame);
	}

	// Covers every return path of PerFrame, so hook - (crc+print+collect) is a
	// measured residual rather than an assumption that nothing else costs.
	struct HookTimer
	{
		long long t0 = Ticks();
		int frame = 0;
		~HookTimer()
		{
			const long long dur = Ticks() - t0;
			hookTicks += dur;
			// frame <= 0 is the pre-game early return; those slices would
			// land in the PREVIOUS game's still-open trace.
			if (frame > 0)
				EmitPhase("hook", t0, dur, frame);
		}
	};

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

				const long long t0 = Ticks();
				const bool appended = archive.AppendFile(member, fd.cFileName);
				const long long dur = Ticks() - t0;
				appendTicks += dur;
				EmitPhase("append", t0, dur, frame);
				if (appended)
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

	// Fast mode: the built buffer goes straight into the archive under
	// exactly the member name CollectSyncFiles would have produced for the
	// engine-written SYNC%01d_%03d.TXT, same frame-stamp semantics.
	void WriteBuilt(const char* buf, size_t len, int frame, int slot)
	{
		// Build() tolerates a null CurrentPlayer; the filename must too.
		const int player = HouseClass::CurrentPlayer
			? HouseClass::CurrentPlayer->ArrayIndex : 0;
		if (archive.IsOpen())
		{
			char member[MAX_PATH];
			sprintf(member, "%s/F%07d_SYNC%01d_%03d.TXT", DumpDir, frame, player, slot);

			const long long t0 = Ticks();
			const bool appended = archive.AppendBytes(member, buf, len);
			const long long dur = Ticks() - t0;
			appendTicks += dur;
			EmitPhase("append", t0, dur, frame);
			if (appended)
				return;

			Debug::Log("[SyncDump] Archive append failed at frame %d (%s);"
				" falling back to plain files\n", frame, archive.LastError());
			archive.Close();
		}

		char dst[MAX_PATH];
		sprintf(dst, "%s\\F%07d_SYNC%01d_%03d.TXT", DumpDir, frame, player, slot);
		if (FILE* pFile = fopen(dst, "wb")) // buffer already carries \r\n
		{
			fwrite(buf, 1, len, pFile);
			fclose(pFile);
		}
	}

	// The one line whose bytes differ by construction in Verify mode: the
	// engine call and Build() each drew their own random number.
	void RandomLineSpan(const char* p, size_t n, size_t& start, size_t& len)
	{
		static const char key[] = "\r\nMy Random Number: ";
		constexpr size_t klen = sizeof(key) - 1;
		for (size_t i = 0; i + klen + 8 <= n; ++i)
		{
			if (p[i] == '\r' && std::memcmp(p + i, key, klen) == 0)
			{
				start = i + klen;
				len = 8; // %08lX
				return;
			}
		}
		start = n;
		len = 0;
	}

	void VerifyBuilt(const char* ours, size_t ourLen, int frame, int slot)
	{
		char line[192];
		char name[MAX_PATH];
		sprintf(name, "SYNC%01d_%03d.TXT",
			HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ArrayIndex : 0, slot);

		char* theirs = nullptr;
		size_t theirLen = 0;
		if (FILE* pFile = fopen(name, "rb"))
		{
			fseek(pFile, 0, SEEK_END);
			theirLen = static_cast<size_t>(ftell(pFile));
			fseek(pFile, 0, SEEK_SET);
			theirs = static_cast<char*>(malloc(theirLen ? theirLen : 1));
			if (theirs)
				theirLen = fread(theirs, 1, theirLen, pFile);
			fclose(pFile);
		}

		if (!theirs)
		{
			sprintf(line, "[SyncPrint] VERIFY frame=%d result=NOFILE %s\n", frame, name);
		}
		else
		{
			size_t oSkip, oSkipLen, tSkip, tSkipLen;
			RandomLineSpan(ours, ourLen, oSkip, oSkipLen);
			RandomLineSpan(theirs, theirLen, tSkip, tSkipLen);

			bool identical = true;
			size_t diffOff = 0;
			unsigned char ca = 0, cb = 0;
			size_t i = 0, j = 0;
			while (i < ourLen && j < theirLen)
			{
				if (i >= oSkip && i < oSkip + oSkipLen)
				{
					++i;
					continue;
				}
				if (j >= tSkip && j < tSkip + tSkipLen)
				{
					++j;
					continue;
				}
				if (ours[i] != theirs[j])
				{
					identical = false;
					diffOff = i;
					ca = static_cast<unsigned char>(ours[i]);
					cb = static_cast<unsigned char>(theirs[j]);
					break;
				}
				++i;
				++j;
			}
			while (i >= oSkip && i < oSkip + oSkipLen)
				++i;
			while (j >= tSkip && j < tSkip + tSkipLen)
				++j;
			if (identical && (i != ourLen || j != theirLen))
			{
				identical = false;
				diffOff = i;
				ca = i < ourLen ? static_cast<unsigned char>(ours[i]) : 0;
				cb = j < theirLen ? static_cast<unsigned char>(theirs[j]) : 0;
			}
			free(theirs);

			if (identical)
				sprintf(line, "[SyncPrint] VERIFY frame=%d result=IDENTICAL bytes=%u\n",
					frame, static_cast<unsigned>(ourLen));
			else
				sprintf(line, "[SyncPrint] VERIFY frame=%d result=DIFF offset=%u"
					" ours=%02X theirs=%02X\n",
					frame, static_cast<unsigned>(diffOff), ca, cb);
		}

		Debug::Log("%s", line);
		char meta[MAX_PATH];
		sprintf(meta, "%s\\META.TXT", DumpDir);
		if (FILE* pMeta = fopen(meta, "at"))
		{
			fputs(line, pMeta);
			fclose(pMeta);
		}
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

		// A pinned seed repeats across games; never truncate an earlier trace
		// (the DAMAGEDUMP lesson) -- probe a free _<n> suffix instead.
		char tracePath[MAX_PATH];
		sprintf(tracePath, "%s\\PHASES_%08X.json", DumpDir,
			static_cast<unsigned int>(Game::Seed));
		for (int n = 1; n < 1000 && GetFileAttributesA(tracePath) != INVALID_FILE_ATTRIBUTES; ++n)
			sprintf(tracePath, "%s\\PHASES_%08X_%d.json", DumpDir,
				static_cast<unsigned int>(Game::Seed), n);
		// Re-probe after the loop: on suffix exhaustion the last candidate
		// still exists, and opening it "wt" would be exactly the truncation
		// the probe exists to prevent.
		pTrace = GetFileAttributesA(tracePath) == INVALID_FILE_ATTRIBUTES
			? fopen(tracePath, "wt") : nullptr;
		if (pTrace)
		{
			traceT0 = Ticks();
			fprintf(pTrace, "[\n");
			Debug::Log("[SyncDump] Tracing phases to %s\n", tracePath);
		}

		Debug::Log("[SyncDump] Armed: Seed=%08X StartFrame=%d MaxFrames=%d ComputeCRC=%d\n",
			Game::Seed, Unsorted::CurrentFrame, SyncDump::MaxFrames, SyncDump::ComputeCRC);
	}
}

void SyncDump::PerFrame()
{
	if (!Enable || !Game::EnableMPSyncDebug)
		return;

	HookTimer hookTimer;

	const int currentFrame = Unsorted::CurrentFrame;
	hookTimer.frame = currentFrame;
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
		verifyWarned = false;
		SyncPrint::InvalidateTypeCache();
		// Partial window discarded, not merged into game 2's first window --
		// same kill semantics as the archive above.
		hookTicks = crcTicks = printTicks = collectTicks = appendTicks = 0;
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
			const long long t0 = Ticks();
			Game::ComputeFrameCRC();
			const long long dur = Ticks() - t0;
			crcTicks += dur;
			EmitPhase("crc", t0, dur, frame);
			// Mirror the network path's history stamp (0x6476A5) so the
			// dump's CRC[] table carries the per-frame CRC values; inert
			// otherwise (only the desync detector reads it, on peer events).
			EventClass::LatestFramesCRC[slot] = EventClass::CurrentFrameCRC;

			if (SlotLoggedFrame(slot) != frame)
				continue;
		}

		// print covers row production, whoever produces it: the engine call
		// (Off), Build (Fast), or both (Verify). All file I/O -- AppendBytes
		// or the plain-file write in Fast mode, the verify compare, and the
		// stray sweep -- lands in collect; append stays the archive-write
		// subset of collect.
		const long long tPrint = Ticks();
		const char* built = nullptr;
		size_t builtLen = 0;
		switch (SyncPrint::PrintMode)
		{
		case SyncPrint::Mode::Off:
			EventClass::Print_CRCs_All_Players(slot, nullptr);
			break;
		case SyncPrint::Mode::Fast:
			built = SyncPrint::Build(slot, builtLen);
			break;
		case SyncPrint::Mode::Verify:
			if (!verifyWarned)
			{
				verifyWarned = true;
				Debug::Log("[SyncPrint] VERIFY mode: Build() draws a SECOND"
					" rng number each frame -- this session is NOT trace-valid\n");
			}
			// Ares writes its file first (one draw), then Build (a second).
			EventClass::Print_CRCs_All_Players(slot, nullptr);
			built = SyncPrint::Build(slot, builtLen);
			break;
		}
		const long long tCollect = Ticks();
		if (SyncPrint::PrintMode == SyncPrint::Mode::Fast)
			WriteBuilt(built, builtLen, frame, slot);
		else if (SyncPrint::PrintMode == SyncPrint::Mode::Verify)
			VerifyBuilt(built, builtLen, frame, slot);
		// In Fast mode this is a stray-file sweep that normally finds
		// nothing; in Verify it archives Ares's file -- ground truth stays
		// in the archive there.
		CollectSyncFiles("F", frame);
		const long long tEnd = Ticks();
		printTicks += tCollect - tPrint;
		collectTicks += tEnd - tCollect;
		EmitPhase("print", tPrint, tCollect - tPrint, frame);
		EmitPhase("collect", tCollect, tEnd - tCollect, frame);
		if (pTrace)
			fflush(pTrace);
		lastDumpedFrame = frame;
		++dumpedCount;

		// Windowed, not cumulative, so growth with object count is visible
		// across a run. dumps= counts dumped frames (not engine frames --
		// frame= is the correlation key into the archive); append is a subset
		// of collect, not a fifth phase; hook excludes the in-progress call.
		if (dumpedCount % PhaseLogEvery == 0)
		{
			char line[256];
			sprintf(line, "PHASES dumps=%d..%d frame=%d hook=%lldus crc=%lldus"
				" print=%lldus collect=%lldus append=%lldus\n",
				dumpedCount - PhaseLogEvery + 1, dumpedCount, frame,
				TicksToUs(hookTicks), TicksToUs(crcTicks), TicksToUs(printTicks),
				TicksToUs(collectTicks), TicksToUs(appendTicks));

			// META.TXT is the one line-oriented artifact the capture pipeline
			// demonstrably collects; the debug log has no recorded consumer.
			char meta[MAX_PATH];
			sprintf(meta, "%s\\META.TXT", DumpDir);
			if (FILE* pMeta = fopen(meta, "at"))
			{
				fputs(line, pMeta);
				fclose(pMeta);
			}
			Debug::Log("[SyncDump] %s", line);

			hookTicks = crcTicks = printTicks = collectTicks = appendTicks = 0;
		}
	}
}

void SyncDump::Finish()
{
	// Before the archive early-return: plain-file sessions still carry a
	// trace, and the Array Format's closing ] is optional per spec.
	if (pTrace)
	{
		fclose(pTrace);
		pTrace = nullptr;
	}

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
