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

#include "SyncPrint.h"
#include "AresChecksummer.h"

#include <AbstractClass.h>
#include <AbstractTypeClass.h>
#include <AircraftClass.h>
#include <AnimClass.h>
#include <BuildingClass.h>
#include <ColorScheme.h>
#include <EventClass.h>
#include <FootClass.h>
#include <FPSCounter.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <InfantryClass.h>
#include <Interfaces.h>
#include <MapClass.h>
#include <ObjectClass.h>
#include <ScenarioClass.h>
#include <TechnoClass.h>
#include <UnitClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <Windows.h>
#include <cstdio>
#include <cstring>

SyncPrint::Mode SyncPrint::PrintMode = SyncPrint::Mode::Off;

namespace
{
	// Ares fopens with "wt", so every \n it prints hits the disk -- and the
	// archive -- as \r\n. All literals and PutNL below emit \r\n directly.
	char Buf[2 * 1024 * 1024];
	char* out = Buf;
	// Longest observed row is under 400 bytes; a section header is shorter.
	// 1024, not 512: the inter-section tail literals (~100 bytes) are written
	// unguarded and spend this same reserve.
	constexpr size_t RowSlack = 1024;
	bool overflowed = false;
	bool overflowWarned = false;

	bool Guard()
	{
		if (out <= Buf + sizeof(Buf) - RowSlack)
			return true;
		overflowed = true;
		return false;
	}

	constexpr char HexDigits[] = "0123456789ABCDEF";

	void PutLit(const char* s)
	{
		while (*s)
			*out++ = *s++;
	}

	// The CRT renders a null %s argument as "(null)"; mirror it.
	void PutStr(const char* s)
	{
		if (!s)
			s = "(null)";
		while (*s)
			*out++ = *s++;
	}

	void PutNL()
	{
		*out++ = '\r';
		*out++ = '\n';
	}

	void PutHex8(DWORD v)
	{
		for (int i = 28; i >= 0; i -= 4)
			*out++ = HexDigits[(v >> i) & 0xF];
	}

	void PutHex2(unsigned v)
	{
		*out++ = HexDigits[(v >> 4) & 0xF];
		*out++ = HexDigits[v & 0xF];
	}

	// %X: minimal digits, uppercase.
	void PutHex(DWORD v)
	{
		if (!v)
		{
			*out++ = '0';
			return;
		}
		bool started = false;
		for (int i = 28; i >= 0; i -= 4)
		{
			const unsigned d = (v >> i) & 0xF;
			if (d || started)
			{
				*out++ = HexDigits[d];
				started = true;
			}
		}
	}

	void PutUInt(unsigned v)
	{
		char tmp[10];
		int n = 0;
		do
		{
			tmp[n++] = char('0' + v % 10u);
			v /= 10u;
		}
		while (v);
		while (n)
			*out++ = tmp[--n];
	}

	void PutInt(int v)
	{
		if (v < 0)
		{
			*out++ = '-';
			PutUInt(0u - static_cast<unsigned>(v));
		}
		else
			PutUInt(static_cast<unsigned>(v));
	}

	// %05d over non-negative loop ordinals.
	void PutZero5(int v)
	{
		if (v >= 100000)
		{
			PutInt(v);
			return;
		}
		for (int i = 10000; i >= 1; i /= 10)
			*out++ = char('0' + v / i % 10);
	}

	DWORD CrcOf(const AbstractClass* it)
	{
		SyncCrcAccumulator crc;
		crc.Value = 0;
		crc.ByteIndex = 0;
		std::memset(crc.Bytes, 0, sizeof(crc.Bytes));
		// Raw Value, no finalize: ares-sync-dialect.md Q4.
		it->ComputeCRC(reinterpret_cast<CRCEngine&>(crc));
		return crc.Value;
	}

	// gamemd's Abs-name table: 74 {const char* Name; int Value} pairs at
	// 0x816EE0..0x817130 -- the table Ares scans, NOT GetRTTIName's. A miss
	// returns nullptr, which PutStr renders "(null)" like the CRT would.
	struct NamedValue
	{
		const char* Name;
		int Value;
	};

	const char* ClassName(AbstractType abs)
	{
		auto p = reinterpret_cast<const NamedValue*>(0x816EE0);
		auto const end = reinterpret_cast<const NamedValue*>(0x817130);
		for (; p != end; ++p)
			if (p->Value == static_cast<int>(abs))
				return p->Name;
		return nullptr;
	}

