#pragma once

#include "Features/ExtendedMaterials.h"
#include "Features/GrassLighting.h"
#include "Features/DynamicCubemaps.h"

struct alignas(16) FeatureBuffer
{
	GrassLighting::Settings grassLightingSettings{};
	ExtendedMaterials::Settings extendedMaterialSettings{};
	DynamicCubemaps::Settings cubemapCreatorSettings{};
};