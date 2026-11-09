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

namespace {
// Control sublanes sit this much further in than the lanes of the clip.
constexpr float kControlIndent = 14.0f;
constexpr float kDefaultControlHeight = 110.0f;
constexpr float kCollapsedRowHeight = 22.0f;

// Draws f(sourceBeat) (0..1) over the timeline from x0 to x1 the way the
// clip plays it there: every repeat, stretch and content offset. Walks the
// screen, so a wrap of the content (a repeat, a rotated start) breaks the
// line instead of drawing back across the clip.
template<typename Value>
void drawOverClip(ImDrawList* dl, const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineEditorView& view,
                  float originX, float x0, float x1, float top, float bottom, ImU32 color, float thickness, Value&& value) {
    const float clipX0 = originX + view.beatToPixels(clip.startBeat);
    const float clipX1 = originX + view.beatToPixels(clip.startBeat + std::max(1.0 / 24.0, clip.durationBeats));
    x0 = std::max(x0, clipX0);
    x1 = std::min(x1, clipX1);
    if(x1 <= x0) return;
    constexpr float kStep = 2.0f;
    bool havePrevious = false;
    ImVec2 previous;
    double previousSource = 0.0;
    for(float x = x0; ; x += kStep) {
        const float at = std::min(x, x1 - 0.01f);
        const double beat = view.pixelsToBeat(at - originX);
        const double source = timelineToSourceBeat(clip, beat);
        const ImVec2 point(at, bottom - ofClamp(value(source), 0.0f, 1.0f) * (bottom - top));
        if(havePrevious && source >= previousSource - 1e-9) dl->AddLine(previous, point, color, thickness);
        previous = point;
        previousSource = source;
        havePrevious = true;
        if(x >= x1) break;
    }
}

// The repeats of the clip whose start reaches [fromBeat, toBeat].
void visibleCycles(const ofxOceanodeTimelineClip& clip, double fromBeat, double toBeat, int& first, int& last) {
    first = 0; last = 0;
    if(!clip.repeatContent) return;
    const double cycle = std::max(1e-6, cycleDuration(clip));
    const int cycles = std::max(1, static_cast<int>(std::ceil(std::max(1.0 / 24.0, clip.durationBeats) / cycle - 1e-9)));
    first = ofClamp(static_cast<int>(std::floor((fromBeat - clip.startBeat) / cycle)), 0, cycles - 1);
    last = ofClamp(static_cast<int>(std::floor((toBeat - clip.startBeat) / cycle)), first, cycles - 1);
}

float controlNormalized(const ofxOceanodeTimelineLane& control, double sourceBeat) {
    if(control.curveInterpolation == "Value") return control.lfoValue;
    return valueAtBeat(control.curvePoints, control.curveTensions, control.curveInterpolation, sourceBeat,
                       control.curvePoints.empty() ? control.lfoValue : control.curvePoints.front().value);
}
} // namespace

void ofxOceanodeTimelineLfoEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    int audible = 0;
    for(const auto& control : context.lane.lfoControls)
        audible += ofxOceanodeTimelineLfo::controlAudible(context.lane, control) ? 1 : 0;
    if(audible < static_cast<int>(context.lane.lfoControls.size()))
        ImGui::TextDisabled("%d of %zu controls muted or not soloed: they use their default.",
                            static_cast<int>(context.lane.lfoControls.size()) - audible, context.lane.lfoControls.size());
}

void ofxOceanodeTimelineLfoEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    if(preview.lane == nullptr) return;
    const auto& lane = *preview.lane;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(preview.left + 1.0f, preview.min.y + 3.0f), ImVec2(preview.right - 1.0f, preview.max.y - 3.0f), true);
    drawOverClip(dl, preview.clip, preview.view, preview.min.x, preview.left, preview.right,
                 preview.min.y + 5.0f, preview.max.y - 5.0f, previewInk(preview, 200), 1.5f,
                 [&](double source) { return ofxOceanodeTimelineLfo::evaluate(lane, source); });
    dl->PopClipRect();
}

void ofxOceanodeTimelineLfoEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                                             const ofxOceanodeTimelineLaneCanvas& canvas) {
    const auto& track = context.track;
    const auto& clip = context.clip;
    auto& lane = context.lane;
    const auto& view = context.view;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float top = canvas.min.y + 10.0f, bottom = canvas.max.y - 10.0f - kLaneResizeHandleHeight * 0.5f;

    for(int row = 0; row <= 4; ++row) {
        const float y = top + (bottom - top) * row / 4.0f;
        dl->AddLine(ImVec2(canvas.timelineOrigin.x, y), ImVec2(canvas.max.x, y),
                    row == 0 || row == 4 ? IM_COL32(110, 112, 125, 140) : IM_COL32(75, 75, 75, 120));
    }
    // Grid and repeat markers, as a Curve lane draws them.
    const double grid = view.displayGridBeats();
    const double visibleFrom = view.pixelsToBeat(std::max(0.0f, canvas.left - canvas.timelineOrigin.x));
    const double visibleTo = std::min(view.endBeat, view.pixelsToBeat(std::max(0.0f, canvas.right - canvas.timelineOrigin.x)) + grid);
    for(int index = std::max(0, static_cast<int>(std::floor(visibleFrom / grid))); index <= static_cast<int>(std::ceil(visibleTo / grid)); ++index) {
        const double beat = index * grid;
        const float x = canvas.timelineOrigin.x + view.beatToPixels(beat);
        const bool bar = std::fmod(beat, context.timeline.getBeatsPerBar()) < 0.001;
        if(bar || view.beatToPixels(beat + grid) - view.beatToPixels(beat) >= 4.0f)
            dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), bar ? kBar : kGrid, bar ? 1.5f : 1.0f);
    }
    int firstCycle = 0, lastCycle = 0;
    visibleCycles(clip, visibleFrom, visibleTo, firstCycle, lastCycle);
    for(int cycle = std::max(1, firstCycle); cycle <= lastCycle; ++cycle) {
        const float x = canvas.timelineOrigin.x + view.beatToPixels(clip.startBeat + cycle * cycleDuration(clip));
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(track.color.r, track.color.g, track.color.b, 220), 2.0f);
    }
    char maxLabel[32], minLabel[32];
    std::snprintf(maxLabel, sizeof(maxLabel), "%.4g", lane.valueMax);
    std::snprintf(minLabel, sizeof(minLabel), "%.4g", lane.valueMin);
    dl->AddText(ImVec2(canvas.left + 5.0f, top + 2.0f), IM_COL32(205, 205, 210, 220), maxLabel);
    dl->AddText(ImVec2(canvas.left + 5.0f, bottom - ImGui::GetTextLineHeight() - 2.0f), IM_COL32(205, 205, 210, 220), minLabel);

    // Sampled from the evaluator playback uses (without its chance rolls), so
    // editing a control sublane previews what the targets receive.
    drawOverClip(dl, clip, view, canvas.timelineOrigin.x, canvas.left, canvas.right, top, bottom,
                 IM_COL32(track.color.r, track.color.g, track.color.b, context.focused ? 245 : 210),
                 context.focused ? 2.0f : 1.6f,
                 [&](double source) { return ofxOceanodeTimelineLfo::evaluate(lane, source); });
    if(canvas.hovered && canvas.min.x <= ImGui::GetIO().MousePos.x) {
        const double beat = view.pixelsToBeat(ImGui::GetIO().MousePos.x - canvas.timelineOrigin.x);
        if(beat >= clip.startBeat && beat < clip.startBeat + clip.durationBeats) {
            const float normalized = ofxOceanodeTimelineLfo::evaluate(lane, timelineToSourceBeat(clip, beat));
            ImGui::SetTooltip("LFO %.4g\nEdit its control sublanes below.", lane.valueMin + normalized * (lane.valueMax - lane.valueMin));
        }
    }
}