	// FacingClass::Current(), transcribed from Ares.dll's own local
	// reimplementation at 0x10009D00 -- calling the engine's 0x4C93D0 would
	// fire the Phobos sync-log hook sitting on it. Layout inside the
	// 0x18-byte FacingClass: +0 desired, +4 start, +8 timer.StartTime,
	// +0x10 timer.TimeLeft, +0x14 ROT.
	WORD FacingCurrentRaw(const BYTE* pFacing)
	{
		const WORD desired = *reinterpret_cast<const WORD*>(pFacing);
		const short rot = *reinterpret_cast<const short*>(pFacing + 0x14);
		if (rot > 0)
		{
			const int startTime = *reinterpret_cast<const int*>(pFacing + 0x8);
			const int timeLeft = *reinterpret_cast<const int*>(pFacing + 0x10);
			int left = timeLeft;
			if (startTime != -1)
			{
				left = timeLeft - Unsorted::CurrentFrame + startTime;
				if (left < 0)
					left = 0;
			}
			if (left != 0)
			{
				const short diff = static_cast<short>(
					desired - *reinterpret_cast<const WORD*>(pFacing + 0x4));
				const int mag = diff < 0 ? -static_cast<int>(diff) : diff;
				// Quotient through its low 16 bits, tested signed -- so a
				// full half-turn (0x8000) also falls back to desired.
				const short steps = static_cast<short>(mag / rot);
				if (steps > 0)
					return static_cast<WORD>(
						desired - static_cast<int>(diff) * left / steps);
			}
		}
		return desired;
	}

	// The shipped quantiser (and the fold's): ((raw >> 7) + 1) >> 1, low
	// byte, wrapping to 0 at raw >= 0xFF80. ares-sync-dialect.md Q2. NOT the
	// 0.A-era raw >> 8. Printed %u.
	unsigned FacingQuant(WORD raw)
	{
		return static_cast<BYTE>(((raw >> 7) + 1) >> 1);
	}

	void Head(int idx, DWORD crc)
	{
		*out++ = '#';
		PutZero5(idx);
		*out++ = ':';
		*out++ = '\t';
		PutHex8(crc);
	}

	void WriteAbstract(const AbstractClass* it)
	{
		const AbstractType abs = it->WhatAmI();
		PutLit("; Abs: ");
		PutUInt(static_cast<unsigned>(abs));
		PutLit(" (");
		PutStr(ClassName(abs));
		*out++ = ')';
	}

	// The shared "%s (%d; %d,%d)" tail of Target: and Destination:.
	void PutTargetTriple(const AbstractClass* pTarget)
	{
		const char* name = "<None>";
		int index = -1;
		int x = -1;
		int y = -1;
		if (pTarget)
		{
			name = ClassName(pTarget->WhatAmI());
			index = pTarget->GetArrayIndex();
			const CoordStruct crd = pTarget->GetCoords();
			x = crd.X;
			y = crd.Y;
		}
		PutStr(name);
		PutLit(" (");
		PutInt(index);
		PutLit("; ");
		PutInt(x);
		*out++ = ',';
		PutInt(y);
		*out++ = ')';
	}

	void WriteObject(const ObjectClass* it)
	{
		WriteAbstract(it);
		const char* typeID = "<None>";
		int typeIndex = -1;
		if (auto const pType = it->GetType())
		{
			typeID = pType->ID;
			typeIndex = pType->GetArrayIndex();
		}
		const CoordStruct crd = it->GetCoords();
		// C-truncating /256 narrowed through short: the shipped cell math.
		const short cellX = static_cast<short>(crd.X / 256);
		const short cellY = static_cast<short>(crd.Y / 256);
		PutLit("; Type: ");
		PutInt(typeIndex);
		PutLit(" (");
		PutStr(typeID);
		PutLit("); Coords: ");
		PutInt(crd.X);
		*out++ = ',';
		PutInt(crd.Y);
		*out++ = ',';
		PutInt(crd.Z);
		PutLit(" (");
		PutInt(cellX);
		*out++ = ',';
		PutInt(cellY);
		PutLit("); Health: ");
		PutInt(it->Health);
		PutLit("; InLimbo: ");
		PutUInt(it->InLimbo ? 1u : 0u);
	}

	void WriteMission(const MissionClass* it)
	{
		WriteObject(it);
		PutLit("; Mission: ");
		PutInt(static_cast<int>(it->GetCurrentMission()));
		PutLit("; StartTime: ");
		PutInt(it->CurrentMissionStartTime);
	}

