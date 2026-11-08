#pragma once
#include "ofxOceanodeTimelineLaneEditor.h"
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeTimelineCurveEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;
    bool hasPointSelection(const std::string& laneId) const {
        return curveSelectionLaneId == laneId && !curveSelectedPointIndices.empty();
    }
    void clearSelection() { curveSelectedPointIndices.clear(); }

private:
    // Selected points (indices into the lane's curvePoints, which stay in
    // beat order) of the one lane they were picked in.
    std::string curveSelectionLaneId;
    std::set<int> curveSelectedPointIndices;
    int curveDragPointIndex = -1;
    int curveDragCycle = 0;
    double curveDragSourceStartBeat = 0.0;
    double curveDragTimelineStartBeat = 0.0;
    int curveValuePointIndex = -1;
    float curveNumericValue = 0.0f;
    int curveTensionSegment = -1;
    float curveTensionDragStartX = 0.0f;
    float curveTensionDragStartY = 0.0f;
    float curveTensionStartInflection = 0.5f;
    float curveTensionStartSteepness = 1.0f;
};
