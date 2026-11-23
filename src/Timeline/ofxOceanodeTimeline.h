#ifndef ofxOceanodeTimeline_h
#define ofxOceanodeTimeline_h
#ifndef OFX_OCEANODE_HAS_TIMELINE
#define OFX_OCEANODE_HAS_TIMELINE 1
#endif

#include "ofMain.h"
#include "ofxOceanodeTransport.h"
#include "ofxOceanodeScheduling.h"
#include "ofxOceanodeTimelineMetronome.h"
#include "Nodes/MacroMixerMixing.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeAbstractParameter;
class ofxOceanodeContainer;
class ofxOceanodeNode;
class ofxOceanodeNodeModel;
class ofxOceanodeNodeMacro;
class MacroMixerRuntime;
namespace MacroMixerGui { struct Action; struct State; }
struct ofxOceanodeTimelineInstrument;

// How a binding's per-frame value should combine with whatever other
// bindings already write to the same parameter. Replace is the classic
// single-source case; the others let several lanes/tracks drive one
// parameter together (e.g. a piano-roll gate multiplying a level curve).
// The base contributor's own mode is irrelevant (nothing precedes it to
// combine with) -- each later contributor's mode decides how it blends
// onto the running result, the same way layer blend modes work.
enum class ofxOceanodeTimelineAutomationMode {
    Replace,
    Add,
    Multiply,
    Min,
    Max
};

enum class ofxOceanodeTimelineLaneType {
    Step,
    PianoRoll,
    Curve,
    // The four below mirror four older, standalone ofxOceanodeNodeModel
    // "track" nodes (ofxSantiNodes' valueTrack/stepValueTrack/gateTrack,
    // ofxOceanodeSuperCollider's waveTrack) that each had their own
    // ppqTimeline/transportTrack-based timeline, predating this one. They
    // live here as lane types instead so they share this timeline's clip
    // (position/stretch/repeat), track, blend-mode and grouping machinery
    // rather than duplicating it a fourth and fifth time.
    MultiValue,
    MultiSlider,
    MultiGate,
    Wave,
    Number,
    // Sequences a dropdown parameter: like Number, each point holds an
    // option index until the next one, but the editor shows and picks the
    // bound parameter's option names (see ofxOceanodeTimelineDropdownOptions).
    Dropdown,
    // A drum kit step sequencer: one row per kit piece (a component of the
    // instrument's Gate/Pitch vectors), see ofxOceanodeTimelineDrumRow. Uses
    // the piano roll's role bindings (pianoGateBindingId / pianoPitchBindingId).
    DrumGrid,
    // An oscillator: its result (0..1, mapped through valueMin/valueMax)
    // drives the lane's targets like a Curve does. Its shape is set by the
    // control sublanes in lfoControls (frequency, roundness, skew...), each a
    // Curve-like envelope over the clip's source beats.
    Lfo,
    // Sequences a colour parameter: each point is a colour key (its value an
    // index into the lane's colors), and each segment's tension is the
    // sigmoid the colour blends along into the next key.
    Color,
    // Several curves on one lane, one per component of a bound vector
    // parameter (curve i -> component i), edited one at a time through tabs
    // like ofxSantiNodes' sigmoidCurve. Each is a Curve-like sublane in
    // multiCurves; mode, range, clamp and snap are the lane's own.
    MultiCurve
};

constexpr int kOfxOceanodeTimelineMaxMultiCurves = 16;
// The most normalizeWaveClip boosts a quiet clip: x1000 (+60 dB).
constexpr float kWaveNormalizeGainMax = 1000.0f;
// The id a Multi Curve lane's curve is known by (the editor's selection).
inline std::string ofxOceanodeTimelineMultiCurveKey(const std::string& laneId, int index) {
    return laneId + "#curve" + std::to_string(index);
}

inline bool ofxOceanodeTimelineIsLfoLane(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::Lfo;
}

// The key an LFO control sublane is addressed by (getLane, editor folding
// and heights): its LFO lane's id and the control it sets. Controls have no
// ids of their own, so copying an LFO lane never leaves stale ones behind.
inline std::string ofxOceanodeTimelineLfoControlKey(const std::string& lfoLaneId, const std::string& parameter) {
    return lfoLaneId + "#" + parameter;
}

// Lane types that drive an instrument through the note roles (pianoPitch /
// pianoGate / pianoVelocity binding ids) instead of plain bindingIds.
inline bool ofxOceanodeTimelineLaneUsesNoteRoles(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::PianoRoll || type == ofxOceanodeTimelineLaneType::DrumGrid;
}

// Number and Dropdown lanes hold absolute values in curvePoints, each until
// the next point -- no normalization, no interpolation. A Color lane's points
// are kept the same way (their values are indices, never interpolated: the
// colours they stand for are, see ofxOceanodeTimelineColorAt), so cutting,
// reversing and merging a clip treat all three alike.
inline bool ofxOceanodeTimelineIsHeldValueLane(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::Number || type == ofxOceanodeTimelineLaneType::Dropdown ||
           type == ofxOceanodeTimelineLaneType::Color;
}

// A Color lane's colour at a source beat: the key at or before it, blended
// into the next along that segment's sigmoid (curveTensions; the default
// tension is a straight blend). False when the lane has no key.
struct ofxOceanodeTimelineLane;
bool ofxOceanodeTimelineColorAt(const ofxOceanodeTimelineLane& lane, double beat, ofFloatColor& color);
// The colour a Color lane's point stands for (white past the lane's colours).
ofFloatColor ofxOceanodeTimelineColorOfPoint(const ofxOceanodeTimelineLane& lane, float value);
// A colour as an automation value: "r,g,b,a", 0..1, or 0..255 for an ofColor.
std::string ofxOceanodeTimelineColorText(const ofFloatColor& color, bool bytes);
inline bool ofxOceanodeTimelineIsColorType(const std::string& valueType) {
    return valueType == typeid(ofFloatColor).name() || valueType == typeid(ofColor).name();
}

// The option names of a scalar int/float dropdown parameter, empty for any
// other parameter.
std::vector<std::string> ofxOceanodeTimelineDropdownOptions(ofxOceanodeAbstractParameter& parameter);
// A chord bank's gate input (ofxOceanodeTimelineInstrument::chords, HarmonyBox):
// true, with what the node reports (its chords: voices, rowNames, rowColors).
// A Multi Gate lane bound to one is a Chords lane, one row per chord.
bool ofxOceanodeTimelineChordInstrument(ofxOceanodeAbstractParameter* parameter,
                                        ofxOceanodeTimelineInstrument* instrument = nullptr);

// Determines what stays fixed when the manual transport tempo changes.
// Beats is the traditional musical timeline: beat positions are unchanged,
// so their duration in seconds follows the tempo. Time rescales global beat
// positions so the same material remains at the same absolute time.
enum class ofxOceanodeTimelineTimebase {
    Beats,
    Time
};

// Explicit right-edge operations, shared by numeric editing and mouse drags.
enum class ofxOceanodeTimelineEndEdit { Repeat, Trim, Stretch };
enum class ofxOceanodeTimelineTargetRole { Lane, Pitch, Gate, Velocity, Duration, Filter, Stutter };

// A read-only explanation of the same evaluation used by playback. Shared
// piano lanes appear as one voice-pool contribution with all source lane IDs.
struct ofxOceanodeTimelineContribution {
    std::string trackId, bindingId, clipId;
    std::vector<std::string> laneIds;
    std::string value;
    bool winningClip = false;
};

struct ofxOceanodeTimelineTargetInspection {
    std::string parameterPath, value, unclampedValue;
    bool missingTarget = false;
    bool writesValue = false;
    bool clampEnabled = false;
    bool clamped = false;
    std::vector<ofxOceanodeTimelineContribution> contributions;
    struct Binding {
        std::string trackId, bindingId, value;
        ofxOceanodeTimelineAutomationMode mode = ofxOceanodeTimelineAutomationMode::Replace;
        bool bypass = false, liveOverride = false, active = false;
    };
    std::vector<Binding> bindings;
};

struct ofxOceanodeTimelineStep {
    double startBeat = 0.0;
    // A non-positive duration uses the lane's current grid-cell length.
    double durationBeats = 0.0;
    std::string value;
    // Probability that this step fires. A value of 1 is an ordinary step and
    // 0 is a muted step. Keeping it on the step makes the editor genuinely
    // probabilistic instead of treating the whole lane as a single gate.
    float probability = 1.0f;
    // Level a gate step plays at (0..1 of the lane's range); value steps
    // ignore it.
    float velocity = 1.0f;
};

class ofxOceanodeTimelineStepLane {
public:
    std::vector<ofxOceanodeTimelineStep> steps;

    void sortSteps();
    void setStep(double startBeat, const std::string& value, double durationBeats = 0.0,
                float probability = 1.0f);
    bool removeStep(double startBeat, double epsilon = 1e-6);

    ofJson toJson() const;
    void fromJson(const ofJson& json);
};

// Shared by every point list in the timeline, but `value` is in whatever
// units its owner works in, and there are three: a lane's curve holds 0..1
// (mapped through the lane's valueMin/valueMax when applied), the tempo lane
// holds absolute BPM (clamped to bpmMinimum/bpmMaximum), and a Wave Track's
// volume holds absolute gain (0..4). Anything moving points between two of
// those has to convert -- see the wave-volume migration in fromJson.
struct ofxOceanodeTimelineCurvePoint {
    double beat = 0.0;
    float value = 0.0f;
};

// One tension per segment, matching the sigmoidCurve node: inflection moves
// the bend horizontally and steepness controls how abrupt it is.
struct ofxOceanodeTimelineCurveTension {
    float inflection = 0.5f;
    float steepness = 1.0f;
};

// Shared by both automation evaluation (ofxOceanodeTimeline.cpp) and the
// timeline editor (ofxOceanodeTimelineController.cpp) so a curve always
// renders exactly what gets applied to the parameter. The steepness range
// is kept symmetric under reciprocal (0.1 <-> 10) so the "logarithmic" and
// "exponential" ends of LogExp bend by comparable amounts instead of one
// side collapsing into a near-step curve before the other looks bent at all.
namespace ofxOceanodeTimelineCurve {
    enum class CurveInterpolationMode {
        Step,
        Linear,
        LogExp,
        Sigmoid
    };

    CurveInterpolationMode curveInterpolationMode(const std::string& name);
    float sigmoidFlex(float x, float inflection, float steepness);
    float curveSegmentShape(float x, CurveInterpolationMode interpolation,
                            const ofxOceanodeTimelineCurveTension& tension);
    // The value a point list holds at one beat, shaped by the matching
    // per-segment tension. Everything that reads a curve goes through this
    // -- automation, a Wave track's volume, an LFO control and the editor's
    // own drawing -- so none of them can drift from what is applied.
    // Points must be sorted by beat; fallback answers an empty list.
    float valueAtBeat(const std::vector<ofxOceanodeTimelineCurvePoint>& points,
                      const std::vector<ofxOceanodeTimelineCurveTension>& tensions,
                      const std::string& interpolation, double beat, float fallback = 0.0f);
    // The segment shape that best reproduces the curve between two beats.
    // When a point cuts a segment in two, each part gets its piece of the
    // original bend instead of a straight line. Neutral for Step/Linear and
    // for flat stretches. Points must be sorted by beat.
    ofxOceanodeTimelineCurveTension fitSegmentShape(const std::vector<ofxOceanodeTimelineCurvePoint>& points,
                                                    const std::vector<ofxOceanodeTimelineCurveTension>& tensions,
                                                    const std::string& interpolation, double fromBeat, double toBeat);
}

struct ofxOceanodeTimelinePianoNote {
    double startBeat = 0.0;
    double durationBeats = 0.25;
    int pitch = 60;
    float velocity = 1.0f;
    float probability = 1.0f;
    // Chance (0..1) that the note plays changed in a repeat, by its lane's
    // mutation rules. Rolled every repeat and every time the clip starts.
    float mutation = 0.0f;
    // Which of the lane's mutation groups' rules it follows: 0 = A (the
    // lane's own rules), 1..3 = mutationGroups[group - 1].
    int mutationGroup = 0;
};

// What changes in a piano note whose mutation roll passes. Each strength
// (0..1) is the chance that aspect changes; 0 turns it off.
struct ofxOceanodeTimelineMutationRules {
    float pitchStrength = 1.0f;
    int pitchRange = 7;          // up to this many semitones...
    int pitchDirection = 0;      // ...both ways (0), up only (1) or down only (2)...
    int scaleMask = 0xFFF;       // ...onto these pitch classes (bit 0 = C)
    float offsetStrength = 0.0f;
    int offsetSteps = 1;         // moved up to this many grid steps...
    int offsetQuantum = 1;       // ...in multiples of this many steps...
    int offsetDirection = 0;     // ...earlier or later (0), later only (1) or earlier only (2)
    float lengthStrength = 0.0f;
    int lengthSteps = 2;         // longer or shorter by up to this many grid steps...
    int lengthQuantum = 1;       // ...in multiples of this many steps...
    int lengthDirection = 0;     // ...either (0), longer only (1) or shorter only (2)
    float velocityStrength = 0.0f;
    float velocityRange = 0.25f; // velocity moves up to this much (0..1)...
    int velocityDirection = 0;   // ...up or down (0), up only (1) or down only (2)
    // Fields, for a group's overrides.
    enum Field { PitchStrength = 1, PitchRange = 2, ScaleMask = 4, OffsetStrength = 8,
                 OffsetSteps = 16, LengthStrength = 32, LengthSteps = 64, PitchDirection = 128,
                 VelocityStrength = 256, VelocityRange = 512, OffsetDirection = 1024,
                 VelocityDirection = 2048, OffsetQuantum = 4096, LengthQuantum = 8192,
                 LengthDirection = 16384 };
    bool active() const {
        return (pitchStrength > 0.0f && pitchRange > 0 && (scaleMask & 0xFFF) != 0) ||
               (offsetStrength > 0.0f && offsetSteps > 0) || (lengthStrength > 0.0f && lengthSteps > 0) ||
               (velocityStrength > 0.0f && velocityRange > 0.0f);
    }
};

// Mutation groups B..D: the lane's rules (group A) with only the fields in
// overrides replaced, so a group keeps following A everywhere else.
struct ofxOceanodeTimelineMutationGroup {
    ofxOceanodeTimelineMutationRules rules;
    int overrides = 0; // ofxOceanodeTimelineMutationRules::Field bits
};
constexpr int kMaxMutationGroups = 4; // A plus three

// One block on a MultiValue lane's row -- see ofxOceanodeTimelineLane::
// multiValueRows. Modeled on ValueRegion from the old standalone "Multi
// Value Track" node (ofxSantiNodes/valueTrack.h): an arbitrary (not
// normalized) float value held for [startBeat, startBeat+durationBeats).
// Both beats live in the clip's SOURCE-beat space, same as
// ofxOceanodeTimelineCurvePoint::beat and ofxOceanodeTimelineStep::
// startBeat -- clip stretch/repeat is handled once, generically, by
// ofxOceanodeTimelineClipTime, not re-implemented per lane type.
struct ofxOceanodeTimelineValueRegion {
    double startBeat = 0.0;
    double durationBeats = 1.0;
    float value = 0.0f;
    double end() const { return startBeat + durationBeats; }
};

// One block on a MultiGate lane's row -- see ofxOceanodeTimelineLane::
// multiGateRows. Modeled on GateRegion from the old standalone "Multi Gate
// Track" node (ofxSantiNodes/gateTrack.h): on for
// [startBeat, startBeat+durationBeats), off elsewhere. Same source-beat-
// space note as ofxOceanodeTimelineValueRegion above.
struct ofxOceanodeTimelineGateRegion {
    double startBeat = 0.0;
    double durationBeats = 1.0;
    float velocity = 1.0f; // level while on (0..1)
    double end() const { return startBeat + durationBeats; }
};
// Joins blocks of one row that overlap (blocks that only touch stay apart:
// back to back they retrigger). A joined block keeps the earliest block's
// velocity. The row comes back sorted by start.
void ofxOceanodeTimelineMergeOverlappingGates(std::vector<ofxOceanodeTimelineGateRegion>& row);

// Drum Grid: one hit placed in a kit piece's row. It has a position in time,
// not a grid index: the editing grid only decides where new hits snap, so a
// hit placed on a 1/32 keeps its place when the grid becomes 1/16.
struct ofxOceanodeTimelineDrumStep {
    double beat = 0.0;        // position from the start of its row's pattern, beats
    double length = 0.25;     // the span it occupies, beats (a ratchet divides it)
    float gate = 0.5f;        // fraction of its span (of each ratchet sub-span) the
                              // gate stays open, 0.05..1. Open time = length x gate / ratchet.
    float velocity = 0.8f;    // 0..1, written as the gate level
    int pitch = 0;            // semitones from the piece's own tuning (-24..24)
    float probability = 1.0f; // 0..1, rolled per pattern repeat
    int ratchet = 1;          // 1..8 evenly spaced sub-hits inside its span
    float duration = 1.0f;    // 0.1..24: multiplier of the hit's decays (the
                              // instrument's Duration input; 1 = as set)
    float stutter = 0.0f;     // 0..127: the instrument's Stutter input, a retrigger rate as
                              // a MIDI note while the gate is open; 0 = off
    float filter = 0.0f;      // -1..1: the instrument's Filter input, -1 low-pass fully
                              // closed .. 0 open .. +1 high-pass fully closed
};

// Drum Grid: one kit piece. Its hits repeat every `length` beats (rows of
// different lengths make a polymeter), restarting at every clip repeat.
struct ofxOceanodeTimelineDrumRow {
    int slot = 0;             // gate/pitch/duration vector component (kit slot)
    std::string name;
    // Pattern length in beats (1/24 .. 64). Hits at or past it are kept but
    // silent ("hidden"): shortening a row and lengthening it back restores them.
    double length = 4.0;
    float gate = 0.5f;        // gate of new hits placed in the row (0.05..1)
    bool mute = false;
    bool solo = false;
    std::vector<ofxOceanodeTimelineDrumStep> steps; // sorted by beat
};

// One sounding hit of a drum row, in the clip's source beats.
struct ofxOceanodeTimelineDrumHit {
    double start = 0.0;       // where it is (its pitch and duration apply from here)
    double open = 0.0;        // where its gate opens (later than start by the retrigger gap
                              // when the hit before it was still open)
    double close = 0.0;       // where its gate closes
    float velocity = 0.0f;
    int pitch = 60;
    float duration = 1.0f;
    float filter = 0.0f;
    float stutter = 0.0f;
    int step = 0;             // index of the placed hit in its row
};

struct ofxOceanodeTimelineLane;
namespace ofxOceanodeTimelineDrumGrid {
    constexpr int kMaxRows = 32;
    constexpr int kMaxHitsPerRow = 512;
    constexpr int kMaxRatchet = 8;
    constexpr float kMaxDuration = 24.0f;   // step duration multiplier range: 0.1..24
    constexpr double kMinLength = 1.0 / 24.0, kMaxLength = 64.0; // row length, beats
    // Pitch the instrument plays a piece at its own tuning.
    constexpr int kBasePitch = 60;
    // Clamps every field into range and sorts the hits by position.
    void normalizeRow(ofxOceanodeTimelineDrumRow& row);
    // count rows on slots 0..count-1 named from names ("Row n" past them).
    std::vector<ofxOceanodeTimelineDrumRow> defaultRows(int count, const std::vector<std::string>& names, double lengthBeats = 4.0);
    // Muted rows and, when some row is soloed, the others are silent.
    bool rowAudible(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row);
    // Swing: a hit on an odd multiple of the lane's drumSwingBeats is delayed
    // by drumSwing x drumSwingBeats. Hits off that grid are not swung.
    double swingOffset(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumStep& step);
    // Deterministic probability roll of one hit in one pattern cycle.
    bool probabilityPasses(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumStep& step, int slot,
                           int64_t cycle, uint64_t epoch = 0);
    // The gate and pitch of a row at a source beat of clip repeat `repeat`.
    // contentLength: the clip's source length (patterns restart there).
    // gap: retrigger gap in source beats. heldPitch keeps the last played
    // hit's pitch after its gate closes; before a row's first hit it holds
    // its last placed hit's values (60 / x1 with no hits). heldDuration
    // (optional) is held the same way. Returns true while the gate is open;
    // gate is then its velocity.
    bool rowStateAt(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
                    double sourceBeat, int64_t repeat, double contentLength, double gap,
                    uint64_t epoch, float& gate, int& heldPitch, float* heldDuration = nullptr,
                    float* heldFilter = nullptr, float* heldStutter = nullptr);
    // Every hit of a row starting in [fromBeat, toBeat] of one repeat (for the
    // scheduler's change beats and the editor), open/close resolved as played.
    void rowHits(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
                 double fromBeat, double toBeat, int64_t repeat, double contentLength, double gap,
                 uint64_t epoch, std::vector<ofxOceanodeTimelineDrumHit>& out);
    // The editing grid of a lane: the master grid when it follows it (1/16
    // while the master is None), else its own beatsPerStep. Playback never
    // depends on it.
    double effectiveStepBeats(const ofxOceanodeTimelineLane& lane, double masterGridBeats);
    // Index of the hit whose position lies in [fromBeat, toBeat) of a row's
    // pattern (the one nearest preferBeat), -1 for none.
    int stepInRange(const ofxOceanodeTimelineDrumRow& row, double fromBeat, double toBeat, double preferBeat);
    // Adds a hit (kept sorted) and returns its index.
    int addStep(ofxOceanodeTimelineDrumRow& row, const ofxOceanodeTimelineDrumStep& step);
    // A hit at or past its row's length is hidden: silent, kept for later.
    inline bool stepHidden(const ofxOceanodeTimelineDrumRow& row, const ofxOceanodeTimelineDrumStep& step) {
        return step.beat >= row.length - 1e-9;
    }
    // The colour a row is drawn in: its slot's colour from the instrument
    // (ofxOceanodeTimelineInstrument::rowColors), else fallback.
    ofColor rowColor(const std::vector<ofColor>& colors, const ofxOceanodeTimelineDrumRow& row, const ofColor& fallback);
}

struct ofxOceanodeTimelineParameterBinding {
    std::string id;
    std::string parameterPath;
    std::string valueType;
    std::string defaultValue;
    ofxOceanodeTimelineAutomationMode mode = ofxOceanodeTimelineAutomationMode::Replace;
    ofxOceanodeTimelineLaneType laneType = ofxOceanodeTimelineLaneType::Curve; // default clip type for new parameters
    bool bypass = false;
    bool missingTarget = false;
    float rowHeight = 28.0f; // timeline row height in pixels (resizable from the row's bottom edge)
    // A live override takes priority over whatever clip automation computes
    // for this binding's parameter, for as long as it's set. Used by the
    // piano roll's keyboard strip so clicking a key sounds a note
    // immediately regardless of playhead/clip content, without touching any
    // clip data. Never serialized -- it's momentary UI interaction state.
    bool hasLiveOverride = false;
    std::string liveOverrideValue;
    // When several bindings (different clips/lanes/tracks, combined via
    // Add/Multiply/Min/Max) drive the same parameter, their combined result
    // can land outside the parameter's own min/max -- nothing about Add or
    // Multiply keeps the sum/product inside range the way a single Replace
    // binding naturally does. This clamps the final combined value back to
    // the parameter's min/max before it's applied. Only the binding that
    // ends up "authoritative" for its parameter (see defaultBinding in
    // applyAutomation -- the first non-bypassed binding sharing that
    // parameterPath, the same one whose valueType/defaultValue already act
    // as the shared source of truth today) is consulted; the flag still
    // lives per-binding, matching valueType/defaultValue, rather than
    // needing a separate per-parameter table.
    bool clampToParameterRange = true;
    // From the parameter's node (ofxOceanodeNodeModel::getTimelineValueScale):
    // "dB" = a gain; its Curve lanes default to a decibel scale. "" = linear.
    std::string valueScale;
};

// A lane is an automation editor inside a clip. It may drive more than one
// parameter, which is useful for piano-roll style editors (pitch/gate/velocity)
// and for reusing one curve as the source of several parameters.
struct ofxOceanodeTimelineLane {
    std::string id;
    std::string name;
    ofxOceanodeTimelineLaneType type = ofxOceanodeTimelineLaneType::Step;
    std::vector<std::string> bindingIds;
    // Targets of its track taken off this lane's Targets list (the list's
    // "-"): neither driven nor listed here, until this lane is given them
    // again. The other lanes of the track keep them.
    std::vector<std::string> hiddenBindingIds;
    bool muted = false;
    bool solo = false;
    // Dropdown lanes: the bound parameter's option names as the editor last
    // saw them (not saved), to follow its points when the list changes.
    std::vector<std::string> dropdownOptionsSeen;
    // Color lanes: the colours its points index (a point's value). Entries no
    // point uses any more are harmless.
    std::vector<ofFloatColor> colors;
    // Independent per-start roll, multiplied with the parent clip's chance.
    float chance = 1.0f;
    // Toggle lanes: also drive their own track's solo -- on while a block
    // plays, off between blocks; outside the clip the solo holds.
    bool drivesTrackSolo = false;
    // Gate lanes (Toggle, Multi Gate): the chance (0..1) that each block
    // reaches a target, by parameter path, rolled per block, row and repeat;
    // missing = always.
    std::map<std::string, float> targetProbability;
    ofxOceanodeTimelineStepLane step;
    int stepCount = 16;
    double beatsPerStep = 0.25;
    float valueMin = 0.0f;
    float valueMax = 1.0f;
    bool probabilityEnabled = true;
    // Probability: roll the gate, Always: ignore probability, Mute: no output.
    std::string behavior = "Probability";
    // Seed of the probability rolls (steps, notes): each lane gets its own at
    // creation, so lanes don't roll together; change it for another pattern.
    int probabilitySeed = 0;
    int pianoLowPitch = 36;
    int pianoHighPitch = 84;
    bool pianoSnapToGrid = true;
    // Sound the note being drawn, grabbed or moved in the piano roll.
    bool pianoLiveEditSound = true;
    std::string pianoPitchBindingId;
    std::string pianoGateBindingId;
    std::string pianoVelocityBindingId;
    // Drum Grid: optional per-slot decay multiplier target (the instrument's
    // Duration), written like the pitch.
    std::string drumDurationBindingId;
    // Drum Grid: optional per-slot bipolar filter target (the instrument's
    // Filter), written like the duration (0 = open before any hit).
    std::string drumFilterBindingId;
    // Drum Grid: optional per-slot stutter target (the instrument's Stutter),
    // written like the pitch (0 = off before any hit).
    std::string drumStutterBindingId;
    float pianoDefaultVelocity = 0.8f;
    bool pianoMonophonic = false;
    // Piano mutation: group A's rules, then groups B..D (see
    // ofxOceanodeTimelineMutationGroup and ofxOceanodeTimelineMutationRulesFor).
    ofxOceanodeTimelineMutationRules mutation;
    std::vector<ofxOceanodeTimelineMutationGroup> mutationGroups;
    // Mutated timing never makes notes overlap (see mutatedPianoNotes).
    bool mutationMonophonic = false;
    // Drift (0..1): the chance each note mutates from what it played in the
    // previous repeat instead of as written, so changes accumulate while
    // playing; stopping returns to the written notes. 0 = off.
    float mutationDrift = 0.0f;
    // Strength (0..1): every note's mutation chance is scaled by it; 0 turns
    // the lane's mutation off without touching the notes, 1 = as set.
    float mutationStrength = 1.0f;
    // Scale overlay (editing aid): keys outside the scale are shaded in the
    // roll; with snap, drawn and moved notes land on scale keys.
    bool pianoScaleEnabled = false;
    int pianoScaleMask = 0xAB5;  // bit 0 = C
    bool pianoScaleSnap = false;
    // Random deviation (Rndm tab), rolled for every note in every repeat:
    // with the octave chance a note moves 1..range octaves, with the pitch
    // chance 1..range semitones (onto scale keys when snapping to the scale),
    // both ways (0), up only (1) or down only (2). 0 = off.
    float randomOctave = 0.0f;
    int randomOctaveRange = 1;
    int randomOctaveDirection = 0;
    float randomPitch = 0.0f;
    int randomPitchRange = 2;
    int randomPitchDirection = 0;
    bool randomDeviates() const {
        return (randomOctave > 0.0f && randomOctaveRange > 0) || (randomPitch > 0.0f && randomPitchRange > 0);
    }
    bool curveClamp = true;
    std::string curveInterpolation = "Linear";
    // Curve value scale: "" = Auto (the binding's valueScale), "Linear", "dB".
    // dB: the 0..1 point values are a decibel axis, from silence (0) through
    // ofxOceanodeTimelineScale::kDbFloor up to valueMax's level (1).
    std::string curveScale;
    // Number reuses the point list with absolute numeric values and holds
    // each value until the next point (no normalization or interpolation).
    std::vector<ofxOceanodeTimelineCurvePoint> curvePoints;
    std::vector<ofxOceanodeTimelineCurveTension> curveTensions;
    std::vector<ofxOceanodeTimelinePianoNote> pianoNotes;

