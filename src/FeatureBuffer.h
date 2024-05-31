#pragma once

#include "Features/ExtendedMaterials.h"
#include "Features/GrassLighting.h"

struct alignas(16) FeatureBuffer
{
	GrassLighting::Settings grassLightingSettings{};
	ExtendedMaterials::Settings extendedMaterialSettings{};
};