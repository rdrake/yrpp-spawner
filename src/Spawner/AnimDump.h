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

// Per-frame anim-state dumper. When armed (ra2md.ini [Options] ANIMDUMP=yes)
// it emits, once per logic frame, one row per live AnimClass instance to
// ANIMDUMP\ANIM_<seed>.TXT.
//
// WHY: the SYNCDUMP per-object row prints only the GetCoords() result, which
// CANNOT distinguish "attached to an owner with an absolute Location" from
// "detached" - AnimClass::GetCoords @0x422BE0 adds the owner's coordinates iff
// [this+0xCC] is non-null, so the printed value is identical either way. The
// flame-detach question (is FIRE01/02/03 folded Free or AttachedToOwner?) is
// exactly the non-null -> null transition of the raw owner pointer together
// with the raw stored Location, neither of which any existing instrument
// records. This dump records both, every frame, keyed by a STABLE instance
// identity (the object pointer plus AbstractClass::UniqueID - the SYNCDUMP
// #NNNNN index shifts whenever a lower-indexed object dies and is not one).
//
// STRICTLY READ-ONLY, ZERO RNG DRAWS. The dumper iterates AnimClass::Array
// (the engine's own DynamicVectorClass<AnimClass*> @0xA8E9A8) by plain
// Items/Count field reads and emits raw FIELD reads only:
//
//   * AnimClass::OwnerObject   [this+0xCC]        - the detach observable
//   * ObjectClass::Location    [this+0x9C..0xA4]  - the RAW stored triple,
//     deliberately NOT GetCoords(): the raw value is the entire point
//   * the owner's own ObjectClass::Location on the same frame
//   * ObjectClass::InLimbo     [this+0x81]
//   * AnimClass::Animation.Value (stage counter, a plain int)
//   * AnimTypeClass::TiberiumSpreadRadius [type+0x33C] (the area-loop bound
//     on AnimClass::Update's terminal detach path), plus End, LoopCount,
//     ArrayIndex and the type ID string
//
// No game function is called - not even an inline accessor - so no code path
// can reach Randomizer::Random @0x65C780 or RandomRanged @0x65C7E0, no
// game-state byte is written, no game pool allocation happens, and object
// lifetimes and iteration order are untouched. Every offset above is pinned
// by a static_assert(offsetof(...)) in AnimDump.cpp against the YRpp headers,
// so a header/layout mismatch is a COMPILE error, never silent garbage.
class AnimDump
{
public:
	// Armed from the ANIMDUMP config flag.
	static bool Enable;

	// Stop appending once Unsorted::CurrentFrame exceeds this (0 = unlimited),
	// non-latching - same contract as RNGDUMP.MaxFrames.
	static int MaxFrames;

	// Hard row cap - DLL-owned, never grows the game pool. On hitting it we log
	// once and stop appending.
	static constexpr long MaxRows = 2000000;

	// Rows emitted in the current session (diagnostic; also drives the cap).
	static long RowCount;

	// Called once per logic frame from the shared MainLoop-after-render hook
	// (0x55DDA0 - the address SyncDump/CellDump/RngDump/ProtocolZero already
	// hook; Syringe chains multiple handlers per address).
	static void PerFrame();
};
