#pragma once
#include "Timeline/ofxOceanodeTimeline.h"
#include "ofxOceanodeTimelineEditorView.h"
#include "imgui.h"
#include <set>
#include <cmath>

// User-facing vocabulary and shared timing controls. Both arrangement menus
// and editors use this panel, so a numeric length always means the same thing.
namespace ofxOceanodeTimelinePresentation {
inline const char* laneKind(const ofxOceanodeTimelineLane& lane) {
    switch(lane.type) {
        case ofxOceanodeTimelineLaneType::Step: return "Step Sequencer";
        case ofxOceanodeTimelineLaneType::Curve: return "Curve";
        case ofxOceanodeTimelineLaneType::PianoRoll: return "Piano Roll";
        case ofxOceanodeTimelineLaneType::MultiValue: return "Multi Value";
        case ofxOceanodeTimelineLaneType::MultiSlider: return "Multi Slider";
        case ofxOceanodeTimelineLaneType::MultiGate: return lane.multiRowCount <= 1 ? "Toggle" : "Multi Gate";
        case ofxOceanodeTimelineLaneType::Number: return "Number";
        case ofxOceanodeTimelineLaneType::Dropdown: return "Dropdown";
        default: return "Audio";
    }
}
inline size_t targetCount(const ofxOceanodeTimelineClip& clip) {
    std::set<std::string> ids;
    if(clip.isLfo && !clip.lfoOutputBindingId.empty()) ids.insert(clip.lfoOutputBindingId);
    else for(const auto& lane : clip.lanes) ids.insert(lane.bindingIds.begin(), lane.bindingIds.end());
    return ids.size();
}
inline std::string clipSummary(const ofxOceanodeTimelineClip& clip, bool audio = false) {
    if(audio || !clip.waveFilePath.empty()) return clip.waveFilePath.empty() ? "Audio / No file" : "Audio";
    if(clip.isLfo) return clip.lfoOutputBindingId.empty() ? "LFO / No target" : "LFO / 1 target";
    if(clip.lanes.empty()) return "Empty clip";
    const std::string content = clip.lanes.size() == 1 ? laneKind(clip.lanes.front())
        : ofToString(clip.lanes.size()) + " lanes";
    const size_t count = targetCount(clip);
    return content + " / " + (count == 0 ? "No target" : ofToString(count) + (count == 1 ? " target" : " targets"));
}
inline const char* endEditName(ofxOceanodeTimelineEndEdit operation) {
    switch(operation) {
        case ofxOceanodeTimelineEndEdit::Trim: return "Trim / extend";
        case ofxOceanodeTimelineEndEdit::Stretch: return "Stretch";
        default: return "Repeat";
    }
}
inline bool drawClipTiming(ofxOceanodeTimelineManager& timeline,
                           const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
                           const ofxOceanodeTimelineEditorView& view,
                           ofxOceanodeTimelineEndEdit operation, bool tableLayout = true) {
    ImGui::PushID(clip.id.c_str());
    bool changed = false;
    const int columns = tableLayout && ImGui::GetContentRegionAvail().x >= 650.0f ? 4 : 2;
    const bool table = ImGui::BeginTable("##sharedClipTiming", columns, ImGuiTableFlags_SizingStretchSame);
    auto field = [&](const char* label) {
        if(table) ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1.0f);
    };
    field("Start (beats)");
    double start = clip.startBeat;
    if(ImGui::InputDouble("##start", &start, view.editIncrement(), 1, "%.3f") && std::isfinite(start)) {
        changed = timeline.setClipTiming(track.id, clip.id, view.snapBeat(start), clip.durationBeats) || changed;
    }
    field("Timeline length (beats)");
    double length = clip.durationBeats;
    if(ImGui::InputDouble("##length", &length, view.editIncrement(), 1, "%.3f"))
        changed = timeline.resizeClipEnd(track.id, clip.id, view.snapBeat(length), operation) || changed;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Uses the right-edge tool: %s. Applies to every lane in this clip.",
        track.isWaveTrack ? "Stretch (audio playback speed changes)" : endEditName(operation));
    field("Source length (beats)");
    double source = ofxOceanodeTimelineClipTime::sourceDuration(clip);
    ImGui::BeginDisabled(track.isWaveTrack);
    if(ImGui::InputDouble("##source", &source, view.editIncrement(), 1, "%.3f") && std::isfinite(source))
        changed = timeline.setClipContentDuration(track.id, clip.id,
            std::max(1.0 / 24.0, view.snapBeat(source)), clip.repeatContent) || changed;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Length of one source cycle. Notes and points stay in source beats. Hidden data is retained.");
    field("Stretch (%)");
    double ratio = ofxOceanodeTimelineClipTime::effectiveStretch(clip) * 100.0;
    if(ImGui::InputDouble("##stretch", &ratio, 1, 10, "%.2f") && std::isfinite(ratio)) {
        ratio = std::max(100.0 / 1024.0, ratio) / 100.0;
        const bool repeat = clip.repeatContent;
        const double duration = clip.durationBeats * ratio / ofxOceanodeTimelineClipTime::effectiveStretch(clip);
        changed = timeline.setClipTiming(track.id, clip.id, clip.startBeat, duration) || changed;
        clip.contentStretch = ratio;
        clip.repeatContent = track.isWaveTrack ? false : repeat;
    }
    if(table) ImGui::EndTable();
    if(!track.isWaveTrack) {
        bool repeat = clip.repeatContent;
        if(ImGui::Checkbox("Repeat source", &repeat)) {
            if(repeat) clip.contentStretch = ofxOceanodeTimelineClipTime::effectiveStretch(clip);
            changed = timeline.setClipContentDuration(track.id, clip.id, clip.contentDurationBeats, repeat) || changed;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("One cycle: %.3f timeline beats", ofxOceanodeTimelineClipTime::sourceDuration(clip) * ofxOceanodeTimelineClipTime::effectiveStretch(clip));
    } else ImGui::TextDisabled("Audio: length and stretch change playback speed for the full source slice.");
    if(changed) timeline.invalidateSchedule();
    ImGui::PopID();
    return changed;
}
inline void drawClipPropertiesPanel(ofxOceanodeTimelineManager& timeline,
        const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
        const ofxOceanodeTimelineEditorView& view, ofxOceanodeTimelineEndEdit operation) {
    ImGui::PushID(clip.id.c_str());
    const float width = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetTextLineHeightWithSpacing() * 3.0f +
        ImGui::GetFrameHeightWithSpacing() * (width >= 650.0f ? 3.0f : 5.0f) + 18.0f;
    if(ImGui::BeginChild("##clipTimingPanel", ImVec2(width, height), true)) {
        ImGui::TextWrapped("Track: %s > Clip: %s", track.name.c_str(), clip.name.c_str());
        ImGui::TextDisabled("%s | Timing applies to the whole clip", clipSummary(clip, track.isWaveTrack).c_str());
        drawClipTiming(timeline, track, clip, view, operation);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

}
