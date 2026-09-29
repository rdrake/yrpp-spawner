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

#include "WatchEngine.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace WatchEngine
{
namespace
{
	constexpr unsigned int PageSize = 0x1000;
	constexpr int MaxPages = 32;
	constexpr int MaxPendingThreads = 32;
	constexpr int MaxPagesPerStep = 4;
	constexpr int MaxHitsPerStep = 4;
	constexpr long RingSize = 1L << 18;
	constexpr int CandidateScanDwords = 256;
	constexpr unsigned int TrapFlag = 0x100;
	constexpr unsigned int NoFaultIndex = 0xFFFFFFFF;

	struct Page
	{
		unsigned int Base;
		DWORD Original;
		int Refs;
	};

	// The whole target is snapshotted at the fault: a string store (rep stos,
	// rep movs) completes in ONE single-step under Rosetta, so the step diffs
	// the snapshot instead of trusting the faulting dword alone.
	struct Hit
	{
		int Target;
		int Frame;
		unsigned int Fault;
		unsigned int FaultIndex;
		unsigned int Eip;
		unsigned int Candidates[MaxCandidates];
		unsigned int Snapshot[MaxTargetLength / 4];
	};

	// One per thread that has faulted, found by thread id. No TLS: a TLS
	// expansion slot allocates on first use, and the handler must not.
	struct Pending
	{
		volatile LONG Owner;
		int PageCount;
		unsigned int Pages[MaxPagesPerStep];
		int HitCount;
		Hit Hits[MaxHitsPerStep];
	};

	TargetInfo targets[MaxTargets];
	int targetCount = 0;
	Page pages[MaxPages];
	int pageCount = 0;
	Pending pending[MaxPendingThreads];
	volatile LONG tableLock = 0;

	PVOID handler = nullptr;
	FrameFn frameFn = nullptr;
	bool armed = false;
	int selfTestHits = 0;
	unsigned int textLow = 0;
	unsigned int textHigh = 0;
	unsigned int ownLow = 0;
	unsigned int ownHigh = 0;

	Row ring[RingSize];
	volatile LONG ringNext = 0;
	LONG ringDrained = 0;
	volatile LONG dropped = 0;
	volatile LONG neighbour = 0;

	void Lock()
	{
		while (InterlockedCompareExchange(&tableLock, 1, 0) != 0)
			YieldProcessor();
	}

	void Unlock()
	{
		InterlockedExchange(&tableLock, 0);
	}

	DWORD ReadOnlyOf(DWORD protect)
	{
		const DWORD modifiers = protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
		switch (protect & 0xFF)
		{
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
			return PAGE_READONLY | modifiers;
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return PAGE_EXECUTE_READ | modifiers;
		default:
			return 0;
		}
	}

	int FindPage(unsigned int base)
	{
		for (int i = 0; i < pageCount; ++i)
			if (pages[i].Base == base)
				return i;
		return -1;
	}

	bool ProtectPage(unsigned int base)
	{
		const int found = FindPage(base);
		if (found >= 0)
		{
			++pages[found].Refs;
			return true;
		}
		if (pageCount == MaxPages)
			return false;

		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(reinterpret_cast<void*>(base), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			return false;
		const DWORD readOnly = ReadOnlyOf(mbi.Protect);
		if (!readOnly)
			return false;
		DWORD original = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(base), PageSize, readOnly, &original))
			return false;
		pages[pageCount++] = { base, original, 1 };
		return true;
	}

	void ReleasePage(unsigned int base)
	{
		const int found = FindPage(base);
		if (found < 0 || --pages[found].Refs > 0)
			return;
		DWORD ignored = 0;
		VirtualProtect(reinterpret_cast<void*>(base), PageSize, pages[found].Original, &ignored);
		pages[found] = pages[--pageCount];
	}

	bool ProtectRange(unsigned int low, unsigned int length)
	{
		for (unsigned int base = low & ~(PageSize - 1); base < low + length; base += PageSize)
			if (!ProtectPage(base))
				return false;
		return true;
	}

	void ReleaseRange(unsigned int low, unsigned int length)
	{
		for (unsigned int base = low & ~(PageSize - 1); base < low + length; base += PageSize)
			ReleasePage(base);
	}

	// A deref target follows its pointer slot: release the old range, protect
	// the new one. Called at arm time and after every store to the slot.
	void Resolve(int index)
	{
		TargetInfo& t = targets[index];
		const unsigned int pointer = *reinterpret_cast<volatile unsigned int*>(t.Slot);
		const unsigned int resolved = pointer ? pointer + t.Offset : 0;
		if (resolved == t.Resolved)
			return;
		if (t.Resolved)
			ReleaseRange(t.Resolved, t.Length);
		t.Resolved = resolved;
		if (resolved && !ProtectRange(resolved, t.Length))
			t.Resolved = 0;
	}

	// A plausible return address: inside .text and preceded by a call. A
	// heuristic filter over raw stack dwords, not an unwinder.
	bool FollowsCall(unsigned int address)
	{
		const unsigned char* b = reinterpret_cast<const unsigned char*>(address);
		if (b[-5] == 0xE8)
			return true;
		for (int length = 2; length <= 7; ++length)
			if (b[-length] == 0xFF && (b[-length + 1] & 0x38) == 0x10)
				return true;
		return false;
	}

	void CollectCandidates(const CONTEXT* context, unsigned int* out)
	{
		for (int i = 0; i < MaxCandidates; ++i)
			out[i] = 0;
		const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
		const unsigned int top = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(tib->StackBase));
		unsigned int sp = context->Esp;
		int found = 0;
		for (int i = 0; i < CandidateScanDwords && found < MaxCandidates && sp + 4 <= top; ++i, sp += 4)
		{
			const unsigned int value = *reinterpret_cast<const unsigned int*>(sp);
			if (value >= textLow + 7 && value < textHigh && FollowsCall(value))
				out[found++] = value;
		}
	}

	Pending* FindPending(DWORD threadId, bool claim)
	{
		for (Pending& p : pending)
			if (p.Owner == static_cast<LONG>(threadId))
				return &p;
		if (!claim)
			return nullptr;
		for (Pending& p : pending)
			if (InterlockedCompareExchange(&p.Owner, static_cast<LONG>(threadId), 0) == 0)
				return &p;
		return nullptr;
	}

	void RecordRow(const Hit& hit, unsigned int dword, unsigned int newValue, DWORD threadId)
	{
		const LONG index = InterlockedIncrement(&ringNext) - 1;
		if (index >= RingSize)
		{
			InterlockedIncrement(&dropped);
			return;
		}
		Row& row = ring[index];
		row.Frame = hit.Frame;
		row.ThreadId = threadId;
		row.Eip = hit.Eip;
		row.FaultAddress = hit.Fault;
		row.Target = static_cast<unsigned int>(hit.Target);
		row.Offset = dword * 4;
		row.OldValue = hit.Snapshot[dword];
		row.NewValue = newValue;
		for (int i = 0; i < MaxCandidates; ++i)
			row.Candidates[i] = hit.Candidates[i];
		MemoryBarrier();
		row.Valid = 1;
	}

	LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
	{
		const EXCEPTION_RECORD* record = info->ExceptionRecord;
		CONTEXT* context = info->ContextRecord;
		const DWORD threadId = GetCurrentThreadId();

		if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			if (record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
				return EXCEPTION_CONTINUE_SEARCH;
			const unsigned int fault = static_cast<unsigned int>(record->ExceptionInformation[1]);
			const unsigned int base = fault & ~(PageSize - 1);

			Lock();
			const int pageIndex = FindPage(base);
			Pending* p = pageIndex >= 0 ? FindPending(threadId, true) : nullptr;
			if (!p || p->PageCount == MaxPagesPerStep)
			{
				Unlock();
				return EXCEPTION_CONTINUE_SEARCH;
			}

			bool matched = false;
			for (int t = 0; t < targetCount; ++t)
			{
				const TargetInfo& target = targets[t];
				if (!target.Resolved || fault + 8 <= target.Resolved || fault >= target.Resolved + target.Length)
					continue;
				// A fault up to 7 bytes BELOW the range may be a wide store that
				// reaches into it, or a store to the field before it; the access
				// size is unknown, so it records only the dwords that changed.
				const bool inside = fault >= target.Resolved;
				if (inside)
					matched = true;
				if (p->HitCount == MaxHitsPerStep)
					continue;
				Hit& hit = p->Hits[p->HitCount++];
				const unsigned int dwords = target.Length / 4;
				hit.Target = t;
				hit.Frame = frameFn ? frameFn() : 0;
				hit.Fault = fault;
				hit.FaultIndex = inside ? (fault - target.Resolved) / 4 : NoFaultIndex;
				for (unsigned int d = 0; d < dwords; ++d)
					hit.Snapshot[d] = reinterpret_cast<volatile unsigned int*>(target.Resolved)[d];
				hit.Eip = context->Eip;
				CollectCandidates(context, hit.Candidates);
			}
			if (!matched)
				InterlockedIncrement(&neighbour);

			DWORD ignored = 0;
			VirtualProtect(reinterpret_cast<void*>(base), PageSize, pages[pageIndex].Original, &ignored);
			p->Pages[p->PageCount++] = base;
			Unlock();

			context->EFlags |= TrapFlag;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if (record->ExceptionCode == EXCEPTION_SINGLE_STEP)
		{
			Lock();
			Pending* p = FindPending(threadId, false);
			if (!p || p->PageCount == 0)
			{
				Unlock();
				return EXCEPTION_CONTINUE_SEARCH;
			}
			for (int i = 0; i < p->HitCount; ++i)
			{
				const Hit& hit = p->Hits[i];
				const TargetInfo& target = targets[hit.Target];
				const volatile unsigned int* now = reinterpret_cast<volatile unsigned int*>(target.Resolved);
				for (unsigned int d = 0; d < target.Length / 4; ++d)
				{
					const unsigned int value = now[d];
					if (d == hit.FaultIndex || value != hit.Snapshot[d])
						RecordRow(hit, d, value, threadId);
				}
				if (targets[hit.Target].Kind == TargetKind::Base)
					Resolve(targets[hit.Target].Owner);
			}
			for (int i = 0; i < p->PageCount; ++i)
			{
				const int pageIndex = FindPage(p->Pages[i]);
				if (pageIndex < 0)
					continue;
				DWORD ignored = 0;
				VirtualProtect(reinterpret_cast<void*>(p->Pages[i]), PageSize, ReadOnlyOf(pages[pageIndex].Original), &ignored);
			}
			p->PageCount = 0;
			p->HitCount = 0;
			Unlock();

			context->EFlags &= ~TrapFlag;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		return EXCEPTION_CONTINUE_SEARCH;
	}

	bool ModuleRange(HMODULE module, unsigned int* low, unsigned int* high, bool textOnly)
	{
		const unsigned char* image = reinterpret_cast<const unsigned char*>(module);
		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
		const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
		const unsigned int imageBase = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(module));
		if (!textOnly)
		{
			*low = imageBase;
			*high = imageBase + nt->OptionalHeader.SizeOfImage;
			return true;
		}
		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (std::strncmp(reinterpret_cast<const char*>(section->Name), ".text", 8) == 0)
			{
				*low = imageBase + section->VirtualAddress;
				*high = *low + section->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	bool ParseHex(const char*& cursor, unsigned int* out)
	{
		if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
			cursor += 2;
		char* end = nullptr;
		const unsigned long value = std::strtoul(cursor, &end, 16);
		if (end == cursor)
			return false;
		cursor = end;
		*out = static_cast<unsigned int>(value);
		return true;
	}

	bool ParseTarget(const char*& cursor, char* err, unsigned int errLen)
	{
		while (*cursor == ' ')
			++cursor;
		const char* start = cursor;
		const bool deref = *cursor == '[';
		unsigned int address = 0;
		unsigned int offset = 0;
		if (deref)
		{
			++cursor;
			if (!ParseHex(cursor, &address) || *cursor++ != ']' || *cursor++ != '+' || !ParseHex(cursor, &offset))
			{
				std::snprintf(err, errLen, "bad deref target at \"%s\"", start);
				return false;
			}
		}
		else if (!ParseHex(cursor, &address))
		{
			std::snprintf(err, errLen, "bad address at \"%s\"", start);
			return false;
		}
		char* end = nullptr;
		const unsigned long length = *cursor == ':' ? std::strtoul(cursor + 1, &end, 0) : 0;
		if (!end || length == 0 || length % 4 != 0 || length > MaxTargetLength)
		{
			std::snprintf(err, errLen, "target \"%s\" needs :LEN, a multiple of 4 from 4 to %u", start, MaxTargetLength);
			return false;
		}
		cursor = end;
		if (address >= ownLow && address < ownHigh)
		{
			std::snprintf(err, errLen, "target \"%s\" is inside the spawner DLL", start);
			return false;
		}
		if (targetCount + (deref ? 2 : 1) > MaxTargets)
		{
			std::snprintf(err, errLen, "more than %d targets (a [SLOT] target counts twice)", MaxTargets);
			return false;
		}
		if (deref)
		{
			const int owner = targetCount;
			targets[targetCount++] = { TargetKind::Deref, address, offset, static_cast<unsigned int>(length), 0, -1 };
			targets[targetCount++] = { TargetKind::Base, address, 0, 4, address, owner };
		}
		else
		{
			targets[targetCount++] = { TargetKind::Static, address, 0, static_cast<unsigned int>(length), address, -1 };
		}
		return true;
	}

	// Two stores to a private page: the first proves the fault is delivered,
	// the second proves the single-step re-protected the page.
	int RunSelfTest()
	{
		void* page = VirtualAlloc(nullptr, PageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!page)
			return 0;
		const unsigned int address = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(page)) + 0x40;
		const int saved = targetCount;
		targets[targetCount++] = { TargetKind::Static, address, 0, 4, address, -1 };
		int hits = 0;
		if (ProtectRange(address, 4))
		{
			volatile unsigned int* cell = reinterpret_cast<volatile unsigned int*>(address);
			*cell = 0x5A5A5A5A;
			*cell = 0xA5A5A5A5;
			const LONG end = ringNext < RingSize ? ringNext : RingSize;
			for (LONG i = 0; i < end; ++i)
			{
				const Row& row = ring[i];
				if (!row.Valid || row.FaultAddress != address)
					continue;
				if ((row.OldValue == 0 && row.NewValue == 0x5A5A5A5A) || (row.OldValue == 0x5A5A5A5A && row.NewValue == 0xA5A5A5A5))
					++hits;
			}
			ReleaseRange(address, 4);
		}
		targetCount = saved;
		for (LONG i = 0; i < RingSize && i < ringNext; ++i)
			ring[i].Valid = 0;
		ringNext = 0;
		ringDrained = 0;
		VirtualFree(page, 0, MEM_RELEASE);
		return hits;
	}

	void ResetPending()
	{
		for (Pending& p : pending)
		{
			p.Owner = 0;
			p.PageCount = 0;
			p.HitCount = 0;
		}
	}
}

bool Arm(const char* spec, FrameFn frame, char* err, unsigned int errLen)
{
	err[0] = '\0';
	if (armed)
	{
		std::snprintf(err, errLen, "already armed");
		return false;
	}
	HMODULE own = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&Arm), &own);
	if (!own || !ModuleRange(own, &ownLow, &ownHigh, false)
		|| !ModuleRange(GetModuleHandleA(nullptr), &textLow, &textHigh, true))
	{
		std::snprintf(err, errLen, "cannot read the module headers");
		return false;
	}

	targetCount = 0;
	for (const char* cursor = spec; *cursor; )
	{
		if (!ParseTarget(cursor, err, errLen))
		{
			targetCount = 0;
			return false;
		}
		while (*cursor == ' ')
			++cursor;
		if (*cursor == ',')
			++cursor;
		else if (*cursor)
		{
			std::snprintf(err, errLen, "unexpected \"%s\" after a target", cursor);
			targetCount = 0;
			return false;
		}
	}
	if (targetCount == 0)
	{
		std::snprintf(err, errLen, "no targets");
		return false;
	}

	frameFn = frame;
	ResetPending();
	handler = AddVectoredExceptionHandler(1, Handler);
	if (!handler)
	{
		std::snprintf(err, errLen, "AddVectoredExceptionHandler failed");
		targetCount = 0;
		return false;
	}

	selfTestHits = RunSelfTest();
	if (selfTestHits != 2)
	{
		std::snprintf(err, errLen, "self-test recorded %d of 2 stores; launch Syringe with --detach", selfTestHits);
		RemoveVectoredExceptionHandler(handler);
		handler = nullptr;
		ResetPending();
		targetCount = 0;
		return false;
	}

	Lock();
	for (int t = 0; t < targetCount; ++t)
	{
		if (targets[t].Kind == TargetKind::Deref)
			Resolve(t);
		else if (!ProtectRange(targets[t].Resolved, targets[t].Length))
		{
			Unlock();
			std::snprintf(err, errLen, "cannot protect target %d at %08X", t, targets[t].Resolved);
			Disarm();
			return false;
		}
	}
	Unlock();
	armed = true;
	return true;
}

