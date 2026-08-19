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

#pragma once
class MainConfig
{
public:
	// Options
	bool AllowChat;
	bool AllowTaunts;
	bool DDrawHandlesClose;
	bool DisableEdgeScrolling;
	bool MPDebug;
	bool QuickExit;
	bool SingleProcAffinity;
	bool SkipScoreScreen;
	bool SpeedControl;
	bool SyncDump;
	bool SyncDumpComputeCRC;
	int SyncDumpMaxFrames;
	bool SyncDumpArchive;
	int SyncDumpArchiveLevel;
	char SyncDumpFastPrint[8];
	char AstarDumpMode[8];
	char CellDumpFrames[128];
	bool DamageDump;
	bool RngDump;
	int RngDumpMaxFrames;
	bool AnimDump;
	int AnimDumpMaxFrames;
	bool MissionDump;
	int MissionDumpMaxFrames;
	bool HarnessProbeEnabled;
	bool HarnessQuitOnEnd;
	char HarnessDir[64];
	int HarnessSeed;

	// Video
	bool NoWindowFrame;
	bool WindowedMode;
	int DDrawTargetFPS;

	// Other
	bool DumpTypes;
	bool NoCD;
	int RA2ModeSaveID;

	MainConfig()
		// Options
		: AllowChat { true }
		, AllowTaunts { true }
		, DDrawHandlesClose { false }
		, DisableEdgeScrolling { false }
		, MPDebug { false }
		, QuickExit { false }
		, SingleProcAffinity { true }
		, SkipScoreScreen { false }
		, SpeedControl { false }
		, SyncDump { false }
		, SyncDumpComputeCRC { true }
		, SyncDumpMaxFrames { 5000 }
		// Archiving defaults ON: a full-length trace is ~14 GB of plain files
		// against ~120 MB archived, and that gap has already cost a
		// measurement (a 3.97% sample where the whole corpus was wanted).
		// Level 6 is the measured knee: 191x at 1.6 ms per captured frame,
		// where 9 buys 5% more for a third again the time and 12 is slower
		// AND slightly worse. Table in TarZstd.h.
		, SyncDumpArchive { true }
		, SyncDumpArchiveLevel { 6 }
		, SyncDumpFastPrint { "no" }
		, AstarDumpMode { "no" }
		, CellDumpFrames { "" }
		, DamageDump { false }
		, RngDump { false }
		, RngDumpMaxFrames { 0 }
		, AnimDump { false }
		, AnimDumpMaxFrames { 0 }
		, MissionDump { false }
		, MissionDumpMaxFrames { 0 }
		, HarnessProbeEnabled { false }
		, HarnessQuitOnEnd { false }
		, HarnessDir { "HARNESS" }
		, HarnessSeed { 0 }

		// Video
		, DDrawTargetFPS { -1 }
		, NoWindowFrame { false }
		, WindowedMode { false }

		// Other
		, DumpTypes { false }
		, NoCD { false }
		, RA2ModeSaveID { 0 }
	{ }

	void LoadFromINIFile();
	void ApplyStaticOptions();
};
