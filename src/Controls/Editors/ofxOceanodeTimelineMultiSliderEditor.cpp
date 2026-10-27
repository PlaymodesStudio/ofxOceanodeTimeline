#include "ofxOceanodeTimelineMultiSliderEditor.h"
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

void ofxOceanodeTimelineMultiSliderEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto* lane = &context.lane;

    int steps = std::max(1, lane->stepCount);
    propertyLabel("Steps");
    if(ImGui::DragInt("##Steps", &steps, 0.2f, 1, 128)) {
        lane->stepCount = steps;
        lane->multiSliderValues.resize(static_cast<size_t>(steps), 0.0f);
    }
    drawDivision(*lane);
    int sliderSnap = std::max(0, lane->valueQuantizeSteps);
    propertyLabel("Value Snap");
    if(ImGui::DragInt("##Value Snap", &sliderSnap, 0.1f, 0, 64))
        lane->valueQuantizeSteps = std::max(0, sliderSnap);
    if(lane->valueQuantizeSteps >= 2) ImGui::TextDisabled("%d levels", lane->valueQuantizeSteps);

}

void ofxOceanodeTimelineMultiSliderEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
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
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if(static_cast<int>(lane->multiSliderValues.size()) != std::max(1, lane->stepCount))
        lane->multiSliderValues.resize(static_cast<size_t>(std::max(1, lane->stepCount)), 0.0f);
    const int stepCount = std::max(1, lane->stepCount);
    const double beatsPerStep = std::max(1.0 / kPPQ, lane->beatsPerStep);
    const double patternLength = stepCount * beatsPerStep;
    const float cellTop = editorMin.y + 10.0f;
    const float cellBottom = editorMax.y - 10.0f;
    const float span = lane->valueMax - lane->valueMin;
    if(right > left) {
        dl->AddRectFilled(ImVec2(left, editorMin.y + 3), ImVec2(right, editorMax.y - kLaneResizeHandleHeight),
                          IM_COL32(track.color.r, track.color.g, track.color.b, 50), 2);
        if(lane->valueQuantizeSteps >= 2) {
            for(int row = 0; row <= lane->valueQuantizeSteps; ++row) {
                const float y = cellTop + (cellBottom - cellTop) * row / static_cast<float>(lane->valueQuantizeSteps);
                dl->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(150, 140, 80, 150));
            }
        }
        const double content = sourceDuration(*clip);
        const int clipCycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleDuration(*clip)))) : 1;
        const int patternCycles = std::max(1, static_cast<int>(std::ceil(content / patternLength)));
        dl->PushClipRect(ImVec2(left, cellTop), ImVec2(right, cellBottom), true);
        for(int clipCycle = 0; clipCycle < clipCycles; ++clipCycle) {
            for(int patternCycle = 0; patternCycle < patternCycles; ++patternCycle) {
                const double sourceOffset = patternCycle * patternLength;
                const double cycleSourceEnd = std::min(content, sourceOffset + patternLength);
                const float cycleX1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, sourceOffset, clipCycle));
                const float cycleX2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, cycleSourceEnd, clipCycle));
                const bool firstPattern = clipCycle == 0 && patternCycle == 0;
                if(!firstPattern) {
                    dl->AddRectFilled(ImVec2(cycleX1, cellTop), ImVec2(cycleX2, cellBottom),
                                      IM_COL32(255, 255, 255, patternCycle % 2 ? 10 : 18));
                    dl->AddLine(ImVec2(cycleX1, cellTop), ImVec2(cycleX1, cellBottom),
                                IM_COL32(track.color.r, track.color.g, track.color.b, 245), 2.0f);
                }
                for(int index = 0; index < stepCount; ++index) {
                    const double dataBeat = index * beatsPerStep;
                    const double sourceStart = sourceOffset + dataBeat;
                    const double sourceEnd = std::min(content, sourceStart + beatsPerStep);
                    if(sourceStart >= content || sourceEnd <= sourceStart) continue;
                    const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, sourceStart, clipCycle));
                    const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, sourceEnd, clipCycle));
                    if(x2 < left || x1 > right) continue;
                    dl->AddLine(ImVec2(x1, cellTop), ImVec2(x1, cellBottom),
                                index == 0 ? kBar : kGrid, index == 0 ? 1.5f : 1.0f);
                    const float value = index < static_cast<int>(lane->multiSliderValues.size()) ? lane->multiSliderValues[index] : 0.0f;
                    const float normalized = std::abs(span) < 1e-9f ? 0.0f : ofClamp((value - lane->valueMin) / span, 0.0f, 1.0f);
                    const float fillTop = cellBottom - normalized * (cellBottom - cellTop);
                    dl->AddRectFilled(ImVec2(std::max(x1, left) + 1, fillTop),
                                      ImVec2(std::min(x2, right) - 1, cellBottom),
                                      IM_COL32(track.color.r, track.color.g, track.color.b, firstPattern ? 210 : 120), 2);
                    dl->AddRect(ImVec2(std::max(x1, left) + 1, cellTop),
                                ImVec2(std::min(x2, right) - 1, cellBottom),
                                IM_COL32(130, 130, 130, firstPattern ? 110 : 65), 1.0f);
                    if(firstPattern && x2 - x1 > 28.0f) {
                        char valueLabel[16];
                        std::snprintf(valueLabel, sizeof(valueLabel), "%.2g", value);
                        dl->AddText(ImVec2(std::max(x1, left) + 3, cellTop + 3), IM_COL32(240, 240, 240, 220), valueLabel);
                    }
                }
            }
        }
        dl->PopClipRect();
        if(isFocused) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool inSliderEditor = hoverRect(ImVec2(left, cellTop), ImVec2(right, cellBottom));
            if(inSliderEditor && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
                const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
                const double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
                const double patternBeat = std::fmod(sourceBeat, patternLength);
                const int index = ofClamp(static_cast<int>(std::floor(patternBeat / beatsPerStep)), 0, stepCount - 1);
                if(index >= 0 && index < static_cast<int>(lane->multiSliderValues.size())) {
                    float normalized = ofClamp((cellBottom - mouse.y) / (cellBottom - cellTop), 0.0f, 1.0f);
                    if(lane->valueQuantizeSteps >= 2)
                        normalized = std::round(normalized * lane->valueQuantizeSteps) / static_cast<float>(lane->valueQuantizeSteps);
                    lane->multiSliderValues[index] = lane->valueMin + normalized * span;
                }
            }
            if(inSliderEditor && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
                const double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
                const double patternBeat = std::fmod(sourceBeat, patternLength);
                const int index = ofClamp(static_cast<int>(std::floor(patternBeat / beatsPerStep)), 0, stepCount - 1);
                if(index >= 0 && index < static_cast<int>(lane->multiSliderValues.size())) lane->multiSliderValues[index] = 0.0f;
            }
        } // isFocused (multi-slider interaction)
    }

}

void ofxOceanodeTimelineMultiSliderEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const double miniPatternLength = std::max(1.0 / kPPQ, lane->stepCount * lane->beatsPerStep);
    const double miniContent = sourceDuration(clip);
    const int miniClipCycles = clip.repeatContent
        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip)))) : 1;
    const int miniPatternCycles = std::max(1, static_cast<int>(std::ceil(miniContent / miniPatternLength)));
    const float span = lane->valueMax - lane->valueMin;
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int clipCycle = firstVisibleCycle; clipCycle < miniClipCycles && clipCycle <= lastVisibleCycle; ++clipCycle) {
        for(int patternCycle = 0; patternCycle < miniPatternCycles; ++patternCycle) {
            for(int index = 0; index < lane->stepCount && index < static_cast<int>(lane->multiSliderValues.size()); ++index) {
                const double sourceStart = patternCycle * miniPatternLength + index * lane->beatsPerStep;
                if(sourceStart >= miniContent) continue;
                const float x1 = min.x + beatOffset(sourceToTimelineBeat(clip, sourceStart, clipCycle));
                const float x2 = min.x + beatOffset(sourceToTimelineBeat(clip, sourceStart + lane->beatsPerStep, clipCycle));
                const float normalized = std::abs(span) < 1e-9f ? 0.0f
                    : ofClamp((lane->multiSliderValues[index] - lane->valueMin) / span, 0.0f, 1.0f);
                const float barTop = max.y - 5.0f - normalized * (max.y - min.y - 10.0f);
                dl->AddRectFilled(ImVec2(x1 + 1, barTop), ImVec2(x2 - 1, max.y - 5.0f), IM_COL32(245, 245, 245, 190));
            }
        }
    }
    dl->PopClipRect();
    return;

}