    // MultiValue / MultiGate: N independent rows, one component of a bound
    // vector-typed parameter per row (row i -> component i of whatever
    // vector<float>/vector<int>/vector<bool> parameter this lane's
    // bindingIds point at). See ofxOceanodeTimelineValueRegion/GateRegion
    // above. A lane only ever populates whichever of these two vectors
    // matches its own type; multiRowCount is shared by both so switching a
    // lane between MultiValue and MultiGate keeps the row count (not the
    // block data itself, which is type-specific).
    int multiRowCount = 4;
    std::vector<std::vector<ofxOceanodeTimelineValueRegion>> multiValueRows;
    std::vector<std::vector<ofxOceanodeTimelineGateRegion>> multiGateRows;
    // MultiValue: when true, a block's value is rounded to the nearest
    // integer everywhere it's entered or displayed (drag, the numeric edit
    // popup, and the label drawn on the block) -- purely a display/editing
    // convenience toggle on the lane, independent of whatever type the
    // bound vector parameter itself is (a vector<float> parameter still
    // happily accepts "3.0, 5.0" strings).
    bool multiValueInteger = false;
    // MultiSlider ("step value"): a DENSE per-cell value grid, one entry
    // per step (sized to stepCount, reusing this lane's own stepCount/
    // beatsPerStep grid rather than duplicating them) -- unlike the Step
    // lane's sparse `step.steps` (only has an entry where the user placed
    // one, whose height is a fire *probability*), every cell here always
    // holds a value (default 0) and that value IS the output, mapped
    // through valueMin/valueMax. Painted with a continuous vertical-drag
    // stroke, matching the old standalone "Step Value Track" node
    // (ofxSantiNodes/stepValueTrack.h).
    std::vector<float> multiSliderValues;
    // Curve / MultiSlider: optional value-axis "snap to grid" -- when >= 2,
    // the lane's value range (0..1 for Curve, valueMin..valueMax for
    // MultiSlider) is divided into this many evenly-spaced discrete levels,
    // drawn as horizontal guide lines, and any value entered by dragging or
    // painting magnetizes to the nearest one. 0 (the default) means free
    // (no value snapping) -- separate from beatsPerStep, which is the
    // existing *time*-axis grid every lane type already has.
    int valueQuantizeSteps = 0;
    // Curve lanes: snap edited values to valueQuantizeSteps levels. The step
    // count is kept when snapping is switched off.
    bool valueSnap = false;
    // Read-only remnants of the first Wave format, in which audio and its
    // volume envelope were lanes inside a clip. Nothing creates either any
    // more: createLane refuses the Wave type, no selector offers it, and
    // fromJson folds both onto the clip and its track and drops the lane.
    // They are still parsed so old presets keep loading, and are no longer
    // written back out.
    std::string waveFilePath;
    float waveGain = 1.0f;
    bool isWaveVolume = false;
    // LFO lanes: the oscillator's control sublanes, one per control (see
    // kLfoParameters), kept in that order. Each uses ordinary Curve data
    // (curvePoints / curveTensions / curveInterpolation, plus the lfo* fields
    // below) and its own muted / solo / chance, solo scoped to its siblings.
    // Kept when the lane is switched to another type, so switching back
    // restores them; only saved for LFO lanes.
    std::vector<ofxOceanodeTimelineLane> lfoControls;
    // Multi Curve lanes: the curves, in component order. Each holds ordinary
    // Curve data (curvePoints / curveTensions); everything else about them is
    // copied from the lane (ofxOceanodeTimelineSyncMultiCurves). Kept when the
    // lane is switched to another type; only saved for Multi Curve lanes.
    // A curve's `name` is its label (empty: its number).
    std::vector<ofxOceanodeTimelineLane> multiCurves;
    // Multi Curve lanes: the editor shows each curve in a lane of its own,
    // one below the other, instead of one at a time behind tabs.
    bool multiCurveStacked = false;
    // LFO control sublanes: which control this is (kLfoParameters id), so
    // the evaluator and editor do not infer it from the user-facing name.
    std::string lfoParameter;
    // LFO lanes also offer a "Value" mode (curveInterpolation == "Value"): the
    // control holds this constant (normalized 0..1 like curve point values)
    // and the curve points are kept untouched for when another mode is chosen.
    float lfoValue = 0.5f;
    // Frequency lane: point / value edits snap to the musical grid values.
    bool lfoSnap = false;
    // Frequency lane grid: 0 = straight, 1 = triplet, 2 = dotted. Changing it
    // only affects new edits; existing points keep their values.
    int lfoSnapMode = 0;
    // Drum Grid: the kit pieces (see ofxOceanodeTimelineDrumRow). Steps are
    // beatsPerStep long; probability rolls use probabilitySeed.
    std::vector<ofxOceanodeTimelineDrumRow> drumRows;
    // Swing: hits on odd multiples of drumSwingBeats are delayed by
    // drumSwing (0..0.75) of drumSwingBeats.
    float drumSwing = 0.0f;
    double drumSwingBeats = 0.25;
    // Editing grid (where new hits snap, which columns are drawn): the
    // timeline's master grid (true) or beatsPerStep. Playback never depends
    // on it. Saved as "stepFollow" in the lane's drumGrid.
    bool drumStepFollow = true;
};

// Gives an LFO lane's control sublanes the ids derived from the lane's own
// (see ofxOceanodeTimelineLfoControlKey). Called wherever a lane gets a new id.
// A Multi Curve lane's curves take their id and everything but their points
// from the lane: the curve editor and the clip operations read a curve as an
// ordinary Curve lane. Called wherever the lane's settings or id change.
inline void ofxOceanodeTimelineSyncMultiCurves(ofxOceanodeTimelineLane& lane) {
    for(size_t index = 0; index < lane.multiCurves.size(); ++index) {
        auto& curve = lane.multiCurves[index];
        curve.id = ofxOceanodeTimelineMultiCurveKey(lane.id, static_cast<int>(index));
        curve.type = ofxOceanodeTimelineLaneType::Curve;
        curve.valueMin = lane.valueMin;
        curve.valueMax = lane.valueMax;
        curve.curveClamp = lane.curveClamp;
        curve.curveInterpolation = lane.curveInterpolation;
        curve.curveScale = "Linear"; // no decibel axis: one range for every curve
        curve.valueSnap = lane.valueSnap;
        curve.valueQuantizeSteps = lane.valueQuantizeSteps;
        curve.curveTensions.resize(curve.curvePoints.empty() ? 0 : curve.curvePoints.size() - 1);
    }
}

// Grows (each new curve a 0..1 ramp over the content) or shrinks (dropping
// the last curves) a Multi Curve lane's curves.
inline void ofxOceanodeTimelineSetMultiCurveCount(ofxOceanodeTimelineLane& lane, int count, double contentDurationBeats) {
    count = std::max(1, std::min(kOfxOceanodeTimelineMaxMultiCurves, count));
    while(static_cast<int>(lane.multiCurves.size()) < count) {
        ofxOceanodeTimelineLane curve;
        curve.curvePoints = {{0.0, 0.0f}, {std::max(1.0 / 24.0, contentDurationBeats), 1.0f}};
        lane.multiCurves.push_back(std::move(curve));
    }
    lane.multiCurves.resize(static_cast<size_t>(count));
    ofxOceanodeTimelineSyncMultiCurves(lane);
}

inline void ofxOceanodeTimelineSyncLfoControlIds(ofxOceanodeTimelineLane& lane) {
    for(auto& control : lane.lfoControls)
        control.id = ofxOceanodeTimelineLfoControlKey(lane.id, control.lfoParameter);
    ofxOceanodeTimelineSyncMultiCurves(lane);
}

// The rules a note of this mutation group follows (0 = A; a missing group
// falls back to A).
inline ofxOceanodeTimelineMutationRules ofxOceanodeTimelineMutationRulesFor(const ofxOceanodeTimelineLane& lane, int group) {
    if(group <= 0 || group > static_cast<int>(lane.mutationGroups.size())) return lane.mutation;
    using Rules = ofxOceanodeTimelineMutationRules;
    const auto& entry = lane.mutationGroups[group - 1];
    const auto& own = entry.rules;
    Rules rules = lane.mutation;
    if(entry.overrides & Rules::PitchStrength) rules.pitchStrength = own.pitchStrength;
    if(entry.overrides & Rules::PitchRange) rules.pitchRange = own.pitchRange;
    if(entry.overrides & Rules::PitchDirection) rules.pitchDirection = own.pitchDirection;
    if(entry.overrides & Rules::ScaleMask) rules.scaleMask = own.scaleMask;
    if(entry.overrides & Rules::OffsetStrength) rules.offsetStrength = own.offsetStrength;
    if(entry.overrides & Rules::OffsetSteps) rules.offsetSteps = own.offsetSteps;
    if(entry.overrides & Rules::LengthStrength) rules.lengthStrength = own.lengthStrength;
    if(entry.overrides & Rules::LengthSteps) rules.lengthSteps = own.lengthSteps;
    if(entry.overrides & Rules::LengthQuantum) rules.lengthQuantum = own.lengthQuantum;
    if(entry.overrides & Rules::LengthDirection) rules.lengthDirection = own.lengthDirection;
    if(entry.overrides & Rules::VelocityStrength) rules.velocityStrength = own.velocityStrength;
    if(entry.overrides & Rules::VelocityRange) rules.velocityRange = own.velocityRange;
    if(entry.overrides & Rules::OffsetDirection) rules.offsetDirection = own.offsetDirection;
    if(entry.overrides & Rules::VelocityDirection) rules.velocityDirection = own.velocityDirection;
    if(entry.overrides & Rules::OffsetQuantum) rules.offsetQuantum = own.offsetQuantum;
    return rules;
}

// Removes group (1..3); its notes go back to A and later groups move down.
inline void ofxOceanodeTimelineRemoveMutationGroup(ofxOceanodeTimelineLane& lane, int group) {
    if(group <= 0 || group > static_cast<int>(lane.mutationGroups.size())) return;
    lane.mutationGroups.erase(lane.mutationGroups.begin() + (group - 1));
    for(auto& note : lane.pianoNotes) {
        if(note.mutationGroup == group) note.mutationGroup = 0;
        else if(note.mutationGroup > group) --note.mutationGroup;
    }
}

// A Global Track keyframe: the patch's state at one position of its clip.
// Values are the parameters' automation texts (parameterValueForAutomation)
// by path, relative to the sequence's canvas like a binding's path. The map
// is shared between copies (an engine's played copy, undo snapshots) and
// never edited in place: a change makes a new map and a new stamp, so the
// stamp alone tells whether the content changed (see sequenceSignature).
struct ofxOceanodeTimelineGlobalKeyframe {
    double beat = 0.0; // the clip's source beats
    std::shared_ptr<const std::map<std::string, std::string>> values;
    // The easing of the transition from this keyframe to the next: a
    // sigmoid like a Curve lane's segment (neutral: linear). It shapes how
    // far along the way every interpolated parameter is.
    ofxOceanodeTimelineCurveTension ease;
    uint64_t stamp = 0;
    static uint64_t nextStamp() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
    // Counts the loads of keyframes from a file. A preset load sets the
    // parameters to their saved values behind a playing track's back, so
    // every track writes its values again after one.
    static std::atomic<uint64_t>& loadCount() {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
};

// The transition into a clip of a Macro Morphing Track, from the clip it
// overlaps (ofxOceanodeTimelineClip::morphIn): what the MacroMixer node asks
// for when it starts one. Its length is the overlap, its progress the
// playhead's position in it. Kept on the later clip, so it follows that clip
// when the clips are moved.
struct ofxOceanodeTimelineMorphTransition {
    bool morph = false;      // Morph; off: Mix
    float biPow = 0.0f;      // the shape of the progress, -1..1 (the node's Bi Pow; 0: linear)
    float logBlend = 1.0f;   // the amount of log blending of a morph, 0..1 (the node's Log Blend)
    // A Morph on the outgoing source's own graph when the two sources allow
    // it (one graph instead of two); on two graphs otherwise.
    bool inPlace = false;
    // The morph policies of this transition and the mix policy of each
    // router it overrides, as the track's engine reads them (null: the
    // track's defaults). JSON, so the model needs no mixer header.
    ofJson morphPolicies;
    ofJson routerOverrides;
    // Changes whenever the two JSON values do, so a signature hashes the
    // stamp instead of them (see setMorphTransition). Not saved.
    uint64_t stamp = 0;
    static uint64_t nextStamp() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
    bool isDefault() const {
        return !morph && !inPlace && biPow == 0.0f && logBlend == 1.0f && morphPolicies.is_null() && routerOverrides.is_null();
    }
};

struct ofxOceanodeTimelineClip {
    std::string id;
    std::string name;
    double startBeat = 0.0;
    double durationBeats = 4.0;
    // The source length is kept separately so a clip can be stretched without
    // destroying its original content length.
    double contentDurationBeats = 4.0;
    // Timeline beats occupied by one source beat. Keeping this independently
    // from the total clip duration lets a stretched cycle retain its scale
    // when the visible clip edge is later extended to add repetitions.
    double contentStretch = 1.0;
    bool repeatContent = true;
    // Where in its content the clip starts playing, in source beats
    // (0 <= offset < contentDurationBeats): the content plays rotated, from
    // there to its end and on from its start. Automation clips only; Wave
    // clips keep their file position in waveSourceStartBeat.
    double contentOffsetBeats = 0.0;
    // A macro clip (on a macro track) plays its macro's timeline through its
    // own timing: content length = one pass of that timeline, stretch = its
    // tempo ratio, offset = which section plays. The macro snapshot recalled
    // when it starts (-1: none).
    int macroSnapshot = -1;
    // A macro clip playing its macro's timeline backwards (the timeline
    // itself is shared and stays as written). Other clips reverse by
    // rewriting their content (reverseClip); Wave clips use waveReverse.
    bool reversed = false;
    // A clip of a Macro Morphing Track (ofxOceanodeTimelineTrack::isMorphTrack)
    // plays one source of the track's engine: that source's id there. Where
    // it overlaps the clip before it, the two are in transition: morphIn says
    // how. No lanes.
    std::string morphSource;
    ofxOceanodeTimelineMorphTransition morphIn;
    bool muted = false; // Preserve content while removing this clip from playback.
    // Probability (0..1) that the whole clip plays. Rolled again every time
    // the clip starts: each play/seek and each loop pass. The result is not
    // stored here: see ofxOceanodeTimelineClipChanceSilenced().
    float chance = 1.0f;
    // Created by "Capture state as curve points". A track keeps at most one
    // such clip and grows it as captures are made at new playhead positions.
    // This is persisted so the workflow continues after loading a preset.
    bool isStateCapture = false;
    // Audio clips belong to a Wave track. These fields intentionally live on
    // the clip rather than on a lane: a Wave track can contain several
    // independently placed files. Volume is not here -- it belongs to the
    // whole track (gain plus an envelope in timeline-beat space), which is
    // what evaluateWaveClipVolume resolves.
    std::string waveFilePath;
    float waveGain = 1.0f;
    // Set by normalizeWaveClip, apart from waveGain and on top of it: what
    // brings the file's loudest peak to full scale, however quiet it is (up
    // to kWaveNormalizeGainMax). 1 means not normalized.
    float waveNormalizeGain = 1.0f;
    // Base playback multiplier for this audio clip. Stretching the clip
    // changes the effective rate sent to the audio provider, while this
    // value remains the user-facing clip property.
    float wavePlaybackRate = 1.0f;
    // Offset into the full source file, expressed in source beats. A newly
    // loaded clip starts at zero; splitting a clip advances this for the
    // right-hand slice while keeping both clips pointed at the same file.
    double waveSourceStartBeat = 0.0;
    // Full-file duration in the same source-beat domain as
    // waveSourceStartBeat. Keeping this fixed when BPM changes makes slice
    // boundaries stable instead of recomputing the file length from the
    // current transport tempo every frame.
    double waveFileDurationBeats = 0.0;
    bool waveReverse = false;
    // A clip longer or shorter than its sound plays it faster or slower.
    // Off, that is tape speed: the pitch follows. On, the audio backend
    // stretches it in time and keeps its pitch (when it can: see
    // ofxOceanodeTimelineWaveAudioProvider::setWaveClipTimestretch).
    bool waveTimestretch = false;
    // Fades at the clip's two ends, in timeline beats of the visible clip:
    // the volume rises from silence over the first and falls to it over the
    // second (see ofxOceanodeTimelineWaveform::fadeGain).
    double waveFadeInBeats = 0.0;
    double waveFadeOutBeats = 0.0;
    // Each fade's curve: a sigmoid segment, as a Curve lane's (an S by
    // default; inflection 0.5 with steepness 1 is a straight line).
    ofxOceanodeTimelineCurveTension waveFadeInTension{0.5f, 2.0f};
    ofxOceanodeTimelineCurveTension waveFadeOutTension{0.5f, 2.0f};
    // Where this clip starts over an earlier clip of its track that is still
    // sounding, the two crossfade over the overlap: this names the curve
    // (one of ofxOceanodeTimelineWaveform::crossfadeCurves()).
    std::string waveCrossfadeCurve = "Equal power";
    int waveNumChannels = 0;
    double waveFileDurationMs = 0.0;
    // The file's envelope: for each channel, a run of [min, max] pairs over
    // equal slices of the whole file (see ofxOceanodeTimelineWaveform).
    // Rebuilt from the file by reloadWaveform, never saved.
    std::vector<float> waveformPeaks;
    std::vector<ofxOceanodeTimelineLane> lanes;
    // A Global Track's clip: the patch states it interpolates between,
    // sorted by beat. No lanes.
    std::vector<ofxOceanodeTimelineGlobalKeyframe> globalKeyframes;
};

inline bool ofxOceanodeTimelineClipHasLfoLane(const ofxOceanodeTimelineClip& clip) {
    for(const auto& lane : clip.lanes)
        if(ofxOceanodeTimelineIsLfoLane(lane.type)) return true;
    return false;
}

struct ofxOceanodeTimelineTrack;
// A wave clip's envelope cache (ofxOceanodeTimelineClip::waveformPeaks):
// channels x bins x [min, max], each bin an equal slice of the whole file.
namespace ofxOceanodeTimelineWaveform {
    // Bins per channel for a long file; a file with fewer frames gets one per frame.
    constexpr int kMaxBins = 4096;
    // Bins per channel in a cache, 0 when it does not have this layout.
    inline int binCount(const std::vector<float>& peaks, int channels) {
        if(channels <= 0 || peaks.empty() || peaks.size() % (static_cast<size_t>(channels) * 2) != 0) return 0;
        return static_cast<int>(peaks.size() / (static_cast<size_t>(channels) * 2));
    }
    // The lowest and highest sample of a channel between two positions in the
    // file (0..1, from < to). A span wider than a bin takes every bin it
    // touches; a narrower one is interpolated between bins, so a zoomed-in
    // waveform is a smooth shape rather than steps. False when there is no data.
    inline bool range(const std::vector<float>& peaks, int channels, int channel,
                      double from, double to, float& low, float& high) {
        const int bins = binCount(peaks, channels);
        if(bins <= 0 || channel < 0 || channel >= channels || !(to > from)) return false;
        const float* data = peaks.data() + static_cast<size_t>(channel) * bins * 2;
        from = std::max(0.0, std::min(1.0, from));
        to = std::max(0.0, std::min(1.0, to));
        if((to - from) * bins >= 1.0) {
            const int first = std::min(bins - 1, static_cast<int>(from * bins));
            const int last = std::max(first, std::min(bins - 1, static_cast<int>(std::ceil(to * bins)) - 1));
            low = data[first * 2]; high = data[first * 2 + 1];
            for(int bin = first + 1; bin <= last; ++bin) {
                low = std::min(low, data[bin * 2]);
                high = std::max(high, data[bin * 2 + 1]);
            }
            return true;
        }
        // Bin values sit at their bins' centres.
        auto at = [&](double position, float& lo, float& hi) {
            const double p = std::max(0.0, std::min(static_cast<double>(bins - 1), position * bins - 0.5));
            const int a = static_cast<int>(p), b = std::min(bins - 1, a + 1);
            const float t = static_cast<float>(p - a);
            lo = data[a * 2] + (data[b * 2] - data[a * 2]) * t;
            hi = data[a * 2 + 1] + (data[b * 2 + 1] - data[a * 2 + 1]) * t;
        };
        float lowEnd = 0.0f, highEnd = 0.0f;
        at(from, low, high);
        at(to, lowEnd, highEnd);
        low = std::min(low, lowEnd);
        high = std::max(high, highEnd);
        return true;
    }
    // The slice of its file a clip plays, as the audio pass maps it: where it
    // starts and how much it spans, in fractions of the file. bpm: for a
    // clip whose file length in beats is not known yet. False when empty.
    bool slice(const ofxOceanodeTimelineClip& clip, float bpm, double& start, double& span);
    // The loudest sample (0..1 for a file within full scale) of that slice,
    // in any channel, before the clip's gain.
    float peak(const ofxOceanodeTimelineClip& clip, float bpm);
    // A clip's fade lengths as they apply: inside the clip, never overlapping.
    inline void fades(const ofxOceanodeTimelineClip& clip, double& in, double& out) {
        const double length = std::max(0.0, clip.durationBeats);
        in = std::max(0.0, std::min(clip.waveFadeInBeats, length));
        out = std::max(0.0, std::min(clip.waveFadeOutBeats, length - in));
    }
    // The fades a clip plays with on its track. An end that overlaps
    // another clip crossfades with it over the overlap (replacing the clip's
    // own fade there); the other end keeps the clip's own fade.
    struct Fades {
        double in = 0.0, out = 0.0;               // beats, inside the clip, never overlapping
        bool crossIn = false, crossOut = false;   // that end is a crossfade
        std::string inCurve, outCurve;            // the crossfade's curve
        std::string crossOutClipId;               // the clip coming in over this one's end
        ofxOceanodeTimelineCurveTension inTension, outTension;
    };
    // The curves a crossfade can take, the default first.
    const std::vector<std::string>& crossfadeCurves();
    // How far in the incoming clip is (0..1) at a position through a
    // crossfade; the outgoing one is at the same curve read from the end.
    float crossfadeShape(const std::string& curve, float position);
    Fades resolveFades(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineClip& clip);
    // What fades leave of a clip's volume (0..1) at a beat counted from its start.
    float fadeGain(const Fades& fades, const ofxOceanodeTimelineClip& clip, double localBeat);
    // The same with the clip's own fades alone (no neighbours).
    float fadeGain(const ofxOceanodeTimelineClip& clip, double localBeat);
}

// Canonical conversion between source beats (the data stored in a clip) and
// timeline beats (where the clip is displayed and played). Keeping this in
// the model prevents the controller and evaluator from developing subtly
// different stretch/repeat behaviour.
namespace ofxOceanodeTimelineClipTime {
    double sourceDuration(const ofxOceanodeTimelineClip& clip);
    double stretch(const ofxOceanodeTimelineClip& clip);
    // One-shots fit their source to the visible length, even when an external
    // producer has not synchronized the stored contentStretch field.
    double effectiveStretch(const ofxOceanodeTimelineClip& clip);
    double cycleDuration(const ofxOceanodeTimelineClip& clip);
    double sourceToTimelineBeat(const ofxOceanodeTimelineClip& clip,
                                double sourceBeat, int64_t cycle = 0);
    double timelineToSourceBeat(const ofxOceanodeTimelineClip& clip,
                                double timelineBeat);
    int64_t cycleIndex(const ofxOceanodeTimelineClip& clip,
                       double timelineBeat);
    // contentOffsetBeats kept inside [0, source duration).
    double contentOffset(const ofxOceanodeTimelineClip& clip);
}

// Where a sequence's beats sit on the main timeline: sequence beat b plays at
// offset + b * ratio. The main timeline plays through the identity map; a
// macro clip gives its macro's sequence the map of each of its repeats
// (matching ofxOceanodeTimelineClipTime). Both
// evaluation walks and the Wave pass convert through it, in one place.
struct ofxOceanodeTimelineTimeMap {
    double offset = 0.0;  // main-timeline beat of sequence beat 0
    double ratio = 1.0;   // main-timeline beats per sequence beat (> 0)
    double toTimeline(double sequenceBeat) const { return offset + sequenceBeat * ratio; }
    double toSequence(double timelineBeat) const { return (timelineBeat - offset) / ratio; }
    // The map of a sequence placed inside this one by `inner`.
    ofxOceanodeTimelineTimeMap then(const ofxOceanodeTimelineTimeMap& inner) const {
        return {toTimeline(inner.offset), ratio * inner.ratio};
    }
};

// What evaluation rolls for: clip chance, step/note/gate probability,
// mutation and drift. Playback state, passed to evaluation and never stored
// on the timeline data, so the scheduler can probe the next loop pass without
// touching what the current frame plays.
struct ofxOceanodeTimelinePlayContext {
    uint64_t generation = 0; // transport generation (play, seek, stop)
    uint64_t pass = 0;       // loop pass
    uint64_t salt = 0;       // per session, so a fresh launch rolls anew
    // The copy of a sequence being evaluated; 0 is the main timeline, whose
    // rolls it leaves unchanged.
    uint64_t instanceKey = 0;
    // The seed step/note probability and mutations roll with.
    uint64_t epoch() const;
    ofxOceanodeTimelinePlayContext nextPass() const {
        auto next = *this;
        ++next.pass;
        return next;
    }
};
// Whether a clip's chance leaves it out of this pass of this copy.
bool ofxOceanodeTimelineClipChanceSilenced(const ofxOceanodeTimelineClip& clip,
                                           const ofxOceanodeTimelinePlayContext& context);
bool ofxOceanodeTimelineLaneChanceSilenced(const ofxOceanodeTimelineLane& lane,
                                           const ofxOceanodeTimelinePlayContext& context);
// A Curve lane's normalized value where its clip starts (atEnd false) or
// ends (its last value, before a repeat wraps): what playback chases into
// the gaps around the clip (see collectActiveValues). False for other lanes
// and for a curve without points.
bool ofxOceanodeTimelineCurveEdgeValue(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane& lane,
                                       bool atEnd, float& value);
// Where no clip plays one of a track's targets, the normalized value playback
// chases there (see collectActiveValues), as the editors draw it: before the
// first clip its first value, after a clip (the one ending latest) its last,
// up to the next clip or, after the last, forever (toBeat infinite). From
// audible Curve lanes of unmuted clips only; none for a bypassed target or a
// piano gate's (it rests at 0). Track audibility is the caller's to check.
struct ofxOceanodeTimelineCurveChaseSegment {
    double fromBeat = 0.0, toBeat = 0.0;
    const ofxOceanodeTimelineClip* clip = nullptr; // whose value it holds
    const ofxOceanodeTimelineLane* lane = nullptr;
    float value = 0.0f;
};
std::vector<ofxOceanodeTimelineCurveChaseSegment> ofxOceanodeTimelineCurveChaseSegments(
    const ofxOceanodeTimelineTrack& track, const std::string& bindingId);

// Curve lanes in decibels (a gain parameter): shared by the evaluator and the
// editor so the curve always draws what it applies. Normalized 0 is silence,
// anything above runs from kDbFloor dB to the level of valueMax (its top).
namespace ofxOceanodeTimelineScale {
    constexpr float kDbFloor = -60.0f;
    bool isDecibels(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineParameterBinding* binding);
    float topDb(const ofxOceanodeTimelineLane& lane);
    float normalizedToDb(const ofxOceanodeTimelineLane& lane, float normalized);   // -inf at 0
    float dbToNormalized(const ofxOceanodeTimelineLane& lane, float db);
    float normalizedToGain(const ofxOceanodeTimelineLane& lane, float normalized);
    float gainToNormalized(const ofxOceanodeTimelineLane& lane, float gain);
}

// A track's LFO sampled at a timeline beat (see ofxOceanodeTimelineManager::evaluateLfoTrack):
// the first LFO lane of each of its clips.
struct ofxOceanodeTimelineLfoSample {
    bool active = false;  // a clip with an LFO lane covers this beat
    float value = 0.0f;   // shaped LFO result, normalized 0..1 (as the lane draws it)
    float phase = 0.0f;   // raw 0..1 ramp of the current cycle, before shaping/phase offset
    double cycle = 0.0;   // whole cycles counted from the timeline start, continuous across
                          // clips and repeats; increments exactly when phase wraps
};

namespace ofxOceanodeTimelineLfo {
    // Normalized (0..1, as stored in curve points / lfoValue) <-> real value
    // for an LFO lane. The frequency lane is logarithmic: every halving of the
    // cycle length gets the same vertical space; other lanes are linear.
    float laneValueFromNormalized(const ofxOceanodeTimelineLane& lane, float normalized);
    float laneNormalizedFromValue(const ofxOceanodeTimelineLane& lane, float value);
    // Musical reference values (cycle length in beats) for the frequency lane:
    // grid lines in the editor and snap targets. mode: 0 straight (powers of
    // two), 1 triplet (x 2/3), 2 dotted (x 3/2), within the frequency range.
    const std::vector<float>& frequencyGridValues(int mode = 0);
    float snapFrequencyValue(float beats, int mode = 0);
    // Label for a grid value, e.g. 1/4, 2T (triplet), 1/2. (dotted)
    std::string frequencyGridLabel(float beats, int mode = 0);

