#include "FrameGenController.h"

#include "../HDRDisplay.h"
#include "../Upscaling.h"
#include "DXVKInterop.h"
#include "Streamline.h"

#include "../../Globals.h"
#include "../../Utils/Game.h"

namespace FrameGen
{
	namespace
	{
		Method DesiredMethod()
		{
			auto& upscaling = globals::features::upscaling;
			if (!upscaling.settings.frameGeneration)
				return Method::kNone;
			return upscaling.GetFrameGenMethod() == Upscaling::FrameGenMethod::kDLSSG
			           ? Method::kDLSSG
			           : Method::kFSR;
		}

		uint32_t FSRDebugSignature(const Upscaling::Settings& a_settings)
		{
			return (a_settings.fgDebugView ? 1u : 0u) |
			       (a_settings.fgDebugTearLines ? 2u : 0u) |
			       (a_settings.fgDebugPacingLines ? 4u : 0u) |
			       (a_settings.fgShowOnlyGenerated ? 8u : 0u);
		}

		struct Dims
		{
			uint32_t renderWidth;
			uint32_t renderHeight;
			uint32_t displayWidth;
			uint32_t displayHeight;
		};

		Dims CurrentDims(bool a_ignoreDynamicResolutionLock)
		{
			const auto display = float2{ (float)globals::game::graphicsState->screenWidth,
				(float)globals::game::graphicsState->screenHeight };
			const auto render = Util::ConvertToDynamic(display, a_ignoreDynamicResolutionLock);
			return { (uint32_t)render.x, (uint32_t)render.y, (uint32_t)display.x, (uint32_t)display.y };
		}

		bool IsHDRActive()
		{
			const auto& hdr = globals::features::hdrDisplay;
			return hdr.loaded && hdr.settings.enableHDR;
		}
	}

	const char* Controller::Name(Method a_method)
	{
		switch (a_method) {
		case Method::kFSR:
			return "FSR-FG";
		case Method::kDLSSG:
			return "DLSS-G";
		default:
			return "off";
		}
	}

	void Controller::Reconcile()
	{
		// Wait for settings and hardware fallbacks to settle.
		if (!globals::features::upscaling.loaded ||
			!Streamline::GetSingleton()->IsFeatureSupportResolved())
			return;

		if (Upscaling::IsWindowUnusable())
			return;

		const Method target = DesiredMethod();

		StepPhaseCompletion();
		StepModeTeardown(target);
		StepLoadState(target);
		StepFSRDelivery(target);
	}

	void Controller::StepPhaseCompletion()
	{
		if (phase != Phase::kTransitioning)
			return;

		auto* sl = Streamline::GetSingleton();
		if (!sl->IsDLSSGLoadSettled() || !sl->IsFSRFGLoadSettled())
			return;

		if (sl->IsDLSSGLoaded()) {
			const auto dims = CurrentDims(false);
			sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight);
			owner = Method::kDLSSG;
		} else if (sl->IsFSRFGLoaded()) {
			owner = Method::kFSR;
		} else {
			owner = Method::kNone;
			Streamline::PushDxvkSyncPresent(false);
		}

