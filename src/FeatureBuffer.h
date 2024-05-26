#pragma once

#include "Features/GrassLighting.h"

struct alignas(16) FeatureBuffer
{
	GrassLighting::Settings grassLightingSettings{};
};