#pragma once

class ofxOceanodeContainer;
class ofxOceanodeTimelineManager;
class ofxOceanodeTimelineEditorView;

class ofxOceanodeTimelineBpmEditor {
public:
    void draw(ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
              const ofxOceanodeTimelineEditorView& view, float contentWidth, double beatPosition);

private:
    int bpmDragPointIndex = -1;
    int bpmValuePointIndex = -1;
    float bpmNumericValue = 120.0f;
    int bpmTensionSegment = -1;
    float bpmTensionDragStartX = 0.0f;
    float bpmTensionDragStartY = 0.0f;
    float bpmTensionStartInflection = 0.5f;
    float bpmTensionStartSteepness = 1.0f;
};