	void WriteTechno(const TechnoClass* it)
	{
		WriteMission(it); // the RadioClass layer adds nothing
		// +0x388 body (PrimaryFacing) and +0x3A0 turret (SecondaryFacing) as
		// raw reads, not through the YRpp members, so the printed byte comes
		// from exactly the loads Ares does (Ares.dll 0x1005F663/0x1005F66E).
		auto const base = reinterpret_cast<const BYTE*>(it);
		PutLit("; Facing: ");
		PutUInt(FacingQuant(FacingCurrentRaw(base + 0x388)));
		PutLit("; Facing2: ");
		PutUInt(FacingQuant(FacingCurrentRaw(base + 0x3A0)));
		PutLit("; Target: ");
		PutTargetTriple(it->Target);
	}

	void WriteFoot(const FootClass* it)
	{
		WriteTechno(it);
		PutLit("; Destination: ");
		PutTargetTriple(it->Destination);
	}

	// The shipped Speed and FPS conversions truncate (CRT _ftol, fisttp, at
	// Ares.dll 0x10078350); they do NOT round through the engine's 0x7C5F00.
	// A plain cast compiles to the same truncation.
	int SpeedOf(const FootClass* it)
	{
		return static_cast<int>(it->SpeedPercentage * 256.0);
	}

	void RowInfantry(const InfantryClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		WriteFoot(it);
		PutLit("; Speed ");
		PutInt(SpeedOf(it));
		PutNL();
	}

	void RowUnit(const UnitClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		WriteFoot(it);
		// FootClass::Locomotor, +0x674. Ares raises E_POINTER on a null one;
		// dereferencing matches that "cannot happen for a live unit" stance.
		auto const pLoco = *reinterpret_cast<ILocomotion* const*>(
			reinterpret_cast<const BYTE*>(it) + 0x674);
		PutLit("; Speed ");
		PutInt(pLoco->Get_Speed_Accum());
		PutLit("; TrackNumber: ");
		PutInt(pLoco->Get_Track_Number());
		PutLit("; TrackIndex: ");
		PutInt(pLoco->Get_Track_Index());
		PutNL();
	}

	void RowAircraft(const AircraftClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		WriteFoot(it);
		PutLit("; Speed ");
		PutInt(SpeedOf(it));
		PutLit("; Height: ");
		PutInt(it->GetHeight());
		PutNL();
	}

	void RowBuilding(const BuildingClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		WriteTechno(it);
		PutNL();
	}

	void RowHouse(const HouseClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		// Shipped-format divergences from 0.A source (fmt at Ares.dll
		// 0x100A5AD8): StartingAllies prints %d, Startspot is the packed
		// CellStruct dword as a single %d.
		PutLit("; CurrentPlayer: ");
		PutUInt(it->IsHumanPlayer ? 1u : 0u);
		PutLit("; ColorScheme: ");
		PutStr(ColorScheme::Array.Items[it->ColorSchemeIndex]->ID);
		PutLit("; ID: ");
		PutInt(it->ArrayIndex);
		PutLit("; HouseType: ");
		PutStr(HouseTypeClass::Array.Items[it->Type->ArrayIndex]->Name);
		PutLit("; Edge: ");
		PutInt(static_cast<int>(it->Edge));
		PutLit("; StartingAllies: ");
		PutInt(static_cast<int>(it->StartingAllies.data));
		PutLit("; Startspot: ");
		PutInt(*reinterpret_cast<const int*>(&it->StartingCell));
		PutLit("; Visionary: ");
		PutInt(it->Visionary); // signed char, movsx in the shipped build
		PutLit("; MapIsClear: ");
		PutUInt(it->MapIsClear ? 1u : 0u);
		PutLit("; Money: ");
		PutInt(static_cast<int>(it->Available_Money())); // %ld == %d on 32-bit
		PutNL();
	}

	// The object-level listings (Current Objects, both Logics dumps, all five
	// layers): shipped VectorLogger<ObjectClass> (Ares.dll 0x1005F350) skips
	// null slots and the anim sentinel; skipped slots still advance idx.
	void RowObject(const ObjectClass* it, int idx)
	{
		if (!it)
			return;
		if (it->WhatAmI() != AnimClass::AbsID || it->Fetch_ID() != -2)
		{
			if (!Guard())
				return;
			Head(idx, CrcOf(it));
			WriteObject(it);
			PutNL();
		}
	}

