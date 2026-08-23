#pragma once

namespace LegacyShaderCompatibility
{
	enum class ImageSpaceSelectorSource
	{
		kAlwaysAdjusted,
		kImageSpaceEffectUnk88
	};

	struct ImageSpaceSelectorTranslation
	{
		std::size_t floatIndex;
		ImageSpaceSelectorSource source;
	};

	/** Returns true when the executable already supplies the native 1.7 shader contract. */
	[[nodiscard]] bool IsNativeLatestContract() noexcept;

	/** Binds the legacy Grass VS PerGeometry block as the shared 1.7 PS contract. */
	void BindLegacyGrassPerGeometryToPixelShader();

	/**
	 * Returns the legacy-only dynamic-sampling selector required by a shared 1.7
	 * image-space shader.  This is a pure contract lookup and does not inspect the
	 * running executable.
	 */
	[[nodiscard]] constexpr std::optional<ImageSpaceSelectorTranslation> GetLegacyImageSpaceSelectorTranslation(
		RE::ImageSpaceManager::ImageSpaceEffectEnum a_effect) noexcept
	{
		using enum RE::ImageSpaceManager::ImageSpaceEffectEnum;

		switch (a_effect) {
		case ISDoubleVision:
			// blurParams.x (b2:c0.x). ImageSpaceEffectGetHit::UpdateParams
			// writes 1.0 in 1.7; the legacy writer left this lane at zero.
			return ImageSpaceSelectorTranslation{ 0, ImageSpaceSelectorSource::kAlwaysAdjusted };
		case ISDepthOfField:
		case ISDepthOfFieldFogged:
		case ISDepthOfFieldMaskedFogged:
		case ISDistantBlur:
		case ISDistantBlurFogged:
		case ISDistantBlurMaskedFogged:
			// invScreenRes.z (b2:c0.z). The 1.7 CPU writer converts the
			// active ImageSpaceEffect::unk88 flag to 0.0/1.0.
			return ImageSpaceSelectorTranslation{ 2, ImageSpaceSelectorSource::kImageSpaceEffectUnk88 };
		case ISRadialBlur:
		case ISRadialBlurMedium:
		case ISRadialBlurHigh:
			// Center.w (b2:c1.w), from the same 1.7 effect flag.
			return ImageSpaceSelectorTranslation{ 7, ImageSpaceSelectorSource::kImageSpaceEffectUnk88 };
		default:
			return std::nullopt;
		}
	}

	[[nodiscard]] constexpr float ResolveLegacyImageSpaceSelector(
		ImageSpaceSelectorTranslation a_translation,
		bool a_imageSpaceEffectUnk88) noexcept
	{
		if (a_translation.source == ImageSpaceSelectorSource::kAlwaysAdjusted || a_imageSpaceEffectUnk88) {
			return 1.0F;
		}
		return 0.0F;
	}

	/**
	 * Applies the pure legacy translation to a reflected pixel constant group.
	 * The caller owns runtime gating and restoration of the prior value.
	 */
	[[nodiscard]] constexpr bool TranslateLegacyImageSpaceConstants(
		RE::ImageSpaceManager::ImageSpaceEffectEnum a_effect,
		bool a_imageSpaceEffectUnk88,
		std::span<float> a_constants) noexcept
	{
		const auto translation = GetLegacyImageSpaceSelectorTranslation(a_effect);
		if (!translation || translation->floatIndex >= a_constants.size()) {
			return false;
		}

		a_constants[translation->floatIndex] =
			ResolveLegacyImageSpaceSelector(*translation, a_imageSpaceEffectUnk88);
		return true;
	}

	/** Installs the legacy adapters for the ten proven image-space contracts. */
	void InstallImageSpaceAdapters();
}
