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

// Runtime half of the fixture provenance manifest (design section 10).
//
// The DLL writes ONLY what it alone can observe: protocol version, game mode,
// RNG seed, and which capture modules are armed. File hashes (game exe, spawner
// DLL, map, rules, scenario script) are computed HOST-side and merged there -
// hashing megabytes on a render call would violate the bounded-work rule.
//
// SYNCDUMP.ComputeCRC is recorded specifically: the skirmish capture path
// computes frame CRCs itself and CONSUMES deterministic RNG draws, so it is part
// of the run contract, not incidental instrumentation (design section 10).
class HarnessManifest
{
public:
	// Bump when any observation output format changes in a way the host must
	// notice. The host refuses to compare runs whose versions differ.
	static constexpr int ProtocolVersion = 1;

	static void Write(const char* dir, int sessionId);
};
