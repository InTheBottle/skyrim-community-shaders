#include "VectorIcons.h"

#include <array>
#include <imgui_internal.h>

#include "Utils/UI.h"

namespace
{
	// Bottle outline, normalised so height == 1; width == kBottleAspect.
	constexpr std::array<ImVec2, 33> kBottleOutline = { {
		ImVec2(0.2438f, 0.0098f), ImVec2(0.2586f, 0.0465f), ImVec2(0.2594f, 0.1682f),
		ImVec2(0.2646f, 0.2165f), ImVec2(0.2825f, 0.2437f), ImVec2(0.3452f, 0.2961f),
		ImVec2(0.3612f, 0.3322f), ImVec2(0.3684f, 0.3958f), ImVec2(0.3689f, 0.4821f),
		ImVec2(0.3668f, 0.7342f), ImVec2(0.3602f, 0.8924f), ImVec2(0.3481f, 0.9448f),
		ImVec2(0.3373f, 0.9595f), ImVec2(0.3179f, 0.9739f), ImVec2(0.2518f, 0.9932f),
		ImVec2(0.1429f, 0.9992f), ImVec2(0.0859f, 0.9881f), ImVec2(0.0390f, 0.9710f),
		ImVec2(0.0132f, 0.9481f), ImVec2(0.0034f, 0.9233f), ImVec2(0.0007f, 0.8587f),
		ImVec2(0.0173f, 0.6572f), ImVec2(0.0101f, 0.4469f), ImVec2(0.0144f, 0.3723f),
		ImVec2(0.0200f, 0.3267f), ImVec2(0.0286f, 0.3073f), ImVec2(0.0917f, 0.2513f),
		ImVec2(0.1094f, 0.2197f), ImVec2(0.1139f, 0.0568f), ImVec2(0.1267f, 0.0232f),
		ImVec2(0.1412f, 0.0095f), ImVec2(0.1624f, 0.0020f), ImVec2(0.2137f, 0.0007f) } };

	constexpr std::array<ImVec2, 8> kBottleCap = { {
		ImVec2(0.1592f, 0.0560f), ImVec2(0.1621f, 0.0422f), ImVec2(0.1714f, 0.0331f),
		ImVec2(0.2160f, 0.0314f), ImVec2(0.2122f, 0.0490f), ImVec2(0.2019f, 0.0580f),
		ImVec2(0.1803f, 0.0606f), ImVec2(0.1592f, 0.0560f) } };

	// The action icons are transcribed from 24x24 artwork with a 2px stroke.
	constexpr float kSourceGrid = 24.0f;
	constexpr float kSourceStroke = 2.0f;

	constexpr float kIconStrokeRatio = kSourceStroke / kSourceGrid;
	constexpr float kBottleStrokeRatio = 0.075f;

	// Below this height the cap detail collapses into a blob, so it is dropped.
	constexpr float kBottleCapMinHeight = 40.0f;

	float ResolveThickness(float thickness, float reference, float ratio)
	{
		return thickness > 0.0f ? thickness : ImMax(1.0f, reference * ratio);
	}

	/** @brief Maps a point from the 24x24 source grid into the destination square. */
	struct GridMapper
	{
		ImVec2 origin;
		float scale;

		ImVec2 operator()(float x, float y) const
		{
			return ImVec2(origin.x + x * scale, origin.y + y * scale);
		}
	};

	void StrokePolyline(ImDrawList* drawList, const ImVec2* points, int count, ImU32 color, float thickness, bool closed)
	{
		drawList->AddPolyline(points, count, color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
	}

	void DrawSaveIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		// Outer body: notched top-left corner, clipped top-right corner.
		const std::array<ImVec2, 7> body = { { g(8, 4), g(4, 4), g(4, 20), g(20, 20), g(20, 8), g(16, 4), g(14, 4) } };
		StrokePolyline(drawList, body.data(), static_cast<int>(body.size()), color, thickness, false);

		// Shutter.
		const std::array<ImVec2, 4> shutter = { { g(8, 4), g(8, 8), g(14, 8), g(14, 4) } };
		StrokePolyline(drawList, shutter.data(), static_cast<int>(shutter.size()), color, thickness, true);

		// Label knob.
		drawList->AddCircle(g(12, 14), 2.0f * g.scale, color, 0, thickness);
	}