void ofxOceanodeTimelineLfoEditor::drawControls(ofxOceanodeTimelineLaneEditorContext& context,
                                               ofxOceanodeTimelineClipEditorState& editorState,
                                               float contentWidth, double beatPosition,
                                               const std::function<void()>& focusLane) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    const auto& track = context.track;
    auto& clip = context.clip;
    auto& lfoLane = context.lane;
    const auto& view = context.view;
    // Controls are addressed by keys derived from the lane's id (getLane,
    // mute/solo/chance): keep them current whatever copied the lane.
    ofxOceanodeTimelineSyncLfoControlIds(lfoLane);
    if(selectedLaneId != lfoLane.id && hasPointSelection(selectedLaneId) && context.focused) selectedPointIndex = -1;

    const float timelineScrollX = timeline.getViewState().scrollX;
    const double content = sourceDuration(clip);
    const double sourcePerTimeline = 1.0 / std::max(1e-9, effectiveStretch(clip));
    const ImU32 ink = IM_COL32(track.color.r, track.color.g, track.color.b, 235);
    const float indent = kClipEditorIndent + kControlIndent;
    bool anyCanvasHovered = false;

    for(auto& control : lfoLane.lfoControls) {
        const std::string key = ofxOceanodeTimelineLfoControlKey(lfoLane.id, control.lfoParameter);
        const bool focused = context.focused && focusedControlKey == key;
        const bool frequency = control.lfoParameter == "frequency";
        const double sourceAtPlayhead = timelineToSourceBeat(clip, beatPosition);
        const float current = ofxOceanodeTimelineLfo::laneValueFromNormalized(control, controlNormalized(control, sourceAtPlayhead));
        auto focusControl = [&]() {
            focusLane();
            focusedControlKey = key;
        };
        auto drawPlayback = [&]() {
            if(playbackToggle("lfoControlMute" + key, "M", control.muted, "Mute control: it uses its default"))
                timeline.setLaneMuted(sequence, track.id, clip.id, key, !control.muted);
            ImGui::SameLine();
            if(playbackToggle("lfoControlSolo" + key, "S", control.solo, "Solo control among this LFO's controls (the others use their defaults)"))
                timeline.setLaneSolo(sequence, track.id, clip.id, key, !control.solo);
            ImGui::SameLine();
        };

        // ---- folded: one line, with the envelope in miniature ----
        // Folded until expanded (see expandedLfoControlKeys).
        if(editorState.expandedLfoControlKeys.count(key) == 0) {
            ImGui::Dummy(ImVec2(contentWidth, kCollapsedRowHeight));
            const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
            const float zoneLeft = rowMin.x + kLabelWidth;
            const float originX = zoneLeft - timelineScrollX;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(rowMin.x + indent, rowMin.y), ImVec2(zoneLeft, rowMax.y), mutedTrackColor(track.color, 0.16f, 0.28f));
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 5.0f + indent, rowMin.y + 2.0f));
            if(foldButton(("##lfoControlExpand" + key).c_str(), false)) editorState.expandedLfoControlKeys.insert(key);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Expand control");
            ImGui::SameLine();
            drawPlayback();
            char label[96];
            std::snprintf(label, sizeof(label), "%s   %.3g", control.name.c_str(), current);
            const float nameWidth = std::max(1.0f, zoneLeft - 6.0f - ImGui::GetCursorScreenPos().x);
            laneNameSelectable(fitText(label, nameWidth - 8.0f) + "##lfoControlCollapsed" + key, focused, ImVec2(nameWidth, 0));
            if(ImGui::IsItemClicked()) focusControl();
            const float left = std::max(zoneLeft, originX + view.beatToPixels(clip.startBeat));
            const float right = std::min({rowMax.x, dl->GetClipRectMax().x, originX + view.beatToPixels(clip.startBeat + clip.durationBeats)});
            if(right > left) {
                dl->PushClipRect(ImVec2(zoneLeft, rowMin.y), rowMax, true);
                dl->AddRectFilled(ImVec2(left, rowMin.y), ImVec2(right, rowMax.y), mutedTrackColor(track.color, 0.11f, 0.24f));
                drawOverClip(dl, clip, view, originX, left, right, rowMin.y + 4.0f, rowMax.y - 4.0f,
                             IM_COL32(track.color.r, track.color.g, track.color.b, 190), 1.2f,
                             [&](double source) { return controlNormalized(control, source); });
                const float playheadX = originX + view.beatToPixels(beatPosition);
                if(playheadX >= zoneLeft && playheadX <= rowMax.x)
                    dl->AddLine(ImVec2(playheadX, rowMin.y), ImVec2(playheadX, rowMax.y), kPlayhead, 2.0f);
                dl->PopClipRect();
                if(right - left >= 44.0f)
                    laneChanceControl(timeline, sequence, track, clip, control, laneChanceCenter(rowMin, ImVec2(right, rowMax.y)));
            }
            finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
            continue;
        }

        // ---- expanded row: properties pad left, envelope over the clip right ----
        const auto heightIt = editorState.laneEditorHeights.find(key);
        const float height = std::max(laneEditorMinHeight(),
            heightIt != editorState.laneEditorHeights.end() ? heightIt->second : kDefaultControlHeight);
        ImGui::Dummy(ImVec2(contentWidth, height));
        const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
        const float zoneLeft = rowMin.x + kLabelWidth;
        const float originX = zoneLeft - timelineScrollX;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float clipLeft = std::max(zoneLeft, originX + view.beatToPixels(clip.startBeat));
        const float clipRight = std::min({rowMax.x, dl->GetClipRectMax().x, originX + view.beatToPixels(clip.startBeat + clip.durationBeats)});
        const bool chanceVisible = clipRight - clipLeft >= 44.0f;
        const auto chanceCenter = laneChanceCenter(rowMin, ImVec2(clipRight, rowMax.y));
        const bool overChance = chanceVisible && hoverRect(ImVec2(chanceCenter.x - 8, chanceCenter.y - 8),
                                                           ImVec2(chanceCenter.x + 8, chanceCenter.y + 8));
        const float canvasStartX = std::max(zoneLeft, originX);
        ImGui::SetCursorScreenPos(ImVec2(canvasStartX, rowMin.y));
        const ImVec2 canvasSize(std::max(1.0f, rowMax.x - canvasStartX), std::max(1.0f, height - kLaneResizeHandleHeight));
        if(overChance) ImGui::Dummy(canvasSize);
        else ImGui::InvisibleButton(("##lfoControlCanvas" + track.id + clip.id + key).c_str(), canvasSize);
        const bool canvasHovered = !overChance && ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x >= zoneLeft;
        anyCanvasHovered = anyCanvasHovered || canvasHovered;

        dl->AddRectFilled(ImVec2(rowMin.x + indent, rowMin.y), rowMax, mutedTrackColor(track.color, 0.10f, 0.22f));
        dl->AddRectFilled(ImVec2(rowMin.x + indent, rowMin.y), ImVec2(zoneLeft, rowMax.y),
                          mutedTrackColor(track.color, focused ? 0.27f : 0.20f, 0.40f));
        dl->AddLine(ImVec2(zoneLeft - 1.0f, rowMin.y), ImVec2(zoneLeft - 1.0f, rowMax.y),
                    IM_COL32(track.color.r, track.color.g, track.color.b, focused ? 210 : 120));
        // Bottom resize strip, tracked from its rectangle like a lane's.
        {
            const ImVec2 handleMin(rowMin.x + indent, rowMax.y - kLaneResizeHandleHeight), handleMax(rowMax.x, rowMax.y);
            const bool resizeHovered = hoverRect(handleMin, handleMax);
            if(resizeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                editorState.resizingLaneId = key;
                editorState.laneResizeStartMouseY = ImGui::GetIO().MousePos.y;
                editorState.laneResizeStartHeight = height;
            }
            const bool resizeActive = editorState.resizingLaneId == key && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if(resizeHovered || resizeActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(resizeActive)
                editorState.laneEditorHeights[key] = ofClamp(editorState.laneResizeStartHeight + ImGui::GetIO().MousePos.y -
                                                             editorState.laneResizeStartMouseY, laneEditorMinHeight(), kLaneEditorMaxHeight);
            if(editorState.resizingLaneId == key && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) editorState.resizingLaneId.clear();
            const float separatorY = rowMax.y - kLaneResizeHandleHeight * 0.5f;
            dl->AddLine(ImVec2(handleMin.x, separatorY), ImVec2(handleMax.x, separatorY),
                        resizeActive ? IM_COL32(205, 205, 215, 235) : resizeHovered ? IM_COL32(160, 160, 170, 205) : IM_COL32(80, 80, 86, 110),
                        resizeActive ? 2.0f : 1.0f);
        }

        // Properties: fold, M/S, name, then the control's mode and value.
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 5.0f + indent, rowMin.y + 4.0f));
        if(compactLaneHeight(height)) ImGui::SetNextWindowScroll(ImVec2(0, 0));
        ImGui::BeginChild(("##lfoControlProperties" + track.id + clip.id + key).c_str(),
                          ImVec2(kLabelWidth - 10.0f - indent, std::max(1.0f, height - 8.0f - kLaneResizeHandleHeight)), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if(foldButton(("##lfoControlCollapse" + key).c_str(), true)) editorState.expandedLfoControlKeys.erase(key);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse control");
        ImGui::SameLine();
        drawPlayback();
        {
            const float nameWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            if(laneNameSelectable(fitText(control.name, nameWidth - 8.0f) + "##lfoControlHeader" + key, focused, ImVec2(nameWidth, 0)))
                focusControl();
        }
        int mode = control.curveInterpolation == "Value" ? 0 : 1 + static_cast<int>(curveInterpolationMode(control.curveInterpolation));
        const int previousMode = mode;
        propertyLabel("Mode");
        if(ImGui::Combo(("##lfoControlMode" + key).c_str(), &mode, kLfoInterpolationNames, 5) && mode != previousMode) {
            if(mode == 0) {
                // Value mode starts from what the control holds at the playhead.
                control.lfoValue = controlNormalized(control, sourceAtPlayhead);
            } else {
                if(previousMode != 0) resetCurveTensions(control);
                // Back from Value mode: the stored points again, or the constant it held.
                if(control.curvePoints.empty()) {
                    control.curvePoints.push_back({0.0, control.lfoValue});
                    resetCurveTensions(control);
                }
            }
            control.curveInterpolation = kLfoInterpolationNames[mode];
            tensionSegment = -1;
            if(selectedControlKey == key) selectedPointIndex = -1;
        }
        const float lo = std::min(control.valueMin, control.valueMax), hi = std::max(control.valueMin, control.valueMax);
        if(control.curveInterpolation == "Value") {
            float realValue = ofxOceanodeTimelineLfo::laneValueFromNormalized(control, control.lfoValue);
            propertyLabel("Value");
            if(ImGui::DragFloat(("##lfoControlValue" + key).c_str(), &realValue,
                                frequency ? std::max(1e-3f, std::abs(realValue) * 0.01f) // proportional: fine at short cycles
                                          : std::max(1e-4f, std::abs(control.valueMax - control.valueMin) / 300.0f),
                                lo, hi, "%.4g"))
                control.lfoValue = ofxOceanodeTimelineLfo::laneNormalizedFromValue(control, ofClamp(realValue, lo, hi));
        } else {
            const auto shape = curveInterpolationMode(control.curveInterpolation);
            if(shape == CurveInterpolationMode::LogExp) ImGui::TextDisabled("Alt-drag vertically");
            else if(shape == CurveInterpolationMode::Sigmoid) ImGui::TextDisabled("Alt-drag freely");
            ImGui::TextDisabled("Now %.4g", current);
        }
        if(frequency) {
            ImGui::Checkbox(("Snap##lfoControlSnap" + key).c_str(), &control.lfoSnap);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Snap edits to the musical lengths shown in the lane");
            ImGui::SameLine();
            const char* snapModes[] = {"Straight", "Triplet", "Dotted"};
            int snapMode = std::max(0, std::min(2, control.lfoSnapMode));
            ImGui::SetNextItemWidth(-FLT_MIN);
            // Only new edits use the new grid; existing points keep their values.
            if(ImGui::Combo(("##lfoControlSnapMode" + key).c_str(), &snapMode, snapModes, 3)) control.lfoSnapMode = snapMode;
        }
        ImGui::EndChild();
        finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));

        // ---- envelope, over the clip as it plays ----
        const float graphTop = rowMin.y + 6.0f, graphBottom = rowMax.y - 6.0f - kLaneResizeHandleHeight;
        const float valueSpan = std::max(1.0f, graphBottom - graphTop);
        auto yForValue = [&](float normalized) { return graphBottom - normalized * valueSpan; };
        auto normalizedAtY = [&](float y) {
            float normalized = ofClamp((graphBottom - y) / valueSpan, 0.0f, 1.0f);
            if(frequency && control.lfoSnap)
                normalized = ofxOceanodeTimelineLfo::laneNormalizedFromValue(control,
                    ofxOceanodeTimelineLfo::snapFrequencyValue(ofxOceanodeTimelineLfo::laneValueFromNormalized(control, normalized), control.lfoSnapMode));
            else if(!frequency && curveSnapLevels(control) >= 2)
                normalized = std::round(normalized * control.valueQuantizeSteps) / static_cast<float>(control.valueQuantizeSteps);
            return normalized;
        };
        const bool valueMode = control.curveInterpolation == "Value";
        if(!valueMode) control.curveTensions.resize(control.curvePoints.empty() ? 0 : control.curvePoints.size() - 1);
        const auto interpolation = curveInterpolationMode(control.curveInterpolation);
        if(clipRight <= clipLeft) {
            if(chanceVisible) laneChanceControl(timeline, sequence, track, clip, control, chanceCenter);
            finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
            continue;
        }
        dl->PushClipRect(ImVec2(zoneLeft, rowMin.y), ImVec2(rowMax.x, rowMax.y - kLaneResizeHandleHeight * 0.5f), true);
        dl->AddLine(ImVec2(clipLeft, graphBottom), ImVec2(clipRight, graphBottom), IM_COL32(95, 100, 115, 110));
        const double visibleFrom = view.pixelsToBeat(clipLeft - originX), visibleTo = view.pixelsToBeat(clipRight - originX);
        int firstCycle = 0, lastCycle = 0;
        visibleCycles(clip, visibleFrom, visibleTo, firstCycle, lastCycle);
        for(int cycle = std::max(1, firstCycle); cycle <= lastCycle; ++cycle) {
            const float x = originX + view.beatToPixels(clip.startBeat + cycle * cycleDuration(clip));
            dl->AddLine(ImVec2(x, graphTop), ImVec2(x, graphBottom), IM_COL32(track.color.r, track.color.g, track.color.b, 160), 1.5f);
        }
        if(frequency) {
            // Reference lines at musical cycle lengths (log-spaced, so evenly
            // spread): the chosen grid labelled, the straight one faint below.
            const int gridMode = std::max(0, std::min(2, control.lfoSnapMode));
            if(gridMode != 0)
                for(float gridValue : ofxOceanodeTimelineLfo::frequencyGridValues(0)) {
                    const float y = yForValue(ofxOceanodeTimelineLfo::laneNormalizedFromValue(control, gridValue));
                    dl->AddLine(ImVec2(clipLeft, y), ImVec2(clipRight, y), IM_COL32(80, 84, 98, 55));
                }
            for(float gridValue : ofxOceanodeTimelineLfo::frequencyGridValues(gridMode)) {
                const float y = yForValue(ofxOceanodeTimelineLfo::laneNormalizedFromValue(control, gridValue));
                const bool oneBeat = gridMode == 0 && gridValue == 1.0f;
                dl->AddLine(ImVec2(clipLeft, y), ImVec2(clipRight, y), oneBeat ? IM_COL32(150, 150, 165, 120) : IM_COL32(95, 100, 115, 85));
                dl->AddText(ImVec2(clipLeft + 3.0f, y - ImGui::GetTextLineHeight() + 1.0f), IM_COL32(160, 165, 180, 150),
                            ofxOceanodeTimelineLfo::frequencyGridLabel(gridValue, gridMode).c_str());
            }
        } else if(curveSnapLevels(control) >= 2) {
            for(int level = 0; level <= control.valueQuantizeSteps; ++level) {
                const float y = yForValue(level / static_cast<float>(control.valueQuantizeSteps));
                dl->AddLine(ImVec2(clipLeft, y), ImVec2(clipRight, y), IM_COL32(150, 140, 80, 110));
            }
        }
        drawOverClip(dl, clip, view, originX, clipLeft, clipRight, graphTop, graphBottom, ink, focused ? 2.0f : 1.5f,
                     [&](double source) { return controlNormalized(control, source); });
        const bool controlSelected = !valueMode && selectedLaneId == lfoLane.id && selectedControlKey == key &&
            selectedPointIndex >= 0 && selectedPointIndex < static_cast<int>(control.curvePoints.size());
        auto pointX = [&](size_t index, int cycle) {
            return originX + view.beatToPixels(sourceToTimelineBeat(clip, control.curvePoints[index].beat, cycle));
        };
        if(!valueMode)
            for(int cycle = firstCycle; cycle <= lastCycle; ++cycle)
                for(size_t i = 0; i < control.curvePoints.size(); ++i) {
                    const ImVec2 p(pointX(i, cycle), yForValue(control.curvePoints[i].value));
                    if(p.x < clipLeft - 6.0f || p.x > clipRight + 6.0f) continue;
                    dl->AddCircleFilled(p, focused ? 4.0f : 3.0f, focused ? IM_COL32(245, 235, 150, cycle == firstCycle ? 255 : 170) : ink);
                    if(controlSelected && static_cast<int>(i) == selectedPointIndex)
                        dl->AddCircle(p, 7.0f, IM_COL32(255, 255, 255, 240), 0, 2.0f);
                }
        const float playheadX = originX + view.beatToPixels(beatPosition);
        if(playheadX >= zoneLeft && playheadX <= rowMax.x)
            dl->AddLine(ImVec2(playheadX, rowMin.y), ImVec2(playheadX, rowMax.y - kLaneResizeHandleHeight), kPlayhead, 2.0f);
        dl->PopClipRect();

        // ---- editing ----
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double mouseBeat = view.pixelsToBeat(mouse.x - originX);
        const bool mouseInClip = mouseBeat >= clip.startBeat && mouseBeat < clip.startBeat + clip.durationBeats;
        const int mouseCycle = static_cast<int>(cycleIndex(clip, mouseBeat));
        if(valueMode) {
            // Click or drag anywhere in the row: the constant follows the mouse.
            if(canvasHovered && mouseInClip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                focusControl();
                dragControlKey = key;
                dragPointIndex = -1;
                tensionSegment = -1;
                valueDragActive = true;
            }
            if(valueDragActive && dragControlKey == key && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                control.lfoValue = normalizedAtY(mouse.y);
            if(chanceVisible) laneChanceControl(timeline, sequence, track, clip, control, chanceCenter);
            finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
            continue;
        }
        auto hitPoint = [&]() {
            int hit = -1;
            float nearest = 9.0f;
            for(int i = static_cast<int>(control.curvePoints.size()) - 1; i >= 0; --i) {
                const float distance = std::hypot(mouse.x - pointX(static_cast<size_t>(i), mouseCycle),
                                                  mouse.y - yForValue(control.curvePoints[i].value));
                if(distance < nearest) { nearest = distance; hit = i; }
            }
            return hit;
        };
        auto selectPoint = [&](int index) {
            selectedLaneId = lfoLane.id;
            selectedControlKey = key;
            selectedPointIndex = index;
        };
        auto deletePoint = [&](int index) {
            // A control keeps at least one point, so it still defines its value.
            if(index < 0 || index >= static_cast<int>(control.curvePoints.size()) || control.curvePoints.size() <= 1) return;
            eraseCurvePointKeepingTensions(control, static_cast<size_t>(index));
            if(selectedControlKey == key) selectedPointIndex = -1;
            dragPointIndex = -1;
            tensionSegment = -1;
        };
        const std::string pointPopupId = "LFO control point##" + key;
        if(canvasHovered && mouseInClip && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const int hit = hitPoint();
            if(hit >= 0) {
                focusControl();
                selectPoint(hit);
                pointNumericValue = ofxOceanodeTimelineLfo::laneValueFromNormalized(control, control.curvePoints[hit].value);
                ImGui::OpenPopup(pointPopupId.c_str());
            }
        }
        if(canvasHovered && mouseInClip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            focusControl();
            dragControlKey = key;
            dragPointIndex = hitPoint();
            tensionSegment = -1;
            const bool alt = ImGui::GetIO().KeyAlt;
            const double source = ofClamp(timelineToSourceBeat(clip, view.snapBeat(mouseBeat)), 0.0, content);
            if(dragPointIndex >= 0 && ImGui::GetIO().KeyShift) {
                deletePoint(dragPointIndex);
            } else if(dragPointIndex >= 0) {
                selectPoint(dragPointIndex);
                dragStartMouseBeat = mouseBeat;
                dragStartSourceBeat = control.curvePoints[dragPointIndex].beat;
            } else if(alt && (interpolation == CurveInterpolationMode::LogExp || interpolation == CurveInterpolationMode::Sigmoid) &&
                      control.curvePoints.size() > 1) {
                // Alt-click near a segment grabs its bend (as in a Curve lane).
                const double at = timelineToSourceBeat(clip, mouseBeat);
                for(size_t i = 1; i < control.curvePoints.size(); ++i) {
                    const auto& a = control.curvePoints[i - 1];
                    const auto& b = control.curvePoints[i];
                    if(at < a.beat || at > b.beat) continue;
                    const float t = static_cast<float>((at - a.beat) / std::max(1e-9, b.beat - a.beat));
                    const auto tension = control.curveTensions[i - 1];
                    const float segmentY = yForValue(ofLerp(a.value, b.value, curveSegmentShape(t, interpolation, tension)));
                    if(std::abs(mouse.y - segmentY) <= 12.0f) {
                        tensionSegment = static_cast<int>(i - 1);
                        tensionDragStartX = mouse.x;
                        tensionDragStartY = mouse.y;
                        tensionStartInflection = tension.inflection;
                        tensionStartSteepness = tension.steepness;
                    }
                    break;
                }
            } else if(!alt) {
                dragPointIndex = insertCurvePoint(control, {source, normalizedAtY(mouse.y)});
                selectPoint(dragPointIndex);
                dragStartMouseBeat = mouseBeat;
                dragStartSourceBeat = source;
            }
        }
        if(dragControlKey == key && tensionSegment >= 0 && tensionSegment < static_cast<int>(control.curveTensions.size()) &&
           ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto& tension = control.curveTensions[tensionSegment];
            if(interpolation == CurveInterpolationMode::Sigmoid)
                tension.inflection = ofClamp(tensionStartInflection + (mouse.x - tensionDragStartX) /
                                             std::max(1.0f, clipRight - clipLeft), 0.01f, 0.99f);
            else tension.inflection = 0.5f;
            const float steepnessDelta = -(mouse.y - tensionDragStartY) / std::max(1.0f, valueSpan / 3.0f);
            tension.steepness = ofClamp(tensionStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
        }
        if(dragControlKey == key && dragPointIndex >= 0 && dragPointIndex < static_cast<int>(control.curvePoints.size()) &&
           tensionSegment < 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Moved by the mouse's travel since the press, in source beats:
            // the point stays in its own repeat, between its neighbours.
            const double moved = (view.snapBeat(mouseBeat) - view.snapBeat(dragStartMouseBeat)) * sourcePerTimeline;
            const double minimum = dragPointIndex > 0 ? control.curvePoints[dragPointIndex - 1].beat + 1.0 / kPPQ : 0.0;
            const double maximum = dragPointIndex + 1 < static_cast<int>(control.curvePoints.size())
                ? control.curvePoints[dragPointIndex + 1].beat - 1.0 / kPPQ : content;
            control.curvePoints[dragPointIndex].beat = std::max(minimum, std::min(maximum, dragStartSourceBeat + moved));
            control.curvePoints[dragPointIndex].value = normalizedAtY(mouse.y);
        }
        // Delete / Backspace removes the selected point (timeline focused, nothing being typed).
        if(controlSelected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
           (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
            deletePoint(selectedPointIndex);
        if(ImGui::BeginPopup(pointPopupId.c_str())) {
            const bool valid = selectedControlKey == key && selectedPointIndex >= 0 &&
                selectedPointIndex < static_cast<int>(control.curvePoints.size());
            if(!valid) {
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::TextUnformatted(control.name.c_str());
                contextMenuSection("Value");
                // Real units, e.g. beats for Frequency.
                ImGui::SetNextItemWidth(110.0f);
                if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if(ImGui::InputFloat("Value", &pointNumericValue, 0.0f, 0.0f, "%.6g"))
                    control.curvePoints[selectedPointIndex].value =
                        ofxOceanodeTimelineLfo::laneNormalizedFromValue(control, ofClamp(pointNumericValue, lo, hi));
                if(ImGui::IsItemDeactivatedAfterEdit()) pointNumericValue = ofClamp(pointNumericValue, lo, hi);
                contextMenuSection("Remove");
                // Decided once: deleting changes the point count before EndDisabled.
                const bool deleteDisabled = control.curvePoints.size() <= 1;
                if(deleteDisabled) ImGui::BeginDisabled();
                if(ImGui::MenuItem("Delete point")) deletePoint(selectedPointIndex);
                if(deleteDisabled) {
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("A control keeps at least one point");
                }
            }
            ImGui::EndPopup();
        }
        if(chanceVisible) laneChanceControl(timeline, sequence, track, clip, control, chanceCenter);
        finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
    }

    // A left click outside these controls (a clip, another lane) drops the
    // point selection, so Delete acts on the timeline selection again.
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !anyCanvasHovered && selectedLaneId == lfoLane.id &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        selectedPointIndex = -1;
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragControlKey.empty()) {
        if(auto* control = timeline.getLane(sequence, track.id, clip.id, dragControlKey)) {
            std::stable_sort(control->curvePoints.begin(), control->curvePoints.end(),
                             [](const auto& a, const auto& b) { return a.beat < b.beat; });
        }
        dragControlKey.clear();
        dragPointIndex = -1;
        tensionSegment = -1;
        valueDragActive = false;
    }
}