    // Evaluates an LFO lane's complete result in normalized 0..1 space. The
    // shape is intentionally shared by playback and the editor's result view.
    float evaluate(const ofxOceanodeTimelineLane& lfoLane, double sourceBeat,
                   const ofxOceanodeTimelinePlayContext* context = nullptr);
    // Oscillator cycles accumulated from source beat 0 (integrated frequency).
    double cyclesAt(const ofxOceanodeTimelineLane& lfoLane, double sourceBeat,
                    const ofxOceanodeTimelinePlayContext* context = nullptr);
    // One control's real value (e.g. beats for frequency) at a source beat.
    float evaluateParameter(const ofxOceanodeTimelineLane& lfoLane,
                            const std::string& parameter,
                            double sourceBeat, float fallback,
                            const ofxOceanodeTimelinePlayContext* context = nullptr);
    // Whether a control sublane takes part: not muted, and soloed when one
    // of its siblings is.
    bool controlAudible(const ofxOceanodeTimelineLane& lfoLane, const ofxOceanodeTimelineLane& control);
    // Adds any control sublane the lane is missing (a new LFO lane, a lane
    // switched to LFO, a file from before a control existed), each at its
    // control's default over contentDurationBeats, in kLfoParameters order.
    void ensureControls(ofxOceanodeTimelineLane& lfoLane, double contentDurationBeats);
    // The first LFO lane of a clip, nullptr when it has none.
    const ofxOceanodeTimelineLane* firstLane(const ofxOceanodeTimelineClip& clip);
}

// A note group shows the parameters a piano roll drives (pitch, gate,
// velocity) as a single timeline row. The member bindings stay ordinary
// bindings of the track, kept adjacent in its binding order; every piano-roll
// lane that uses a member follows the group's roles.
struct ofxOceanodeTimelineNoteGroup {
    std::string id;
    std::string name;
    std::string pitchBindingId;
    std::string gateBindingId;
    std::string velocityBindingId;
    // A kit's Duration input (Drum Grid lanes only; piano lanes ignore it).
    std::string durationBindingId;
    // The Duration role was set (or cleared) by the user or adopted once from
    // the instrument. While false and empty, the timeline adopts the
    // instrument's Duration as soon as it offers one (groups saved before it
    // existed). Saved as "durationDecided".
    bool durationDecided = false;
    // A kit's Filter input, adopted / decided like the Duration ("filter",
    // "filterDecided").
    std::string filterBindingId;
    bool filterDecided = false;
    // A kit's Stutter input, the same way ("stutter", "stutterDecided").
    std::string stutterBindingId;
    bool stutterDecided = false;
    bool expanded = false;       // show the member rows under the group row
    float rowHeight = 48.0f;     // height of the group row in the timeline
    // Voice slots the piano roll writes to vector targets. 0 = the bound
    // instrument's own count (ofxOceanodeNodeModel::getTimelineInstrument), or
    // as many as the notes overlap when the targets are not an instrument.
    int voices = 0;
    std::vector<std::string> members() const {
        std::vector<std::string> ids;
        for(const auto* id : {&pitchBindingId, &gateBindingId, &velocityBindingId, &durationBindingId, &filterBindingId,
                              &stutterBindingId})
            if(!id->empty()) ids.push_back(*id);
        return ids;
    }
    bool contains(const std::string& bindingId) const {
        return !bindingId.empty() &&
            (pitchBindingId == bindingId || gateBindingId == bindingId || velocityBindingId == bindingId ||
             durationBindingId == bindingId || filterBindingId == bindingId || stutterBindingId == bindingId);
    }
};

// How a piano-roll lane's notes map onto voice slots. A note keeps its slot
// from start to end, so a held note never changes voice when another starts.
// A pure function of the lane's notes (and the voice count), so playback and
// the scheduler's lookahead allocate identically. Indexed like pianoNotes.
struct ofxOceanodeTimelinePianoVoicing {
    int width = 1;                        // slots notes play in
    int outputWidth = 1;                  // values written to a vector target (>= width:
                                          // the instrument's voices; the rest stay closed)
    bool kit = false;                     // slot = key - 36 (a drum kit's pads)
    bool instrument = false;              // plays an instrument node: its gate is a level (the velocity)
    std::vector<int> slot;                // -1: plays nowhere (a kit key out of range)
    std::vector<double> end;              // source beat it stops (earlier when its slot is stolen)
    std::vector<uint8_t> retrigger;       // its slot was still busy at its start: gate opens after a gap
    std::vector<uint8_t> wrapRetrigger;   // same, against the previous repeat of the clip
    std::vector<std::vector<size_t>> slotNotes; // per slot, its notes in start order
    bool chance = false;                  // a note may not sound (probability < 1): the
                                          // slots then depend on the repeat being played
};

// A marker of a Marker Track (ofxOceanodeTimelineTrack::isMarkerTrack): a
// moment of the timeline with a title, a description and the colour of the
// section it starts (up to the next marker). Not the ruler's markers
// (ofxOceanodeTimelineMarker, the global transport's). Its number, the one
// Go To Marker takes, is its place in time order (0 the earliest): not
// stored, it follows the markers as they move.
struct ofxOceanodeTimelineTrackMarker {
    int uid = 0;                 // stable within its track (edits find it by this)
    double beat = 0.0;           // timeline beats
    std::string title;           // short, drawn on the track
    std::string description;     // long text
    ofColor color = ofColor(210, 80, 80, 255); // every new marker's, until changed
};

struct ofxOceanodeTimelineTrack {
    std::string id;
    std::string name;
    ofColor color = ofColor(65, 165, 245, 255);
    bool collapsed = false;
    // The height of its row under an unfolded macro track, dragged from that
    // row's bottom edge (0: the default one).
    float unfoldedRowHeight = 0.0f;
    // The height of its own row in the Clips view, dragged from that row's
    // bottom edge (0: the view's default, ofxOceanodeTimelineViewState::trackHeight).
    // A Wave track uses waveTrackHeight instead.
    float trackHeight = 0.0f;
    bool muted = false;
    bool solo = false;
    std::vector<ofxOceanodeTimelineParameterBinding> bindings;
    std::vector<ofxOceanodeTimelineClip> clips;
    std::vector<ofxOceanodeTimelineNoteGroup> noteGroups;
    // A Wave Track is created via ofxOceanodeTimelineManager::createWaveTrack
    // and never gets any bindings -- it isn't automation for a parameter at
    // all, just audio clips (each carrying its own file) arranged on the
    // timeline. Kept as a plain flag on an ordinary track, rather than a
    // parallel data structure, so it still gets every other track feature
    // (color, collapse, clip grouping, JSON persistence) for free; the
    // controller uses this flag to render it as a single always-visible row
    // (there's nothing to expand into per-binding rows) and to skip the
    // usual parameter-picking step when creating a clip on it.
    bool isWaveTrack = false;
    // A Global Track (createGlobalTrack) has no bindings either: its clips
    // hold keyframes of the whole patch (ofxOceanodeTimelineGlobalKeyframe)
    // and playback interpolates every parameter that differs between them.
    // A parameter another track automates, or one with an incoming
    // connection, is left alone.
    bool isGlobalTrack = false;
    // Height of the single Wave Track row in the timeline. Not persisted:
    // editor layout (lane heights, collapsed lanes, zoom, snap) is session
    // state everywhere else in the timeline, and this was the one exception.
    float waveTrackHeight = 96.0f;
    // Track-wide volume controls. The automation points live in global
    // timeline-beat space and affect every clip on this Wave Track.
    float waveVolume = 1.0f;
    bool waveVolumeAutomationEnabled = true;
    // Draws the clips' waveforms through the track's volume curve too (their
    // gain and fades always show).
    bool waveVolumeShapesWaveform = false;
    // Sigmoid: each segment bends by its tension (a straight line when
    // neutral, which is what "Linear" in older files becomes).
    std::string waveVolumeInterpolation = "Sigmoid";
    std::vector<ofxOceanodeTimelineCurvePoint> waveVolumePoints;
    std::vector<ofxOceanodeTimelineCurveTension> waveVolumeTensions;
    // Latency correction, milliseconds: shifts when this track's scheduled
    // events reach their backend (positive later, negative earlier). Only
    // sample-accurate targets move; frame-rate parameters cannot be shifted.
    float latencyOffsetMs = 0.0f;
    // Folders: a folder is a track with no bindings or clips that holds the
    // tracks whose parentId is its id (folders nest). Its mute, solo and
    // latency apply to everything inside it. The sequence keeps each folder
    // followed by its contents (see ofxOceanodeTimelineSequence::normalizeFolders).
    bool isFolder = false;
    std::string parentId; // empty: at the top level
    // Macro track: its clips play the timeline of this macro (the canvas id
    // of the macro's container, relative to the sequence's own container:
    // absolute on the main timeline). Empty: an ordinary track. A macro
    // track has no bindings, and its clips no lanes.
    // "." is the sequence's own canvas (another timeline of it). Which of
    // that canvas's timelines it plays: macroTimeline ("" its default one).
    std::string macroCanvas;
    std::string macroTimeline;
    bool isMacroTrack() const { return !macroCanvas.empty(); }
    // Gating: the macro is switched on (its Active) only around this track's
    // clips: preRollMs before one starts (at least the macro's earliest
    // negative latency) until tailMs after it ends, so its nodes start up
    // before the first events and its gates and releases finish after the
    // rest values went out.
    bool macroGate = false;
    // Unfolded: the editor shows the macro's tracks as rows under this one,
    // with its clips where this track's clips play them, editable in place.
    bool macroUnfolded = false;
    // Unfolded, each timeline its clips play (the one placed, then those of
    // the same macro they carry) has a row of its own with its tracks under
    // it: the ids of the timelines whose tracks are folded away again.
    std::vector<std::string> macroFoldedTimelines;
    float macroPreRollMs = 100.0f;
    float macroTailMs = 500.0f;
    // Macro Morphing Track (createMorphTrack): the MacroMixer node laid out
    // in time. Each clip plays one source of the track's engine
    // (ofxOceanodeTimelineClip::morphSource) and the overlap of two
    // neighbouring clips is the transition between them. Not a macro track
    // (macroCanvas stays empty); no bindings, and its clips no lanes. A
    // timeline has at most one. It unfolds with macroUnfolded and
    // macroFoldedTimelines, as a macro track does.
    bool isMorphTrack = false;
    // Its engine's state (routers, sources, kept-out nodes, default
    // policies) as the engine saves it. Opaque here.
    ofJson morphState;
    // Changes whenever morphState does, so a signature hashes the stamp
    // instead of the state (see setMorphState). Not saved.
    uint64_t morphStateStamp = 0;
    // How long before a clip starts its source is loaded (0: when the
    // playhead reaches it, which can stall that frame). Loaded ahead, the
    // source runs silent until its clip starts.
    float morphPreloadSeconds = 0.0f;
    // Unfolded, the sources that are not playing at the playhead are rows
    // under an "Other tracks" folder row: folded away, all of them at once.
    bool morphOthersFolded = false;
    // Marker Track (createMarkerTrack): no bindings and no clips, only
    // markers in timeline beats, kept sorted by beat (a marker's index is
    // its number). A timeline has at most one; a Timeline node pointing at
    // it publishes Go To Marker.
    bool isMarkerTrack = false;
    std::vector<ofxOceanodeTimelineTrackMarker> trackMarkers;
};
// Two neighbouring clips of a Macro Morphing Track that overlap: the
// transition from the earlier one into the later one, over these beats.
struct ofxOceanodeTimelineMorphOverlap {
    std::string earlierClipId;
    std::string laterClipId;
    double fromBeat = 0.0;  // the later clip's start
    double toBeat = 0.0;    // the earlier clip's end
};
// A source of a Macro Morphing Track that is not playing at the playhead:
// its timelines as they are kept (read from its folder, with the edits made
// since), to show, not to edit: they belong to no canvas until it plays.
class ofxOceanodeTimelineSequence;
struct ofxOceanodeTimelineMorphIdleSource {
    std::string id, name;
    std::vector<const ofxOceanodeTimelineSequence*> timelines;
};
// Something a Macro Morphing Track's clip has to say about its source or
// the transition into it, as the MacroMixer's Details do: severe (red: the
// source fails, or the whole graph falls back) or not (orange).
struct ofxOceanodeTimelineMorphWarning {
    bool severe = false;
    std::string text;
};
// What a Macro Morphing Track holds at a beat: no clip (a gap: both ids
// empty), one clip (the origin alone), or two in transition, with how far
// through their overlap the beat is (0 at its start, towards 1 at its end:
// linear, before the transition's Bi Pow).
struct ofxOceanodeTimelineMorphScene {
    std::string originClipId;
    std::string targetClipId;
    float position = 0.0f;
};
constexpr float kMaxMacroGateMs = 5000.0f;
constexpr float kMaxTrackLatencyOffsetMs = 250.0f;

// A lightweight cross-track grouping: it holds no automation data of its
// own, just references to existing clips by (trackId, clipId). Each member
// clip still belongs to its own track and serializes exactly as before --
// the group is purely coordination metadata layered on top, so the
// controller can fan a move/stretch drag on any one member out to the rest.
// (This is "option A" from the multi-lane clip design notes: a first-class
// track-independent clip would be the "real" version of a combined clip,
// but is a much bigger restructure than what grouping already-existing
// clips needs.)
// A named position on the timeline (a cue): shown on the ruler, jumped to
// by clicking it.
struct ofxOceanodeTimelineMarker {
    std::string id;
    std::string name;
    double beat = 0.0;
};

struct ofxOceanodeTimelineClipGroup {
    std::string id;
    // (trackId, clipId) pairs, in the order they were grouped -- not
    // necessarily timeline order.
    std::vector<std::pair<std::string, std::string>> members;
};

constexpr float kMinTrackHeight = 40.0f;
constexpr float kMaxTrackHeight = 240.0f;

// Editor viewport state belongs to the preset just like the timeline data:
// switching presets should restore both the horizontal scale and the place
// the user was looking at. Keeping it on the manager also means savePreset()
// always sees the latest values even though the editor is owned by a separate
// controller.
struct ofxOceanodeTimelineViewState {
    float pixelsPerSecond = 140.0f;
    float scrollX = 0.0f;
    // Pinned left column width (track pads / lane properties), in pixels.
    float labelWidth = 280.0f;
    // Default height of a track row in the Clips view: a track whose bottom
    // edge was dragged has its own (ofxOceanodeTimelineTrack::trackHeight),
    // and so does every Wave track.
    float trackHeight = kMinTrackHeight;
};

// Optional, addon-agnostic hook that lets a project which also includes
// ofxOceanodeSuperCollider drive REAL audio playback for Wave tracks,
// without ofxOceanodeTimeline itself ever depending on SuperCollider types
// -- this header and its .cpp include nothing SC-specific, and never will.
// The conditional-compilation boundary the user asked for (Wave audio only
// available when ofxOceanodeSuperCollider is in the project) lives entirely
// on the implementer's side: a small bridge class inside
// ofxOceanodeSuperCollider, gated by its own build flag, implements this
// interface and registers itself via setWaveAudioProvider below. With no
// provider registered, wave clips are visual-only -- the waveform displays
// and the clip can be positioned/stretched like any other clip, but
// nothing actually plays.
class ofxOceanodeTimelineWaveAudioProvider {
public:
    virtual ~ofxOceanodeTimelineWaveAudioProvider() = default;
    // Called before each updateWaveClip: whether that clip keeps its pitch
    // when it plays at another speed than its file's (see
    // ofxOceanodeTimelineClip::waveTimestretch). A backend that cannot
    // ignores it and plays the clip at tape speed.
    virtual void setWaveClipTimestretch(const std::string& trackId, const std::string& clipId, bool timestretch) {}
    // Called once per frame for every clip on every Wave track. inactive is
    // sent too, which lets a backend stop voices as the playhead leaves a
    // clip without needing a separate begin/end-frame callback.
    virtual void updateWaveClip(const std::string& trackId, const std::string& clipId,
                                const std::string& filePath, float gain, float trackVolume,
                                int numChannels, double clipContentStartBeat, double contentDurationBeats,
                                double sourceStartBeat, double sourceFileDurationBeats,
                                float playbackRate, bool reverse, float bpm, bool isPlaying, bool active,
                                bool forceTransportSync) = 0;
    virtual void releaseWaveClip(const std::string& trackId, const std::string& clipId) = 0;
};

// A note tool another addon gives the piano roll (ofxOceanodeSuperCollider's
// arpeggiator, built on its ArpeggiatorBox engine): the roll's properties get
// a tab that opens it in a window of its own, floating or docked. Like the
// wave audio provider above, the timeline knows nothing about the tool: it
// lends the window's contents the lane and its selection, and the tool writes
// ordinary notes. With none registered the piano roll has no such tab.
struct ofxOceanodeTimelinePianoToolContext {
    ofxOceanodeTimelineLane& lane;      // the piano roll the window works on
    std::set<int>& selection;           // its selected notes (indices into lane.pianoNotes);
                                        // the tool leaves the notes it wrote selected
    double contentBeats;                // the length of the clip's content (the notes' beats)
};
struct ofxOceanodeTimelinePianoTool {
    std::string name;                   // the window's title ("Arpeggiator")
    std::string tab;                    // the tab's label ("Arp")
    std::string description;            // what it does, a line or two
    std::function<void(ofxOceanodeTimelinePianoToolContext&)> draw;   // the window's contents (ImGui)
};
inline std::vector<ofxOceanodeTimelinePianoTool>& ofxOceanodeTimelinePianoTools() {
    static std::vector<ofxOceanodeTimelinePianoTool> tools;
    return tools;
}
// (Registering a name again replaces that tool.)
inline void ofxOceanodeTimelineRegisterPianoTool(ofxOceanodeTimelinePianoTool tool) {
    auto& tools = ofxOceanodeTimelinePianoTools();
    for(auto& existing : tools) if(existing.name == tool.name) { existing = std::move(tool); return; }
    tools.push_back(std::move(tool));
}

// Finds parameters by timeline path relative to the container a sequence
// lives in: the root for the main timeline (absolute paths, as saved in
// every file), a macro's container for that macro's sequence -- so one
// shared sequence reaches each copy's own parameters.
// A macro timeline's target outside its macro: a parameter of a canvas above
// it. Its path starts with one "../" for each canvas up, then goes on as that
// canvas's own timelines would keep it ("../../Node/Param"). Such a target is
// never saved with the macro: the canvas the parameter lives in keeps it,
// under the Macro node the way down starts from (see outsideTargetsJson).
namespace ofxOceanodeTimelineOutside {
    // How many canvases up the path starts (0: a target inside the macro).
    size_t levels(const std::string& path);
    inline bool isOutside(const std::string& path) { return path.compare(0, 3, "../") == 0; }
    // A path from the root as a timeline of `canvas` keeps it: inside the
    // canvas the usual way, anywhere else with its "../".
    std::string relativePath(const std::string& canvas, const std::string& absolutePath);
    // And back: a path a timeline of `canvas` keeps, from the root.
    std::string absolutePath(const std::string& canvas, const std::string& path);
    // A canvas id's macros, from the root down (none for the root).
    std::vector<std::string> canvasLevels(const std::string& canvas);
    std::string joinLevels(const std::vector<std::string>& levels, size_t from, size_t to);
}

struct ofxOceanodeTimelineResolver {
    ofxOceanodeContainer* base = nullptr;
    // The patch's root: where a target outside base is looked up from.
    ofxOceanodeContainer* root = nullptr;
    ofxOceanodeAbstractParameter* find(const std::string& path) const;
    // Empty when the parameter is not inside base: what a sequence drives
    // inside the container it belongs to.
    std::string pathOf(ofxOceanodeAbstractParameter& parameter) const;
    // The "../" path of a parameter outside base (empty: inside it, or
    // base is the root).
    std::string outsidePathOf(ofxOceanodeAbstractParameter& parameter) const;
};

// How a timeline plays: its name, clock source (free, or linked to the
// global transport), tempo (a fixed BPM, a ratio of the clock's, or a BPM
// lane: used while it plays free), loop, meter and launch quantize. A
// timeline linked to the global transport takes its tempo lane and meter
// from the manager (they are the global ones); its loop is its own, like
// every timeline's: the global transport never loops, the timeline wraps
// that transport's position in its loop itself.
struct ofxOceanodeTimelinePlaySettings {
    std::string name;                // empty: named after its canvas
    // Free: its own transport and tempo. Off: linked to the global transport
    // (its position, play state and tempo; the loop stays the timeline's
    // own). Neither applies while the timeline is placed as clips in
    // another: it then depends on that one.
    bool free = false;
    float bpm = 120.0f;
    bool followClockTempo = false;   // tempo = the clock's x tempoRatio
    float tempoRatio = 1.0f;
    bool bpmAutomationEnabled = false;
    float bpmMinimum = 20.0f;
    float bpmMaximum = 300.0f;
    std::string bpmInterpolation = "Linear";
    std::vector<ofxOceanodeTimelineCurvePoint> bpmPoints;
    std::vector<ofxOceanodeTimelineCurveTension> bpmTensions;
    bool loopEnabled = false;
    double loopStartBeat = 0.0;
    double loopEndBeat = 4.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    int quantize = 2;                // play/stop on: 0 now, 1 the next beat, 2 the next bar
    // Playing on its own: starts from its beginning once a preset (the
    // project's or its macro's) loads, and whenever its macro turns active;
    // waits while the macro (or one around it) is inactive, stops when it is.
    bool playOnLoad = false;
    // The colour its window's scope bar is tinted with; off: its kind's colour.
    bool customColor = false;
    ofColor color = ofColor(160, 100, 210);
    // A macro canvas's timeline: nodes inside the macro (Time Source Auto)
    // follow the clip playing it (beat, stretched tempo, restarting with each
    // clip) instead of the canvas around it. At most one timeline of a
    // canvas has it. Saved with the macro.
    bool nodesFollowClips = false;
    // Its BPM lane in the editor (view state, left out of the comparison).
    bool bpmLaneVisible = false;
    bool bpmLaneCollapsed = true;
    bool operator==(const ofxOceanodeTimelinePlaySettings& other) const;
    bool operator!=(const ofxOceanodeTimelinePlaySettings& other) const { return !(*this == other); }
};

// The arrangement of one timeline: its tracks (with their bindings, clips and
// lanes), clip groups and the counters new ids start from. Every timeline of
// every canvas is one; an engine plays a copy of one. Data and plain lookups
// only: parameters, playback, scheduling and undo stay with the manager.
class ofxOceanodeTimelineSequence {
public:
    std::vector<ofxOceanodeTimelineTrack> tracks;
    std::vector<ofxOceanodeTimelineClipGroup> clipGroups;
    // The container its binding paths are relative to (not saved; nullptr:
    // the root canvas, absolute paths).
    ofxOceanodeContainer* base = nullptr;
    // Set when the timeline is read from a file (a preset, a Macro's or a
    // MacroMixer source's folder), later loads higher; 0: made here. Not
    // saved. The editor fits its view to a timeline it has not shown since
    // that load.
    uint64_t loadStamp = 0;
    static uint64_t nextLoadStamp() { static uint64_t stamp = 0; return ++stamp; }
    // First number tried for each kind of new id.
    uint64_t nextTrackNumber = 1;
    uint64_t nextBindingNumber = 1;
    uint64_t nextClipNumber = 1;
    uint64_t nextLaneNumber = 1;
    uint64_t nextGroupNumber = 1;
    // Its name, clock source, tempo, loop, meter and the rest of its settings.
    ofxOceanodeTimelinePlaySettings play;
    // Its id within its canvas ("tl_N"; empty for a copy or a scratch sequence).
    std::string timelineId;