	// Same null + sentinel skip in the shipped Abstracts loop (0x10060125).
	void RowAbstract(const AbstractClass* it, int idx)
	{
		if (!it)
			return;
		if (it->WhatAmI() != AnimClass::AbsID || it->Fetch_ID() != -2)
		{
			if (!Guard())
				return;
			Head(idx, CrcOf(it));
			WriteAbstract(it);
			PutNL();
		}
	}

	// No guard and no null check in the shipped AbstractTypes loop
	// (0x10060201) -- a type is never an Anim.
	void RowType(const AbstractTypeClass* it, int idx)
	{
		if (!Guard())
			return;
		Head(idx, CrcOf(it));
		WriteAbstract(it);
		PutLit("; ID: ");
		PutStr(it->ID);
		PutLit("; Name: ");
		PutStr(it->Name);
		PutNL();
	}

	void SectionHeader(const char* label, int count)
	{
		if (!Guard())
			return;
		PutLit("Checksums for [");
		PutStr(label);
		PutLit("] (");
		PutUInt(static_cast<unsigned>(count)); // shipped prints %u
		*out++ = ')';
		PutNL();
	}

	template <typename T, typename Row>
	void VectorSection(const DynamicVectorClass<T*>& arr, const char* label, Row row)
	{
		if (label)
			SectionHeader(label, arr.Count);
		for (int i = 0; i < arr.Count; ++i)
			row(arr.Items[i], i);
	}

	template <typename T, typename Row>
	void HouseSection(const DynamicVectorClass<T*>& arr, const char* label, Row row)
	{
		for (int j = 0; j < HouseClass::Array.Count; ++j)
		{
			auto const pHouse = HouseClass::Array.Items[j];
			if (!Guard())
				return;
			PutLit("-------------------- ");
			PutStr(pHouse->Type->Name);
			PutLit(" (");
			PutInt(j);
			PutLit(") ");
			PutStr(label);
			PutLit(" -------------------");
			PutNL();
			for (int i = 0; i < arr.Count; ++i)
			{
				T* const it = arr.Items[i];
				if (it->Owner == pHouse)
					row(it, i);
			}
		}
	}

	// The AbstractTypes block is rules-immutable, 79% of the file; cache its
	// bytes and reuse while the type count holds. Verify mode never touches
	// the cache, so verification exercises the real path -- which also means
	// verify cannot catch a stale cache: a type-field mutation at constant
	// Count would pass verify and ship stale bytes in Fast mode. The premise
	// is measured, not assumed: the block hashed identical across all 55
	// intact corpus frames.
	char TypeCache[1536 * 1024];
	size_t TypeCacheLen = 0;
	int TypeCacheCount = -1;

	// The immutability premise is measured MOSTLY true: the 2026-08-15 verify
	// session's block shifted 3 times in 8,184 frames, single TeamType rows
	// (the AI mutates team state). Recompute a rolling slice per frame
	// (512/10,470 rows ~= 2.1 ms); a mismatch invalidates and the SAME call
	// rebuilds fresh, so at most 20 frames ship stale bytes. Events go to
	// META.TXT (the collected artifact) with the frame number; a CLIMBING
	// count means a frame-varying type checksum has turned the cache into a
	// rebuild-every-cycle loop (~+2 ms/frame) -- correct output, lost speed.
	constexpr int RevalRowsPerFrame = 512;
	int revalIdx = 0;
	size_t revalOff = 0;
	size_t typeFirstRowOff = 0;
	long long typeStaleCount = 0;

