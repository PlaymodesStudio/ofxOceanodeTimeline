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
    // A Chords lane's chord names, left of its rows (taken from the
    // properties column, like the piano roll's keyboard).
    static float chordHeaderWidth();

private:
    // MultiValue / MultiGate region drag (create/move/resize), shared by
    // both lane types since only one lane is ever focused (interactive) at
    // a time -- see the .cpp for why that makes sharing this state safe.
    // Keyed by row index (into the lane's multiValueRows/multiGateRows) +
    // index into that row's region vector.
    enum class RegionDragMode { None, Create, Move, ResizeRight };
    RegionDragMode regionDragMode = RegionDragMode::None;
    // A Toggle (one-row MultiGate) drag in its velocity strip.
    bool velocityDrag = false;
    int regionDragRow = -1;
    int regionDragIndex = -1;
    double regionDragAnchorBeat = 0.0;
    double regionDragOriginalStart = 0.0;
    double regionDragOriginalDuration = 0.0;
    // A Chords lane plays one chord at a time: the block drawn or moved
    // carves its span out of the other rows when it is let go.
    static void carveOtherRows(ofxOceanodeTimelineLane& lane, int row, double start, double end);
    // MultiValue's double-click "edit value" popup (mirrors the Curve
    // lane's "Curve point value" popup).
    int multiValueEditRow = -1;
    int multiValueEditIndex = -1;
    float multiValueEditNumeric = 0.0f;
};
