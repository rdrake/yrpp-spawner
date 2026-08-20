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

#pragma once

#include <GeneralStructures.h>

class ObjectClass;

class PlacementProbe
{
public:
	static bool Enable;
	static int RowCount;
	static constexpr int MaxRows = 16;

	static void Arm(bool enable);
	static void Record(ObjectClass* pObject, const CoordStruct* pCoord);
};
