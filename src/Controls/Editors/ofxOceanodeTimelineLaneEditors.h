#pragma once
#include "ofxOceanodeTimelineStepEditor.h"
#include "ofxOceanodeTimelineCurveEditor.h"
#include "ofxOceanodeTimelinePianoRollEditor.h"
#include "ofxOceanodeTimelineNumberEditor.h"
#include "ofxOceanodeTimelineDropdownEditor.h"
#include "ofxOceanodeTimelineMultiSliderEditor.h"
#include "ofxOceanodeTimelineMultiValueEditor.h"
#include "ofxOceanodeTimelineMultiGateEditor.h"

// One editor per type, owned by the controller; no model pointers are retained.
class ofxOceanodeTimelineLaneEditors {
public:
    ofxOceanodeTimelineLaneEditor& forType(ofxOceanodeTimelineLaneType type);
    void clearSelection() { pianoRoll.clearSelection(); }
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
};
