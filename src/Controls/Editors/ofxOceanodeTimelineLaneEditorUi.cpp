#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>

namespace ofxOceanodeTimelineLaneEditorUi {
bool playbackToggle(const std::string& id, const char* label, bool on, const char* tooltip) {
    ImGui::PushID(id.c_str());
    // Lit, Solo ("S") is black on yellow and Mute ("M") white on red;
    // unlit, the letter is the same white as the track names.
    const bool solo = label[0] == 'S';
    const ImVec4 onColor = solo ? ImVec4(1.0f, 190.0f / 255.0f, 0.0f, 1.0f) : ImVec4(0.72f, 0.20f, 0.20f, 1.0f);
    if(on) {
        ImGui::PushStyleColor(ImGuiCol_Button, onColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(onColor.x * 1.2f, onColor.y * 1.2f, onColor.z * 1.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(onColor.x * 0.85f, onColor.y * 0.85f, onColor.z * 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, solo ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    } else ImGui::PushStyleColor(ImGuiCol_Text, ofxOceanodeTimelineEditorUi::labelTextColor);
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(on ? 4 : 1);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

void drawChanceKnob(ImDrawList* dl, float clipChance, bool active, const ImVec2& center, float radius) {
    // Ring = 100%; the filled arc shows how likely the clip is to play.
    const float chance = ofClamp(clipChance, 0.0f, 1.0f);
    dl->AddCircleFilled(center, radius, IM_COL32(18, 18, 22, chance < 1.0f || active ? 200 : 110));
    if(chance > 0.0f) {
        constexpr float pi = 3.14159265358979323846f;
        const float a0 = -pi * 0.5f;
        dl->PathLineTo(center);
        dl->PathArcTo(center, radius - 1.0f, a0, a0 + pi * 2.0f * chance, 24);
        dl->PathFillConvex(chance < 1.0f ? IM_COL32(255, 205, 90, 230) : IM_COL32(235, 240, 245, 120));
    }
    dl->AddCircle(center, radius, active ? IM_COL32(255, 255, 255, 240) : IM_COL32(235, 240, 245, 150), 20, 1.0f);
}
ImVec2 laneChanceCenter(const ImVec2& min, const ImVec2& max) {
    const float right = std::min(max.x, ImGui::GetWindowDrawList()->GetClipRectMax().x);
    const float titleBottom = std::min(max.y - 3.0f, min.y + 9.0f + ImGui::GetTextLineHeight());
    return ImVec2(right - 15.0f, (min.y + 3.0f + titleBottom) * 0.5f);
}

bool laneChanceControl(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence,
        const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
        ofxOceanodeTimelineLane& lane, const ImVec2& center) {
    constexpr float radius = 6.0f;
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(clip.id.c_str());
    ImGui::PushID(lane.id.c_str());
    ImGui::SetCursorScreenPos(ImVec2(center.x - 8.0f, center.y - 8.0f));
    ImGui::InvisibleButton("##laneChance", ImVec2(16.0f, 16.0f));
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID startValue = ImGui::GetID("chanceStartValue");
    const ImGuiID startY = ImGui::GetID("chanceStartY");
    if(ImGui::IsItemClicked()) {
        storage->SetFloat(startValue, lane.chance);
        storage->SetFloat(startY, ImGui::GetIO().MousePos.y);
    }
    bool changed = false;
    const bool active = ImGui::IsItemActive();
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        changed = timeline.setLaneChance(sequence, track.id, clip.id, lane.id, 1.0f);
    else if(active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float pixelsPerUnit = ImGui::GetIO().KeyShift ? 600.0f : 150.0f;
        const float chance = ofClamp(storage->GetFloat(startValue, lane.chance) +
            (storage->GetFloat(startY) - ImGui::GetIO().MousePos.y) / pixelsPerUnit, 0.0f, 1.0f);
        changed = timeline.setLaneChance(sequence, track.id, clip.id, lane.id, std::round(chance * 100.0f) / 100.0f);
    }
    drawChanceKnob(ImGui::GetWindowDrawList(), lane.chance, active, center, radius);
    if(ImGui::IsItemHovered() || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::SetTooltip("Lane chance: %.0f%%\nCombined with clip: %.0f%%\nDrag vertically. Shift: fine adjustment. Double-click: 100%%.",
                          lane.chance * 100.0f, clip.chance * lane.chance * 100.0f);
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(cursor);
    return changed;
}

bool laneNameSelectable(const std::string& label, bool focused, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Selectable(label.c_str(), focused, ImGuiSelectableFlags_None, size);
    ImGui::PopStyleColor(3);
    return clicked;
}


int divisionIndexForBeats(double beats) {
    int best = 0;
    double bestDistance = std::abs(beats - kDivisionOptions[0].beats);
    for(int i = 1; i < kDivisionOptionCount; ++i) {
        const double distance = std::abs(beats - kDivisionOptions[i].beats);
        if(distance < bestDistance) { best = i; bestDistance = distance; }
    }
    return best;
}

std::string compactParameterName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

ImU32 mutedTrackColor(const ofColor& color, float brightness, float saturation) {
    const float luminance = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
    auto channel = [&](float value) {
        const float desaturated = luminance + (value - luminance) * saturation;
        return static_cast<int>(ofClamp(9.0f + desaturated * brightness, 0.0f, 255.0f));
    };
    return IM_COL32(channel(color.r), channel(color.g), channel(color.b), 255);
}

bool hoverRect(const ImVec2& min, const ImVec2& max) {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
           ImGui::IsMouseHoveringRect(min, max);
}

std::vector<std::string> laneDropdownOptions(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineSequence& sequence, ofxOceanodeContainer& container,
                                             const std::string& trackId, const ofxOceanodeTimelineLane& lane) {
    if(lane.bindingIds.empty()) return {};
    const auto* binding = timeline.getBinding(sequence, trackId, lane.bindingIds.front());
    auto* parameter = binding == nullptr ? nullptr : container.findTimelineParameter(binding->parameterPath);
    return parameter == nullptr ? std::vector<std::string>() : ofxOceanodeTimelineDropdownOptions(*parameter);
}

std::string dropdownOptionLabel(const std::vector<std::string>& options, float value) {
    const int index = static_cast<int>(std::lround(value));
    if(index < 0 || index >= static_cast<int>(options.size())) return ofToString(index);
    const auto& label = options[index];
    // Some sources include the option index in their labels (e.g. "0: Name").
    const std::string prefix = ofToString(index) + ": ";
    return label.compare(0, prefix.size(), prefix) == 0 ? label.substr(prefix.size()) : label;
}

int insertCurvePoint(ofxOceanodeTimelineLane& lane,
                     const ofxOceanodeTimelineCurvePoint& point) {
    const size_t oldPointCount = lane.curvePoints.size();
    auto oldTensions = lane.curveTensions;
    oldTensions.resize(oldPointCount > 0 ? oldPointCount - 1 : 0);
    const auto insertion = std::lower_bound(lane.curvePoints.begin(), lane.curvePoints.end(), point.beat,
        [](const auto& existing, double beat) { return existing.beat < beat; });
    const size_t insertionIndex = static_cast<size_t>(insertion - lane.curvePoints.begin());
    // A point inside a segment cuts it in two: each part keeps its piece of
    // the segment's bend (Sigmoid, Log/Exp) instead of becoming a straight line.
    ofxOceanodeTimelineCurveTension before{}, after{};
    if(insertionIndex > 0 && insertionIndex < oldPointCount) {
        before = ofxOceanodeTimelineCurve::fitSegmentShape(lane.curvePoints, oldTensions, lane.curveInterpolation,
                                                           lane.curvePoints[insertionIndex - 1].beat, point.beat);
        after = ofxOceanodeTimelineCurve::fitSegmentShape(lane.curvePoints, oldTensions, lane.curveInterpolation,
                                                          point.beat, lane.curvePoints[insertionIndex].beat);
    }
    lane.curvePoints.insert(insertion, point);

    std::vector<ofxOceanodeTimelineCurveTension> newTensions(
        lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    for(size_t segment = 0; segment < newTensions.size(); ++segment) {
        if(insertionIndex > 0 && segment == insertionIndex - 1) { newTensions[segment] = before; continue; }
        if(segment == insertionIndex) { newTensions[segment] = after; continue; }
        const size_t oldSegment = segment < insertionIndex ? segment : segment - 1;
        if(oldSegment < oldTensions.size()) newTensions[segment] = oldTensions[oldSegment];
    }
    lane.curveTensions = std::move(newTensions);
    return static_cast<int>(insertionIndex);
}

int curveSnapLevels(const ofxOceanodeTimelineLane& lane) {
    return lane.valueSnap && lane.valueQuantizeSteps >= 2 ? lane.valueQuantizeSteps : 0;
}

void resetCurveTensions(ofxOceanodeTimelineLane& lane) {
    lane.curveTensions.assign(lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0,
                              ofxOceanodeTimelineCurveTension{});
}

void eraseCurvePointWithTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex) {
    eraseCurvePointKeepingTensions(lane, pointIndex);
}

void eraseCurvePointKeepingTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex) {
    if(pointIndex >= lane.curvePoints.size()) return;
    auto tensions = lane.curveTensions;
    tensions.resize(lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    lane.curvePoints.erase(lane.curvePoints.begin() + pointIndex);
    if(pointIndex < tensions.size()) tensions.erase(tensions.begin() + pointIndex);
    else if(!tensions.empty()) tensions.pop_back();
    // An interior deletion merges two spans. Keep the incoming segment's
    // settings on the merged span; every other segment retains its settings.
    tensions.resize(lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    lane.curveTensions = std::move(tensions);
}
void propertyLabel(const char* label, float reserveRight) {
    const float startX = ImGui::GetCursorPosX();
    const float textWidth = ImGui::CalcTextSize(label).x;
    const float column = std::max(std::min(64.0f, ImGui::GetContentRegionAvail().x * 0.36f),
                                  textWidth + ImGui::GetStyle().ItemSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(startX + column);
    ImGui::SetNextItemWidth(std::max(24.0f, ImGui::GetContentRegionAvail().x - reserveRight));
}
bool pitchClassKeyboard(const char* id, int& mask, ImU32 onColor, float height) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const int whites[] = {0, 2, 4, 5, 7, 9, 11};
    static const int blacks[] = {1, 3, 6, 8, 10};
    static const float blackAfter[] = {0, 1, 3, 4, 5}; // white key each black one follows
    const float width = std::max(70.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float whiteWidth = width / 7.0f, blackWidth = whiteWidth * 0.62f, blackHeight = height * 0.6f;
    auto blackRect = [&](int i, ImVec2& a, ImVec2& b) {
        const float x = origin.x + (blackAfter[i] + 1.0f) * whiteWidth - blackWidth * 0.5f;
        a = ImVec2(x, origin.y); b = ImVec2(x + blackWidth, origin.y + blackHeight);
    };
    // Black keys sit on top, so they take the hit first.
    int hoveredClass = -1;
    if(hovered) {
        for(int i = 0; i < 5 && hoveredClass < 0; ++i) {
            ImVec2 a, b; blackRect(i, a, b);
            if(mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) hoveredClass = blacks[i];
        }
        if(hoveredClass < 0) hoveredClass = whites[std::clamp(static_cast<int>((mouse.x - origin.x) / whiteWidth), 0, 6)];
    }
    bool changed = false;
    if(hoveredClass >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        mask ^= 1 << hoveredClass;
        changed = true;
    }
    if(hoveredClass >= 0)
        ImGui::SetTooltip("%s: %s. Click to toggle.", names[hoveredClass], (mask >> hoveredClass) & 1 ? "allowed" : "not allowed");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for(int i = 0; i < 7; ++i) {
        const int pitchClass = whites[i];
        const ImVec2 a(origin.x + i * whiteWidth, origin.y), b(origin.x + (i + 1) * whiteWidth - 1.0f, origin.y + height);
        const bool on = (mask >> pitchClass) & 1;
        dl->AddRectFilled(a, b, on ? onColor : IM_COL32(205, 205, 200, 255), 2.0f);
        if(pitchClass == hoveredClass) dl->AddRect(a, b, IM_COL32(255, 255, 255, 230), 2.0f, 0, 1.5f);
        if(pitchClass == 0) dl->AddText(ImVec2(a.x + 3.0f, b.y - ImGui::GetTextLineHeight() - 1.0f), IM_COL32(40, 40, 45, 230), "C");
    }
    for(int i = 0; i < 5; ++i) {
        ImVec2 a, b; blackRect(i, a, b);
        const bool on = (mask >> blacks[i]) & 1;
        dl->AddRectFilled(a, b, on ? onColor : IM_COL32(35, 35, 38, 255), 2.0f);
        dl->AddRect(a, b, blacks[i] == hoveredClass ? IM_COL32(255, 255, 255, 230) : IM_COL32(20, 20, 22, 255), 2.0f, 0, 1.0f);
    }
    return changed;
}
std::string fitText(const std::string& text, float maxWidth) {
    if(ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    std::string shortened = text;
    while(!shortened.empty() && ImGui::CalcTextSize((shortened + "...").c_str()).x > maxWidth) shortened.pop_back();
    return shortened + "...";
}
float smallButtonReserve(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f +
           ImGui::GetStyle().ItemSpacing.x;
}
void drawDivision(ofxOceanodeTimelineLane& lane) {
    int divisionIndex = divisionIndexForBeats(lane.beatsPerStep);
    propertyLabel("Grid");
    if(ImGui::BeginCombo("##Grid", kDivisionOptions[divisionIndex].label)) {
        for(int i = 1; i < kDivisionOptionCount; ++i) {
            if(ImGui::Selectable(kDivisionOptions[i].label, i == divisionIndex)) {
                divisionIndex = i;
                lane.beatsPerStep = kDivisionOptions[i].beats;
            }
        }
        ImGui::EndCombo();
    }
}
}
