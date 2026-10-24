#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>

namespace ofxOceanodeTimelineLaneEditorUi {
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

std::vector<std::string> laneDropdownOptions(ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
                                             const std::string& trackId, const ofxOceanodeTimelineLane& lane) {
    if(lane.bindingIds.empty()) return {};
    const auto* binding = timeline.getBinding(trackId, lane.bindingIds.front());
    auto* parameter = binding == nullptr ? nullptr : container.findTimelineParameter(binding->parameterPath);
    return parameter == nullptr ? std::vector<std::string>() : ofxOceanodeTimelineDropdownOptions(*parameter);
}

std::string dropdownOptionLabel(const std::vector<std::string>& options, float value) {
    const int index = static_cast<int>(std::lround(value));
    return index >= 0 && index < static_cast<int>(options.size()) ? options[index] : ofToString(index);
}

int insertLinearCurvePoint(ofxOceanodeTimelineLane& lane,
                           const ofxOceanodeTimelineCurvePoint& point) {
    const size_t oldPointCount = lane.curvePoints.size();
    auto oldTensions = lane.curveTensions;
    oldTensions.resize(oldPointCount > 0 ? oldPointCount - 1 : 0);
    const auto insertion = std::lower_bound(lane.curvePoints.begin(), lane.curvePoints.end(), point.beat,
        [](const auto& existing, double beat) { return existing.beat < beat; });
    const size_t insertionIndex = static_cast<size_t>(insertion - lane.curvePoints.begin());
    lane.curvePoints.insert(insertion, point);

    std::vector<ofxOceanodeTimelineCurveTension> newTensions(
        lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    for(size_t segment = 0; segment < newTensions.size(); ++segment) {
        // Both segments touching the inserted point use the neutral sigmoid.
        if((insertionIndex > 0 && segment == insertionIndex - 1) || segment == insertionIndex) continue;
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
    if(pointIndex >= lane.curvePoints.size()) return;
    lane.curvePoints.erase(lane.curvePoints.begin() + pointIndex);
    // Topology changes should not retain hidden shaping state from segments
    // that no longer exist. The resulting curve starts from a predictable,
    // neutral shape in every interpolation mode.
    resetCurveTensions(lane);
}

void eraseCurvePointKeepingTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex) {
    if(pointIndex >= lane.curvePoints.size()) return;
    auto tensions = lane.curveTensions;
    tensions.resize(lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    lane.curvePoints.erase(lane.curvePoints.begin() + pointIndex);
    if(pointIndex < tensions.size()) tensions.erase(tensions.begin() + pointIndex);
    else if(!tensions.empty()) tensions.pop_back();
    if(pointIndex > 0 && pointIndex - 1 < tensions.size()) tensions[pointIndex - 1] = ofxOceanodeTimelineCurveTension{};
    tensions.resize(lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
    lane.curveTensions = std::move(tensions);
}
float labeledWidgetWidth(float desired, const char* label) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    const float budget = avail - labelWidth - ImGui::GetStyle().ItemInnerSpacing.x - 2.0f;
    return std::max(24.0f, std::min(desired, budget));
}
void drawDivision(ofxOceanodeTimelineLane& lane) {
    int divisionIndex = divisionIndexForBeats(lane.beatsPerStep);
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "Grid"));
    if(ImGui::BeginCombo("Grid", kDivisionOptions[divisionIndex].label)) {
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
