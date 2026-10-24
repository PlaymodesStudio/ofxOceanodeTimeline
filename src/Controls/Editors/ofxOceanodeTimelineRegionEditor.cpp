#include "ofxOceanodeTimelineRegionEditor.h"
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

void ofxOceanodeTimelineRegionEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;

    int rows = std::max(1, lane->multiRowCount);
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "Rows"));
    if(ImGui::DragInt("Rows", &rows, 0.1f, 1, 16))
        timeline.setLaneMultiRowCount(track.id, clip->id, lane->id, rows);
    drawDivision(*lane);
    if(lane->type == ofxOceanodeTimelineLaneType::MultiValue) {
        ImGui::TextDisabled("Dbl-click a block to edit");
        if(ImGui::Checkbox("Integer", &lane->multiValueInteger) && lane->multiValueInteger) {
            for(auto& row : lane->multiValueRows)
                for(auto& region : row)
                    region.value = std::round(region.value);
        }
    } else ImGui::TextDisabled("Drag to draw gates");

}

void ofxOceanodeTimelineRegionEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
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

    const bool isValueRow = lane->type == ofxOceanodeTimelineLaneType::MultiValue;
    const int rowCount = std::max(1, lane->multiRowCount);
    if(isValueRow) {
        if(static_cast<int>(lane->multiValueRows.size()) != rowCount) lane->multiValueRows.resize(static_cast<size_t>(rowCount));
    } else {
        if(static_cast<int>(lane->multiGateRows.size()) != rowCount) lane->multiGateRows.resize(static_cast<size_t>(rowCount));
    }
    const float rowsTop = editorMin.y + 6.0f;
    const float rowsBottom = editorMax.y - 6.0f;
    const float rowGap = 3.0f;
    const float rowHeight = std::max(6.0f, (rowsBottom - rowsTop - rowGap * std::max(0, rowCount - 1)) / rowCount);
    auto rowTopY = [&](int r) { return rowsTop + r * (rowHeight + rowGap); };
    auto rowBottomY = [&](int r) { return rowTopY(r) + rowHeight; };
    const int clipCycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleDuration(*clip)))) : 1;
    const float span = lane->valueMax - lane->valueMin;
    {
        const double multiGrid = view.displayGridBeats();
        const int multiGridLines = static_cast<int>(std::ceil(endBeat / multiGrid));
        for(int gridIndex = 0; gridIndex <= multiGridLines; ++gridIndex) {
            const double beat = gridIndex * multiGrid;
            const float x = timelineMin.x + beatOffset(beat);
            const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
            if(bar || beatOffset(beat + multiGrid) - beatOffset(beat) >= 4.0f)
                dl->AddLine(ImVec2(x, rowsTop), ImVec2(x, rowsBottom), bar ? kBar : kGrid, bar ? 1.5f : 1.0f);
        }
    }
    dl->PushClipRect(ImVec2(left, rowsTop), ImVec2(right, rowsBottom), true);
    for(int r = 0; r < rowCount; ++r) {
        const float rTop = rowTopY(r), rBottom = rowBottomY(r);
        dl->AddRectFilled(ImVec2(left, rTop), ImVec2(right, rBottom), IM_COL32(255, 255, 255, r % 2 ? 8 : 14));
        for(int cycle = 0; cycle < clipCycles; ++cycle) {
            if(isValueRow) {
                for(const auto& region : lane->multiValueRows[r]) {
                    const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, region.startBeat, cycle));
                    const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, region.end(), cycle));
                    if(x2 < left || x1 > right) continue;
                    const float normalized = std::abs(span) < 1e-9f ? 0.0f : ofClamp((region.value - lane->valueMin) / span, 0.0f, 1.0f);
                    const float fillTop = rBottom - normalized * (rBottom - rTop);
                    dl->AddRectFilled(ImVec2(std::max(x1, left) + 1, fillTop), ImVec2(std::min(x2, right) - 1, rBottom),
                                      IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 220 : 110), 2);
                    dl->AddRect(ImVec2(std::max(x1, left) + 1, rTop), ImVec2(std::min(x2, right) - 1, rBottom),
                                IM_COL32(200, 200, 200, cycle == 0 ? 160 : 70), 1.0f);
                    if(cycle == 0 && x2 - x1 > 26.0f) {
                        char valueLabel[16];
                        std::snprintf(valueLabel, sizeof(valueLabel), lane->multiValueInteger ? "%.0f" : "%.3g", region.value);
                        dl->AddText(ImVec2(std::max(x1, left) + 3, rTop + 2), IM_COL32(240, 240, 240, 225), valueLabel);
                    }
                }
            } else {
                for(const auto& region : lane->multiGateRows[r]) {
                    const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, region.startBeat, cycle));
                    const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, region.end(), cycle));
                    if(x2 < left || x1 > right) continue;
                    dl->AddRectFilled(ImVec2(std::max(x1, left) + 1, rTop + 1), ImVec2(std::min(x2, right) - 1, rBottom - 1),
                                      IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 225 : 115), 2);
                }
            }
        }
    }
    dl->PopClipRect();
    if(isFocused) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
        const double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
        int hoveredRow = -1;
        for(int r = 0; r < rowCount; ++r) {
            if(mouse.y >= rowTopY(r) && mouse.y <= rowBottomY(r)) { hoveredRow = r; break; }
        }
        const bool inRegionEditor = editorCanvasHovered && mouse.x >= left && mouse.x <= right &&
            mouse.y >= rowsTop && mouse.y <= rowsBottom;

        auto hitValueRegion = [&](int r, double beat) -> int {
            if(!isValueRow || r < 0 || r >= static_cast<int>(lane->multiValueRows.size())) return -1;
            const auto& regionsInRow = lane->multiValueRows[r];
            for(int i = static_cast<int>(regionsInRow.size()) - 1; i >= 0; --i)
                if(beat >= regionsInRow[i].startBeat && beat <= regionsInRow[i].end()) return i;
            return -1;
        };
        auto hitGateRegion = [&](int r, double beat) -> int {
            if(isValueRow || r < 0 || r >= static_cast<int>(lane->multiGateRows.size())) return -1;
            const auto& regionsInRow = lane->multiGateRows[r];
            for(int i = static_cast<int>(regionsInRow.size()) - 1; i >= 0; --i)
                if(beat >= regionsInRow[i].startBeat && beat <= regionsInRow[i].end()) return i;
            return -1;
        };
        auto hitRegion = [&](int r, double beat) { return isValueRow ? hitValueRegion(r, beat) : hitGateRegion(r, beat); };

        if(inRegionEditor && hoveredRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const int hit = hitRegion(hoveredRow, sourceBeat);
            if(hit >= 0) {
                if(isValueRow) lane->multiValueRows[hoveredRow].erase(lane->multiValueRows[hoveredRow].begin() + hit);
                else lane->multiGateRows[hoveredRow].erase(lane->multiGateRows[hoveredRow].begin() + hit);
            }
        }
        if(isValueRow && inRegionEditor && hoveredRow >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const int hit = hitRegion(hoveredRow, sourceBeat);
            if(hit >= 0) {
                multiValueEditRow = hoveredRow;
                multiValueEditIndex = hit;
                multiValueEditNumeric = lane->multiValueInteger
                    ? std::round(lane->multiValueRows[hoveredRow][hit].value)
                    : lane->multiValueRows[hoveredRow][hit].value;
                ImGui::OpenPopup("Multi value block");
            }
        }
        if(inRegionEditor && hoveredRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
           regionDragMode == RegionDragMode::None) {
            const int hit = hitRegion(hoveredRow, sourceBeat);
            if(hit >= 0) {
                const double regionStart = isValueRow ? lane->multiValueRows[hoveredRow][hit].startBeat : lane->multiGateRows[hoveredRow][hit].startBeat;
                const double regionDuration = isValueRow ? lane->multiValueRows[hoveredRow][hit].durationBeats : lane->multiGateRows[hoveredRow][hit].durationBeats;
                const int hoveredCycle = clip->repeatContent
                    ? std::max(0, static_cast<int>(std::floor(std::max(0.0, timelineBeat - clip->startBeat) / cycleDuration(*clip)))) : 0;
                const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, regionStart + regionDuration, hoveredCycle));
                const bool nearRightEdge = std::abs(mouse.x - x2) <= 6.0f;
                regionDragMode = nearRightEdge ? RegionDragMode::ResizeRight : RegionDragMode::Move;
                regionDragRow = hoveredRow;
                regionDragIndex = hit;
                regionDragAnchorBeat = sourceBeat;
                regionDragOriginalStart = regionStart;
                regionDragOriginalDuration = regionDuration;
            } else {
                const double gridStep = std::max(1.0 / kPPQ, lane->beatsPerStep);
                const double startBeat = std::max(0.0, std::floor(sourceBeat / gridStep) * gridStep);
                if(isValueRow) {
                    ofxOceanodeTimelineValueRegion newRegion;
                    newRegion.startBeat = startBeat;
                    newRegion.durationBeats = gridStep;
                    newRegion.value = lane->valueMin + ofClamp((rowBottomY(hoveredRow) - mouse.y) / std::max(1.0f, rowHeight), 0.0f, 1.0f) * span;
                    if(lane->multiValueInteger) newRegion.value = std::round(newRegion.value);
                    lane->multiValueRows[hoveredRow].push_back(newRegion);
                    regionDragIndex = static_cast<int>(lane->multiValueRows[hoveredRow].size()) - 1;
                } else {
                    ofxOceanodeTimelineGateRegion newRegion;
                    newRegion.startBeat = startBeat;
                    newRegion.durationBeats = gridStep;
                    lane->multiGateRows[hoveredRow].push_back(newRegion);
                    regionDragIndex = static_cast<int>(lane->multiGateRows[hoveredRow].size()) - 1;
                }
                regionDragMode = RegionDragMode::Create;
                regionDragRow = hoveredRow;
                regionDragAnchorBeat = startBeat;
                regionDragOriginalStart = startBeat;
                regionDragOriginalDuration = gridStep;
            }
        }
        const int regionRowCount = isValueRow ? static_cast<int>(lane->multiValueRows.size()) : static_cast<int>(lane->multiGateRows.size());
        if(regionDragMode != RegionDragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
           regionDragRow >= 0 && regionDragRow < regionRowCount) {
            const int regionCount = isValueRow ? static_cast<int>(lane->multiValueRows[regionDragRow].size())
                                                : static_cast<int>(lane->multiGateRows[regionDragRow].size());
            if(regionDragIndex >= 0 && regionDragIndex < regionCount) {
                double newStart = regionDragOriginalStart;
                double newDuration = regionDragOriginalDuration;
                const double gridStep = std::max(1.0 / kPPQ, lane->beatsPerStep);
                if(regionDragMode == RegionDragMode::Move) {
                    newStart = std::max(0.0, regionDragOriginalStart + (sourceBeat - regionDragAnchorBeat));
                    newStart = std::max(0.0, std::round(newStart / gridStep) * gridStep);
                } else {
                    newDuration = std::max(1.0 / kPPQ, sourceBeat - newStart);
                    newDuration = std::max(gridStep, std::round(newDuration / gridStep) * gridStep);
                }
                if(isValueRow) {
                    auto& region = lane->multiValueRows[regionDragRow][regionDragIndex];
                    region.startBeat = newStart;
                    region.durationBeats = newDuration;
                    const float rTop = rowTopY(regionDragRow), rBottom = rowBottomY(regionDragRow);
                    region.value = lane->valueMin + ofClamp((rBottom - mouse.y) / std::max(1.0f, rBottom - rTop), 0.0f, 1.0f) * span;
                    if(lane->multiValueInteger) region.value = std::round(region.value);
                } else {
                    auto& region = lane->multiGateRows[regionDragRow][regionDragIndex];
                    region.startBeat = newStart;
                    region.durationBeats = newDuration;
                }
            }
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            regionDragMode = RegionDragMode::None;
            regionDragRow = -1;
            regionDragIndex = -1;
        }
        if(isValueRow && ImGui::BeginPopup("Multi value block")) {
            if(multiValueEditRow >= 0 && multiValueEditRow < static_cast<int>(lane->multiValueRows.size()) &&
               multiValueEditIndex >= 0 && multiValueEditIndex < static_cast<int>(lane->multiValueRows[multiValueEditRow].size())) {
                ImGui::SetNextItemWidth(110.0f);
                if(ImGui::InputFloat("Value", &multiValueEditNumeric, 0.001f, 0.01f, lane->multiValueInteger ? "%.0f" : "%.6g")) {
                    multiValueEditNumeric = lane->multiValueInteger ? std::round(multiValueEditNumeric) : multiValueEditNumeric;
                    lane->multiValueRows[multiValueEditRow][multiValueEditIndex].value = multiValueEditNumeric;
                }
                if(ImGui::Button("Delete block")) {
                    lane->multiValueRows[multiValueEditRow].erase(lane->multiValueRows[multiValueEditRow].begin() + multiValueEditIndex);
                    multiValueEditRow = -1;
                    multiValueEditIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    } // isFocused (multi-value / multi-gate interaction)

}

void ofxOceanodeTimelineRegionEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool isValueRow = lane->type == ofxOceanodeTimelineLaneType::MultiValue;
    const int rowCount = std::max(1, lane->multiRowCount);
    const int miniCycles = clip.repeatContent
        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip)))) : 1;
    const float rowHeight = std::max(2.0f, (max.y - min.y - 10.0f) / rowCount);
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int r = 0; r < rowCount; ++r) {
        const float rTop = min.y + 5.0f + r * rowHeight;
        const float rBottom = rTop + rowHeight - 1.0f;
        for(int cycle = firstVisibleCycle; cycle < miniCycles && cycle <= lastVisibleCycle; ++cycle) {
            if(isValueRow && r < static_cast<int>(lane->multiValueRows.size())) {
                for(const auto& region : lane->multiValueRows[r]) {
                    const float x1 = min.x + beatOffset(sourceToTimelineBeat(clip, region.startBeat, cycle));
                    const float x2 = min.x + beatOffset(sourceToTimelineBeat(clip, region.end(), cycle));
                    dl->AddRectFilled(ImVec2(x1, rTop), ImVec2(x2, rBottom), IM_COL32(245, 245, 245, 170));
                }
            } else if(!isValueRow && r < static_cast<int>(lane->multiGateRows.size())) {
                for(const auto& region : lane->multiGateRows[r]) {
                    const float x1 = min.x + beatOffset(sourceToTimelineBeat(clip, region.startBeat, cycle));
                    const float x2 = min.x + beatOffset(sourceToTimelineBeat(clip, region.end(), cycle));
                    dl->AddRectFilled(ImVec2(x1, rTop), ImVec2(x2, rBottom), IM_COL32(245, 245, 245, 170));
                }
            }
        }
    }
    dl->PopClipRect();
    return;

}
