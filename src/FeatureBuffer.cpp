#include "FeatureBuffer.h"

#include "Features/DynamicCubemaps.h"
#include "Features/ExtendedMaterials.h"
#include "Features/GrassLighting.h"

std::pair<unsigned char*, size_t> GetFeatureBufferData()
{
	return _GetFeatureBufferData(
		GrassLighting::GetSingleton()->settings,
		ExtendedMaterials::GetSingleton()->settings,
		DynamicCubemaps::GetSingleton()->settings);
}