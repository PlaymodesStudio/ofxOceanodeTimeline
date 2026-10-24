#include "ofxOceanodeTimelineLfoEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

bool ofxOceanodeTimelineLfoEditor::draw(
    ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
    const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
    const ofxOceanodeTimelineEditorView& view,
    ofxOceanodeTimelineClipEditorState& editorState,
    float contentWidth, double beatPosition) {
    bool renameRequested = false;

    const float timelineScrollX = timeline.getViewState().scrollX;
    // Laid out like drawLaneEditor's rows rather than as one fixed block: a
    // row per section, each reserving its own height from editorState.laneEditorHeights,
    // foldable through editorState.collapsedLaneIds and resizable by the same bottom
    // strip. The result view is a row like any other -- it just has no lane
    // behind it, so it borrows a key that cannot collide with a lane id.
    const float clipIndent = 20.0f;
    const std::string resultKey = "lfoResult:" + clip.id;
    constexpr float kDefaultResultHeight = 230.0f;
    constexpr float kDefaultLaneHeight = 120.0f;

    ImGui::Dummy(ImVec2(contentWidth, 22.0f));
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(headerMin.x + clipIndent, headerMin.y),
                      ImVec2(headerMin.x + kLabelWidth, headerMax.y),
                      mutedTrackColor(track.color, 0.28f, 0.46f));
    dl->AddLine(ImVec2(headerMin.x + kLabelWidth - 1.0f, headerMin.y),
                ImVec2(headerMin.x + kLabelWidth - 1.0f, headerMax.y),
                IM_COL32(track.color.r, track.color.g, track.color.b, 210));
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x + 5.0f + clipIndent, headerMin.y + 2.0f));
    const bool editorFolded = editorState.foldedClipEditors.count(clip.id) > 0;
    if(ImGui::SmallButton(editorFolded ? ">##foldLfoEditor" : "v##foldLfoEditor")) {
        if(editorFolded) editorState.foldedClipEditors.erase(clip.id); else editorState.foldedClipEditors.insert(clip.id);
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(editorFolded ? "Unfold the clip editor" : "Fold the clip editor to this header");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(track.color.r / 255.0f, track.color.g / 255.0f,
                              track.color.b / 255.0f, 1.0f),
                       "%s  [LFO]", clip.name.c_str());
    if(ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click to rename this clip");
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) renameRequested = true;
    }
    ImGui::SameLine(kLabelWidth - 50.0f);
    if(ImGui::SmallButton("x##closeLfoEditor")) editorState.clipEditorOpen = false;
    finishAbsoluteLayout(ImVec2(headerMin.x, headerMax.y));
    if(editorFolded) return renameRequested; // folded: only the header row is shown

    // Every row is a full-width block starting at the same x, so one origin
    // serves them all -- and it is kLabelWidth, the same gutter the clip rows
    // and every other lane editor use.
    const float rowOriginX = headerMin.x;
    const float graphOriginX = rowOriginX + kLabelWidth - timelineScrollX;
    const float graphWidth = std::max(40.0f, contentWidth - kLabelWidth);
    const float fallbackBpm = view.fallbackBpm;
    const double contentDuration = std::max(1.0 / kPPQ, clip.contentDurationBeats);
    const ImU32 lfoColor = IM_COL32(track.color.r, track.color.g, track.color.b, 235);
    const int samples = std::max(64, static_cast<int>(graphWidth / 2.0f));
    const double resultSourceBeat = timelineToSourceBeat(clip, beatPosition);

    auto xForSource = [&](double sourceBeat) {
        return graphOriginX + view.beatToPixels(sourceBeat);
    };
    const ofxOceanodeTimelineEditorView sourceView(timeline, fallbackBpm, contentDuration, false, 0.0);
    auto sourceForX = [&](float x) {
        return sourceView.pixelsToBeat(x - graphOriginX);
    };
    auto drawGrid = [&](const ImVec2& graphMin, const ImVec2& graphMax) {
        const double grid = std::max(1.0 / kPPQ, view.displayGridBeats());
        const int gridLines = static_cast<int>(std::ceil(contentDuration / grid));
        for(int i = 0; i <= gridLines; ++i) {
            const double beat = std::min(contentDuration, i * grid);
            const float x = xForSource(beat);
            if(x < graphMin.x || x > graphMax.x) continue;
            const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
            dl->AddLine(ImVec2(x, graphMin.y), ImVec2(x, graphMax.y),
                        bar ? kBar : kGrid, bar ? 1.5f : 1.0f);
        }
    };
    auto drawPlayhead = [&](const ImVec2& graphMin, const ImVec2& graphMax) {
        const float x = xForSource(resultSourceBeat);
        if(x >= graphMin.x && x <= graphMax.x)
            dl->AddLine(ImVec2(x, graphMin.y), ImVec2(x, graphMax.y), kPlayhead, 1.5f);
    };

    // One folded row, shared by the result view and the lanes. Returns true
    // when its label was clicked, which is how a lane takes focus.
    auto drawCollapsedRow = [&](const std::string& key, const std::string& label, bool focused) {
        ImGui::Dummy(ImVec2(contentWidth, 22.0f));
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        ImDrawList* rowDl = ImGui::GetWindowDrawList();
        rowDl->AddRectFilled(ImVec2(rowMin.x + clipIndent, rowMin.y),
                             ImVec2(rowMin.x + kLabelWidth, rowMax.y),
                             mutedTrackColor(track.color, 0.18f, 0.30f));
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 5.0f + clipIndent, rowMin.y + 2.0f));
        if(ImGui::SmallButton((">##lfoExpand" + key).c_str())) editorState.collapsedLaneIds.erase(key);
        ImGui::SameLine();
        ImGui::Selectable((label + "##lfoHeaderCollapsed" + key).c_str(), focused,
                          ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0));
        const bool clicked = ImGui::IsItemClicked();
        finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
        return clicked;
    };

    struct LfoRow {
        ImVec2 rowMin, rowMax, graphMin, graphMax;
        float height = 0.0f;
        bool canvasHovered = false;
    };
    // Reserves one row, paints its background, runs the bottom resize strip
    // and hands back the graph rectangle -- the same sequence, in the same
    // order, as an ordinary lane editor row.
    auto beginRow = [&](const std::string& key, float defaultHeight, bool focused) {
        LfoRow row;
        const auto heightIt = editorState.laneEditorHeights.find(key);
        row.height = heightIt != editorState.laneEditorHeights.end() ? heightIt->second : defaultHeight;
        ImGui::Dummy(ImVec2(contentWidth, row.height));
        row.rowMin = ImGui::GetItemRectMin();
        row.rowMax = ImGui::GetItemRectMax();
        const float zoneLeft = row.rowMin.x + kLabelWidth;
        // The canvas stops short of the resize strip so the two can never
        // fight over the same click.
        ImGui::SetCursorScreenPos(ImVec2(zoneLeft, row.rowMin.y));
        ImGui::InvisibleButton(("##lfoRowCanvas" + editorState.editorTrackId + editorState.editorClipId + key).c_str(),
                               ImVec2(std::max(1.0f, contentWidth - kLabelWidth),
                                      std::max(1.0f, row.height - kLaneResizeHandleHeight)));
        row.canvasHovered = ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x >= zoneLeft;

        dl->AddRectFilled(ImVec2(row.rowMin.x + clipIndent, row.rowMin.y), row.rowMax,
                          IM_COL32(27, 29, 35, 235));
        dl->AddRectFilled(ImVec2(row.rowMin.x + clipIndent, row.rowMin.y),
                          ImVec2(zoneLeft, row.rowMax.y),
                          mutedTrackColor(track.color, focused ? 0.30f : 0.21f, 0.42f));
        dl->AddLine(ImVec2(zoneLeft - 1.0f, row.rowMin.y), ImVec2(zoneLeft - 1.0f, row.rowMax.y),
                    IM_COL32(track.color.r, track.color.g, track.color.b, focused ? 220 : 150));

        const ImVec2 handleMin(row.rowMin.x + clipIndent, row.rowMax.y - kLaneResizeHandleHeight);
        const ImVec2 handleMax(row.rowMax.x, row.rowMax.y);
        const bool resizeHovered = hoverRect(handleMin, handleMax);
        if(resizeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            editorState.resizingLaneId = key;
            editorState.laneResizeStartMouseY = ImGui::GetIO().MousePos.y;
            editorState.laneResizeStartHeight = row.height;
        }
        const bool resizeActive = editorState.resizingLaneId == key && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if(resizeHovered || resizeActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(resizeActive) {
            editorState.laneEditorHeights[key] = ofClamp(editorState.laneResizeStartHeight + ImGui::GetIO().MousePos.y - editorState.laneResizeStartMouseY,
                                             kLaneEditorMinHeight, kLaneEditorMaxHeight);
        }
        if(editorState.resizingLaneId == key && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) editorState.resizingLaneId.clear();
        const float separatorY = row.rowMax.y - kLaneResizeHandleHeight * 0.5f;
        dl->AddLine(ImVec2(handleMin.x, separatorY), ImVec2(handleMax.x, separatorY),
                    resizeActive ? IM_COL32(205, 205, 215, 235)
                                 : resizeHovered ? IM_COL32(160, 160, 170, 205)
                                                 : IM_COL32(80, 80, 86, 110),
                    resizeActive ? 2.0f : 1.0f);

        row.graphMin = ImVec2(zoneLeft, row.rowMin.y + 5.0f);
        row.graphMax = ImVec2(row.rowMax.x, row.rowMax.y - 5.0f - kLaneResizeHandleHeight);
        return row;
    };
    // Opens the row's left-hand properties child with its fold button; the
    // caller fills it and closes it with EndChild, hence the "begin" name.
    auto beginRowProperties = [&](const LfoRow& row, const std::string& key) {
        ImGui::SetCursorScreenPos(ImVec2(row.rowMin.x + 5.0f + clipIndent, row.rowMin.y + 4.0f));
        ImGui::BeginChild(("##lfoRowProperties" + editorState.editorTrackId + editorState.editorClipId + key).c_str(),
                          ImVec2(kLabelWidth - 10.0f - clipIndent,
                                 std::max(1.0f, row.height - 8.0f - kLaneResizeHandleHeight)), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if(ImGui::SmallButton(("v##lfoCollapse" + key).c_str())) editorState.collapsedLaneIds.insert(key);
        ImGui::SameLine();
    };

    // --- result row ------------------------------------------------------
    // Sampled from the same evaluator playback uses, so editing any control
    // lane below previews exactly what the target binding receives.
    if(editorState.collapsedLaneIds.count(resultKey) > 0) {
        drawCollapsedRow(resultKey, "LFO result", false);
    } else {
        const auto row = beginRow(resultKey, kDefaultResultHeight, false);
        beginRowProperties(row, resultKey);
        ImGui::TextUnformatted("LFO result");
        const auto* outputBinding = clip.lfoOutputBindingId.empty()
            ? nullptr : timeline.getBinding(track.id, clip.lfoOutputBindingId);
        const std::string outputPreview = outputBinding == nullptr
            ? "No output target" : compactParameterName(outputBinding->parameterPath);
        ImGui::SetNextItemWidth(-1.0f);
        if(ImGui::BeginCombo("Output", outputPreview.c_str())) {
            for(const auto& binding : track.bindings) {
                const bool selected = binding.id == clip.lfoOutputBindingId;
                if(ImGui::Selectable(compactParameterName(binding.parameterPath).c_str(), selected)) {
                    clip.lfoOutputBindingId = binding.id;
                    if(binding.valueType == typeid(float).name()) {
                        if(auto* parameter = container.findTimelineParameter(binding.parameterPath)) {
                            clip.lfoOutputMin = parameter->cast<float>().getParameter().getMin();
                            clip.lfoOutputMax = parameter->cast<float>().getParameter().getMax();
                        }
                    } else if(binding.valueType == typeid(int).name()) {
                        if(auto* parameter = container.findTimelineParameter(binding.parameterPath)) {
                            clip.lfoOutputMin = static_cast<float>(parameter->cast<int>().getParameter().getMin());
                            clip.lfoOutputMax = static_cast<float>(parameter->cast<int>().getParameter().getMax());
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("The LFO result is mapped to this range.");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("Min", &clip.lfoOutputMin, 0.01f, -99999.0f, 99999.0f, "%.4g");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("Max", &clip.lfoOutputMax, 0.01f, -99999.0f, 99999.0f, "%.4g");
        if(clip.lfoOutputMax < clip.lfoOutputMin) std::swap(clip.lfoOutputMax, clip.lfoOutputMin);
        ImGui::EndChild();

        drawGrid(row.graphMin, row.graphMax);
        for(int level = 0; level <= 4; ++level) {
            const float y = row.graphMax.y - 8.0f - level / 4.0f *
                (row.graphMax.y - row.graphMin.y - 16.0f);
            dl->AddLine(ImVec2(row.graphMin.x, y), ImVec2(row.graphMax.x, y),
                        level == 0 || level == 4 ? IM_COL32(130, 135, 150, 125) : IM_COL32(75, 80, 95, 90));
        }
        dl->PushClipRect(row.graphMin, row.graphMax, true);
        for(int i = 0; i < samples; ++i) {
            const double source1 = contentDuration * i / static_cast<double>(samples);
            const double source2 = contentDuration * (i + 1) / static_cast<double>(samples);
            const float value1 = ofxOceanodeTimelineLfo::evaluate(clip, source1);
            const float value2 = ofxOceanodeTimelineLfo::evaluate(clip, source2);
            const ImVec2 a(xForSource(source1), row.graphMax.y - 8.0f - value1 *
                           (row.graphMax.y - row.graphMin.y - 16.0f));
            const ImVec2 b(xForSource(source2), row.graphMax.y - 8.0f - value2 *
                           (row.graphMax.y - row.graphMin.y - 16.0f));
            dl->AddLine(a, b, lfoColor, 2.0f);
        }
        dl->PopClipRect();
        drawPlayhead(row.graphMin, row.graphMax);
        finishAbsoluteLayout(ImVec2(row.rowMin.x, row.rowMax.y));
    }

    // --- one row per oscillator control ----------------------------------
    bool anyLfoCanvasHovered = false;
    for(size_t laneIndex = 0; laneIndex < clip.lanes.size(); ++laneIndex) {
        auto& lane = clip.lanes[laneIndex];
        const bool focused = editorState.editorLaneId == lane.id;
        const float current = ofxOceanodeTimelineLfo::evaluateParameter(clip, lane.lfoParameter,
                                                                        resultSourceBeat, 0.0f);
        if(editorState.collapsedLaneIds.count(lane.id) > 0) {
            char collapsedLabel[96];
            std::snprintf(collapsedLabel, sizeof(collapsedLabel), "%s   %.3g", lane.name.c_str(), current);
            if(drawCollapsedRow(lane.id, collapsedLabel, focused)) editorState.editorLaneId = lane.id;
            continue;
        }

        const auto row = beginRow(lane.id, kDefaultLaneHeight, focused);
        anyLfoCanvasHovered = anyLfoCanvasHovered || row.canvasHovered;
        beginRowProperties(row, lane.id);
        if(ImGui::Selectable((lane.name + "##lfoLaneHeader" + lane.id).c_str(), focused,
                             ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0))) {
            editorState.editorLaneId = lane.id;
        }
        ImGui::TextDisabled("%.3g", current);
        const float laneSpan = lane.valueMax - lane.valueMin;
        {
            int interpolationMode = lane.curveInterpolation == "Value" ? 0
                : 1 + static_cast<int>(curveInterpolationMode(lane.curveInterpolation));
            const int previousMode = interpolationMode;
            const float fieldWidth = std::max(40.0f, std::min(105.0f, ImGui::GetContentRegionAvail().x - 4.0f));
            ImGui::SetNextItemWidth(fieldWidth);
            if(ImGui::Combo(("##lfoInterpolation" + lane.id).c_str(), &interpolationMode, kLfoInterpolationNames, 5) &&
               interpolationMode != previousMode) {
                if(interpolationMode == 0) {
                    // Entering Value mode: start from what the lane outputs at the playhead.
                    lane.lfoValue = ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane, current);
                } else {
                    if(previousMode != 0) resetCurveTensions(lane);
                    // Leaving Value mode keeps the stored points; a lane without any
                    // starts from the constant it held.
                    if(lane.curvePoints.empty()) {
                        lane.curvePoints.push_back({0.0, lane.lfoValue});
                        resetCurveTensions(lane);
                    }
                }
                lane.curveInterpolation = kLfoInterpolationNames[interpolationMode];
                lfoTensionSegment = -1;
                if(lfoSelectedLaneId == lane.id) lfoSelectedPointIndex = -1;
            }
            if(lane.curveInterpolation == "Value") {
                // Drag, or double-click / Ctrl+click to type a value.
                float realValue = ofxOceanodeTimelineLfo::laneValueFromNormalized(lane, lane.lfoValue);
                const float lo = std::min(lane.valueMin, lane.valueMax);
                const float hi = std::max(lane.valueMin, lane.valueMax);
                ImGui::SetNextItemWidth(fieldWidth);
                if(ImGui::DragFloat(("##lfoValue" + lane.id).c_str(), &realValue,
                                    lane.lfoParameter == "frequency"
                                        ? std::max(1e-3f, std::abs(realValue) * 0.01f) // proportional: fine at short cycles
                                        : std::max(1e-4f, std::abs(laneSpan) / 300.0f),
                                    lo, hi, "%.4g")) {
                    realValue = ofClamp(realValue, lo, hi);
                    lane.lfoValue = ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane, realValue);
                }
            } else {
                const auto modeShape = curveInterpolationMode(lane.curveInterpolation);
                if(modeShape == CurveInterpolationMode::LogExp) ImGui::TextDisabled("Alt-drag vertically");
                else if(modeShape == CurveInterpolationMode::Sigmoid) ImGui::TextDisabled("Alt-drag freely");
            }
            if(lane.lfoParameter == "frequency") {
                ImGui::Checkbox(("Snap##lfoSnap" + lane.id).c_str(), &lane.lfoSnap);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Snap edits to the grid lengths shown in the lane");
                ImGui::SameLine();
                const char* snapModes[] = {"Straight", "Triplet", "Dotted"};
                int snapMode = std::max(0, std::min(2, lane.lfoSnapMode));
                ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - 4.0f));
                // Only new edits use the new grid; existing points keep their values.
                if(ImGui::Combo(("##lfoSnapMode" + lane.id).c_str(), &snapMode, snapModes, 3))
                    lane.lfoSnapMode = snapMode;
            }
        }
        ImGui::EndChild();
        lane.curveTensions.resize(lane.curvePoints.empty() ? 0 : lane.curvePoints.size() - 1);
        const auto laneInterpolation = curveInterpolationMode(lane.curveInterpolation);
        const bool valueMode = lane.curveInterpolation == "Value";
        const bool laneSelected = !valueMode && lfoSelectedClipId == clip.id && lfoSelectedLaneId == lane.id &&
            lfoSelectedPointIndex >= 0 && lfoSelectedPointIndex < static_cast<int>(lane.curvePoints.size());

        drawGrid(row.graphMin, row.graphMax);
        dl->AddLine(ImVec2(row.graphMin.x, row.graphMax.y - 1.0f),
                    ImVec2(row.graphMax.x, row.graphMax.y - 1.0f), IM_COL32(95, 100, 115, 110));
        const float valueSpan = std::max(1.0f, row.graphMax.y - row.graphMin.y - 12.0f);
        auto yForValue = [&](float value) { return row.graphMax.y - 6.0f - value * valueSpan; };
        const bool frequencyLane = lane.lfoParameter == "frequency";
        // Normalized value under the mouse, snapped to the frequency grid when enabled.
        auto normalizedAtMouseY = [&](float mouseY) {
            float normalized = ofClamp((row.graphMax.y - 6.0f - mouseY) / valueSpan, 0.0f, 1.0f);
            if(frequencyLane && lane.lfoSnap) {
                normalized = ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane,
                    ofxOceanodeTimelineLfo::snapFrequencyValue(ofxOceanodeTimelineLfo::laneValueFromNormalized(lane, normalized), lane.lfoSnapMode));
            }
            return normalized;
        };
        dl->PushClipRect(row.graphMin, row.graphMax, true);
        if(frequencyLane) {
            // Reference lines at musical cycle lengths (log-spaced, so evenly spread):
            // the selected grid (straight / triplet / dotted) labelled, and the
            // straight lengths kept faint underneath for orientation.
            const int gridMode = std::max(0, std::min(2, lane.lfoSnapMode));
            if(gridMode != 0) {
                for(float gridValue : ofxOceanodeTimelineLfo::frequencyGridValues(0)) {
                    const float y = yForValue(ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane, gridValue));
                    dl->AddLine(ImVec2(row.graphMin.x, y), ImVec2(row.graphMax.x, y), IM_COL32(80, 84, 98, 55));
                }
            }
            for(float gridValue : ofxOceanodeTimelineLfo::frequencyGridValues(gridMode)) {
                const float y = yForValue(ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane, gridValue));
                const bool oneBeat = gridMode == 0 && gridValue == 1.0f;
                dl->AddLine(ImVec2(row.graphMin.x, y), ImVec2(row.graphMax.x, y),
                            oneBeat ? IM_COL32(150, 150, 165, 120) : IM_COL32(95, 100, 115, 85));
                const std::string label = ofxOceanodeTimelineLfo::frequencyGridLabel(gridValue, gridMode);
                dl->AddText(ImVec2(row.graphMin.x + 3.0f, y - ImGui::GetTextLineHeight() + 1.0f),
                            IM_COL32(160, 165, 180, 150), label.c_str());
            }
        }
        if(valueMode) {
            // Value mode: one constant across the whole lane; points are hidden (kept for later).
            const float y = yForValue(lane.lfoValue);
            dl->AddLine(ImVec2(row.graphMin.x, y), ImVec2(row.graphMax.x, y), lfoColor, focused ? 2.0f : 1.5f);
        }
        for(int i = 0; i < (valueMode ? 0 : samples); ++i) {
            const double source1 = contentDuration * i / static_cast<double>(samples);
            const double source2 = contentDuration * (i + 1) / static_cast<double>(samples);
            dl->AddLine(ImVec2(xForSource(source1), yForValue(valueAtBeat(lane.curvePoints, lane.curveTensions, lane.curveInterpolation, source1, 0.0f))),
                        ImVec2(xForSource(source2), yForValue(valueAtBeat(lane.curvePoints, lane.curveTensions, lane.curveInterpolation, source2, 0.0f))),
                        lfoColor, focused ? 2.0f : 1.5f);
        }
        for(size_t pointIndex = 0; pointIndex < (valueMode ? 0 : lane.curvePoints.size()); ++pointIndex) {
            const auto& point = lane.curvePoints[pointIndex];
            const float x = xForSource(point.beat);
            if(x >= row.graphMin.x - 6.0f && x <= row.graphMax.x + 6.0f) {
                dl->AddCircleFilled(ImVec2(x, yForValue(point.value)), focused ? 4.0f : 3.0f,
                                    focused ? IM_COL32(245, 235, 150, 255) : lfoColor);
                if(laneSelected && static_cast<int>(pointIndex) == lfoSelectedPointIndex)
                    dl->AddCircle(ImVec2(x, yForValue(point.value)), 7.0f, IM_COL32(255, 255, 255, 240), 0, 2.0f);
            }
        }
        dl->PopClipRect();
        drawPlayhead(row.graphMin, row.graphMax);

        if(valueMode) {
            // Clicking or dragging in the graph also sets the constant.
            if(row.canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                editorState.editorLaneId = lane.id;
                lfoDragLaneId = lane.id;
                lfoDragPointIndex = -1;
                lfoTensionSegment = -1;
                lfoValueDragActive = true;
            }
            if(lfoValueDragActive && lfoDragLaneId == lane.id && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float normalized = normalizedAtMouseY(ImGui::GetIO().MousePos.y);
                if(!frequencyLane && curveSnapLevels(lane) >= 2)
                    normalized = std::round(normalized * lane.valueQuantizeSteps) / static_cast<float>(lane.valueQuantizeSteps);
                lane.lfoValue = normalized;
            }
            finishAbsoluteLayout(ImVec2(row.rowMin.x, row.rowMax.y));
            continue;
        }

        auto hitLfoPoint = [&](const ImVec2& mouse) {
            int hit = -1;
            float nearest = 9.0f;
            for(int i = static_cast<int>(lane.curvePoints.size()) - 1; i >= 0; --i) {
                const float px = xForSource(lane.curvePoints[i].beat);
                const float py = yForValue(lane.curvePoints[i].value);
                const float distance = std::hypot(mouse.x - px, mouse.y - py);
                if(distance < nearest) { nearest = distance; hit = i; }
            }
            return hit;
        };
        auto selectLfoPoint = [&](int index) {
            lfoSelectedClipId = clip.id;
            lfoSelectedLaneId = lane.id;
            lfoSelectedPointIndex = index;
        };
        auto deleteLfoPoint = [&](int index) {
            // Keep at least one point so the lane still defines its control.
            if(index < 0 || index >= static_cast<int>(lane.curvePoints.size()) || lane.curvePoints.size() <= 1) return;
            eraseCurvePointKeepingTensions(lane, static_cast<size_t>(index));
            if(lfoSelectedLaneId == lane.id) lfoSelectedPointIndex = -1;
            lfoDragPointIndex = -1;
            lfoTensionSegment = -1;
        };
        const std::string pointPopupId = "LFO point##" + lane.id;

        if(row.canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const int hit = hitLfoPoint(ImGui::GetIO().MousePos);
            if(hit >= 0) {
                editorState.editorLaneId = lane.id;
                selectLfoPoint(hit);
                lfoPointNumericValue = ofxOceanodeTimelineLfo::laneValueFromNormalized(lane, lane.curvePoints[hit].value);
                ImGui::OpenPopup(pointPopupId.c_str());
            }
        }
        if(row.canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            editorState.editorLaneId = lane.id;
            lfoDragLaneId = lane.id;
            lfoDragPointIndex = -1;
            lfoTensionSegment = -1;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const double beat = view.snapBeat(ofClamp(sourceForX(mouse.x), 0.0, contentDuration));
            const float value = normalizedAtMouseY(mouse.y);
            lfoDragPointIndex = hitLfoPoint(mouse);
            const bool alt = ImGui::GetIO().KeyAlt;
            if(lfoDragPointIndex >= 0 && ImGui::GetIO().KeyShift) {
                deleteLfoPoint(lfoDragPointIndex);
            } else if(lfoDragPointIndex >= 0) {
                selectLfoPoint(lfoDragPointIndex);
            } else if(alt && (laneInterpolation == CurveInterpolationMode::LogExp ||
                              laneInterpolation == CurveInterpolationMode::Sigmoid) &&
                      lane.curvePoints.size() > 1) {
                // Alt-click near a segment: grab its tension (same gesture as the Curve editor).
                const double sourceBeat = sourceForX(mouse.x);
                for(size_t i = 1; i < lane.curvePoints.size(); ++i) {
                    const auto& a = lane.curvePoints[i - 1];
                    const auto& b = lane.curvePoints[i];
                    if(sourceBeat < a.beat || sourceBeat > b.beat) continue;
                    const float t = static_cast<float>((sourceBeat - a.beat) / std::max(1e-9, b.beat - a.beat));
                    const auto tension = lane.curveTensions[i - 1];
                    const float segmentY = yForValue(ofLerp(a.value, b.value, curveSegmentShape(t, laneInterpolation, tension)));
                    if(std::abs(mouse.y - segmentY) <= 12.0f) {
                        lfoTensionSegment = static_cast<int>(i - 1);
                        lfoTensionDragStartX = mouse.x;
                        lfoTensionDragStartY = mouse.y;
                        lfoTensionStartInflection = tension.inflection;
                        lfoTensionStartSteepness = tension.steepness;
                    }
                    break;
                }
            } else if(!alt) {
                lfoDragPointIndex = insertLinearCurvePoint(lane, {beat, value});
                selectLfoPoint(lfoDragPointIndex);
            }
        }
        if(lfoDragLaneId == lane.id && lfoTensionSegment >= 0 &&
           lfoTensionSegment < static_cast<int>(lane.curveTensions.size()) &&
           ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            auto& tension = lane.curveTensions[lfoTensionSegment];
            if(laneInterpolation == CurveInterpolationMode::Sigmoid) {
                tension.inflection = ofClamp(lfoTensionStartInflection +
                    (mouse.x - lfoTensionDragStartX) / std::max(1.0f, row.graphMax.x - row.graphMin.x), 0.01f, 0.99f);
            } else {
                tension.inflection = 0.5f;
            }
            const float steepnessDelta = -(mouse.y - lfoTensionDragStartY) / std::max(1.0f, valueSpan / 3.0f);
            tension.steepness = ofClamp(lfoTensionStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
        }
        // Delete / Backspace removes the selected point (Timeline focused, nothing being edited).
        if(laneSelected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
           (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
            deleteLfoPoint(lfoSelectedPointIndex);
        }
        if(ImGui::BeginPopup(pointPopupId.c_str())) {
            const bool valid = lfoSelectedLaneId == lane.id && lfoSelectedPointIndex >= 0 &&
                lfoSelectedPointIndex < static_cast<int>(lane.curvePoints.size());
            if(!valid) {
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::TextUnformatted("LFO point");
                contextMenuSection("Value");
                // Set value: type or drag an exact value (real units, e.g. beats for Frequency).
                const float lo = std::min(lane.valueMin, lane.valueMax);
                const float hi = std::max(lane.valueMin, lane.valueMax);
                ImGui::SetNextItemWidth(110.0f);
                if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if(ImGui::InputFloat("Value", &lfoPointNumericValue, 0.0f, 0.0f, "%.6g")) {
                    lane.curvePoints[lfoSelectedPointIndex].value =
                        ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane, ofClamp(lfoPointNumericValue, lo, hi));
                }
                if(ImGui::IsItemDeactivatedAfterEdit()) lfoPointNumericValue = ofClamp(lfoPointNumericValue, lo, hi);
                contextMenuSection("Remove");
                // Decide once: deleting changes the point count before EndDisabled.
                const bool deleteDisabled = lane.curvePoints.size() <= 1;
                if(deleteDisabled) ImGui::BeginDisabled();
                if(ImGui::MenuItem("Delete point")) deleteLfoPoint(lfoSelectedPointIndex);
                if(deleteDisabled) {
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("A lane keeps at least one point");
                }
            }
            ImGui::EndPopup();
        }
        if(lfoDragLaneId == lane.id && lfoDragPointIndex >= 0 &&
           lfoDragPointIndex < static_cast<int>(lane.curvePoints.size()) &&
           ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const double beat = view.snapBeat(ofClamp(sourceForX(mouse.x), 0.0, contentDuration));
            const double minimum = lfoDragPointIndex > 0
                ? lane.curvePoints[lfoDragPointIndex - 1].beat + 1.0 / kPPQ : 0.0;
            const double maximum = lfoDragPointIndex + 1 < static_cast<int>(lane.curvePoints.size())
                ? lane.curvePoints[lfoDragPointIndex + 1].beat - 1.0 / kPPQ : contentDuration;
            lane.curvePoints[lfoDragPointIndex].beat = std::max(minimum, std::min(maximum, beat));
            lane.curvePoints[lfoDragPointIndex].value = normalizedAtMouseY(mouse.y);
        }
        finishAbsoluteLayout(ImVec2(row.rowMin.x, row.rowMax.y));
    }

    // A left click anywhere outside the LFO lanes (e.g. selecting a clip) drops the
    // point selection, so Delete goes back to acting on the timeline selection.
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !anyLfoCanvasHovered &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        lfoSelectedPointIndex = -1;
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if(!lfoDragLaneId.empty()) {
            if(auto* lane = timeline.getLane(track.id, clip.id, lfoDragLaneId))
                std::sort(lane->curvePoints.begin(), lane->curvePoints.end(),
                          [](const auto& a, const auto& b) { return a.beat < b.beat; });
        }
        lfoDragLaneId.clear();
        lfoDragPointIndex = -1;
        lfoTensionSegment = -1;
        lfoValueDragActive = false;
    }
    // Drop a stale selection (clip closed/changed, point removed elsewhere).
    if(lfoSelectedClipId != clip.id) lfoSelectedPointIndex = -1;

    return renameRequested;
}

void ofxOceanodeTimelineLfoEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    const double content = sourceDuration(clip);
    const int samples = std::max(24, static_cast<int>((right - left) / 3.0f));
    const float graphTop = min.y + 5.0f;
    const float graphBottom = max.y - 5.0f;
    for(int sample = 0; sample < samples; ++sample) {
        const double source1 = content * sample / static_cast<double>(samples);
        const double source2 = content * (sample + 1) / static_cast<double>(samples);
        const float value1 = ofxOceanodeTimelineLfo::evaluate(clip, source1);
        const float value2 = ofxOceanodeTimelineLfo::evaluate(clip, source2);
        const float sx1 = min.x + beatOffset(sourceToTimelineBeat(clip, source1, 0));
        const float sx2 = min.x + beatOffset(sourceToTimelineBeat(clip, source2, 0));
        dl->AddLine(ImVec2(sx1, graphBottom - value1 * (graphBottom - graphTop)),
                    ImVec2(sx2, graphBottom - value2 * (graphBottom - graphTop)),
                    IM_COL32(245, 245, 245, 200), 1.5f);
    }
    dl->PopClipRect();
    return;

}
