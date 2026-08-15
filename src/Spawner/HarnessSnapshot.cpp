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

#include "HarnessSnapshot.h"

#include <Utilities/Debug.h>
// FootClass.h, not TechnoClass.h: the latter instantiates the generic_cast
// helpers in YRpp/Helpers/Cast.h, which static_assert on a COMPLETE FootClass.
// Including TechnoClass.h alone fails to compile. Same reason HarnessProbe.cpp
// includes it.
#include <FootClass.h>
#include <HouseClass.h>

#include <Windows.h>
#include <cstdio>

bool HarnessSnapshot::Write(const char* dir, int sessionId, int frame, int label)
{
	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\snapshots.csv", dir);

	const bool existed = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);

	FILE* pFile = std::fopen(path, "at");
	if (!pFile)
	{
		Debug::Log("[HarnessSnapshot] Could not append to %s\n", path);
		return false;
	}

	if (!existed)
		std::fprintf(pFile,
			"session,frame,label,uid,rtti,house,x,y,z,hp,mission,facing\n");

	const int total = TechnoClass::Array.Count;
	int written = 0;

	for (int i = 0; i < total && written < MaxRowsPerSnapshot; ++i)
	{
		TechnoClass* pTechno = TechnoClass::Array.GetItem(i);
		if (!pTechno)
			continue;

		// Liveness filter - see the header note on RemoveAllInactive ordering.
		if (!pTechno->IsAlive || pTechno->InLimbo || !pTechno->IsOnMap)
			continue;

		const CoordStruct coord = pTechno->GetCoords();
		const int house = pTechno->Owner ? pTechno->Owner->ArrayIndex : -1;

		std::fprintf(pFile,
			"%d,%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d\n",
			sessionId, frame, label,
			static_cast<unsigned int>(pTechno->UniqueID),
			static_cast<int>(pTechno->WhatAmI()),
			house,
			coord.X, coord.Y, coord.Z,
			pTechno->Health,
			static_cast<int>(pTechno->CurrentMission),
			// FacingClass::Current() returns a DirStruct, whose raw BAM field is
			// named `Raw`, not `Value` - the brief's field name was wrong; see
			// YRpp/Dir.h. This is the raw 0..65535 angle, not degrees.
			static_cast<int>(pTechno->PrimaryFacing.Current().Raw));
		++written;
	}

	std::fprintf(pFile, "# session=%d frame=%d label=%d rows=%d of %d\n",
		sessionId, frame, label, written, total);

	std::fclose(pFile);

	if (written >= MaxRowsPerSnapshot)
		Debug::Log("[HarnessSnapshot] Row cap %d hit at frame %d (array=%d)\n",
			MaxRowsPerSnapshot, frame, total);

	return true;
}
