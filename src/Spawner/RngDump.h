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

// RNG draw-attribution dumper. When armed (ra2md.ini [Options] RNGDUMP=yes) it
// records, for EVERY call into the two Randomizer entry points, the CALLER's
// return address plus the generator's table indices as of that call, to
// RNGDUMP\RNG_<seed>.TXT.
//
// WHY: the host's draw ledger can only bound which subsystem consumed which
// stream advances - it infers attribution from per-frame totals. The caller
// return address makes attribution exact, and the observation ORDER within a
// frame is the engine's true Logic-layer object-iteration order (which the host
// currently only *declares* as a modelling convention).
//
// HOW ADVANCES ARE RECOVERED. Randomizer::RandomRanged @0x65c7e0 is
// self-contained: it does NOT call Randomizer::Random, it inlines the same
// table arithmetic in a rejection-sampling loop, so a call consumes a VARIABLE
// number of advances (>=1) that no entry hook can read directly. But every
// advance increments Next1 by exactly one (mod 250), so the advances consumed
// by observation k are
//
//     (next1[k+1] - next1[k]) mod 250
//
// over consecutive observations OF THE SAME GENERATOR. Both advancing entry
// points are hooked, so no advance falls between two observations unattributed,
// and a per-frame checkpoint (PerFrame) closes the last call of each frame. A
// single call can never consume 250 advances, so the modular delta is exact.
//
// TWO GENERATORS. Unsorted::InitRandom @0x52fc20 seeds two: one inside the
// object pointed to by 0x00A8B230 (at +0x218), one static at 0x00886B88. Rows
// carry the `this` pointer so the host separates the streams; the header emits
// both addresses so they can be labelled.
//
// Read-only hook sites (see RngDump.cpp):
//   0x65c7e0 (6) - Randomizer::RandomRanged entry (this=ECX, [ESP+0]=caller,
//                  [ESP+4]=nMin, [ESP+8]=nMax).
//   0x65c780 (5) - Randomizer::Random entry (this=ECX, [ESP+0]=caller).
//   0x55DDA0 (5) - MainLoop-after-render, chained; per-frame checkpoint.
// Phobos hooks 0x65c7d0 and 0x65c88a in this same pair of functions; 0x65c7d0
// is Random's `ret` followed by 15 bytes of NOP padding, so neither window can
// reach 0x65c7e0. Verified against the gamemd-spawn.exe instruction stream.
class RngDump
{
public:
	// Armed from the RNGDUMP config flag.
	static bool Enable;

	// Stop appending once Unsorted::CurrentFrame exceeds this (0 = unlimited).
	// Draws made while loading a scenario (before frame 1) are ALWAYS recorded
	// regardless - that preamble is the whole point of the hook.
	static int MaxFrames;

	// Hard row cap - DLL-owned, never grows the game pool. On hitting it we log
	// once and stop appending.
	static constexpr long MaxRows = 2000000;

	// Rows emitted in the current session (diagnostic; also drives the cap).
	static long RowCount;

	// Called by the two entry hooks. `nMin`/`nMax` are RandomRanged's arguments;
	// RecordRaw is the argument-less Randomizer::Random.
	static void RecordRanged(unsigned int caller, const void* self, int nMin, int nMax);
	static void RecordRaw(unsigned int caller, const void* self);

	// Called once per logic frame from the MainLoop hook: samples both
	// generators so the last call of a frame has a closing bracket.
	static void PerFrame();
};
