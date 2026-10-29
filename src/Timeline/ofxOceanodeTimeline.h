#ifndef ofxOceanodeTimeline_h
#define ofxOceanodeTimeline_h
#ifndef OFX_OCEANODE_HAS_TIMELINE
#define OFX_OCEANODE_HAS_TIMELINE 1
#endif

#include "ofMain.h"
#include "ofxOceanodeTransport.h"
#include "ofxOceanodeScheduling.h"
#include "ofxOceanodeTimelineMetronome.h"
#include <algorithm>
#include <cstdint>
#include <deque>
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
class ofxOceanodeNodeModel;

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
    DrumGrid
};

// Lane types that drive an instrument through the note roles (pianoPitch /
// pianoGate / pianoVelocity binding ids) instead of plain bindingIds.
inline bool ofxOceanodeTimelineLaneUsesNoteRoles(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::PianoRoll || type == ofxOceanodeTimelineLaneType::DrumGrid;
}

// Number and Dropdown lanes hold absolute values in curvePoints, each until
// the next point -- no normalization, no interpolation.
inline bool ofxOceanodeTimelineIsHeldValueLane(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::Number || type == ofxOceanodeTimelineLaneType::Dropdown;
}

// The option names of a scalar int/float dropdown parameter, empty for any
// other parameter.
std::vector<std::string> ofxOceanodeTimelineDropdownOptions(ofxOceanodeAbstractParameter& parameter);

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
enum class ofxOceanodeTimelineTargetRole { Lane, Pitch, Gate, Velocity, LfoOutput, Duration, Filter, Stutter };

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
    bool muted = false;
    bool solo = false;
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
    bool pianoLiveEditSound = false;
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
    // LFO clips use ordinary Curve data for their parameter automation, but
    // keep the semantic role here so the evaluator and editor do not have to
    // infer it from the user-facing lane name.
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
    // clips keep their file position in waveSourceStartBeat, LFOs their phase.
    double contentOffsetBeats = 0.0;
    // A macro clip (on a macro track) plays its macro's timeline through its
    // own timing: content length = one pass of that timeline, stretch = its
    // tempo ratio, offset = which section plays. The macro snapshot recalled
    // when it starts (-1: none).
    int macroSnapshot = -1;
    bool muted = false; // Preserve content while removing this clip from playback.
    // Probability (0..1) that the whole clip plays. Rolled again every time
    // the clip starts: each play/seek and each loop pass. The result is not
    // stored here: see ofxOceanodeTimelineClipChanceSilenced().
    float chance = 1.0f;
    // An LFO clip is a self-contained continuous modulator. Its Curve lanes
    // describe the oscillator controls and this binding receives the final
    // normalized result mapped through lfoOutputMin/lfoOutputMax.
    bool isLfo = false;
    // Created by "Capture state as curve points". A track keeps at most one
    // such clip and grows it as captures are made at new playhead positions.
    // This is persisted so the workflow continues after loading a preset.
    bool isStateCapture = false;
    std::string lfoOutputBindingId;
    float lfoOutputMin = 0.0f;
    float lfoOutputMax = 1.0f;
    // Audio clips belong to a Wave track. These fields intentionally live on
    // the clip rather than on a lane: a Wave track can contain several
    // independently placed files. Volume is not here -- it belongs to the
    // whole track (gain plus an envelope in timeline-beat space), which is
    // what evaluateWaveClipVolume resolves.
    std::string waveFilePath;
    float waveGain = 1.0f;
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
    int waveNumChannels = 0;
    double waveFileDurationMs = 0.0;
    std::vector<float> waveformPeaks;
    std::vector<ofxOceanodeTimelineLane> lanes;
};

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
// macro clip will give its sequence the map of the repeat cycle under the
// playhead (its own Repeat stays in ofxOceanodeTimelineClipTime). Both
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

// One LFO track sampled at a timeline beat (see ofxOceanodeTimelineManager::evaluateLfoTrack).
struct ofxOceanodeTimelineLfoSample {
    bool active = false;  // an LFO clip covers this beat
    float value = 0.0f;   // shaped LFO result, normalized 0..1 (as the clip draws it)
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

