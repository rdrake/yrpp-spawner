/**
*  yrpp-spawner
*
*  Copyright(C) 2026-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*/

#include "PlacementProbe.h"

#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <FootClass.h>
#include <ObjectClass.h>
#include <Unsorted.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

bool PlacementProbe::Enable = false;
int PlacementProbe::RowCount = 0;

static_assert(offsetof(AbstractClass, UniqueID) == 0x10, "AbstractClass::UniqueID moved");
static_assert(offsetof(ObjectClass, Location) == 0x9C, "ObjectClass::Location moved");
static_assert(offsetof(BuildingClass, Type) == 0x520, "BuildingClass::Type moved");
static_assert(offsetof(BuildingTypeClass, Foundation) == 0xEF0, "BuildingTypeClass::Foundation moved");

namespace
{
	constexpr char OutputPath[] = "PLACEMENTPROBE.TXT";
}

void PlacementProbe::Arm(bool enable)
{
	Enable = false;
	RowCount = 0;
	errno = 0;
	if (std::remove(OutputPath) != 0 && errno != ENOENT)
	{
		Debug::Log("[PlacementProbe] Could not remove stale %s\n", OutputPath);
		return;
	}
	if (!enable)
		return;

	FILE* pFile = std::fopen(OutputPath, "wt");
	if (!pFile)
	{
		Debug::Log("[PlacementProbe] Could not create %s\n", OutputPath);
		return;
	}

	const int headerResult = std::fprintf(pFile, "PLACEMENTPROBE=1\n");
	const int columnsResult = std::fprintf(pFile,
		"COLUMNS=frame,object,uid,type,input_x,input_y,input_z,previous_x,previous_y,previous_z,foundation,width,height\n");
	const int streamError = std::ferror(pFile);
	const int closeResult = std::fclose(pFile);
	if (headerResult < 0 || columnsResult < 0 || streamError != 0 || closeResult != 0)
	{
		errno = 0;
		if (std::remove(OutputPath) != 0 && errno != ENOENT)
			Debug::Log("[PlacementProbe] Could not remove failed %s\n", OutputPath);
		Debug::Log("[PlacementProbe] Could not initialize %s\n", OutputPath);
		return;
	}

	Enable = true;
}

void PlacementProbe::Record(ObjectClass* pObject, const CoordStruct* pCoord)
{
	if (!Enable || RowCount >= MaxRows || !pObject || !pCoord)
		return;

	if (pObject->WhatAmI() != AbstractType::Building)
		return;

	auto* pBuilding = static_cast<BuildingClass*>(pObject);
	auto* pType = pBuilding->Type;
	if (!pType)
		return;

	if (::_stricmp(pType->ID, "INGALITE") != 0
		&& ::_stricmp(pType->ID, "INYELWLAMP") != 0)
	{
		return;
	}

	FILE* pFile = std::fopen(OutputPath, "at");
	if (!pFile)
	{
		Enable = false;
		Debug::Log("[PlacementProbe] Could not append to %s\n", OutputPath);
		return;
	}

	const int writeResult = std::fprintf(pFile,
		"R=%d,%08X,%u,%.24s,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		Unsorted::CurrentFrame,
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pObject)),
		static_cast<unsigned int>(pObject->UniqueID),
		pType->ID,
		pCoord->X,
		pCoord->Y,
		pCoord->Z,
		pObject->Location.X,
		pObject->Location.Y,
		pObject->Location.Z,
		static_cast<int>(pType->Foundation),
		static_cast<int>(pType->GetFoundationWidth()),
		static_cast<int>(pType->GetFoundationHeight(false)));
	const int streamError = std::ferror(pFile);
	const int closeResult = std::fclose(pFile);
	if (writeResult < 0 || streamError != 0 || closeResult != 0)
	{
		Enable = false;
		Debug::Log("[PlacementProbe] Could not persist a row to %s\n", OutputPath);
		return;
	}

	++RowCount;
}

DEFINE_HOOK(0x5F6940, ObjectClass_SetLocation_PlacementProbe, 0xA)
{
	if (!PlacementProbe::Enable)
		return 0;

	auto* pObject = R->ECX<ObjectClass*>();
	GET_STACK(CoordStruct*, pCoord, 0x4);
	PlacementProbe::Record(pObject, pCoord);
	return 0;
}
