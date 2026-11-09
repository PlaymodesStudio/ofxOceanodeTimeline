#pragma once
#include "ofxOceanodeTimelineClipEditorState.h"
#include "ofxOceanodeTimelineLaneEditor.h"
#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <functional>
#include <string>

// The LFO lane type. The lane itself is drawn by the shell like any other
// (header, Targets / Lane tabs, its canvas showing the oscillator's result
// over the clip); its control sublanes -- frequency, roundness, skew... --
// are rows of their own, drawn below it by drawControls.
class ofxOceanodeTimelineLfoEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;
    // The control sublanes of context.lane, one row each, indented under the
    // LFO lane's own row. Each folds and resizes on its own (keyed by
    // ofxOceanodeTimelineLfoControlKey); focusLane focuses the LFO lane.
    void drawControls(ofxOceanodeTimelineLaneEditorContext& context,
                      ofxOceanodeTimelineClipEditorState& editorState,
                      float contentWidth, double beatPosition,
                      const std::function<void()>& focusLane);
    // A point of one of this LFO lane's controls is selected (Delete removes it).
    bool hasPointSelection(const std::string& lfoLaneId) const {
        return selectedPointIndex >= 0 && selectedLaneId == lfoLaneId;
    }
    void clearSelection() { selectedPointIndex = -1; }

private:
    // The control being edited (its key), of the LFO lane selectedLaneId.
    std::string focusedControlKey;
    std::string selectedLaneId;
    std::string selectedControlKey;
    int selectedPointIndex = -1;
    // Point drag: in source beats, from where the mouse went down.
    std::string dragControlKey;
    int dragPointIndex = -1;
    double dragStartMouseBeat = 0.0;
    double dragStartSourceBeat = 0.0;
    // Alt-drag tension editing (Log/Exp, Sigmoid).
    int tensionSegment = -1;
    float tensionDragStartX = 0.0f;
    float tensionDragStartY = 0.0f;
    float tensionStartInflection = 0.5f;
    float tensionStartSteepness = 1.0f;
    bool valueDragActive = false;   // dragging the constant of a control in "Value" mode
    float pointNumericValue = 0.0f; // right-click "Value" field (real units)
};
