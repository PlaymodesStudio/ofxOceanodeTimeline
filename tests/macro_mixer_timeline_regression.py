#!/usr/bin/env python3
"""A MacroMixer source's timelines play inside the mixer.

Uses the core's MacroMixer fixture (real Macro nodes, no window). A source
preset saved with a free timeline set to play on load starts when the mixer
activates it and drives the graph the mixer reads; a morph in place loads the
destination's timeline into the running graph as the morph begins, and a
control that timeline drives follows it rather than the control morph; a
timeline linked to the global transport follows the host's; the timelines go
with their graph. A source's timeline is one of its windows: ticked, it opens
as a window of the host's Timeline named after the source and never saved;
its close button, the end of a transition and Clear Playback close it.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#include "Nodes/MacroMixerRuntime.h"
#include "router.h"
#include <filesystem>
class Probe : public ofxOceanodeNodeModel {
public:
    Probe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> level, output;
    void setup() override {
        addParameter(level.set("Level", -1, -1, 4));
        addOutputParameter(output.set("Output", 0, -1, 4));
    }
    void update(ofEventArgs&) override { output = level.get(); }
};
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
static bool near(float a, float b) { return std::abs(a - b) < 0.02f; }
static Probe& probeOf(const MacroMixerSource::Graph* graph) {
    return static_cast<Probe&>(graph->container->getDynamicNodes().at("Probe").at(1)->getNodeModel());
}
int main(int argc, char** argv) {
    assert(argc == 3); namespace fs = std::filesystem;
    const std::string scratch = argv[2]; fs::current_path(scratch); ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto singleton = ofxOceanodeTypesRegistry::getInstance();
    types->registerType<float>("f"); singleton->registerType<float>("f");
    registry->registerModel<router<float>>("Router", "f", 0.f, -100.f, 100.f);
    registry->registerModel<Probe>("Test");
    registry->registerModel<ofxOceanodeNodeMacro>("Macro");
    auto transport = std::make_shared<ofxOceanodeTransport>();
    transport->setDriverMode(TransportDriverMode::FrameStep);
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types, transport); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    model.setSchedulingEnabled(false);
    using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
    using ClockSource = ofxOceanodeTimelineManager::ClockSource;

    // A source preset: Probe/Output -> Router "Value", and a timeline on
    // Probe/Level: a ramp 0 -> 1 over 8 beats, or a held value.
    auto save = [&](const std::string& folder, bool free, bool playOnLoad, float held) {
        ofxOceanodeContainer c(registry, types, transport); c.setCanvasID("Canvas");
        auto& timeline = ofxOceanodeTimeline::attach(c);
        auto& out = c.createNode(std::make_unique<router<float>>("f", 0.f, -100.f, 100.f), 1).getNodeModel();
        out.getParameter<std::string>("Name") = "Value";
        auto& node = c.createNode(std::make_unique<Probe>(), 1);
        static_cast<Probe&>(node.getNodeModel()).level = -1;
        assert(c.createConnectionFromInfo("Probe_1", "Output", "Router_f_1", "Value"));
        auto& level = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Level"));
        const auto track = timeline.createTrack("Level");
        const auto binding = timeline.addBinding(track, level);
        const auto clip = timeline.createClip(track, "Clip", 0, 8);
        if(held < 0) {
            const auto lane = timeline.createLane(track, clip, "Level", ofxOceanodeTimelineLaneType::Curve);
            timeline.addBindingToLane(track, clip, lane, binding);
            auto* ramp = timeline.getLane(track, clip, lane);
            ramp->curveInterpolation = "Linear";
            ramp->curvePoints = {{0, 0.0f}, {8, 1.0f}};
            ramp->curveTensions = {{}};
        } else {
            const auto lane = timeline.createLane(track, clip, "Level", ofxOceanodeTimelineLaneType::Number);
            timeline.addBindingToLane(track, clip, lane, binding);
            timeline.setClipNumber(track, clip, lane, 0, held);
        }
        auto settings = *timeline.playSettings({});
        settings.playOnLoad = playOnLoad; settings.quantize = 0; settings.loopEnabled = false; settings.created = true;
        timeline.setPlaySettings({}, settings);
        timeline.setFree({}, free);
        assert(c.savePreset(folder));
        assert(fs::exists(folder + "/timeline.json"));
    };
    const auto a = scratch + "/a", b = scratch + "/b", g = scratch + "/g";
    save(a, true, true, -1);      // free, play on load: the ramp
    save(b, true, true, 0.75f);   // free, play on load: holds 0.75
    save(g, false, false, 0.6f);  // linked to the global transport: holds 0.6

    MacroMixerRuntime runtime(*owner);
    MacroMixerContract contract;
    assert(runtime.setContract(contract));
    assert(runtime.importMacro(a) && runtime.importMacro(b) && runtime.importMacro(g));
    double ms = 0;
    auto frame = [&](double seconds) {
        transport->advanceFrameStep(seconds); transport->latchFrameState();
        model.update(); model.applyAutomation(); // the host's timeline: players, evaluation, apply
        ms += seconds * 1000; runtime.update(ms);
    };

    // Activation: the source's timeline is the source Macro's, in a canvas
    // nested under the mixer's; free and set to play on load, it starts.
    runtime.request(a, 0, 0); frame(0.05);
    const auto* graph = runtime.origin();
    check(graph != nullptr, "Source a runs");
    if(graph == nullptr) return 1;
    const std::string canvas = graph->container->getCanvasID();
    check(canvas.rfind("MacroMixer/", 0) == 0 && canvas.size() > 10 && canvas.substr(canvas.size() - 10) == " / Macro 1",
          "The graph's canvas nests under the mixer's: " + canvas);
    const TimelineRef ref{canvas, std::string()};
    check(model.timelineExists(ref) && model.isFree(ref) && model.clockSourceOf(ref) == ClockSource::Free,
          "The preset's timeline.json became the source Macro's free timeline");
    frame(0.05);
    auto playing = model.transportFor(ref);
    check(playing != nullptr && playing->isDerived(), "A free timeline inside the mixer has its own transport");
    for(int i = 0; i < 10; ++i) frame(0.1);
    check(playing != nullptr && playing->getState().isPlaying, "Play on load started it when the graph started");
    auto& probe = probeOf(graph);
    const float early = probe.level.get();
    check(early > 0.05f && early < 0.95f, "The ramp drives the graph's Probe/Level: " + ofToString(early));
    check(near(runtime.value(), early), "...and the mixer reads what it drives: " + ofToString(runtime.value()));
    for(int i = 0; i < 10; ++i) frame(0.1);
    check(probe.level.get() > early + 0.15f, "The ramp advances with the timeline's own clock");

    // The source's timeline is one of its windows: closed until ticked, then
    // a window of the host's Timeline named after the source, never saved.
    const auto aID = runtime.importedMacro(a)->id();
    std::string timelineKey;
    for(const auto& window : runtime.sourceWindows(aID))
        if(window.title.rfind("Timeline: ", 0) == 0) timelineKey = window.key;
    check(timelineKey == "Timeline main", "The source lists its timeline as a window: " + timelineKey);
    check(model.getTimelineWindows().empty(), "...closed until ticked");
    check(runtime.setSourceWindow(aID, timelineKey, true), "Ticked");
    frame(0.05);
    const auto windows = model.getTimelineWindows();
    check(windows.size() == 1 && ofxOceanodeTimelineManager::normalizedRef(windows.front().ref) == ref &&
          windows.front().hostLabel == runtime.importedMacro(a)->name(),
          "A window of the host's Timeline shows the source's timeline, labelled after the source");
    check(!model.toJson().contains("windows"), "...and is never saved with the host preset");
    // Closed with its own button (outside the mixer's draw): unticked, not reopened.
    if(!windows.empty()) model.closeTimelineWindow(windows.front().id);
    frame(0.05); frame(0.05);
    check(model.getTimelineWindows().empty() && runtime.openWindows(aID).count(timelineKey) == 0, "Closing it unticks it");
    runtime.setSourceWindow(aID, timelineKey, true); frame(0.05);
    check(model.getTimelineWindows().size() == 1, "Ticked again, it opens again");

    // A morph in place: the destination's timeline replaces the origin's in
    // the running canvas and starts; what it drives follows it, not the
    // control morph from the ramp's value to the saved -1.
    runtime.request(b, 1000, 0, true); frame(0.05);
    check(runtime.morphingInPlace(), "a -> b morphs in place");
    frame(0.05);
    for(int i = 0; i < 5; ++i) frame(0.1);
    check(near(probe.level.get(), 0.75f), "The destination's timeline holds Level at 0.75 during the morph: " + ofToString(probe.level.get()));
    check(model.transportFor(ref) != nullptr && model.transportFor(ref)->getState().isPlaying, "...playing from its start");
    check(model.getTimelineWindows().size() == 1, "The window a ticked stays open during the morph in place");
    runtime.finish(); frame(0.05);
    check(!runtime.transitioning() && runtime.currentID() == runtime.importedMacro(b)->id(), "Completed into b");
    for(int i = 0; i < 3; ++i) frame(0.1);
    check(near(probe.level.get(), 0.75f), "After completion b's timeline still drives Level");
    check(model.getTimelineWindows().empty(), "Completed into b, which never ticked it: the window closed");

    // Linked to the global transport: it follows the host's.
    runtime.request(g, 0, 0); frame(0.05); frame(0.05);
    const auto* global = runtime.origin();
    check(global != nullptr && global != graph, "Source g runs in a graph of its own");
    if(global == nullptr) return 1;
    const TimelineRef gref{global->container->getCanvasID(), std::string()};
    check(model.timelineExists(gref) && !model.isFree(gref) && model.clockSourceOf(gref) == ClockSource::Global,
          "g's timeline is linked to the global transport");
    check(model.transportFor(gref) == transport, "...the host's");
    check(!model.timelineExists(ref), "a/b's canvas went with its graph");
    transport->play();
    for(int i = 0; i < 5; ++i) frame(0.1);
    check(transport->getState().isPlaying && transport->getState().beatPosition > 0.5, "The host transport plays");
    check(near(probeOf(global).level.get(), 0.6f), "g's timeline drives Level along the host transport: " + ofToString(probeOf(global).level.get()));
    transport->stop();
    const auto gID = runtime.importedMacro(g)->id();
    runtime.setSourceWindow(gID, "Timeline main", true); frame(0.05);
    check(model.getTimelineWindows().size() == 1, "g's timeline window opens while g plays");

    // Clear Playback: the graph goes, and its timeline with it.
    runtime.clear(); frame(0.05);
    check(!model.timelineExists(gref) && model.transportFor(gref) == nullptr, "The timeline went with its graph");
    check(model.getTimelineWindows().empty(), "Clear Playback closed the window the mixer opened");
    check(runtime.openWindows(gID).count("Timeline main") == 1, "...and kept g's choice for its next activation");
    for(const auto& listed : model.allCanvases()) check(listed.rfind("MacroMixer/", 0) != 0, "No mixer canvas is listed any more: " + listed);

    if(failures > 0) return 1;
    std::cout << "MacroMixer timelines passed: nested canvas, timeline.json as the source Macro's timeline, play on load at activation, "
                 "destination timeline on a morph in place, global-linked timeline on the host transport, gone with the graph, "
                 "timeline windows ticked per source (labelled, unsaved, closed by their button, by completion and by Clear Playback).\n";
    return 0;
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