    // Linear lookups by id (nullptr when missing).
    ofxOceanodeTimelineTrack* findTrack(const std::string& trackId);
    const ofxOceanodeTimelineTrack* findTrack(const std::string& trackId) const;
    ofxOceanodeTimelineClip* findClip(const std::string& trackId, const std::string& clipId);
    const ofxOceanodeTimelineClip* findClip(const std::string& trackId, const std::string& clipId) const;
    ofxOceanodeTimelineLane* findLane(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    const ofxOceanodeTimelineLane* findLane(const std::string& trackId, const std::string& clipId,
                                            const std::string& laneId) const;
    ofxOceanodeTimelineParameterBinding* findBinding(const std::string& trackId, const std::string& bindingId);
    const ofxOceanodeTimelineParameterBinding* findBinding(const std::string& trackId,
                                                           const std::string& bindingId) const;

    // "prefix_number", the form of every timeline id.
    static std::string makeId(const char* prefix, uint64_t number);
    // Ids not used anywhere in this sequence, from the counters on.
    std::string makeUniqueTrackId() const;
    std::string makeUniqueBindingId() const;
    std::string makeUniqueClipId() const;
    std::string makeUniqueLaneId() const;
    std::string makeUniqueGroupId() const;
    // The requested name, or it with the first free " 2", " 3"... suffix.
    std::string makeUniqueTrackName(const std::string& requestedName) const;
    static std::string makeUniqueClipName(const ofxOceanodeTimelineTrack& track, const std::string& requestedName);

    // ---- Folders ----
    // The folder a track is in (nullptr at the top level).
    const ofxOceanodeTimelineTrack* parentOf(const ofxOceanodeTimelineTrack& track) const;
    // How many folders a track is inside.
    int depthOf(const ofxOceanodeTimelineTrack& track) const;
    // True when trackId is folderId or anywhere inside it.
    bool isWithin(const std::string& trackId, const std::string& folderId) const;
    // Index one past the last track inside the track at `index` (its whole subtree).
    size_t subtreeEnd(size_t index) const;
    // Clears parents that are missing, not folders, or would make a cycle,
    // and reorders tracks so each folder is followed by its contents, keeping
    // the relative order of siblings. A list without folders is unchanged.
    void normalizeFolders();
};

// One canvas's timelines (the root canvas's, or a macro's): any number, none
// special, in the order they were made. A canvas may have none.
struct ofxOceanodeTimelineCanvas {
    std::vector<std::shared_ptr<ofxOceanodeTimelineSequence>> timelines;
    // First number tried for a new timeline's id (saved: a deleted
    // timeline's id is never given to a new one, so tracks, windows and
    // time sources that still name it stay unbound).
    uint64_t nextTimelineNumber = 1;
    // The container the timelines' paths are relative to (nullptr: the root).
    ofxOceanodeContainer* base = nullptr;
    // The targets outside their macro of the macro timelines below this
    // canvas whose parameters live in it, as its file had them (see
    // ofxOceanodeTimelineManager::outsideTargetsJson): given to those
    // timelines once this canvas has loaded.
    ofJson outsideTargets = ofJson::array();
    ofxOceanodeTimelineSequence* find(const std::string& timelineId) const {
        for(const auto& timeline : timelines) if(timeline->timelineId == timelineId) return timeline.get();
        return nullptr;
    }
};

class ofxOceanodeTimelineManager {
public:
    // A timeline is named by its canvas (the canvas id as the core reports
    // it, "" for the root) and its id within the canvas ("tl_N"). An empty
    // id names no timeline ({} is "none").
    struct TimelineRef {
        std::string canvas;
        std::string timeline;
        bool operator==(const TimelineRef& other) const { return canvas == other.canvas && timeline == other.timeline; }
        bool operator!=(const TimelineRef& other) const { return !(*this == other); }
        bool operator<(const TimelineRef& other) const {
            return canvas != other.canvas ? canvas < other.canvas : timeline < other.timeline;
        }
    };
    // Editing operations come in pairs: one takes the Sequence to edit (a
    // timeline as kept here), the other is an engine's, on the copy it plays
    // (playedSequence). The primary keeps every canvas's timelines, the
    // global transport's settings, the players and the undo histories, and
    // plays nothing itself.
    explicit ofxOceanodeTimelineManager(ofxOceanodeContainer* container = nullptr);

    void setContainer(ofxOceanodeContainer* container);
    // Applies automation for the current frame. Equivalent to calling
    // evaluateAutomation() followed by applyAutomation().
    void update();
    // Recomputes this frame's active lane values, resolves the loop/BPM
    // automation, and applies live piano-roll pitch ranges. Call once per
    // frame, before node updates.
    void evaluateAutomation();
    // Parameter lookups by path are cached between these two calls (while
    // no node can be added or deleted). Nothing is kept across frames.
    void beginParameterCache();
    void endParameterCache();
    // Re-applies the values computed by the last evaluateAutomation() call,
    // without recomputing them. Intended to be called again after node
    // updates, so a node that writes back to its own parameter during
    // update() doesn't silently override automation for that frame -
    // without paying for a second full evaluation pass.
    void applyAutomation();
    // See ofxOceanodeTimelineParameterBinding::hasLiveOverride. setLiveOverride
    // takes effect starting with the next evaluateAutomation()/applyAutomation()
    // pass; the caller is responsible for calling clearLiveOverride once the
    // interaction ends (e.g. on mouse release) -- there's no timeout.
    // Live overrides are playback state: they live in the engine playing
    // the timeline (the sequence overloads find it; nothing happens while
    // the timeline has no engine yet) and are kept across the engine's
    // copy rebuilds. The id-only overloads are the engine's own, on its copy.
    void setLiveOverride(const std::string& trackId, const std::string& bindingId, const std::string& value);
    void clearLiveOverride(const std::string& trackId, const std::string& bindingId);
    void setLiveOverride(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId, const std::string& value);
    void clearLiveOverride(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId);
    // The piano roll's keyboard: a key sounds in one voice (a kit: its own
    // pad) through live overrides on the lane's gate and pitch, until cleared.
    void setPianoKeyPreview(const std::string& trackId, const std::string& clipId, const std::string& laneId, int pitch);
    void clearPianoKeyPreview(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    void setPianoKeyPreview(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, int pitch);
    void clearPianoKeyPreview(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId);
    // Several keys at once (dragging a chord): each pitch in its own voice
    // slot, the first one alone on a scalar target.
    void setPianoChordPreview(const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::vector<int>& pitches);
    void setPianoChordPreview(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::vector<int>& pitches);
    void clear();

    static ofColor defaultTrackColor(size_t trackIndex);
    std::string createTrack(const std::string& requestedName = "Timeline Track");
    std::string createTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Timeline Track");
    // A track with no bindings, dedicated to Wave clips (see
    // ofxOceanodeTimelineTrack::isWaveTrack). Reuses createTrack for the
    // usual id/uniqueness/list bookkeeping and just marks the result.
    std::string createWaveTrack(const std::string& requestedName = "Wave Track");
    std::string createWaveTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Wave Track");
    // ---- Global Track (see ofxOceanodeTimelineTrack::isGlobalTrack) ----
    // One per timeline: returns the existing one's id when there is one.
    // It is made with one empty clip, which grows as keyframes are added.
    std::string createGlobalTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Global Track");
    static const ofxOceanodeTimelineTrack* globalTrackOf(const ofxOceanodeTimelineSequence& sequence);
    // Captures every storable parameter of the sequence's canvas (and the
    // macros inside it) at a timeline beat: a new keyframe, or the one
    // already there overwritten. A beat outside the clip grows the clip to
    // it (an unstretched clip only). clipId empty: the clip at the beat, or
    // the nearest one. Returns how many parameters were captured (0: none).
    int addGlobalKeyframe(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::string& clipId, double timelineBeat);
    bool removeGlobalKeyframe(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                               const std::string& clipId, size_t index);
    // The easing from keyframe `index` to the next one (see ofxOceanodeTimelineGlobalKeyframe::ease).
    bool setGlobalKeyframeEase(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                const std::string& clipId, size_t index, const ofxOceanodeTimelineCurveTension& ease);
    // Moves a keyframe to a timeline beat inside its clip; returns its new index (-1: not moved).
    int moveGlobalKeyframe(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                            const std::string& clipId, size_t index, double timelineBeat);
    // Moves several keyframes of a clip together, keeping their spacing:
    // the one at `anchorIndex` goes to a timeline beat as for
    // moveGlobalKeyframe, as far as the first and last of them stay inside
    // the clip's content. Returns their new indexes, in the order of
    // `indexes` (empty: not moved).
    std::vector<int> moveGlobalKeyframes(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                         const std::string& clipId, const std::vector<size_t>& indexes,
                                         size_t anchorIndex, double timelineBeat);
    // Removes several keyframes of a clip at once; returns how many.
    int removeGlobalKeyframes(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                               const std::string& clipId, std::vector<size_t> indexes);
    // Copy and paste of keyframes, between the Global Track clips of any
    // timelines (the clipboard is the manager's: every Timeline window
    // shares it). A copy keeps the keyframes' values, easings and spacing,
    // and takes the place of copied tracks or clips (as those do of it).
    // Returns how many were copied.
    int copyGlobalKeyframes(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                             const std::string& clipId, const std::vector<size_t>& indexes);
    int copiedGlobalKeyframeCount() const { return static_cast<int>(copiedGlobalKeys.size()); }
    // Pastes them into a clip, the earliest at a timeline beat and the
    // others as far after it as they were copied (in the clip's source
    // beats). A keyframe already at one of those places is overwritten. An
    // unstretched clip grows to hold them, as for addGlobalKeyframe; in a
    // stretched, repeated or offset one, those past its content are left
    // out. Returns the pasted keyframes' indexes.
    std::vector<int> pasteGlobalKeyframes(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                          const std::string& clipId, double timelineBeat);
    // ---- Marker Track (see ofxOceanodeTimelineTrack::isMarkerTrack) ----
    // One per timeline: returns the existing one's id when there is one.
    std::string createMarkerTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Markers");
    static ofxOceanodeTimelineTrack* markerTrackOf(ofxOceanodeTimelineSequence& sequence);
    static const ofxOceanodeTimelineTrack* markerTrackOf(const ofxOceanodeTimelineSequence& sequence);
    // A new marker at a timeline beat (the default colour, titled
    // "Marker"); returns its uid (-1: not a marker track).
    int addTrackMarker(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat);
    bool removeTrackMarker(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, int uid);
    // Moves it and keeps the markers in time order; returns its new index (-1: none).
    int moveTrackMarker(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, int uid, double beat);
    bool setTrackMarkerText(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, int uid,
                            const std::string& title, const std::string& description);
    bool setTrackMarkerColor(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, int uid, const ofColor& color);
    // The marker with that uid and its index (number); nullptr: none.
    static const ofxOceanodeTimelineTrackMarker* findTrackMarker(const ofxOceanodeTimelineTrack& track, int uid, int* index = nullptr);
    // The beat of marker number `index` of the timeline's Marker Track (false: none).
    static bool trackMarkerBeat(const ofxOceanodeTimelineSequence& sequence, int index, double& beat);
    // Independent copy immediately below the source, with fresh routing/content IDs.
    std::string duplicateTrack(const std::string& trackId);
    std::string duplicateTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId);
    // Replaces one multi-lane automation clip with independent single-lane tracks.
    std::vector<std::pair<std::string, std::string>> breakClipLanesIntoTracks(
        const std::string& trackId, const std::string& clipId);
    std::vector<std::pair<std::string, std::string>> breakClipLanesIntoTracks(ofxOceanodeTimelineSequence& sequence, 
        const std::string& trackId, const std::string& clipId);
    bool removeTrack(const std::string& trackId);
    bool removeTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId);
    ofxOceanodeTimelineTrack* getTrack(const std::string& trackId);
    ofxOceanodeTimelineTrack* getTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId);
    const ofxOceanodeTimelineTrack* getTrack(const std::string& trackId) const;
    const ofxOceanodeTimelineTrack* getTrack(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId) const;
    // Every timeline as kept here, with its reference (the root canvas's,
    // then each macro's), for callers outside the manager.
    std::vector<std::pair<TimelineRef, ofxOceanodeTimelineSequence*>> allTimelineSequences() const;
    template<typename Fn> void forEachTimeline(Fn&& fn) {
        for(auto& [ref, sequence] : allTimelineSequences()) fn(ref, *sequence);
    }