    // Evaluates the complete LFO result in normalized 0..1 space. The shape
    // is intentionally shared by playback and the editor's result view.
    float evaluate(const ofxOceanodeTimelineClip& clip, double sourceBeat);
    // Oscillator cycles accumulated from source beat 0 (integrated frequency).
    double cyclesAt(const ofxOceanodeTimelineClip& clip, double sourceBeat);
    float evaluateParameter(const ofxOceanodeTimelineClip& clip,
                            const std::string& parameter,
                            double sourceBeat, float fallback);
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

struct ofxOceanodeTimelineTrack {
    std::string id;
    std::string name;
    ofColor color = ofColor(65, 165, 245, 255);
    bool collapsed = false;
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
    // Height of the single Wave Track row in the timeline. Not persisted:
    // editor layout (lane heights, collapsed lanes, zoom, snap) is session
    // state everywhere else in the timeline, and this was the one exception.
    float waveTrackHeight = 96.0f;
    // Track-wide volume controls. The automation points live in global
    // timeline-beat space and affect every clip on this Wave Track.
    float waveVolume = 1.0f;
    bool waveVolumeAutomationEnabled = true;
    std::string waveVolumeInterpolation = "Linear";
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
    std::string macroCanvas;
    bool isMacroTrack() const { return !macroCanvas.empty(); }
};
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

// Finds parameters by timeline path relative to the container a sequence
// lives in: the root for the main timeline (absolute paths, as saved in
// every file), a macro's container for that macro's sequence -- so one
// shared sequence reaches each copy's own parameters.
struct ofxOceanodeTimelineResolver {
    ofxOceanodeContainer* base = nullptr;
    ofxOceanodeAbstractParameter* find(const std::string& path) const;
    // Empty when the parameter is not inside base: a sequence only drives
    // what is inside the container it belongs to.
    std::string pathOf(ofxOceanodeAbstractParameter& parameter) const;
};

// The arrangement of one timeline: its tracks (with their bindings, clips and
// lanes), clip groups and the counters new ids start from. The main timeline
// is one; a macro will own one. Data and plain lookups only: parameters,
// playback, scheduling and undo stay with the manager.
class ofxOceanodeTimelineSequence {
public:
    std::vector<ofxOceanodeTimelineTrack> tracks;
    std::vector<ofxOceanodeTimelineClipGroup> clipGroups;
    // The container its binding paths are relative to (not saved; nullptr:
    // the timeline's own root, as for the main timeline).
    ofxOceanodeContainer* base = nullptr;
    // First number tried for each kind of new id.
    uint64_t nextTrackNumber = 1;
    uint64_t nextBindingNumber = 1;
    uint64_t nextClipNumber = 1;
    uint64_t nextLaneNumber = 1;
    uint64_t nextGroupNumber = 1;

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

class ofxOceanodeTimelineManager {
public:
    // Editing operations come in pairs: one takes the Sequence to edit, the
    // other edits the main timeline (mainSequence). Undo, playback and the
    // per-frame caches follow the main timeline only.
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
    void setLiveOverride(const std::string& trackId, const std::string& bindingId, const std::string& value);
    void clearLiveOverride(const std::string& trackId, const std::string& bindingId);
    // The piano roll's keyboard: a key sounds in one voice (a kit: its own
    // pad) through live overrides on the lane's gate and pitch, until cleared.
    void setPianoKeyPreview(const std::string& trackId, const std::string& clipId, const std::string& laneId, int pitch);
    void clearPianoKeyPreview(const std::string& trackId, const std::string& clipId, const std::string& laneId);
    void clear();

    std::string createTrack(const std::string& requestedName = "Timeline Track");
    std::string createTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Timeline Track");
    // A track with no bindings, dedicated to Wave clips (see
    // ofxOceanodeTimelineTrack::isWaveTrack). Reuses createTrack for the
    // usual id/uniqueness/list bookkeeping and just marks the result.
    std::string createWaveTrack(const std::string& requestedName = "Wave Track");
    std::string createWaveTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName = "Wave Track");
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
    const std::vector<ofxOceanodeTimelineTrack>& getTracks() const { return mainSequence.tracks; }
    // The main timeline's arrangement, read-only (folder depth, parents...).
    const ofxOceanodeTimelineSequence& getMainSequence() const { return mainSequence; }
    ofxOceanodeTimelineSequence& getMainSequence() { return mainSequence; }

