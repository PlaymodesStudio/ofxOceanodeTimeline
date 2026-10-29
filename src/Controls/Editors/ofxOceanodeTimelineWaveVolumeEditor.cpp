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
    const float controlWidth = std::min(96.0f, std::max(58.0f, width * 0.22f));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("Gain", &track.waveVolume, 0.01f, 0.0f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(controlWidth);
    if(ImGui::BeginCombo("Shape", track.waveVolumeInterpolation.c_str())) {
        for(const char* option : {"Step", "Linear", "Log / Exp", "Sigmoid"}) {
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
    for(size_t i = 1; i < points.size(); ++i) {
        dl->AddLine(ImVec2(xForBeat(points[i - 1].beat), yForValue(points[i - 1].value)),
                    ImVec2(xForBeat(points[i].beat), yForValue(points[i].value)),
                    IM_COL32(255, 220, 90, 235), 2.0f);
    }
    for(const auto& point : points) {
        const float x = xForBeat(point.beat);
        if(x < graphMin.x - 6.0f || x > graphMax.x + 6.0f) continue;
        dl->AddCircleFilled(ImVec2(x, yForValue(point.value)), 4.0f, IM_COL32(255, 220, 90, 255));
    }

    if(hovered) {
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
        }
    }
    if(waveVolumeDragPointIndex >= 0 && waveVolumeDragPointIndex < static_cast<int>(points.size())) {
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

