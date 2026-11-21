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

    // Inside the overlap (4..8): both sources, progress the position in it.
    seek(5.0);
    check(engine->currentID() == aID && engine->destinationID() == bID && engine->transitioning(), "At beat 5 a and b are in transition");
    check(engine->origin() == graphA, "Entering the overlap keeps a's graph");
    const auto* graphB = engine->destination();
    if(graphB == nullptr) return 1;
    check(near(engine->progress(), 0.25f), "Progress is the playhead's position in the overlap: " + ofToString(engine->progress()));
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

    if(failures > 0) return 1;
    std::cout << "Macro morphing playback passed: engine per timeline, routers and sources stored in the track, scene at the playhead "
                 "(alone, transition, gap), source timelines placed by their clips (both in an overlap), playing, muted clip and track, "
                 "publishers, source timelines kept across graphs and saved on demand, removed source, state restored behind the engine, engine gone with its track.\n";
    return 0;
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
