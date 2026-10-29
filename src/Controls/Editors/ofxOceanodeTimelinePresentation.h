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
        case ofxOceanodeTimelineLaneType::DrumGrid: return "Drum Grid";
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
inline bool drawClipTiming(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence,
                           const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
                           const ofxOceanodeTimelineEditorView& view,
                           ofxOceanodeTimelineEndEdit operation, bool tableLayout = true) {
    ImGui::PushID(clip.id.c_str());
    bool changed = false;
    bool muted = clip.muted;
    if(ImGui::Checkbox("Mute", &muted))
        changed = timeline.setClipMuted(sequence, track.id, clip.id, muted);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Mute clip: keeps its content, removes it from playback.");
    if(!track.isWaveTrack) {
        ImGui::SameLine();
        bool repeat = clip.repeatContent;
        if(ImGui::Checkbox("Repeat", &repeat)) {
            if(repeat) clip.contentStretch = ofxOceanodeTimelineClipTime::effectiveStretch(clip);
            changed = timeline.setClipContentDuration(sequence, track.id, clip.id, clip.contentDurationBeats, repeat) || changed;
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat source: a longer clip loops its source cycle instead of stretching it.");
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = width < 400.0f ? 1 : (tableLayout && width >= 650.0f ? 4 : 2);
    // Narrow (a lane pad): label column on the left, short labels, details in tooltips.
    const bool rows = columns == 1;
    const float labelColumn = std::min(64.0f, width * 0.36f);
    const bool table = !rows && ImGui::BeginTable("##sharedClipTiming", columns, ImGuiTableFlags_SizingStretchSame);
    auto field = [&](const char* label, const char* shortLabel) {
        if(table) ImGui::TableNextColumn();
        if(rows) {
            const float startX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(shortLabel);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
            ImGui::SameLine(startX + labelColumn);
        } else ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    field("Chance (%)", "Chance");
    float chance = clip.chance * 100.0f;
    if(ImGui::SliderFloat("##chance", &chance, 0.0f, 100.0f, "%.0f%%"))
        changed = timeline.setClipChance(sequence, track.id, clip.id, std::round(chance) / 100.0f) || changed;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Probability that the whole clip plays.\nRolled again each time the clip starts (play, seek, every loop pass).");
    field("Start (beats)", "Start");
    double start = clip.startBeat;
    if(ImGui::InputDouble("##start", &start, rows ? 0.0 : view.editIncrement(), 1, "%.3f") && std::isfinite(start)) {
        changed = timeline.setClipTiming(sequence, track.id, clip.id, view.snapBeat(start), clip.durationBeats) || changed;
    }
    field("Timeline length (beats)", "Length");
    double length = clip.durationBeats;
    if(ImGui::InputDouble("##length", &length, rows ? 0.0 : view.editIncrement(), 1, "%.3f"))
        changed = timeline.resizeClipEnd(sequence, track.id, clip.id, view.snapBeat(length), operation) || changed;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Timeline length (beats). Uses the right-edge tool: %s. Applies to every lane in this clip.",
        track.isWaveTrack ? "Stretch (audio playback speed changes)" : endEditName(operation));
    field("Source length (beats)", "Source");
    double source = ofxOceanodeTimelineClipTime::sourceDuration(clip);
    ImGui::BeginDisabled(track.isWaveTrack);
    if(ImGui::InputDouble("##source", &source, rows ? 0.0 : view.editIncrement(), 1, "%.3f") && std::isfinite(source))
        changed = timeline.setClipContentDuration(sequence, track.id, clip.id,
            std::max(1.0 / 24.0, view.snapBeat(source)), clip.repeatContent) || changed;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Source length (beats): one source cycle. Notes and points stay in source beats. Hidden data is retained.");
    // Stretch and Tempo are one setting: timeline beats per source beat, or
    // its inverse read as speed. Either keeps the number of repeats.
    auto setStretch = [&](double stretch) {
        stretch = std::max(1.0 / 1024.0, stretch);
        const bool repeat = clip.repeatContent;
        const double duration = clip.durationBeats * stretch / ofxOceanodeTimelineClipTime::effectiveStretch(clip);
        changed = timeline.setClipTiming(sequence, track.id, clip.id, clip.startBeat, duration) || changed;
        clip.contentStretch = stretch;
        clip.repeatContent = track.isWaveTrack ? false : repeat;
    };
    field("Stretch (%)", "Stretch");
    double ratio = ofxOceanodeTimelineClipTime::effectiveStretch(clip) * 100.0;
    if(ImGui::InputDouble("##stretch", &ratio, rows ? 0.0 : 1.0, 10, "%.2f%%") && std::isfinite(ratio))
        setStretch(std::max(100.0 / 1024.0, ratio) / 100.0);
    field("Tempo (x global)", "Tempo");
    {
        struct TempoRatio { double speed; const char* label; };
        static const TempoRatio kTempoRatios[] = {
            {4.0, "x4"}, {3.0, "x3"}, {2.0, "x2"}, {1.5, "3:2"}, {4.0 / 3.0, "4:3"}, {1.0, "x1"},
            {0.75, "3:4"}, {2.0 / 3.0, "2:3"}, {0.5, "x1/2"}, {1.0 / 3.0, "x1/3"}, {0.25, "x1/4"}};
        const double speed = 1.0 / ofxOceanodeTimelineClipTime::effectiveStretch(clip);
        std::string current = "x" + ofToString(speed, 3);
        for(const auto& tempo : kTempoRatios)
            if(std::abs(tempo.speed - speed) < 1e-6) current = tempo.label;
        if(ImGui::BeginCombo("##tempo", current.c_str())) {
            for(const auto& tempo : kTempoRatios)
                if(ImGui::Selectable(tempo.label, std::abs(tempo.speed - speed) < 1e-6)) setStretch(1.0 / tempo.speed);
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip(track.isWaveTrack
            ? "Playback speed against the global tempo (the same setting as Stretch). Audio pitch follows."
            : "Speed against the global tempo (the same setting as Stretch): x2 plays the content twice as fast.");
    }
    if(!track.isWaveTrack && !clip.isLfo) {
        field("Offset (source beats)", "Offset");
        double offset = clip.contentOffsetBeats;
        if(ImGui::InputDouble("##offset", &offset, rows ? 0.0 : view.editIncrement(), 1, "%.3f") && std::isfinite(offset))
            changed = timeline.setClipContentOffset(sequence, track.id, clip.id, view.snapBeat(offset)) || changed;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Where the content starts playing: from here to its end, then on from its start.\nThe content itself is not changed.");
    }
    if(table) ImGui::EndTable();
    if(!track.isWaveTrack)
        ImGui::TextDisabled("Cycle %.3f beats", ofxOceanodeTimelineClipTime::sourceDuration(clip) * ofxOceanodeTimelineClipTime::effectiveStretch(clip));
    else ImGui::TextDisabled("Audio: length and stretch change playback speed.");
    if(changed) timeline.invalidateSchedule();
    ImGui::PopID();
    return changed;
}
inline void drawClipPropertiesPanel(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence,
        const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
        const ofxOceanodeTimelineEditorView& view, ofxOceanodeTimelineEndEdit operation) {
    ImGui::PushID(clip.id.c_str());
    const float width = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    // drawClipTiming: a checkbox row, five fields (in 4, 2 or 1 columns) and a note.
    const float fieldRows = width >= 650.0f ? 2.0f : width >= 400.0f ? 3.0f : 5.0f;
    const float height = ImGui::GetTextLineHeightWithSpacing() * (3.0f + (width >= 400.0f ? fieldRows : 0.0f)) +
        ImGui::GetFrameHeightWithSpacing() * (1.0f + fieldRows) + 18.0f;
    if(ImGui::BeginChild("##clipTimingPanel", ImVec2(width, height), true)) {
        ImGui::TextWrapped("Track: %s > Clip: %s", track.name.c_str(), clip.name.c_str());
        ImGui::TextDisabled("%s | Timing applies to the whole clip", clipSummary(clip, track.isWaveTrack).c_str());
        drawClipTiming(timeline, sequence, track, clip, view, operation);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

}
