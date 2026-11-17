#include "ofxOceanodeTimelineMultiCurveEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

namespace {
void ensureCurves(ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineClip& clip) {
    if(lane.multiCurves.empty()) ofxOceanodeTimelineSetMultiCurveCount(lane, 1, clip.contentDurationBeats);
    else ofxOceanodeTimelineSyncMultiCurves(lane);
}
}

int ofxOceanodeTimelineMultiCurveEditor::activeCurve(const ofxOceanodeTimelineLane& lane) const {
    const auto it = activeCurves.find(lane.id);
    const int last = std::max(0, static_cast<int>(lane.multiCurves.size()) - 1);
    return it == activeCurves.end() ? 0 : std::max(0, std::min(last, it->second));
}

bool ofxOceanodeTimelineMultiCurveEditor::hasPointSelection(const std::string& laneId) const {
    const auto it = activeCurves.find(laneId);
    return curve.hasPointSelection(ofxOceanodeTimelineMultiCurveKey(laneId, it == activeCurves.end() ? 0 : it->second));
}

void ofxOceanodeTimelineMultiCurveEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& lane = context.lane;
    ensureCurves(lane, context.clip);

    int curves = static_cast<int>(lane.multiCurves.size());
    propertyLabel("Curves");
    // On Enter (or - / +), never while typing: a passing "1" would drop the rest.
    if(ImGui::InputInt("##multiCurveCount", &curves, 1, 1, ImGuiInputTextFlags_EnterReturnsTrue))
        ofxOceanodeTimelineSetMultiCurveCount(lane, curves, context.clip.contentDurationBeats);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("One curve per element of the vector it drives (a single value takes the first).\n"
                          "Fewer: the last curves are removed.");

    int interpolationMode = static_cast<int>(curveInterpolationMode(lane.curveInterpolation));
    propertyLabel("Mode");
    if(ImGui::Combo("##multiCurveInterpolation", &interpolationMode, kCurveInterpolationNames, 4)) {
        lane.curveInterpolation = kCurveInterpolationNames[interpolationMode];
        for(auto& each : lane.multiCurves) resetCurveTensions(each);
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How every curve of the lane runs between its points");
    ImGui::Checkbox("Clamp to range", &lane.curveClamp);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Clamp curve values to the configured Range min..max");
    if(ImGui::Checkbox("Snap##multiCurveValueSnap", &lane.valueSnap) && lane.valueSnap && lane.valueQuantizeSteps < 2)
        lane.valueQuantizeSteps = 8; // sensible first grid
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap point values to evenly spaced levels across the range");
    ImGui::SameLine();
    int steps = std::max(2, lane.valueQuantizeSteps);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if(!lane.valueSnap) ImGui::BeginDisabled();
    if(ImGui::DragInt("##multiCurveValueSteps", &steps, 0.1f, 2, 128, "%d steps"))
        lane.valueQuantizeSteps = std::max(2, std::min(128, steps));
    if(!lane.valueSnap) ImGui::EndDisabled();
    ofxOceanodeTimelineSyncMultiCurves(lane);
}

void ofxOceanodeTimelineMultiCurveEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                                                     const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& lane = context.lane;
    const auto& clip = context.clip;
    const auto& track = context.track;
    const auto& view = context.view;
    ensureCurves(lane, clip);
    int active = activeCurve(lane);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The tab strip is pinned to the visible left of the clip; the curve
    // canvas starts below it, so a click on a tab never lands on the curve.
    const float tabHeight = ImGui::GetTextLineHeight() + 4.0f;
    const float stripHeight = std::min(tabHeight + 4.0f, std::max(0.0f, canvas.max.y - canvas.min.y - 40.0f));
    const bool stripVisible = stripHeight >= tabHeight;
    auto inner = canvas;
    if(stripVisible) inner.min.y += stripHeight;

    const ImU32 trackInk = IM_COL32(track.color.r, track.color.g, track.color.b, 255);
    const int count = static_cast<int>(lane.multiCurves.size());
    int hoveredTab = -1;
    if(stripVisible) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool popupOpen = ImGui::IsPopupOpen("Curve point value") || ImGui::IsPopupOpen("Multi curve tab");
        float x = canvas.left + 4.0f;
        const float y = canvas.min.y + 2.0f;
        // One tab per curve, then "+" while another fits.
        for(int tab = 0; tab <= count; ++tab) {
            const bool addTab = tab == count;
            if(addTab && count >= kOfxOceanodeTimelineMaxMultiCurves) break;
            char label[8];
            if(addTab) std::snprintf(label, sizeof(label), "+");
            else std::snprintf(label, sizeof(label), "%d", tab + 1);
            const float width = std::max(tabHeight + 4.0f, ImGui::CalcTextSize(label).x + 12.0f);
            const ImVec2 min(x, y), max(x + width, y + tabHeight);
            x += width + 2.0f;
            if(max.x > canvas.max.x) break;
            const bool hovered = canvas.hovered && !popupOpen && mouse.x >= min.x && mouse.x < max.x &&
                                 mouse.y >= min.y && mouse.y < max.y && hoverRect(min, max);
            const bool current = !addTab && tab == active;
            dl->AddRectFilled(min, max, current ? trackInk : hovered ? IM_COL32(78, 78, 84, 255) : IM_COL32(44, 44, 48, 255), 3.0f);
            if(!current) dl->AddRect(min, max, IM_COL32(95, 95, 102, 255), 3.0f);
            const float luminance = 0.299f * track.color.r + 0.587f * track.color.g + 0.114f * track.color.b;
            dl->AddText(ImVec2(min.x + (width - ImGui::CalcTextSize(label).x) * 0.5f, min.y + 2.0f),
                        current ? (luminance > 140.0f ? IM_COL32(15, 15, 15, 255) : IM_COL32(245, 245, 245, 255))
                                : IM_COL32(205, 205, 210, 255), label);
            if(!hovered) continue;
            if(addTab) {
                ImGui::SetTooltip("Add a curve");
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ofxOceanodeTimelineSetMultiCurveCount(lane, count + 1, clip.contentDurationBeats);
                    active = count;
                }
                continue;
            }
            hoveredTab = tab;
            if(tab != active) ImGui::SetTooltip("Curve %d: element %d of the vector.\nRight-click: duplicate, delete.", tab + 1, tab + 1);
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) active = tab;
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                menuLaneId = lane.id;
                menuCurve = tab;
                ImGui::OpenPopup("Multi curve tab");
            }
        }
    }
    if(menuLaneId == lane.id && ImGui::BeginPopup("Multi curve tab")) {
        const int size = static_cast<int>(lane.multiCurves.size());
        if(menuCurve >= 0 && menuCurve < size) {
            ImGui::Text("Curve %d", menuCurve + 1);
            ImGui::Separator();
            if(ImGui::MenuItem("Duplicate", nullptr, false, size < kOfxOceanodeTimelineMaxMultiCurves)) {
                const auto copy = lane.multiCurves[menuCurve]; // not a reference into the vector it grows
                lane.multiCurves.insert(lane.multiCurves.begin() + menuCurve + 1, copy);
                active = menuCurve + 1;
                menuCurve = -1;
            } else if(ImGui::MenuItem("Delete", nullptr, false, size > 1)) {
                lane.multiCurves.erase(lane.multiCurves.begin() + menuCurve);
                if(active > menuCurve || active >= size - 1) active = std::max(0, active - 1);
                menuCurve = -1;
            }
            if(menuCurve < 0) {
                ofxOceanodeTimelineSyncMultiCurves(lane); // the curves after it take new ids
                curve.clearSelection();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    active = std::max(0, std::min(static_cast<int>(lane.multiCurves.size()) - 1, active));
    activeCurves[lane.id] = active;

    // The other curves, behind the one being edited: dim, brighter while
    // the mouse is on their tab.
    const float top = inner.min.y + 10.0f, bottom = inner.max.y - 10.0f;
    if(canvas.right > canvas.left && bottom > top) {
        const float span = lane.valueMax - lane.valueMin;
        const float rangeMin = std::min(lane.valueMin, lane.valueMax), rangeMax = std::max(lane.valueMin, lane.valueMax);
        const auto yOf = [&](float normalized) {
            if(std::abs(span) < 1e-9f) return bottom;
            const float visible = ofClamp((lane.valueMin + normalized * span - rangeMin) / std::max(1e-9f, rangeMax - rangeMin),
                                          0.0f, 1.0f);
            return bottom - visible * (bottom - top);
        };
        const double clipEnd = clip.startBeat + clip.durationBeats;
        dl->PushClipRect(ImVec2(canvas.left, inner.min.y), ImVec2(canvas.right, inner.max.y), true);
        for(int index = 0; index < static_cast<int>(lane.multiCurves.size()); ++index) {
            const auto& other = lane.multiCurves[index];
            if(index == active || other.curvePoints.empty()) continue;
            const ImU32 ink = index == hoveredTab ? IM_COL32(245, 245, 245, 230) : IM_COL32(200, 200, 208, 80);
            ImVec2 previous;
            bool hasPrevious = false;
            for(float x = canvas.left; x <= canvas.right + 2.0f; x += 3.0f) {
                // just inside the clip: its end beat belongs to what follows
                const double beat = std::min(view.pixelsToBeat(std::min(x, canvas.right) - canvas.timelineOrigin.x),
                                             clipEnd - 1e-6);
                const float value = valueAtBeat(other.curvePoints, other.curveTensions, lane.curveInterpolation,
                                                timelineToSourceBeat(clip, std::max(clip.startBeat, beat)), 0.0f);
                const ImVec2 point(std::min(x, canvas.right), yOf(value));
                if(hasPrevious) dl->AddLine(previous, point, ink, index == hoveredTab ? 2.0f : 1.5f);
                previous = point;
                hasPrevious = true;
            }
        }
        dl->PopClipRect();
    }

    ofxOceanodeTimelineLaneEditorContext sub{context.timeline, context.container, context.sequence, context.track,
                                             context.clip, lane.multiCurves[static_cast<size_t>(active)], context.view,
                                             context.focusedLaneId, context.focused};
    curve.drawCanvas(sub, inner);
}

void ofxOceanodeTimelineMultiCurveEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    if(preview.lane == nullptr) return;
    for(const auto& each : preview.lane->multiCurves) {
        auto single = preview;
        single.lane = &each;
        curve.drawPreview(single);
    }
}