    // ---- Macro Morphing Track: macro sources in time, overlaps as transitions ----
    // (ofxOceanodeTimelineTrack::isMorphTrack. The model only: the engine
    // that plays the sources keeps its state in the track's morphState.)
    // A timeline has at most one: the id of the new track, empty when the
    // timeline already has one.
    std::string createMorphTrack(const std::string& requestedName = "Macro Morphing");
    std::string createMorphTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Macro Morphing");
    // A timeline's Macro Morphing Track (nullptr: none).
    ofxOceanodeTimelineTrack* morphTrackOf(ofxOceanodeTimelineSequence& sequence);
    const ofxOceanodeTimelineTrack* morphTrackOf(const ofxOceanodeTimelineSequence& sequence) const;
    // A clip playing the source `sourceId` of the track's engine, named
    // after it. The timing is held to the overlap rules below (the nearest
    // valid one); empty when the clip fits nowhere near there.
    std::string createMorphClip(const std::string& trackId, const std::string& sourceId,
                                double startBeat, double durationBeats);
    std::string createMorphClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                const std::string& sourceId, double startBeat, double durationBeats);
    // The same after the track's last clip, overlapping it by overlapSeconds
    // at the tempo there (never more than half of the shorter of the two
    // clips); at beat 0 on an empty track.
    std::string appendMorphClip(const std::string& trackId, const std::string& sourceId,
                                double durationBeats, double overlapSeconds = 4.0);
    std::string appendMorphClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                const std::string& sourceId, double durationBeats, double overlapSeconds = 4.0);
    // The overlap rules of a Macro Morphing Track's clips, which
    // setClipTiming, resizeClipEnd, trimClipStart, createClip, duplicateClip
    // and importClip keep by holding a clip at the nearest valid timing: in
    // start order, a clip overlaps only the clip before it and the one after
    // it (at most two play at any beat), and neither its start nor its end
    // comes within kMorphMinOffsetBeats of a neighbour's (none lies inside
    // another). A move takes a clip past a neighbour when it fits beyond it
    // at its length and that is nearer to where it was asked to go.
    static constexpr double kMorphMinOffsetBeats = 1.0 / 24.0;
    // The transitions of a track: its neighbouring clips that overlap, in
    // start order. Clips that only touch are a hard switch: no entry.
    static std::vector<ofxOceanodeTimelineMorphOverlap> morphOverlaps(const ofxOceanodeTimelineTrack& track);
    // What the track holds at a beat (a clip runs from its start up to, not
    // including, its end). Muted clips count: muting is playback's matter.
    static ofxOceanodeTimelineMorphScene morphSceneAt(const ofxOceanodeTimelineTrack& track, double beat);
    // The track's engine state and a clip's transition are JSON: set through
    // these, their stamps follow (a signature hashes the stamps). False:
    // not a Macro Morphing Track, or no such clip.
    bool setMorphState(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const ofJson& state);
    bool setMorphTransition(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                            const ofxOceanodeTimelineMorphTransition& transition);
    // ---- Macro Morphing Track: the engine playing its sources ----
    // (ofxOceanodeTimelineMorph.cpp. Kept by the primary, one per timeline
    // that has the track; every frame it is told the scene at the timeline's
    // playhead: MacroMixerRuntime::present.)
    // The engine of a timeline's morph track; null without one. Created
    // lazily, with the timeline's canvas container as parent.
    MacroMixerRuntime* morphEngine(const ofxOceanodeTimelineSequence& sequence);
    const MacroMixerRuntime* morphEngine(const ofxOceanodeTimelineSequence& sequence) const;
    // Applies a panel's action to the engine (MacroMixerGui::apply) and
    // stores the engine's state in the track (setMorphState). Clips whose
    // source was removed are removed with it. The caller wraps it in a
    // history gesture. Inside a transition, an edit the engine refuses there
    // releases the playing sources first (they load again on the next frame).
    bool applyMorphAction(ofxOceanodeTimelineSequence& sequence, const MacroMixerGui::Action& action,
                          MacroMixerGui::State& state, std::string& error);
    // Imports a folder as a source (reusing an already imported one) and
    // appends a clip for it with the default 4 s overlap: one pass of the
    // source's first timeline long, 8 bars when it has none. The clip id,
    // or empty with the reason in error.
    std::string addMorphSource(ofxOceanodeTimelineSequence& sequence, const std::string& folder, std::string& error);
    // A source's name (its id while unknown), what the engine has to say
    // (why a scene is not ready, a failure; empty when fine) and whether
    // some Timeline node points at this timeline (its routers reach the patch).
    std::string morphSourceName(const ofxOceanodeTimelineSequence& sequence, const std::string& sourceId) const;
    std::string morphEngineStatus(const ofxOceanodeTimelineSequence& sequence) const;
    bool morphHasPublisher(const ofxOceanodeTimelineSequence& sequence) const;
    // What a clip of the track shows under its name: its source's problems
    // and the issues of the last transition into it (none: all is well).
    std::vector<ofxOceanodeTimelineMorphWarning> morphClipWarnings(const ofxOceanodeTimelineSequence& sequence,
                                                                   const ofxOceanodeTimelineClip& clip) const;
    // For the Timeline nodes pointing at the timeline. The engine's Output
    // routers by router id, read once per frame (a Trigger is consumed by
    // the read), for every node to publish; null without an engine.
    using MorphValues = std::map<std::string, MacroMixerMixing::Value>;
    const MorphValues* morphOutputs(const ofxOceanodeTimelineSequence& sequence) const;
    // Called by such a node on every update: it publishes the routers.
    void noteMorphPublisher(const ofxOceanodeTimelineSequence& sequence);
    // Called by such a node from its draw, with the GL context current: the
    // engine's graphs draw once per frame (the first call), and the outputs
    // are read again (textures are rendered there).
    void drawMorphEngine(const ofxOceanodeTimelineSequence& sequence);
    // Called by such a node on its update, once its inputs are written: the
    // engine updates there (once per frame, the first call), so what the
    // node's inputs hold reaches the sources in the same frame. A timeline
    // no node points at is updated by the frame pass instead.
    void updateMorphEngine(const ofxOceanodeTimelineSequence& sequence);
    // The canvases of every engine's live graphs (one, or the two of a
    // transition), for the host's time system: their phasors and time
    // generators run with the patch's.
    void forEachMorphGraph(const std::function<void(ofxOceanodeContainer&)>& visit);
    // Called on the host's draw, before the nodes': every engine not drawn
    // yet this frame draws (a timeline no Timeline node points at too).
    void drawMorphEngines();
    // Something a Macro Morphing Track refused, said where no editor is at
    // hand (a node's menu): the next editor drawn shows it.
    void reportMorphError(const std::string& message);
    std::string takeMorphError();
    // ---- Macro Morphing Track: its sources' own timelines ----
    // Every graph built from a source's folder plays the same timelines, kept
    // here from the first load on (a source document): edits are heard at
    // once, outlive the graph (rebuilt each time the source comes to play)
    // and reach the folder only with saveMorphSource.
    // The timelines of the track's sources that are live now (the ones its
    // clips place), each with its source's id: the rows the track unfolds.
    std::vector<std::pair<ofxOceanodeTimelineSequence*, std::string>> morphLiveTimelines(
        const ofxOceanodeTimelineSequence& host, const ofxOceanodeTimelineTrack& track);
    // A source's patch, in a canvas tab of its own (the tab of the Macro its
    // graph runs in). Only while the source plays alone: a transition closes
    // it, and it goes with the graph when the source stops playing. What is
    // changed there lasts while the source plays; it is not saved.
    bool morphSourceAlone(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const;
    // Two sources are live on the track (a crossfade, or one loaded ahead).
    bool morphInTransition(const ofxOceanodeTimelineSequence& host) const;
    bool morphSourceCanvasOpen(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const;
    bool openMorphSourceCanvas(const ofxOceanodeTimelineSequence& host, const std::string& sourceId);
    // The source's patch was opened on the graph playing now, so it may have
    // been changed: saveMorphSource then writes the patch too.
    bool morphSourcePatchOpened(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const;
    // ...and was changed there by hand since it was opened or last saved: a
    // value on a node's controls, or a canvas gesture (nodes, cables), the
    // Macros inside it included.
    // (Looked at a few times a second; exact: now.)
    bool morphSourcePatchDirty(const ofxOceanodeTimelineSequence& host, const std::string& sourceId, bool exact = false) const;
    // Every source with changes not written to its folder, by name (its
    // timelines, its patch), for the host's "Save changes?" dialog; and
    // writing them all. A patch can only be written while its source plays alone.
    std::vector<std::string> morphUnsavedSources();
    bool saveMorphUnsavedSources(std::string& error);
    // The track's sources that have clips and are not live now, in the order
    // of their first clips: shown under the live ones, read-only.
    std::vector<ofxOceanodeTimelineMorphIdleSource> morphIdleSources(const ofxOceanodeTimelineSequence& host,
                                                                     const ofxOceanodeTimelineTrack& track);
    // Whether a source's timelines differ from what its folder holds
    // (looked at a few times a second; exact: now).
    bool morphSourceDirty(const ofxOceanodeTimelineSequence& host, const std::string& sourceId, bool exact = false);
    // Writes a source's timelines into its folder (the file they were read
    // from). When its patch was opened on the graph playing now and it plays
    // alone, the patch is written as well, as the preset it is: its nodes as
    // they are now, their cables, and the nodes kept out of the graph as the
    // folder had them. False, with the reason.
    bool saveMorphSource(const ofxOceanodeTimelineSequence& host, const std::string& sourceId, std::string& error);

    // ---- Macro tracks: clips that play a macro canvas's timeline ----
    // (Made by createTimelineTrack below, with the timeline placed.)
    // A clip on a macro track. A content length of 0 uses one pass of what
    // the track plays (macroPassLength).
    std::string createMacroClip(const std::string& trackId, double startBeat, double durationBeats = 0.0);
    std::string createMacroClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                double startBeat, double durationBeats = 0.0);
    // The timeline a timeline track of `host` plays (nullptr: gone).
    ofxOceanodeTimelineSequence* placedTimeline(const ofxOceanodeTimelineSequence& host,
                                                const ofxOceanodeTimelineTrack& track);
    // The timelines a timeline track's clips play: the one it places, then
    // (a macro placed as a whole) the timelines of that macro no track
    // places, carried with it. Empty when the placed one is gone.
    std::vector<ofxOceanodeTimelineSequence*> carriedTimelines(const ofxOceanodeTimelineSequence& host,
                                                               const ofxOceanodeTimelineTrack& track);
    // A track of `host` whose clips play the timeline `placed`: one of the
    // same canvas or of a canvas inside it, never one that (through its own
    // timeline tracks) plays host. Its id, empty when refused.
    std::string createTimelineTrack(ofxOceanodeTimelineSequence& host, const TimelineRef& placed);
    // The name of a track placing a timeline of the macro whose canvas this
    // is: "MT:" and the title the macro's node shows in its header (its
    // local name, or the library Macro's; without one, the canvas's last step).
    std::string macroTrackName(const std::string& canvas) const;
    // Gives the macro tracks of `host` still named that way ("MT:...") the
    // current name of their macro, and their clips named after the track
    // with them. A track renamed by hand is left alone. True: one changed.
    bool syncMacroTrackNames(ofxOceanodeTimelineSequence& host);
    // A timeline clip whose speed was changed (a Stretch edit, Cmd+drag on its
    // right edge): its timeline at its natural speed again, as many repeats
    // as it had (a one-shot fitted to the clip goes back to its own length).
    bool isClipStretched(const ofxOceanodeTimelineClip& clip) const;
    bool restoreClipLength(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    // Whether `from`, through its timeline tracks, plays `target` (or is it).
    bool timelineReaches(const TimelineRef& from, const TimelineRef& target);
    // The live container of the macro a macro track plays (nullptr: gone).
    ofxOceanodeContainer* findMacroContainer(const ofxOceanodeTimelineSequence& sequence,
                                             const ofxOceanodeTimelineTrack& track) const;
    // One pass of a macro timeline: the end of its last clip, at least a bar
    // of its own meter.
    double macroTimelineLength(const ofxOceanodeTimelineSequence& macroSequence) const;
    // One pass of what a timeline track's clips play: the longest of those
    // timelines (carriedTimelines), a bar of the host when there is none.
    double macroPassLength(const ofxOceanodeTimelineSequence& host, const ofxOceanodeTimelineTrack& track);
    // Gives a macro clip that pass as its content length. One lasting a
    // whole number of passes is resized to as many. False: not a macro clip.
    bool fitMacroClipToPass(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    // A timeline's beats per bar: the global meter for the root canvas's
    // default timeline while it is linked to the global transport, else its own.
    double beatsPerBarOf(const ofxOceanodeTimelineSequence& sequence) const;
    // A macro timeline as a reversed macro clip plays it: mirrored over one
    // pass (macroTimelineLength of the original).
    ofxOceanodeTimelineSequence reversedMacroTimeline(const ofxOceanodeTimelineSequence& macroSequence) const;

    // Where the main playhead is in this macro's timeline, through the macro
    // clip playing that macro there (repeat, tempo, offset). False when no
    // clip plays it at the playhead (or its macro track is silent).
    bool macroPlayheadBeat(const ofxOceanodeContainer& macroContainer, double& beat);

    // Plays a clip backwards at the same speed, in place: automation clips
    // get their content mirrored, macro clips play their macro's timeline
    // backwards, Wave clips toggle Reverse.
    bool reverseClip(const std::string& trackId, const std::string& clipId);
    bool reverseClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);

    // ---- Encapsulation (nodes moved into a new macro) ----
    // Paths old -> new, absolute: every timeline (main and each macro's)
    // follows. Returns the bindings changed.
    int retargetAllParameterPaths(const std::map<std::string, std::string>& oldToNew);
    // Nodes of `canvas` were encapsulated into macroContainer's macro. Paths
    // follow; with moveIntoMacro, the tracks of the canvas's timeline that
    // drive only those nodes move into the macro's own timeline, and a macro
    // track with one clip plays it in their place. Returns what undo and
    // redo need (replayParameterMove).
    ofJson encapsulateParameters(ofxOceanodeContainer& canvas, ofxOceanodeContainer& macroContainer,
                                 const std::map<std::string, std::string>& oldToNew, bool moveIntoMacro);
    void replayParameterMove(ofxOceanodeContainer& canvas, ofxOceanodeContainer* macroContainer,
                             const std::map<std::string, std::string>& oldToNew, const ofJson& record, bool backwards);
    // Every parameter any timeline drives.
    void collectBoundParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const;
    // The parameters Global Tracks move: those whose value differs between
    // the keyframes of a clip. Not bindings (they stay hand-editable); a
    // MacroMixer morph asks, to leave them to the track.
    void collectGlobalDrivenParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const;
    // Drops the parameter's bindings from every timeline (a node going away).
    void removeBindingsOf(ofxOceanodeAbstractParameter& parameter);

    // ---- Macro canvases: each macro's timelines ----
    // The timelines of the macro owning this container (an empty list on
    // first use, their paths relative to the macro); nullptr when no macro
    // owns it. Copies of a library macro share one list; a local macro owns
    // its own.
    ofxOceanodeTimelineCanvas* getMacroTimelines(ofxOceanodeContainer& macroContainer);
    // How the editor names the macro: its title, else its canvas id.
    std::string getMacroSequenceLabel(ofxOceanodeContainer& macroContainer) const;
    // The macro whose canvas this is comes from the Macro library (a "global"
    // Macro, not a local one).
    bool macroIsGlobal(ofxOceanodeContainer& macroContainer) const;
    // Writes that Macro back to the library as it is now, its timelines
    // included (what its node's own save does). False: local, or not found.
    bool saveGlobalMacro(ofxOceanodeContainer& macroContainer);
    // Called with the macro's own preset or library folder (sequence.json).
    void loadMacroSequence(ofxOceanodeContainer& macroContainer, const std::string& folder);
    bool saveMacroSequence(const ofxOceanodeContainer& macroContainer, const std::string& folder) const;
    // False once the timeline is gone, or its macro (an editor scope then closes).
    bool isLiveSequence(const ofxOceanodeTimelineSequence& sequence) const;
    // A sequence's own data as JSON (its id, "tracks", "clipGroups",
    // "play"), and back (the sequence keeps its id and canvas).
    ofJson sequenceToJson(const ofxOceanodeTimelineSequence& sequence) const;
    void sequenceFromJson(ofxOceanodeTimelineSequence& sequence, const ofJson& json);
    // Undo per timeline (each keeps its own history), and the global
    // transport's settings (markers, tempo lane, loop, meter...) apart.
    void beginTimelineEdit(const ofxOceanodeTimelineSequence& sequence);
    // True when the edit changed something (an undo step was recorded).
    bool endTimelineEdit(const ofxOceanodeTimelineSequence& sequence);
    bool undoTimelineEdit(ofxOceanodeTimelineSequence& sequence);
    bool redoTimelineEdit(ofxOceanodeTimelineSequence& sequence);
    bool canUndoTimelineEdit(const ofxOceanodeTimelineSequence& sequence) const;
    bool canRedoTimelineEdit(const ofxOceanodeTimelineSequence& sequence) const;
    // A canvas to show in the editor (a macro's timelines, from its node
    // menu), picked up by the editor.
    void requestMacroScope(ofxOceanodeContainer& macroContainer) { pendingMacroScope = &macroContainer; }
    ofxOceanodeContainer* consumeMacroScopeRequest() { auto* requested = pendingMacroScope; pendingMacroScope = nullptr; return requested; }
    // Any timeline to show in the editor (picked up like the above).
    void requestTimelineScope(const TimelineRef& ref) { pendingTimelineScope = ref; timelineScopeRequested = true; }
    bool consumeTimelineScopeRequest(TimelineRef& ref) {
        if(!timelineScopeRequested) return false;
        timelineScopeRequested = false;
        ref = pendingTimelineScope;
        return true;
    }
    // Every new timeline goes through the editor's New Timeline dialog (its
    // name, colour and owner canvas), picked up like the above.
    struct NewTimelineRequest {
        std::string canvas;                  // the owner preselected ("" the root)
        std::vector<std::string> canvases;   // the owners offered; empty: every canvas
        ofxOceanodeAbstractParameter* parameter = nullptr; // put on its first track
        bool globalTrack = false;           // made with a Global Track
        TimelineRef duplicateOf;             // a copy of this one (its canvas only)
        bool duplicate = false;
        int window = 0;                      // the Timeline window to show it in
    };
    void requestNewTimeline(const NewTimelineRequest& request) { pendingNewTimeline = request; newTimelineRequested = true; }
    bool consumeNewTimelineRequest(NewTimelineRequest& request) {
        if(!newTimelineRequested) return false;
        newTimelineRequested = false;
        request = pendingNewTimeline;
        return true;
    }
    // ---- The automation of copied nodes ----
    // Nodes of `canvas` were copied to the clipboard folder: the tracks
    // driving them, of that canvas's timelines and of the ones around it,
    // are saved beside them (clipboardTimelines.json; no track, no file),
    // their paths relative to the canvas, with the preset they come from.
    void saveClipboardAutomation(ofxOceanodeContainer& canvas, const std::vector<ofxOceanodeNode*>& nodes,
                                 const std::string& folder);
    // What a paste into another preset can bring along: one new timeline
    // per copied one, on the canvas pasted into, picked up by the editor,
    // which asks first.
    struct ClipboardPasteRequest {
        std::string canvas;                         // pasted into ("" the root)
        ofJson timelines;                           // [{"name", "sequence"}]
        std::map<std::string, std::string> paths;   // as copied -> as pasted, relative to the canvas
        std::vector<std::string> names;             // the timelines' names
        int tracks = 0;                             // how many tracks they hold
    };
    // The user pasted the clipboard folder's nodes into `canvas` (oldToNew:
    // their parameters' paths as copied -> now, absolute). Asks for the
    // dialog when the folder holds automation copied from another preset.
    void offerClipboardAutomation(ofxOceanodeContainer& canvas, const std::string& folder,
                                  const std::map<std::string, std::string>& oldToNew);
    bool consumeClipboardPasteRequest(ClipboardPasteRequest& request) {
        if(!clipboardPasteRequested) return false;
        clipboardPasteRequested = false;
        request = std::move(pendingClipboardPaste);
        pendingClipboardPaste = {};
        return true;
    }
    // Makes the timelines of a request, driving the pasted nodes only.
    std::vector<TimelineRef> pasteClipboardAutomation(const ClipboardPasteRequest& request);
    // What tells one preset from another: its path, or, never saved, this run.
    static std::string clipboardPresetIdentity();
    // ---- Copied tracks and clips (any timeline to any other) ----
    // A copy by value, shared by every Timeline window: the timeline it came
    // from may be gone by the paste. Paths are kept as the copied timeline
    // has them and rebased onto the canvas pasted into.
    struct TrackClipboard {
        enum class Kind { None, Tracks, Clips };
        Kind kind = Kind::None;
        std::string canvas;  // the copied timeline's canvas ("" the root)
        std::string timeline; // ...and its id (a timeline track may not play the one it is in)
        // Tracks: each copied track (a folder followed by its contents).
        // Clips: the tracks the clips are on (their bindings and note
        // groups), holding only the copied clips.
        std::vector<ofxOceanodeTimelineTrack> tracks;
        std::vector<ofxOceanodeTimelineClipGroup> groups; // among the copied clips
        double firstBeat = 0.0; // Clips: where the earliest one starts
        int count = 0;          // tracks or clips copied
    };
    // Copies tracks (a folder with its contents; never the Macro Morphing
    // Track: its sources belong to its timeline). Returns how many.
    int copyTracks(const ofxOceanodeTimelineSequence& sequence, const std::vector<std::string>& trackIds);
    // Copies clips with their tracks' targets (not a Macro Morphing Track's).
    int copyClips(const ofxOceanodeTimelineSequence& sequence,
                  const std::vector<std::pair<std::string, std::string>>& clips);
    const TrackClipboard& trackClipboard() const { return copied; }
    // Pastes the copied tracks after `afterTrackId` (in its folder; empty:
    // at the end), with their targets, clips, note groups and clip groups.
    // A track that cannot be in that timeline is left out (a second Global
    // Track, a timeline track playing the timeline itself or from another
    // canvas). Returns the new tracks' ids, the top-level ones first.
    std::vector<std::string> pasteTracks(ofxOceanodeTimelineSequence& sequence, const std::string& afterTrackId,
                                         std::string* skipped = nullptr);
    // Pastes the copied clips onto a track from `startBeat`, keeping their
    // spacing; the targets the track lacks are added to it. trackId empty:
    // onto new tracks (one per copied clip's track) at the end. A clip that
    // cannot go on that track (audio onto automation...) is left out.
    // Returns the new clips (track id, clip id).
    std::vector<std::pair<std::string, std::string>> pasteClips(ofxOceanodeTimelineSequence& sequence,
        const std::string& trackId, double startBeat, std::string* skipped = nullptr);
    // A parameter's right-click "Add to Timeline": a new clip driving it on
    // a track of that timeline (a new track when trackId is empty), a new
    // lane driving it in one of the track's clips, or one more target of an
    // existing lane, set up in the editor's dialog, picked up like the above.
    struct ParameterClipRequest {
        enum class Kind { Clip, Lane, Target };
        TimelineRef timeline;
        std::string trackId;        // empty: a new track
        std::string parameterPath;  // as that timeline saves it
        Kind kind = Kind::Clip;
    };
    void requestParameterClip(const ParameterClipRequest& request) { pendingParameterClip = request; parameterClipRequested = true; }
    bool consumeParameterClipRequest(ParameterClipRequest& request) {
        if(!parameterClipRequested) return false;
        parameterClipRequested = false;
        request = pendingParameterClip;
        return true;
    }
    // Whether a request an editor handles (a parameter lane, a track rename,
    // a parameter clip, a piano roll setup) waits, and which timeline it
    // names ({}: none named): the controller picks the window to handle it.
    bool peekEditorRequest(TimelineRef& ref) const {
        if(!pendingParameterLanes.empty()) { ref = pendingParameterLanes.front().ref; return true; }
        if(!pendingTrackRenameId.empty()) { ref = pendingTrackRenameRef; return true; }
        if(parameterClipRequested) { ref = normalizedRef(pendingParameterClip.timeline); return true; }
        if(pendingNoteGroupSetupParameter != nullptr) { ref = pendingNoteGroupSetupRef; return true; }
        return false;
    }
    // Every canvas a timeline can belong to: the root (""), then each
    // macro's, at any depth.
    std::vector<std::string> allCanvases() const;

    // ---- Timelines by reference (TimelineRef), and independent players ----
    // The canvas's container (the root for ""), nullptr when gone.
    ofxOceanodeContainer* canvasContainer(const std::string& canvas) const;
    // A canvas's id as a reference names it ("" for the root).
    std::string canvasIdOf(const ofxOceanodeContainer& canvas) const;
    // A canvas's timelines: the root's, or a macro's (its entry made on first
    // use); nullptr when the canvas is unknown. findCanvasTimelines makes
    // nothing and walks no graph.
    ofxOceanodeTimelineCanvas* canvasTimelines(const std::string& canvas);
    ofxOceanodeTimelineCanvas* findCanvasTimelines(const std::string& canvas) const;
    // The timeline itself; nullptr when its canvas or id is unknown.
    ofxOceanodeTimelineSequence* timelineFor(const TimelineRef& ref);
    // The same, making no canvas entry.
    ofxOceanodeTimelineSequence* findTimeline(const TimelineRef& ref) const;
    bool timelineExists(const TimelineRef& ref) const { return findTimeline(ref) != nullptr; }
    // Where a sequence lives (false for a copy or an unknown one).
    bool refOf(const ofxOceanodeTimelineSequence& sequence, TimelineRef& ref) const;
    TimelineRef refForCanvas(const ofxOceanodeContainer& canvas, const std::string& timeline) const;
    // The name shown for a timeline: its own, else "Timeline N".
    std::string timelineName(const TimelineRef& ref) const;
    // The name a new timeline on a canvas would get.
    std::string suggestTimelineName(const std::string& canvas) const;
    // The time source id a node saves to follow it.
    static std::string timeSourceId(const TimelineRef& ref);
    // Every timeline there is: the root canvas's, then each macro's.
    std::vector<TimelineRef> allTimelines() const;
    // Every timeline of a canvas, in order.
    std::vector<TimelineRef> timelinesOfCanvas(const std::string& canvas);
    // A new timeline on a canvas (its id, "tl_N"), named, or "Timeline N".
    std::string createTimeline(const std::string& canvas, const std::string& name);
    // The same, as a reference ({} when the canvas is unknown).
    TimelineRef addTimeline(const std::string& canvas, const std::string& name);
    // Removes a timeline with its tracks (clips placing it elsewhere then
    // play nothing). Its id is never given to another.
    bool removeTimeline(const TimelineRef& ref);
    bool renameTimeline(const TimelineRef& ref, const std::string& name);
    // Copies a timeline as a new one on the same canvas (its id).
    std::string duplicateTimeline(const TimelineRef& ref, const std::string& name = std::string());

    // Where a timeline's transport comes from. Global: it is slaved to the
    // global transport (the root's: position, play state, tempo and loop),
    // the default. Free: its own transport on the shared clock, started,
    // stopped and seeked by itself, at its own tempo (the Free setting). A
    // timeline placed as clips in another is Dependent whatever its setting:
    // it plays where the clips place it, at that timeline's tempo; its own
    // transport previews it alone (at its host's tempo), and never plays the
    // host. A macro's default timeline playing free also becomes what the
    // macro's canvas plays to, so nodes inside it set to Auto follow it.
    enum class ClockSource { Global, Free, Dependent };
    ClockSource clockSourceOf(const TimelineRef& ref);
    // The same reference however the root canvas was spelled ("" for it).
    static TimelineRef normalizedRef(TimelineRef ref);
    // The Free setting (a dependent timeline keeps it for when it is unplaced).
    bool isFree(const TimelineRef& ref) const;
    void setFree(const TimelineRef& ref, bool free);
    // The first timeline whose timeline tracks place `ref` (false: none).
    bool placedBy(const TimelineRef& ref, TimelineRef& host);
    // The tempo a dependent timeline plays at on its own: its host's now.
    float hostBpmFor(const TimelineRef& ref);
    // Its transport: the root's while it is linked to the global transport,
    // its own when free or dependent; nullptr before its player runs.
    std::shared_ptr<ofxOceanodeTransport> transportFor(const TimelineRef& ref) const;
    // Play, stop (pause, or back to the start) and seek it, at the next
    // beat or bar of its quantize setting when quantized (the root
    // timeline's grid while it plays, else the clock's). A timeline linked
    // to the global transport plays, stops and seeks that transport, at once.
    void playTimeline(const TimelineRef& ref, bool quantized = true);
    void stopTimeline(const TimelineRef& ref, bool rewind = false, bool quantized = true);
    void seekTimeline(const TimelineRef& ref, double beat);
    // Nodes inside a macro follow, as Auto, the macro clips playing one of
    // its timelines (one that does not play on its own): its beat
    // (restarting with each clip and repeat), its stretched tempo, playing
    // while a clip plays it. Off (the default): they follow the canvas
    // around it. At most one timeline of a canvas: setting it clears the
    // others. Saved with the timeline.
    bool nodesFollowClips(const TimelineRef& ref);
    void setNodesFollowClips(const TimelineRef& ref, bool follow);
    // The timeline of a canvas whose clips its nodes follow, if any.
    bool clipFollowTimeline(const std::string& canvas, TimelineRef& ref);
    // Where its playhead is: where the clip of a playing timeline places it
    // now when it is dependent (the host drives it, as a precomposition's
    // playhead follows its composition), else its own transport's beat
    // (false: no transport yet).
    bool timelinePlayheadBeat(const TimelineRef& ref, double& beat);
    // Where a clip of a playing timeline places it now (false: none does).
    bool placedPlayheadBeat(const TimelineRef& ref, double& beat);
    // Where the last clip that ended before the playhead left it (false:
    // none has ended yet).
    bool heldPlayheadBeat(const TimelineRef& ref, double& beat);
    // Timeline windows besides the main one: which timeline each shows.
    // Saved with the timeline; the editor opens and closes them.
    // A host's window (a MacroMixer's, on a source graph's timeline) carries
    // its host label: titled "(label)" after it, its ImGui id keyed on it
    // (so docking finds it again across activations), never saved.
    // Each window keeps its own zoom, scroll and column width, and its Right
    // edge tool (all saved with it).
    struct TimelineWindow {
        int id = 0; TimelineRef ref; std::string hostLabel; ofxOceanodeTimelineViewState view;
        ofxOceanodeTimelineEndEdit rightEdgeTool = ofxOceanodeTimelineEndEdit::Repeat;
    };
    const std::vector<TimelineWindow>& getTimelineWindows() const { return timelineWindows; }
    TimelineWindow* findTimelineWindow(int id) {
        for(auto& window : timelineWindows) if(window.id == id) return &window;
        return nullptr;
    }
    // A window on this timeline (one a host opened last); nullptr: none.
    const TimelineWindow* windowShowing(const TimelineRef& ref) const;
    int openTimelineWindow(const TimelineRef& ref);
    void closeTimelineWindow(int id);
    // Opens (once) or closes every window on this timeline, for a host.
    void setHostedTimelineWindow(const TimelineRef& ref, bool open, const std::string& hostLabel);
    void setTimelineWindowRef(int id, const TimelineRef& ref);
    // A timeline's play settings (nullptr: unknown timeline).
    const ofxOceanodeTimelinePlaySettings* playSettings(const TimelineRef& ref);
    void setPlaySettings(const TimelineRef& ref, const ofxOceanodeTimelinePlaySettings& settings);
    // A timeline's own colour (its scope bar's tint), the main one's
    // included; false when it keeps its kind's colour.
    bool customTimelineColor(const TimelineRef& ref, ofColor& color);
    void setTimelineColor(const TimelineRef& ref, const ofColor& color);
    void resetTimelineColor(const TimelineRef& ref);

    // Resolves a sequence's binding paths (its base, else this timeline's root).
    ofxOceanodeTimelineResolver resolverFor(const ofxOceanodeTimelineSequence& sequence) const {
        return {sequence.base != nullptr ? sequence.base : container, container};
    }
    // A sequence's target, by the path it keeps, looked up from its
    // container (a target outside a macro from the root).
    ofxOceanodeAbstractParameter* findTarget(const ofxOceanodeContainer& base, const std::string& path) const;
    // The canvases a timeline can take targets from: its own first, then
    // each one above it up to the root.
    std::vector<ofxOceanodeContainer*> targetCanvases(const ofxOceanodeTimelineSequence& sequence) const;
    // The outside targets to save with `canvas` ("" the root): those of the
    // macro timelines below it whose parameters live in it, each under its
    // macro (canvas id below this one), timeline and track, with the lanes
    // that use it; and what `own` still keeps for a macro that holds another
    // definition now.
    ofJson outsideTargetsJson(const std::string& canvas, const ofxOceanodeTimelineCanvas& own) const;

    std::string addBinding(const std::string& trackId,
                          ofxOceanodeAbstractParameter& parameter,
                          ofxOceanodeTimelineAutomationMode mode = ofxOceanodeTimelineAutomationMode::Replace);
    std::string addBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                          ofxOceanodeAbstractParameter& parameter,
                          ofxOceanodeTimelineAutomationMode mode = ofxOceanodeTimelineAutomationMode::Replace);
    // Publish the destination on demand, then route only this lane/role to it.
    bool bindClipTarget(const std::string& trackId, const std::string& clipId,
                        const std::string& laneId, const std::string& parameterPath,
                        ofxOceanodeTimelineTargetRole role = ofxOceanodeTimelineTargetRole::Lane);
    bool bindClipTarget(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                        const std::string& laneId, const std::string& parameterPath,
                        ofxOceanodeTimelineTargetRole role = ofxOceanodeTimelineTargetRole::Lane);
    bool renameTrack(const std::string& trackId, const std::string& requestedName);
    bool renameTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& requestedName);
    // Parameter menus bind immediately, then let the controller choose the clip
    // from its current selection. Existing bindings can be used by another clip.
    // Each request names its timeline ({}: the one the editor shows).
    struct ParameterLaneRequest { TimelineRef ref; std::string trackId, bindingId; };
    bool requestParameterLane(const std::string& trackId, ofxOceanodeAbstractParameter& parameter);
    bool requestParameterLane(const TimelineRef& ref, const std::string& trackId, ofxOceanodeAbstractParameter& parameter);
    std::vector<ParameterLaneRequest> consumePendingParameterLanes() {
        std::vector<ParameterLaneRequest> requests;
        requests.swap(pendingParameterLanes);
        return requests;
    }
    void requestTrackRename(const std::string& trackId, bool isNewTrack = false);
    void requestTrackRename(const TimelineRef& ref, const std::string& trackId, bool isNewTrack = false);
    bool consumePendingTrackRename(std::string& trackId, bool* isNewTrack = nullptr, TimelineRef* ref = nullptr);
    bool isParameterBound(const ofxOceanodeAbstractParameter& parameter) const;
    bool getParameterTrackColor(const ofxOceanodeAbstractParameter& parameter, ofColor& color) const;
    bool isStepLaneCompatible(const ofxOceanodeAbstractParameter& parameter) const;
    bool removeBinding(const std::string& trackId, const std::string& bindingId);
    bool removeBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId);
    // A parameter was renamed: the bindings that pointed at its old path now
    // point at the new one. All pairs apply at once (names swapped between two
    // parameters stay with their parameters). Returns the bindings changed.
    int retargetParameterPaths(const std::map<std::string, std::string>& oldToNew);
    int retargetParameterPaths(ofxOceanodeTimelineSequence& sequence, const std::map<std::string, std::string>& oldToNew);
    // Moves a parameter row within its track (newIndex is clamped). The order is saved.
    bool moveBinding(const std::string& trackId, const std::string& bindingId, int newIndex);
    bool moveBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId, int newIndex);
    bool setBindingMode(const std::string& trackId, const std::string& bindingId, ofxOceanodeTimelineAutomationMode mode);
    bool setBindingMode(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId, ofxOceanodeTimelineAutomationMode mode);
    bool setBindingClamp(const std::string& trackId, const std::string& bindingId, bool clampToParameterRange);
    bool setBindingClamp(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId, bool clampToParameterRange);
    ofxOceanodeTimelineParameterBinding* getBinding(const std::string& trackId, const std::string& bindingId);
    ofxOceanodeTimelineParameterBinding* getBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId);
    // Samples the current numeric parameter value(s) into a dedicated Curve
    // clip at timelineBeat. Numeric vectors use their first component as one
    // scalar curve which is broadcast back across the vector's current width.
    // An empty bindingId captures every compatible binding in the track;
    // otherwise only that binding is sampled. The clip expands in either
    // direction as needed. Returns the number of points set.
    int captureStateAsCurvePoints(const std::string& trackId,
                                  const std::string& bindingId,
                                  double timelineBeat);
    int captureStateAsCurvePoints(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                  const std::string& bindingId,
                                  double timelineBeat);
    // Reorders a track's bindings to the given id order (ids not listed keep
    // their relative order at the end). Note groups are kept together.
    bool setBindingOrder(const std::string& trackId, const std::vector<std::string>& orderedBindingIds);
    bool setBindingOrder(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::vector<std::string>& orderedBindingIds);

    // Note groups: a piano roll's pitch/gate/velocity parameters as one row.
    // A binding belongs to at most one group; assigning it to a group takes
    // it out of any other.
    std::string createNoteGroup(const std::string& trackId, const std::string& name,
                                const std::string& pitchBindingId, const std::string& gateBindingId,
                                const std::string& velocityBindingId);
    std::string createNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& name,
                                const std::string& pitchBindingId, const std::string& gateBindingId,
                                const std::string& velocityBindingId);
    bool setNoteGroupRoles(const std::string& trackId, const std::string& groupId,
                           const std::string& pitchBindingId, const std::string& gateBindingId,
                           const std::string& velocityBindingId);
    bool setNoteGroupRoles(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                           const std::string& pitchBindingId, const std::string& gateBindingId,
                           const std::string& velocityBindingId);
    // A kit's Duration input (Drum Grid lanes of the group write it).
    bool setNoteGroupDuration(const std::string& trackId, const std::string& groupId,
                              const std::string& durationBindingId);
    // A kit's Filter input (Drum Grid lanes of the group write it).
    bool setNoteGroupFilter(const std::string& trackId, const std::string& groupId,
                            const std::string& filterBindingId);
    // A kit's Stutter input (Drum Grid lanes of the group write it).
    bool setNoteGroupStutter(const std::string& trackId, const std::string& groupId,
                             const std::string& stutterBindingId);
    bool renameNoteGroup(const std::string& trackId, const std::string& groupId, const std::string& name);
    // removeBindings: also remove the member parameters from the timeline;
    // otherwise they become ordinary rows again.
    bool removeNoteGroup(const std::string& trackId, const std::string& groupId, bool removeBindings);
    bool removeNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId, bool removeBindings);
    // 0 = automatic (see ofxOceanodeTimelineNoteGroup::voices).
    bool setNoteGroupVoices(const std::string& trackId, const std::string& groupId, int voices);
    ofxOceanodeTimelineNoteGroup* getNoteGroup(const std::string& trackId, const std::string& groupId);
    ofxOceanodeTimelineNoteGroup* getNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId);
    static const ofxOceanodeTimelineNoteGroup* findNoteGroupForBinding(const ofxOceanodeTimelineTrack& track,
                                                                        const std::string& bindingId);
    // Creates a clip with one piano-roll lane driving the group's roles; the
    // key range follows the pitch parameter's range when it is a note range.
    // laneType DrumGrid: a Drum Grid lane instead, one row per kit slot of
    // the instrument (named from its rowNames).
    std::string createNoteGroupClip(const std::string& trackId, const std::string& groupId,
                                    const std::string& requestedName, double startBeat, double durationBeats,
                                    std::string* laneIdOut = nullptr,
                                    ofxOceanodeTimelineLaneType laneType = ofxOceanodeTimelineLaneType::PianoRoll);
    std::string createNoteGroupClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                                    const std::string& requestedName, double startBeat, double durationBeats,
                                    std::string* laneIdOut = nullptr,
                                    ofxOceanodeTimelineLaneType laneType = ofxOceanodeTimelineLaneType::PianoRoll);
    // The instrument a note-role lane plays (its Gate's or Pitch's node):
    // its voice count and kit slot names. False when it plays no instrument.
    // colors (optional): its kit slot colours (rowColors), see
    // ofxOceanodeTimelineDrumGrid::rowColor.
    // instrument (optional): all it reports (row waveforms, Filter / Stutter inputs).
    bool drumGridInstrument(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane,
                            int& voices, std::vector<std::string>& names, bool* kit = nullptr,
                            std::vector<ofColor>* colors = nullptr,
                            ofxOceanodeTimelineInstrument* instrument = nullptr) const;
    // A Chords lane: a Multi Gate lane whose first target is a chord bank's
    // gate (see ofxOceanodeTimelineChordInstrument). False for any other lane.
    bool chordLaneInstrument(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane,
                             ofxOceanodeTimelineInstrument* instrument = nullptr) const;
    // The same for a note group's roles (the id overload: a track of the
    // edit target's).
    bool noteGroupInstrument(const std::string& trackId, const std::string& groupId,
                             int& voices, bool& kit, std::vector<std::string>* names = nullptr) const;
    bool noteGroupInstrument(const ofxOceanodeTimelineTrack& track, const std::string& groupId,
                             int& voices, bool& kit, std::vector<std::string>* names = nullptr) const;
    // "Add to Timeline as Piano Roll..." from a parameter's right-click menu:
    // queued here, shown by the timeline controller (like track renames).
    void requestNoteGroupSetup(ofxOceanodeAbstractParameter* parameter) { requestNoteGroupSetup(TimelineRef{}, parameter); }
    void requestNoteGroupSetup(const TimelineRef& ref, ofxOceanodeAbstractParameter* parameter) {
        pendingNoteGroupSetupParameter = parameter;
        pendingNoteGroupSetupRef = normalizedRef(ref);
    }
    ofxOceanodeAbstractParameter* consumePendingNoteGroupSetup(TimelineRef* ref = nullptr) {
        auto* parameter = pendingNoteGroupSetupParameter;
        if(ref != nullptr) *ref = pendingNoteGroupSetupRef;
        pendingNoteGroupSetupParameter = nullptr;
        pendingNoteGroupSetupRef = {};
        return parameter;
    }
    const ofxOceanodeTimelineParameterBinding* getBinding(const std::string& trackId, const std::string& bindingId) const;
    const ofxOceanodeTimelineParameterBinding* getBinding(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId) const;
    // The node a piano lane's pitch binding drives (nullptr when it has none):
    // what Global Pitch makes of the lane's notes depends on that node's ticks.
    // The sequence overload: the lane as that timeline keeps it (its paths
    // relative to its canvas).
    ofxOceanodeNodeModel* pianoPitchNode(const std::string& trackId, const ofxOceanodeTimelineLane& lane) const;
    ofxOceanodeNodeModel* pianoPitchNode(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                         const ofxOceanodeTimelineLane& lane) const;

    std::string createClip(const std::string& trackId,
                           const std::string& requestedName = "Clip",
                           double startBeat = 0.0,
                           double durationBeats = 4.0);
    std::string createClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::string& requestedName = "Clip",
                           double startBeat = 0.0,
                           double durationBeats = 4.0);
    // Samples a track's LFO (the first LFO lane of each clip) at a timeline
    // beat. A pure function of the beat, so playback, scrubbing and seeking
    // give identical results. Between or outside clips: active = false,
    // phase 0, cycle holds the count reached so far.
    ofxOceanodeTimelineLfoSample evaluateLfoTrack(const std::string& trackId, double beat) const;
    ofxOceanodeTimelineLfoSample evaluateLfoTrack(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat) const;
    bool renameClip(const std::string& trackId, const std::string& clipId, const std::string& requestedName);
    bool renameClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& requestedName);
    // Cuts an automation clip in two at a timeline beat; the right
    // part becomes a new clip (returned) and both halves play exactly what
    // the original played. Repeating clips are cut anywhere (the right part
    // keeps repeating from the phase it was cut at). Not for Wave clips.
    std::string splitClip(const std::string& trackId, const std::string& clipId, double timelineBeat);
    // Joins two clips of one track into one plain clip spanning both (no
    // repeat, stretch or offset: each clip's content is laid out as it
    // plays). Lanes driving the same parameters with the same kind of lane
    // become one lane; the others are kept as separate lanes holding data only
    // over their own clip's span. The earlier clip keeps its id and settings,
    // the other is removed. Refused for Wave and state-capture clips, and
    // for a Drum Grid that would need a row longer than its maximum.
    // Returns the merged clip's id ("" when refused).
    std::string mergeClips(const std::string& trackId, const std::string& firstClipId, const std::string& secondClipId);
    std::string mergeClips(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::string& firstClipId, const std::string& secondClipId);
    // Any number of clips of one track, joined the same way in start order:
    // the earliest keeps its id, and where clips overlap the later one wins.
    // All or nothing: refused (and nothing changes) when any clip could not
    // be merged.
    std::string mergeClips(const std::string& trackId, const std::vector<std::string>& clipIds);
    std::string mergeClips(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::vector<std::string>& clipIds);
    std::string splitClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double timelineBeat);
    // Copies a clip, with fresh clip/lane ids, to dstTrackId at newStartBeat
    // and returns the new clip's id ("" when it cannot go there).
    //  - dstBindingId empty: the whole clip. On another track every binding is
    //    remapped to that track's binding of the same parameter. A piano roll
    //    or drum grid with no match plays that track's own note group (the
    //    first one no other lane of the copy drives); other lanes with no
    //    match are dropped (nothing left: refused).
    //  - dstBindingId set: only the lane srcLaneId, now driving that binding (a
    //    piano roll needs a note group's row and takes its pitch/gate/velocity).
    // Wave clips only go to Wave tracks.
    std::string duplicateClip(const std::string& srcTrackId, const std::string& clipId,
                              const std::string& dstTrackId, double newStartBeat,
                              const std::string& srcLaneId = std::string(),
                              const std::string& dstBindingId = std::string());
    std::string duplicateClip(ofxOceanodeTimelineSequence& sequence, const std::string& srcTrackId, const std::string& clipId,
                              const std::string& dstTrackId, double newStartBeat,
                              const std::string& srcLaneId = std::string(),
                              const std::string& dstBindingId = std::string());
    // Turns one multi-lane state-capture clip into one independent clip per
    // parameter lane. Timing and curve data are preserved; later captures
    // keep appending to the matching per-parameter destination.
    int separateStateCaptureClip(const std::string& trackId, const std::string& clipId);
    int separateStateCaptureClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    // Moves a clip's start, keeping its end and what plays under it (the left
    // part is dropped). Growing to the left works for repeating clips only.
    bool trimClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat);
    bool trimClipStart(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double newStartBeat);
    // Shift+drag of the left edge. Moving the start earlier adds empty source
    // time in front of the content, which stays where it is on the timeline
    // (works for one-shot clips too, unlike trimClipStart, which can only grow
    // repeating content by whole cycles). Moving it later is a plain trim.
    bool extendClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat);
    bool extendClipStart(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double newStartBeat);
    bool removeClip(const std::string& trackId, const std::string& clipId);
    bool removeClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    ofxOceanodeTimelineClip* getClip(const std::string& trackId, const std::string& clipId);
    ofxOceanodeTimelineClip* getClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    const ofxOceanodeTimelineClip* getClip(const std::string& trackId, const std::string& clipId) const;
    const ofxOceanodeTimelineClip* getClip(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) const;
    std::string createLane(const std::string& trackId, const std::string& clipId,
                           const std::string& requestedName = "Lane",
                           ofxOceanodeTimelineLaneType type = ofxOceanodeTimelineLaneType::Step);
    std::string createLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                           const std::string& requestedName = "Lane",
                           ofxOceanodeTimelineLaneType type = ofxOceanodeTimelineLaneType::Step);
    // Independent content, same bindings/settings, inserted immediately after the source.
    // Legacy Wave lanes cannot be duplicated.
    std::string duplicateLane(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    std::string duplicateLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId);
    bool removeLane(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    bool removeLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId);
    bool addBindingToLane(const std::string& trackId, const std::string& clipId,
                          const std::string& laneId, const std::string& bindingId);
    bool addBindingToLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                          const std::string& laneId, const std::string& bindingId);
    bool removeBindingFromLane(const std::string& trackId, const std::string& clipId,
                               const std::string& laneId, const std::string& bindingId);
    bool removeBindingFromLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                               const std::string& laneId, const std::string& bindingId);
    // The "-" of a lane's Targets list: the lane no longer drives the target
    // nor lists it; the track's other lanes keep it as they have it. Once no
    // lane of the track drives or lists it (and no note group holds it), the
    // track lets go of it too.
    bool removeTargetFromLaneList(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                  const std::string& laneId, const std::string& bindingId);
    ofxOceanodeTimelineLane* getLane(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    ofxOceanodeTimelineLane* getLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId);
    const ofxOceanodeTimelineLane* getLane(const std::string& trackId, const std::string& clipId, const std::string& laneId) const;
    const ofxOceanodeTimelineLane* getLane(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId) const;
    bool setClipLaneType(const std::string& trackId, const std::string& clipId,
                         const std::string& laneId, ofxOceanodeTimelineLaneType type);
    bool setClipLaneType(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                         const std::string& laneId, ofxOceanodeTimelineLaneType type);
    bool setClipTiming(const std::string& trackId, const std::string& clipId,
                       double startBeat, double durationBeats);
    bool setClipTiming(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                       double startBeat, double durationBeats);
    bool isTrackAudible(const ofxOceanodeTimelineTrack& track) const;
    // ---- Folders (see ofxOceanodeTimelineTrack::isFolder) ----
    std::string createFolder(const std::string& requestedName = "Folder", const std::string& parentId = std::string());
    std::string createFolder(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Folder",
                             const std::string& parentId = std::string());
    // Moves a track (a folder with everything inside it) into parentId (empty:
    // the top level), at `index` among that folder's tracks (clamped).
    // Refused into the track itself or into anything inside it.
    bool moveTrack(const std::string& trackId, const std::string& parentId, size_t index);
    bool moveTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                   const std::string& parentId, size_t index);
    // keepTracks: what was inside moves up to the folder's own parent;
    // otherwise it is removed with the folder.
    bool removeFolder(const std::string& folderId, bool keepTracks);
    bool removeFolder(ofxOceanodeTimelineSequence& sequence, const std::string& folderId, bool keepTracks);
    // Mute, solo and latency as playback uses them: the track's own, with
    // the folders it is in (any of them muted mutes it, soloed solos it,
    // latencies add up, clamped to +-kMaxTrackLatencyOffsetMs).
    bool isTrackMutedWithFolders(const ofxOceanodeTimelineTrack& track) const;
    bool isTrackSoloedWithFolders(const ofxOceanodeTimelineTrack& track) const;
    float trackLatencyWithFolders(const ofxOceanodeTimelineTrack& track) const;
    // The solo playback uses: the one a Toggle lane last drove, else the
    // track's own (saved) solo.
    bool isTrackSoloed(const ofxOceanodeTimelineTrack& track) const;
    // Whether the clip's chance leaves it out of the pass now playing.
    bool isClipChanceSilenced(const ofxOceanodeTimelineClip& clip) const;
    bool setTrackMuted(const std::string& trackId, bool muted);
    bool setTrackMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, bool muted);
    bool setTrackSolo(const std::string& trackId, bool solo);
    bool setTrackSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, bool solo);
    bool setTrackLatencyOffset(const std::string& trackId, float milliseconds);
    bool setTrackLatencyOffset(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, float milliseconds);
    bool setLaneMuted(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool muted);
    bool setLaneMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, bool muted);
    bool setLaneSolo(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool solo);
    bool setLaneSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, bool solo);
    bool setLaneChance(const std::string& trackId, const std::string& clipId, const std::string& laneId, float chance);
    bool setLaneChance(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, float chance);
    bool setLaneDrivesTrackSolo(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool drives);
    bool setLaneDrivesTrackSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId, bool drives);
    bool setLaneTargetProbability(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                                  const std::string& parameterPath, float probability);
    bool setLaneTargetProbability(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId,
                                  const std::string& parameterPath, float probability);
    bool setClipMuted(const std::string& trackId, const std::string& clipId, bool muted);
    bool setClipMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, bool muted);
    bool setClipChance(const std::string& trackId, const std::string& clipId, float chance);
    bool setClipChance(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, float chance);
    // The notes a piano lane plays in one repeat of the current clip start,
    // mutations applied (indexed like its pianoNotes). False when it has no
    // mutating notes. For the editor's preview of the playing repeat. On the
    // primary: answered by the engine playing the timeline (the edit
    // target's, or the sequence's), whose rolls are the ones heard.
    bool pianoRepeatNotes(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                          int64_t repeat, std::vector<ofxOceanodeTimelinePianoNote>& out) const;
    bool pianoRepeatNotes(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                          const std::string& laneId, int64_t repeat, std::vector<ofxOceanodeTimelinePianoNote>& out) const;
    bool resizeClipEnd(const std::string& trackId, const std::string& clipId,
                       double durationBeats, ofxOceanodeTimelineEndEdit operation);
    bool resizeClipEnd(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                       double durationBeats, ofxOceanodeTimelineEndEdit operation);
    // Does not write parameters, schedule events, or change source content.
    // On the primary: answered by the engine playing the edit target (made
    // and brought up to date if need be), else by the engine driving the
    // path; the sequence overload by the engine playing that timeline (its
    // paths as the sequence keeps them, relative to its canvas).
    ofxOceanodeTimelineTargetInspection inspectTarget(const std::string& parameterPath,
                                                     double beat, bool isPlaying) const;
    ofxOceanodeTimelineTargetInspection inspectTarget(const ofxOceanodeTimelineSequence& sequence, const std::string& parameterPath,
                                                     double beat, bool isPlaying) const;
    // A copy of the clip with its content laid out from where it starts
    // playing (contentOffsetBeats baked into the lanes): it sounds the same,
    // and previews can draw it without a wrap.
    ofxOceanodeTimelineClip clipAsPlayed(const ofxOceanodeTimelineClip& clip) const {
        ofxOceanodeTimelineClip played = clip;
        bakeContentOffset(played);
        return played;
    }
    // Rotates where the clip's content starts (see contentOffsetBeats);
    // refused for Wave clips.
    bool setClipContentOffset(const std::string& trackId, const std::string& clipId, double offsetBeats);
    bool setClipContentOffset(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                              const std::string& clipId, double offsetBeats);
    bool setClipContentDuration(const std::string& trackId, const std::string& clipId,
                                 double contentDurationBeats, bool repeatContent);
    bool setClipContentDuration(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                 double contentDurationBeats, bool repeatContent);
    bool consolidateClipContent(const std::string& trackId, const std::string& clipId);
    bool consolidateClipContent(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    bool setClipStep(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                     double startBeat, const std::string& value, double durationBeats = 0.0);
    bool setClipStep(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId,
                     double startBeat, const std::string& value, double durationBeats = 0.0);
    bool setClipNumber(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                       double beat, float value);
    bool setClipNumber(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId,
                       double beat, float value);
    bool removeClipStep(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                        double startBeat);
    bool removeClipStep(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& laneId,
                        double startBeat);
    // MultiValue / MultiGate row count (see ofxOceanodeTimelineLane::
    // multiRowCount). Preserves existing row data when growing; drops the
    // trailing rows' data when shrinking, the same "data is lost" tradeoff
    // the old standalone Multi Value/Gate Track nodes made when their own
    // Num Lanes shrank.
    bool setLaneMultiRowCount(const std::string& trackId, const std::string& clipId,
                              const std::string& laneId, int rowCount);
    bool setLaneMultiRowCount(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                              const std::string& laneId, int rowCount);
    // Gives a Multi Curve lane one curve per element its first vector target
    // has now (existing curves are kept, as when setting the count by hand).
    // False when the lane has no vector target to follow.
    bool fitMultiCurvesToTarget(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                const std::string& laneId);
    // Recomputes waveNumChannels/waveFileDurationMs/waveformPeaks from
    // whatever waveFilePath currently points at (WAV files only: PCM 8 to
    // 32 bit and 32/64 bit float). Pure file IO, no
    // SuperCollider dependency -- safe to call unconditionally regardless
    // of whether a wave audio provider is registered. Called automatically
    // by fromJson()/loadPreset() for every wave clip, and by the controller
    // whenever the user points a clip at a different file. It also re-derives
    // waveFileDurationBeats at the current tempo -- see the definition.
    bool reloadWaveform(const std::string& trackId, const std::string& clipId);
    bool reloadWaveform(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);

    // A wave clip's volume at one beat. Volume is a property of the whole
    // Wave Track (gain plus an optional envelope in timeline-beat space), so
    // this resolves the clip's track; it stays clip-scoped because that is
    // what the audio provider asks for.
    float evaluateWaveClipVolume(const std::string& trackId, const std::string& clipId,
                                 double beat) const;
    // The same for a clip of any timeline (beat: of that timeline): its
    // track's volume there, through the clip's fades.
    float evaluateWaveClipVolume(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineClip& clip,
                                 double beat) const;
    float evaluateWaveTrackVolume(const ofxOceanodeTimelineTrack& track, double beat) const;
    // Sets a wave clip's gain to the most its loudest sample takes without
    // going over full scale (at most the gain's range, x4). False when the
    // clip is silent or has no waveform.
    bool normalizeWaveClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    // Undoes normalizeWaveClip; the clip gain is untouched.
    bool clearWaveClipNormalization(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    // Sets a wave clip's fade lengths (timeline beats), kept inside the clip
    // and off each other: the one being changed stops at the other.
    bool setWaveClipFades(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                          double fadeInBeats, double fadeOutBeats);
    // Opens/prepares the track-wide volume envelope for a Wave Track, seeding
    // a flat one from the track's gain if it has none yet. Named for the
    // per-clip lane this replaced; it creates no lane, and the returned
    // string is only a stable handle for the editor, not a lane id.
    std::string createWaveVolumeLane(const std::string& trackId, const std::string& clipId);
    std::string createWaveVolumeLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);

    float evaluateWaveTrackVolume(const std::string& trackId, double beat) const;
    std::vector<ofxOceanodeTimelineCurvePoint>& getWaveTrackVolumePoints(const std::string& trackId);
    std::vector<ofxOceanodeTimelineCurvePoint>& getWaveTrackVolumePoints(ofxOceanodeTimelineSequence& sequence, const std::string& trackId);
    const std::vector<ofxOceanodeTimelineCurvePoint>& getWaveTrackVolumePoints(const std::string& trackId) const;
    const std::vector<ofxOceanodeTimelineCurvePoint>& getWaveTrackVolumePoints(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId) const;
    bool addWaveTrackVolumePoint(const std::string& trackId, double beat, float value);
    bool addWaveTrackVolumePoint(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat, float value);
    bool removeWaveTrackVolumePoint(const std::string& trackId, double beat, double epsilon = 1e-6);
    bool removeWaveTrackVolumePoint(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat, double epsilon = 1e-6);

    // Registers/clears the optional audio backend for Wave lanes (see
    // ofxOceanodeTimelineWaveAudioProvider above). The manager does not own
    // the provider; the caller (typically a bridge object living inside
    // ofxOceanodeSuperCollider) is responsible for its lifetime and must
    // clear it (pass nullptr) before destroying it. The players' engines
    // hold the same pointer, so they take the change at once.
    void setWaveAudioProvider(ofxOceanodeTimelineWaveAudioProvider* provider);
    ofxOceanodeTimelineWaveAudioProvider* getWaveAudioProvider() const { return waveAudioProvider; }

    // Cross-track clip grouping (see ofxOceanodeTimelineClipGroup above).
    // groupClips folds in the membership of any group its arguments
    // already belong to, so grouping a mix of already-grouped and
    // ungrouped clips merges everything into one group instead of leaving
    // overlapping ones -- returns the new group's id, or an empty string if
    // fewer than two distinct valid clips ended up in it. ungroupClip
    // dissolves the whole group a clip belongs to (groups are flat, not
    // nested, so there's no partial-ungroup of just one member).
    std::string groupClips(const std::vector<std::pair<std::string, std::string>>& members);
    std::string groupClips(ofxOceanodeTimelineSequence& sequence, const std::vector<std::pair<std::string, std::string>>& members);
    bool ungroupClip(const std::string& trackId, const std::string& clipId);
    bool ungroupClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    const ofxOceanodeTimelineClipGroup* getGroupForClip(const std::string& trackId, const std::string& clipId) const;
    const ofxOceanodeTimelineClipGroup* getGroupForClip(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) const;
    const std::vector<ofxOceanodeTimelineClipGroup>& getClipGroups() const { return editSequence().clipGroups; }

    ofxOceanodeTimelineViewState& getViewState() { return viewState; }
    const ofxOceanodeTimelineViewState& getViewState() const { return viewState; }

    ofxOceanodeTimelineTimebase getTimebase() const { return timebase; }
    void setTimebase(ofxOceanodeTimelineTimebase value) { timebase = value; }
    // Called before applying a manual BPM change while Time is selected.
    // Source data inside clips remains in its local beat domain; scaling each
    // clip's placement and stretch keeps every contained event at the same
    // absolute time without destructively rewriting the pattern itself.
    void retimeForBpmChange(float oldBpm, float newBpm);

    bool isBpmAutomationEnabled() const { return bpmAutomationEnabled; }
    void setBpmAutomationEnabled(bool enabled);
    bool isBpmLaneCollapsed() const { return bpmLaneCollapsed; }
    void setBpmLaneCollapsed(bool collapsed) { bpmLaneCollapsed = collapsed; }
    // Show/hide the tempo (BPM) automation row. Hidden only affects the editor:
    // tempo automation keeps playing.
    bool isBpmLaneVisible() const { return bpmLaneVisible; }
    void setBpmLaneVisible(bool visible) { bpmLaneVisible = visible; }
    const bool& bpmLaneVisibleRef() const { return bpmLaneVisible; }
    float getBpmMinimum() const { return bpmMinimum; }
    float getBpmMaximum() const { return bpmMaximum; }
    void setBpmRange(float minimum, float maximum);
    std::vector<ofxOceanodeTimelineCurvePoint>& getBpmAutomationPoints() { return bpmAutomationPoints; }
    const std::vector<ofxOceanodeTimelineCurvePoint>& getBpmAutomationPoints() const { return bpmAutomationPoints; }
    std::vector<ofxOceanodeTimelineCurveTension>& getBpmCurveTensions() { return bpmCurveTensions; }
    const std::vector<ofxOceanodeTimelineCurveTension>& getBpmCurveTensions() const { return bpmCurveTensions; }
    const std::string& getBpmInterpolation() const { return bpmInterpolation; }
    void setBpmInterpolation(const std::string& interpolation);
    float evaluateBpm(double beat, float fallbackBpm) const;
    double beatToSeconds(double beat, float fallbackBpm) const;
    // The inverse: the beat reached `seconds` after beat 0.
    double secondsToBeat(double seconds, float fallbackBpm) const;

    // The loop this manager plays with: an engine's is its timeline's own
    // (adopted from its play settings each pass); the primary plays nothing
    // and keeps none. A transport of the timeline's own (derived) wraps in
    // it itself; the root transport is never looped: a manager playing on
    // it wraps the position into the loop itself (loopedState).
    bool isLoopEnabled() const { return loopEnabled; }
    bool didLoopWrapThisFrame() const { return loopWrappedThisFrame; }
    // ---- Markers ----
    const std::vector<ofxOceanodeTimelineMarker>& getMarkers() const { return markers; }
    std::string addMarker(double beat, const std::string& name = std::string());
    bool removeMarker(const std::string& markerId);
    bool moveMarker(const std::string& markerId, double beat);
    bool renameMarker(const std::string& markerId, const std::string& name);
    const ofxOceanodeTimelineMarker* getMarker(const std::string& markerId) const;

    void setLoopEnabled(bool enabled);
    // An offline render (Render Audio...) plays every timeline straight
    // through: loops are ignored while this is on, their settings kept.
    static void setLoopsSuspended(bool suspended) { loopsSuspendedFlag() = suspended; }
    static bool loopsSuspended() { return loopsSuspendedFlag(); }
    // Play on preset load: when a preset with this on finishes loading, the
    // transport starts from the beginning (saved per preset, off by default)
    bool getPlayOnPresetLoad() const { return playOnPresetLoad; }
    void setPlayOnPresetLoad(bool enabled) { playOnPresetLoad = enabled; }
    // The metronome clicks along one timeline's transport (whatever its
    // clock source), in that timeline's meter: the one it was switched on for.
    bool isMetronomeEnabled() const { return metronome != nullptr; }
    bool isMetronomeEnabled(const TimelineRef& ref) const { return metronome != nullptr && normalizedRef(ref) == metronomeRef; }
    bool setMetronomeEnabled(bool enabled, const TimelineRef& ref = TimelineRef());
    double getLoopStartBeat() const { return loopStartBeat; }
    double getLoopEndBeat() const { return loopEndBeat; }
    void setLoopRange(double startBeat, double endBeat);

    // The timeline isn't always 4/4: numerator/denominator are stored and
    // persisted like any other project setting, and getBeatsPerBar() is
    // what every bar-line/grid computation in the timeline UI should use
    // instead of assuming 4 beats (quarter notes) per bar.
    int getTimeSignatureNumerator() const { return timeSignatureNumerator; }
    int getTimeSignatureDenominator() const { return timeSignatureDenominator; }
    double getBeatsPerBar() const { return timeSignatureNumerator * (4.0 / timeSignatureDenominator); }
    // The timeline's grid / snap division in beats (0 = None), the "master"
    // a Drum Grid lane's editing grid may follow. Saved with the timeline
    // ("masterGridBeats"). Editing only: no lane plays differently with it.
    double getMasterGridBeats() const { return masterGridBeats; }
    void setMasterGridBeats(double beats);
    // The step length a Drum Grid lane plays with (see drumStepFollow).
    double drumStepBeats(const ofxOceanodeTimelineLane& lane) const {
        return ofxOceanodeTimelineDrumGrid::effectiveStepBeats(lane, masterGridBeats);
    }
    void setTimeSignature(int numerator, int denominator);

    // --- Timestamped scheduling -------------------------------------------
    // A parameter whose backend registered a handler through
    // ofxOceanodeScheduling receives its discrete events (steps, notes, gates,
    // multi-value blocks) ahead of time, with the exact instant they are due,
    // instead of on the GUI frame that happens to cross them. Continuous lanes
    // (curves) and any parameter without such a backend keep the frame path.
    void setSchedulingEnabled(bool enabled);
    bool isSchedulingEnabled() const { return schedulingEnabled; }
    // The longest window events are handed to the backend ahead of time. The
    // scheduler uses three recent frame intervals plus 10 ms (at least 50 ms)
    // up to this limit: a longer window survives longer GUI stalls, a shorter
    // one leaves less already-sent content that a stop, seek or edit can only
    // correct, not cancel.
    void setSchedulingLookaheadMs(double milliseconds);
    double getSchedulingLookaheadMs() const { return schedulingLookaheadMs; }
    // Off: the window is always getSchedulingLookaheadMs().
    void setSchedulingLookaheadAdaptive(bool adaptive) { schedulingLookaheadAdaptive = adaptive; }
    bool isSchedulingLookaheadAdaptive() const { return schedulingLookaheadAdaptive; }
    // The window actually used: long enough for the recent frame intervals,
    // never longer than getSchedulingLookaheadMs().
    double effectiveSchedulingLookaheadMs() const;
    // Editors may call this after changing clip/lane/loop data; the scheduler
    // also repairs itself when it notices the playhead's value is not the one
    // the backend was given. Corrections go out on the next update.
    void invalidateSchedule();
    // Hash of everything in a lane that changes what it plays (not its name or
    // view settings). Editors that write lanes in place compare it before and
    // after drawing, and invalidate the schedule when it changed.
    static uint64_t lanePlaybackSignature(const ofxOceanodeTimelineLane& lane);

    // Like canvas history, transactions span a complete user gesture. Runtime
    // automation and view scrolling/zooming are never recorded. The global
    // edit covers the global transport's settings (markers, tempo lane,
    // loop, meter, timebase, scheduling); each timeline has its own history
    // (beginTimelineEdit(sequence)...).
    void beginGlobalEdit();
    bool endGlobalEdit();
    bool undoGlobalEdit();
    bool redoGlobalEdit();
    bool canUndoGlobalEdit() const { return !timelineUndo.empty(); }
    bool canRedoGlobalEdit() const { return !timelineRedo.empty(); }
    // Every history: the global one and each timeline's.
    void clearTimelineHistory();
    // ---- The edit target: the timeline the id-only overloads act on ----
    // An engine: the copy it plays. The primary: the timeline set here (a
    // caller keeping one timeline; the tests), else a scratch sequence that
    // is never shown or played. Nothing is made by itself.
    void setEditTarget(const TimelineRef& ref) { editTarget = normalizedRef(ref); }
    TimelineRef getEditTarget() const { return editTarget; }
    ofxOceanodeTimelineSequence& editSequence();
    const ofxOceanodeTimelineSequence& editSequence() const;
    // The edit target's history, with the global one: one gesture over both.
    void beginTimelineEdit();
    bool endTimelineEdit();
    bool undoTimelineEdit();
    bool redoTimelineEdit();
    bool canUndoTimelineEdit() const;
    bool canRedoTimelineEdit() const;

    ofJson toJson() const;
    void fromJson(const ofJson& json);
    // Authored root and macro timelines, without running clocks or editor views.
    ofJson presetEditState() const;
    // Portable clip documents contain one clip and only its target descriptors.
    // Map saved binding IDs to parameter paths in the destination project.
    // Missing mappings leave content intact and unassigned. Empty trackId creates a track.
    ofJson exportClip(const std::string& trackId, const std::string& clipId) const;
    ofJson exportClip(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) const;
    bool saveClipFile(const std::string& trackId, const std::string& clipId,
                      const std::string& path, std::string* error = nullptr) const;
    static ofJson readClipFile(const std::string& path, std::string* error = nullptr);
    std::pair<std::string, std::string> importClip(const ofJson& document,
        const std::string& trackId, double startBeat,
        const std::map<std::string, std::string>& targetPaths = {}, std::string* error = nullptr);
    bool savePreset(const std::string& presetFolderPath) const;
    bool loadPreset(const std::string& presetFolderPath);

    // Public because the controller UI displays a binding's blend mode
    // (e.g. next to the parameter name) and needs to turn it into a label.
    static std::string modeToString(ofxOceanodeTimelineAutomationMode mode);
    static ofxOceanodeTimelineAutomationMode modeFromString(const std::string& mode);

private:
    // 16: the loop is each timeline's own (the top-level "loop", the global
    // transport's until then, is read into the root canvas's timelines
    // linked to it).
    static constexpr int kPresetVersion = 17;
    // The global transport's settings for the global history: typed copies
    // of what the JSON would reload lossily, and the JSON a step compares.
    struct TimelineState {
        ofJson data;
        std::vector<ofxOceanodeTimelineMarker> markers;
        std::vector<ofxOceanodeTimelineCurvePoint> tempoPoints;
        std::vector<ofxOceanodeTimelineCurveTension> tempoTensions;
        size_t bytes = 0;
    };
    struct TimelineEdit {
        TimelineState before, after;
        float beforeBpm = 0.0f, afterBpm = 0.0f;
        bool manualTempo = false;
        size_t bytes = 0;
        uint64_t gesture = 0; // the gesture that recorded it (see openGesture)
    };
    std::unique_ptr<TimelineEdit> pendingTimelineEdit;
    // One timeline's state for its history: typed copies (waveform caches
    // kept) and the content JSON they were taken from (what a step compares).
    struct SequenceSnapshot {
        ofJson data;
        std::vector<ofxOceanodeTimelineTrack> tracks;
        std::vector<ofxOceanodeTimelineClipGroup> groups;
        ofxOceanodeTimelinePlaySettings play;
        uint64_t nextTrack = 1, nextBinding = 1, nextClip = 1, nextLane = 1, nextGroup = 1;
        size_t bytes = 0;
        uint64_t gesture = 0; // on an entry's snapshots: the gesture that recorded it
    };
    SequenceSnapshot captureSequence(const ofxOceanodeTimelineSequence& sequence) const;
    void restoreSequence(ofxOceanodeTimelineSequence& sequence, const SequenceSnapshot& snapshot);
    // Macro canvases: each macro's timelines, where they live and who shares them.
    struct MacroSequenceEntry {
        std::weak_ptr<ofxOceanodeContainer> container; // expires with the macro
        const ofxOceanodeContainer* address = nullptr;
        std::shared_ptr<ofxOceanodeTimelineCanvas> timelines;
        std::string libraryFolder; // a library macro's definition: shared by every copy
    };
    mutable std::vector<MacroSequenceEntry> macroSequences;
    // Macro timelines as the main timeline plays them: a copy of each one
    // its macro tracks reach, with absolute paths and canvases (from the
    // root container), so indexes, apply and the scheduler handle its
    // bindings like the main timeline's. Rebuilt with the evaluation indexes.
    struct PlayedMacro {
        const ofxOceanodeContainer* macroContainer = nullptr;
        std::string canvas; // absolute canvas id ("" the root)
        std::string timeline; // its id within the canvas ("" the default)
        std::unique_ptr<ofxOceanodeTimelineSequence> sequence;
        float outerLatencyMs = 0.0f; // the macro tracks' around it, added up
        // The copy reversed macro clips play: the timeline mirrored over
        // mirrorLength (one pass of it).
        bool reversed = false;
        double mirrorLength = 0.0;
        // A timeline no track places, of a macro placed from outside: the
        // macro is placed as a whole, so the clips placing this other
        // timeline of it (its id) carry it too. Empty: placed by its own tracks.
        std::string inheritsFrom;
        // A timeline of a live source of a Macro Morphing Track: the source's
        // id in the track's engine, and the timeline holding the track (its
        // timeSourceId). The track's clips of that source place it.
        std::string morphSource;
        std::string morphHost;
    };
    std::vector<PlayedMacro> playedMacros;
    // A macro timeline the main timeline reaches: through the macro track
    // `trackIndex` of `parent` (-1: the main timeline, else a source index).
    struct MacroSource {
        const ofxOceanodeContainer* macroContainer = nullptr;
        std::string canvas;
        std::string timeline;
        const ofxOceanodeTimelineSequence* sequence = nullptr;
        int parent = -1;
        size_t trackIndex = 0;
        std::string inheritsFrom; // as PlayedMacro has it
        std::string morphSource;  // as PlayedMacro has them
        std::string morphHost;
    };
    std::vector<MacroSource> collectMacroSources() const;
    // The timeline of a macro that encapsulation moves tracks into: its
    // first one, made (named after the macro) when it has none.
    ofxOceanodeTimelineSequence* encapsulationTarget(ofxOceanodeContainer& macroContainer, bool& made);
    // Moves source's tracks driving only what is inside the macro at
    // `relativeCanvas` into target (only `onlyIds` when given) and adds the
    // macro track playing them (id `macroTrackId` when free). Record for undo.
    ofJson moveTracksIntoMacro(ofxOceanodeTimelineSequence& source, ofxOceanodeTimelineSequence& target,
                               ofxOceanodeContainer& macroContainer, const std::string& relativeCanvas,
                               const std::set<std::string>* onlyIds, const std::string& macroTrackId);
    // A parameter of the macro node owning this container ("Active",
    // "Snapshot"); nullptr when no macro owns it.
    ofxOceanodeAbstractParameter* macroNodeParameter(const ofxOceanodeContainer& macroContainer,
                                                     const std::string& name) const;
    // Whether this is the Active of a macro a macro track gates.
    bool isGatedMacroActive(const ofxOceanodeAbstractParameter& parameter);
    // Gated macros' Active and each macro clip's snapshot, once per frame.
    void updateMacroGating(const ofxOceanodeTransportState& transport);
    // Macros gating switched off (switched back on when no longer gated).
    std::set<const ofxOceanodeContainer*> gatedOffMacros;
    // The clip whose snapshot each macro last recalled (track/clip ids).
    std::map<const ofxOceanodeContainer*, std::string> recalledMacroClip;
    // Inside played macro timelines: what each gate, toggle and step binding
    // writes when no clip plays it (its rest), so a macro clip's end, a muted
    // macro track or a skipped clip leave nothing open.
    std::unordered_map<const ofxOceanodeTimelineParameterBinding*, std::string> restWhenInactiveCache;
    void rebuildPlayedMacros();
    const PlayedMacro* playedMacroAt(const std::string& canvas, const std::string& timeline, bool reversed = false) const;
    const PlayedMacro* playedMacroAt(const ofxOceanodeTimelineTrack& track, bool reversed = false) const;
    const PlayedMacro* playedMacroOf(const ofxOceanodeTimelineSequence& sequence) const;
    // A sequence as reversed macro clips play it: clips mirrored over length.
    void mirrorSequence(ofxOceanodeTimelineSequence& sequence, double length) const;
    // The played copies, then the main timeline: what evaluation walks.
    std::vector<ofxOceanodeTimelineSequence*> evaluatedSequences() const;
    // The sequence a track belongs to (the played copy, a placed copy, or
    // a timeline as kept here).
    const ofxOceanodeTimelineSequence& sequenceOf(const ofxOceanodeTimelineTrack& track) const;
    // Drops entries of deleted macros and keeps each shared list's base on
    // a live copy's container.
    void pruneMacroSequences() const;
    // A Global Track keyframe's values: every control (globalControlOf)
    // of `base` and the macros in it that a snapshot can hold, skipping
    // those in `bound`. Paths are relative to `base`.
    void captureGlobalValues(ofxOceanodeContainer& base,
                              const std::unordered_set<ofxOceanodeAbstractParameter*>& bound,
                              std::map<std::string, std::string>& values) const;
    // What a Global Track does with a parameter, by the rules a MacroMixer
    // morph follows (the core's ofxOceanodeMorph). control: a value the
    // user sets; false for what something else produces (Portals, a Macro's
    // pins, routers fed through their Macro's pin, cabled or read-only
    // parameters) and for node configuration, which no keyframe stores and
    // no track writes.
    struct GlobalControl { bool control = true; bool discrete = false; bool logarithmic = false; };
    GlobalControl globalControlOf(ofxOceanodeAbstractParameter& parameter) const;
    // A numeric control's value between two keyframes (t in 0..1), by the
    // MacroMixer's interpolation: lists of different lengths are resampled
    // into each other, integers round, a logarithmic control glides
    // geometrically while all its values are positive.
    static std::vector<float> blendGlobalNumbers(const std::vector<float>& from, const std::vector<float>& to,
                                                  float t, bool integers, bool logarithmic);
    struct SequenceHistory {
        std::unique_ptr<SequenceSnapshot> pending;
        std::deque<std::pair<SequenceSnapshot, SequenceSnapshot>> undo, redo; // (before, after)
    };
    mutable std::map<const ofxOceanodeTimelineSequence*, SequenceHistory> sequenceHistories;
    ofxOceanodeContainer* pendingMacroScope = nullptr;
    std::deque<TimelineEdit> timelineUndo, timelineRedo;
    // One gesture spans every history begun while it is open (the global
    // one, each timeline's): its entries carry its number, so the id-only
    // undo and redo step the latest gesture, in every history it changed.
    uint64_t gestureSerial = 0, currentGesture = 0;
    uint64_t openGesture();

    // ---- Players ----
    // Every existing timeline plays through an engine: a manager of its own
    // that evaluates, schedules and applies an absolute-path copy of it on
    // the player's transport. The primary manager plays nothing itself: it
    // keeps the timelines, the global transport's settings and the players.
    // A player runs on the root transport itself when its timeline is linked
    // to the global transport, on a transport of its own when free, and on a
    // preview transport of its own when dependent (run only while that
    // previews). Engines run inner canvases first each frame, so on a path
    // two timelines drive the outer timeline's value lands last.
    struct Player {
        TimelineRef ref;
        ClockSource source = ClockSource::Global;
        std::shared_ptr<ofxOceanodeTransport> transport;
        std::unique_ptr<ofxOceanodeTimelineManager> engine;
        std::weak_ptr<ofxOceanodeContainer> canvas; // a macro's container (empty at the root)
        const ofxOceanodeContainer* canvasAddress = nullptr;
        uint64_t sourceSignature = 0;
        const ofxOceanodeTimelineSequence* sourceSequence = nullptr;
        bool overridesCanvas = false;
        // A dependent timeline's preview ran last frame (one more pass after
        // it stops releases what it held).
        bool ranLastFrame = false;
        // Its timeline was replaced by a load: the engine still holds the
        // previous content, and writes nothing until it has the new one
        // (syncPlayer).
        bool awaitingSync = false;
        // Play on load: armed by a preset load, it starts once its macro is active.
        bool startArmed = false;
        int wasActive = -1; // -1: not watched (not set to play on load)
        // The macros around its canvas (outermost first), each with the canvas
        // holding it (the root's, or a macro's) and that canvas's node revision
        // when found: an unchanged revision keeps the macro pointer valid.
        struct MacroLink {
            const ofxOceanodeNodeMacro* macro;
            std::weak_ptr<ofxOceanodeContainer> holder;
            bool heldByRoot;
            uint64_t revision;
        };
        std::vector<MacroLink> macros;
    };
    std::set<TimelineRef> playOnLoadPending; // loaded since their players last ran
    // Whether its macro and every macro around it are active (the root's: always).
    bool macrosActive(Player& player) const;
    // Arms, starts and stops the free timelines set to play on load.
    void updatePlayOnLoad();
    std::vector<TimelineWindow> timelineWindows; // saved
    struct ClipTransport {
        std::shared_ptr<ofxOceanodeTransport> transport;
        std::weak_ptr<ofxOceanodeContainer> canvas;
        const ofxOceanodeContainer* address = nullptr;
        std::string placement;  // the placement it follows (track, clip, repeat)
        uint64_t rootGeneration = 0;
    };
    std::map<std::string, ClipTransport> clipTransports;
    // Moves each macro's clip-following transport to the clip playing it.
    void updateClipTransports(const ofxOceanodeTransportState& rootState);
    void releaseClipTransport(ClipTransport& entry);
    TimelineRef pendingTimelineScope;
    bool timelineScopeRequested = false;
    NewTimelineRequest pendingNewTimeline;
    bool newTimelineRequested = false;
    ClipboardPasteRequest pendingClipboardPaste;
    bool clipboardPasteRequested = false;
    TrackClipboard copied;
    // The copied Global Track keyframes, in beat order (copyGlobalKeyframes).
    std::vector<ofxOceanodeTimelineGlobalKeyframe> copiedGlobalKeys;
    // A copied track or clip as the timeline pasted into keeps it: paths
    // rebased from the copied canvas onto `canvas`, targets looked up.
    void rebaseCopiedTrack(ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineSequence& sequence,
                           const std::string& canvas) const;
    // Whether a copied track can be in `sequence` (of `ref`).
    bool copiedTrackFits(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineSequence& sequence,
                         const TimelineRef& ref);
    ParameterClipRequest pendingParameterClip;
    bool parameterClipRequested = false;
    std::vector<std::unique_ptr<Player>> players;
    std::set<std::string> registeredTimeSources;
    uint64_t timeSourcesFrame = 0;
    size_t timeSourcesShape = static_cast<size_t>(-1);
    // Creates, refreshes and runs the engines of the free timelines.
    void updatePlayers();
    void updateTimeSources();
    void destroyPlayer(Player& player);
    Player* playerFor(const TimelineRef& ref) const;
    // The engine copy: paths and macro canvases made absolute from canvas.
    static void makeAbsolute(ofxOceanodeTimelineSequence& sequence, const std::string& canvas);
    // Outside targets (see ofxOceanodeTimelineOutside). A canvas that has
    // just loaded ("" the root; the macros in it loaded before it) gives the
    // macro timelines below it the ones it keeps, in place of any they had
    // from it. A saved macro leaves all of its own out (levels 0: every one;
    // else only those that many canvases up).
    void restoreOutsideTargets(const std::string& canvas, const ofxOceanodeTimelineCanvas& keeper);
    bool adoptOutsideTargets(const MacroSequenceEntry& entry, const ofxOceanodeTimelineCanvas& keeper,
                             size_t depth, const std::string& macro);
    static bool stripOutsideTargets(ofxOceanodeTimelineSequence& sequence, size_t levels = 0);
    static ofJson bindingToJson(const ofxOceanodeTimelineParameterBinding& binding);
    static void bindingFromJson(const ofJson& json, ofxOceanodeTimelineParameterBinding& binding);
    static std::string absolutePathIn(const std::string& canvas, const std::string& path);
    // Content signature of one sequence (what a rebuild of its copy follows).
    uint64_t sequenceSignature(const ofxOceanodeTimelineSequence& sequence) const;
    // The same, hashed once per update on the primary: from its evaluation
    // until its last applyAutomation (the nodes' updates in between, an LFO
    // Track's sync, reuse it). Any other time, or on an edit through the
    // setters (invalidateEvaluationIndexes), it is hashed again.
    uint64_t sourceSequenceSignature(const ofxOceanodeTimelineSequence& sequence) const;
    mutable std::unordered_map<const ofxOceanodeTimelineSequence*, uint64_t> sourceSignatures;
    bool sourceSignaturesOpen = false;
    void closeSourceSignatures() { sourceSignaturesOpen = false; sourceSignatures.clear(); }
    // Engine state: the manager owning its sequences, and its transport.
    ofxOceanodeTimelineManager* primary = nullptr;
    std::shared_ptr<ofxOceanodeTransport> engineTransport;
    // The engine plays on the root transport itself (a timeline linked to
    // the global transport): it reads the transport's tempo and loop, and
    // never sets them.
    bool engineSharesRoot = false;
    // The engine playing a timeline (nullptr until its player runs), and the
    // one playing a sequence as kept here (an engine: itself, for its copy).
    ofxOceanodeTimelineManager* engineFor(const TimelineRef& ref) const;
    ofxOceanodeTimelineManager* enginePlaying(const ofxOceanodeTimelineSequence& sequence) const;
    // The players as they should be, one per timeline, made and dropped
    // without running any; an engine's copy brought up to date with its
    // source (rebuilt when the content changed) and the settings it plays
    // with. updatePlayers does both each pass; the primary also does them
    // on demand, to answer a playback question or pass a live change on.
    void syncPlayerList();
    void syncPlayer(Player& player);
    // The engine playing a timeline, current with it (its player made if
    // the timeline has none yet); nullptr on an engine, or for no timeline.
    ofxOceanodeTimelineManager* syncedEngine(const TimelineRef& ref);
    ofxOceanodeTimelineManager* syncedEnginePlaying(const ofxOceanodeTimelineSequence& sequence);
    // pianoRepeatNotes rolled here, from a sequence as kept here.
    bool pianoRepeatNotesIn(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                            const std::string& laneId, int64_t repeat, std::vector<ofxOceanodeTimelinePianoNote>& out) const;
    // Whether a timeline, as kept here, follows the global transport (its
    // position, tempo and meter; its loop is its own): not free, and not
    // placed in another timeline.
    bool followsGlobalTransport(const ofxOceanodeTimelineSequence& sequence) const;
    // Explicit live overrides (piano key previews...) by timeline, kept here
    // so an engine gets them back when its copy is rebuilt. The engine
    // notes them as they are set and cleared on it.
    std::map<TimelineRef, std::map<std::pair<std::string, std::string>, std::string>> liveOverrides;
    void noteLiveOverride(const TimelineRef& ref, const std::string& trackId, const std::string& bindingId, const std::string& value);
    void forgetLiveOverride(const TimelineRef& ref, const std::string& trackId, const std::string& bindingId);
    // After an engine's copy was rebuilt: the explicit overrides again; the
    // id-keyed state of the old copy (state-capture override keys, driven
    // solos) dropped, path-keyed state kept.
    void reapplyTransients(Player& player);
    // An engine's state-capture bookkeeping, told by the primary's edits.
    void dropStateCaptureOverride(const std::string& trackId, const std::string& bindingId);
    void noteStateCaptured(const std::string& absolutePath, const std::string& value);
    // A path as a sequence keeps it (relative to its canvas), from the root.
    std::string absolutePathOf(const ofxOceanodeTimelineSequence& sequence, const std::string& path) const;
    // Which engine's value lands last on each path this frame: it schedules
    // the path; the others leave it to the frame path.
    std::unordered_map<std::string, const ofxOceanodeTimelineManager*> lastDriverForPath;
    // The timelines placed by timeline tracks, each with the first timeline
    // placing it (its host), refreshed each update pass and after edits.
    mutable std::map<TimelineRef, TimelineRef> placedTimelines;
    mutable bool placedTimelinesValid = false;
    // A macro placed from outside is placed as a whole: its timelines no
    // track places follow the clips placing one of its timelines. Each with
    // that placed timeline (they are in placedTimelines too, with its host).
    mutable std::map<TimelineRef, TimelineRef> inheritedPlacements;
    void refreshPlacedTimelines() const;
    // Every manager playing placements now (this one, then each player's
    // engine), with the state it plays at.
    template<typename Fn> void forEachPlayingManager(Fn&& fn);
    // Which timeline the metronome follows.
    TimelineRef metronomeRef;
    // Older files: which timelines played free and whose clips a macro's
    // nodes followed, by reference, applied as their macros load.
    std::set<TimelineRef> legacyFreeTimelines;
    std::set<TimelineRef> legacyClipFollowTimelines;
    // Paths an outer timeline drives: an engine leaves them to it.
    std::function<bool(const std::string&)> pathDrivenElsewhere;
    bool isEngine() const { return primary != nullptr; }
    // The timeline an engine plays (for cycles: it never places itself).
    TimelineRef engineRef;
    // An engine's tempo when no tempo lane plays.
    float engineBpm = 120.0f;
    bool engineFollowsClockTempo = false;
    float engineTempoRatio = 1.0f;
    // An engine's Wave voices are keyed apart from the main timeline's
    // (and other engines') by this prefix on the track id.
    std::string waveKeyPrefix;
    // Releases the Wave voices of the clips in `previous` that `next` no
    // longer has (all of them when next is null).
    void releaseWaveClips(const ofxOceanodeTimelineSequence& previous, const ofxOceanodeTimelineSequence* next);
    // Scheduling stops here (a timestamped stop); 0: no limit.
    uint64_t scheduleHorizonUs = 0;
    // The transport this manager plays (its engine transport, else the root's).
    std::shared_ptr<ofxOceanodeTransport> playTransport() const;
    // Its state as played: the root's wrapped into this manager's loop
    // (loopedState); a derived transport's as it is (it loops itself).
    ofxOceanodeTransportState playState() const;
    // A live loop edit keeps this timeline's current position while the
    // shared global transport continues on its own unwrapped clock.
    void preserveLoopPlayhead();
    bool hasAdoptedPlaySettings = false;
    struct LoopPositionAnchor {
        bool valid = false;
        double clockBeat = 0.0, localBeat = 0.0;
        uint64_t pass = 0, generation = 0;
    };
    mutable LoopPositionAnchor loopPositionAnchor;
    mutable ofxOceanodeTransportState previousLoopClock;
    mutable bool hasPreviousLoopClock = false;
    // Copies a timeline's play settings into this engine's tempo, loop and meter.
    void adoptPlaySettings(const ofxOceanodeTimelinePlaySettings& settings);
    // The macro sequences entries (an engine reads its primary's).
    std::vector<MacroSequenceEntry>& sequenceEntries() const { return primary != nullptr ? primary->macroSequences : macroSequences; }
    bool replayingTimelineEdit = false;
    ofJson timelineEditData() const;
    TimelineState captureTimelineEdit() const;
    void restoreTimelineState(const TimelineState& state);
    bool stepTimelineHistory(bool backwards);
    void trimTimelineHistory();
    std::unique_ptr<ofxOceanodeTimelineMetronome> metronome;
    static std::string laneTypeToString(ofxOceanodeTimelineLaneType laneType);
    static ofxOceanodeTimelineLaneType laneTypeFromString(const std::string& laneType);
    // The codec of one sequence's content (tracks, clip groups, play
    // settings): timeline.json, sequence.json, undo snapshots and clip files
    // all go through it. A sequence keeps its id, canvas and further
    // timelines across a read.
    ofJson trackToJson(const ofxOceanodeTimelineTrack& track) const;
    ofJson sequenceContentToJson(const ofxOceanodeTimelineSequence& sequence) const;
    void sequenceContentFromJson(ofxOceanodeTimelineSequence& sequence, const ofJson& json);
    // A canvas's timelines ("timelines", "nextTimelineNumber"), and back
    // (the list's base is kept; timelines absent from the document go).
    ofJson canvasToJson(const ofxOceanodeTimelineCanvas& canvas) const;
    void canvasFromJson(ofxOceanodeTimelineCanvas& canvas, const ofJson& json);
    // A document written before version 15 (a timeline at its top level,
    // the canvas's others in "timelines") in today's shape: the top-level
    // timeline, when it existed, becomes "tl_1", and empty references to a
    // canvas's then-default timeline name it.
    static ofJson upgradeTimelineDocument(ofJson json, bool macroFile);
    std::string insertTrackCopy(ofxOceanodeTimelineTrack copy, const std::string& name, size_t index);
    std::string insertTrackCopy(ofxOceanodeTimelineSequence& sequence, ofxOceanodeTimelineTrack copy, const std::string& name, size_t index);
    // How many beats of a timeline the `seconds` ending at `beat` take, at
    // its tempo there: its own when it plays free, else the global
    // transport's (through the tempo lane when one plays).
    double beatsInSecondsBefore(const ofxOceanodeTimelineSequence& sequence, double beat, double seconds) const;
    // Engine state keyed by ids (wave voices, state-capture overrides,
    // driven solos) exists for the sequence an engine plays: its copy, in
    // playedSequence. The primary keeps the timelines there and plays none.
    bool isPlayedSequence(const ofxOceanodeTimelineSequence& sequence) const { return &sequence == &playedSequence; }
    // Keeps clipGroups' invariants (every member clip still exists, every
    // group has at least two members) after a clip is removed -- called
    // from removeClip/removeTrack rather than left to every caller.
    void removeClipFromGroups(const std::string& trackId, const std::string& clipId);
    void removeClipFromGroups(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    void clearTimelineFlag(const ofxOceanodeTimelineTrack& track);
    void refreshTimelineFlag(const std::string& parameterPath);
    // The same for a path as `sequence` keeps it (a macro's timeline: from
    // its macro's canvas, "../" in front for a parameter outside it).
    void refreshTimelineFlag(const ofxOceanodeTimelineSequence& sequence, const std::string& parameterPath);
    void releaseWaveClipIfNeeded(const std::string& trackId, const std::string& clipId);

    using ActiveValueMap = std::unordered_map<std::string,
        std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>>>;
    // Pure evaluation of every binding at one beat. evaluateAutomation() uses
    // it for the current playhead; the scheduler uses it for beats that have
    // not been reached yet, which is what makes lookahead possible without a
    // second implementation of the lane semantics. Curves whose end beat the
    // playhead rests on, or crossed since a finite previousBeat, contribute
    // one terminal sample (their exact end value).
    // chaseGaps (playback starting, a seek, a loop wrap): a curve binding no
    // clip covers at `beat` takes the value its timeline implies there -- the
    // last value of its previous clip, or else the first of its next one.
    void collectActiveValues(const ofxOceanodeTimelinePlayContext& context,
                             double beat, bool isPlaying, bool applyPianoRanges,
                             ActiveValueMap& activeValues,
                             ofxOceanodeTimelineTargetInspection* inspection = nullptr,
                             const std::set<std::string>* onlyPaths = nullptr,
                             double previousBeat = std::numeric_limits<double>::quiet_NaN(),
                             bool chaseGaps = false) const;
    bool computeParameterValue(const std::vector<ofxOceanodeTimelineParameterBinding*>& bindings,
                               const ActiveValueMap& activeValues,
                               ofxOceanodeAbstractParameter* parameter,
                               std::string& outValue) const;
    // computeParameterValue's blend of activeAutomationValues per path, kept
    // until that map changes (activeValuesRevision): the scheduler and both
    // applyAutomation passes of a frame blend each path once. The broadcast
    // and clamp after it read the parameter, and still run every time.
    struct CombinedValue { uint64_t revision = 0; std::string valueType, value; };
    mutable std::unordered_map<std::string, CombinedValue> combinedValues;
    uint64_t activeValuesRevision = 1;
    // Rebuilt once before each automation evaluation and reused by the
    // frame pass, the scheduler and the post-node re-apply. Keeping these
    // indexes here avoids repeatedly grouping the same bindings and doing
    // linear id lookups for every lane/change beat.
    void rebuildEvaluationIndexes();
    void invalidateEvaluationIndexes();
    void refreshPlaybackEnablement();
    // Beats in (fromBeat, toBeat] at which any discrete lane can change value:
    // clip edges, repeat cycles, step grid cells, step/region/note boundaries.
    void collectChangeBeats(const ofxOceanodeTimelinePlayContext& context,
                            double fromBeat, double toBeat, bool inclusiveStart,
                            std::vector<double>& outBeats) const;
    // A sequence as it sounds on the main timeline: where (its time map) and
    // which copy (the instance key its rolls mix in).
    struct PlayedSequence {
        const ofxOceanodeTimelineSequence* sequence = nullptr;
        ofxOceanodeTimelineTimeMap map;
        uint64_t instanceKey = 0;
        ofxOceanodeTimelineResolver resolver; // where its paths point
        // Main-timeline beats it plays in: the stretch of one macro clip
        // repeat (split where its content offset wraps). The main timeline
        // plays everywhere.
        double windowFrom = -std::numeric_limits<double>::infinity();
        double windowTo = std::numeric_limits<double>::infinity();
        // Mixed into the pass: each macro clip repeat rolls anew (0: main).
        uint64_t passMix = 0;
        // The macro track and clip that placed it (nullptr: the main timeline).
        const ofxOceanodeTimelineTrack* placedBy = nullptr;
        const ofxOceanodeTimelineClip* placedClip = nullptr;
        // The context its lanes roll with.
        ofxOceanodeTimelinePlayContext contextFrom(const ofxOceanodeTimelinePlayContext& context) const {
            auto copy = context;
            copy.instanceKey = instanceKey;
            copy.pass ^= passMix;
            return copy;
        }
    };
    // The sequences heard between two main-timeline beats: the main timeline,
    // then the macro timelines its macro tracks' clips place there (and
    // theirs), each through its map. Asked for one beat, only the placements
    // playing at that beat, one per macro track.
    std::vector<PlayedSequence> playedSequences(const ofxOceanodeTimelinePlayContext& context,
                                                double fromBeat, double toBeat) const;
    // The context a transport position plays with; passOffset 1 is the
    // next loop pass, which the scheduler reaches before the wrap.
    ofxOceanodeTimelinePlayContext playContextFor(const ofxOceanodeTransportState& transport,
                                                  uint64_t passOffset = 0) const;
    double beatAfterSeconds(double startBeat, double seconds, float fallbackBpm) const;
    const std::vector<std::string>& orderedAutomationPaths() const;
    uint64_t pianoInputLeadUs(const std::string& path) const;
    // Rebuilds the evaluation indexes when evaluationSignature() changed.
    void ensureEvaluationIndexes();
    uint64_t evaluationSignature() const;
    void runScheduler(const ofxOceanodeTransportState& transport, bool loopWrappedThisFrame);
    // Paths whose discrete changes are handed to a timestamping backend.
    std::vector<std::string> selectSchedulablePaths() const;
    // Retrigger gap of a lane's back-to-back events, in timeline beats.
    double laneRetriggerGapBeats(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane) const;
    void sendScheduleCorrections(const ofxOceanodeTransportState& transport, uint64_t nowUs,
                                 const std::set<std::string>* onlyPaths = nullptr);
    // Beat the playhead will have reached offsetSeconds after transport's
    // instant, following the tempo map and wrapping at the loop end.
    double beatAtOffset(const ofxOceanodeTransportState& transport, double offsetSeconds) const;
    // Value path will hold at dueUs, for a correction stamped then.
    bool scheduledValueAt(const ofxOceanodeTransportState& transport, uint64_t nowUs, uint64_t dueUs,
                          const std::string& path, ofxOceanodeAbstractParameter* parameter,
                          std::string& outValue) const;

    struct ScheduledPathState {
        bool initialized = false;
        // Value the backend is playing right now (the last event whose due
        // time has passed).
        std::string valueInEffect;
        // The value before it, and when it was replaced: a playhead slightly
        // behind the backend still reports it.
        std::string previousValueInEffect;
        uint64_t valueInEffectSinceUs = 0;
        // After a repair, values still queued before it are known to be
        // stale; they are not compared against the playhead until then.
        uint64_t repairQuietUntilUs = 0;
        // Value of the last event handed to the backend, with its due time.
        std::string lastScheduledValue;
        uint64_t lastScheduledDueUs = 0;
        std::deque<std::pair<uint64_t, std::string>> pending;
    };

    ofxOceanodeContainer* container = nullptr;
    // The root canvas's timelines (the primary's; a macro's are in
    // macroSequences). Tempo, markers, loop, time signature and view
    // settings are the global transport's on the primary.
    ofxOceanodeTimelineCanvas rootTimelines;
    // An engine: the copy of the timeline it plays, with its own tempo, loop
    // and meter adopted into the fields above. On the primary: the scratch
    // the id-only overloads edit while no edit target is set.
    ofxOceanodeTimelineSequence playedSequence;
    TimelineRef editTarget;
    std::vector<ofxOceanodeTimelineMarker> markers; // sorted by beat
    ofxOceanodeTimelineViewState viewState;
    ofxOceanodeTimelineTimebase timebase = ofxOceanodeTimelineTimebase::Beats;
    std::vector<ParameterLaneRequest> pendingParameterLanes;
    std::string pendingTrackRenameId;
    TimelineRef pendingTrackRenameRef;
    bool pendingTrackRenameIsNew = false;
    TimelineRef pendingNoteGroupSetupRef;
    mutable std::string failedLoadPath; // timeline.json that could not be read
    // Per-update cache of parameter lookups by path (see update()).
    ofxOceanodeAbstractParameter* findParameterCached(const std::string& path) const;
    // The target of an automated path for applyAutomation. One not found is
    // not looked up again for a while (a miss scans its whole canvas: a node
    // left out of a Macro Morphing source, or deleted, would be searched for
    // three times a frame), unless nodes are made or deleted meanwhile.
    ofxOceanodeAbstractParameter* findAutomatedTarget(const std::string& path);
    struct MissingTarget { uint64_t retryFrame = 0, generation = 0; };
    // Moves whenever a node is made or deleted anywhere (the core's
    // ofxOceanodeNode::graphGeneration), or a canvas id changes: a target
    // missing before may be there now, one found may be gone.
    static uint64_t nodeGraphGeneration();
    std::unordered_map<std::string, MissingTarget> missingTargets;
    // Kept across passes (an engine uses its primary's): an entry holds while
    // the node graph's generation stays and its parameter lives (alive); one
    // whose lifetime cannot be told (untracked) only for the pass that found
    // it. A miss is looked up again after a while, or once the graph moves.
    // A tracked entry also holds where it sat in its node's group: one taken
    // out (and kept alive by the node) or moved is looked up again.
    struct CachedParameter {
        ofxOceanodeAbstractParameter* parameter = nullptr;
        std::weak_ptr<void> alive;
        ofxOceanodeNodeModel* model = nullptr; // alive while the generation holds
        size_t index = 0;
        std::string name;   // a rename in place keeps identity and index: the path no longer names it
        bool tracked = false;
        uint64_t pass = 0, retryFrame = 0;
    };
    static bool stillInGroup(const CachedParameter& cached);
    mutable std::unordered_map<std::string, CachedParameter> parameterCache;
    mutable uint64_t parameterCacheGeneration = 0;
    uint64_t parameterCachePass = 0;
    bool parameterCacheActive = false;
    std::unordered_map<std::string, std::vector<ofxOceanodeTimelineParameterBinding*>> bindingsByPathCache;
    std::unordered_map<std::string, ofxOceanodeTimelineTrack*> trackByIdCache;
    struct SharedPianoPlayback {
        const ofxOceanodeTimelineLane* first = nullptr;
        ofxOceanodeTimelineLane lane;
        std::vector<const ofxOceanodeTimelineLane*> noteLanes;
        std::vector<size_t> noteIndex; // each note's index in its own lane
        ofxOceanodeTimelinePianoVoicing voicing;
    };
    struct TrackEvaluationIndex {
        std::unordered_map<std::string, ofxOceanodeTimelineParameterBinding*> bindingById;
        std::unordered_map<const ofxOceanodeTimelineClip*, size_t> clipOrder;
        std::unordered_map<const ofxOceanodeTimelineLane*, ofxOceanodeTimelinePianoVoicing> pianoVoicing;
        std::unordered_map<const ofxOceanodeTimelineLane*, std::shared_ptr<SharedPianoPlayback>> sharedPiano;
        // Drum Grid lanes: the voice count of the instrument they drive (0: none).
        std::unordered_map<const ofxOceanodeTimelineLane*, int> drumOutputWidth;
    };
    // The voice count and kit mode a piano lane plays with: the lane's mono
    // switch, its note group's override, else the instrument it drives.
    // instrumentVoices: the instrument's own count (0: not an instrument).
    // pathsCanvas: the canvas the track's binding paths are relative to
    // (a macro timeline as it is kept), "" when they are absolute.
    void pianoLaneVoicesAndKit(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane,
                               int& voices, bool& kit, int* instrumentVoices = nullptr,
                               const std::string& pathsCanvas = std::string()) const;
    void computeLaneVoicing(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineClip& clip,
                            const ofxOceanodeTimelineLane& lane, ofxOceanodeTimelinePianoVoicing& out,
                            const std::vector<uint8_t>* sounding = nullptr) const;
    // One repeat of a lane with chance or mutating notes: its notes (when
    // mutated: same order, changed pitch / start / length) and its voicing,
    // in which only the notes sounding in that repeat take a voice.
    struct PianoRepeatPlayback {
        bool mutated = false;
        ofxOceanodeTimelineLane lane; // the repeat's notes, when mutated
        ofxOceanodeTimelinePianoVoicing voicing;
    };
    // Cached per lane for a few repeats of the current clip start.
    const PianoRepeatPlayback& repeatPlayback(const ofxOceanodeTimelinePlayContext& context,
                                              const ofxOceanodeTimelineTrack& track,
                                              const ofxOceanodeTimelineClip& clip,
                                              const ofxOceanodeTimelineLane& lane,
                                              const SharedPianoPlayback* shared,
                                              int64_t repeat) const;
    // The notes a lane plays in one repeat of the current start, mutated
    // (and, with drift, from the repeat played before).
    std::vector<ofxOceanodeTimelinePianoNote> repeatNotes(const ofxOceanodeTimelinePlayContext& context,
                                                          const ofxOceanodeTimelineLane& lane, int64_t repeat,
                                                          double sourceLength) const;
    // Drift: per lane, the last few repeats played (epoch, repeat) in order.
    using PianoDriftStep = std::pair<std::pair<uint64_t, int64_t>, std::vector<ofxOceanodeTimelinePianoNote>>;
    // Where content plays: the copy (PlayContext::instanceKey) and the id of
    // a track, clip or lane in it. Ids, not pointers, so one shared content
    // can be followed separately in each copy.
    using PlayKey = std::pair<uint64_t, std::string>;
    struct PlayKeyHash {
        size_t operator()(const PlayKey& key) const {
            return std::hash<std::string>{}(key.second) ^ (key.first * 0x9e3779b97f4a7c15ULL);
        }
    };
    // Everything playback remembers between evaluations, kept off the data.
    struct PlaybackState {
        // A few repeats of each piano lane (or shared piano pool) around the
        // playhead, by (epoch, repeat).
        std::unordered_map<PlayKey, std::map<std::pair<uint64_t, int64_t>, PianoRepeatPlayback>, PlayKeyHash> repeatVoicing;
        std::unordered_map<PlayKey, std::vector<PianoDriftStep>, PlayKeyHash> pianoDrift;
        // Solo set by a track's Toggle lanes. Never saved; held after the
        // block until a lane or the user sets it again.
        std::unordered_map<PlayKey, bool, PlayKeyHash> drivenSolo;
        void clearRolls() { repeatVoicing.clear(); pianoDrift.clear(); }
    };
    mutable PlaybackState playback;
    // The pass the frame plays (set from the transport each update).
    ofxOceanodeTimelinePlayContext playContext;
    // Sets each track's driven solo from its Toggle lanes bound to it at this
    // beat. True when some track's solo changed.
    bool applyLaneTrackSolos(double beat);
    // Per-session salt so a fresh launch does not replay the same rolls.
    uint64_t clipChanceSalt = 0;
    struct PianoPitchRangeIndex {
        std::string valueType;
        int low = 127;
        int high = 0;
    };
    mutable std::unordered_map<const ofxOceanodeTimelineTrack*, TrackEvaluationIndex> trackEvaluationIndexes;
    std::unordered_map<std::string, PianoPitchRangeIndex> pianoPitchRangesCache;
    std::unordered_map<std::string, int> pianoInputPriorityCache;
    // Parameter paths routed as Pitch/Velocity/Gate of one piano lane. The
    // scheduler takes such a group whole or not at all.
    std::vector<std::vector<std::string>> pianoRoleGroupsCache;
    std::vector<std::string> automationPathOrderCache;
    std::unordered_set<const ofxOceanodeTimelineParameterBinding*> zeroWhenInactiveBindingsCache;
    std::unordered_set<std::string> continuouslyEvaluatedPathsCache;
    // State-capture curves let a stopped transport be auditioned from the node
    // GUI: an external parameter edit becomes a temporary live override until
    // the next capture commits it as a point (or playback resumes).
    std::unordered_map<std::string, std::string> stateCaptureLastAppliedValues;
    // Global Tracks (an engine's): one channel per parameter whose value
    // differs between the keyframes of a global clip, rebuilt with the
    // evaluation indexes. Numeric channels interpolate; the others hold the
    // keyframe before the playhead. A path that is not a control
    // (globalControlOf; older keyframes stored some) is never written.
    struct GlobalChannel {
        std::string path;                        // absolute
        int kind = 0;                            // 0: not resolved yet, 1: interpolated, 2: held, 3: not a control
        bool logarithmic = false;                // interpolated: geometric glide
        std::vector<std::vector<float>> numbers; // per keyframe (interpolated; empty where absent)
        std::vector<const std::string*> texts;   // per keyframe (into the keyframes' maps; null where absent)
        // A path not found (its node left out of a Macro Morphing source, or
        // deleted) is looked up again from this frame on, or as soon as the
        // node graph changes from this generation (ofxOceanodeNode::
        // graphGeneration), not on every pass: a miss scans its whole canvas.
        uint64_t retryFrame = 0;
        uint64_t retryGeneration = 0;
        // The parameter it last found (compared, never dereferenced): another
        // one (its node made anew) is written even if the value has not moved.
        const ofxOceanodeAbstractParameter* foundParameter = nullptr;
    };
    struct GlobalClipChannels {
        // The played timeline's copy, or the copy of a timeline its clips
        // place (a PlayedMacro's): that one plays where a clip places it.
        const ofxOceanodeTimelineSequence* sequence = nullptr;
        const ofxOceanodeTimelineTrack* track = nullptr;
        const ofxOceanodeTimelineClip* clip = nullptr;
        std::vector<double> beats;
        std::vector<ofxOceanodeTimelineCurveTension> eases; // per keyframe: to the next one
        std::vector<GlobalChannel> channels;
    };
    // The played timeline's clips first, then the placed timelines', outer
    // ones first: an outer Global Track moving a parameter keeps it.
    std::vector<GlobalClipChannels> globalChannelsCache;
    bool globalChannelsValid = false;
    // Which placement (macro clip and repeat) last played each placed
    // timeline's Global Track, by timeSourceId: when another one starts, the
    // track writes its values again (whatever happened in between).
    std::unordered_map<std::string, uint64_t> globalPlacementSeen;
    // What a Global Track last computed for a path. A value is written only
    // when it differs from this, never because the parameter does: a
    // parameter stays editable by hand wherever the track's value holds
    // still (stopped, or between two equal keyframes).
    // blended: the last write went through the parameter's automation filter
    // (a MacroMixer morph blending the track in), so it was not the track's
    // own value and the next write is due whatever the track computes.
    struct GlobalApplied { std::vector<float> numbers; std::string text; bool blended = false; };
    std::unordered_map<std::string, GlobalApplied> globalLastApplied;
    uint64_t globalLoadCountSeen = 0;
    // A Global Track writes a value only when it differs from what it last
    // wrote, so a repeat of a pass (the host applies again after the morph
    // engines and after the nodes ran) with the same inputs writes nothing:
    // it is skipped. Its inputs: the evaluation it follows, the playhead, the
    // node graph, the automation filters, the channels and the preset loads.
    struct GlobalPassKey {
        uint64_t evaluation = 0;
        double beat = 0.0;
        uint64_t nodes = 0, filters = 0, channels = 0, loads = 0;
        bool operator==(const GlobalPassKey& other) const {
            return evaluation == other.evaluation && beat == other.beat && nodes == other.nodes &&
                   filters == other.filters && channels == other.channels && loads == other.loads;
        }
    };
    GlobalPassKey globalPassKey;
    bool globalPassDone = false;
    // A channel reached in the last full pass had an automation filter (a
    // morph blending it in): what it writes moves, so repeats run.
    bool globalPassFiltered = false;
    uint64_t evaluationCount = 0;     // goes up with every evaluateAutomation
    uint64_t globalChannelsBuild = 0; // goes up with every rebuildGlobalChannels
    // The parameters a Global Track is moving right now (the playhead is
    // between two keyframes that differ for them), with their track's
    // colour: the node GUI marks them like a track's own parameters. Kept by
    // the primary, noted by its engines on every pass.
    std::unordered_map<std::string, ofColor> globalMovingColors;
    uint64_t globalMovingFrame = std::numeric_limits<uint64_t>::max();
    void noteGlobalMoving(const std::string& path, const ofColor& color);
    void rebuildGlobalChannels();
    void applyGlobalTracks();
    // Paths bound by curve lanes of state-capture clips (the only readers of the map above).
    std::unordered_set<std::string> stateCapturePathsCache;
    uint64_t evaluationIndexesSignature = 0;
    std::unordered_set<std::string> stateCaptureManualOverrideKeys;
    bool evaluationIndexesValid = false;
    // Within one update pass (beginParameterCache to endParameterCache) only
    // this manager edits its model, and it invalidates the indexes when it
    // does: the model is hashed once per pass, not once per call.
    bool evaluationHashedThisPass = false;
    // Instrument roles are adopted when the model changed, else now and then.
    uint64_t adoptedInstrumentsSignature = 0;
    uint64_t adoptedInstrumentsFrame = 0;
    // Whether any track of a sequence is soloed, cached for the pass (solos
    // change through the GUI between passes, or through applyLaneTrackSolos,
    // which clears it).
    mutable std::unordered_map<const ofxOceanodeTimelineSequence*, bool> anySoloCache;
    bool anySoloCacheActive = false;
    // The node GUI asks this for every visible parameter. Cache path -> color
    // once per frame instead of scanning every track/binding for every row.
    void rebuildParameterTrackColorCache() const;
    mutable std::unordered_map<std::string, ofColor> parameterTrackColorCache;
    mutable uint64_t parameterTrackColorCacheFrame = std::numeric_limits<uint64_t>::max();
    // beatToSeconds(): seconds at each tempo point (see there).
    mutable std::vector<double> tempoSecondsAtPoint;
    mutable uint64_t tempoSecondsCacheSignature = 0;
    mutable bool tempoSecondsCacheValid = false;
    // True when a tempo map applies; refreshes tempoSecondsAtPoint if it changed.
    bool refreshTempoCache() const;
    // Seconds from tempo point i - 1 to `fraction` of the segment ending at point i.
    double tempoSegmentSeconds(size_t i, double fraction) const;
    // A piano roll narrows its pitch parameter's range to the keys it shows;
    // the original range is kept here and restored when no piano roll drives
    // that parameter any more.
    struct SavedParameterRange {
        std::string valueType;
        float floatMin = 0.0f, floatMax = 0.0f;
        int intMin = 0, intMax = 0;
        std::vector<float> floatVectorMin, floatVectorMax;
        std::vector<int> intVectorMin, intVectorMax;
    };
    mutable std::map<std::string, SavedParameterRange> savedPianoRanges;
    ofxOceanodeAbstractParameter* pendingNoteGroupSetupParameter = nullptr;
    // Drops dangling roles, removes empty groups and keeps each group's
    // members adjacent (in pitch, gate, velocity order) in the binding list.
    void normalizeNoteGroups(ofxOceanodeTimelineTrack& track);
    // The two halves of a clip cut at a timeline beat (see splitClip).
    bool computeClipSplit(const ofxOceanodeTimelineClip& original, double timelineBeat,
                          ofxOceanodeTimelineClip& left, ofxOceanodeTimelineClip& right) const;
    // The content of both parts of a cut at source beat `cut`: left keeps
    // [0, cut) (one-shots only), right gets [cut, end) then, when repeating,
    // [0, cut) -- the content rotated to start at the cut.
    void cutClipContent(const ofxOceanodeTimelineClip& original, double cut, bool repeating,
                        ofxOceanodeTimelineClip& left, ofxOceanodeTimelineClip& right) const;
    // Rewrites the lanes so the content starts where it plays from, and
    // clears contentOffsetBeats: before edits that cut or re-lay content.
    void bakeContentOffset(ofxOceanodeTimelineClip& clip) const;
    // Rewrites a clip's lanes as their time-mirror, so the clip plays what it
    // played backwards (same start, length, repeat and tempo).
    void mirrorClipContent(ofxOceanodeTimelineClip& clip) const;
    // The content offset at which a clip played backwards over mirrored
    // content plays exactly what it played forwards at `offset`.
    double reversedContentOffset(const ofxOceanodeTimelineClip& clip, double offset) const;
    // The root transport's state as this manager plays it: its position
    // wrapped into this manager's loop (the loop's pass as loopCount), the
    // loop in the state. The root transport itself never loops, so every
    // timeline linked to it loops on its own while it runs on. Identity
    // while the loop is off or the position is before it.
    ofxOceanodeTransportState loopedState(ofxOceanodeTransportState state) const;
    static bool& loopsSuspendedFlag() { static bool flag = false; return flag; }
    // Points every piano-roll lane that uses any of affectedBindingIds at the group's roles.
    void syncNoteGroupLanes(ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineNoteGroup& group,
                            const std::vector<std::string>& affectedBindingIds);
    void updatePianoGateOwnership(bool isPlaying);
    // Toggle, Multi Gate, Multi Value and Step lanes whose clip is under the
    // playhead but stops being heard (its track muted or left out by a solo,
    // the clip or lane muted) write their rest once, as a clip its chance
    // leaves out does; otherwise a gate left at 1 stays at 1.
    void releaseSilencedRests(const ofxOceanodeTransportState& transport);
    // Gives note groups that never had a Duration / Filter role their
    // instrument's input (see ofxOceanodeTimelineNoteGroup::durationDecided).
    // Not an undoable edit: history snapshots get the same role.
    void adoptInstrumentDurations();
    // which: 0 Duration, 1 Filter, 2 Stutter.
    bool setNoteGroupKitRole(const std::string& trackId, const std::string& groupId,
                             const std::string& bindingId, int which);
    bool setNoteGroupKitRole(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                             const std::string& bindingId, int which);
    std::unordered_set<std::string> playingPianoGatePaths;
    // Gates closed by the last updatePianoGateOwnership().
    std::vector<std::string> releasedPianoGatePaths;
    // Gates closed once since playback stopped (or the engine was made or
    // reloaded): a gate a preset left open is shut while stopped, so the first
    // note on play is a fresh rising edge. Live input owns them afterwards.
    std::unordered_set<std::string> restedGatePaths;
    // Rest values of the gate-type lanes heard last frame, by parameter path.
    std::unordered_map<std::string, std::string> soundingRestPaths;
    ActiveValueMap activeAutomationValues;
    bool bpmAutomationEnabled = false;
    bool bpmLaneCollapsed = true;
    bool bpmLaneVisible = false; // hidden by default; toggled from the ruler
    float bpmMinimum = 20.0f;
    float bpmMaximum = 300.0f;
    std::vector<ofxOceanodeTimelineCurvePoint> bpmAutomationPoints;
    std::vector<ofxOceanodeTimelineCurveTension> bpmCurveTensions;
    std::string bpmInterpolation = "Linear";
    bool loopEnabled = false;
    bool playOnPresetLoad = false;
    double loopStartBeat = 0.0;
    double loopEndBeat = 4.0;
    bool loopWrappedThisFrame = false;
    // Loop wrapping is an edge-triggered transport operation. Remembering
    // the previous evaluated beat prevents a wrapped transport from being
    // wrapped again (or from being interpreted as an ordinary forward jump)
    // while the audio provider is being resynchronised.
    bool hasEvaluatedTransportBeat = false;
    double lastEvaluatedTransportBeat = 0.0;
    // Whether the last evaluated frame was playing: the first playing frame
    // after it chases curve values into the gaps between clips.
    bool playingAtLastEvaluation = false;
    // Back-to-back repeated notes/steps/gate blocks (one ends exactly where
    // the next, equal one starts) get a short gate-off at the join so they
    // retrigger instead of merging into one long gate (overlapping ones do
    // merge). In timeline beats; long enough (>= 1.2 frames, >= 10 ms) that
    // the per-frame path always sees it, and the scheduler uses the same value.
    double retriggerGapBeats = 1.0 / 48.0;
    // The gap of lanes whose targets are all scheduled (a few milliseconds).
    double scheduledRetriggerGapBeats = 1.0 / 160.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    bool schedulingEnabled = true;
    double masterGridBeats = 0.25;
    // The longest window; the scheduler uses less while frames are short.
    double schedulingLookaheadMs = 120.0;
    bool schedulingLookaheadAdaptive = true;
    // Longest recent frame interval, decaying over a few seconds.
    double frameIntervalPeakSeconds = 1.0 / 60.0;
    // Selected before the frame's evaluation; the scheduled lanes' paths.
    std::vector<std::string> schedulablePathsThisFrame;
    std::unordered_set<std::string> fastRetriggerPaths;
    bool hasScheduleCursor = false;
    // invalidateSchedule() was called: the next runScheduler() corrects what
    // was queued, once the transport position it must correct towards is known.
    bool scheduleInvalidationPending = false;
    double scheduleCursorBeat = 0.0;
    // Loop pass the cursor belongs to (it can already be in the next pass,
    // when events past the loop end were scheduled before the wrap).
    uint64_t scheduleCursorPass = 0;
    uint64_t lastSeenLoopCount = 0;
    bool hasSeenLoopCount = false;
    bool scheduleUsesTempoMap = true; // false while an external clock sets the tempo
    uint64_t lastSeenTransportGeneration = 0;
    bool hasSeenTransportGeneration = false;
    std::map<std::string, ScheduledPathState> scheduledPaths;
    // Paths whose backend is driven by the scheduler: their ordinary
    // frame-rate send is suppressed (the value still reaches the parameter,
    // the GUI and the node graph as usual).
    std::set<std::string> scheduledBackendPaths;
    // Not owned -- see setWaveAudioProvider above.
    ofxOceanodeTimelineWaveAudioProvider* waveAudioProvider = nullptr;

    // ---- Macro Morphing Track engines (ofxOceanodeTimelineMorph.cpp) ----
    // The primary's: an engine (a manager playing a copy) has none and asks
    // its primary. One per timeline that has a Macro Morphing Track.
    struct MorphEngine;
    // A live graph of a timeline's morph engine: the source it plays and
    // the canvas of the Macro holding it (where its timelines live).
    struct MorphGraph {
        std::string source;
        const ofxOceanodeContainer* canvas = nullptr;
        std::string canvasId;
    };
    std::vector<MorphGraph> morphGraphsOf(const TimelineRef& ref) const;
    // The timelines of those graphs' canvases, each with the timeline
    // holding the morph track: they play where its clips place them.
    std::vector<std::pair<TimelineRef, TimelineRef>> morphPlacedTimelines() const;
    MorphEngine* findMorphEngine(const ofxOceanodeTimelineSequence& sequence) const;
    // The engine of the timeline `ref`, whose morph track this is, made
    // when it has none. follow: an engine whose track's state was replaced
    // behind it (an undo, a redo, a load) is restored from it; its playing
    // sources go, so only the frame pass asks for that.
    MorphEngine* ensureMorphEngine(const TimelineRef& ref, const ofxOceanodeTimelineTrack& track, bool follow);
    // Once per frame, before the players run: the engines there should be,
    // each told the scene at its timeline's playhead. After them: each
    // engine's update and its outputs.
    void presentMorphScenes();
    void updateMorphEngines();
    // One engine's update and outputs, once per frame.
    // (False: it had updated this frame already, or has no runtime.)
    bool updateMorphEngineNow(MorphEngine& engine);
    // Releases the engines (rootOnly: those of the root canvas's timelines).
    void releaseMorphEngines(bool rootOnly);
    void releaseMorphEngine(const TimelineRef& ref);
    // The engine's live graphs changed: indexes, placements and schedules follow.
    void noteMorphGraphs(MorphEngine& engine);
    // Writes the engine's state into its track when it differs.
    void storeMorphState(ofxOceanodeTimelineSequence& sequence, MorphEngine& engine);
    // The answers of the blocked-node dialog, and the sources waiting on them.
    void applyMorphAnswers(const TimelineRef& ref, MorphEngine& engine);
    // The scene a morph track holds now, for its engine (see presentMorphScenes).
    void presentMorphScene(const TimelineRef& ref, const ofxOceanodeTimelineSequence& sequence,
                           const ofxOceanodeTimelineTrack& track, MorphEngine& engine);
    // Where a morph track's scene is read: the timeline's playhead; a
    // dependent timeline's where a clip places it or its preview plays
    // (false: nowhere, nothing plays).
    bool morphPlayheadBeat(const TimelineRef& ref, double& beat);
    // One pass of the first timeline saved in a source folder, in beats (0: none).
    double morphSourceLength(const std::string& folder);
    // Whether a Macro still owns this canvas (a macro's container).
    bool macroCanvasAlive(const ofxOceanodeContainer& canvas) const;
    // A source's timelines, by its folder (one spelling per folder).
    struct MorphDocument {
        std::shared_ptr<ofxOceanodeTimelineCanvas> timelines;
        std::string folder;
        bool macroFile = false;          // read from sequence.json (else the preset's timeline.json)
        uint64_t savedSignature = 0;     // as last read from or written to the folder
        uint64_t seenSignature = 0;      // as last looked at (seenAt)
        float seenAt = -1.0f;
    };
    mutable std::map<std::string, MorphDocument> morphDocuments;
    // Writes a document's timelines into its folder's own file.
    bool writeMorphDocument(MorphDocument& document, std::string& error) const;
    // The source's patch, saved from the graph playing it alone (Morph.cpp).
    bool saveMorphSourcePatch(const ofxOceanodeTimelineSequence& host, const std::string& sourceId, std::string& error);
    std::map<std::string, std::string> morphDocumentKeys;   // a folder as spelled -> its key
    MorphDocument* morphDocumentOf(const std::string& folder);
    // The same, read from the folder when no graph of the source has loaded yet.
    MorphDocument* ensureMorphDocument(const std::string& folder);
    uint64_t morphDocumentSignature(const MorphDocument& document) const;
    void dropMorphDocument(const std::string& folder);
    // The documents that differ from their folders (looked at a few times a second).
    std::vector<MorphDocument*> dirtyMorphDocuments();
    // A project load starts from the folders again: a source has one
    // version, its folder's. Edits not written there with Save Source are
    // kept for the session only, never in the project.
    void clearMorphDocuments();
    bool isMorphDocument(const ofxOceanodeTimelineCanvas* timelines) const;
    // The folders of every engine's sources, as the engines spell them.
    std::vector<std::string> morphSourceFolders() const;
    std::string morphSourceFolder(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const;
    uint64_t morphFrame = 0;
    std::string morphError;
    float morphErrorAt = 0.0f;
    // The frame a Timeline node last published each timeline's routers.
    std::map<TimelineRef, uint64_t> morphPublishers;
    // Last: released first, while everything their graphs call back into is there.
    std::map<TimelineRef, std::shared_ptr<MorphEngine>> morphEngines;
};

class ofxOceanode;
class ofxOceanodeContainer;
namespace ofxOceanodeTimeline {
// Attach after oceanode.setup(), before loading presets or updating nodes.
void setup(ofxOceanode& oceanode);
// Headless attachment: no editor/controller or node registration.
ofxOceanodeTimelineManager& attach(ofxOceanodeContainer& container);
}

#endif
