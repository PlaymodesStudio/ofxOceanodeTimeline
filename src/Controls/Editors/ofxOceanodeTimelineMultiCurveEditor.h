#pragma once
#include "ofxOceanodeTimelineCurveEditor.h"
#include <map>
#include <string>

// Several curves on one lane (one per component of a bound vector), edited
// one at a time: a strip of tabs along the top picks the curve the canvas
// edits, as in ofxSantiNodes' sigmoidCurve; the others are drawn behind it.
// Or, from a tab's menu, each curve in a lane of its own, one below the other.
// The same menu names a curve.
// The curve itself is edited by an ordinary Curve editor.
class ofxOceanodeTimelineMultiCurveEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;
    // A point of the curve this lane's canvas edits is selected.
    bool hasPointSelection(const std::string& laneId) const;
    void clearSelection() { curve.clearSelection(); }

private:
    int activeCurve(const ofxOceanodeTimelineLane& lane) const;
    ofxOceanodeTimelineCurveEditor curve;
    // The tab each lane is on (by lane id). Editor state: not saved.
    std::map<std::string, int> activeCurves;
    // The tab a right-click's menu is for.
    std::string menuLaneId;
    int menuCurve = -1;
    char menuName[64] = {};
    // The Curves count being typed (and the lane it is for), until the edit ends.
    std::string pendingCountLaneId;
    int pendingCount = 0;
};
