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

// Write-watch engine: records every instruction that stores into a watched
// address range, with the value before and after the store.
//
// HOW. Hardware debug registers do not fire under Wine on Apple Silicon
// (Rosetta 2 stores DR0-DR7 but never raises the trap), so this uses page
// protection instead: each page holding a watched range is made read-only;
// a store faults into a vectored handler, which records the hit, restores
// the page, sets the trap flag and resumes; the single-step trap after the
// store re-protects the page and records the new value. Stores to the
// watched page OUTSIDE a watched range are counted as neighbour traps.
//
// REQUIRES Syringe --detach. An attached Syringe debugger consumes the
// single-step trap, so the page is never re-protected and every write after
// the first is lost. Arm() runs a self-test on a private page and refuses to
// arm when it fails, so a non-detached launch leaves no dump rather than a
// partial one.
//
// EXEC targets (`X:ADDR@LEN`) answer the other question a per-frame dump
// cannot: who reaches this instruction, with what. Arming copies the LEN
// bytes at ADDR into a trampoline and writes a jmp over them, once. Each
// pass then runs pushfd/pushad, logs the registers, four stack dwords and up
// to four pointer-chain reads, restores everything, runs the copied bytes and
// jumps back to ADDR+LEN. No trap is taken: an int3 per pass stalled the game
// on every hot target under Wine on Apple Silicon, while the same engine took
// 100,000 in a test host. At a function's first instruction the row is the
// return address, the receiver and the arguments; at its `ret`, the result.
//
// Deliberately free of YRpp so it builds with mingw for a local test under
// Wine; WatchDump.cpp is the game-side glue.
//
// LIMITS. A syscall that writes into a watched page fails instead of
// faulting (the kernel does not raise into user mode). On a HEAP page that
// can hang the game: `[A8B230]+214:4` (the UniqueID counter) spun the spawn
// round at 100% CPU with no further fault after the slot store. Watch
// globals; reach a heap field through an exec target on its writer instead. A second thread's
// store to a watched page, in the window between one thread's fault and its
// single-step, is not seen. The handler never allocates, so a store made
// under the heap lock cannot deadlock it. An exec target's LEN bytes must be
// whole instructions with no relative operand (no call, jmp or jcc) and no
// jump into them from elsewhere; nothing checks this, so take LEN from the
// disassembly as a Syringe hook's size is taken. A store made by a copied
// instruction reports the trampoline's EIP in a W= row.
namespace WatchEngine
{
	constexpr int MaxTargets = 8;
	constexpr int MaxCandidates = 4;
	constexpr unsigned int MaxTargetLength = 64;
	constexpr int MaxExecTargets = 16;
	constexpr int MaxReads = 4;
	constexpr int MaxReadSteps = 4;
	constexpr int ExecStackDwords = 4;
	constexpr unsigned int MinExecLength = 5;   // the jmp
	constexpr unsigned int MaxExecLength = 16;

	// The order the dump prints them in.
	enum Reg : int { Eax, Ecx, Edx, Ebx, Esp, Ebp, Esi, Edi, RegCount };

	struct ExecRow
	{
		int Frame;
		unsigned int ThreadId;
		unsigned int Target;        // index into the exec table
		unsigned int Regs[RegCount]; // Esp is the value at ADDR
		unsigned int Stack[ExecStackDwords]; // [esp], [esp+4], ...
		unsigned int Reads[MaxReads];
		unsigned int ReadOk;        // bit i set when read i reached readable memory at every step
		volatile long Valid;
	};

	// `reg+OFF>OFF2>OFF3`: the dword at reg+OFF, then the dword at that
	// value+OFF2, and so on. Offsets are signed hex.
	struct ReadSpec
	{
		int Base;                   // Reg
		int Steps;
		int Offsets[MaxReadSteps];
	};

	struct ExecInfo
	{
		unsigned int Address;
		unsigned int Length;
		unsigned char Original[MaxExecLength];
		unsigned int Trampoline;
		int ReadCount;
		ReadSpec Reads[MaxReads];
		long MaxHits;               // 0 = unbounded; past it a pass logs nothing
		volatile long Hits;
		bool Planted;
	};

	struct Row
	{
		int Frame;
		unsigned int ThreadId;
		unsigned int Eip;
		unsigned int FaultAddress;
		unsigned int Target;        // index into the target table
		unsigned int Offset;        // offset of the recorded dword inside the target
		unsigned int OldValue;
		unsigned int NewValue;
		unsigned int Candidates[MaxCandidates]; // stack dwords in .text preceded by a call
		volatile long Valid;
	};

	enum class TargetKind : int { Static, Deref, Base };

	struct TargetInfo
	{
		TargetKind Kind;
		unsigned int Slot;          // Deref: the pointer slot; Base: its own address
		unsigned int Offset;        // Deref: added to *Slot
		unsigned int Length;
		unsigned int Resolved;      // 0 while a Deref's pointer is null
		int Owner;                  // Base: the Deref target it re-resolves
	};

	using FrameFn = int(__cdecl*)();

	// Parses `spec`, installs the handler, runs the self-test and protects the
	// resolved pages. Spec: comma-separated `ADDR:LEN` or `[SLOT]+OFF:LEN`,
	// hex with or without 0x, LEN a multiple of 4 from 4 to MaxTargetLength,
	// or `X:ADDR@LEN[/READ]...[#MAXHITS]` for an exec target in any module but
	// the spawner (gamemd or Ares.dll), LEN decimal from 5 to 16, e.g.
	// `X:6FC0B0@6/ecx+0/ecx+1C8#5000` (MAXHITS decimal).
	// One store yields one row per dword it changed, and always one for the
	// dword it faulted on, so a store of an unchanged value is still seen. A `[SLOT]` target
	// also watches SLOT itself and re-resolves when SLOT is written.
	// Returns false with a message in `err` and leaves nothing armed.
	bool Arm(const char* spec, FrameFn frame, char* err, unsigned int errLen);

	// Unprotects every page and removes the handler. Rows already recorded
	// stay drainable.
	void Disarm();

	bool Armed();
	int SelfTestHits();
	int ExecSelfTestHits();
	int ExecCount();
	const ExecInfo& Exec(int index);
	long ExecDropped();
	int TargetCount();
	const TargetInfo& Target(int index);
	unsigned int TextLow();
	unsigned int TextHigh();
	long Dropped();
	long NeighbourTraps();

	// Up to MaxCandidates stack dwords from `sp` upward that sit inside the
	// game's .text and follow a call instruction, nearest first. Needs no Arm.
	void ReturnCandidates(unsigned int sp, unsigned int* out);

	// Calls `emit` for each completed row in record order and frees their
	// storage. Returns the number emitted.
	int Drain(void(__cdecl* emit)(const Row& row, void* ctx), void* ctx);
	int DrainExec(void(__cdecl* emit)(const ExecRow& row, void* ctx), void* ctx);
}
