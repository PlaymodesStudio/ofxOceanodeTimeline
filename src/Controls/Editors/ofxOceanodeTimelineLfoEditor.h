#pragma once
#include "ofxOceanodeTimelineClipEditorState.h"
#include "ofxOceanodeTimelineLaneEditor.h"
#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <string>

class ofxOceanodeContainer;
class ofxOceanodeTimelineLfoEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const;
    // Returns a rename request for the shell to service.
    bool draw(ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
              const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
              const ofxOceanodeTimelineEditorView& view,
              ofxOceanodeTimelineClipEditorState& editorState,
              float contentWidth, double beatPosition,
              ofxOceanodeTimelineEndEdit endEdit = ofxOceanodeTimelineEndEdit::Repeat);
    void setTargetPaths(std::vector<std::string> paths) { availableTargetPaths = std::move(paths); }
    std::string consumeOutputTargetRequest() {
        auto path = std::move(requestedOutputTargetPath); requestedOutputTargetPath.clear(); return path;
    }
    bool hasPointSelection(const std::string& clipId) const {
        return lfoSelectedPointIndex >= 0 && lfoSelectedClipId == clipId;
    }
private:
    std::vector<std::string> availableTargetPaths;
    std::string requestedOutputTargetPath;
    char targetSearch[128] = {};
    std::string lfoDragLaneId;
    int lfoDragPointIndex = -1;
    // LFO editor point selection (Delete/Backspace or right-click > Delete point)
    std::string lfoSelectedClipId;
    std::string lfoSelectedLaneId;
    int lfoSelectedPointIndex = -1;
    // LFO editor Alt-drag tension editing (Log/Exp, Sigmoid)
    int lfoTensionSegment = -1;
    bool lfoValueDragActive = false; // dragging the constant in an LFO lane's "Value" mode
    float lfoPointNumericValue = 0.0f; // right-click "Set value" field (real units)
    float lfoTensionDragStartX = 0.0f;
    float lfoTensionDragStartY = 0.0f;
    float lfoTensionStartInflection = 0.5f;
    float lfoTensionStartSteepness = 1.0f;
};