		phase = Phase::kIdle;
		logger::info("[FrameGen] FG method switch settled - present owner: {}", Name(owner));
	}

	void Controller::StepModeTeardown(Method a_target)
	{
		auto* sl = Streamline::GetSingleton();

		// Streamline requires DLSS-G to be disabled and drained before teardown.
		if (dlssgModeOn && a_target != Method::kDLSSG) {
			const auto dims = CurrentDims(false);
			sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight);
			if (auto* dxvk = DXVKInterop::GetSingleton())
				dxvk->WaitDeviceIdle();

			dlssgModeOn = false;
			logger::info("[FrameGen] DLSS-G interpolation off + device drained (leaving DLSS-G)");
		}

		if (fsrDelivered == 1 && a_target != Method::kFSR) {
			const auto dims = CurrentDims(false);
			const auto& s = globals::features::upscaling.settings;
			(void)sl->SetFSRFrameGen(false, dims.renderWidth, dims.renderHeight,
				dims.displayWidth, dims.displayHeight, fsrHDRDelivered,
				s.fgDebugView, s.fgDebugTearLines, s.fgDebugPacingLines, s.fgShowOnlyGenerated);
			fsrDelivered = 0;
			fsrVsyncRebakePending = false;
			if (owner == Method::kFSR)
				owner = Method::kNone;
			logger::info("[FrameGen] FSR-FG unwrapped (leaving FSR-FG)");
		}
	}

	void Controller::StepLoadState(Method a_target)
	{
		if (phase != Phase::kIdle)
			return;

		auto* sl = Streamline::GetSingleton();
		const bool wantDLSSG = a_target == Method::kDLSSG;
		const bool wantFSRFG = a_target == Method::kFSR;

		// Enable synchronous present before installing either present proxy.
		if (wantDLSSG || wantFSRFG)
			Streamline::PushDxvkSyncPresent(true);

		if (sl->IsDLSSGLoaded() == wantDLSSG && sl->IsFSRFGLoaded() == wantFSRFG) {
			if (wantDLSSG && owner != Method::kDLSSG) {
				const auto dims = CurrentDims(false);
				sl->SetDLSSGMode(false, dims.displayWidth, dims.displayHeight);
				owner = Method::kDLSSG;
				logger::info("[FrameGen] DLSS-G already loaded - registered + adopted as present owner");
			} else if (wantFSRFG && owner != Method::kFSR) {
				owner = Method::kFSR;
				logger::info("[FrameGen] FSR-FG already loaded - adopted as present owner");
			}
			return;
		}

		sl->SetDLSSGDesiredLoaded(wantDLSSG);
		sl->SetFSRFGDesiredLoaded(wantFSRFG);
		Streamline::RequestDxvkSwapchainRecreate("FG method switch");
		phase = Phase::kTransitioning;
		if (owner == Method::kDLSSG && !wantDLSSG)
			owner = Method::kNone;
		if (owner == Method::kFSR && !wantFSRFG)
			owner = Method::kNone;
		logger::info("[FrameGen] FG method switch requested: DLSS-G load={} FSR-FG load={} (swapchain recreate, guide section 18)",
			wantDLSSG, wantFSRFG);
	}

	void Controller::StepFSRDelivery(Method a_target)
	{
		auto& upscaling = globals::features::upscaling;
		auto* sl = Streamline::GetSingleton();
		const bool wantFSR = a_target == Method::kFSR;

		if (!wantFSR || phase != Phase::kIdle || !sl->IsFSRFGLoaded())
			return;

		// Present the new interval once before recreating the FFX-wrapped swapchain.
		if (fsrDelivered == 1 && upscaling.settings.vsync != fsrWrapVsync) {
			if (!fsrVsyncRebakePending) {
				fsrVsyncRebakePending = true;
			} else {
				fsrVsyncRebakePending = false;
				fsrWrapVsync = upscaling.settings.vsync;
				Streamline::RequestDxvkSwapchainRecreate("FSR-FG vsync change");
			}
		} else {
			fsrVsyncRebakePending = false;
		}

		const uint32_t debugSig = FSRDebugSignature(upscaling.settings);
		const bool hdr = IsHDRActive();
		if (fsrDelivered == 1 && debugSig == fsrDebugSigDelivered && hdr == fsrHDRDelivered)
			return;

		const bool enableEdge = fsrDelivered != 1;
		const bool hdrChanged = fsrDelivered == 1 && hdr != fsrHDRDelivered;

		const auto dims = CurrentDims(false);
		const auto& s = upscaling.settings;
		if (sl->SetFSRFrameGen(true,
				dims.renderWidth, dims.renderHeight, dims.displayWidth, dims.displayHeight, hdr,
				s.fgDebugView, s.fgDebugTearLines, s.fgDebugPacingLines, s.fgShowOnlyGenerated)) {
			fsrDelivered = 1;
			fsrDebugSigDelivered = debugSig;
			fsrHDRDelivered = hdr;
			owner = Method::kFSR;
			fsrWrapVsync = s.vsync;
			logger::info("[FrameGen] FSR-FG enable delivered - present owner: {}", Name(owner));

			// FFX installs its interpolation swapchain during vkCreateSwapchainKHR.
			if (enableEdge)
				Streamline::RequestDxvkSwapchainRecreate("FSR-FG wrap");
			else if (hdrChanged)
				Streamline::RequestDxvkSwapchainRecreate("FSR-FG HDR transfer change");
		}
	}

	void Controller::EngageDLSSG()
	{
		auto* sl = Streamline::GetSingleton();
		if (phase != Phase::kIdle || !sl->IsDLSSGLoaded())
			return;

		auto& upscaling = globals::features::upscaling;
		const auto& s = upscaling.settings;

		const auto dims = CurrentDims(true);

		const bool dynamic = s.dlssgDynamic;
		const bool useDynamic = dynamic && sl->IsDLSSGDynamicSupported();
		const bool useAuto = dynamic && !useDynamic;
		const uint32_t numFramesToGenerate = s.frameGenMultiplier > 1 ? s.frameGenMultiplier - 1 : 1;
		const float dynTargetFps = dynamic ? static_cast<float>(upscaling.GetTargetFrameRate()) : 0.0f;

		sl->SetDLSSGMode(true, dims.displayWidth, dims.displayHeight,
			numFramesToGenerate, useAuto, useDynamic, dynTargetFps);
		dlssgModeOn = true;
	}

}