    // ---- Macro tracks: clips that play a macro's own timeline ----
    std::string createMacroTrack(ofxOceanodeContainer& macroContainer);
    std::string createMacroTrack(ofxOceanodeTimelineSequence& sequence, ofxOceanodeContainer& macroContainer);
    // A clip on a macro track. A content length of 0 uses the macro
    // timeline's length (the end of its last clip, at least one bar).
    std::string createMacroClip(const std::string& trackId, double startBeat, double durationBeats = 0.0);
    std::string createMacroClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                double startBeat, double durationBeats = 0.0);
    // The live container of the macro a macro track plays (nullptr: gone).
    ofxOceanodeContainer* findMacroContainer(const ofxOceanodeTimelineSequence& sequence,
                                             const ofxOceanodeTimelineTrack& track) const;
    // One pass of a macro timeline: the end of its last clip, at least a bar.
    double macroTimelineLength(const ofxOceanodeTimelineSequence& macroSequence) const;

    // ---- Macro timelines: each macro's own sequence ----
    // The sequence of the macro owning this container (created empty on first
    // use, its paths relative to the macro); nullptr when no macro owns it.
    // Copies of a library macro share one; a local macro owns its own.
    ofxOceanodeTimelineSequence* getMacroSequence(ofxOceanodeContainer& macroContainer);
    // How the editor names it: the macro's title, else its canvas id.
    std::string getMacroSequenceLabel(ofxOceanodeContainer& macroContainer) const;
    // Called with the macro's own preset or library folder (sequence.json).
    void loadMacroSequence(ofxOceanodeContainer& macroContainer, const std::string& folder);
    bool saveMacroSequence(const ofxOceanodeContainer& macroContainer, const std::string& folder) const;
    // False once the macro it belongs to is gone (an editor scope then closes).
    bool isLiveSequence(const ofxOceanodeTimelineSequence& sequence) const;
    // A sequence's own data as JSON ("tracks", "clipGroups"), and back.
    ofJson sequenceToJson(const ofxOceanodeTimelineSequence& sequence) const;
    void sequenceFromJson(ofxOceanodeTimelineSequence& sequence, const ofJson& json) const;
    // Undo per sequence: the main timeline's is the timeline history above,
    // a macro sequence keeps its own.
    void beginTimelineEdit(const ofxOceanodeTimelineSequence& sequence);
    void endTimelineEdit(const ofxOceanodeTimelineSequence& sequence);
    bool undoTimelineEdit(ofxOceanodeTimelineSequence& sequence);
    bool redoTimelineEdit(ofxOceanodeTimelineSequence& sequence);
    // "Edit timeline" from a macro's node menu, picked up by the editor.
    void requestMacroScope(ofxOceanodeContainer& macroContainer) { pendingMacroScope = &macroContainer; }
    ofxOceanodeContainer* consumeMacroScopeRequest() { auto* requested = pendingMacroScope; pendingMacroScope = nullptr; return requested; }

    // Resolves a sequence's binding paths (its base, else this timeline's root).
    ofxOceanodeTimelineResolver resolverFor(const ofxOceanodeTimelineSequence& sequence) const {
        return {sequence.base != nullptr ? sequence.base : container};
    }

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
    bool requestParameterLane(const std::string& trackId, ofxOceanodeAbstractParameter& parameter);
    std::vector<std::pair<std::string, std::string>> consumePendingParameterLanes() {
        std::vector<std::pair<std::string, std::string>> requests;
        requests.swap(pendingParameterLanes);
        return requests;
    }
    void requestTrackRename(const std::string& trackId, bool isNewTrack = false);
    bool consumePendingTrackRename(std::string& trackId, bool* isNewTrack = nullptr);
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
    bool drumGridInstrument(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane,
                            int& voices, std::vector<std::string>& names, bool* kit = nullptr,
                            std::vector<ofColor>* colors = nullptr) const;
    // The same for a note group's roles.
    bool noteGroupInstrument(const std::string& trackId, const std::string& groupId,
                             int& voices, bool& kit, std::vector<std::string>* names = nullptr) const;
    // "Add to Timeline as Piano Roll..." from a parameter's right-click menu:
    // queued here, shown by the timeline controller (like track renames).
    void requestNoteGroupSetup(ofxOceanodeAbstractParameter* parameter) { pendingNoteGroupSetupParameter = parameter; }
    ofxOceanodeAbstractParameter* consumePendingNoteGroupSetup() {
        auto* parameter = pendingNoteGroupSetupParameter;
        pendingNoteGroupSetupParameter = nullptr;
        return parameter;
    }
    const ofxOceanodeTimelineParameterBinding* getBinding(const std::string& trackId, const std::string& bindingId) const;
    const ofxOceanodeTimelineParameterBinding* getBinding(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId) const;
    // The node a piano lane's pitch binding drives (nullptr when it has none):
    // what Global Pitch makes of the lane's notes depends on that node's ticks.
    ofxOceanodeNodeModel* pianoPitchNode(const std::string& trackId, const ofxOceanodeTimelineLane& lane) const;