	// Emits fresh rows at `out` (inside Buf, so Guard() semantics hold) and
	// restores `out` before returning. False = stale row found, cache dead.
	bool RevalidateSlice(int count)
	{
		// A header-only cache (count 0) has no rows to check or index.
		if (count <= 0)
			return true;

		char* const scratch = out;
		for (int step = 0; step < RevalRowsPerFrame; ++step)
		{
			if (revalIdx >= count)
			{
				revalIdx = 0;
				revalOff = typeFirstRowOff;
			}
			out = scratch;
			RowType(AbstractTypeClass::Array.Items[revalIdx], revalIdx);
			const size_t freshLen = static_cast<size_t>(out - scratch);
			if (freshLen == 0)
			{
				// Guard tripped: buffer pressure, not a type mutation. The
				// memcpy fit-check downstream reports that; keep the stale
				// counter clean.
				out = scratch;
				return true;
			}
			// A cached row with no terminator is a broken invariant, never a
			// legitimate match -- treat as stale unconditionally.
			const void* nl = std::memchr(TypeCache + revalOff, '\n',
				TypeCacheLen - revalOff);
			bool stale = !nl;
			size_t cachedLen = 0;
			if (nl)
			{
				cachedLen = static_cast<size_t>(static_cast<const char*>(nl)
					- (TypeCache + revalOff)) + 1;
				stale = cachedLen != freshLen
					|| std::memcmp(TypeCache + revalOff, scratch, freshLen) != 0;
			}
			if (stale)
			{
				++typeStaleCount;
				char msg[128];
				std::sprintf(msg, "TYPES stale idx=%d frame=%d event=%lld\n",
					revalIdx, Unsorted::CurrentFrame,
					static_cast<long long>(typeStaleCount));
				// META.TXT is the artifact captures collect; the debug log
				// has no recorded consumer.
				if (FILE* pMeta = std::fopen("SYNCDUMP\\META.TXT", "at"))
				{
					std::fputs(msg, pMeta);
					std::fclose(pMeta);
				}
				Debug::Log("[SyncPrint] %s", msg);
				TypeCacheCount = -1;
				out = scratch;
				return false;
			}
			revalOff += cachedLen;
			++revalIdx;
		}
		out = scratch;
		return true;
	}

	void EmitAbstractTypes(bool useCache)
	{
		const int count = AbstractTypeClass::Array.Count;
		if (useCache && TypeCacheCount == count && RevalidateSlice(count))
		{
			if (out + TypeCacheLen <= Buf + sizeof(Buf))
			{
				std::memcpy(out, TypeCache, TypeCacheLen);
				out += TypeCacheLen;
			}
			else
				overflowed = true;
			return;
		}
		char* const start = out;
		VectorSection(AbstractTypeClass::Array, "AbstractTypes", RowType);
		const size_t len = static_cast<size_t>(out - start);
		if (useCache && !overflowed && len <= sizeof(TypeCache))
		{
			std::memcpy(TypeCache, start, len);
			TypeCacheLen = len;
			TypeCacheCount = count;
			const void* nl = std::memchr(TypeCache, '\n', len);
			typeFirstRowOff = nl
				? static_cast<size_t>(static_cast<const char*>(nl) - TypeCache) + 1
				: len;
			revalIdx = 0;
			revalOff = typeFirstRowOff;
		}
	}

	// The shipped LogFrame reads the mod name, version and CRC out of
	// Ares.dll's own data (RVAs 0xC1078, 0xC10B8, 0xC2950; disassembly at
	// 0x1005F8C6). Resolved through the module handle so a relocated
	// Ares.dll still yields its real values. Pinned to build 21.352.1218.
	BYTE* AresBase()
	{
		static BYTE* const base =
			reinterpret_cast<BYTE*>(GetModuleHandleA("Ares.dll"));
		return base;
	}
}

