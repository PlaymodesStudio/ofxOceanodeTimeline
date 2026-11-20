#include "ofxOceanodeTimelineCurveEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeColors.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

void ofxOceanodeTimelineCurveEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    const auto& track = context.track;
    auto* lane = &context.lane;

    int interpolationMode = static_cast<int>(curveInterpolationMode(lane->curveInterpolation));
    propertyLabel("Mode");
    if(ImGui::Combo("##curveInterpolation", &interpolationMode, kCurveInterpolationNames, 4)) {
        lane->curveInterpolation = kCurveInterpolationNames[interpolationMode];
        resetCurveTensions(*lane);
        curveDragPointIndex = -1;
        curveTensionSegment = -1;
    }
    const auto selectedInterpolation = curveInterpolationMode(lane->curveInterpolation);
    if(selectedInterpolation == CurveInterpolationMode::LogExp) ImGui::TextDisabled("Drag a handle up / down");
    else if(selectedInterpolation == CurveInterpolationMode::Sigmoid) ImGui::TextDisabled("Drag a segment's handle");
    if((selectedInterpolation == CurveInterpolationMode::LogExp || selectedInterpolation == CurveInterpolationMode::Sigmoid) &&
       ImGui::IsItemHovered())
        ImGui::SetTooltip("Each segment has a handle on its curve: drag it to bend the segment,\n"
                          "double-click it for a straight line. Alt-drag anywhere on a segment does the same.");
    ImGui::Checkbox("Clamp to range", &lane->curveClamp);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Clamp curve values to the configured Range min..max");
    {
        // Scale: Auto follows the parameter (a gain: dB)
        const auto* scaleBinding = lane->bindingIds.empty() ? nullptr : timeline.getBinding(sequence, track.id, lane->bindingIds.front());
        const bool autoDb = scaleBinding != nullptr && scaleBinding->valueScale == "dB";
        int scale = lane->curveScale == "Linear" ? 1 : lane->curveScale == "dB" ? 2 : 0;
        const std::string autoLabel = std::string("Auto (") + (autoDb ? "dB" : "linear") + ")";
        const char* scales[] = {autoLabel.c_str(), "Linear", "dB"};
        propertyLabel("Scale");
        if(ImGui::Combo("##curveScale", &scale, scales, 3)) {
            const bool wasDb = ofxOceanodeTimelineScale::isDecibels(*lane, scaleBinding);
            lane->curveScale = scale == 1 ? "Linear" : scale == 2 ? "dB" : "";
            const bool isDb = ofxOceanodeTimelineScale::isDecibels(*lane, scaleBinding);
            // keep the values the points stand for
            if(wasDb != isDb && std::abs(lane->valueMax - lane->valueMin) > 1e-9f) {
                for(auto& point : lane->curvePoints) {
                    const float actual = wasDb ? ofxOceanodeTimelineScale::normalizedToGain(*lane, point.value)
                                               : lane->valueMin + point.value * (lane->valueMax - lane->valueMin);
                    point.value = isDb ? ofxOceanodeTimelineScale::gainToNormalized(*lane, actual)
                                       : ofClamp((actual - lane->valueMin) / (lane->valueMax - lane->valueMin), 0.0f, 1.0f);
                }
            }
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("dB: for gains (a level, a send). The curve's height is the level in decibels,\n"
                              "from silence at the bottom up to the range's maximum; 0 dB near the top.");
    }
    if(ImGui::Checkbox("Snap##curveValueSnap", &lane->valueSnap) && lane->valueSnap &&
       lane->valueQuantizeSteps < 2) {
        lane->valueQuantizeSteps = 8; // sensible first grid
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap point values to evenly spaced levels across the range");
    ImGui::SameLine();
    int curveSteps = std::max(2, lane->valueQuantizeSteps);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if(!lane->valueSnap) ImGui::BeginDisabled();
    if(ImGui::DragInt("##curveValueSteps", &curveSteps, 0.1f, 2, 128, "%d steps"))
        lane->valueQuantizeSteps = std::max(2, std::min(128, curveSteps));
    if(!lane->valueSnap) ImGui::EndDisabled();
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Number of intervals: %d steps = %d levels (min and max included)",
                          std::max(2, lane->valueQuantizeSteps), std::max(2, lane->valueQuantizeSteps) + 1);

}

void ofxOceanodeTimelineCurveEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    const bool isFocused = context.focused;
    const double endBeat = view.endBeat;
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float left = canvas.left, right = canvas.right;
    const bool editorCanvasHovered = canvas.hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const auto interpolation = curveInterpolationMode(lane->curveInterpolation);
    const float curveTop = editorMin.y + 10.0f, curveBottom = editorMax.y - 10.0f;
    const float curveValueSpan = lane->valueMax - lane->valueMin;
    const float curveRangeMin = std::min(lane->valueMin, lane->valueMax);
    const float curveRangeMax = std::max(lane->valueMin, lane->valueMax);
    // a gain on a decibel axis: the point values are positions on it
    const auto* curveBinding = lane->bindingIds.empty() ? nullptr : timeline.getBinding(sequence, track.id, lane->bindingIds.front());
    const bool curveDb = ofxOceanodeTimelineScale::isDecibels(*lane, curveBinding);
    const auto curveValueToY = [&](float normalizedValue) {
        if(curveDb) return curveBottom - ofClamp(normalizedValue, 0.0f, 1.0f) * (curveBottom - curveTop);
        if(std::abs(curveValueSpan) < 1e-9f) return curveBottom;
        const float actualValue = lane->valueMin + normalizedValue * curveValueSpan;
        const float visibleValue = ofClamp((actualValue - curveRangeMin) /
                                           std::max(1e-9f, curveRangeMax - curveRangeMin),
                                           0.0f, 1.0f);
        return curveBottom - visibleValue * (curveBottom - curveTop);
    };
    // The editor's grid: the theme's OC_CurveEditorLines (a bar's line is thicker).
    const ImU32 editorLines = ImGui::ColorConvertFloat4ToU32(OceanodeColors::CurveEditorLines);
    if(curveSnapLevels(*lane) >= 2) {
        const int levels = curveSnapLevels(*lane);
        for(int row = 0; row <= levels; ++row) {
            const float y = curveTop + (curveBottom - curveTop) * row / static_cast<float>(levels);
            dl->AddLine(ImVec2(timelineMin.x, y), ImVec2(editorMax.x, y), IM_COL32(150, 140, 80, 150));
        }
    } else if(curveDb) {
        // dB lines (0 dB stronger), labelled on the left
        for(float db : {0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f}) {
            if(db >= ofxOceanodeTimelineScale::topDb(*lane) - 0.5f) continue;
            const float y = curveValueToY(ofxOceanodeTimelineScale::dbToNormalized(*lane, db));
            dl->AddLine(ImVec2(timelineMin.x, y), ImVec2(editorMax.x, y),
                        db == 0.0f ? IM_COL32(150, 150, 150, 170) : editorLines);
            char dbLabel[16];
            std::snprintf(dbLabel, sizeof(dbLabel), "%g dB", db);
            if(y - ImGui::GetTextLineHeight() > curveTop + ImGui::GetTextLineHeight() &&
               y < curveBottom - ImGui::GetTextLineHeight())
                dl->AddText(ImVec2(left + 5.0f, y - ImGui::GetTextLineHeight()), IM_COL32(170, 170, 178, 200), dbLabel);
        }
    } else {
        for(int row = 0; row <= 4; ++row) {
            const float y = curveTop + (curveBottom - curveTop) * row / 4.0f;
            dl->AddLine(ImVec2(timelineMin.x, y), ImVec2(editorMax.x, y), editorLines);
        }
    }
    char curveMaxLabel[32];
    char curveMinLabel[32];
    if(curveDb) {
        std::snprintf(curveMaxLabel, sizeof(curveMaxLabel), "%+.1f dB", ofxOceanodeTimelineScale::topDb(*lane));
        std::snprintf(curveMinLabel, sizeof(curveMinLabel), "-inf dB");
    } else {
        std::snprintf(curveMaxLabel, sizeof(curveMaxLabel), "%.4g", lane->valueMax);
        std::snprintf(curveMinLabel, sizeof(curveMinLabel), "%.4g", lane->valueMin);
    }
    dl->AddText(ImVec2(left + 5.0f, curveTop + 2.0f), IM_COL32(205, 205, 210, 220), curveMaxLabel);
    dl->AddText(ImVec2(left + 5.0f, curveBottom - ImGui::GetTextLineHeight() - 2.0f),
                IM_COL32(205, 205, 210, 220), curveMinLabel);
    const double curveGrid = view.displayGridBeats();
    const double visibleFrom = beatAtOffset(std::max(0.0f, left - timelineMin.x));
    const double visibleTo = std::min(endBeat, beatAtOffset(std::max(0.0f, right - timelineMin.x)) + curveGrid);
    const int curveGridLines = static_cast<int>(std::ceil(visibleTo / curveGrid));
    for(int gridIndex = std::max(0, static_cast<int>(std::floor(visibleFrom / curveGrid))); gridIndex <= curveGridLines; ++gridIndex) {
        const double beat = gridIndex * curveGrid;
        const float x = timelineMin.x + beatOffset(beat);
        const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
        if(bar || beatOffset(beat + curveGrid) - beatOffset(beat) >= 4.0f)
            dl->AddLine(ImVec2(x, curveTop), ImVec2(x, curveBottom), editorLines, bar ? 1.5f : 1.0f);
    }
    // Points stay in beat order; an edit that moves one past a neighbour is
    // reordered here. Stable, so two points on one beat (a vertical step)
    // never swap -- an unstable sort could reorder them every frame.
    const auto byBeat = [](const auto& a, const auto& b) { return a.beat < b.beat; };
    if(!std::is_sorted(lane->curvePoints.begin(), lane->curvePoints.end(), byBeat)) {
        std::stable_sort(lane->curvePoints.begin(), lane->curvePoints.end(), byBeat);
        if(curveSelectionLaneId == lane->id) curveSelectedPointIndices.clear(); // the indices no longer name them
    }
    // The selection belongs to one lane: focusing another starts afresh.
    // Indices an undo or another edit took away are dropped.
    if(isFocused && curveSelectionLaneId != lane->id) {
        curveSelectionLaneId = lane->id;
        curveSelectedPointIndices.clear();
    }
    if(curveSelectionLaneId == lane->id)
        for(auto it = curveSelectedPointIndices.begin(); it != curveSelectedPointIndices.end();) {
            if(*it < 0 || *it >= static_cast<int>(lane->curvePoints.size())) it = curveSelectedPointIndices.erase(it);
            else ++it;
        }
    const auto pointSelected = [&](size_t i) {
        return curveSelectionLaneId == lane->id && curveSelectedPointIndices.count(static_cast<int>(i)) > 0;
    };
    const size_t segments = lane->curvePoints.empty() ? 0 : lane->curvePoints.size() - 1;
    if(lane->curveTensions.size() != segments) lane->curveTensions.resize(segments);
    const auto& points = lane->curvePoints;
    const double content = sourceDuration(*clip);
    const double repeatCycleDuration = cycleDuration(*clip);
    const int cycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / repeatCycleDuration))) : 1;
    // Only the repeats reaching the visible part of the canvas.
    int firstCycle = 0, lastCycle = cycles - 1;
    if(clip->repeatContent && cycles > 1) {
        const double cycleLength = std::max(1e-6, repeatCycleDuration);
        firstCycle = ofClamp(static_cast<int>(std::floor((visibleFrom - clip->startBeat) / cycleLength)), 0, cycles - 1);
        lastCycle = ofClamp(static_cast<int>(std::floor((visibleTo - clip->startBeat) / cycleLength)), firstCycle, cycles - 1);
    }
    // The point the mouse is on (within its 8px pick area), or the one being
    // dragged, is drawn lit and 3px larger.
    int litPoint = -1, litCycle = 0;
    if(isFocused && curveDragPointIndex >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        litPoint = curveDragPointIndex;
        litCycle = curveDragCycle;
    } else if(isFocused && editorCanvasHovered && !ImGui::IsPopupOpen("Curve point value") && right > left &&
       hoverRect(ImVec2(left, editorMin.y), ImVec2(right, editorMax.y))) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double mouseBeat = beatAtOffset(mouse.x - timelineMin.x);
        litCycle = clip->repeatContent
            ? ofClamp(static_cast<int>(cycleIndex(*clip, mouseBeat)), 0, cycles - 1) : 0;
        for(int i = static_cast<int>(points.size()) - 1; i >= 0; --i) {
            const float pointX = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, points[i].beat, litCycle));
            if(std::abs(mouse.x - pointX) <= 8.0f && std::abs(mouse.y - curveValueToY(points[i].value)) <= 8.0f) {
                litPoint = i;
                break;
            }
        }
    }
    // A Sigmoid or Log/Exp segment has a handle on its curve, as a Global
    // Track's transition has: where the sigmoid is half way (it follows the
    // bend sideways), or the middle of a Log/Exp segment (it follows the bend
    // up and down). Segments too short to hold one beside their points have none.
    const bool tensionMode = interpolation == CurveInterpolationMode::LogExp || interpolation == CurveInterpolationMode::Sigmoid;
    constexpr float kTensionHandleMinWidth = 24.0f, kTensionHandlePick = 7.0f;
    struct TensionHandle { ImVec2 at; float width; };
    const auto tensionHandle = [&](size_t segment, int cycle) {
        const auto& a = lane->curvePoints[segment];
        const auto& b = lane->curvePoints[segment + 1];
        const auto tension = lane->curveTensions[segment];
        const float t = interpolation == CurveInterpolationMode::Sigmoid ? ofClamp(tension.inflection, 0.01f, 0.99f) : 0.5f;
        const float xa = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, a.beat, cycle));
        const float xb = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, b.beat, cycle));
        return TensionHandle{ImVec2(xa + (xb - xa) * t,
                                    curveValueToY(ofLerp(a.value, b.value, curveSegmentShape(t, interpolation, tension)))),
                             xb - xa};
    };
    const auto tensionNeutral = [&](const ofxOceanodeTimelineCurveTension& tension) {
        return std::abs(tension.steepness - 1.0f) < 1e-4f &&
               (interpolation != CurveInterpolationMode::Sigmoid || std::abs(tension.inflection - 0.5f) < 1e-4f);
    };
    // Keep the margin available for endpoint handles, including selection rings.
    dl->PushClipRect(ImVec2(left, editorMin.y), ImVec2(right, editorMax.y), true);
    for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        if(cycle > 0) {
            const float markerX = timelineMin.x + beatOffset(clip->startBeat + cycle * repeatCycleDuration);
            dl->AddLine(ImVec2(markerX, curveTop), ImVec2(markerX, curveBottom), IM_COL32(track.color.r, track.color.g, track.color.b, 220), 2.0f);
        }
        for(size_t i = 1; i < points.size(); ++i) {
            const auto tension = lane->curveTensions[i - 1];
            if(interpolation == CurveInterpolationMode::Step) {
                const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, points[i - 1].beat, cycle));
                const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, points[i].beat, cycle));
                const float y1 = curveValueToY(points[i - 1].value);
                const float y2 = curveValueToY(points[i].value);
                const ImU32 color = IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 240 : 130);
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y1), color, 2.0f);
                dl->AddLine(ImVec2(x2, y1), ImVec2(x2, y2), color, 2.0f);
                continue;
            }
            const int samples = std::max(16, static_cast<int>(std::abs(
                beatOffset(sourceToTimelineBeat(*clip, points[i].beat, cycle)) -
                beatOffset(sourceToTimelineBeat(*clip, points[i - 1].beat, cycle))) / 4.0f));
            for(int sample = 0; sample < samples; ++sample) {
                const float t1 = sample / static_cast<float>(samples);
                const float t2 = (sample + 1) / static_cast<float>(samples);
                const double source1 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t1;
                const double source2 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t2;
                const float value1 = ofLerp(points[i - 1].value, points[i].value,
                                            curveSegmentShape(t1, interpolation, tension));
                const float value2 = ofLerp(points[i - 1].value, points[i].value,
                                            curveSegmentShape(t2, interpolation, tension));
                const ImVec2 a(timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, source1, cycle)),
                               curveValueToY(value1));
                const ImVec2 b(timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, source2, cycle)),
                               curveValueToY(value2));
                dl->AddLine(a, b, IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 240 : 130), 2.0f);
            }
        }
        if(tensionMode) {
            const ImU32 ink = IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 255 : 130);
            const ImU32 dark = IM_COL32(20, 20, 20, cycle == 0 ? 230 : 130);
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            for(size_t i = 0; i + 1 < points.size(); ++i) {
                const auto handle = tensionHandle(i, cycle);
                if(handle.width < kTensionHandleMinWidth) continue;
                // Hollow while the segment is straight, filled once bent;
                // white while dragged, larger under the mouse.
                const bool neutral = tensionNeutral(lane->curveTensions[i]);
                const bool dragged = isFocused && curveTensionSegment == static_cast<int>(i);
                const bool hovered = isFocused && editorCanvasHovered && litPoint < 0 && curveDragPointIndex < 0 &&
                    curveTensionSegment < 0 && std::abs(mouse.x - handle.at.x) <= kTensionHandlePick &&
                    std::abs(mouse.y - handle.at.y) <= kTensionHandlePick;
                const float handleRadius = dragged || hovered ? 5.0f : 3.5f;
                dl->AddCircleFilled(handle.at, handleRadius, dragged ? IM_COL32(255, 255, 255, 255) : neutral ? dark : ink);
                dl->AddCircle(handle.at, handleRadius, neutral ? ink : dark, 0, 1.0f);
            }
        }
        for(size_t i = 0; i < points.size(); ++i) {
            const ImVec2 p(timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, points[i].beat, cycle)), curveValueToY(points[i].value));
            const float radius = cycle == 0 ? 4.0f : 3.0f;
            const bool lit = static_cast<int>(i) == litPoint && cycle == litCycle;
            if(pointSelected(i)) {
                // Selected: a larger amber dot in a white ring, on every repeat.
                dl->AddCircleFilled(p, radius + 2.0f, IM_COL32(255, 185, 50, cycle == 0 ? 255 : 150));
                dl->AddCircle(p, radius + 4.5f, lit ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive)
                                                    : IM_COL32(255, 255, 255, cycle == 0 ? 235 : 120), 0, 1.5f);
            } else if(lit)
                dl->AddCircleFilled(p, radius + 3.0f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
            else
                dl->AddCircleFilled(p, radius, IM_COL32(245, 245, 245, cycle == 0 ? 230 : 115));
        }
    }
    dl->PopClipRect();
    // Require the editor canvas item itself to be hoverable, not merely
    // geometric overlap. This prevents clicks in the numeric popup from
    // passing through and creating a point underneath it.
    if(isFocused) {
    const bool curveHovered = editorCanvasHovered && !ImGui::IsPopupOpen("Curve point value") && right > left &&
        hoverRect(ImVec2(left, editorMin.y), ImVec2(right, editorMax.y));
    const ImVec2 curveMouse = ImGui::GetIO().MousePos;
    const double curveTimelineBeat = beatAtOffset(curveMouse.x - timelineMin.x);
    const double curveSourceBeat = timelineToSourceBeat(*clip, curveTimelineBeat);
    const int hoveredCycle = clip->repeatContent
        ? ofClamp(static_cast<int>(cycleIndex(*clip, curveTimelineBeat)), 0, cycles - 1) : 0;
    int hitCycle = hoveredCycle;
    float curveValue = (curveBottom - curveMouse.y) / (curveBottom - curveTop);
    if(curveDb) curveValue = ofClamp(curveValue, 0.0f, 1.0f);
    else if(lane->curveClamp && std::abs(curveValueSpan) > 1e-9f) {
        const float actualValue = ofClamp(lane->valueMin + curveValue * curveValueSpan,
                                          curveRangeMin, curveRangeMax);
        curveValue = (actualValue - lane->valueMin) / curveValueSpan;
    }
    if(curveSnapLevels(*lane) >= 2) {
        const float levels = static_cast<float>(curveSnapLevels(*lane));
        curveValue = std::round(curveValue * levels) / levels;
    }
    auto hitPoint = [&]() {
        for(int i = static_cast<int>(lane->curvePoints.size()) - 1; i >= 0; --i) {
            const auto& point = lane->curvePoints[i];
            const float pointY = curveValueToY(point.value);
            // At a repeat seam the previous cycle's endpoint is still pickable.
            for(int cycle = hoveredCycle; cycle >= std::max(0, hoveredCycle - 1); --cycle) {
                const float pointX = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, point.beat, cycle));
                if(std::abs(curveMouse.x - pointX) <= 8.0f && std::abs(curveMouse.y - pointY) <= 8.0f) {
                    hitCycle = cycle;
                    return i;
                }
            }
        }
        return -1;
    };
    // The segment whose tension handle the mouse is on (points win over it).
    auto hitTensionHandle = [&]() {
        if(!tensionMode) return -1;
        for(size_t i = 0; i + 1 < lane->curvePoints.size() && i < lane->curveTensions.size(); ++i) {
            const auto handle = tensionHandle(i, hoveredCycle);
            if(handle.width >= kTensionHandleMinWidth && std::abs(curveMouse.x - handle.at.x) <= kTensionHandlePick &&
               std::abs(curveMouse.y - handle.at.y) <= kTensionHandlePick)
                return static_cast<int>(i);
        }
        return -1;
    };
    if(curveHovered && curveDragPointIndex < 0 && curveTensionSegment < 0 && hitPoint() < 0) {
        const int hoveredHandle = hitTensionHandle();
        if(hoveredHandle >= 0) {
            const auto& tension = lane->curveTensions[hoveredHandle];
            if(interpolation == CurveInterpolationMode::Sigmoid) {
                ImGui::SetTooltip("Bend at %d%%, steepness %.2f\n"
                                  "Drag sideways: where it bends. Up / down: how sharply.\nDouble-click: linear.",
                                  static_cast<int>(std::round(tension.inflection * 100.0f)), tension.steepness);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else {
                ImGui::SetTooltip("Steepness %.2f\nDrag up / down: how it bends.\nDouble-click: linear.", tension.steepness);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
        }
    }

    if(curveHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        curveValuePointIndex = hitPoint();
        if(curveValuePointIndex >= 0) {
            const float pointValue = lane->curvePoints[curveValuePointIndex].value;
            curveNumericValue = curveDb ? std::max(-120.0f, ofxOceanodeTimelineScale::normalizedToDb(*lane, pointValue))
                                        : lane->valueMin + pointValue * (lane->valueMax - lane->valueMin);
            ImGui::OpenPopup("Curve point value");
        }
    }
    // Click a point: select it alone (and drag it). Shift+click a point adds
    // it to the selection or takes it out; Delete removes the selection.
    if(curveHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        curveDragPointIndex = hitPoint();
        curveTensionSegment = -1;
        curveTensionFromHandle = false;
        const int clickedHandle = curveDragPointIndex < 0 ? hitTensionHandle() : -1;
        if(curveDragPointIndex >= 0 && ImGui::GetIO().KeyShift) {
            if(!curveSelectedPointIndices.erase(curveDragPointIndex)) curveSelectedPointIndices.insert(curveDragPointIndex);
            curveDragPointIndex = -1;
        } else if(curveDragPointIndex >= 0) {
            curveSelectedPointIndices = {curveDragPointIndex};
        } else if(clickedHandle >= 0) {
            // A segment's handle: drag it to bend the segment, double-click
            // it for a straight line (as a Global Track's easing handle).
            auto& tension = lane->curveTensions[clickedHandle];
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) tension = ofxOceanodeTimelineCurveTension{};
            else {
                curveTensionSegment = clickedHandle;
                curveTensionFromHandle = true;
                curveTensionDragStartX = curveMouse.x;
                curveTensionDragStartY = curveMouse.y;
                curveTensionStartInflection = tension.inflection;
                curveTensionStartSteepness = tension.steepness;
                // the handle follows the mouse along its own segment
                curveTensionDragWidth = tensionHandle(static_cast<size_t>(clickedHandle), hoveredCycle).width;
            }
        } else if(curveDragPointIndex < 0 && ImGui::GetIO().KeyAlt &&
                  (interpolation == CurveInterpolationMode::LogExp || interpolation == CurveInterpolationMode::Sigmoid) &&
                  lane->curvePoints.size() > 1) {
            for(size_t i = 1; i < lane->curvePoints.size(); ++i) {
                const auto& a = lane->curvePoints[i - 1];
                const auto& b = lane->curvePoints[i];
                if(curveSourceBeat < a.beat || curveSourceBeat > b.beat) continue;
                const float t = static_cast<float>((curveSourceBeat - a.beat) / std::max(1e-9, b.beat - a.beat));
                const auto tension = lane->curveTensions[i - 1];
                const float segmentValue = ofLerp(a.value, b.value,
                    curveSegmentShape(t, interpolation, tension));
                const float segmentY = curveValueToY(segmentValue);
                if(std::abs(curveMouse.y - segmentY) <= 12.0f) {
                    curveTensionSegment = static_cast<int>(i - 1);
                    curveTensionDragStartX = curveMouse.x;
                    curveTensionDragStartY = curveMouse.y;
                    curveTensionStartInflection = tension.inflection;
                    curveTensionStartSteepness = tension.steepness;
                    curveTensionDragWidth = right - left;
                }
                break;
            }
        }
        if(curveDragPointIndex < 0 && curveTensionSegment < 0 && clickedHandle < 0 &&
           !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift) {
            // Snapped onto a beat that has a point: after it, unless the
            // click was to its left.
            const double newBeat = std::max(0.0, std::min(content, view.snapBeat(curveSourceBeat)));
            curveDragPointIndex = insertCurvePoint(*lane, {newBeat, curveValue}, curveSourceBeat >= newBeat);
            curveSelectedPointIndices = {curveDragPointIndex}; // the new point, as if clicked
        }
        if(curveDragPointIndex >= 0) {
            curveDragCycle = hitCycle;
            curveDragSourceStartBeat = lane->curvePoints[curveDragPointIndex].beat;
            curveDragTimelineStartBeat = sourceToTimelineBeat(*clip, curveDragSourceStartBeat, curveDragCycle);
        }
    }
    // Once a point or a tension handle has been grabbed, keep updating
    // it for as long as the mouse button is held, even if the cursor
    // strays outside the curve editor's rect - a normal, fast Alt-drag
    // easily overshoots the lane's vertical bounds. Re-requiring hover
    // here made the drag silently stop applying the moment that happened.
    if((curveDragPointIndex >= 0 || curveTensionSegment >= 0) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if(curveDragPointIndex >= 0 && curveDragPointIndex < static_cast<int>(lane->curvePoints.size())) {
            // A point may sit on its neighbour's beat (a vertical step: the
            // curve jumps from the earlier point's value to the later one's),
            // never past it, so the points keep their order.
            const double minimumBeat = curveDragPointIndex > 0 ? lane->curvePoints[curveDragPointIndex - 1].beat : 0.0;
            const double maximumBeat = curveDragPointIndex + 1 < static_cast<int>(lane->curvePoints.size())
                ? lane->curvePoints[curveDragPointIndex + 1].beat : content;
            // Playback wraps at each repeat. Editing instead follows the grabbed
            // occurrence continuously, stopping at the clip/source/neighbour bounds.
            const double dragTimelineBeat = ofClamp(curveTimelineBeat, clip->startBeat, clip->startBeat + clip->durationBeats);
            const double dragSourceBeat = curveDragSourceStartBeat +
                (dragTimelineBeat - curveDragTimelineStartBeat) / effectiveStretch(*clip);
            const double clipMinimumBeat = curveDragSourceStartBeat +
                (clip->startBeat - curveDragTimelineStartBeat) / effectiveStretch(*clip);
            const double clipMaximumBeat = curveDragSourceStartBeat +
                (clip->startBeat + clip->durationBeats - curveDragTimelineStartBeat) / effectiveStretch(*clip);
            const double dragMinimumBeat = std::max(minimumBeat, clipMinimumBeat);
            const double dragMaximumBeat = std::min(maximumBeat, clipMaximumBeat);
            lane->curvePoints[curveDragPointIndex].beat = std::max(dragMinimumBeat,
                std::min(std::max(dragMinimumBeat, dragMaximumBeat), view.snapBeat(dragSourceBeat)));
            lane->curvePoints[curveDragPointIndex].value = curveValue;

            // Use the edited value (after clamping/snapping), even outside the canvas.
            if(curveValue <= 0.0f || curveValue >= 1.0f) {
                const float y = curveValueToY(curveValue);
                dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                            ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
            }
            ImGui::SetNextWindowPos(ImVec2(curveMouse.x + 16.0f, curveMouse.y + 8.0f));
            ImGui::BeginTooltip();
            if(curveDb) {
                if(curveValue <= 0.0f) ImGui::TextUnformatted("Y: -inf dB");
                else ImGui::Text("Y: %+.2f dB", ofxOceanodeTimelineScale::normalizedToDb(*lane, curveValue));
            } else ImGui::Text("Y: %.6g", lane->valueMin + curveValue * curveValueSpan);
            ImGui::EndTooltip();
        } else if(curveTensionSegment >= 0 && curveTensionSegment < static_cast<int>(lane->curveTensions.size())) {
            auto& tension = lane->curveTensions[curveTensionSegment];
            if(interpolation == CurveInterpolationMode::Sigmoid) {
                tension.inflection = ofClamp(curveTensionStartInflection +
                    (curveMouse.x - curveTensionDragStartX) / std::max(1.0f, curveTensionDragWidth), 0.01f, 0.99f);
            } else {
                tension.inflection = 0.5f;
            }
            // A Log/Exp handle sits on the middle of its segment, where the
            // curve is at 0.5^steepness of the way: it follows the mouse.
            const float yA = curveValueToY(lane->curvePoints[curveTensionSegment].value);
            const float yB = curveValueToY(lane->curvePoints[curveTensionSegment + 1].value);
            if(curveTensionFromHandle && interpolation == CurveInterpolationMode::LogExp && std::abs(yB - yA) > 4.0f) {
                if(ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                    const float way = ofClamp((curveMouse.y - yA) / (yB - yA), 0.0005f, 0.9995f);
                    tension.steepness = ofClamp(std::log(way) / std::log(0.5f), 0.1f, 10.0f);
                }
            } else {
                const float steepnessDelta = -(curveMouse.y - curveTensionDragStartY) /
                    std::max(1.0f, (curveBottom - curveTop) / 3.0f);
                tension.steepness = ofClamp(curveTensionStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
            }
        }
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        curveDragPointIndex = -1;
        curveTensionSegment = -1;
        curveTensionFromHandle = false;
    }
    // Delete / Backspace removes the selected points (Timeline focused, nothing
    // being edited). One key press: the controller records it as one undo step.
    if(!curveSelectedPointIndices.empty() && curveDragPointIndex < 0 &&
       ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !ImGui::IsPopupOpen("Curve point value") && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput &&
       (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        for(auto it = curveSelectedPointIndices.rbegin(); it != curveSelectedPointIndices.rend(); ++it)
            eraseCurvePointWithTensions(*lane, static_cast<size_t>(*it));
        curveSelectedPointIndices.clear();
        curveValuePointIndex = -1;
    }
    if(ImGui::BeginPopup("Curve point value")) {
        if(curveValuePointIndex >= 0 && curveValuePointIndex < static_cast<int>(lane->curvePoints.size())) {
            ImGui::TextUnformatted("Curve point");
            contextMenuSection("Value");
            ImGui::SetNextItemWidth(110.0f);
            if(curveDb) {
                // in dB (at or below the floor: silence)
                if(ImGui::InputFloat("dB", &curveNumericValue, 0.5f, 3.0f, "%.1f"))
                    lane->curvePoints[curveValuePointIndex].value =
                        ofxOceanodeTimelineScale::dbToNormalized(*lane, std::min(curveNumericValue, ofxOceanodeTimelineScale::topDb(*lane)));
                ImGui::TextDisabled("%g dB or below: silence", ofxOceanodeTimelineScale::kDbFloor);
            } else if(ImGui::InputFloat("Value", &curveNumericValue, 0.001f, 0.01f, "%.6g")) {
                const float span = lane->valueMax - lane->valueMin;
                float actualValue = curveNumericValue;
                if(lane->curveClamp)
                    actualValue = ofClamp(actualValue,
                                          std::min(lane->valueMin, lane->valueMax),
                                          std::max(lane->valueMin, lane->valueMax));
                lane->curvePoints[curveValuePointIndex].value = std::abs(span) < 1e-9f ? 0.0f
                    : (actualValue - lane->valueMin) / span;
            }
            contextMenuSection("Remove");
            if(ImGui::MenuItem("Delete point")) {
                eraseCurvePointWithTensions(*lane, static_cast<size_t>(curveValuePointIndex));
                curveSelectedPointIndices.clear();
                curveValuePointIndex = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    } // isFocused (curve interaction)

}

void ofxOceanodeTimelineCurveEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const auto interpolation = curveInterpolationMode(lane->curveInterpolation);
    const auto& points = lane->curvePoints; // kept sorted by beat
    const int cycles = clip.repeatContent ? static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip))) : 1;
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int cycle = firstVisibleCycle; cycle < cycles && cycle <= lastVisibleCycle; ++cycle) {
        for(size_t i = 1; i < points.size(); ++i) {
            const auto tension = i - 1 < lane->curveTensions.size()
                ? lane->curveTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
            if(interpolation == CurveInterpolationMode::Step) {
                const float x1 = min.x + beatOffset(sourceToTimelineBeat(clip, points[i - 1].beat, cycle));
                const float x2 = min.x + beatOffset(sourceToTimelineBeat(clip, points[i].beat, cycle));
                const float y1 = max.y - 5.0f - points[i - 1].value * (max.y - min.y - 10.0f);
                const float y2 = max.y - 5.0f - points[i].value * (max.y - min.y - 10.0f);
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y1), previewInk(preview, 200), 1.5f);
                dl->AddLine(ImVec2(x2, y1), ImVec2(x2, y2), previewInk(preview, 200), 1.5f);
                continue;
            }
            const int samples = 20;
            for(int sample = 0; sample < samples; ++sample) {
                const float t1 = sample / static_cast<float>(samples);
                const float t2 = (sample + 1) / static_cast<float>(samples);
                const double source1 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t1;
                const double source2 = points[i - 1].beat + (points[i].beat - points[i - 1].beat) * t2;
                const float value1 = ofLerp(points[i - 1].value, points[i].value,
                                            curveSegmentShape(t1, interpolation, tension));
                const float value2 = ofLerp(points[i - 1].value, points[i].value,
                                            curveSegmentShape(t2, interpolation, tension));
                const float x1 = min.x + beatOffset(sourceToTimelineBeat(clip, source1, cycle));
                const float x2 = min.x + beatOffset(sourceToTimelineBeat(clip, source2, cycle));
                const float y1 = max.y - 5.0f - value1 * (max.y - min.y - 10.0f);
                const float y2 = max.y - 5.0f - value2 * (max.y - min.y - 10.0f);
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), previewInk(preview, 200), 1.5f);
            }
        }
    }
    dl->PopClipRect();
    return;

}