    std::string createClip(const std::string& trackId,
                           const std::string& requestedName = "Clip",
                           double startBeat = 0.0,
                           double durationBeats = 4.0);
    std::string createClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::string& requestedName = "Clip",
                           double startBeat = 0.0,
                           double durationBeats = 4.0);
    std::string createLfoClip(const std::string& trackId,
                              const std::string& outputBindingId = std::string(),
                              const std::string& requestedName = "LFO",
                              double startBeat = 0.0,
                              double durationBeats = 4.0);
    std::string createLfoClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                              const std::string& outputBindingId = std::string(),
                              const std::string& requestedName = "LFO",
                              double startBeat = 0.0,
                              double durationBeats = 4.0);
    // Samples the LFO clips of a track at a timeline beat. A pure function of the
    // beat, so playback, scrubbing and seeking give identical results. Between or
    // outside clips: active = false, phase 0, cycle holds the count reached so far.
    ofxOceanodeTimelineLfoSample evaluateLfoTrack(const std::string& trackId, double beat) const;
    // Adds any LFO control lane the clip is missing (e.g. clips saved before a
    // control existed), initialised to the control's default.
    void ensureLfoLanes(const std::string& trackId, const std::string& clipId);
    void ensureLfoLanes(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    bool renameClip(const std::string& trackId, const std::string& clipId, const std::string& requestedName);
    bool renameClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, const std::string& requestedName);
    // Cuts an automation or LFO clip in two at a timeline beat; the right
    // part becomes a new clip (returned) and both halves play exactly what
    // the original played. Repeating clips are cut anywhere (the right part
    // keeps repeating from the phase it was cut at). Not for Wave clips.
    std::string splitClip(const std::string& trackId, const std::string& clipId, double timelineBeat);
    // Joins two clips of one track into one plain clip spanning both (no
    // repeat, stretch or offset: each clip's content is laid out as it
    // plays). Lanes driving the same parameters with the same kind of lane
    // become one lane; the others are kept as separate lanes holding data only
    // over their own clip's span. The earlier clip keeps its id and settings,
    // the other is removed. Refused for Wave, LFO and state-capture clips, and
    // for a Drum Grid that would need a row longer than its maximum.
    // Returns the merged clip's id ("" when refused).
    std::string mergeClips(const std::string& trackId, const std::string& firstClipId, const std::string& secondClipId);
    std::string mergeClips(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                           const std::string& firstClipId, const std::string& secondClipId);
    std::string splitClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double timelineBeat);
    // Copies a clip, with fresh clip/lane ids, to dstTrackId at newStartBeat
    // and returns the new clip's id ("" when it cannot go there).
    //  - dstBindingId empty: the whole clip. On another track every binding is
    //    remapped to that track's binding of the same parameter; lanes with no
    //    match are dropped (nothing left: refused).
    //  - dstBindingId set: only the lane srcLaneId, now driving that binding (a
    //    piano roll needs a note group's row and takes its pitch/gate/velocity;
    //    an LFO clip is copied whole with its output moved there).
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
    // LFO clips (whose phase runs from the clip start) fall back to trimClipStart.
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
    // Fixed-role LFO and legacy Wave lanes cannot be duplicated.
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
    // mutating notes. For the editor's preview of the playing repeat.
    bool pianoRepeatNotes(const std::string& trackId, const std::string& clipId, const std::string& laneId,
                          int64_t repeat, std::vector<ofxOceanodeTimelinePianoNote>& out) const;
    bool resizeClipEnd(const std::string& trackId, const std::string& clipId,
                       double durationBeats, ofxOceanodeTimelineEndEdit operation);
    bool resizeClipEnd(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                       double durationBeats, ofxOceanodeTimelineEndEdit operation);
    // Does not write parameters, schedule events, or change source content.
    ofxOceanodeTimelineTargetInspection inspectTarget(const std::string& parameterPath,
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
    // refused for Wave and LFO clips.
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
    // Recomputes waveNumChannels/waveFileDurationMs/waveformPeaks from
    // whatever waveFilePath currently points at (WAV files only, matching
    // the old standalone Wave Track node's own reader). Pure file IO, no
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
    // clear it (pass nullptr) before destroying it.
    void setWaveAudioProvider(ofxOceanodeTimelineWaveAudioProvider* provider) { waveAudioProvider = provider; }

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
    const std::vector<ofxOceanodeTimelineClipGroup>& getClipGroups() const { return mainSequence.clipGroups; }

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
    // Play on preset load: when a preset with this on finishes loading, the
    // transport starts from the beginning (saved per preset, off by default)
    bool getPlayOnPresetLoad() const { return playOnPresetLoad; }
    void setPlayOnPresetLoad(bool enabled) { playOnPresetLoad = enabled; }
    bool isMetronomeEnabled() const { return metronome != nullptr; }
    bool setMetronomeEnabled(bool enabled);
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
    // automation and view scrolling/zooming are never recorded.
    void beginTimelineEdit();
    void endTimelineEdit();
    bool undoTimelineEdit();
    bool redoTimelineEdit();
    bool canUndoTimelineEdit() const { return !timelineUndo.empty(); }
    bool canRedoTimelineEdit() const { return !timelineRedo.empty(); }
    void clearTimelineHistory();

    ofJson toJson() const;
    void fromJson(const ofJson& json);
    // Portable clip documents contain one clip and only its target descriptors.
    // Map saved binding IDs to parameter paths in the destination project.
    // Missing mappings leave content intact and unassigned. Empty trackId creates a track.
    ofJson exportClip(const std::string& trackId, const std::string& clipId) const;
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
    static constexpr int kPresetVersion = 12;
    struct TimelineState {
        ofJson data;
        std::vector<ofxOceanodeTimelineTrack> tracks;
        std::vector<ofxOceanodeTimelineClipGroup> groups;
        std::vector<ofxOceanodeTimelineMarker> markers;
        std::vector<ofxOceanodeTimelineCurvePoint> tempoPoints;
        std::vector<ofxOceanodeTimelineCurveTension> tempoTensions;
        uint64_t nextTrack, nextBinding, nextClip, nextLane, nextGroup;
        size_t bytes = 0;
    };
    struct TimelineEdit {
        TimelineState before, after;
        float beforeBpm = 0.0f, afterBpm = 0.0f;
        bool manualTempo = false;
        size_t bytes = 0;
    };
    std::unique_ptr<TimelineEdit> pendingTimelineEdit;
    // Macro sequences: where each one lives and who shares it.
    struct MacroSequenceEntry {
        std::weak_ptr<ofxOceanodeContainer> container; // expires with the macro
        const ofxOceanodeContainer* address = nullptr;
        std::shared_ptr<ofxOceanodeTimelineSequence> sequence;
        std::string libraryFolder; // a library macro's definition: shared by every copy
    };
    mutable std::vector<MacroSequenceEntry> macroSequences;
    // Drops entries of deleted macros and keeps each shared sequence's base
    // on a live copy's container.
    void pruneMacroSequences() const;
    struct SequenceHistory {
        std::unique_ptr<ofJson> pending;
        std::deque<std::pair<ofJson, ofJson>> undo, redo; // (before, after)
    };
    mutable std::map<const ofxOceanodeTimelineSequence*, SequenceHistory> sequenceHistories;
    ofxOceanodeContainer* pendingMacroScope = nullptr;
    std::deque<TimelineEdit> timelineUndo, timelineRedo;
    bool replayingTimelineEdit = false;
    ofJson timelineEditData() const;
    TimelineState captureTimelineEdit() const;
    void restoreTimelineState(const TimelineState& state);
    bool stepTimelineHistory(bool backwards);
    void trimTimelineHistory();
    std::unique_ptr<ofxOceanodeTimelineMetronome> metronome;
    static std::string laneTypeToString(ofxOceanodeTimelineLaneType laneType);
    static ofxOceanodeTimelineLaneType laneTypeFromString(const std::string& laneType);
    std::string insertTrackCopy(ofxOceanodeTimelineTrack copy, const std::string& name, size_t index);
    std::string insertTrackCopy(ofxOceanodeTimelineSequence& sequence, ofxOceanodeTimelineTrack copy, const std::string& name, size_t index);
    // Engine state keyed by ids (wave voices, state-capture overrides,
    // driven solos, the track id cache) exists for the main timeline only.
    bool isMainSequence(const ofxOceanodeTimelineSequence& sequence) const { return &sequence == &mainSequence; }
    // Keeps clipGroups' invariants (every member clip still exists, every
    // group has at least two members) after a clip is removed -- called
    // from removeClip/removeTrack rather than left to every caller.
    void removeClipFromGroups(const std::string& trackId, const std::string& clipId);
    void removeClipFromGroups(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId);
    void clearTimelineFlag(const ofxOceanodeTimelineTrack& track);
    void refreshTimelineFlag(const std::string& parameterPath);
    void releaseWaveClipIfNeeded(const std::string& trackId, const std::string& clipId);

    using ActiveValueMap = std::unordered_map<std::string,
        std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>>>;
    // Pure evaluation of every binding at one beat. evaluateAutomation() uses
    // it for the current playhead; the scheduler uses it for beats that have
    // not been reached yet, which is what makes lookahead possible without a
    // second implementation of the lane semantics.
    void collectActiveValues(const ofxOceanodeTimelinePlayContext& context,
                             double beat, bool isPlaying, bool applyPianoRanges,
                             ActiveValueMap& activeValues,
                             ofxOceanodeTimelineTargetInspection* inspection = nullptr,
                             const std::set<std::string>* onlyPaths = nullptr) const;
    bool computeParameterValue(const std::vector<ofxOceanodeTimelineParameterBinding*>& bindings,
                               const ActiveValueMap& activeValues,
                               ofxOceanodeAbstractParameter* parameter,
                               std::string& outValue) const;
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
    };
    // The sequences heard between two main-timeline beats. For now only the
    // main timeline itself; nested macro sequences will be added here.
    std::vector<PlayedSequence> playedSequences(double fromBeat, double toBeat) const;
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
    // The main timeline's arrangement. Tempo, markers, loop, time signature
    // and view settings belong to the main timeline only and stay here.
    ofxOceanodeTimelineSequence mainSequence;
    std::vector<ofxOceanodeTimelineMarker> markers; // sorted by beat
    ofxOceanodeTimelineViewState viewState;
    ofxOceanodeTimelineTimebase timebase = ofxOceanodeTimelineTimebase::Beats;
    std::vector<std::pair<std::string, std::string>> pendingParameterLanes;
    std::string pendingTrackRenameId;
    bool pendingTrackRenameIsNew = false;
    mutable std::string failedLoadPath; // timeline.json that could not be read
    // Per-update cache of parameter lookups by path (see update()).
    ofxOceanodeAbstractParameter* findParameterCached(const std::string& path) const;
    mutable std::unordered_map<std::string, ofxOceanodeAbstractParameter*> parameterCache;
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
    void pianoLaneVoicesAndKit(const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane,
                               int& voices, bool& kit, int* instrumentVoices = nullptr) const;
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
    // Paths bound by curve lanes of state-capture clips (the only readers of the map above).
    std::unordered_set<std::string> stateCapturePathsCache;
    uint64_t evaluationIndexesSignature = 0;
    std::unordered_set<std::string> stateCaptureManualOverrideKeys;
    bool evaluationIndexesValid = false;
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
    // Keep the transport-side range in lockstep with edits made by the UI.
    // Waiting for the next automation pass leaves a frame in which the icon
    // says loop is on but the playhead can cross the old end point.
    void syncLoopToTransport();
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
