#pragma once

#include <functional>
#include <string>
#include <vector>

class ofxOceanodeTimelineEditorView;
struct ofxOceanodeTimelineCurvePoint;
struct ofxOceanodeTimelineCurveTension;

// The BPM lane the editor edits: the global tempo's (the manager's own
// fields, for a timeline linked to the global transport) or a timeline's own
// (its play settings, for a free one). Points and tensions are edited in
// place; the rest goes through setters, so each owner keeps its side effects.
struct ofxOceanodeTimelineTempoLane {
    std::vector<ofxOceanodeTimelineCurvePoint>* points = nullptr;
    std::vector<ofxOceanodeTimelineCurveTension>* tensions = nullptr;
    const bool* visible = nullptr;
    bool collapsed = true;
    bool enabled = false;
    float minimum = 20.0f;
    float maximum = 300.0f;
    std::string interpolation = "Linear";
    float manualBpm = 120.0f; // the flat tempo while the lane is not enabled
    bool loopEnabled = false;
    double loopStartBeat = 0.0;
    double loopEndBeat = 0.0;
    double beatsPerBar = 4.0;
    std::function<void(bool)> setCollapsed;
    std::function<void(bool)> setEnabled;
    std::function<void(float, float)> setRange;
    std::function<void(const std::string&)> setInterpolation;
    std::function<void(float)> setManualBpm;
};

class ofxOceanodeTimelineBpmEditor {
public:
    void draw(const ofxOceanodeTimelineTempoLane& lane, const ofxOceanodeTimelineEditorView& view,
              float contentWidth, double beatPosition);

private:
    // The manual BPM, typed (applies on Enter), while it is being edited.
    char manualBpmText[24] = {};
    int bpmDragPointIndex = -1;
    int bpmValuePointIndex = -1;
    float bpmNumericValue = 120.0f;
    int bpmTensionSegment = -1;
    float bpmTensionDragStartX = 0.0f;
    float bpmTensionDragStartY = 0.0f;
    float bpmTensionStartInflection = 0.5f;
    float bpmTensionStartSteepness = 1.0f;
};
