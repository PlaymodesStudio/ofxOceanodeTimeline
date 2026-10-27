#pragma once
#include "ofxOceanodeTimelineEditorView.h"
#include "ofxOceanodeTimelineEditorUi.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <string>
#include <vector>

namespace ofxOceanodeTimelineLaneEditorUi {
bool playbackToggle(const std::string& id, const char* label, bool on, const char* tooltip);
constexpr double kPPQ = ofxOceanodeTimelineEditorView::ppq;
constexpr float kPianoKeyboardWidth = 38.0f;
constexpr float kPianoScrollbarWidth = 10.0f;
constexpr float kPianoZoomButtonHeight = 14.0f;
constexpr float kLaneResizeHandleHeight = 10.0f;
constexpr float kLaneEditorMinHeight = 90.0f;
constexpr float kLaneEditorMaxHeight = 640.0f;
inline float& kLabelWidth = ofxOceanodeTimelineEditorUi::labelWidth;
const ImU32 kGrid = ofxOceanodeTimelineEditorUi::gridColor;
const ImU32 kBar = ofxOceanodeTimelineEditorUi::barColor;
const ImU32 kPlayhead = ofxOceanodeTimelineEditorUi::playheadColor;
constexpr auto& kCurveInterpolationNames = ofxOceanodeTimelineEditorUi::curveInterpolationNames;
constexpr const char* kLfoInterpolationNames[] = {
    "Value", "Step", "Linear", "Log / Exp", "Sigmoid"
};
struct DivisionOption {
    const char* label;
    double beats;
};

constexpr DivisionOption kDivisionOptions[] = {
    {"None", 0.0},
    {"Whole", 4.0}, {"Half", 2.0}, {"Quarter", 1.0},
    {"8th", 0.5}, {"8thD", 0.75}, {"8thT", 1.0 / 3.0},
    {"16th", 0.25}, {"16thD", 0.375}, {"16thT", 1.0 / 6.0},
    {"32nd", 0.125}, {"32ndD", 0.1875}, {"32ndT", 1.0 / 12.0},
    {"64th", 0.0625}
};

constexpr int kDivisionOptionCount = static_cast<int>(sizeof(kDivisionOptions) / sizeof(kDivisionOptions[0]));

int divisionIndexForBeats(double beats);
std::string compactParameterName(const std::string& path);
ImU32 mutedTrackColor(const ofColor& color, float brightness, float saturation = 0.34f);
bool hoverRect(const ImVec2& min, const ImVec2& max);
std::vector<std::string> laneDropdownOptions(ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& container,
                                             const std::string& trackId, const ofxOceanodeTimelineLane& lane);
std::string dropdownOptionLabel(const std::vector<std::string>& options, float value);
int insertLinearCurvePoint(ofxOceanodeTimelineLane& lane,
                           const ofxOceanodeTimelineCurvePoint& point);
int curveSnapLevels(const ofxOceanodeTimelineLane& lane);
void resetCurveTensions(ofxOceanodeTimelineLane& lane);
void eraseCurvePointWithTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex);
void eraseCurvePointKeepingTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex);
// Lane-pad property row: the label in a left column, then sizes the next
// widget to fill the row (less reserveRight for a trailing button).
// Give the widget a "##id" label.
void propertyLabel(const char* label, float reserveRight = 0.0f);
// Width a trailing SmallButton takes, for propertyLabel's reserveRight.
float smallButtonReserve(const char* label);
// One octave of keys, C at the left, toggling pitch classes in mask
// (bit 0 = C). Returns true when the mask changed.
bool pitchClassKeyboard(const char* id, int& mask, ImU32 onColor, float height = 34.0f);
// Text shortened with "..." to fit maxWidth.
std::string fitText(const std::string& text, float maxWidth);
// Indentation of a clip editor's header and lane rows under the track.
constexpr float kClipEditorIndent = 10.0f;
// The header row of an open clip editor, kept inside the left column:
// fold toggle, the clip's name (shortened to fit), then "+" (add lane, when
// asked for) and "x" (close) pinned to the column's right edge.
struct ClipEditorHeaderActions { bool toggleFold = false, rename = false, addLane = false, close = false; };
ClipEditorHeaderActions drawClipEditorHeader(const ImVec2& rowMin, const ImVec2& rowMax,
                                             const ofxOceanodeTimelineTrack& track,
                                             const ofxOceanodeTimelineClip& clip,
                                             bool folded, const char* tag, bool addLaneButton);
void drawDivision(ofxOceanodeTimelineLane& lane);
}
