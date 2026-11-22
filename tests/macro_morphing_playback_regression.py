#!/usr/bin/env python3
"""A Macro Morphing Track plays its sources through the timeline's own engine.

Uses the core's MacroMixer fixture (real Macro nodes, no window). The
timeline keeps one MacroMixerRuntime for its morph track, restored from and
stored in the track's state. Every frame the engine is told the scene at the
playhead: a clip alone, two clips in transition with the overlap position as
progress, or nothing in a gap, on a muted clip or a muted track; stopped and
seeking as well as playing. The sources' timelines never play on their own:
each follows the clip playing its source, both clips of an overlap at once.
The track's state follows an undo of the engine's edits, the engine goes with
its track, and the routers are read once per frame for the Timeline nodes.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#include "Nodes/MacroMixerRuntime.h"
#include "Nodes/MacroMixerGui.h"
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
static bool near(float a, float b) { return std::abs(a - b) < 0.03f; }
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
    transport->setBpm(120.0f);
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types, transport); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
    using ClockSource = ofxOceanodeTimelineManager::ClockSource;
    const TimelineRef main = model.addTimeline("", "Timeline 1");
    model.setEditTarget(main);
    model.setSchedulingEnabled(false);

    // A source preset: Probe/Output -> Router "Value", and a timeline on
    // Probe/Level, 8 beats long: a ramp 0 -> 1, or a held value. Saved free
    // and set to play on load, as a MacroMixer source that plays by itself.
    auto save = [&](const std::string& folder, float held) {
        ofxOceanodeContainer c(registry, types, transport); c.setCanvasID("Canvas");
        auto& timeline = ofxOceanodeTimeline::attach(c);
        timeline.setEditTarget(timeline.addTimeline("", "Timeline 1"));
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
        const auto own = timeline.getEditTarget();
        auto settings = *timeline.playSettings(own);
        settings.playOnLoad = true; settings.quantize = 0; settings.loopEnabled = false;
        timeline.setPlaySettings(own, settings);
        timeline.setFree(own, true);
        assert(c.savePreset(folder));
        assert(fs::exists(folder + "/timeline.json"));
    };
    const auto a = scratch + "/a", b = scratch + "/b";
    save(a, -1);      // the ramp
    save(b, 0.75f);   // holds 0.75
    // A source with a Macro inside: the preset's own timeline holds Level at
    // 0.5, and the Macro's own timeline ramps its inner Probe 0 -> 1 over 8
    // beats. Both saved free and set to play on load.
    const auto nested = scratch + "/nested";
    {
        ofxOceanodeContainer c(registry, types, transport); c.setCanvasID("Canvas");
        auto& timeline = ofxOceanodeTimeline::attach(c);
        timeline.setEditTarget(timeline.addTimeline("", "Timeline 1"));
        auto& out = c.createNode(std::make_unique<router<float>>("f", 0.f, -100.f, 100.f), 1).getNodeModel();
        out.getParameter<std::string>("Name") = "Value";
        auto& node = c.createNode(std::make_unique<Probe>(), 1);
        assert(c.createConnectionFromInfo("Probe_1", "Output", "Router_f_1", "Value"));
        auto& level = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Level"));
        const auto track = timeline.createTrack("Level");
        const auto binding = timeline.addBinding(track, level);
        const auto clip = timeline.createClip(track, "Clip", 0, 8);
        const auto lane = timeline.createLane(track, clip, "Level", ofxOceanodeTimelineLaneType::Number);
        timeline.addBindingToLane(track, clip, lane, binding);
        timeline.setClipNumber(track, clip, lane, 0, 0.5f);
        auto& macroNode = c.createNode(std::make_unique<ofxOceanodeNodeMacro>(), 1);
        auto inside = static_cast<ofxOceanodeNodeMacro&>(macroNode.getNodeModel()).getContainer();
        auto& innerNode = inside->createNode(std::make_unique<Probe>(), 1);
        auto& innerLevel = static_cast<ofxOceanodeAbstractParameter&>(innerNode.getParameters().get("Level"));
        auto* list = timeline.getMacroTimelines(*inside);
        assert(list != nullptr);
        if(list->timelines.empty()) timeline.createTimeline(inside->getCanvasID(), "Inner");
        assert(!list->timelines.empty());
        auto& inner = *list->timelines.front();
        const auto it = timeline.createTrack(inner, "Ramp");
        const auto ib = timeline.addBinding(inner, it, innerLevel);
        const auto ic = timeline.createClip(inner, it, "Phrase", 0, 8);
        const auto il = timeline.createLane(inner, it, ic, "Level", ofxOceanodeTimelineLaneType::Curve);
        timeline.addBindingToLane(inner, it, ic, il, ib);
        auto* ramp = timeline.getLane(inner, it, ic, il);
        ramp->curveInterpolation = "Linear";
        ramp->curvePoints = {{0, 0.0f}, {8, 1.0f}};
        ramp->curveTensions = {{}};
        for(const TimelineRef ref : {timeline.getEditTarget(), TimelineRef{inside->getCanvasID(), inner.timelineId}}) {
            auto settings = *timeline.playSettings(ref);
            settings.playOnLoad = true; settings.quantize = 0; settings.loopEnabled = false;
            timeline.setPlaySettings(ref, settings);
            timeline.setFree(ref, true);
        }
        assert(c.savePreset(nested));
    }

    // ---- Nodes a registry filter keeps out of a load survive a save ----
    // (What saving a source's patch from its graph relies on: the graph has
    // no kept-out node, and the folder must keep them.)
    {
        auto filtered = std::make_shared<ofxOceanodeNodeRegistry>(*registry);
        filtered->setNodeLoadFilter([](const std::string&, const std::string& node) { return node == "Probe_1"; });
        ofxOceanodeContainer partial(filtered, types, transport); partial.setCanvasID("Partial");
        check(partial.loadPreset(b), "A preset loads with one node kept out");
        check(partial.getDynamicNodes().count("Probe") == 0 || partial.getDynamicNodes().at("Probe").empty(), "...which is not created");
        check(partial.getDynamicNodes().count("Router f") == 1, "...while the others are");
        const auto resaved = scratch + "/resaved";
        check(partial.savePreset(resaved), "The loaded container is saved to a folder");
        const auto modules = ofLoadJson(resaved + "/modules.json");
        check(modules.contains("Probe") && modules["Probe"].contains("01") && modules.contains("Router f"),
              "The node kept out is still in the saved node list: " + modules.dump());
        check(fs::exists(resaved + "/Probe_1.json") &&
              ofBufferFromFile(resaved + "/Probe_1.json").getText() == ofBufferFromFile(b + "/Probe_1.json").getText(),
              "...with its own file, as it was");
        const auto cables = ofLoadJson(resaved + "/connections.json");
        check(cables.contains("Probe_1") && cables["Probe_1"].contains("Output") && cables["Probe_1"]["Output"].contains("Router_f_1"),
              "...and its cable: " + cables.dump());
    }

    auto frame = [&](double seconds = 0.05) {
        transport->advanceFrameStep(seconds); transport->latchFrameState();
        model.update(); model.applyAutomation(); // players, the morph engines, evaluation, apply
    };
    auto seek = [&](double beat) { model.seekTimeline(main, beat); frame(); frame(); };

    // ---- The engine: one per timeline with a morph track ----
    auto& sequence = *model.timelineFor(main);
    check(model.morphEngine(sequence) == nullptr, "No morph track: no engine");
    const auto trackId = model.createMorphTrack(sequence, "Morph");
    auto* engine = model.morphEngine(sequence);
    check(engine != nullptr && engine->contract().routers.empty() && engine->macros().empty(),
          "A new track's engine has no routers and no sources");
    if(engine == nullptr) return 1;
    check(std::as_const(model).morphEngine(sequence) == engine, "The const lookup finds the same engine");

    // Its routers and sources are edited through the track; the track keeps the engine's state.
    MacroMixerGui::State panel;
    std::string error;
    MacroMixerGui::Action routers{MacroMixerGui::ActionKind::Routers};
    routers.contract = MacroMixerContract{}; // one Float Output router, "Value"
    check(model.applyMorphAction(sequence, routers, panel, error), "A router is added: " + error);
    check(engine->contract().routers.size() == 1 && model.getTrack(sequence, trackId)->morphState.is_object() &&
          model.getTrack(sequence, trackId)->morphState.at("routers").size() == 1, "...and stored in the track");

    // A source is a folder; its clip lasts one pass of the source's timeline.
    const auto clipA = model.addMorphSource(sequence, a, error);
    check(!clipA.empty(), "Source a is added with its clip: " + error);
    const auto clipB = model.addMorphSource(sequence, b, error);
    check(!clipB.empty(), "Source b is added after it: " + error);
    if(clipA.empty() || clipB.empty()) return 1;
    const auto aID = engine->importedMacro(a)->id(), bID = engine->importedMacro(b)->id();
    const auto* track = model.getTrack(sequence, trackId);
    const auto* first = model.getClip(sequence, trackId, clipA);
    const auto* second = model.getClip(sequence, trackId, clipB);
    check(first->morphSource == aID && second->morphSource == bID, "Each clip names its source");
    check(near(first->startBeat, 0) && near(first->durationBeats, 8) && near(first->contentDurationBeats, 8),
          "Clip a: one pass of a's timeline (8 beats)");
    // 4 s at 120 BPM is 8 beats: held to half of the shorter clip.
    check(near(second->startBeat, 4) && near(second->durationBeats, 8), "Clip b overlaps a by half of it: 4..12");
    check(model.morphSourceName(sequence, aID) == engine->macro(aID)->name(), "A source is named after its folder");
    check(track->morphState.at("macros").size() == 2, "Both sources are stored in the track");
    check(model.addMorphSource(sequence, a, error) != clipA && engine->macros().size() == 2,
          "Adding a folder again reuses the source for another clip");
    {   // (that third clip is not part of what follows)
        std::vector<std::string> extra;
        for(const auto& clip : track->clips) if(clip.id != clipA && clip.id != clipB) extra.push_back(clip.id);
        for(const auto& id : extra) model.removeClip(sequence, trackId, id);
    }

    // ---- The scene at the playhead, stopped ----
    seek(2.0);
    check(engine->sceneDriven() && engine->currentID() == aID && !engine->transitioning(), "At beat 2 source a plays alone");
    const auto* graphA = engine->origin();
    if(graphA == nullptr) return 1;
    const TimelineRef aRef{graphA->container->getCanvasID(), "tl_1"};
    check(model.timelineExists(aRef), "a's timeline lives in its graph's canvas");
    check(model.clockSourceOf(aRef) == ClockSource::Dependent, "...placed by the clip, whatever the source saved (free, play on load)");
    check(near(probeOf(graphA).level.get(), 0.25f), "The clip places a's timeline: Level is the ramp at beat 2: " + ofToString(probeOf(graphA).level.get()));
    check(near(engine->value(), 0.25f), "...and the engine's router reads it: " + ofToString(engine->value()));
    for(int i = 0; i < 10; ++i) frame(0.1);
    check(near(probeOf(graphA).level.get(), 0.25f), "Stopped, a's timeline does not run by itself");
    double placed = -1;
    check(model.timelinePlayheadBeat(aRef, placed) && near(placed, 2.0), "a's playhead is where the clip places it");
    const auto* outputs = model.morphOutputs(sequence);
    check(outputs != nullptr && outputs->count("value") == 1 && near(std::get<float>(outputs->at("value")), 0.25f),
          "The routers are read once per frame for the Timeline nodes");
    check(model.morphEngineStatus(sequence).empty(), "Nothing to report: " + model.morphEngineStatus(sequence));
    // The host's time system advances the phasors and time generators of the
    // live graphs: it is handed their canvases (no node of the patch reaches them).
    auto clocked = [&]() {
        std::vector<ofxOceanodeContainer*> canvases;
        model.forEachMorphGraph([&](ofxOceanodeContainer& canvas) { canvases.push_back(&canvas); });
        return canvases;
    };
    check(clocked().size() == 1 && clocked()[0] == graphA->container.get(), "The live source's canvas is handed to the time system");

    // Inside the overlap (4..8): both sources, progress the position in it.
    seek(5.0);
    check(engine->currentID() == aID && engine->destinationID() == bID && engine->transitioning(), "At beat 5 a and b are in transition");
    check(engine->origin() == graphA, "Entering the overlap keeps a's graph");
    const auto* graphB = engine->destination();
    if(graphB == nullptr) return 1;
    check(near(engine->progress(), 0.25f), "Progress is the playhead's position in the overlap: " + ofToString(engine->progress()));
    check(clocked().size() == 2 && clocked()[0] == graphA->container.get() && clocked()[1] == graphB->container.get(),
          "In a transition both sources' canvases are clocked");
    check(near(probeOf(graphA).level.get(), 0.625f), "a's timeline still follows its clip (beat 5 of it)");
    check(near(probeOf(graphB).level.get(), 0.75f), "b's timeline follows its own clip at the same time");
    check(near(engine->value(), 0.625f * 0.75f + 0.75f * 0.25f), "The router mixes the two: " + ofToString(engine->value()));
    // Backwards inside the overlap: free, nothing is rebuilt.
    seek(4.5);
    check(engine->origin() == graphA && engine->destination() == graphB && near(engine->progress(), 0.125f), "Scrubbing back in the overlap keeps both graphs");
    // The transition's shape is the later clip's.
    {
        auto transition = model.getClip(sequence, trackId, clipB)->morphIn;
        transition.biPow = 0.8f;
        check(model.setMorphTransition(sequence, trackId, clipB, transition), "The transition into b is edited");
        seek(6.0);
        check(near(engine->progress(), 0.5f), "Bi Pow leaves the middle of the overlap where it is");
        seek(5.0);
        check(near(engine->progress(), MacroMixerRuntime::shape(0.25f, 0.8f)) && !near(engine->progress(), 0.25f),
              "...and bends the progress off it: " + ofToString(engine->progress()));
        check(engine->origin() == graphA && engine->destination() == graphB, "...on the same graphs");
        transition.biPow = 0.0f;
        model.setMorphTransition(sequence, trackId, clipB, transition);
    }

    // Past the overlap: the scene moved on to b, a's graph and timeline go.
    seek(10.0);
    check(engine->currentID() == bID && !engine->transitioning() && engine->origin() == graphB, "At beat 10 b plays alone, on its graph");
    check(!model.timelineExists(aRef), "a's timeline went with its graph");
    check(near(engine->value(), 0.75f), "b's held value reaches the router");
    // Back into the overlap from the right: a is loaded again.
    seek(5.0);
    check(engine->currentID() == aID && engine->destinationID() == bID, "Seeking back into the overlap loads a again");
    // A gap: nothing plays.
    seek(20.0);
    check(engine->origin() == nullptr && engine->destination() == nullptr, "In a gap nothing plays");
    check(clocked().empty(), "...and no canvas is clocked");
    for(const auto& listed : model.allCanvases()) check(listed.rfind("MacroMixer/", 0) != 0, "No source canvas is left: " + listed);

    // ---- Playing ----
    seek(0.0);
    transport->play();
    for(int i = 0; i < 10; ++i) frame(0.1); // 1 s at 120 BPM: 2 beats
    check(transport->getState().isPlaying && engine->currentID() == aID, "Playing from the start, a plays");
    check(engine->origin() != nullptr && near(probeOf(engine->origin()).level.get(), static_cast<float>(transport->getState().beatPosition / 8.0)),
          "a's timeline advances with the host timeline");
    for(int i = 0; i < 15; ++i) frame(0.1); // beat 5
    check(engine->transitioning() && engine->destinationID() == bID, "The overlap is reached while playing");
    check(engine->progress() > 0.1f && engine->progress() < 0.5f, "...its progress following the playhead: " + ofToString(engine->progress()));
    transport->stop();
    frame();

    // ---- Silence: a muted clip, a muted track ----
    seek(2.0);
    check(engine->currentID() == aID, "a plays at beat 2");
    model.getClip(sequence, trackId, clipA)->muted = true;
    frame(); frame();
    check(engine->origin() == nullptr, "A muted clip plays nothing");
    seek(5.0);
    check(engine->currentID() == bID && !engine->transitioning(), "In the overlap, the other clip plays alone");
    model.getClip(sequence, trackId, clipA)->muted = false;
    model.getTrack(sequence, trackId)->muted = true;
    frame(); frame();
    check(engine->origin() == nullptr, "A muted track plays nothing");
    model.getTrack(sequence, trackId)->muted = false;
    frame(); frame();
    check(engine->currentID() == aID && engine->destinationID() == bID, "Unmuted, the scene is back");

    // ---- Timeline nodes ----
    check(!model.morphHasPublisher(sequence), "No Timeline node points at the timeline");
    model.noteMorphPublisher(sequence);
    check(model.morphHasPublisher(sequence), "A node noted on its update publishes the routers");
    frame();
    check(model.morphHasPublisher(sequence), "...still between its updates");
    frame(); frame();
    check(!model.morphHasPublisher(sequence), "...and no longer once it stopped updating");

    // ---- A source's own timelines: kept across its graphs, saved on demand ----
    {
        seek(2.0);
        check(engine->currentID() == aID && engine->origin() != nullptr, "a plays alone at beat 2");
        if(engine->origin() == nullptr) return 1;
        const TimelineRef liveRef{engine->origin()->container->getCanvasID(), "tl_1"};
        auto* kept = model.timelineFor(liveRef);
        check(kept != nullptr, "a's timeline is there while it plays");
        if(kept == nullptr) return 1;
        const auto live = model.morphLiveTimelines(sequence, *model.getTrack(sequence, trackId));
        check(live.size() == 1 && live[0].first == kept && live[0].second == aID, "The track unfolds the timeline of the source playing");
        const auto idle = model.morphIdleSources(sequence, *model.getTrack(sequence, trackId));
        check(idle.size() == 1 && idle[0].id == bID && idle[0].timelines.size() == 1 && idle[0].timelines[0]->tracks.size() == 1,
              "...and shows the other source's, read from its folder, without loading it");
        check(engine->origin() != nullptr && engine->destination() == nullptr, "(no graph was made for it)");
        check(!model.morphSourceDirty(sequence, aID, true), "As read from its folder, a source is not dirty");
        const auto edited = model.createTrack(*kept, "Edited here");
        check(!edited.empty() && model.morphSourceDirty(sequence, aID, true), "An edit to its timeline makes it dirty");
        check(!model.morphSourceDirty(sequence, bID, true), "...and only it");
        // Its graph goes (a gap) and comes back: the same timelines, with the edit.
        seek(20.0);
        check(engine->origin() == nullptr && !model.timelineExists(liveRef), "In a gap a's graph and canvas are gone");
        check(model.morphSourceDirty(sequence, aID, true), "The edit is kept while the source does not play");
        seek(2.0);
        if(engine->origin() == nullptr) return 1;
        const TimelineRef againRef{engine->origin()->container->getCanvasID(), "tl_1"};
        check(!(againRef == liveRef), "a plays again, on a new graph");
        check(model.timelineFor(againRef) == kept && model.getTrack(*kept, edited) != nullptr, "...which plays the kept timelines, with the edit");
        check(near(probeOf(engine->origin()).level.get(), 0.25f), "...and they still drive it");
        // Save Source writes them to the folder.
        check(ofLoadJson(a + "/timeline.json").dump().find("Edited here") == std::string::npos, "The folder does not have the edit yet");
        std::string saveError;
        check(model.saveMorphSource(sequence, aID, saveError), "Save Source: " + saveError);
        check(!model.morphSourceDirty(sequence, aID, true), "Saved, it is not dirty");
        const auto written = ofLoadJson(a + "/timeline.json");
        check(written.dump().find("Edited here") != std::string::npos && written.value("version", 0) > 0 &&
              written.contains("timelines") && written["timelines"].size() == 1, "The folder's timeline.json holds the edit");
        // Its patch, once opened on the graph playing it, is saved with it.
        check(!model.morphSourcePatchOpened(sequence, aID), "The patch was not opened");
        check(model.openMorphSourceCanvas(sequence, aID) && model.morphSourcePatchOpened(sequence, aID), "Open Source, while a plays alone");
        // (A value changed on a node's control makes it dirty: the node's GUI
        // records that, and this fixture has no GUI to do it.)
        check(!model.morphSourcePatchDirty(sequence, aID, true), "Opened, nothing in it was changed yet");
        const auto timelineBefore = ofLoadJson(a + "/timeline.json");
        check(model.saveMorphSource(sequence, aID, saveError), "Save Source with the patch open: " + saveError);
        check(!model.morphSourcePatchDirty(sequence, aID, true) && model.morphUnsavedSources().empty(),
              "Saved, the patch is clean and nothing is left to warn about");
        check(fs::exists(a + "/modules.json") && ofLoadJson(a + "/modules.json").contains("Probe") &&
              ofLoadJson(a + "/connections.json").contains("Probe_1"), "The folder is still the preset: its nodes and cables");
        check(!fs::exists(a + "/sequence.json"), "Its timelines did not go to a sequence.json beside the preset's file");
        check(ofLoadJson(a + "/timeline.json").dump().find("Edited here") != std::string::npos &&
              ofLoadJson(a + "/timeline.json").value("version", 0) == timelineBefore.value("version", -1),
              "...but stayed in its timeline.json");
        seek(5.0);
        check(engine->transitioning() && !model.morphSourcePatchOpened(sequence, aID) && !model.morphSourceAlone(sequence, aID),
              "In a crossfade no source is alone: nothing to open or save as a patch");
        seek(2.0);
        check(model.morphSourceAlone(sequence, aID), "(a alone again)");
        // Refresh reads the folder again: unsaved edits go, saved ones stay.
        const auto unsaved = model.createTrack(*kept, "Unsaved");
        check(!unsaved.empty() && model.morphSourceDirty(sequence, aID, true), "Another edit, not saved");
        MacroMixerGui::Action refresh{MacroMixerGui::ActionKind::Refresh};
        refresh.text = engine->macro(aID)->source();
        check(model.applyMorphAction(sequence, refresh, panel, error), "The source is refreshed: " + error);
        seek(2.0);
        check(engine->origin() != nullptr && engine->importedMacro(a) != nullptr, "a plays again after the refresh");
        if(engine->origin() == nullptr || engine->importedMacro(a) == nullptr) return 1;
        const auto* reread = model.timelineFor(TimelineRef{engine->origin()->container->getCanvasID(), "tl_1"});
        bool hasSaved = false, hasUnsaved = false;
        if(reread != nullptr)
            for(const auto& listed : reread->tracks) {
                hasSaved = hasSaved || listed.name == "Edited here";
                hasUnsaved = hasUnsaved || listed.name == "Unsaved";
            }
        check(reread != nullptr && hasSaved && !hasUnsaved, "Refreshed, its timelines are the folder's: the saved edit, not the unsaved one");
        check(!model.morphSourceDirty(sequence, engine->importedMacro(a)->id(), true), "...and it is not dirty");
        check(engine->importedMacro(a)->id() == aID, "(the source keeps its id)");
    }

    // ---- The next source loaded ahead of its clip ----
    {
        model.getTrack(sequence, trackId)->morphPreloadSeconds = 1.0f; // 2 beats at 120 BPM; clip b starts at beat 4
        seek(1.0);
        check(engine->currentID() == aID && !engine->transitioning(), "Three beats before b's clip, a plays alone");
        seek(3.0);
        check(engine->currentID() == aID && engine->destinationID() == bID && engine->transitioning(), "Within the lead, b is loaded ahead");
        check(near(engine->progress(), 0.0f) && near(engine->value(), 0.375f), "...silent: the router is a's alone: " + ofToString(engine->value()));
        const auto* ahead = engine->destination();
        seek(5.0);
        check(engine->destination() == ahead && near(engine->progress(), 0.25f), "The overlap starts on the graph loaded ahead");
        model.getTrack(sequence, trackId)->morphPreloadSeconds = 0.0f;
        seek(3.0);
        check(engine->currentID() == aID && !engine->transitioning(), "Off again, nothing is loaded ahead");
    }

    // ---- Morph in place: one graph for both sources ----
    {
        auto transition = model.getClip(sequence, trackId, clipB)->morphIn;
        transition.morph = true;
        transition.inPlace = true;
        check(model.setMorphTransition(sequence, trackId, clipB, transition), "The transition into b morphs in place");
        seek(2.0);
        const auto* only = engine->origin();
        check(only != nullptr && !engine->transitioning(), "a plays alone before the overlap");
        seek(5.0);
        check(engine->transitioning() && engine->morphingInPlace(), "Entered from a, the morph runs in place: " + engine->sceneInPlaceNote());
        check(engine->origin() == only && engine->destination() == only && engine->currentID() == aID && engine->destinationID() == bID,
              "...on a's graph, towards b");
        check(clocked().size() == 1 && clocked()[0] == only->container.get(), "...clocked once: one graph plays both");
        check(near(engine->progress(), 0.25f), "...its progress the position in the overlap: " + ofToString(engine->progress()));
        seek(4.5);
        check(engine->morphingInPlace() && engine->origin() == only && near(engine->progress(), 0.125f), "Scrubbing back keeps the morph in place");
        seek(10.0);
        check(!engine->transitioning() && engine->currentID() == bID && engine->origin() == only, "Past the overlap, the same graph is b");
        check(near(engine->value(), 0.75f), "...playing b's value: " + ofToString(engine->value()));
        seek(5.0);
        check(engine->transitioning() && engine->morphing() && !engine->morphingInPlace() && !engine->sceneInPlaceNote().empty(),
              "Entered from b, the morph runs on two graphs: " + engine->sceneInPlaceNote());
        seek(2.0);
        check(engine->currentID() == aID && !engine->transitioning(), "a alone again");
        seek(5.0);
        check(engine->morphingInPlace(), "In place again from a");
        seek(2.0);
        check(engine->currentID() == aID && !engine->transitioning() && engine->origin() != nullptr, "Left on the left, the graph is a again");
        if(engine->origin() != nullptr)
            check(near(probeOf(engine->origin()).level.get(), 0.25f), "...driven by a's timeline: " + ofToString(probeOf(engine->origin()).level.get()));
        transition.morph = false;
        transition.inPlace = false;
        model.setMorphTransition(sequence, trackId, clipB, transition);
        seek(5.0);
        check(engine->transitioning() && !engine->morphing(), "Back to Mix");
    }

    // ---- The timelines of a Macro inside a source follow the clip too ----
    {
        const auto clipN = model.addMorphSource(sequence, nested, error);
        check(!clipN.empty(), "The source with a Macro inside is added: " + error);
        if(clipN.empty()) return 1;
        const auto nID = engine->importedMacro(nested)->id();
        const auto* placed = model.getClip(sequence, trackId, clipN);
        const double start = placed->startBeat, end = placed->startBeat + placed->durationBeats;
        // (Where it plays alone: after b's clip.)
        const double alone = std::max(start, 12.0) + 1.0;
        check(alone < end, "The nested source's clip outlasts b's");
        seek(alone);
        check(engine->currentID() == nID && !engine->transitioning() && engine->origin() != nullptr, "The nested source plays alone");
        if(engine->origin() == nullptr) return 1;
        auto& hostedMacro = static_cast<ofxOceanodeNodeMacro&>(
            engine->origin()->container->getDynamicNodes().at("Macro").at(1)->getNodeModel());
        auto innerCanvas = hostedMacro.getContainer();
        auto& innerProbe = static_cast<Probe&>(innerCanvas->getDynamicNodes().at("Probe").at(1)->getNodeModel());
        const auto* innerList = model.getMacroTimelines(*innerCanvas);
        check(innerList != nullptr && innerList->timelines.size() == 1, "The Macro inside brought its own timeline");
        if(innerList == nullptr || innerList->timelines.empty()) return 1;
        const TimelineRef innerRef{innerCanvas->getCanvasID(), innerList->timelines.front()->timelineId};
        check(model.clockSourceOf(innerRef) == ClockSource::Dependent, "...placed by the clip, whatever it saved (free, play on load)");
        const float expected = static_cast<float>((alone - start) / 8.0);
        check(near(innerProbe.level.get(), expected), "...at the clip's own beat: " + ofToString(innerProbe.level.get()) + " for " + ofToString(expected));
        for(int i = 0; i < 10; ++i) frame(0.1);
        check(near(innerProbe.level.get(), expected), "Stopped, it does not run by itself");
        check(near(probeOf(engine->origin()).level.get(), 0.5f), "...next to the source's own timeline");
        MacroMixerGui::Action gone{MacroMixerGui::ActionKind::RemoveMacro};
        gone.text = nID;
        check(model.applyMorphAction(sequence, gone, panel, error) && model.getClip(sequence, trackId, clipN) == nullptr,
              "(the nested source is removed again): " + error);
        seek(5.0);
    }

    // ---- Undo and redo of a source removal: the engine follows the track ----
    {
        // (A router cannot be added while a source that lacks it is imported:
        // the engine refuses that edit, and nothing is recorded.)
        MacroMixerGui::Action more{MacroMixerGui::ActionKind::Routers};
        more.contract = engine->contract();
        auto second = more.contract.routers.front();
        second.id = "second";
        second.name = "Second";
        more.contract.routers.push_back(second);
        check(!model.applyMorphAction(sequence, more, panel, error) && engine->contract().routers.size() == 1 && !error.empty(),
              "A router the sources do not have is refused, with the reason");
        model.beginTimelineEdit(sequence);
        MacroMixerGui::Action drop{MacroMixerGui::ActionKind::RemoveMacro};
        drop.text = bID;
        check(model.applyMorphAction(sequence, drop, panel, error) && engine->macros().size() == 1 &&
              model.getClip(sequence, trackId, clipB) == nullptr, "Source b is removed, with its clip: " + error);
        check(model.endTimelineEdit(sequence), "...as one undo step");
        check(model.undoTimelineEdit(sequence), "Undo");
        frame();
        engine = model.morphEngine(sequence);
        check(engine != nullptr && engine->macros().size() == 2 && engine->macro(bID) != nullptr &&
              model.getClip(sequence, trackId, clipB) != nullptr, "The engine has b again, and its clip is back");
        check(model.redoTimelineEdit(sequence), "Redo");
        frame();
        engine = model.morphEngine(sequence);
        check(engine != nullptr && engine->macros().size() == 1 && model.getClip(sequence, trackId, clipB) == nullptr, "...removed again");
        check(model.undoTimelineEdit(sequence), "Undo again");
        frame();
        engine = model.morphEngine(sequence);
        check(engine != nullptr && engine->macros().size() == 2 && engine->contract().routers.size() == 1, "Both sources, one router");
        if(engine == nullptr) return 1;
        seek(5.0);
        check(engine->currentID() == aID && engine->destinationID() == bID, "...and it plays as before");
    }

    // ---- A target not found is looked up again as soon as nodes change ----
    {
        // (A parameter found one frame after its node is made again was found
        // because a node was made: the wait for a missing target is 30 frames.)
        auto probeAt = [&](int id) -> Probe* {
            const auto& nodes = owner->getDynamicNodes();
            const auto type = nodes.find("Probe");
            if(type == nodes.end() || type->second.count(id) == 0) return nullptr;
            return &static_cast<Probe&>(type->second.at(id)->getNodeModel());
        };
        auto& made = owner->createNode(std::make_unique<Probe>(), 9);
        auto& level = static_cast<ofxOceanodeAbstractParameter&>(made.getParameters().get("Level"));
        const auto bound = model.createTrack("Missing then back");
        const auto binding = model.addBinding(bound, level);
        const auto boundClip = model.createClip(bound, "Clip", 0, 8);
        const auto lane = model.createLane(bound, boundClip, "Level", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(bound, boundClip, lane, binding);
        model.setClipNumber(bound, boundClip, lane, 0, 2.0f);
        seek(2.0);
        check(probeAt(9) != nullptr && near(probeAt(9)->level.get(), 2.0f), "A bound parameter takes its clip's value");
        made.deleteSelf();
        frame(); frame();
        check(probeAt(9) == nullptr, "Its node is deleted: the target is missing");
        owner->createNode(std::make_unique<Probe>(), 9);
        frame();
        check(probeAt(9) != nullptr && near(probeAt(9)->level.get(), 2.0f),
              "Made again, it is found and written on the next frame: " + ofToString(probeAt(9) ? probeAt(9)->level.get() : -9.0f));
        model.removeTrack(sequence, bound);
        // The same for a Global Track's parameter, written again although its
        // value has not moved since it went missing.
        owner->createNode(std::make_unique<Probe>(), 8);
        const auto globalId = model.createGlobalTrack(sequence);
        const auto globalClip = model.getTrack(sequence, globalId)->clips.front().id;
        auto keyframe = [](const char* value) {
            return std::make_shared<const std::map<std::string, std::string>>(std::map<std::string, std::string>{{"Probe_8/Level", value}});
        };
        model.getClip(sequence, globalId, globalClip)->globalKeyframes = {{0, keyframe("1")}, {8, keyframe("3")}};
        seek(4.0);
        auto moved = [&]() { auto* p = probeAt(8); return p != nullptr && p->level.get() > 1.0f && p->level.get() < 3.0f; };
        check(moved(), "A Global Track moves its parameter: " + ofToString(probeAt(8) ? probeAt(8)->level.get() : -9.0f));
        {
            // A repeat pass with the same inputs is skipped (it would write
            // nothing); these change between passes, so the next pass runs.
            auto levelOf8 = [&]() -> ofxOceanodeParameter<float>& {
                return static_cast<ofxOceanodeAbstractParameter&>(
                    owner->getDynamicNodes().at("Probe").at(8)->getParameters().get("Level")).cast<float>();
            };
            // A filter set between passes (a morph starting during the node updates).
            levelOf8().setAutomationFilter([](const float&) { return 3.5f; });
            model.applyAutomation();
            check(near(probeAt(8)->level.get(), 3.5f),
                  "A filter set between passes is written by the next one: " + ofToString(probeAt(8)->level.get()));
            levelOf8().clearAutomationFilter();
            model.applyAutomation();
            check(moved(), "...and once dropped, the next pass lands back on the track's value: " + ofToString(probeAt(8)->level.get()));
            // A node made anew between passes.
            owner->getDynamicNodes().at("Probe").at(8)->deleteSelf();
            owner->createNode(std::make_unique<Probe>(), 8);
            model.applyAutomation();
            check(moved(), "A node made anew between passes is written by the next one: " +
                           ofToString(probeAt(8) ? probeAt(8)->level.get() : -9.0f));
        }
        owner->getDynamicNodes().at("Probe").at(8)->deleteSelf();
        frame(); frame();
        owner->createNode(std::make_unique<Probe>(), 8);
        frame();
        check(moved(), "Its node made again, the Global Track writes it on the next frame: " +
                       ofToString(probeAt(8) ? probeAt(8)->level.get() : -9.0f));
        model.removeTrack(sequence, globalId);
        for(const int id : {8, 9}) if(probeAt(id) != nullptr) owner->getDynamicNodes().at("Probe").at(id)->deleteSelf();
        frame();
    }

    // ---- The track's state and the engine follow each other ----
    // Removing a source removes its clips.
    const auto withBoth = model.getTrack(sequence, trackId)->morphState;
    MacroMixerGui::Action removal{MacroMixerGui::ActionKind::RemoveMacro};
    removal.text = bID;
    check(model.applyMorphAction(sequence, removal, panel, error), "Source b is removed: " + error);
    check(model.getClip(sequence, trackId, clipB) == nullptr && model.getClip(sequence, trackId, clipA) != nullptr, "...with its clip");
    check(model.getTrack(sequence, trackId)->morphState.at("macros").size() == 1, "...and the track's state follows");
    // An older state put back behind the engine (an undo): the engine is restored from it.
    check(model.setMorphState(sequence, trackId, withBoth), "The earlier state is put back");
    frame();
    engine = model.morphEngine(sequence);
    check(engine != nullptr && engine->macros().size() == 2 && engine->macro(bID) != nullptr, "The engine follows the track's state");
    // The track goes: so does its engine.
    seek(2.0);
    check(engine != nullptr && engine->origin() != nullptr, "A source plays before the track is removed");
    check(model.removeTrack(sequence, trackId), "The track is removed");
    frame();
    check(model.morphEngine(sequence) == nullptr, "Its engine went with it");
    for(const auto& listed : model.allCanvases()) check(listed.rfind("MacroMixer/", 0) != 0, "...and its sources: " + listed);

    // ---- A source has one version: its folder's. The project keeps no copy ----
    {
        const auto again = model.createMorphTrack(sequence, "Morph again");
        MacroMixerGui::Action contract{MacroMixerGui::ActionKind::Routers};
        contract.contract = MacroMixerContract{};
        check(!again.empty() && model.applyMorphAction(sequence, contract, panel, error), "A new morph track: " + error);
        check(!model.addMorphSource(sequence, a, error).empty(), "...with source a: " + error);
        seek(2.0);
        auto* playing = model.morphEngine(sequence);
        check(playing != nullptr && playing->origin() != nullptr, "a plays on it");
        if(playing == nullptr || playing->origin() == nullptr) return 1;
        const auto id = playing->importedMacro(a)->id();
        auto* live = model.timelineFor(TimelineRef{playing->origin()->container->getCanvasID(), "tl_1"});
        if(live == nullptr) return 1;
        check(!model.createTrack(*live, "Not saved to the folder").empty() && model.morphSourceDirty(sequence, id, true),
              "An edit to a's timeline, not saved to its folder");
        const auto project = model.toJson();
        check(!project.contains("morphSourceEdits") && project.dump().find("Not saved to the folder") == std::string::npos,
              "The project does not hold the source's timeline");
        // A project written by an earlier version, with a copy of a source's
        // timeline in it: the copy is ignored, the folder is what plays.
        auto older = project;
        older["morphSourceEdits"] = ofJson::array({{{"folder", a}, {"macroFile", false}, {"nextTimelineNumber", 2},
            {"timelines", ofJson::array({{{"id", "tl_1"}, {"tracks", ofJson::array()}}})}}});
        model.fromJson(older);
        auto* reloaded = model.timelineFor(main);
        check(reloaded != nullptr, "The project loads again");
        if(reloaded == nullptr) return 1;
        frame();
        seek(2.0);
        auto* after = model.morphEngine(*reloaded);
        check(after != nullptr && after->origin() != nullptr, "a plays after the reload: " + model.morphEngineStatus(*reloaded));
        if(after == nullptr || after->origin() == nullptr) return 1;
        const auto* restored = model.timelineFor(TimelineRef{after->origin()->container->getCanvasID(), "tl_1"});
        bool unsaved = false, fromFolder = false;
        if(restored != nullptr) for(const auto& listed : restored->tracks) {
            unsaved = unsaved || listed.name == "Not saved to the folder";
            fromFolder = fromFolder || listed.name == "Edited here";
        }
        check(restored != nullptr && fromFolder && !unsaved, "Its timeline is the folder's: the unsaved edit and the stored copy are gone");
        check(!model.morphSourceDirty(*reloaded, after->importedMacro(a)->id(), true), "...and nothing is pending for it");
        check(!model.toJson().contains("morphSourceEdits"), "Saved again, the project drops the old entry");
    }

    if(failures > 0) return 1;
    std::cout << "Macro morphing playback passed: engine per timeline, routers and sources stored in the track, scene at the playhead "
                 "(alone, transition, gap), source timelines placed by their clips (both in an overlap), playing, muted clip and track, "
                 "publishers, source timelines kept across graphs and saved on demand, source loaded ahead, morph in place, missing targets picked up when nodes change, repeat Global Track passes, nested macro timelines, undo of a source removal, removed source, state restored behind the engine, engine gone with its track, no copy of a source kept in the project.\n";
    return 0;
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
