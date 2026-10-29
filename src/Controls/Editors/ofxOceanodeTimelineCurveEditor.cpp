#include "ofxOceanodeTimelineCurveEditor.h"
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
    if(selectedInterpolation == CurveInterpolationMode::LogExp) ImGui::TextDisabled("Alt-drag vertically");
    else if(selectedInterpolation == CurveInterpolationMode::Sigmoid) ImGui::TextDisabled("Alt-drag freely");
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
    const float curveTop = editorMin.y + 8.0f, curveBottom = editorMax.y - 8.0f;
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
                        db == 0.0f ? IM_COL32(150, 150, 150, 170) : IM_COL32(75, 75, 75, 120));
            char dbLabel[16];
            std::snprintf(dbLabel, sizeof(dbLabel), "%g dB", db);
            if(y - ImGui::GetTextLineHeight() > curveTop + ImGui::GetTextLineHeight() &&
               y < curveBottom - ImGui::GetTextLineHeight())
                dl->AddText(ImVec2(left + 5.0f, y - ImGui::GetTextLineHeight()), IM_COL32(170, 170, 178, 200), dbLabel);
        }
    } else {
        for(int row = 0; row <= 4; ++row) {
            const float y = curveTop + (curveBottom - curveTop) * row / 4.0f;
            dl->AddLine(ImVec2(timelineMin.x, y), ImVec2(editorMax.x, y), IM_COL32(75, 75, 75, 120));
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
            dl->AddLine(ImVec2(x, curveTop), ImVec2(x, curveBottom), bar ? kBar : kGrid, bar ? 1.5f : 1.0f);
    }
    // Points stay in beat order; an edit that moves one past a neighbour is
    // reordered here. Stable, so two points on one beat (a vertical step)
    // never swap -- an unstable sort could reorder them every frame.
    const auto byBeat = [](const auto& a, const auto& b) { return a.beat < b.beat; };
    if(!std::is_sorted(lane->curvePoints.begin(), lane->curvePoints.end(), byBeat))
        std::stable_sort(lane->curvePoints.begin(), lane->curvePoints.end(), byBeat);
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
    dl->PushClipRect(ImVec2(left, curveTop), ImVec2(right, curveBottom), true);
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
        for(const auto& point : points) {
            const ImVec2 p(timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, point.beat, cycle)), curveValueToY(point.value));
            dl->AddCircleFilled(p, cycle == 0 ? 4.0f : 3.0f, IM_COL32(245, 245, 245, cycle == 0 ? 230 : 115));
        }
    }
    dl->PopClipRect();
    // Require the editor canvas item itself to be hoverable, not merely
    // geometric overlap. This prevents clicks in the numeric popup from
    // passing through and creating a point underneath it.
    if(isFocused) {
    const bool curveHovered = editorCanvasHovered && !ImGui::IsPopupOpen("Curve point value") && right > left &&
        hoverRect(ImVec2(left, curveTop), ImVec2(right, curveBottom));
    const ImVec2 curveMouse = ImGui::GetIO().MousePos;
    const double curveTimelineBeat = beatAtOffset(curveMouse.x - timelineMin.x);
    const double curveSourceBeat = timelineToSourceBeat(*clip, curveTimelineBeat);
    const int hoveredCycle = clip->repeatContent
        ? std::max(0, static_cast<int>(std::floor(std::max(0.0, curveTimelineBeat - clip->startBeat) / repeatCycleDuration))) : 0;
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
            const float pointX = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, point.beat, hoveredCycle));
            const float pointY = curveValueToY(point.value);
            if(std::abs(curveMouse.x - pointX) <= 8.0f && std::abs(curveMouse.y - pointY) <= 8.0f) return i;
        }
        return -1;
    };

    if(curveHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        curveValuePointIndex = hitPoint();
        if(curveValuePointIndex >= 0) {
            const float pointValue = lane->curvePoints[curveValuePointIndex].value;
            curveNumericValue = curveDb ? std::max(-120.0f, ofxOceanodeTimelineScale::normalizedToDb(*lane, pointValue))
                                        : lane->valueMin + pointValue * (lane->valueMax - lane->valueMin);
            ImGui::OpenPopup("Curve point value");
        }
    }
    if(curveHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        curveDragPointIndex = hitPoint();
        curveTensionSegment = -1;
        if(curveDragPointIndex >= 0 && ImGui::GetIO().KeyShift) {
            eraseCurvePointWithTensions(*lane, static_cast<size_t>(curveDragPointIndex));
            curveDragPointIndex = -1;
            curveValuePointIndex = -1;
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
                }
                break;
            }
        }
        if(curveDragPointIndex < 0 && curveTensionSegment < 0 &&
           !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift) {
            curveDragPointIndex = insertCurvePoint(*lane, {
                std::max(0.0, std::min(content, view.snapBeat(curveSourceBeat))), curveValue
            });
        }
    }
    // Once a point or a tension handle has been grabbed, keep updating
    // it for as long as the mouse button is held, even if the cursor
    // strays outside the curve editor's rect - a normal, fast Alt-drag
    // easily overshoots the lane's vertical bounds. Re-requiring hover
    // here made the drag silently stop applying the moment that happened.
    if((curveDragPointIndex >= 0 || curveTensionSegment >= 0) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if(curveDragPointIndex >= 0 && curveDragPointIndex < static_cast<int>(lane->curvePoints.size())) {
            const double minimumBeat = curveDragPointIndex > 0 ? lane->curvePoints[curveDragPointIndex - 1].beat + 1.0 / kPPQ : 0.0;
            const double maximumBeat = curveDragPointIndex + 1 < static_cast<int>(lane->curvePoints.size())
                ? lane->curvePoints[curveDragPointIndex + 1].beat - 1.0 / kPPQ : content;
            lane->curvePoints[curveDragPointIndex].beat = std::max(minimumBeat,
                std::min(std::max(minimumBeat, maximumBeat), view.snapBeat(curveSourceBeat)));
            lane->curvePoints[curveDragPointIndex].value = curveValue;
        } else if(curveTensionSegment >= 0 && curveTensionSegment < static_cast<int>(lane->curveTensions.size())) {
            auto& tension = lane->curveTensions[curveTensionSegment];
            if(interpolation == CurveInterpolationMode::Sigmoid) {
                tension.inflection = ofClamp(curveTensionStartInflection +
                    (curveMouse.x - curveTensionDragStartX) / std::max(1.0f, right - left), 0.01f, 0.99f);
            } else {
                tension.inflection = 0.5f;
            }
            const float steepnessDelta = -(curveMouse.y - curveTensionDragStartY) /
                std::max(1.0f, (curveBottom - curveTop) / 3.0f);
            tension.steepness = ofClamp(curveTensionStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
        }
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        curveDragPointIndex = -1;
        curveTensionSegment = -1;
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
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y1), IM_COL32(245, 245, 245, 200), 1.5f);
                dl->AddLine(ImVec2(x2, y1), ImVec2(x2, y2), IM_COL32(245, 245, 245, 200), 1.5f);
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
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(245, 245, 245, 200), 1.5f);
            }
        }
    }
    dl->PopClipRect();
    return;

}
