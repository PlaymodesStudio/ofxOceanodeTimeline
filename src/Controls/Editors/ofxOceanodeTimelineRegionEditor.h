#pragma once
#include "ofxOceanodeTimelineLaneEditor.h"
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeTimelineRegionEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;

private:
    // MultiValue / MultiGate region drag (create/move/resize), shared by
    // both lane types since only one lane is ever focused (interactive) at
    // a time -- see the .cpp for why that makes sharing this state safe.
    // Keyed by row index (into the lane's multiValueRows/multiGateRows) +
    // index into that row's region vector.
    enum class RegionDragMode { None, Create, Move, ResizeRight };
    RegionDragMode regionDragMode = RegionDragMode::None;
    int regionDragRow = -1;
    int regionDragIndex = -1;
    double regionDragAnchorBeat = 0.0;
    double regionDragOriginalStart = 0.0;
    double regionDragOriginalDuration = 0.0;
    // MultiValue's double-click "edit value" popup (mirrors the Curve
    // lane's "Curve point value" popup).
    int multiValueEditRow = -1;
    int multiValueEditIndex = -1;
    float multiValueEditNumeric = 0.0f;
};
