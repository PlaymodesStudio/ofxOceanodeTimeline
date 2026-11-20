#pragma once
#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeContainer;

// Borrowed only for this draw call. The shell owns focus and structural edits.
struct ofxOceanodeTimelineLaneEditorContext {
    ofxOceanodeTimelineManager& timeline;
    ofxOceanodeContainer& container;
    ofxOceanodeTimelineSequence& sequence; // the one the editor shows (main or a macro's)
    const ofxOceanodeTimelineTrack& track;
    ofxOceanodeTimelineClip& clip;
    ofxOceanodeTimelineLane& lane;
    const ofxOceanodeTimelineEditorView& view;
    std::string& focusedLaneId;
    bool focused;
    // Output request; the shell opens the instrument dialog after properties.
    std::string noteGroupToEdit;
    std::vector<std::string> targetPaths;
    // A macro's timeline: where each canvas it reaches starts in targetPaths
    // (its own first, then every canvas above it), with that canvas's heading.
    std::vector<std::pair<size_t, std::string>> targetGroups;
    std::string requestedTargetPath;
    ofxOceanodeTimelineTargetRole requestedTargetRole = ofxOceanodeTimelineTargetRole::Lane;
};

// The parameters of a target picker, inside its open combo. A macro's
// timeline gets one branch per canvas it reaches (groups), folded until
// asked for; a search lists what matches in all of them, under their
// headings. `current` is the path shown as chosen. True once one is picked.
inline bool ofxOceanodeTimelineTargetList(const std::vector<std::string>& paths,
                                          const std::vector<std::pair<size_t, std::string>>& groups,
                                          const char* search, const std::string& current, std::string& picked) {
    std::string needle = search == nullptr ? std::string() : std::string(search);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto matches = [&](const std::string& path) {
        if(needle.empty()) return true;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find(needle) != std::string::npos;
    };
    bool chosen = false;
    auto row = [&](size_t index, bool underHeading) {
        const auto& path = paths[index];
        // Under its canvas's heading a parameter goes without the canvas in front.
        const size_t canvas = underHeading ? path.find("::") : std::string::npos;
        const std::string label = (canvas == std::string::npos ? path : path.substr(canvas + 2)) +
            "##target" + std::to_string(index);
        if(ImGui::Selectable(label.c_str(), !current.empty() && path == current)) { picked = path; chosen = true; }
        if(canvas != std::string::npos && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    };
    const size_t firstGroup = groups.empty() ? paths.size() : std::min(groups.front().first, paths.size());
    for(size_t index = 0; index < firstGroup; ++index) if(matches(paths[index])) row(index, false);
    for(size_t group = 0; group < groups.size(); ++group) {
        const size_t begin = std::min(groups[group].first, paths.size());
        const size_t end = group + 1 < groups.size() ? std::min(groups[group + 1].first, paths.size()) : paths.size();
        if(begin >= end) continue;
        if(!needle.empty()) {
            // A search looks in every canvas: its matches, under their heading.
            bool headed = false;
            for(size_t index = begin; index < end; ++index) {
                if(!matches(paths[index])) continue;
                if(!headed) { ImGui::SeparatorText(groups[group].second.c_str()); headed = true; }
                row(index, true);
            }
            continue;
        }
        // (Folded, unless it is the only place to look in.)
        const std::string branch = groups[group].second + "##targetScope" + std::to_string(group);
        if(ImGui::TreeNodeEx(branch.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                             (groups.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))) {
            for(size_t index = begin; index < end; ++index) row(index, true);
            ImGui::TreePop();
        }
    }
    return chosen;
}

// Screen geometry belongs to the shell; editors draw inside its clip rectangle.
struct ofxOceanodeTimelineLaneCanvas {
    ImVec2 min, max, timelineOrigin;
    float zoneLeft, left, right;
    bool hovered;
    double beatPosition;
};

// Read-only content and arrangement geometry for a clip's miniature preview.
// lane is null when the clip has no lane to show.
struct ofxOceanodeTimelineClipPreview {
    ofxOceanodeTimelineManager& timeline;
    ofxOceanodeContainer& container;
    ofxOceanodeTimelineSequence& sequence;
    const ofxOceanodeTimelineTrack& track;
    const ofxOceanodeTimelineClip& clip;
    const ofxOceanodeTimelineLane* lane;
    const ofxOceanodeTimelineEditorView& view;
    ImVec2 min, max;
    float left, right;
    int firstVisibleCycle, lastVisibleCycle;
    // What the content is drawn in: white, or the clip's own colour where
    // the clip darkens its plot area.
    ImU32 ink = IM_COL32(245, 245, 245, 255);
};
inline ImU32 previewInk(const ofxOceanodeTimelineClipPreview& preview, int alpha) {
    return (preview.ink & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

class ofxOceanodeTimelineLaneEditor {
public:
    virtual ~ofxOceanodeTimelineLaneEditor() = default;
    virtual void drawTargets(ofxOceanodeTimelineLaneEditorContext& context) {}
    virtual void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {}
    // Lanes whose notes can mutate get a Mutate tab with their rules.
    virtual bool hasMutation() const { return false; }
    virtual void drawMutation(ofxOceanodeTimelineLaneEditorContext& context) {}
    // Lanes of notes get a Rndm tab: random octave and pitch deviation.
    virtual bool hasRandomDeviation() const { return false; }
    virtual void drawRandomDeviation(ofxOceanodeTimelineLaneEditorContext& context) {}
    // An optional extra properties tab of the lane type (nullptr: none).
    virtual const char* extraTabLabel() const { return nullptr; }
    virtual void drawExtraTab(ofxOceanodeTimelineLaneEditorContext& context) {}
    virtual void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const = 0;
    virtual void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                            const ofxOceanodeTimelineLaneCanvas& canvas) = 0;
};
