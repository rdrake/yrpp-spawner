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
int PlacementProbe::IniRowCount = 0;

static_assert(offsetof(AbstractClass, UniqueID) == 0x10, "AbstractClass::UniqueID moved");
static_assert(offsetof(ObjectClass, Location) == 0x9C, "ObjectClass::Location moved");
static_assert(offsetof(BuildingClass, Type) == 0x520, "BuildingClass::Type moved");
static_assert(offsetof(BuildingTypeClass, Foundation) == 0xEF0, "BuildingTypeClass::Foundation moved");
static_assert(offsetof(ObjectTypeClass, ImageFile) == 0x1F8, "ObjectTypeClass::ImageFile moved");

namespace
{
	constexpr char OutputPath[] = "PLACEMENTPROBE.TXT";

	bool IsLampType(const BuildingTypeClass* pType)
	{
		return pType
			&& (::_stricmp(pType->ID, "INGALITE") == 0
				|| ::_stricmp(pType->ID, "INYELWLAMP") == 0);
	}
}

void PlacementProbe::Arm(bool enable)
{
	Enable = false;
	RowCount = 0;
	IniRowCount = 0;
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
	const int columnsRResult = std::fprintf(pFile,
		"COLUMNS_R=frame,object,uid,type_ptr,type,input_x,input_y,input_z,previous_x,previous_y,previous_z,foundation,width,height\n");
	const int columnsIResult = std::fprintf(pFile,
		"COLUMNS_I=seq,type_ptr,type,image,foundation,width,height\n");
	const int streamError = std::ferror(pFile);
	const int closeResult = std::fclose(pFile);
	if (headerResult < 0 || columnsRResult < 0 || columnsIResult < 0
		|| streamError != 0 || closeResult != 0)
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
	if (!IsLampType(pType))
		return;

	FILE* pFile = std::fopen(OutputPath, "at");
	if (!pFile)
	{
		Enable = false;
		Debug::Log("[PlacementProbe] Could not append to %s\n", OutputPath);
		return;
	}

	const int writeResult = std::fprintf(pFile,
		"R=%d,%08X,%u,%08X,%.24s,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		Unsorted::CurrentFrame,
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pObject)),
		static_cast<unsigned int>(pObject->UniqueID),
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pType)),
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

void PlacementProbe::RecordIni(BuildingTypeClass* pType)
{
	if (!Enable || IniRowCount >= MaxIniRows || !IsLampType(pType))
		return;

	FILE* pFile = std::fopen(OutputPath, "at");
	if (!pFile)
	{
		Enable = false;
		Debug::Log("[PlacementProbe] Could not append to %s\n", OutputPath);
		return;
	}

	const int writeResult = std::fprintf(pFile,
		"I=%d,%08X,%.24s,%.24s,%d,%d,%d\n",
		IniRowCount,
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(pType)),
		pType->ID,
		pType->ImageFile,
		static_cast<int>(pType->Foundation),
		static_cast<int>(pType->GetFoundationWidth()),
		static_cast<int>(pType->GetFoundationHeight(false)));
	const int streamError = std::ferror(pFile);
	const int closeResult = std::fclose(pFile);
	if (writeResult < 0 || streamError != 0 || closeResult != 0)
	{
		Enable = false;
		Debug::Log("[PlacementProbe] Could not persist an INI row to %s\n", OutputPath);
		return;
	}

	++IniRowCount;
}

DEFINE_HOOK(0x461263, BuildingTypeClass_LoadFromINI_FoundationProbe, 0x5)
{
	if (PlacementProbe::Enable)
		PlacementProbe::RecordIni(R->EBP<BuildingTypeClass*>());

	R->ECX(0x887180);
	return 0;
}

DEFINE_HOOK(0x5F694A, ObjectClass_SetLocation_PlacementProbe, 0x7)
{
	auto* pObject = reinterpret_cast<ObjectClass*>(
		R->ECX() - static_cast<DWORD>(offsetof(ObjectClass, Location)));
	auto* pCoord = R->EAX<const CoordStruct*>();
	if (PlacementProbe::Enable)
		PlacementProbe::Record(pObject, pCoord);

	auto* pLocation = R->ECX<CoordStruct*>();
	pLocation->X = pCoord->X;
	R->EDX(static_cast<DWORD>(pCoord->Y));
	return 0;
}
