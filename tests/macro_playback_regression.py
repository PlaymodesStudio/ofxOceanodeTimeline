#!/usr/bin/env python3
"""Macro clips play their macro's timeline.

Uses real macro nodes (core's native headless fixture). A macro clip places
the macro's timeline through its repeats, tempo and content offset; the
macro's bindings reach the parameters inside that macro; the main timeline
layers on top of it with its bindings' modes; mute and solo stay inside
their own sequence; and an edit to the macro's timeline is heard at once.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
class Probe : public ofxOceanodeNodeModel {
public:
    Probe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> level, other, gate, trig;
    void setup() override {
        addParameter(level.set("Level", -1, -1, 4));
        addParameter(other.set("Other", -1, -1, 4));
        addParameter(gate.set("Gate", 0, 0, 1));
        addParameter(trig.set("Trig", 0, 0, 1));
    }
};
static int failures = 0;
static float levelNow = 0, otherNow = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << " (level " << levelNow << ", other " << otherNow << ")\n"; ++failures; }
}
int main(int, char**) {
    ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    model.setSchedulingEnabled(false);
    auto& macroNode = owner->createNode(std::make_unique<ofxOceanodeNodeMacro>(), 1);
    auto inside = static_cast<ofxOceanodeNodeMacro&>(macroNode.getNodeModel()).getContainer();
    auto& probeNode = inside->createNode(std::make_unique<Probe>(), 1);
    auto& probe = static_cast<Probe&>(probeNode.getNodeModel());
    auto& level = static_cast<ofxOceanodeAbstractParameter&>(probeNode.getParameters().get("Level"));
    auto& other = static_cast<ofxOceanodeAbstractParameter&>(probeNode.getParameters().get("Other"));

    // The macro's timeline: Level ramps 0 -> 1 over 4 beats; Other holds 2.
    auto* pattern = model.getMacroSequence(*inside);
    const auto pt = model.createTrack(*pattern, "Ramp");
    const auto pb = model.addBinding(*pattern, pt, level);
    const auto pc = model.createClip(*pattern, pt, "Phrase", 0, 4);
    const auto pl = model.createLane(*pattern, pt, pc, "Level", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(*pattern, pt, pc, pl, pb);
    auto* ramp = model.getLane(*pattern, pt, pc, pl);
    ramp->curveInterpolation = "Linear";
    ramp->curvePoints = {{0, 0.0f}, {4, 1.0f}};
    ramp->curveTensions = {{}};
    const auto ot = model.createTrack(*pattern, "Hold");
    const auto ob = model.addBinding(*pattern, ot, other);
    const auto oc = model.createClip(*pattern, ot, "Hold", 0, 4);
    const auto ol = model.createLane(*pattern, ot, oc, "Other", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(*pattern, ot, oc, ol, ob);
    model.setClipNumber(*pattern, ot, oc, ol, 0, 2.0f);
    // The curve's own range is 0..1 (the lane's, not the parameter's).
    auto expectedLevel = [](double value) { return static_cast<float>(value); };

    // Main timeline: a macro clip at beat 8, two repeats of its 4 beats.
    const auto mt = model.createMacroTrack(*inside);
    const auto mc = model.createMacroClip(mt, 8.0, 8.0);
    check(!mc.empty() && std::abs(model.getClip(mt, mc)->contentDurationBeats - 4.0) < 1e-9, "Macro clip content is the macro timeline's 4 beats");
    owner->getTransport()->play();
    auto at = [&](double beat) {
        owner->getTransport()->seekToBeat(beat);
        model.update(); model.applyAutomation();
        levelNow = probe.level.get(); otherNow = probe.other.get();
        return probe.level.get();
    };
    auto near = [](float a, float b) { return std::abs(a - b) < 0.02f; };

    check(near(at(2.0), -1.0f), "Before the macro clip nothing is written");
    check(near(probe.other.get(), -1.0f), "...on any target");
    check(near(at(9.0), expectedLevel(0.25)), "First repeat, one beat in: a quarter of the ramp");
    check(near(probe.other.get(), 2.0f), "The other track of the macro timeline plays too");
    check(near(at(13.0), expectedLevel(0.25)), "Second repeat starts the ramp again");
    check(near(at(15.5), expectedLevel(0.875)), "Near the end of the second repeat");
    check(near(at(17.0), expectedLevel(0.875)) || near(probe.level.get(), expectedLevel(1.0)), "After the clip the value holds");

    // The main playhead as the macro's timeline view shows it.
    auto playheadIn = [&](double mainBeat, double& beat) {
        owner->getTransport()->seekToBeat(mainBeat);
        return model.macroPlayheadBeat(*inside, beat);
    };
    double macroBeat = 0.0;
    check(!playheadIn(2.0, macroBeat), "Macro view: no playhead before the macro clip");
    check(playheadIn(9.0, macroBeat) && std::abs(macroBeat - 1.0) < 1e-6, "Macro view: one beat in");
    check(playheadIn(13.5, macroBeat) && std::abs(macroBeat - 1.5) < 1e-6, "Macro view: the second repeat starts over");
    check(!playheadIn(16.5, macroBeat), "Macro view: no playhead after the clip");

    // Reversed: the clip plays the macro's timeline backwards at the same
    // speed (what it played at the mirror beat), the timeline as written.
    check(model.reverseClip(mt, mc) && model.getClip(mt, mc)->reversed, "Macro clip reversed");
    check(near(at(9.0), expectedLevel(0.75)), "Reversed: beat 9 plays what beat 15 did");
    check(near(at(13.5), expectedLevel(0.625)), "...and 13.5 what 10.5 did");
    check(model.getLane(*pattern, pt, pc, pl)->curvePoints.front().value == 0.0f, "The macro's timeline stays as written");
    check(playheadIn(9.0, macroBeat) && std::abs(macroBeat - 3.0) < 1e-6, "Macro view: the playhead runs backwards");
    check(model.reverseClip(mt, mc) && !model.getClip(mt, mc)->reversed, "Reversed twice: forwards");
    check(near(at(9.0), expectedLevel(0.25)), "...plays as before");

    // Tempo: the macro timeline at half speed (stretch 2): one beat in is an eighth.
    auto* clip = model.getClip(mt, mc);
    clip->contentStretch = 2.0;
    check(near(at(10.0), expectedLevel(0.25)), "Half speed: two beats in is a quarter");
    check(playheadIn(10.0, macroBeat) && std::abs(macroBeat - 1.0) < 1e-6, "Macro view at half speed: two beats in is beat one");
    clip->contentStretch = 1.0;
    // Content offset: the clip starts one beat into the macro timeline.
    model.setClipContentOffset(mt, mc, 1.0);
    clip = model.getClip(mt, mc);
    check(near(at(8.0 + 1e-3), expectedLevel(0.25)), "Offset 1: starts a quarter in");
    check(near(at(11.5), expectedLevel(0.125)), "...and wraps to the start after three beats");
    check(playheadIn(8.5, macroBeat) && std::abs(macroBeat - 1.5) < 1e-6, "Macro view with offset 1: half a beat in is 1.5");
    check(playheadIn(11.5, macroBeat) && std::abs(macroBeat - 0.5) < 1e-6, "...and 0.5 after the wrap");
    model.setClipContentOffset(mt, mc, 0.0);

    // The main timeline layers on top: an Add binding on the same parameter.
    const auto lt = model.createTrack("Layer");
    const auto lb = model.addBinding(lt, level);
    check(model.getBinding(lt, lb)->parameterPath == inside->getCanvasID() + "::Probe_1/Level", "Main binding path is absolute");
    model.getBinding(lt, lb)->mode = ofxOceanodeTimelineAutomationMode::Add;
    const auto lc = model.createClip(lt, "Add", 8, 8);
    const auto ll = model.createLane(lt, lc, "Level", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(lt, lc, ll, lb);
    model.setClipNumber(lt, lc, ll, 0, 0.5f);
    check(near(at(9.0), expectedLevel(0.25) + 0.5f), "Main Add layers on the macro value");
    model.getBinding(lt, lb)->mode = ofxOceanodeTimelineAutomationMode::Replace;
    check(near(at(9.0), 0.5f), "Main Replace wins over the macro value");
    model.removeTrack(lt);

    // Mute and solo: the macro track's own, and solo scoped per sequence.
    model.setTrackMuted(mt, true);
    probe.level = -1.0f; probe.other = -1.0f;
    at(9.0);
    check(near(probe.level.get(), -1.0f) && near(probe.other.get(), -1.0f), "A muted macro track writes nothing");
    check(!playheadIn(9.0, macroBeat), "...and shows no playhead in the macro view");
    model.setTrackMuted(mt, false);
    model.setTrackSolo(*pattern, ot, true); // a solo inside the macro timeline
    probe.level = -1.0f;
    at(9.0);
    check(near(probe.level.get(), -1.0f) && near(probe.other.get(), 2.0f), "A solo inside the macro silences only its siblings");
    const auto st = model.createTrack("Solo");
    model.setTrackSolo(st, true); // a solo on the main timeline
    probe.other = -1.0f;
    at(9.5);
    check(near(probe.other.get(), -1.0f), "A main-timeline solo silences the macro track");
    model.removeTrack(st);
    model.setTrackSolo(*pattern, ot, false);

    // Editing the macro's timeline is heard at once (no explicit invalidation).
    model.setClipNumber(*pattern, ot, oc, ol, 0, 3.0f);
    at(9.0);
    check(near(probe.other.get(), 3.0f), "An edit to the macro timeline plays at once");
    model.getLane(*pattern, pt, pc, pl)->curvePoints = {{0, 1.0f}, {4, 1.0f}};
    check(near(at(9.5), expectedLevel(1.0)), "...curve edits too");

    // The inspector sees the macro's binding under its absolute path.
    const auto inspection = model.inspectTarget(inside->getCanvasID() + "::Probe_1/Level", 9.0, true);
    check(inspection.writesValue && near(static_cast<float>(ofToFloat(inspection.value)), expectedLevel(1.0)), "Inspector agrees with playback");

    // A macro inside the macro, with its own timeline, placed by the outer
    // macro's timeline: beats 1..3 of each outer repeat, at its own half speed.
    auto& innerNode = inside->createNode(std::make_unique<ofxOceanodeNodeMacro>(), 1);
    auto innerInside = static_cast<ofxOceanodeNodeMacro&>(innerNode.getNodeModel()).getContainer();
    auto& innerProbeNode = innerInside->createNode(std::make_unique<Probe>(), 1);
    auto& innerProbe = static_cast<Probe&>(innerProbeNode.getNodeModel());
    auto* innerPattern = model.getMacroSequence(*innerInside);
    const auto it = model.createTrack(*innerPattern, "Inner");
    const auto ib = model.addBinding(*innerPattern, it, static_cast<ofxOceanodeAbstractParameter&>(innerProbeNode.getParameters().get("Level")));
    const auto ic = model.createClip(*innerPattern, it, "Ramp", 0, 4);
    const auto il = model.createLane(*innerPattern, it, ic, "Level", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(*innerPattern, it, ic, il, ib);
    model.getLane(*innerPattern, it, ic, il)->curveInterpolation = "Linear";
    model.getLane(*innerPattern, it, ic, il)->curvePoints = {{0, 0.0f}, {4, 1.0f}};
    model.getLane(*innerPattern, it, ic, il)->curveTensions = {{}};
    const auto imt = model.createMacroTrack(*pattern, *innerInside);
    check(!imt.empty() && model.getTrack(*pattern, imt)->macroCanvas == innerInside->getCanvasID().substr(inside->getCanvasID().size() + 3),
          "Inner macro track's canvas is relative to the outer macro");
    const auto imc = model.createMacroClip(*pattern, imt, 1.0, 2.0);
    model.getClip(*pattern, imt, imc)->contentStretch = 2.0; // half speed, one 4-beat pass would take 8
    innerProbe.level = -1.0f;
    at(8.5);
    check(near(innerProbe.level.get(), -1.0f), "Inner macro silent before its clip in the outer timeline");
    at(10.0);
    check(near(innerProbe.level.get(), 0.125f), "Inner ramp one outer beat into its clip, at half speed: an eighth");
    at(14.0);
    check(near(innerProbe.level.get(), 0.125f), "...and again in the outer clip's second repeat");
    innerProbe.level = -1.0f;
    at(11.5);
    check(near(innerProbe.level.get(), -1.0f), "Silent after its clip ends inside the outer repeat");

    // Rest values: a toggle on for the whole macro timeline closes when the
    // macro clip ends (not held open, as a gate left by a main clip is).
    const auto gt = model.createTrack(*pattern, "Gate");
    const auto gb = model.addBinding(*pattern, gt, static_cast<ofxOceanodeAbstractParameter&>(probeNode.getParameters().get("Gate")));
    const auto gc = model.createClip(*pattern, gt, "Open", 0, 4);
    const auto gl = model.createLane(*pattern, gt, gc, "Gate", ofxOceanodeTimelineLaneType::MultiGate);
    model.addBindingToLane(*pattern, gt, gc, gl, gb);
    model.getLane(*pattern, gt, gc, gl)->multiRowCount = 1;
    model.getLane(*pattern, gt, gc, gl)->multiGateRows = {{{0.0, 4.0, 1}}};
    at(9.0);
    check(near(probe.gate.get(), 1.0f), "Toggle open inside the macro clip");
    at(16.5);
    check(near(probe.gate.get(), 0.0f), "...rests when the macro clip ends");
    at(9.0);
    model.setTrackMuted(mt, true); at(9.2);
    check(near(probe.gate.get(), 0.0f), "...and when its macro track is muted");
    model.setTrackMuted(mt, false);

    // Gating: Active on from the pre-roll (100 ms = 0.2 beats at 120 BPM)
    // before the clip until the tail (500 ms = 1 beat) after it.
    auto& active = static_cast<ofxOceanodeAbstractParameter&>(macroNode.getParameters().get("Active")).cast<bool>().getParameter();
    const float bpm = owner->getTransport()->getState().bpm;
    check(std::abs(bpm - 120.0f) < 0.01f, "Test assumes 120 BPM");
    model.getTrack(mt)->macroGate = true;
    at(2.0);  check(!active.get(), "Gated: off before the clip");
    at(7.85); check(active.get(), "On within the pre-roll");
    at(9.0);  check(active.get(), "On in the clip");
    at(16.8); check(active.get(), "On through the tail");
    at(17.3); check(!active.get(), "Off after the tail");
    const auto hand = model.createTrack("Hand");
    auto& activeParameter = static_cast<ofxOceanodeAbstractParameter&>(macroNode.getParameters().get("Active"));
    check(model.addBinding(hand, activeParameter).empty(), "A gated macro's Active cannot be bound by hand");
    model.getTrack(mt)->macroGate = false;
    check(!model.addBinding(hand, activeParameter).empty(), "...but can when ungated");
    model.removeTrack(hand);
    model.getTrack(mt)->macroGate = true;
    // Loop start inside the clip: on across the wrap.
    // (clip 8..16, loop 12..20: after the tail it is off, then the
    // pre-roll before the loop end reaches the clip at the loop start)
    model.setLoopRange(12.0, 20.0); model.setLoopEnabled(true);
    at(18.5);  check(!active.get(), "Looping: off after the tail");
    at(19.9);  check(active.get(), "On again before the wrap, the loop start being inside the clip");
    model.setLoopEnabled(false);
    model.getTrack(mt)->macroGate = false;
    at(2.0);  check(active.get(), "Ungated: switched back on");

    // Snapshot per clip: recalled once when the clip comes up.
    auto& snapshot = static_cast<ofxOceanodeAbstractParameter&>(macroNode.getParameters().get("Snapshot")).cast<int>().getParameter();
    model.getClip(mt, mc)->macroSnapshot = 2;
    snapshot = -1;
    at(2.0); check(snapshot.get() == -1, "No snapshot outside the clip");
    at(8.5); check(snapshot.get() == 2, "Clip start recalls its snapshot");
    snapshot = 5; at(9.0);
    check(snapshot.get() == 5, "...once (a later change stays)");
    at(2.0); at(8.5);
    check(snapshot.get() == 2, "...and again when the clip starts again");

    // Retrigger gap at another tempo: two back-to-back gate steps close for
    // the same time before the second one, whatever tempo the clip plays at.
    {
        auto stepPair = [&](ofxOceanodeTimelineSequence& sequence, const std::string& track, const std::string& binding,
                            double start) {
            const auto clip = model.createClip(sequence, track, "Pair", start, 4);
            const auto laneId = model.createLane(sequence, track, clip, "Trig", ofxOceanodeTimelineLaneType::Step);
            model.addBindingToLane(sequence, track, clip, laneId, binding);
            auto* lane = model.getLane(sequence, track, clip, laneId);
            lane->beatsPerStep = 0.5; lane->stepCount = 8;
            lane->step.steps = {{0.0, 0.0, "1"}, {0.5, 0.0, "1"}};
            return clip;
        };
        // Reference: the same pair on the main timeline, at beat 40.
        auto& refNode = owner->createNode(std::make_unique<Probe>(), 7);
        auto& ref = static_cast<Probe&>(refNode.getNodeModel());
        const auto rt = model.createTrack("Reference");
        const auto rb = model.addBinding(rt, static_cast<ofxOceanodeAbstractParameter&>(refNode.getParameters().get("Trig")));
        stepPair(model.getMainSequence(), rt, rb, 40.0);
        const auto tt = model.createTrack(*pattern, "Trig");
        const auto tb = model.addBinding(*pattern, tt, static_cast<ofxOceanodeAbstractParameter&>(probeNode.getParameters().get("Trig")));
        stepPair(*pattern, tt, tb, 0.0);
        // Closed time (main beats) around a step boundary.
        auto closed = [&](ofParameter<float>& target, double boundary) {
            double off = 0.0;
            constexpr double step = 0.0005;
            for(double beat = boundary - 0.05; beat < boundary + 0.1; beat += step) {
                at(beat);
                if(target.get() < 0.5f) off += step;
            }
            return off;
        };
        const double reference = closed(ref.trig, 40.5);
        check(reference > 0.002, "The reference pair retriggers with a gap");
        for(double tempo : {2.0, 0.5}) {
            model.getClip(mt, mc)->contentStretch = 1.0 / tempo; // tempo ratio: the macro timeline plays this much faster
            model.setClipContentOffset(mt, mc, 0.0);
            const double gap = closed(probe.trig, 8.0 + 0.5 / tempo);
            check(std::abs(gap - reference) < 0.0015, "Macro clip at tempo x" + ofToString(tempo) + ": gap " + ofToString(gap) +
                  " beats, as on the main timeline (" + ofToString(reference) + ")");
        }
        model.getClip(mt, mc)->contentStretch = 1.0;
    }

    // The macro goes away: its timeline stops playing, nothing dangles.
    owner->getTransport()->stop();
    inside.reset();
    macroNode.deleteSelf();
    model.update(); model.applyAutomation();
    owner->getTransport()->play();
    owner->getTransport()->seekToBeat(9.0);
    model.update(); model.applyAutomation();
    if(failures > 0) return 1;
    std::cout << "Macro playback passed: placement, repeats, tempo, content offset, main layer (Add, Replace), "
                 "mute, solo per sequence, live edits, inspector, a macro inside a macro, playhead in the macro view, reversed macro clip, rest values, gating (pre-roll, tail, loop, no hand binding), snapshot per clip, retrigger gap at another tempo, macro removed.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
