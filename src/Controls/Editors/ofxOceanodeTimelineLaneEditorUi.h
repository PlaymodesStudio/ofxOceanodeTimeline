#pragma once
#include <algorithm>
#include <set>
#include "ofxOceanodeTimelineEditorView.h"
#include "ofxOceanodeTimelineEditorUi.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <string>
#include <vector>

namespace ofxOceanodeTimelineLaneEditorUi {
bool playbackToggle(const std::string& id, const char* label, bool on, const char* tooltip);
// A lane's top row is its fold arrow and name from the left, then M and S
// against its right edge, as on a track pad. playbackPairWidth: what the
// two buttons take, `spacing` apart. sameLinePlaybackPair: stays on the
// current line and moves to where M goes for S to end at `right` (screen x).
float playbackPairWidth(float spacing);
void sameLinePlaybackPair(float right, float spacing);
// A lane's name: a Selectable drawn with no background, focused or not.
void drawChanceKnob(ImDrawList* dl, float chance, bool active, const ImVec2& center, float radius);
ImVec2 laneChanceCenter(const ImVec2& min, const ImVec2& max);
bool laneChanceControl(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence,
                       const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
                       ofxOceanodeTimelineLane& lane, const ImVec2& center);
bool laneNameSelectable(const std::string& label, bool focused, const ImVec2& size);
constexpr double kPPQ = ofxOceanodeTimelineEditorView::ppq;
constexpr float kPianoKeyboardWidth = 38.0f;
constexpr float kPianoScrollbarWidth = 10.0f;
constexpr float kPianoZoomButtonHeight = 14.0f;
constexpr float kLaneResizeHandleHeight = 10.0f;
// One header line, the pad's top/bottom inset and the bottom resize grip.
inline float laneEditorMinHeight() {
    return ImGui::GetTextLineHeight() + 8.0f + kLaneResizeHandleHeight;
}
inline bool compactLaneHeight(float height) {
    return height < laneEditorMinHeight() + 2.0f * ImGui::GetFrameHeightWithSpacing();
}
constexpr float kLaneEditorMaxHeight = 640.0f;
inline float& kLabelWidth = ofxOceanodeTimelineEditorUi::labelWidth;
const ImU32 kGrid = ofxOceanodeTimelineEditorUi::gridColor;
const ImU32 kBar = ofxOceanodeTimelineEditorUi::barColor;
const ImU32 kPlayhead = ofxOceanodeTimelineEditorUi::playheadColor;
inline constexpr auto& kCurveInterpolationNames =ofxOceanodeTimelineEditorUi::curveInterpolationNames;
constexpr const char* kLfoInterpolationNames[] = {
    "Value", "Step", "Linear", "Log / Exp", "Sigmoid"
};
struct DivisionOption {
    const char* label;
    double beats;
};

constexpr DivisionOption kDivisionOptions[] = {
    {"None", 0.0},
    {"Whole", 4.0},
    {"Half", 2.0}, {"HalfD", 3.0}, {"HalfT", 4.0 / 3.0},
    {"Quarter", 1.0}, {"QuarterD", 1.5}, {"QuarterT", 2.0 / 3.0},
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
// A Dropdown lane's points hold option indices. When the bound parameter's
// list gains or loses options, each point follows its option to its new
// index, and a point whose option is gone is removed.
void followDropdownOptions(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineLane& lane, const std::vector<std::string>& options);
std::vector<std::string> laneDropdownOptions(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineSequence& sequence, ofxOceanodeContainer& container,
                                             const std::string& trackId, const ofxOceanodeTimelineLane& lane);
std::string dropdownOptionLabel(const std::vector<std::string>& options, float value);
// Inserts a point in beat order; a segment it cuts keeps its bend in both
// parts (ofxOceanodeTimelineCurve::fitSegmentShape). Returns its index.
// afterSameBeat: on a beat that already has points, the new one goes after
// them instead of before (see the definition).
int insertCurvePoint(ofxOceanodeTimelineLane& lane,
                     const ofxOceanodeTimelineCurvePoint& point, bool afterSameBeat = false);
int curveSnapLevels(const ofxOceanodeTimelineLane& lane);
void resetCurveTensions(ofxOceanodeTimelineLane& lane);
void eraseCurvePointWithTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex);
void eraseCurvePointKeepingTensions(ofxOceanodeTimelineLane& lane, size_t pointIndex);
// Piano roll note clipboard, shared by every piano roll (so notes go from
// one clip, track or timeline to another).
// The chosen notes of a lane, their starts counted from the earliest one.
std::vector<ofxOceanodeTimelinePianoNote> copyPianoNotes(const ofxOceanodeTimelineLane& lane, const std::set<int>& indices);
// Adds the notes with the earliest at atBeat (pitches kept) and returns
// their indices. A monophonic lane first drops the notes they overlap.
std::set<int> pastePianoNotes(ofxOceanodeTimelineLane& lane, const std::vector<ofxOceanodeTimelinePianoNote>& notes, double atBeat);
std::vector<ofxOceanodeTimelinePianoNote>& pianoNoteClipboard();
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
// One level of indentation in the left column, the same at every level: a
// folder's tracks, the rows of an unfolded macro track, a clip editor's
// lanes under their track, an LFO lane's controls under it.
constexpr float kIndentStep = 12.0f;
// Indentation of a clip editor's lane rows under the track.
constexpr float kClipEditorIndent = kIndentStep;
// Added to it while the clip editor is docked under a row that is itself
// tabbed in (a track inside a folder, a row of an unfolded macro track).
inline float clipEditorExtraIndent = 0.0f;
// The header colours of the levels the rows being drawn are inside,
// outermost first (a folder, a macro track, one of its timelines, a track
// for its lanes...). A row's indentation is one strip per level, each in
// that level's header colour: down the left edge, a level's colour runs the
// whole height of what it holds, and shows what every row belongs to.
inline std::vector<ImU32> gutterLevels;
// Fills a row's indentation (`indent` wide, from rowMin down to `bottom`)
// with those strips. The last level takes what is left of the indentation.
inline void drawGutter(ImDrawList* dl, const ImVec2& rowMin, float bottom, float indent,
                       const std::vector<ImU32>& levels = gutterLevels) {
    for(size_t level = 0; level < levels.size(); ++level) {
        const float x1 = rowMin.x + level * kIndentStep;
        const float x2 = level + 1 == levels.size() ? rowMin.x + indent : std::min(rowMin.x + indent, x1 + kIndentStep);
        if(x2 <= x1) break;
        dl->AddRectFilled(ImVec2(x1, rowMin.y), ImVec2(x2, bottom), levels[level]);
    }
}
// The colour of text drawn on `background`: near black on a light one, near
// white on a dark one (by its perceived brightness, leaning to white as the
// rest of the editor is dark).
inline ImU32 readableTextColor(ImU32 background) {
    const float brightness = 0.299f * ((background >> IM_COL32_R_SHIFT) & 0xFF) + 0.587f * ((background >> IM_COL32_G_SHIFT) & 0xFF) +
                             0.114f * ((background >> IM_COL32_B_SHIFT) & 0xFF);
    return brightness >= 150.0f ? IM_COL32(15, 15, 18, 255) : IM_COL32(245, 250, 255, 255);
}
// What a translucent `top` looks like over an opaque `under`.
inline ImU32 blendOver(ImU32 top, ImU32 under) {
    const float alpha = ((top >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    auto channel = [&](int shift) {
        return static_cast<int>(((top >> shift) & 0xFF) * alpha + ((under >> shift) & 0xFF) * (1.0f - alpha));
    };
    return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), 255);
}
// A lane's header: its track's header colour (the innermost level the lane
// is inside), 25% darker.
inline ImU32 laneHeaderColor(const ofColor& trackColor) {
    const ImU32 track = gutterLevels.empty() ? mutedTrackColor(trackColor, 0.27f, 0.42f) : gutterLevels.back();
    auto channel = [&](int shift) { return static_cast<int>(((track >> shift) & 0xFF) * 0.75f); };
    return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), 255);
}
// The levels (and the indentation that goes with them) of the lanes drawn
// while it lives.
struct GutterScope {
    std::vector<ImU32> previousLevels;
    float previousIndent;
    GutterScope(std::vector<ImU32> levels, float extraIndent)
    : previousLevels(std::move(gutterLevels)), previousIndent(clipEditorExtraIndent) {
        gutterLevels = std::move(levels);
        clipEditorExtraIndent = extraIndent;
    }
    ~GutterScope() {
        gutterLevels = std::move(previousLevels);
        clipEditorExtraIndent = previousIndent;
    }
};
void drawDivision(ofxOceanodeTimelineLane& lane);
}
