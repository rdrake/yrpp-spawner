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

#include <Windows.h>

// The raw CRC accumulator the shipped Ares.dll 21.352.1218 LogFrame builds on
// the stack for every checksum row. Layout is Checksummer's from ares-yrpp
// (github.com/Ares-Developers/YRpp, Checksummer.h, GPL v3): {+0 Value,
// +4 ByteIndex, +8 staging[4]}, plus the +0xC byte the engine's byte feeder
// (gamemd 0x4A1CC9) writes but never reads. Every shipped caller zeroes
// exactly the first THREE dwords, calls the object's ComputeCRC virtual, and
// prints the raw +0 value with NO finalize -- a trailing partial block is
// dropped (ratwo specs/ares-sync-dialect.md Q4; equals ratwo CrcEngine's
// committed()).
struct SyncCrcAccumulator
{
	DWORD Value;
	DWORD ByteIndex;
	BYTE Bytes[4];
	BYTE Padding;
};
