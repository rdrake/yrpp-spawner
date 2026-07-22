/**
*  yrpp-spawner
*
*  Copyright(C) 2022-present CnCNet
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

#include "Main.Config.h"
#include <Spawner/AstarDump.h>
#include <Spawner/CellDump.h>
#include <Spawner/DamageDump.h>
#include <Spawner/HarnessProbe.h>
#include <Spawner/SyncDump.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <CCINIClass.h>
#include <GameOptionsClass.h>
#include <Unsorted.h>

#include <cstdlib>
#include <cstring>

void MainConfig::LoadFromINIFile()
{
	auto pINI = &CCINIClass::INI_RA2MD;
	if (!pINI)
		return;

	const char* pOptionsSection = "Options";
	if (pINI->GetSection(pOptionsSection))
	{
		this->AllowChat            = pINI->ReadBool(pOptionsSection, "AllowChat", this->AllowChat);
		this->AllowTaunts          = pINI->ReadBool(pOptionsSection, "AllowTaunts", this->AllowTaunts);
		this->DDrawHandlesClose    = pINI->ReadBool(pOptionsSection, "DDrawHandlesClose", this->DDrawHandlesClose);
		this->DisableEdgeScrolling = pINI->ReadBool(pOptionsSection, "DisableEdgeScrolling", this->DisableEdgeScrolling);
		this->MPDebug              = pINI->ReadBool(pOptionsSection, "MPDEBUG", this->MPDebug);
		this->QuickExit            = pINI->ReadBool(pOptionsSection, "QuickExit", this->QuickExit);
		this->SingleProcAffinity   = pINI->ReadBool(pOptionsSection, "SingleProcAffinity", this->SingleProcAffinity);
		this->SkipScoreScreen      = pINI->ReadBool(pOptionsSection, "SkipScoreScreen", this->SkipScoreScreen);
		this->SpeedControl         = pINI->ReadBool(pOptionsSection, "SpeedControl", this->SpeedControl);
		this->SyncDump             = pINI->ReadBool(pOptionsSection, "SYNCDUMP", this->SyncDump);
		this->SyncDumpComputeCRC   = pINI->ReadBool(pOptionsSection, "SYNCDUMP.ComputeCRC", this->SyncDumpComputeCRC);
		this->SyncDumpMaxFrames    = pINI->ReadInteger(pOptionsSection, "SYNCDUMP.MaxFrames", this->SyncDumpMaxFrames);
		pINI->ReadString(pOptionsSection, "ASTARDUMP", this->AstarDumpMode, this->AstarDumpMode, sizeof(this->AstarDumpMode));
		pINI->ReadString(pOptionsSection, "CELLDUMP.Frames", this->CellDumpFrames, this->CellDumpFrames, sizeof(this->CellDumpFrames));
		this->DamageDump           = pINI->ReadBool(pOptionsSection, "DAMAGEDUMP", this->DamageDump);
		this->HarnessProbeEnabled  = pINI->ReadBool(pOptionsSection, "HARNESS.Probe", this->HarnessProbeEnabled);
		pINI->ReadString(pOptionsSection, "HARNESS.Dir", this->HarnessDir, this->HarnessDir, sizeof(this->HarnessDir));
		this->HarnessSeed          = pINI->ReadInteger(pOptionsSection, "HARNESS.Seed", this->HarnessSeed);
	}

	const char* pVideoSection = "Video";
	if (pINI->GetSection(pVideoSection))
	{
		this->DDrawTargetFPS = pINI->ReadInteger(pVideoSection, "DDrawTargetFPS", this->DDrawTargetFPS);
		this->NoWindowFrame  = pINI->ReadBool(pVideoSection, "NoWindowFrame", this->NoWindowFrame);
		this->WindowedMode   = pINI->ReadBool(pVideoSection, "Video.Windowed", this->WindowedMode);
	}
}

void MainConfig::ApplyStaticOptions()
{
	if (this->MPDebug)
	{
		Game::EnableMPDebug     = true;
		Game::DrawMPDebugStats  = true;
		Game::EnableMPSyncDebug = true;

		// Fixes text layout in the MPDebug panel
		Patch::Apply_TYPED<DWORD>(0x542A19, { 312 });
		Patch::Apply_TYPED<DWORD>(0x542AA6, { 322 });
		Patch::Apply_TYPED<DWORD>(0x542B08, { 332 });
		Patch::Apply_TYPED<DWORD>(0x542B72, { 342 });
		Patch::Apply_TYPED<DWORD>(0x542BD4, { 352 });
		Patch::Apply_TYPED<DWORD>(0x542C94, { 362 });
		Patch::Apply_TYPED<DWORD>(0x542CF7, { 372 });
		Patch::Apply_TYPED<DWORD>(0x542D5E, { 382 });
		Patch::Apply_TYPED<DWORD>(0x542DC2, { 392 });
	}

	if (this->SyncDump)
	{
		// Arm the retail per-frame sync recording (Game::LogFrameCRC) even if
		// MPDEBUG is off; the dump hook itself lives in Spawner/SyncDump.cpp.
		Game::EnableMPSyncDebug = true;
		SyncDump::Enable = true;
		SyncDump::ComputeCRC = this->SyncDumpComputeCRC;
		SyncDump::MaxFrames = this->SyncDumpMaxFrames;
	}

	if (_stricmp(this->AstarDumpMode, "yes") == 0)
	{
		AstarDump::Enable = true;
		AstarDump::CaptureMode = AstarDump::Mode::Narrow;
		Debug::Log("[AstarDump] Armed (mode=narrow)\n");
	}
	else if (_stricmp(this->AstarDumpMode, "all") == 0)
	{
		AstarDump::Enable = true;
		AstarDump::CaptureMode = AstarDump::Mode::All;
		Debug::Log("[AstarDump] Armed (mode=all)\n");
	}
	else
	{
		AstarDump::Enable = false;
		AstarDump::CaptureMode = AstarDump::Mode::Disabled;
	}

	// HARNESS.Probe=yes - read-only driving-harness evidence probe
	// (Spawner/HarnessProbe.cpp). HARNESS.Dir names its working directory
	// relative to the game dir. Off by default; strictly read-only, so it is
	// safe to leave armed alongside the dump hooks.
	{
		HarnessProbe::Enable = this->HarnessProbeEnabled;
		if (HarnessProbe::Enable)
		{
			// Fixed-size copy into DLL-owned storage; always NUL-terminated.
			std::strncpy(HarnessProbe::Dir, this->HarnessDir, HarnessProbe::MaxDirLen - 1);
			HarnessProbe::Dir[HarnessProbe::MaxDirLen - 1] = '\0';
			if (!HarnessProbe::Dir[0])
			{
				std::strncpy(HarnessProbe::Dir, "HARNESS", HarnessProbe::MaxDirLen - 1);
				HarnessProbe::Dir[HarnessProbe::MaxDirLen - 1] = '\0';
			}
			Debug::Log("[HarnessProbe] Armed (dir=%s, read-only)\n", HarnessProbe::Dir);
		}
	}

	// HARNESS.Seed=<int> - pin the simulation RNG so a scenario can be replayed.
	//
	// spawn.ini's Seed= does NOT do this: Spawner::GetConfig()->Seed feeds only
	// random-MAP generation (RandomMap.cpp). The simulation draws from
	// Game::Seed (0xA8ED94), which in single player is seeded from the system
	// timer and pinned by nothing -- two identical skirmishes diverge.
	//
	// The engine already has a dormant override slot for exactly this. At
	// 0x52FDD4, immediately before Game::Seed is assigned:
	//
	//     mov  eax, [0xA8ED98]     ; preset seed
	//     test eax, eax
	//     jne  0x52FDF4            ; nonzero -> use it verbatim
	//     ...  call [0x7E1138]     ; else the system timer
	//     mov  [0xA8ED94], eax     ; Game::Seed = eax
	//
	// 0xA8ED98 has exactly ONE reference in the whole image -- that read. No
	// engine code ever writes it, so it is permanently zero in stock play.
	// Storing to it here therefore needs no hook and cannot race the engine:
	// the value simply survives until each scenario picks it up.
	//
	// Zero (the default) is indistinguishable from "unset" to the engine, so
	// it means "leave stock behaviour alone" -- a seed of 0 cannot be pinned.
	{
		HarnessProbe::PinnedSeed = this->HarnessSeed;
		if (HarnessProbe::PinnedSeed != 0)
		{
			*reinterpret_cast<int*>(0xA8ED98) = HarnessProbe::PinnedSeed;
			Debug::Log("[HarnessProbe] Seed pinned to %08X\n", HarnessProbe::PinnedSeed);
		}
	}

	// CELLDUMP.Frames=<frame>[,<frame>...] - whole-map per-cell pathfinding
	// snapshots (Spawner/CellDump.cpp). Empty (the default) leaves it off.
	// Malformed tokens are skipped; at most CellDump::MaxDumpFrames entries.
	{
		CellDump::FrameCount = 0;
		const char* p = this->CellDumpFrames;
		while (*p && CellDump::FrameCount < CellDump::MaxDumpFrames)
		{
			while (*p == ',' || *p == ' ')
				++p;
			if (!*p)
				break;
			char* end = nullptr;
			const long v = strtol(p, &end, 10);
			if (end == p)
			{
				// Non-numeric garbage: skip to the next separator.
				while (*p && *p != ',')
					++p;
				continue;
			}
			if (v >= 0)
				CellDump::Frames[CellDump::FrameCount++] = static_cast<int>(v);
			p = end;
		}
		CellDump::Enable = (CellDump::FrameCount > 0);
		if (CellDump::Enable)
		{
			Debug::Log("[CellDump] Armed (%d dump frame(s): %s)\n",
				CellDump::FrameCount, this->CellDumpFrames);
		}
	}

	// DAMAGEDUMP=yes - trace every GetTotalDamage call (Spawner/DamageDump.cpp).
	DamageDump::Enable = this->DamageDump;
	if (DamageDump::Enable)
		Debug::Log("[DamageDump] Armed\n");

	if (this->SingleProcAffinity)
	{
		auto const process = GetCurrentProcess();
		DWORD_PTR const processAffinityMask = 1;
		SetProcessAffinityMask(process, processAffinityMask);
	}

	if (this->WindowedMode)
	{
		GameOptionsClass::WindowedMode = true;

		if (this->NoWindowFrame)
			Patch::Apply_RAW(0x777CC0, // CreateMainWindow
			{
				0x68, 0x00, 0x00, 0x0A, 0x86 // push    0x860A0000; vs 0x02CA0000
			});
	}

	if (this->SpeedControl)
	{
		auto& speedControl = *reinterpret_cast<bool*>(0xA8EDDCu);
		speedControl = true;
	}

	auto& LANTaunts = *reinterpret_cast<bool*>(0xA8D110u);
	LANTaunts = this->AllowTaunts;

	// Set 3rd party ddraw.dll options
	if (HMODULE hDDraw = LoadLibraryA("ddraw.dll"))
	{
		if (bool* gameHandlesClose = (bool*)GetProcAddress(hDDraw, "GameHandlesClose"))
			*gameHandlesClose = !this->DDrawHandlesClose;

		LPDWORD TargetFPS = (LPDWORD)GetProcAddress(hDDraw, "TargetFPS");
		if (TargetFPS && this->DDrawTargetFPS != -1)
			*TargetFPS = this->DDrawTargetFPS;
	}
}
