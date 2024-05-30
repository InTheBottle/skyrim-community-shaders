#pragma once

#include "Features/GrassLighting.h"
#include "Features/ExtendedMaterials.h"

struct alignas(16) FeatureBuffer
{
	GrassLighting::Settings grassLightingSettings{};
	ExtendedMaterials::Settings extendedMaterialSettings{};
};