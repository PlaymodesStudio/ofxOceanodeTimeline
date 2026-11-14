#pragma once
#include "ofxOceanodeTimelineLaneEditor.h"
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeTimelineHeldValueEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;

private:
    int curveDragPointIndex = -1;
    int curveValuePointIndex = -1;
    float curveNumericValue = 0.0f;
};

// A Colour lane: two bands over the same time axis. Above, the colour keys (a
// swatch each, the band painted in the colour the lane plays); below, the
// sigmoid each key blends along into the next, shaped like a Project Track's
// easing. Here rather than in a file of its own: its points are kept like a
// held-value lane's.
class ofxOceanodeTimelineColorEditor final : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;

private:
    int keyDragIndex = -1;
    int keyEditIndex = -1;
    int blendDragIndex = -1;
    ImVec2 blendDragStart = ImVec2(0.0f, 0.0f);
    float blendDragWidth = 1.0f;
    ofxOceanodeTimelineCurveTension blendDragInitial;
};