	void DrawReloadIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		// Three-quarter ring opening at the top, then a tail running off to the right.
		drawList->PathArcTo(g(11.5f, 13.5f), 7.5f * g.scale, 0.0f, IM_PI * 1.5f, 32);
		drawList->PathLineTo(g(20, 6));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		// Arrow head on the tail.
		const std::array<ImVec2, 3> head = { { g(17, 3), g(20, 6), g(17, 9) } };
		StrokePolyline(drawList, head.data(), static_cast<int>(head.size()), color, thickness, false);
	}

	void DrawRefreshIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		drawList->AddCircle(g(12, 12), 10.0f * g.scale, color, 0, thickness);

		// Upper arm: runs right, then turns down.
		drawList->PathLineTo(g(8, 8));
		drawList->PathLineTo(g(14, 8));
		drawList->PathArcTo(g(14, 10), 2.0f * g.scale, -IM_PI * 0.5f, 0.0f, 8);
		drawList->PathLineTo(g(16, 11));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		const std::array<ImVec2, 3> upperHead = { { g(10, 6), g(8, 8), g(10, 10) } };
		StrokePolyline(drawList, upperHead.data(), static_cast<int>(upperHead.size()), color, thickness, false);

		// Lower arm: runs left, then turns up.
		drawList->PathLineTo(g(16, 16));
		drawList->PathLineTo(g(10, 16));
		drawList->PathArcTo(g(10, 14), 2.0f * g.scale, IM_PI * 0.5f, IM_PI, 8);
		drawList->PathLineTo(g(8, 13));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		const std::array<ImVec2, 3> lowerHead = { { g(14, 18), g(16, 16), g(14, 14) } };
		StrokePolyline(drawList, lowerHead.data(), static_cast<int>(lowerHead.size()), color, thickness, false);
	}

	/** @brief Width of an icon + label group, or of the label alone when there is no icon. */
	float MeasureIconLabel(Util::Icons::Kind kind, const char* label, float iconSize, float gap)
	{
		const float textWidth = ImGui::CalcTextSize(label, nullptr, true).x;
		return kind == Util::Icons::Kind::None ? textWidth : iconSize + gap + textWidth;
	}
}

namespace Util::Icons
{
	void DrawBottle(ImDrawList* drawList, const ImVec2& topLeft, float height, ImU32 color, float thickness)
	{
		if (!drawList || height <= 0.0f)
			return;

		const float stroke = ResolveThickness(thickness, height, kBottleStrokeRatio);

		std::array<ImVec2, kBottleOutline.size()> outline{};
		for (size_t i = 0; i < kBottleOutline.size(); ++i)
			outline[i] = ImVec2(topLeft.x + kBottleOutline[i].x * height, topLeft.y + kBottleOutline[i].y * height);
		StrokePolyline(drawList, outline.data(), static_cast<int>(outline.size()), color, stroke, true);

		if (height >= kBottleCapMinHeight) {
			std::array<ImVec2, kBottleCap.size()> cap{};
			for (size_t i = 0; i < kBottleCap.size(); ++i)
				cap[i] = ImVec2(topLeft.x + kBottleCap[i].x * height, topLeft.y + kBottleCap[i].y * height);
			StrokePolyline(drawList, cap.data(), static_cast<int>(cap.size()), color, stroke * 0.8f, true);
		}
	}

	void Draw(ImDrawList* drawList, Kind kind, const ImVec2& topLeft, float size, ImU32 color, float thickness)
	{
		if (!drawList || size <= 0.0f || kind == Kind::None)
			return;

		const GridMapper g{ topLeft, size / kSourceGrid };
		const float stroke = ResolveThickness(thickness, size, kIconStrokeRatio);

		switch (kind) {
		case Kind::Save:
			DrawSaveIcon(drawList, g, color, stroke);
			break;
		case Kind::Reload:
			DrawReloadIcon(drawList, g, color, stroke);
			break;
		case Kind::Refresh:
			DrawRefreshIcon(drawList, g, color, stroke);
			break;
		default:
			break;
		}
	}

	bool Button(const char* label, Kind kind, const ImVec2& size)
	{
		const auto& style = ImGui::GetStyle();
		const float iconSize = ImGui::GetFontSize();
		const float gap = style.ItemInnerSpacing.x;
		const float contentWidth = MeasureIconLabel(kind, label, iconSize, gap);

		ImVec2 buttonSize = size;
		if (buttonSize.x == 0.0f)
			buttonSize.x = contentWidth + style.FramePadding.x * 2.0f;
		if (buttonSize.y == 0.0f)
			buttonSize.y = ImGui::GetFrameHeight();

		// An empty label stops ImGui drawing the text itself; the ID still comes from `label`.
		ImGui::PushID(label);
		const bool pressed = ImGui::Button("##IconButton", buttonSize);
		ImGui::PopID();

		const ImVec2 rectMin = ImGui::GetItemRectMin();
		const ImVec2 rectSize = ImGui::GetItemRectSize();
		const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		float cursorX = rectMin.x + ImMax(style.FramePadding.x, (rectSize.x - contentWidth) * style.ButtonTextAlign.x);
		const float centerY = rectMin.y + rectSize.y * 0.5f;

		if (kind != Kind::None) {
			Draw(drawList, kind, ImVec2(cursorX, centerY - iconSize * 0.5f), iconSize, textColor);
			cursorX += iconSize + gap;
		}

		drawList->AddText(ImVec2(cursorX, centerY - ImGui::GetTextLineHeight() * 0.5f), textColor, label,
			ImGui::FindRenderedTextEnd(label));

		return pressed;
	}

	bool ButtonWithFlash(const char* label, Kind kind, const ImVec2& size, int flashDurationMs)
	{
		Util::ButtonFlashGuard flashGuard(label, flashDurationMs);
		const bool pressed = Button(label, kind, size);
		if (pressed)
			Util::NotifyButtonFlash(label);
		return pressed;
	}
}
