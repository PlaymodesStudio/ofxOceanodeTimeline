#pragma once

#include "imgui.h"

// Shared layout/style vocabulary; no interaction state or model mutations.
namespace ofxOceanodeTimelineEditorUi {
// Width of the pinned left column (track pads, lane properties). The user
// resizes it from the ruler; the controller copies it from the view state
// at the start of every draw.
constexpr float defaultLabelWidth = 280.0f;
constexpr float minLabelWidth = 200.0f;
constexpr float maxLabelWidth = 520.0f;
inline float labelWidth = defaultLabelWidth;
constexpr ImU32 gridColor = IM_COL32(70, 70, 70, 115);
constexpr ImU32 barColor = IM_COL32(125, 125, 125, 190);
constexpr ImU32 playheadColor = IM_COL32(255, 85, 85, 255);
// Track names, fold arrows and the unlit + / M / S buttons share one white.
constexpr ImU32 labelTextColor = IM_COL32(235, 235, 235, 255);
constexpr const char* curveInterpolationNames[] = {"Step", "Linear", "Log / Exp", "Sigmoid"};

inline void finishAbsoluteLayout(const ImVec2& position) {
    // Submit an item so an absolutely positioned child grows safely.
    ImGui::SetCursorScreenPos(position);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

inline void contextMenuSection(const char* label, bool separator = true) {
    if(separator) ImGui::Separator();
    ImGui::TextDisabled("%s", label);
}

// A fold arrow: a solid triangle pointing right (folded) or down (open),
// centred in the font-sized square whose top-left corner is pos.
inline void drawFoldArrow(ImDrawList* dl, const ImVec2& pos, bool open, ImU32 color = labelTextColor) {
    const float size = ImGui::GetFontSize();
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.3f;
    if(open) dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.8f), color);
    else dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x + r * 0.8f, c.y), ImVec2(c.x - r * 0.6f, c.y + r), color);
}

// A SmallButton-sized button showing a fold arrow; id is an ImGui id ("##...").
inline bool foldButton(const char* id, bool open) {
    const float size = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool clicked = ImGui::Button(id, ImVec2(size + ImGui::GetStyle().FramePadding.x, size));
    ImGui::PopStyleVar();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    drawFoldArrow(ImGui::GetWindowDrawList(), ImVec2((min.x + max.x - size) * 0.5f, min.y), open);
    return clicked;
}
}
