#include "ofxOceanodeTimelineWaveVolumeEditor.h"
#include "ofxOceanodeTimelineEditorView.h"
#include "ofxOceanodeTimelineEditorUi.h"
#include "Timeline/ofxOceanodeTimeline.h"

#include <algorithm>
#include <cmath>

using namespace ofxOceanodeTimelineEditorUi;

void ofxOceanodeTimelineWaveVolumeEditor::draw(
    ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence, ofxOceanodeTimelineTrack& track,
    const ofxOceanodeTimelineEditorView& view, float width, float height,
    double beatPosition, float timelineOriginX) {
    const double endBeat = view.endBeat;
    ImGui::TextUnformatted("Track volume");
    ImGui::SameLine();
    // Both of these are read by evaluateWaveTrackVolume and persisted, and
    // neither had any way to be set: the flag was forced on here every frame
    // and the shape was stuck at whatever a preset happened to hold.
    ImGui::Checkbox("Automate", &track.waveVolumeAutomationEnabled);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Off plays the track at its gain alone; the envelope stays as you left it");
    ImGui::SameLine();
    ImGui::Checkbox("Waveform follows volume", &track.waveVolumeShapesWaveform);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the clips' waveforms through this volume too (their gain and fades always show)");
    const float controlWidth = std::min(96.0f, std::max(58.0f, width * 0.22f));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("Gain", &track.waveVolume, 0.01f, 0.0f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(controlWidth);
    if(ImGui::BeginCombo("Shape", track.waveVolumeInterpolation.c_str())) {
        // A sigmoid segment left neutral is a straight line.
        for(const char* option : {"Sigmoid", "Log / Exp", "Step"}) {
            if(ImGui::Selectable(option, track.waveVolumeInterpolation == option))
                track.waveVolumeInterpolation = option;
        }
        ImGui::EndCombo();
    }
    // Through the manager's accessor rather than the member, so every path
    // that touches these points goes through one door.
    auto& points = timeline.getWaveTrackVolumePoints(sequence, track.id);
    if(points.empty()) {
        const double automationEnd = std::max(1.0, endBeat);
        points = {{0.0, track.waveVolume}, {automationEnd, track.waveVolume}};
        track.waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
    }

    const float graphHeight = std::max(80.0f, height - 58.0f);
    ImGui::InvisibleButton("##waveTrackVolumeCanvas",
                           ImVec2(std::max(40.0f, width - 8.0f), graphHeight));
    const ImVec2 graphMin = ImGui::GetItemRectMin();
    const ImVec2 graphMax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(graphMin, graphMax, IM_COL32(22, 24, 30, 220), 3.0f);
    dl->AddRect(graphMin, graphMax, IM_COL32(track.color.r, track.color.g, track.color.b, 180), 3.0f);

    auto xForBeat = [&](double beat) {
        return timelineOriginX + view.beatToPixels(beat);
    };
    auto yForValue = [&](float value) {
        return graphMax.y - 5.0f - ofClamp(value, 0.0f, 4.0f) / 4.0f * (graphMax.y - graphMin.y - 10.0f);
    };
    auto beatForX = [&](float x) {
        return view.pixelsToBeat(x - timelineOriginX);
    };

    // Match the timeline's vertical beat/bar grid in the automation panel so
    // envelope points can be read against the same musical positions as the
    // clips above.
    const double grid = view.displayGridBeats();
    const int gridLines = static_cast<int>(std::ceil(endBeat / grid));
    for(int i = 0; i <= gridLines; ++i) {
        const double beat = i * grid;
        const float x = xForBeat(beat);
        if(x < graphMin.x || x > graphMax.x) continue;
        const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
        const bool quarter = std::fmod(beat, 1.0) < 0.001;
        dl->AddLine(ImVec2(x, graphMin.y), ImVec2(x, graphMax.y),
                    bar ? IM_COL32(150, 155, 190, 125)
                        : quarter ? IM_COL32(105, 110, 135, 100)
                                  : IM_COL32(75, 80, 100, 75),
                    bar ? 1.5f : 1.0f);
    }

    for(int level = 0; level <= 4; ++level) {
        const float y = yForValue(static_cast<float>(level));
        dl->AddLine(ImVec2(graphMin.x, y), ImVec2(graphMax.x, y),
                    level == 1 ? IM_COL32(190, 190, 205, 120) : IM_COL32(90, 95, 110, 90));
        if(level < 4)
            dl->AddText(ImVec2(graphMin.x + 5.0f, y - 14.0f), IM_COL32(150, 155, 170, 190),
                        ofToString(level).c_str());
    }
    const float playheadX = xForBeat(beatPosition);
    if(playheadX >= graphMin.x && playheadX <= graphMax.x)
        dl->AddLine(ImVec2(playheadX, graphMin.y), ImVec2(playheadX, graphMax.y), playheadColor, 1.5f);

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    auto& tensions = track.waveVolumeTensions;
    tensions.resize(points.size() > 1 ? points.size() - 1 : 0);
    // The curve as it plays: every segment through its shape and bend.
    const auto mode = ofxOceanodeTimelineCurve::curveInterpolationMode(track.waveVolumeInterpolation);
    const bool bends = mode == ofxOceanodeTimelineCurve::CurveInterpolationMode::Sigmoid ||
                       mode == ofxOceanodeTimelineCurve::CurveInterpolationMode::LogExp;
    const bool sigmoid = mode == ofxOceanodeTimelineCurve::CurveInterpolationMode::Sigmoid;
    {
        static std::vector<ImVec2> curve;
        curve.clear();
        const float from = std::max(graphMin.x, xForBeat(points.front().beat)), to = std::min(graphMax.x, xForBeat(points.back().beat));
        for(float x = from; x < to + 2.0f; x += 2.0f) {
            const float px = std::min(x, to);
            curve.push_back(ImVec2(px, yForValue(ofxOceanodeTimelineCurve::valueAtBeat(points, tensions, track.waveVolumeInterpolation,
                                                                                     beatForX(px), track.waveVolume))));
        }
        dl->PushClipRect(graphMin, graphMax, true);
        if(curve.size() > 1) dl->AddPolyline(curve.data(), static_cast<int>(curve.size()), IM_COL32(255, 220, 90, 235), ImDrawFlags_None, 2.0f);
        dl->PopClipRect();
    }
    // A handle on each segment wide enough for one, where it bends.
    constexpr float kBendMinWidth = 24.0f, kBendPick = 7.0f;
    auto bendPoint = [&](size_t segment) {
        const auto& a = points[segment];
        const auto& b = points[segment + 1];
        const float t = sigmoid ? ofClamp(tensions[segment].inflection, 0.01f, 0.99f) : 0.5f;
        const float xa = xForBeat(a.beat), xb = xForBeat(b.beat);
        return ImVec2(xa + (xb - xa) * t,
                      yForValue(ofLerp(a.value, b.value, ofxOceanodeTimelineCurve::curveSegmentShape(t, mode, tensions[segment]))));
    };
    auto bendShown = [&](size_t segment) {
        return bends && xForBeat(points[segment + 1].beat) - xForBeat(points[segment].beat) >= kBendMinWidth;
    };
    int hoveredBend = -1;
    const bool bendDragging = waveVolumeBendTrackId == track.id && waveVolumeBendSegment >= 0;
    if(bendDragging) hoveredBend = waveVolumeBendSegment;
    else if(hovered && waveVolumeDragPointIndex < 0) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for(size_t i = 0; i + 1 < points.size(); ++i) {
            if(!bendShown(i)) continue;
            const ImVec2 at = bendPoint(i);
            if(std::abs(mouse.x - at.x) <= kBendPick && std::abs(mouse.y - at.y) <= kBendPick) hoveredBend = static_cast<int>(i);
        }
    }
    for(size_t i = 0; i + 1 < points.size(); ++i) {
        if(!bendShown(i)) continue;
        const ImVec2 at = bendPoint(i);
        if(at.x < graphMin.x - 6.0f || at.x > graphMax.x + 6.0f) continue;
        const bool lit = hoveredBend == static_cast<int>(i);
        dl->AddCircleFilled(at, 3.5f, IM_COL32(30, 32, 40, 255));
        dl->AddCircle(at, 3.5f, IM_COL32(255, 220, 90, lit ? 255 : 150), 12, lit ? 2.0f : 1.2f);
    }
    for(const auto& point : points) {
        const float x = xForBeat(point.beat);
        if(x < graphMin.x - 6.0f || x > graphMax.x + 6.0f) continue;
        dl->AddCircleFilled(ImVec2(x, yForValue(point.value)), 4.0f, IM_COL32(255, 220, 90, 255));
    }
    if(hoveredBend >= 0 && !bendDragging && hoveredBend < static_cast<int>(tensions.size())) {
        const auto& tension = tensions[hoveredBend];
        if(sigmoid) {
            ImGui::SetTooltip("Bend at %d%%, steepness %.2f\nDrag sideways: where it bends. Up / down: how sharply.\nDouble-click: a straight line.",
                              static_cast<int>(std::round(tension.inflection * 100.0f)), tension.steepness);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            ImGui::SetTooltip("Steepness %.2f\nDrag up / down: how it bends.\nDouble-click: a straight line.", tension.steepness);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    }
    if(bendDragging) {
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left) && waveVolumeBendSegment < static_cast<int>(tensions.size())) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            auto& tension = tensions[waveVolumeBendSegment];
            if(sigmoid) tension.inflection = ofClamp(waveVolumeBendStartInflection + (mouse.x - waveVolumeBendStartX) / waveVolumeBendWidth, 0.01f, 0.99f);
            // Toward the segment's higher end sharpens it, whichever way it runs.
            const float rising = points[waveVolumeBendSegment + 1].value >= points[waveVolumeBendSegment].value ? 1.0f : -1.0f;
            const float steepnessDelta = -(mouse.y - waveVolumeBendStartY) / std::max(1.0f, (graphMax.y - graphMin.y) / 3.0f) * (sigmoid ? 1.0f : -rising);
            tension.steepness = ofClamp(waveVolumeBendStartSteepness * std::exp(steepnessDelta * 0.5f), 0.1f, 10.0f);
            ImGui::SetMouseCursor(sigmoid ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeNS);
        } else {
            waveVolumeBendSegment = -1;
            waveVolumeBendTrackId.clear();
        }
    }

    if(hovered && hoveredBend >= 0 && !bendDragging) {
        // A bend handle takes the click: a drag bends, a double-click straightens.
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            tensions[hoveredBend] = ofxOceanodeTimelineCurveTension{};
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            waveVolumeBendSegment = hoveredBend;
            waveVolumeBendTrackId = track.id;
            waveVolumeBendStartX = mouse.x;
            waveVolumeBendStartY = mouse.y;
            waveVolumeBendWidth = std::max(1.0f, xForBeat(points[hoveredBend + 1].beat) - xForBeat(points[hoveredBend].beat));
            waveVolumeBendStartInflection = tensions[hoveredBend].inflection;
            waveVolumeBendStartSteepness = tensions[hoveredBend].steepness;
        }
    } else if(hovered && !bendDragging) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int nearest = -1;
        float nearestDistance = 9.0f;
        for(size_t i = 0; i < points.size(); ++i) {
            const float distance = std::abs(mouse.x - xForBeat(points[i].beat));
            if(distance < nearestDistance) {
                nearest = static_cast<int>(i);
                nearestDistance = distance;
            }
        }
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const double beat = view.snapBeat(ofClamp(beatForX(mouse.x), 0.0, endBeat));
            const float value = ofClamp(4.0f * (graphMax.y - 5.0f - mouse.y) /
                                        std::max(1.0f, graphMax.y - graphMin.y - 10.0f), 0.0f, 4.0f);
            timeline.addWaveTrackVolumePoint(sequence, track.id, beat, value);
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearest >= 0) {
            // Right-click: set an exact value or delete (was: delete immediately).
            waveVolumeValuePointIndex = nearest;
            waveVolumeNumericValue = points[nearest].value;
            ImGui::OpenPopup("Volume point value");
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && nearest >= 0) {
            waveVolumeDragPointIndex = nearest;
            waveVolumeDragTrackId = track.id;
        }
    }
    if(waveVolumeDragTrackId == track.id && waveVolumeDragPointIndex >= 0 && waveVolumeDragPointIndex < static_cast<int>(points.size())) {
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            points[waveVolumeDragPointIndex].beat = view.snapBeat(ofClamp(beatForX(mouse.x), 0.0, endBeat));
            points[waveVolumeDragPointIndex].value = ofClamp(4.0f * (graphMax.y - 5.0f - mouse.y) /
                                                               std::max(1.0f, graphMax.y - graphMin.y - 10.0f),
                                                               0.0f, 4.0f);
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
            track.waveVolumeTensions.resize(points.size() > 1 ? points.size() - 1 : 0);
            waveVolumeDragPointIndex = -1;
        }
    }
    if(ImGui::BeginPopup("Volume point value")) {
        if(waveVolumeValuePointIndex >= 0 && waveVolumeValuePointIndex < static_cast<int>(points.size())) {
            ImGui::TextUnformatted("Volume point");
            contextMenuSection("Value");
            ImGui::SetNextItemWidth(110.0f);
            if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if(ImGui::InputFloat("Value", &waveVolumeNumericValue, 0.0f, 0.0f, "%.4g"))
                points[waveVolumeValuePointIndex].value = ofClamp(waveVolumeNumericValue, 0.0f, 4.0f);
            if(ImGui::IsItemDeactivatedAfterEdit()) waveVolumeNumericValue = ofClamp(waveVolumeNumericValue, 0.0f, 4.0f);
            contextMenuSection("Remove");
            // Decide once: deleting changes the point count before EndDisabled.
            const bool deleteDisabled = points.size() <= 2;
            if(deleteDisabled) ImGui::BeginDisabled();
            if(ImGui::MenuItem("Delete point")) {
                timeline.removeWaveTrackVolumePoint(sequence, track.id, points[waveVolumeValuePointIndex].beat);
                waveVolumeValuePointIndex = -1;
            }
            if(deleteDisabled) ImGui::EndDisabled();
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

