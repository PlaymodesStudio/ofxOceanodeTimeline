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
#include "ofxOceanodeTimelineLfoEditor.h"

// One editor per type, owned by the controller; no model pointers are retained.
class ofxOceanodeTimelineLaneEditors {
public:
    ofxOceanodeTimelineLaneEditor& forType(ofxOceanodeTimelineLaneType type);
    bool hasNoteSelection(const std::string& laneId) const { return pianoRoll.hasNoteSelection(laneId); }
    bool hasCurvePointSelection(const std::string& laneId) const { return curve.hasPointSelection(laneId); }
    // A point of one of this LFO lane's control sublanes is selected.
    bool hasLfoPointSelection(const std::string& laneId) const { return lfo.hasPointSelection(laneId); }
    // The LFO lane editor, for the control sublanes the shell draws below it.
    ofxOceanodeTimelineLfoEditor& lfoEditor() { return lfo; }
    void updateMonitoring(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence, const std::string& laneId) {
        pianoRoll.updateMonitoring(timeline, sequence, laneId);
    }
    void clearSelection() { pianoRoll.clearSelection(); drumGrid.clearSelection(); curve.clearSelection(); lfo.clearSelection(); }
    void forgetTrack(const std::string& trackId) { pianoRoll.forgetTrack(trackId); }
    static float keyboardReserve(ofxOceanodeTimelineLaneType type);
    static float defaultHeight(ofxOceanodeTimelineLaneType type);
private:
    ofxOceanodeTimelineStepEditor step;
    ofxOceanodeTimelineCurveEditor curve;
    ofxOceanodeTimelinePianoRollEditor pianoRoll;
    ofxOceanodeTimelineNumberEditor number;
    ofxOceanodeTimelineDropdownEditor dropdown;
    ofxOceanodeTimelineColorEditor color;
    ofxOceanodeTimelineMultiSliderEditor multiSlider;
    ofxOceanodeTimelineMultiValueEditor multiValue;
    ofxOceanodeTimelineMultiGateEditor multiGate;
    ofxOceanodeTimelineDrumGridEditor drumGrid;
    ofxOceanodeTimelineLfoEditor lfo;
};