const char* SyncPrint::Build(int frameSlot, size_t& outLen)
{
	(void)frameSlot; // names the file; never the content
	out = Buf;
	overflowed = false;

	// The shipped Ares.dll's VERSION_STR, hardcoded: this is what the
	// installed build prints and what the archived corpus contains.
	PutLit("Ares synchronization log (version 21.352.1218)\r\n");

	for (int i = 0; i < 256; ++i)
	{
		PutLit("CRC[");
		PutHex2(static_cast<unsigned>(i));
		PutLit("] = ");
		PutHex8(EventClass::LatestFramesCRC[i]);
		PutNL();
	}

	// Exactly ONE draw from the sim generator, exactly here -- the traced
	// contract. Ares's own Random is a thunk to the same engine 0x65C780.
	PutLit("My Random Number: ");
	PutHex8(static_cast<DWORD>(ScenarioClass::Instance->Random.Random()));
	PutNL();
	PutLit("My Frame: ");
	PutHex8(static_cast<DWORD>(Unsorted::CurrentFrame));
	PutNL();
	PutLit("Average FPS: ");
	PutInt(static_cast<int>(FPSCounter::GetAverageFrameRate())); // truncates, like the shipped CRT _ftol
	PutNL();
	PutLit("Max MaxAhead: ");
	PutInt(Game::Network::MaxMaxAhead);
	PutNL();
	PutLit("Latency setting: ");
	PutInt(Game::Network::LatencyFudge);
	PutNL();
	PutLit("Game speed setting: ");
	PutInt(GameOptionsClass::Instance.GameSpeed);
	PutNL();
	PutLit("FrameSendRate: ");
	PutInt(Game::Network::FrameSendRate);
	PutNL();

	// Five 21.352 additions absent from the public 0.A source; read off the
	// shipped bytes at 0x1005F8C6..0x1005F94A.
	auto const ares = AresBase();
	PutLit("Mod is ");
	PutStr(ares ? reinterpret_cast<const char*>(ares + 0xC1078) : "");
	PutLit(" (");
	PutStr(ares ? reinterpret_cast<const char*>(ares + 0xC10B8) : "");
	PutLit(") with ");
	PutHex(ares ? *reinterpret_cast<const DWORD*>(ares + 0xC2950) : 0);
	PutNL();
	if (HouseClass::CurrentPlayer) // shipped omits the line when null
	{
		PutLit("Player Name: ");
		PutStr(HouseClass::CurrentPlayer->PlainName);
		PutNL();
	}
	{
		// Ares's checksum-triple helper (0x1002B240): session type 0xA8B238
		// == 3 selects the engine triple at 0xAC026C, else 0xB77E00; a zero
		// entry is recomputed by an Ares helper.
		auto const src = *reinterpret_cast<const int*>(0xA8B238) == 3
			? reinterpret_cast<const DWORD*>(0xAC026C)
			: reinterpret_cast<const DWORD*>(0xB77E00);
		DWORD rules = src[0];
		DWORD art = src[1];
		DWORD ai = src[2];
		if (ares)
		{
			using ComputeFn = DWORD(__stdcall*)();
			if (!rules)
				rules = reinterpret_cast<ComputeFn>(ares + 0x2B2F0)();
			if (!art)
				art = reinterpret_cast<ComputeFn>(ares + 0x2B230)();
			if (!ai)
				ai = reinterpret_cast<ComputeFn>(ares + 0x2B220)();
		}
		PutLit("Rules checksum: ");
		PutHex8(rules);
		PutNL();
		PutLit("Art checksum: ");
		PutHex8(art);
		PutNL();
		PutLit("AI checksum: ");
		PutHex8(ai);
		PutNL();
	}

	// ev is always nullptr on this path: no offending-event block, ever.

	PutLit("\r\nTypes\r\n");
	HouseSection(InfantryClass::Array, "Infantry", RowInfantry);
	HouseSection(UnitClass::Array, "Units", RowUnit);
	HouseSection(AircraftClass::Array, "Aircraft", RowAircraft);
	HouseSection(BuildingClass::Array, "Buildings", RowBuilding);

	PutLit("\r\nChecksums\r\n");
	VectorSection(HouseClass::Array, "Houses", RowHouse);
	VectorSection(InfantryClass::Array, "Infantry", RowInfantry);
	VectorSection(UnitClass::Array, "Units", RowUnit);
	VectorSection(AircraftClass::Array, "Aircraft", RowAircraft);
	VectorSection(BuildingClass::Array, "Buildings", RowBuilding);

	PutNL();
	VectorSection(ObjectClass::CurrentObjects, "Current Objects", RowObject);
	VectorSection<ObjectClass>(LogicClass::Instance, "Logics", RowObject);

	PutLit("\r\nChecksums for Map Layers\r\n");
	for (int layer = 0; layer < 5; ++layer)
	{
		if (Guard())
		{
			PutLit("Checksums for Layer ");
			PutInt(layer);
			PutNL();
		}
		VectorSection<ObjectClass>(MapClass::ObjectsInLayers[layer], nullptr, RowObject);
	}

	PutLit("\r\nChecksums for Logics\r\n");
	VectorSection<ObjectClass>(LogicClass::Instance, nullptr, RowObject);

	PutLit("\r\nChecksums for Abstracts\r\n");
	VectorSection(AbstractClass::Array, "Abstracts", RowAbstract);
	EmitAbstractTypes(PrintMode == Mode::Fast);

	if (overflowed && !overflowWarned)
	{
		overflowWarned = true;
		Debug::Log("[SyncPrint] 2 MB buffer overflowed; output truncated"
			" (verify would flag this)\n");
	}

	outLen = static_cast<size_t>(out - Buf);
	return Buf;
}

void SyncPrint::InvalidateTypeCache()
{
	TypeCacheLen = 0;
	TypeCacheCount = -1;
	// Called on new-game; a fresh game gets a fresh overflow warning, same
	// lifecycle as SyncDump's verifyWarned.
	overflowWarned = false;
}
