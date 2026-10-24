#pragma once
#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include "imgui.h"
#include <string>

class ofxOceanodeContainer;

// Borrowed only for this draw call. The shell owns focus and structural edits.
struct ofxOceanodeTimelineLaneEditorContext {
    ofxOceanodeTimelineManager& timeline;
    ofxOceanodeContainer& container;
    const ofxOceanodeTimelineTrack& track;
    ofxOceanodeTimelineClip& clip;
    ofxOceanodeTimelineLane& lane;
    const ofxOceanodeTimelineEditorView& view;
    std::string& focusedLaneId;
    bool focused;
    // Output request; the shell opens the instrument dialog after properties.
    std::string noteGroupToEdit;
};

// Screen geometry belongs to the shell; editors draw inside its clip rectangle.
struct ofxOceanodeTimelineLaneCanvas {
    ImVec2 min, max, timelineOrigin;
    float zoneLeft, left, right;
    bool hovered;
    double beatPosition;
};

// Read-only content and arrangement geometry for a clip's miniature preview.
// lane is null for self-contained LFO clips.
struct ofxOceanodeTimelineClipPreview {
    ofxOceanodeTimelineManager& timeline;
    ofxOceanodeContainer& container;
    const ofxOceanodeTimelineTrack& track;
    const ofxOceanodeTimelineClip& clip;
    const ofxOceanodeTimelineLane* lane;
    const ofxOceanodeTimelineEditorView& view;
    ImVec2 min, max;
    float left, right;
    int firstVisibleCycle, lastVisibleCycle;
};

class ofxOceanodeTimelineLaneEditor {
public:
    virtual ~ofxOceanodeTimelineLaneEditor() = default;
    virtual void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {}
    virtual void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const = 0;
    virtual void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                            const ofxOceanodeTimelineLaneCanvas& canvas) = 0;
};
