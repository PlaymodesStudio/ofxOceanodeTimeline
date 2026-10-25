#include "ofxOceanodeTimelineBpmEditor.h"
#include "ofxOceanodeTimelineEditorView.h"
#include "ofxOceanodeTimelineEditorUi.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include "ofxOceanodeContainer.h"
#include <algorithm>
#include <cmath>

using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;

void ofxOceanodeTimelineBpmEditor::draw(
    ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
    const ofxOceanodeTimelineEditorView& view, float contentWidth, double beatPosition) {
    const double endBeat = view.endBeat;
    const float fallbackBpm = view.fallbackBpm;
    const float timelineScrollX = timeline.getViewState().scrollX;
    const bool collapsed = timeline.isBpmLaneCollapsed();
    const float height = collapsed ? 29.0f : 132.0f;
    ImGui::Dummy(ImVec2(contentWidth, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float zoneLeft = min.x + labelWidth;
    const ImVec2 timelineMin(zoneLeft - timelineScrollX, min.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(27, 27, 30, 255));
    dl->AddRectFilled(min, ImVec2(zoneLeft, max.y), IM_COL32(85, 90, 150, 75));
    dl->AddLine(ImVec2(zoneLeft - 1.0f, min.y), ImVec2(zoneLeft - 1.0f, max.y), IM_COL32(125, 135, 225, 190));

    // Keep every BPM control inside the fixed properties column. The combo
    // used to continue the top row after the Auto/manual controls, which put
    // it over the automation canvas on narrower windows.
    ImGui::PushClipRect(min, ImVec2(zoneLeft, max.y), true);
    ImGui::SetCursorScreenPos(ImVec2(min.x + 5.0f, min.y + 4.0f));
    ImGui::SetNextItemAllowOverlap();
    if(ImGui::SmallButton(collapsed ? ">##bpmCollapse" : "v##bpmCollapse"))
        timeline.setBpmLaneCollapsed(!collapsed);
    ImGui::SameLine();
    ImGui::TextUnformatted("Global tempo (BPM)");
    ImGui::SameLine();
    bool enabled = timeline.isBpmAutomationEnabled();
    if(ImGui::Checkbox("Auto##bpmAuto", &enabled)) timeline.setBpmAutomationEnabled(enabled);
    if(!enabled) {
        // With automation off, this lane still shows Oceanode's actual BPM
        // (the flat line below), but there was no way to type an exact
        // value here - only via the separate transport toolbar or the Time
        // controller. Both write through container.setBpm(), and this
        // reads transportState.bpm fresh every frame, so all three stay in
        // sync regardless of which one last changed it.
        ImGui::SameLine();
        float manualBpm = fallbackBpm;
        ImGui::SetNextItemWidth(70.0f);
        if(ImGui::DragFloat("##bpmManual", &manualBpm, 0.1f, 1.0f, 999.0f, "%.1f"))
            container.setBpm(manualBpm);
    }
    // Same 4 shapes as a curve lane (Step/Linear/Log-Exp/Sigmoid), reusing
    // the shared curve math so the BPM curve is evaluated, drawn and timed
    // the exact same way a regular automation curve is.
    const auto bpmInterpolation = curveInterpolationMode(timeline.getBpmInterpolation());
    if(!collapsed) {
        int interpolationMode = static_cast<int>(bpmInterpolation);
        ImGui::SetCursorScreenPos(ImVec2(min.x + 5.0f, min.y + 35.0f));
        ImGui::TextUnformatted("Mode");
        ImGui::SetCursorScreenPos(ImVec2(min.x + 55.0f, min.y + 31.0f));
        ImGui::SetNextItemWidth(std::max(1.0f, zoneLeft - min.x - 61.0f));
        if(ImGui::Combo("##bpmInterpolation", &interpolationMode, curveInterpolationNames, 4)) {
            timeline.setBpmInterpolation(curveInterpolationNames[interpolationMode]);
            bpmTensionSegment = -1;
        }
        float minimum = timeline.getBpmMinimum();
        float maximum = timeline.getBpmMaximum();
        const float rangeStartX = min.x + 55.0f;
        const float rangeAvailable = std::max(2.0f, zoneLeft - rangeStartX - 6.0f);
        const float rangeFieldWidth = std::max(1.0f, (rangeAvailable - 4.0f) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(min.x + 5.0f, min.y + 64.0f));
        ImGui::TextUnformatted("Range");
        ImGui::SetCursorScreenPos(ImVec2(rangeStartX, min.y + 60.0f));
        ImGui::SetNextItemWidth(rangeFieldWidth);
        if(ImGui::DragFloat("##bpmMin", &minimum, 0.5f, 1.0f, 998.0f, "%.0f")) timeline.setBpmRange(minimum, maximum);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum BPM");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::SetNextItemWidth(rangeFieldWidth);
        if(ImGui::DragFloat("##bpmMax", &maximum, 0.5f, minimum + 1.0f, 999.0f, "%.0f")) timeline.setBpmRange(minimum, maximum);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum BPM");
        ImGui::SetCursorScreenPos(ImVec2(min.x + 55.0f, min.y + 89.0f));
        if(bpmInterpolation == CurveInterpolationMode::LogExp) ImGui::TextDisabled("Alt-drag vertically");
        else if(bpmInterpolation == CurveInterpolationMode::Sigmoid) ImGui::TextDisabled("Alt-drag freely");
    }
    ImGui::PopClipRect();

    const float bpmCanvasStartX = std::max(zoneLeft, timelineMin.x);
    ImGui::SetCursorScreenPos(ImVec2(bpmCanvasStartX, timelineMin.y));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bpmAutomationCanvas",
                          ImVec2(std::max(1.0f, contentWidth - labelWidth - (bpmCanvasStartX - timelineMin.x)), height));
    const bool canvasHovered = ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x >= zoneLeft;
    const float graphTop = min.y + (collapsed ? 4.0f : 8.0f);
    const float graphBottom = max.y - (collapsed ? 4.0f : 8.0f);
    dl->PushClipRect(ImVec2(zoneLeft, graphTop), ImVec2(max.x, graphBottom), true);
    if(timeline.isLoopEnabled()) {
        const float loopX1 = timelineMin.x + view.beatToPixels(timeline.getLoopStartBeat());
        const float loopX2 = timelineMin.x + view.beatToPixels(timeline.getLoopEndBeat());
        dl->AddRectFilled(ImVec2(std::max(timelineMin.x, loopX1), graphTop), ImVec2(std::min(max.x, loopX2), graphBottom), IM_COL32(95, 105, 220, 25));
    }
    const double grid = view.displayGridBeats();
    const int gridLines = static_cast<int>(std::ceil(endBeat / grid));
    for(int i = 0; i <= gridLines; ++i) {
        const double beat = i * grid;
        const float x = timelineMin.x + view.beatToPixels(beat);
        const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
        if(bar || view.beatToPixels(beat + grid) - view.beatToPixels(beat) >= 4.0f)
            dl->AddLine(ImVec2(x, graphTop), ImVec2(x, graphBottom), bar ? barColor : gridColor, bar ? 1.5f : 1.0f);
    }

    auto& sourcePoints = timeline.getBpmAutomationPoints();
    auto& sourceTensions = timeline.getBpmCurveTensions();
    if(bpmDragPointIndex < 0 && bpmTensionSegment < 0)
        std::sort(sourcePoints.begin(), sourcePoints.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    sourceTensions.resize(sourcePoints.empty() ? 0 : sourcePoints.size() - 1);
    std::vector<ofxOceanodeTimelineCurvePoint> points = sourcePoints;
    if(points.empty()) points.push_back({0.0, fallbackBpm});
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    const float bpmSpan = std::max(1.0f, timeline.getBpmMaximum() - timeline.getBpmMinimum());
    auto bpmToY = [&](float value) {
        const float normalized = ofClamp((value - timeline.getBpmMinimum()) / bpmSpan, 0.0f, 1.0f);
        return graphBottom - normalized * (graphBottom - graphTop);
    };
    if(points.size() == 1) {
        dl->AddLine(ImVec2(timelineMin.x, bpmToY(points.front().value)),
                    ImVec2(max.x, bpmToY(points.front().value)), IM_COL32(155, 170, 255, 220), 2.0f);
    } else if(bpmInterpolation == CurveInterpolationMode::Step) {
        for(size_t i = 1; i < points.size(); ++i) {
            const ImVec2 a(timelineMin.x + view.beatToPixels(points[i - 1].beat), bpmToY(points[i - 1].value));
            const ImVec2 b(timelineMin.x + view.beatToPixels(points[i].beat), bpmToY(points[i].value));
            dl->AddLine(a, ImVec2(b.x, a.y), IM_COL32(155, 170, 255, 230), 2.0f);
            dl->AddLine(ImVec2(b.x, a.y), b, IM_COL32(155, 170, 255, 230), 2.0f);
        }
    } else {
        for(size_t i = 1; i < points.size(); ++i) {
            const auto tension = i - 1 < sourceTensions.size() ? sourceTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
            const float x1 = timelineMin.x + view.beatToPixels(points[i - 1].beat);
            const float x2 = timelineMin.x + view.beatToPixels(points[i].beat);
            const int samples = std::max(16, static_cast<int>(std::abs(x2 - x1) / 4.0f));
            for(int sample = 0; sample < samples; ++sample) {
                const float t1 = sample / static_cast<float>(samples);
                const float t2 = (sample + 1) / static_cast<float>(samples);
                const double beat1 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t1;
                const double beat2 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t2;
                const float value1 = ofLerp(points[i - 1].value, points[i].value, curveSegmentShape(t1, bpmInterpolation, tension));
                const float value2 = ofLerp(points[i - 1].value, points[i].value, curveSegmentShape(t2, bpmInterpolation, tension));
                const ImVec2 a(timelineMin.x + view.beatToPixels(beat1), bpmToY(value1));
                const ImVec2 b(timelineMin.x + view.beatToPixels(beat2), bpmToY(value2));
                dl->AddLine(a, b, IM_COL32(155, 170, 255, 230), 2.0f);
            }
        }
    }
    if(!collapsed) for(const auto& point : points) {
        const ImVec2 p(timelineMin.x + view.beatToPixels(point.beat), bpmToY(point.value));
        dl->AddCircleFilled(p, 4.0f, IM_COL32(225, 230, 255, 245));
    }
    dl->PopClipRect();

    if(!collapsed && canvasHovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double beat = view.snapBeat(view.pixelsToBeat(mouse.x - timelineMin.x));
        const float value = ofClamp(timeline.getBpmMinimum() +
            (graphBottom - mouse.y) / std::max(1.0f, graphBottom - graphTop) * bpmSpan,
            timeline.getBpmMinimum(), timeline.getBpmMaximum());
        const double tolerance = std::max(1.0 / ofxOceanodeTimelineEditorView::ppq,
            std::abs(view.pixelsToBeat(mouse.x - timelineMin.x + 5.0f) - beat));
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            bpmValuePointIndex = -1;
            if(sourcePoints.empty() && std::abs(mouse.x - timelineMin.x) <= 8.0f &&
               std::abs(bpmToY(fallbackBpm) - mouse.y) <= 9.0f) {
                sourcePoints.push_back({0.0, ofClamp(fallbackBpm, timeline.getBpmMinimum(), timeline.getBpmMaximum())});
                bpmValuePointIndex = 0;
            }
            for(int i = static_cast<int>(sourcePoints.size()) - 1; i >= 0; --i) {
                const float pointX = timelineMin.x + view.beatToPixels(sourcePoints[i].beat);
                if(std::abs(pointX - mouse.x) <= 8.0f && std::abs(bpmToY(sourcePoints[i].value) - mouse.y) <= 9.0f) {
                    bpmValuePointIndex = i;
                    bpmNumericValue = sourcePoints[i].value;
                    break;
                }
            }
            if(bpmValuePointIndex >= 0) ImGui::OpenPopup("BPM point value");
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bpmDragPointIndex = -1;
            bpmTensionSegment = -1;
            for(int i = static_cast<int>(sourcePoints.size()) - 1; i >= 0; --i) {
                if(std::abs(sourcePoints[i].beat - beat) <= tolerance && std::abs(bpmToY(sourcePoints[i].value) - mouse.y) <= 9.0f) {
                    bpmDragPointIndex = i;
                    break;
                }
            }
            if(bpmDragPointIndex >= 0 && ImGui::GetIO().KeyShift) {
                // Matches the regular curve lane editor's shift-click-to-delete
                // gesture. Tensions are keyed by segment index, so removing a
                // point shifts every tension after it -- reset them the same
                // way the curve editor does rather than leave stale shaping on
                // the wrong (now different) pair of neighbours.
                sourcePoints.erase(sourcePoints.begin() + bpmDragPointIndex);
                sourceTensions.assign(sourcePoints.size() > 0 ? sourcePoints.size() - 1 : 0,
                                      ofxOceanodeTimelineCurveTension{});
                bpmDragPointIndex = -1;
                bpmValuePointIndex = -1;
            } else {
                if(bpmDragPointIndex < 0 && ImGui::GetIO().KeyAlt &&
                   (bpmInterpolation == CurveInterpolationMode::LogExp || bpmInterpolation == CurveInterpolationMode::Sigmoid) &&
                   sourcePoints.size() > 1) {
                    for(size_t i = 1; i < sourcePoints.size(); ++i) {
                        const auto& a = sourcePoints[i - 1];
                        const auto& b = sourcePoints[i];
                        if(beat < a.beat || beat > b.beat) continue;
                        const float t = static_cast<float>((beat - a.beat) / std::max(1e-9, b.beat - a.beat));
                        const auto tension = i - 1 < sourceTensions.size() ? sourceTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
                        const float segmentValue = ofLerp(a.value, b.value, curveSegmentShape(t, bpmInterpolation, tension));
                        const float segmentY = bpmToY(segmentValue);
                        if(std::abs(mouse.y - segmentY) <= 12.0f) {
                            bpmTensionSegment = static_cast<int>(i - 1);
                            bpmTensionDragStartX = mouse.x;
                            bpmTensionDragStartY = mouse.y;
                            bpmTensionStartInflection = tension.inflection;
                            bpmTensionStartSteepness = tension.steepness;
                        }
                        break;
                    }
                }
                if(bpmDragPointIndex < 0 && bpmTensionSegment < 0 && !ImGui::GetIO().KeyAlt) {
                    sourcePoints.push_back({beat, value});
                    std::sort(sourcePoints.begin(), sourcePoints.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
                    sourceTensions.resize(sourcePoints.empty() ? 0 : sourcePoints.size() - 1);
                    bpmDragPointIndex = static_cast<int>(std::min_element(sourcePoints.begin(), sourcePoints.end(), [&](const auto& a, const auto& b) {
                        return std::abs(a.beat - beat) < std::abs(b.beat - beat);
                    }) - sourcePoints.begin());
                }
            }
        }
        // Once a point or a tension handle has been grabbed, keep updating
        // it for as long as the mouse button is held, even if the cursor
        // strays outside the lane's rect, matching the curve lane editor.
        if((bpmDragPointIndex >= 0 || bpmTensionSegment >= 0) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if(bpmDragPointIndex >= 0 && bpmDragPointIndex < static_cast<int>(sourcePoints.size())) {
                const double minimumBeat = bpmDragPointIndex > 0
                    ? sourcePoints[bpmDragPointIndex - 1].beat + 1.0 / ofxOceanodeTimelineEditorView::ppq : 0.0;
                const double maximumBeat = bpmDragPointIndex + 1 < static_cast<int>(sourcePoints.size())
                    ? sourcePoints[bpmDragPointIndex + 1].beat - 1.0 / ofxOceanodeTimelineEditorView::ppq : endBeat;
                sourcePoints[bpmDragPointIndex].beat = std::max(minimumBeat,
                    std::min(std::max(minimumBeat, maximumBeat), beat));
                sourcePoints[bpmDragPointIndex].value = value;
            } else if(bpmTensionSegment >= 0 && bpmTensionSegment < static_cast<int>(sourceTensions.size())) {
                auto& tension = sourceTensions[bpmTensionSegment];
                if(bpmInterpolation == CurveInterpolationMode::Sigmoid) {
                    tension.inflection = ofClamp(bpmTensionStartInflection +
                        (mouse.x - bpmTensionDragStartX) / std::max(1.0f, max.x - timelineMin.x), 0.01f, 0.99f);
                } else {
                    tension.inflection = 0.5f;
                }
                const float steepnessDelta = -(mouse.y - bpmTensionDragStartY) /
                    std::max(1.0f, (graphBottom - graphTop) / 3.0f);
                tension.steepness = ofClamp(bpmTensionStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
            }
        }
    }
    if(ImGui::BeginPopup("BPM point value")) {
        if(bpmValuePointIndex >= 0 && bpmValuePointIndex < static_cast<int>(sourcePoints.size())) {
            ImGui::TextUnformatted("BPM point");
            contextMenuSection("Value");
            ImGui::SetNextItemWidth(110.0f);
            if(ImGui::InputFloat("BPM", &bpmNumericValue, 0.1f, 1.0f, "%.3f")) {
                sourcePoints[bpmValuePointIndex].value = ofClamp(bpmNumericValue,
                    timeline.getBpmMinimum(), timeline.getBpmMaximum());
            }
            contextMenuSection("Remove");
            if(ImGui::MenuItem("Delete point")) {
                sourcePoints.erase(sourcePoints.begin() + bpmValuePointIndex);
                sourceTensions.resize(sourcePoints.empty() ? 0 : sourcePoints.size() - 1);
                bpmValuePointIndex = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if(bpmDragPointIndex >= 0)
            std::sort(sourcePoints.begin(), sourcePoints.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
        bpmDragPointIndex = -1;
        bpmTensionSegment = -1;
    }
    if(collapsed && canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        timeline.setBpmLaneCollapsed(false);

    const float playheadX = timelineMin.x + view.beatToPixels(beatPosition);
    if(playheadX >= zoneLeft && playheadX <= max.x)
        dl->AddLine(ImVec2(playheadX, min.y), ImVec2(playheadX, max.y), playheadColor, 2.0f);
    finishAbsoluteLayout(ImVec2(min.x, max.y));
}

