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
}
