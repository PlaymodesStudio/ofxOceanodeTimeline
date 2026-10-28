#pragma once
#include "ofxOceanodeTimelineLaneEditor.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Drum Grid: one row per kit piece (see ofxOceanodeTimelineDrumRow). Hits sit
// at beat positions; the lane's grid (the timeline's master grid, or its own
// division) is where new hits snap and which columns are drawn. Hits off the
// current grid stay where they are and stay editable.
// Row headers sit left of the timeline zone (where a piano roll has its
// keyboard); a parameter strip under the grid paints the focused row's
// velocity, pitch, probability, ratchet, gate or duration, one bar per hit.
class ofxOceanodeTimelineDrumGridEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawTargets(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;
    // "Row" tab: the focused row's settings and which parameter the strip edits.
    const char* extraTabLabel() const override { return "Row"; }
    void drawExtraTab(ofxOceanodeTimelineLaneEditorContext& context) override;
    // Width of the row-header column left of the timeline zone.
    static float headerWidth();
    // Width of a row header's length field (between its - and + buttons).
    static float lengthFieldWidth();
    void clearSelection() { drag = Drag::None; popupRow = popupStep = -1; }

    enum class StripMode { Velocity, Pitch, Probability, Ratchet, Gate, Duration, Filter, Stutter };
    static constexpr int kStripModeCount = 8;

private:
    int focusedRow(const ofxOceanodeTimelineLane& lane) const;
    void drawRowMenu(ofxOceanodeTimelineLane& lane, double grid);
    void drawStepMenu(ofxOceanodeTimelineLane& lane, double grid);
    void drawRowSettings(ofxOceanodeTimelineLane& lane, int rowIndex, double grid);

    char targetSearch[128] = {};
    // Per lane (session state): the row the parameter strip edits, and its tab.
    std::map<std::string, int> focusedRows;
    std::map<std::string, StripMode> stripModes;

    // Pending: pressed, not moved yet (release toggles). Paint / Erase: add
    // hits to empty cells / remove hits along the stroke. Velocity / Move:
    // dragging the grabbed hit. Strip / StripFine: painting bars.
    // Length: dragging a row's end-of-cycle handle.
    enum class Drag { None, Pending, Paint, Erase, Velocity, Move, Strip, StripFine, Length };
    Drag drag = Drag::None;
    std::string dragLaneId;
    int dragRow = -1;
    int dragStep = -1;            // grabbed hit (index in its row), -1: an empty cell
    double dragCellBeat = 0.0;    // pattern-relative start of the pressed cell
    double dragStartPos = 0.0;    // pattern-relative beat under the press
    double dragStartBeat = 0.0;   // the grabbed hit's position at the press
    float dragStartY = 0.0f;
    float dragStartValue = 0.0f;
    float previousMouseX = 0.0f, previousMouseY = 0.0f;
    int dragLengthCycle = 0;      // the clip repeat whose end line was grabbed

    // Context menus: which row / hit they edit.
    std::string popupLaneId;
    int popupRow = -1;
    int popupStep = -1;
    char renameBuffer[128] = {};
    int popupFramesClosed = 0;   // swallow the click that closes a menu

    bool hasClipboard = false;
    ofxOceanodeTimelineDrumRow clipboard;
};