void Disarm()
{
	Lock();
	for (int i = 0; i < pageCount; ++i)
	{
		DWORD ignored = 0;
		VirtualProtect(reinterpret_cast<void*>(pages[i].Base), PageSize, pages[i].Original, &ignored);
	}
	pageCount = 0;
	ResetPending();
	Unlock();
	if (handler)
		RemoveVectoredExceptionHandler(handler);
	handler = nullptr;
	armed = false;
}

bool Armed() { return armed; }
int SelfTestHits() { return selfTestHits; }
int TargetCount() { return targetCount; }
const TargetInfo& Target(int index) { return targets[index]; }
unsigned int TextLow() { return textLow; }
unsigned int TextHigh() { return textHigh; }
long Dropped() { return dropped; }
long NeighbourTraps() { return neighbour; }

int Drain(void(__cdecl* emit)(const Row& row, void* ctx), void* ctx)
{
	const LONG end = ringNext;
	const LONG limit = end < RingSize ? end : RingSize;
	int emitted = 0;
	while (ringDrained < limit && ring[ringDrained].Valid)
	{
		emit(ring[ringDrained], ctx);
		ring[ringDrained].Valid = 0;
		++ringDrained;
		++emitted;
	}
	if (ringDrained == limit && InterlockedCompareExchange(&ringNext, 0, end) == end)
		ringDrained = 0;
	return emitted;
}
}
