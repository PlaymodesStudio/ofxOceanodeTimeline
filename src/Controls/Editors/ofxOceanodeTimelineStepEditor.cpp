#include "ofxOceanodeTimelineStepEditor.h"
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

void ofxOceanodeTimelineStepEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto* lane = &context.lane;

    int steps = std::max(1, lane->stepCount);
    propertyLabel("Steps");
    if(ImGui::DragInt("##Steps", &steps, 0.2f, 1, 128)) {
        lane->stepCount = steps;
    }
    drawDivision(*lane);
    int behavior = lane->behavior == "Always" ? 1 : lane->behavior == "Mute" ? 2 : 0;
    const char* behaviors[] = {"Probability", "Always", "Mute"};
    propertyLabel("Mode");
    if(ImGui::Combo("##Mode", &behavior, behaviors, 3)) lane->behavior = behaviors[behavior];
    ImGui::Checkbox("Probability", &lane->probabilityEnabled);
    propertyLabel("Seed", smallButtonReserve("New##stepSeed"));
    ImGui::DragInt("##Seed", &lane->probabilitySeed, 1.0f, 0, 99999);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Which steps a probability below 100%% lets through.\nChange it for another pattern.");
    ImGui::SameLine();
    if(ImGui::SmallButton("New##stepSeed")) lane->probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));

}

void ofxOceanodeTimelineStepEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    const auto& editorTrackId = track.id;
    const auto& editorClipId = clip->id;
    const bool isFocused = context.focused;
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float left = canvas.left, right = canvas.right;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const int stepCount = std::max(1, lane->stepCount);
    const double beatsPerStep = std::max(1.0 / kPPQ, lane->beatsPerStep);
    const double patternLength = stepCount * beatsPerStep;
    const float cellTop = editorMin.y + 10.0f;
    const float laneBottom = editorMax.y - 10.0f;
    // Velocity sliders sit in a strip under the probability cells, when the
    // lane is tall enough to hold both.
    const bool showVelocity = laneBottom - cellTop >= 60.0f;
    const float velocityHeight = showVelocity ? ofClamp((laneBottom - cellTop) * 0.3f, 18.0f, 48.0f) : 0.0f;
    const float velocityBottom = laneBottom;
    const float velocityTop = velocityBottom - velocityHeight;
    const float cellBottom = showVelocity ? velocityTop - 4.0f : laneBottom;
    if(right > left) {
        dl->AddRectFilled(ImVec2(left, editorMin.y + 3),
                          ImVec2(right, editorMax.y - kLaneResizeHandleHeight),
                          IM_COL32(track.color.r, track.color.g, track.color.b, 50), 2);
        const double content = sourceDuration(*clip);
        const int clipCycles = clip->repeatContent
            ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleDuration(*clip)))) : 1;
        const int patternCycles = std::max(1, static_cast<int>(std::ceil(content / patternLength)));
        dl->PushClipRect(ImVec2(left, cellTop), ImVec2(right, laneBottom), true);
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
                    const auto stepIt = std::find_if(lane->step.steps.begin(), lane->step.steps.end(), [&](const auto& step) {
                        return std::abs(step.startBeat - dataBeat) < 1.0 / kPPQ;
                    });
                    const float probability = stepIt == lane->step.steps.end() ? 0.0f : ofClamp(stepIt->probability, 0.0f, 1.0f);
                    if(probability > 0.0f) {
                        const float fillTop = cellBottom - probability * (cellBottom - cellTop);
                        dl->AddRectFilled(ImVec2(std::max(x1, left) + 1, fillTop),
                                              ImVec2(std::min(x2, right) - 1, cellBottom),
                                              IM_COL32(240, 240, 240, firstPattern ? 215 : 125), 2);
                    } else {
                        dl->AddRect(ImVec2(std::max(x1, left) + 1, cellTop),
                                    ImVec2(std::min(x2, right) - 1, cellBottom),
                                    IM_COL32(130, 130, 130, firstPattern ? 110 : 65), 1.0f);
                    }
                    if(firstPattern && x2 - x1 > 28.0f && stepIt != lane->step.steps.end()) {
                        char probabilityLabel[16];
                        std::snprintf(probabilityLabel, sizeof(probabilityLabel), "%.0f%%", probability * 100.0f);
                        dl->AddText(ImVec2(std::max(x1, left) + 3, cellTop + 3),
                                    IM_COL32(35, 35, 35, 230), probabilityLabel);
                    }
                    if(showVelocity && probability > 0.0f) {
                        const float velocity = ofClamp(stepIt->velocity, 0.0f, 1.0f);
                        const ImVec2 sliderMin(std::max(x1, left) + 1, velocityTop);
                        const ImVec2 sliderMax(std::min(x2, right) - 1, velocityBottom);
                        dl->AddRectFilled(sliderMin, sliderMax, IM_COL32(20, 20, 20, firstPattern ? 150 : 90), 2);
                        dl->AddRectFilled(ImVec2(sliderMin.x, velocityBottom - velocity * velocityHeight), sliderMax,
                                          IM_COL32(track.color.r, track.color.g, track.color.b, firstPattern ? 230 : 120), 2);
                        if(firstPattern && x2 - x1 > 28.0f && velocityHeight >= ImGui::GetTextLineHeight()) {
                            char velocityLabel[16];
                            std::snprintf(velocityLabel, sizeof(velocityLabel), "%.0f", velocity * 100.0f);
                            dl->AddText(ImVec2(sliderMin.x + 2, velocityTop + 1), IM_COL32(245, 245, 245, 220), velocityLabel);
                        }
                    }
                }
            }
        }
        dl->PopClipRect();
        if(isFocused) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inStepEditor = hoverRect(ImVec2(left, cellTop), ImVec2(right, cellBottom));
        const bool inVelocity = showVelocity && hoverRect(ImVec2(left, velocityTop), ImVec2(right, velocityBottom));
        auto cellBeatAtMouse = [&]() {
            const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
            const double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
            const double patternBeat = std::fmod(sourceBeat, patternLength);
            const int index = ofClamp(static_cast<int>(std::floor(patternBeat / beatsPerStep)), 0, stepCount - 1);
            return index * beatsPerStep;
        };
        auto findStep = [&](double cellBeat) {
            return std::find_if(lane->step.steps.begin(), lane->step.steps.end(), [&](const auto& step) {
                return std::abs(step.startBeat - cellBeat) < 1.0 / kPPQ;
            });
        };
        auto stepAtMouse = [&]() {
            const double cellBeat = cellBeatAtMouse();
            auto stepIt = findStep(cellBeat);
            if(stepIt == lane->step.steps.end()) {
                timeline.setClipStep(sequence, editorTrackId, editorClipId, lane->id, cellBeat, "1", beatsPerStep);
                stepIt = findStep(cellBeat);
            }
            return stepIt;
        };
        if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = Drag::None;
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            drag = inStepEditor ? Drag::Probability : inVelocity ? Drag::Velocity : Drag::None;
        if(drag == Drag::Probability && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
           mouse.x >= left && mouse.x <= right) {
            const auto stepIt = stepAtMouse();
            if(stepIt != lane->step.steps.end()) {
                stepIt->probability = ofClamp((cellBottom - mouse.y) / (cellBottom - cellTop), 0.0f, 1.0f);
            }
        }
        // Velocity only edits steps that can sound: an empty cell has no slider.
        if(drag == Drag::Velocity && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
           mouse.x >= left && mouse.x <= right) {
            const auto stepIt = findStep(cellBeatAtMouse());
            if(stepIt != lane->step.steps.end() && stepIt->probability > 0.0f) {
                stepIt->velocity = ofClamp((velocityBottom - mouse.y) / velocityHeight, 0.0f, 1.0f);
            }
        }
        if(inVelocity && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const auto stepIt = findStep(cellBeatAtMouse());
            if(stepIt != lane->step.steps.end() && stepIt->probability > 0.0f) stepIt->velocity = 1.0f;
        }
        if(inVelocity) ImGui::SetTooltip("Step velocity. Right-click: 100%%");
        if(inStepEditor && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const auto stepIt = stepAtMouse();
            if(stepIt != lane->step.steps.end()) stepIt->probability = stepIt->probability > 0.5f ? 0.0f : 1.0f;
        }
        } // isFocused (step interaction)
    }

}

void ofxOceanodeTimelineStepEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& track = preview.track;
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const double patternLength = std::max(1.0 / kPPQ, lane->stepCount * lane->beatsPerStep);
    const double content = sourceDuration(clip);
    const int clipCycles = clip.repeatContent
        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip)))) : 1;
    const int patternCycles = std::max(1, static_cast<int>(std::ceil(content / patternLength)));
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int clipCycle = firstVisibleCycle; clipCycle < clipCycles && clipCycle <= lastVisibleCycle; ++clipCycle) {
        for(int patternCycle = 0; patternCycle < patternCycles; ++patternCycle) {
            const double offset = patternCycle * patternLength;
            if(clipCycle > 0 || patternCycle > 0) {
                const float markerX = min.x + beatOffset(sourceToTimelineBeat(clip, offset, clipCycle));
                dl->AddLine(ImVec2(markerX, min.y + 3.0f), ImVec2(markerX, max.y - 3.0f),
                            IM_COL32(track.color.r, track.color.g, track.color.b, 245), 2.0f);
            }
            for(const auto& step : lane->step.steps) {
                const double stepEnd = std::min(patternLength, step.startBeat +
                    (step.durationBeats > 0.0 ? step.durationBeats : lane->beatsPerStep));
                const double startSource = offset + step.startBeat;
                const double endSource = std::min(content, offset + stepEnd);
                if(startSource >= content || endSource <= startSource) continue;
                const float sx1 = min.x + beatOffset(sourceToTimelineBeat(clip, startSource, clipCycle));
                const float sx2 = min.x + beatOffset(sourceToTimelineBeat(clip, endSource, clipCycle));
                const float probability = ofClamp(step.probability, 0.0f, 1.0f);
                const float top = max.y - 6.0f - probability * (max.y - min.y - 12.0f);
                dl->AddRectFilled(ImVec2(std::max(sx1, min.x) + 2, top),
                                      ImVec2(std::min(sx2, max.x) - 2, max.y - 6),
                                      IM_COL32(245, 245, 245, 85), 2);
            }
        }
    }
    dl->PopClipRect();

}
