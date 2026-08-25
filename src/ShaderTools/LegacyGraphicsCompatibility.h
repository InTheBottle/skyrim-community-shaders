#pragma once

namespace LegacyGraphicsCompatibility
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

	/** Returns true on previous game versions. */
	[[nodiscard]] bool IsLegacyVersion() noexcept;

	/**
	 * Translates the legacy Utility shadow-filter encoding to Skyrim 1.7's
	 * one-hot selector. Bit 20 remains the legacy grayscale flag outside the
	 * shadow-mask family, but is selector bit 3 for 1.7 shadow masks.
	 */
	[[nodiscard]] constexpr std::uint32_t NormalizeLegacyUtilityDescriptor(std::uint32_t a_descriptor) noexcept
	{
		constexpr std::uint32_t shadowMaskTechniques = 0x01E00000;
		constexpr std::uint32_t selectorMask = 0x001E0000;

		if ((a_descriptor & shadowMaskTechniques) == 0) {
			return a_descriptor;
		}

		auto selector = (a_descriptor >> 17) & 0x7;
		if (selector == 3) {
			selector = 4;
		}
		if (selector != 0 && selector != 1 && selector != 2 && selector != 4) {
			selector = 0;
		}

		return (a_descriptor & ~selectorMask) | (selector << 17);
	}

	static_assert(NormalizeLegacyUtilityDescriptor((1u << 21) | (3u << 17)) ==
				  ((1u << 21) | (4u << 17)));
	static_assert(NormalizeLegacyUtilityDescriptor(1u << 20) == (1u << 20));

	/** Binds the legacy Grass VS PerGeometry block as the shared 1.7 PS contract. */
	void BindLegacyGrassPerGeometryToPixelShader();

	/**
	 * Returns the legacy-only dynamic-sampling selector required by a shared 1.7
	 * image-space shader. This is a pure contract lookup and does not inspect the
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

	/** Installs Skyrim 1.7 changes on previous game versions */
	void Install();
}
