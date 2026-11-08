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

// Threat-scan probe (ratwo r1 section 743). When armed (ra2md.ini [Options]
// PROBEDUMP=yes) it records, for frames PROBEDUMP.First..PROBEDUMP.Last and
// one scanner (PROBEDUMP.Scanner=<UniqueID>, 0 = every scanner), what
// TechnoClass::GreatestThreat's aircraft-tracker loop @0x6F91EC and
// TechnoClass::CanAutoTargetObject @0x6F7CA0 see per candidate, to
// PROBEDUMP\PROBE_<seed>.TXT.
//
// Sites (each stolen run holds no relative operand, so Syringe relocates it,
// and every handler returns 0):
//   TRK   0x6F91EC  tracker loop body: every candidate Get returned (EBP);
//                   A = the threat flags [ESP+0x70]
//   ONMAP 0x6F9251  the IsOnMap read [EBP+0x74], after the alliance test
//   LAYER 0x6F926D  past the layer-2 reject; A = InWhichLayer's return (EAX),
//                   B = the house-filter byte [ESP+0x78]
//   CALL  0x6F9292  argument setup for the CanAutoTargetObject call
//   FERR  0x6F7CE2  set-up of CanAutoTargetObject's GetFireError call (ESI cand, EDI scanner)
//   EST   0x6F7CF7  the EstimatedHealth read [ESI+0x70]; A = EAX before it,
//                   which is GetFireError's return when FERR fired just before;
//                   FERR and EST carry B = the caller's return address [ESP+0x3C]
//                   (0x6F92B1 tracker, 0x6F8C05 TryAutoTargetObject); EST's
//                   C = the scanner type's [+0x394] (VHPScan, 2 = Strong)
//   SCORE 0x6F9363  the score compare, reached only when CanAutoTargetObject
//                   returned true at 0x6F92B1; A = score [ESP+0x74], B = best
//                   so far [ESP+0x1C]
//   RET   0x6F9DA1  GreatestThreat's shared return: cand = the returned
//                   [ESP+0x20] (UniqueID only, it need not be an ObjectClass),
//                   A = the best score [ESP+0x1C]
//
// Read-only: reads registers, stack and object fields, calls only GetType,
// draws no RNG, and owns its row buffer.
class ProbeDump
{
public:
	static bool Enable;
	static int First;
	static int Last;
	static unsigned int Scanner;

	static constexpr long MaxRows = 200000;

	static void Record(const char* site, const void* scanner, const void* candidate, int a, int b, int c = 0, bool objectFields = true);

	// Flushes buffered rows; called from the MainLoop-after-render hook.
	static void PerFrame();
};
