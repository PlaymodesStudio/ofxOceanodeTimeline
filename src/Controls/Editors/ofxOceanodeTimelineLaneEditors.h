#pragma once
#include "ofxOceanodeTimelineStepEditor.h"
#include "ofxOceanodeTimelineCurveEditor.h"
#include "ofxOceanodeTimelinePianoRollEditor.h"
#include "ofxOceanodeTimelineNumberEditor.h"
#include "ofxOceanodeTimelineDropdownEditor.h"
#include "ofxOceanodeTimelineMultiSliderEditor.h"
#include "ofxOceanodeTimelineMultiValueEditor.h"
#include "ofxOceanodeTimelineMultiGateEditor.h"
#include "ofxOceanodeTimelineDrumGridEditor.h"

// One editor per type, owned by the controller; no model pointers are retained.
class ofxOceanodeTimelineLaneEditors {
public:
    ofxOceanodeTimelineLaneEditor& forType(ofxOceanodeTimelineLaneType type);
    bool hasNoteSelection(const std::string& laneId) const { return pianoRoll.hasNoteSelection(laneId); }
    void updateMonitoring(ofxOceanodeTimelineManager& timeline, const std::string& laneId) { pianoRoll.updateMonitoring(timeline, laneId); }
    void clearSelection() { pianoRoll.clearSelection(); drumGrid.clearSelection(); }
    void forgetTrack(const std::string& trackId) { pianoRoll.forgetTrack(trackId); }
    static float keyboardReserve(ofxOceanodeTimelineLaneType type);
    static float defaultHeight(ofxOceanodeTimelineLaneType type);
private:
    ofxOceanodeTimelineStepEditor step;
    ofxOceanodeTimelineCurveEditor curve;
    ofxOceanodeTimelinePianoRollEditor pianoRoll;
    ofxOceanodeTimelineNumberEditor number;
    ofxOceanodeTimelineDropdownEditor dropdown;
    ofxOceanodeTimelineMultiSliderEditor multiSlider;
    ofxOceanodeTimelineMultiValueEditor multiValue;
    ofxOceanodeTimelineMultiGateEditor multiGate;
    ofxOceanodeTimelineDrumGridEditor drumGrid;
};
