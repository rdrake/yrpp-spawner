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

#include "WatchDump.h"
#include "WatchEngine.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <Unsorted.h>

#include <Windows.h>
#include <cstdio>

namespace
{
	constexpr char DumpDir[] = "WATCHDUMP";

	FILE* pFile = nullptr;
	bool fileOpenAttempted = false;
	int maxFrames = 0;
	long rowsWritten = 0;

	int __cdecl CurrentFrame()
	{
		return Unsorted::CurrentFrame;
	}

	const char* KindName(WatchEngine::TargetKind kind)
	{
		switch (kind)
		{
		case WatchEngine::TargetKind::Static: return "static";
		case WatchEngine::TargetKind::Deref: return "deref";
		case WatchEngine::TargetKind::Base: return "base";
		}
		return "?";
	}

	// Opened at the first frame, so SEED is the scenario's; rows recorded
	// before it (settings load, scenario init) are written then. Never
	// truncates: a pinned seed repeats across games (same policy as RngDump).
	bool EnsureFile()
	{
		if (pFile)
			return true;
		if (fileOpenAttempted)
			return false;
		fileOpenAttempted = true;

		CreateDirectoryA(DumpDir, nullptr);
		const unsigned int seed = static_cast<unsigned int>(Game::Seed);
		char path[MAX_PATH];
		std::sprintf(path, "%s\\WATCH_%08X.TXT", DumpDir, seed);
		for (int n = 1; n < 1000 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; ++n)
			std::sprintf(path, "%s\\WATCH_%08X_%d.TXT", DumpDir, seed, n);

		pFile = std::fopen(path, "wt");
		if (!pFile)
		{
			Debug::Log("[WatchDump] Failed to open %s for write\n", path);
			return false;
		}

		std::fprintf(pFile, "WATCHDUMP=1\n");
		std::fprintf(pFile, "SEED=%08X\n", seed);
		std::fprintf(pFile, "SELFTEST=%d of 2\n", WatchEngine::SelfTestHits());
		std::fprintf(pFile, "TEXT=%08X-%08X\n", WatchEngine::TextLow(), WatchEngine::TextHigh());
		for (int i = 0; i < WatchEngine::TargetCount(); ++i)
		{
			const auto& t = WatchEngine::Target(i);
			std::fprintf(pFile, "T=%d,%s,%08X,%X,%u,%d\n", i, KindName(t.Kind), t.Slot, t.Offset, t.Length, t.Owner);
		}
		// c1..c4 are stack dwords that look like return addresses, nearest
		// first; a stale frame can supply one, so read them as candidates.
		static const char* const regNames[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
		if (WatchEngine::ExecCount() > 0)
			std::fprintf(pFile, "XSELFTEST=%d of 2\n", WatchEngine::ExecSelfTestHits());
		for (int i = 0; i < WatchEngine::ExecCount(); ++i)
		{
			const auto& x = WatchEngine::Exec(i);
			std::fprintf(pFile, "XT=%d,%08X,%u,%ld", i, x.Address, x.Length, x.MaxHits);
			for (int r = 0; r < x.ReadCount; ++r)
			{
				const auto& read = x.Reads[r];
				std::fprintf(pFile, ",%s", regNames[read.Base]);
				for (int s = 0; s < read.Steps; ++s)
					std::fprintf(pFile, "%s%s%X", s ? ">" : "", read.Offsets[s] < 0 ? "-" : "+",
						static_cast<unsigned int>(read.Offsets[s] < 0 ? -read.Offsets[s] : read.Offsets[s]));
			}
			std::fprintf(pFile, "\n");
		}
		std::fprintf(pFile, "COLUMNS.T=index,kind,slot,offset,length,owner\n");
		std::fprintf(pFile, "COLUMNS.W=frame,tid,eip,fault,target,offset,old,new,c1,c2,c3,c4\n");
		std::fprintf(pFile, "COLUMNS.F=frame,rows,neighbour,dropped\n");
		std::fprintf(pFile, "COLUMNS.XT=index,address,length,maxhits,read0..read3\n");
		std::fprintf(pFile, "COLUMNS.X=frame,tid,target,eax,ecx,edx,ebx,esp,ebp,esi,edi,s0,s1,s2,s3,then one per XT read\n");
		std::fprintf(pFile, "COLUMNS.FX=frame,rows,dropped\n");
		Debug::Log("[WatchDump] Opened %s\n", path);
		return true;
	}

	void __cdecl EmitRow(const WatchEngine::Row& row, void*)
	{
		std::fprintf(pFile, "W=%d,%u,%08X,%08X,%u,%u,%08X,%08X,%08X,%08X,%08X,%08X\n",
			row.Frame, row.ThreadId, row.Eip, row.FaultAddress, row.Target, row.Offset,
			row.OldValue, row.NewValue,
			row.Candidates[0], row.Candidates[1], row.Candidates[2], row.Candidates[3]);
	}

	// s0 is the return address at a function's first instruction. A read that
	// met unreadable memory prints `-`, never a value.
	void __cdecl EmitExec(const WatchEngine::ExecRow& row, void*)
	{
		std::fprintf(pFile, "X=%d,%u,%u", row.Frame, row.ThreadId, row.Target);
		for (unsigned int reg : row.Regs)
			std::fprintf(pFile, ",%08X", reg);
		for (unsigned int dword : row.Stack)
			std::fprintf(pFile, ",%08X", dword);
		const int reads = WatchEngine::Exec(static_cast<int>(row.Target)).ReadCount;
		for (int r = 0; r < reads; ++r)
		{
			if (row.ReadOk & (1u << r))
				std::fprintf(pFile, ",%08X", row.Reads[r]);
			else
				std::fprintf(pFile, ",-");
		}
		std::fprintf(pFile, "\n");
	}

	void Flush(int frame)
	{
		if (!EnsureFile())
			return;
		const int rows = WatchEngine::Drain(EmitRow, nullptr);
		rowsWritten += rows;
		std::fprintf(pFile, "F=%d,%d,%ld,%ld\n", frame, rows, WatchEngine::NeighbourTraps(), WatchEngine::Dropped());
		if (WatchEngine::ExecCount() > 0)
		{
			const int execRows = WatchEngine::DrainExec(EmitExec, nullptr);
			rowsWritten += execRows;
			std::fprintf(pFile, "FX=%d,%d,%ld\n", frame, execRows, WatchEngine::ExecDropped());
		}
		std::fflush(pFile);
	}
}

void WatchDump::Arm(const char* targets, int maxFramesArg)
{
	char err[256];
	if (!WatchEngine::Arm(targets, CurrentFrame, err, sizeof(err)))
	{
		Debug::Log("[WatchDump] NOT armed: %s\n", err);
		return;
	}
	maxFrames = maxFramesArg;
	Debug::Log("[WatchDump] Armed %d targets and %d exec targets from \"%s\" (self-test %d of 2, exec self-test %d of 2, MaxFrames=%d)\n",
		WatchEngine::TargetCount(), WatchEngine::ExecCount(), targets, WatchEngine::SelfTestHits(),
		WatchEngine::ExecSelfTestHits(), maxFrames);
}

void WatchDump::PerFrame()
{
	if (!WatchEngine::Armed())
		return;

	const int frame = Unsorted::CurrentFrame;
	Flush(frame);
	if (maxFrames > 0 && frame >= maxFrames)
	{
		WatchEngine::Disarm();
		Flush(frame);
		if (pFile)
		{
			std::fprintf(pFile, "END=%d,%ld\n", frame, rowsWritten);
			std::fclose(pFile);
			pFile = nullptr;
		}
		Debug::Log("[WatchDump] Disarmed at frame %d after %ld rows\n", frame, rowsWritten);
	}
}

// Chained with the other dumps on the MainLoop-after-render point.
DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__WatchDump, 0x5)
{
	WatchDump::PerFrame();
	return 0;
}
