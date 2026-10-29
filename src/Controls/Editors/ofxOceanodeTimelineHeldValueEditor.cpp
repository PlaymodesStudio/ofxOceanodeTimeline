#include "ofxOceanodeTimelineHeldValueEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

void ofxOceanodeTimelineHeldValueEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    auto* container = &context.container;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    const bool isFocused = context.focused;
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float left = canvas.left, right = canvas.right;
    const bool editorCanvasHovered = canvas.hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Number: free values. Dropdown: option indices of the bound
    // parameter, shown by name and picked from its list.
    const bool dropdown = lane->type == ofxOceanodeTimelineLaneType::Dropdown;
    const auto options = dropdown ? laneDropdownOptions(timeline, sequence, *container, track.id, *lane) : std::vector<std::string>();
    const char* valuePopup = dropdown ? "Dropdown option" : "Number value";
    const double content = sourceDuration(*clip);
    const double cycleLength = cycleDuration(*clip);
    const int cycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleLength))) : 1;
    const float top = editorMin.y + 8.0f, bottom = editorMax.y - 8.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double mouseBeat = beatAtOffset(mouse.x - timelineMin.x);
    const double sourceBeat = timelineToSourceBeat(*clip, mouseBeat);
    const int hoveredCycle = clip->repeatContent ? std::max(0, static_cast<int>(std::floor(
        std::max(0.0, mouseBeat - clip->startBeat) / cycleLength))) : 0;
    int hit = -1;
    for(int cycle = 0; cycle < cycles; ++cycle) {
        for(size_t i = 0; i < lane->curvePoints.size(); ++i) {
            const auto& point = lane->curvePoints[i];
            const float x = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, point.beat, cycle));
            const float endX = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip,
                i + 1 < lane->curvePoints.size() ? lane->curvePoints[i + 1].beat : content, cycle));
            ImU32 color = IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 65 : 30);
            // a hue per option, so repeated choices read at a glance
            if(dropdown) {
                const int optionCount = std::max(1, static_cast<int>(options.size()));
                const int option = std::max(0, static_cast<int>(std::lround(point.value)));
                color = ImColor::HSV(static_cast<float>(option % optionCount) / optionCount, 0.5f, 0.75f,
                                     cycle == 0 ? 0.4f : 0.18f);
            }
            dl->AddRectFilled(ImVec2(x, top), ImVec2(endX, bottom), color);
            dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(235, 235, 235, 210), 2.0f);
            dl->PushClipRect(ImVec2(std::max(left, x + 3.0f), top), ImVec2(std::min(right, endX), bottom), true);
            const std::string label = dropdown ? dropdownOptionLabel(options, point.value) : ofToString(point.value, 6);
            dl->AddText(ImVec2(x + 6.0f, top + 6.0f), IM_COL32(245, 245, 245, 235), label.c_str());
            dl->PopClipRect();
            if(cycle == hoveredCycle && std::abs(mouse.x - x) <= 8.0f) hit = static_cast<int>(i);
        }
    }
    if(isFocused) {
        const bool hovered = editorCanvasHovered && !ImGui::IsPopupOpen(valuePopup) &&
            hoverRect(ImVec2(left, top), ImVec2(right, bottom));
        if(hovered) ImGui::SetTooltip(dropdown
            ? "Click to select an option from here on. Drag a marker to move it.\nDouble-click or right-click a marker to change it. Shift-click to delete."
            : "Click to add a number. Drag a marker to move it.\nDouble-click or right-click a marker to edit. Shift-click to delete.");
        if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            curveDragPointIndex = hit;
            if(hit >= 0 && ImGui::GetIO().KeyShift) {
                eraseCurvePointWithTensions(*lane, static_cast<size_t>(hit));
                timeline.invalidateSchedule();
                curveDragPointIndex = -1;
            } else if(hit < 0 && !ImGui::GetIO().KeyShift) {
                const double beat = std::max(0.0, std::min(content, view.snapBeat(sourceBeat)));
                const float value = ofxOceanodeTimelineCurve::valueAtBeat(lane->curvePoints, {}, "Step", beat, 0.0f);
                timeline.setClipNumber(sequence, track.id, clip->id, lane->id, beat, value);
                for(size_t i = 0; i < lane->curvePoints.size(); ++i)
                    if(std::abs(lane->curvePoints[i].beat - beat) < 1e-6) curveValuePointIndex = static_cast<int>(i);
                curveNumericValue = value;
                ImGui::OpenPopup(valuePopup);
            }
        }
        if(hovered && hit >= 0 && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            curveValuePointIndex = hit;
            curveNumericValue = lane->curvePoints[hit].value;
            curveDragPointIndex = -1;
            ImGui::OpenPopup(valuePopup);
        }
        if(curveDragPointIndex >= 0 && curveDragPointIndex < static_cast<int>(lane->curvePoints.size()) &&
           ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsPopupOpen(valuePopup)) {
            const int i = curveDragPointIndex;
            const double minimum = i > 0 ? lane->curvePoints[i - 1].beat + 1.0 / kPPQ : 0.0;
            const double maximum = i + 1 < static_cast<int>(lane->curvePoints.size()) ? lane->curvePoints[i + 1].beat - 1.0 / kPPQ : content;
            lane->curvePoints[i].beat = std::max(minimum, std::min(std::max(minimum, maximum), view.snapBeat(sourceBeat)));
            timeline.invalidateSchedule();
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) curveDragPointIndex = -1;
        if(ImGui::BeginPopup(valuePopup)) {
            if(curveValuePointIndex >= 0 && curveValuePointIndex < static_cast<int>(lane->curvePoints.size())) {
                auto& point = lane->curvePoints[curveValuePointIndex];
                if(dropdown && !options.empty()) {
                    ImGui::TextUnformatted("Option");
                    const int current = static_cast<int>(std::lround(point.value));
                    for(int option = 0; option < static_cast<int>(options.size()); ++option) {
                        ImGui::PushID(option);
                        if(ImGui::Selectable(options[option].c_str(), option == current)) {
                            point.value = static_cast<float>(option);
                            timeline.invalidateSchedule();
                            ImGui::CloseCurrentPopup();
                        }
                        if(option == current && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                } else if(dropdown) {
                    // the bound parameter is missing or has no options
                    ImGui::TextDisabled("No dropdown options bound");
                    int index = static_cast<int>(std::lround(point.value));
                    ImGui::SetNextItemWidth(140.0f);
                    if(ImGui::InputInt("Index", &index)) {
                        point.value = static_cast<float>(std::max(0, index));
                        timeline.invalidateSchedule();
                    }
                } else {
                    ImGui::TextUnformatted("Number");
                    ImGui::SetNextItemWidth(140.0f);
                    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    if(ImGui::InputFloat("Value", &curveNumericValue, 0.0f, 0.0f, "%.9g") && std::isfinite(curveNumericValue)) {
                        point.value = curveNumericValue;
                        timeline.invalidateSchedule();
                    }
                }
                float beat = static_cast<float>(point.beat);
                ImGui::SetNextItemWidth(140.0f);
                if(ImGui::InputFloat("Beat", &beat, 0.0f, 0.0f, "%.6g") && std::isfinite(beat)) {
                    const double minimum = curveValuePointIndex > 0 ? lane->curvePoints[curveValuePointIndex - 1].beat + 1.0 / kPPQ : 0.0;
                    const double maximum = curveValuePointIndex + 1 < static_cast<int>(lane->curvePoints.size()) ? lane->curvePoints[curveValuePointIndex + 1].beat - 1.0 / kPPQ : content;
                    point.beat = std::max(minimum, std::min(std::max(minimum, maximum), static_cast<double>(beat)));
                    timeline.invalidateSchedule();
                }
                if(ImGui::MenuItem(dropdown ? "Delete option change" : "Delete number")) {
                    eraseCurvePointWithTensions(*lane, static_cast<size_t>(curveValuePointIndex));
                    curveValuePointIndex = -1;
                    timeline.invalidateSchedule();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

}

void ofxOceanodeTimelineHeldValueEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    auto& timeline = preview.timeline;
    auto& sequence = preview.sequence;
    auto* container = &preview.container;
    const auto& track = preview.track;
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool dropdown = lane->type == ofxOceanodeTimelineLaneType::Dropdown;
    const auto options = dropdown ? laneDropdownOptions(timeline, sequence, *container, track.id, *lane) : std::vector<std::string>();
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    const int cycles = clip.repeatContent ? static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip))) : 1;
    for(int cycle = firstVisibleCycle; cycle < cycles && cycle <= lastVisibleCycle; ++cycle) {
        for(const auto& point : lane->curvePoints) {
            const float x = min.x + beatOffset(sourceToTimelineBeat(clip, point.beat, cycle));
            dl->AddLine(ImVec2(x, min.y + 4.0f), ImVec2(x, max.y - 4.0f), IM_COL32(245, 245, 245, 160));
            const std::string label = dropdown ? dropdownOptionLabel(options, point.value) : ofToString(point.value, 4);
            dl->AddText(ImVec2(x + 4.0f, min.y + 4.0f), IM_COL32(245, 245, 245, 220), label.c_str());
        }
    }
    dl->PopClipRect();
    return;

}
