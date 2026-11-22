// Macro Morphing Track engines: the MacroMixerRuntime of each timeline that
// has the track, kept by the primary manager. The track's clips and overlaps
// become, every frame, the scene the engine is told (MacroMixerRuntime::
// present) at the timeline's playhead; the engine's state (routers, sources,
// kept-out nodes, default policies) is the track's morphState. The Timeline
// nodes pointing at the timeline publish its routers.

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "Nodes/MacroMixerRuntime.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#include "Nodes/MacroMixerGui.h"
#include "Nodes/MacroMixerPorts.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace {
// A transition as its clip keeps it (JSON, written by the editor): the morph
// policies by type, {type: {policy, options}}, and the mix policy of each
// router it overrides, {routerId: {policy, options}}. Anything else is skipped.
bool readPolicy(const ofJson& item, std::string& policy, ofJson& options) {
    if(!item.is_object() || !item.contains("policy") || !item["policy"].is_string()) return false;
    policy = item["policy"].get<std::string>();
    options = item.contains("options") && item["options"].is_object() ? item["options"] : ofJson::object();
    return !policy.empty();
}

void readTransition(const ofxOceanodeTimelineMorphTransition& transition, MacroMixerMorphPolicies& policies,
                    std::map<std::string, MacroMixerContract::Router>& overrides) {
    policies.clear();
    overrides.clear();
    if(transition.morphPolicies.is_object())
        for(auto item = transition.morphPolicies.begin(); item != transition.morphPolicies.end(); ++item) {
            MacroMixerMorphPolicy policy;
            if(readPolicy(item.value(), policy.policy, policy.options)) policies[item.key()] = std::move(policy);
        }
    if(transition.routerOverrides.is_object())
        for(auto item = transition.routerOverrides.begin(); item != transition.routerOverrides.end(); ++item) {
            MacroMixerContract::Router router;
            router.id = item.key();
            if(readPolicy(item.value(), router.policy, router.options)) overrides[item.key()] = std::move(router);
        }
}

// What the user changed by hand in a canvas and the Macros inside it, as a
// number that moves with every such change: values edited on a node's
// controls (each container counts its own) and canvas gestures (nodes,
// cables, positions: one history for the whole graph).
uint64_t patchEditCount(ofxOceanodeContainer& canvas, int depth = 0) {
    uint64_t count = canvas.getUserEditSerial();
    if(depth == 0) count += canvas.getCanvasHistoryRevision();
    if(depth < 32)
        for(auto* node : canvas.getAllModules())
            if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel()))
                if(auto inner = macro->getContainer()) count += patchEditCount(*inner, depth + 1);
    return count;
}

// An engine's state without the values of its Input routers: those follow
// the Timeline nodes' ports and are no edit of the track.
ofJson withoutInputs(ofJson state) {
    if(state.is_object()) state.erase("inputs");
    return state;
}
} // namespace

struct ofxOceanodeTimelineManager::MorphEngine {
    MorphEngine(ofxOceanodeContainer& parentCanvas, std::shared_ptr<ofxOceanodeContainer> held)
        : parent(&parentCanvas), keepAlive(std::move(held)) {
        // The blocked-node dialog asks about this engine's sources; its
        // answers are queued in gui (applyMorphAnswers).
        MacroMixerGui::addBlockedOwner(gui, [this]() -> const MacroMixerRuntime* { return runtime.get(); });
    }
    ~MorphEngine() { MacroMixerGui::removeBlockedOwner(gui); }
    MorphEngine(const MorphEngine&) = delete;
    MorphEngine& operator=(const MorphEngine&) = delete;

    // A new engine with this state (null: no routers, no sources). The
    // previous one goes, with its graphs.
    void restore(const ofJson& state) {
        auto fresh = std::make_unique<MacroMixerRuntime>(*parent);
        MacroMixerContract none;
        none.routers.clear();
        fresh->setContract(none);
        problem.clear();
        if(!state.is_null()) {
            // Every source is read (no node is made), as after loading a preset.
            if(fresh->restoreState(state)) fresh->checkSources();
            else {
                problem = "The track's routers and sources could not be restored";
                const auto reason = MacroMixerGui::failureText(*fresh);
                if(!reason.empty()) problem += ": " + reason;
            }
        }
        runtime = std::move(fresh);
        gui.actions.clear();
        gui.answeredBlocked.clear();
        outputs.clear();
        pendingFolders.clear();
        transitionKnown = false;
        checksWanted = false;
    }

    // Before an edit the engine refuses while two sources play (inside an
    // overlap): the one further along stays live, alone; the other loads
    // again with the next frame's scene.
    void holdOne() {
        if(runtime == nullptr || !runtime->transitioning()) return;
        // (A morph in place ends on that side: committed, or rolled back.)
        MacroMixerScene scene;
        scene.origin = progress >= 0.5f ? runtime->destinationID() : runtime->currentID();
        if(scene.origin.empty()) scene.origin = runtime->currentID();
        runtime->present(scene);
    }

    // The canvas the engine's graphs nest under: the timeline's. A macro's
    // is held, so it outlives the engine (which is released in the next
    // frame pass once its macro is gone).
    ofxOceanodeContainer* parent = nullptr;
    std::shared_ptr<ofxOceanodeContainer> keepAlive;
    std::unique_ptr<MacroMixerRuntime> runtime;
    MacroMixerGui::State gui;
    // The track it was restored from, and that track's state stamp then.
    std::string trackId;
    uint64_t stateStamp = 0;
    // Why its state could not be restored (the track's state is then left alone).
    std::string problem;
    // The Output routers as read this frame, for every Timeline node.
    MacroMixerPorts::Values outputs;
    // The live graphs as last seen (compared, never dereferenced) with their canvases.
    std::vector<std::pair<const MacroMixerRuntime::Graph*, std::string>> graphs;
    uint64_t drawnFrame = 0;
    uint64_t updatedFrame = 0;
    // The graph whose patch was opened in a canvas tab (compared, never dereferenced).
    const MacroMixerRuntime::Graph* patchGraph = nullptr;
    // Its count of hand edits (patchEditCount) when it was opened or last saved.
    uint64_t patchRevision = 0;
    // (The count walks the graph's nodes: looked at a few times a second.)
    mutable uint64_t patchSeen = 0;
    mutable float patchSeenAt = -1.0f;
    // The patch opened on the graph playing alone now, and changed since.
    bool patchDirty(bool exact = false) const {
        if(runtime == nullptr || patchGraph == nullptr || runtime->transitioning() || runtime->origin() != patchGraph) return false;
        if(patchGraph->container == nullptr) return false;
        const float now = ofGetElapsedTimef();
        if(exact || patchSeenAt < 0.0f || now - patchSeenAt > 0.25f || now < patchSeenAt) {
            patchSeen = patchEditCount(*patchGraph->container);
            patchSeenAt = now;
        }
        return patchSeen != patchRevision;
    }
    // How far through its overlap the scene last told was (0 outside one).
    float progress = 0.0f;
    // Source folders whose import waits on the blocked-node dialog: each
    // gets its clip once it is in.
    std::set<std::string> pendingFolders;
    // The transition last read from its clip's JSON.
    bool transitionKnown = false;
    std::string transitionClip;
    uint64_t transitionStamp = 0;
    MacroMixerMorphPolicies transitionPolicies;
    std::map<std::string, MacroMixerContract::Router> transitionOverrides;
    // Nothing plays and the timeline is stopped: a requested source check may run.
    bool idle = false;
    bool checksWanted = false;
};

// ---- Engines ----

ofxOceanodeTimelineManager::MorphEngine* ofxOceanodeTimelineManager::findMorphEngine(
        const ofxOceanodeTimelineSequence& sequence) const {
    if(isEngine() || morphEngines.empty()) return nullptr;
    TimelineRef ref;
    if(!refOf(sequence, ref)) return nullptr;
    const auto found = morphEngines.find(normalizedRef(ref));
    return found == morphEngines.end() ? nullptr : found->second.get();
}

ofxOceanodeTimelineManager::MorphEngine* ofxOceanodeTimelineManager::ensureMorphEngine(
        const TimelineRef& ref, const ofxOceanodeTimelineTrack& track, bool follow) {
    if(isEngine() || container == nullptr || !track.isMorphTrack) return nullptr;
    auto* parent = canvasContainer(ref.canvas);
    if(parent == nullptr) return nullptr;
    auto found = morphEngines.find(ref);
    // Another canvas under the same name (its macro was made again).
    if(found != morphEngines.end() && found->second->parent != parent) {
        releaseMorphEngine(ref);
        found = morphEngines.end();
    }
    if(found == morphEngines.end()) {
        std::shared_ptr<ofxOceanodeContainer> held;
        if(parent != container) {
            pruneMacroSequences();
            for(const auto& entry : macroSequences)
                if(entry.address == parent) held = entry.container.lock();
            if(held == nullptr) return nullptr;
        }
        auto engine = std::make_shared<MorphEngine>(*parent, std::move(held));
        engine->restore(track.morphState);
        engine->trackId = track.id;
        engine->stateStamp = track.morphStateStamp;
        found = morphEngines.emplace(ref, std::move(engine)).first;
        return found->second.get();
    }
    auto& engine = *found->second;
    if(follow && (engine.trackId != track.id || engine.stateStamp != track.morphStateStamp)) {
        // Its track's state was replaced behind it (an undo, a redo, a load,
        // another track): the engine follows. One that already is in that
        // state (the stamp alone moved) keeps playing.
        const bool same = engine.trackId == track.id && engine.problem.empty() && engine.runtime != nullptr &&
                          !track.morphState.is_null() &&
                          withoutInputs(engine.runtime->saveState()) == withoutInputs(track.morphState);
        if(!same) {
            engine.restore(track.morphState);
            noteMorphGraphs(engine);
        }
        engine.trackId = track.id;
        engine.stateStamp = track.morphStateStamp;
    }
    return &engine;
}

MacroMixerRuntime* ofxOceanodeTimelineManager::morphEngine(const ofxOceanodeTimelineSequence& sequence) {
    if(isEngine()) return nullptr;
    TimelineRef ref;
    const auto* track = morphTrackOf(sequence);
    if(track == nullptr || !refOf(sequence, ref)) return nullptr;
    auto* engine = ensureMorphEngine(normalizedRef(ref), *track, false);
    return engine != nullptr ? engine->runtime.get() : nullptr;
}

const MacroMixerRuntime* ofxOceanodeTimelineManager::morphEngine(const ofxOceanodeTimelineSequence& sequence) const {
    if(morphTrackOf(sequence) == nullptr) return nullptr;
    const auto* engine = findMorphEngine(sequence);
    return engine != nullptr ? engine->runtime.get() : nullptr;
}

void ofxOceanodeTimelineManager::releaseMorphEngine(const TimelineRef& ref) {
    const auto found = morphEngines.find(ref);
    if(found == morphEngines.end()) return;
    // Out of the list first: its graphs go as it is destroyed, and what
    // they call back into finds no engine half gone.
    auto engine = std::move(found->second);
    morphEngines.erase(found);
    const bool hadGraphs = engine != nullptr && !engine->graphs.empty();
    engine.reset();
    if(!hadGraphs) return;
    invalidateEvaluationIndexes();
    for(auto& player : players)
        if(player->engine != nullptr) {
            player->engine->invalidateEvaluationIndexes();
            player->engine->invalidateSchedule();
        }
    parameterCache.clear();
}

void ofxOceanodeTimelineManager::releaseMorphEngines(bool rootOnly) {
    std::vector<TimelineRef> refs;
    for(const auto& entry : morphEngines)
        if(!rootOnly || entry.first.canvas.empty()) refs.push_back(entry.first);
    for(const auto& ref : refs) releaseMorphEngine(ref);
}

void ofxOceanodeTimelineManager::noteMorphGraphs(MorphEngine& engine) {
    std::vector<std::pair<const MacroMixerRuntime::Graph*, std::string>> now;
    if(engine.runtime != nullptr)
        for(const MacroMixerRuntime::Graph* graph : {engine.runtime->origin(), engine.runtime->destination()}) {
            if(graph == nullptr || graph->container == nullptr) continue;
            if(!now.empty() && now.back().first == graph) continue;
            // (With the source whose timelines it holds: a morph in place
            // puts the destination's on the origin's canvas.)
            const std::string source = engine.runtime->morphingInPlace() ? engine.runtime->destinationID()
                                     : graph->revision != nullptr ? graph->revision->id() : std::string();
            now.push_back({graph, graph->container->getCanvasID() + "#" + source});
        }
    if(now == engine.graphs) return;
    engine.graphs = std::move(now);
    // A graph is a canvas of its own ("MacroMixer/<n> / ..."), made anew by
    // every build: where the sources' timelines are placed, every engine's
    // indexes (their played copies point into the graphs) and what was
    // handed ahead to the backends follow.
    invalidateEvaluationIndexes();
    for(auto& player : players)
        if(player->engine != nullptr) {
            player->engine->invalidateEvaluationIndexes();
            player->engine->invalidateSchedule();
        }
    parameterCache.clear();
}

std::vector<ofxOceanodeTimelineManager::MorphGraph> ofxOceanodeTimelineManager::morphGraphsOf(const TimelineRef& ref) const {
    std::vector<MorphGraph> graphs;
    const auto found = morphEngines.find(ref);
    if(found == morphEngines.end() || found->second == nullptr || found->second->runtime == nullptr) return graphs;
    const auto& runtime = *found->second->runtime;
    for(const MacroMixerRuntime::Graph* graph : {runtime.origin(), runtime.destination()}) {
        if(graph == nullptr || graph->container == nullptr || graph->revision == nullptr) continue;
        if(!graphs.empty() && graphs.back().canvas == graph->container.get()) continue;
        // A morph in place: one graph, holding the destination's timelines
        // from the start of the morph (the destination's clips place them).
        graphs.push_back({runtime.morphingInPlace() ? runtime.destinationID() : graph->revision->id(),
                          graph->container.get(), graph->container->getCanvasID()});
    }
    return graphs;
}

std::vector<std::pair<ofxOceanodeTimelineManager::TimelineRef, ofxOceanodeTimelineManager::TimelineRef>>
ofxOceanodeTimelineManager::morphPlacedTimelines() const {
    std::vector<std::pair<TimelineRef, TimelineRef>> placed;
    for(const auto& engine : morphEngines)
        for(const auto& graph : morphGraphsOf(engine.first)) {
            // The source's own timelines, then those of the macros inside it,
            // at any depth: the whole source follows the clip playing it.
            // (One a timeline track places stays that track's: see refreshPlacedTimelines.)
            const std::string inside = graph.canvasId + " / ";
            for(const auto& entry : macroSequences) {
                if(entry.address == nullptr || entry.container.expired()) continue;
                const std::string canvas = entry.address == graph.canvas ? graph.canvasId : entry.address->getCanvasID();
                if(entry.address != graph.canvas && canvas.compare(0, inside.size(), inside) != 0) continue;
                for(const auto& timeline : entry.timelines->timelines)
                    placed.push_back({TimelineRef{canvas, timeline->timelineId}, engine.first});
            }
        }
    return placed;
}

void ofxOceanodeTimelineManager::storeMorphState(ofxOceanodeTimelineSequence& sequence, MorphEngine& engine) {
    const auto* track = morphTrackOf(sequence);
    // (A state that could not be restored stays as its file had it.)
    if(track == nullptr || engine.runtime == nullptr || !engine.problem.empty()) return;
    const auto state = engine.runtime->saveState();
    const std::string trackId = track->id;
    if(state != track->morphState) setMorphState(sequence, trackId, state);
    engine.trackId = trackId;
    engine.stateStamp = track->morphStateStamp;
}

// ---- The sources' own timelines ----

std::vector<std::string> ofxOceanodeTimelineManager::morphSourceFolders() const {
    std::vector<std::string> folders;
    for(const auto& entry : morphEngines) {
        if(entry.second == nullptr || entry.second->runtime == nullptr) continue;
        for(const auto& source : entry.second->runtime->macros())
            if(source != nullptr) folders.push_back(source->source());
    }
    return folders;
}

std::string ofxOceanodeTimelineManager::morphSourceFolder(const ofxOceanodeTimelineSequence& host,
                                                          const std::string& sourceId) const {
    const auto* engine = findMorphEngine(host);
    if(engine == nullptr || engine->runtime == nullptr) return std::string();
    const auto source = engine->runtime->macro(sourceId);
    return source != nullptr ? source->source() : std::string();
}

std::vector<std::pair<ofxOceanodeTimelineSequence*, std::string>> ofxOceanodeTimelineManager::morphLiveTimelines(
        const ofxOceanodeTimelineSequence& host, const ofxOceanodeTimelineTrack& track) {
    std::vector<std::pair<ofxOceanodeTimelineSequence*, std::string>> live;
    TimelineRef hostRef;
    if(isEngine() || !track.isMorphTrack || !refOf(host, hostRef)) return live;
    hostRef = normalizedRef(hostRef);
    if(!placedTimelinesValid) refreshPlacedTimelines();
    pruneMacroSequences();
    // Origin first, then the destination of a transition.
    for(const auto& graph : morphGraphsOf(hostRef))
        for(const auto& entry : macroSequences) {
            if(entry.address != graph.canvas) continue;
            for(const auto& timeline : entry.timelines->timelines) {
                // (One a timeline track of its own canvas places is that one's.)
                const auto placer = placedTimelines.find(TimelineRef{graph.canvasId, timeline->timelineId});
                if(placer == placedTimelines.end() || !(placer->second == hostRef)) continue;
                live.push_back({timeline.get(), graph.source});
            }
        }
    return live;
}

bool ofxOceanodeTimelineManager::morphSourceAlone(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const {
    const auto* engine = findMorphEngine(host);
    if(engine == nullptr || engine->runtime == nullptr || sourceId.empty()) return false;
    const auto& runtime = *engine->runtime;
    return runtime.origin() != nullptr && !runtime.transitioning() && runtime.currentID() == sourceId;
}

bool ofxOceanodeTimelineManager::morphInTransition(const ofxOceanodeTimelineSequence& host) const {
    const auto* engine = findMorphEngine(host);
    return engine != nullptr && engine->runtime != nullptr && engine->runtime->transitioning();
}

bool ofxOceanodeTimelineManager::morphSourceCanvasOpen(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const {
    if(!morphSourceAlone(host, sourceId)) return false;
    const auto* graph = findMorphEngine(host)->runtime->origin();
    return graph->macro != nullptr && graph->macro->isWindowVisible();
}

bool ofxOceanodeTimelineManager::openMorphSourceCanvas(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) {
    if(!morphSourceAlone(host, sourceId)) return false;
    const auto* graph = findMorphEngine(host)->runtime->origin();
    if(graph->macro == nullptr) return false;
    // The Macro's own tab: docked with the other canvases, and focused.
    graph->macro->setWindowVisible(true);
    auto* engine = findMorphEngine(host);
    if(engine->patchGraph != graph) {
        engine->patchGraph = graph;
        engine->patchRevision = graph->container != nullptr ? patchEditCount(*graph->container) : 0;
        engine->patchSeenAt = -1.0f;
    }
    return true;
}

bool ofxOceanodeTimelineManager::morphSourcePatchDirty(const ofxOceanodeTimelineSequence& host, const std::string& sourceId,
                                                       bool exact) const {
    if(!morphSourceAlone(host, sourceId)) return false;
    return findMorphEngine(host)->patchDirty(exact);
}

std::vector<std::string> ofxOceanodeTimelineManager::morphUnsavedSources() {
    std::vector<std::string> unsaved;
    if(isEngine()) return unsaved;
    for(const auto* document : dirtyMorphDocuments())
        unsaved.push_back(ofFilePath::getFileName(ofFilePath::removeTrailingSlash(document->folder)) + " (timeline)");
    for(const auto& entry : morphEngines)
        if(entry.second != nullptr && entry.second->patchDirty() && entry.second->patchGraph->revision != nullptr)
            unsaved.push_back(entry.second->patchGraph->revision->name() + " (patch)");
    return unsaved;
}

bool ofxOceanodeTimelineManager::saveMorphUnsavedSources(std::string& error) {
    error.clear();
    if(isEngine()) return true;
    bool saved = true;
    auto fail = [&](const std::string& reason) {
        saved = false;
        if(!reason.empty()) error += (error.empty() ? "" : "\n") + reason;
    };
    // Patches first: a patch is written with its timelines.
    std::vector<std::pair<TimelineRef, std::string>> patches;
    for(const auto& entry : morphEngines)
        if(entry.second != nullptr && entry.second->patchDirty() && entry.second->patchGraph->revision != nullptr)
            patches.push_back({entry.first, entry.second->patchGraph->revision->id()});
    for(const auto& patch : patches) {
        const auto* host = findTimeline(patch.first);
        std::string reason;
        if(host == nullptr || !saveMorphSourcePatch(*host, patch.second, reason)) fail(reason);
    }
    for(auto* document : dirtyMorphDocuments()) {
        std::string reason;
        if(!writeMorphDocument(*document, reason)) fail(reason);
    }
    return saved;
}

bool ofxOceanodeTimelineManager::morphSourcePatchOpened(const ofxOceanodeTimelineSequence& host, const std::string& sourceId) const {
    if(!morphSourceAlone(host, sourceId)) return false;
    const auto* engine = findMorphEngine(host);
    return engine->patchGraph != nullptr && engine->patchGraph == engine->runtime->origin();
}

bool ofxOceanodeTimelineManager::saveMorphSourcePatch(const ofxOceanodeTimelineSequence& host, const std::string& sourceId,
                                                     std::string& error) {
    error.clear();
    if(!morphSourcePatchOpened(host, sourceId)) {
        error = "The source's patch can be saved while it plays alone, after opening it.";
        return false;
    }
    const auto* graph = findMorphEngine(host)->runtime->origin();
    const std::string folder = graph->source;
    if(graph->container == nullptr || folder.empty() || !ofFile::doesFileExist(ofFilePath::join(folder, "modules.json"))) {
        error = "The source's folder is not a preset any more: " + folder;
        return false;
    }
    // What a preset keeps for the project it is opened as, which this graph
    // (a Macro in another project) does not hold: left exactly as it is.
    const std::vector<std::string> projectFiles{"scope_config.json", "midi.json", "transport.json", "transportSync.json", "layout.json"};
    std::map<std::string, std::string> kept;
    for(const auto& name : projectFiles) {
        const auto path = ofFilePath::join(folder, name);
        if(ofFile::doesFileExist(path)) kept[name] = ofBufferFromFile(path).getText();
    }
    // The preset as its container saves itself: its nodes as they are now and
    // their cables; the nodes kept out of the graph as the folder had them
    // (the container kept them aside when it loaded); its timelines through
    // saveMacroSequence, into the file they were read from.
    const bool saved = graph->container->savePreset(folder);
    for(const auto& name : projectFiles) {
        const auto path = ofFilePath::join(folder, name);
        const auto original = kept.find(name);
        if(original != kept.end()) ofBufferToFile(path, ofBuffer(original->second.data(), original->second.size()));
        else if(ofFile::doesFileExist(path)) ofFile::removeFile(path);
    }
    if(!saved) {
        error = "The source's patch could not be saved to " + folder;
        return false;
    }
    auto* engine = findMorphEngine(host);
    engine->patchRevision = patchEditCount(*graph->container);
    engine->patchSeenAt = -1.0f;
    return true;
}

std::vector<ofxOceanodeTimelineMorphIdleSource> ofxOceanodeTimelineManager::morphIdleSources(
        const ofxOceanodeTimelineSequence& host, const ofxOceanodeTimelineTrack& track) {
    std::vector<ofxOceanodeTimelineMorphIdleSource> idle;
    TimelineRef hostRef;
    const auto* engine = findMorphEngine(host);
    if(isEngine() || !track.isMorphTrack || engine == nullptr || engine->runtime == nullptr || !refOf(host, hostRef)) return idle;
    std::set<std::string> seen;
    for(const auto& graph : morphGraphsOf(normalizedRef(hostRef))) seen.insert(graph.source);   // live: unfolded as rows
    // In the order of their first clips.
    std::vector<const ofxOceanodeTimelineClip*> clips;
    for(const auto& clip : track.clips) clips.push_back(&clip);
    std::stable_sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) { return a->startBeat < b->startBeat; });
    for(const auto* clip : clips) {
        if(clip->morphSource.empty() || !seen.insert(clip->morphSource).second) continue;
        const auto source = engine->runtime->macro(clip->morphSource);
        if(source == nullptr) continue;
        ofxOceanodeTimelineMorphIdleSource entry;
        entry.id = clip->morphSource;
        entry.name = source->name();
        if(const auto* document = ensureMorphDocument(source->source()))
            for(const auto& timeline : document->timelines->timelines) entry.timelines.push_back(timeline.get());
        idle.push_back(std::move(entry));
    }
    return idle;
}

// ---- The panels' edits ----

bool ofxOceanodeTimelineManager::applyMorphAction(ofxOceanodeTimelineSequence& sequence, const MacroMixerGui::Action& action,
                                                  MacroMixerGui::State& state, std::string& error) {
    TimelineRef ref;
    const auto* track = morphTrackOf(sequence);
    auto* engine = !isEngine() && track != nullptr && refOf(sequence, ref)
        ? ensureMorphEngine(normalizedRef(ref), *track, false) : nullptr;
    if(engine == nullptr || engine->runtime == nullptr) {
        error = "This timeline has no Macro Morphing Track";
        return false;
    }
    if(!engine->problem.empty()) {
        error = engine->problem;
        return false;
    }
    auto& runtime = *engine->runtime;
    using Kind = MacroMixerGui::ActionKind;
    // The engine refuses these while two sources play (inside an overlap):
    // one of them is released first, and loads again with the next frame's scene.
    const bool needsOneGraph = action.kind == Kind::Import || action.kind == Kind::Refresh || action.kind == Kind::Routers ||
                               action.kind == Kind::LoadRouters || action.kind == Kind::MorphPolicy ||
                               action.kind == Kind::KeepOut;
    if(needsOneGraph) engine->holdOne();
    // The folder of the source a Refresh or a removal is about, as the engine spells it.
    std::string documentFolder;
    if(action.kind == Kind::Refresh) documentFolder = action.text;
    else if(action.kind == Kind::RemoveMacro)
        if(const auto source = runtime.macro(action.text)) documentFolder = source->source();
    if(action.kind == Kind::Refresh) {
        // Read again from its folder: its graph goes, and with it the
        // timelines kept since the first load (their unsaved edits).
        runtime.present(MacroMixerScene{});
        dropMorphDocument(documentFolder);
    }
    const bool applied = MacroMixerGui::apply(runtime, action, state, error, engine->parent);
    if(applied && action.kind == Kind::CheckStatus) engine->checksWanted = true;
    if(applied && action.kind == Kind::RemoveMacro) {
        // Its clips go with it.
        const std::string trackId = track->id;
        std::vector<std::string> gone;
        for(const auto& clip : track->clips) if(clip.morphSource == action.text) gone.push_back(clip.id);
        for(const auto& clipId : gone) removeClip(sequence, trackId, clipId);
        dropMorphDocument(documentFolder);
    }
    storeMorphState(sequence, *engine);
    noteMorphGraphs(*engine);
    return applied;
}

double ofxOceanodeTimelineManager::morphSourceLength(const std::string& folder) {
    // As loadMacroSequence reads a source: a macro's own timelines
    // (sequence.json), else a preset's (timeline.json).
    std::string file = ofFilePath::join(folder, "sequence.json");
    const bool macroFile = ofFile::doesFileExist(file);
    if(!macroFile) file = ofFilePath::join(folder, "timeline.json");
    if(!ofFile::doesFileExist(file)) return 0.0;
    ofxOceanodeTimelineCanvas saved;
    canvasFromJson(saved, upgradeTimelineDocument(ofLoadJson(file), macroFile));
    if(saved.timelines.empty()) return 0.0;
    return macroTimelineLength(*saved.timelines.front());
}

std::string ofxOceanodeTimelineManager::addMorphSource(ofxOceanodeTimelineSequence& sequence, const std::string& folder,
                                                       std::string& error) {
    error.clear();
    TimelineRef ref;
    const auto* track = morphTrackOf(sequence);
    auto* engine = !isEngine() && track != nullptr && refOf(sequence, ref)
        ? ensureMorphEngine(normalizedRef(ref), *track, false) : nullptr;
    if(engine == nullptr || engine->runtime == nullptr) {
        error = "This timeline has no Macro Morphing Track";
        return std::string();
    }
    if(!engine->problem.empty()) {
        error = engine->problem;
        return std::string();
    }
    auto& runtime = *engine->runtime;
    auto source = runtime.importedMacro(folder);
    if(source == nullptr) {
        // (An import is refused inside a transition: see applyMorphAction.)
        engine->holdOne();
        if(!runtime.importMacro(folder)) {
            error = MacroMixerGui::failureText(runtime);
            if(!runtime.blockedNodes().empty()) {
                // The dialog asks about each blocked node; the clip is made
                // once they are all kept out and the source is in. Nothing
                // to report meanwhile: no clip and no error.
                engine->pendingFolders.insert(folder);
                error.clear();
            } else if(error.empty()) error = "The source could not be read";
            noteMorphGraphs(*engine);
            return std::string();
        }
        source = runtime.importedMacro(folder);
    }
    if(source == nullptr) {
        error = "The source could not be read";
        return std::string();
    }
    const std::string trackId = track->id;
    const std::string sourceId = source->id();
    double length = morphSourceLength(source->source());
    if(!(length > 0.0)) length = 8.0 * beatsPerBarOf(sequence);
    storeMorphState(sequence, *engine);
    noteMorphGraphs(*engine);
    const auto clipId = appendMorphClip(sequence, trackId, sourceId, length);
    if(clipId.empty()) {
        error = "No room for a clip of this source on the track";
        return std::string();
    }
    // As a macro clip: its content is one pass of the source's timeline, repeated.
    if(auto* clip = getClip(sequence, trackId, clipId)) {
        clip->name = source->name();   // (not its id, which the clip keeps in morphSource)
        clip->contentDurationBeats = length;
        clip->contentStretch = 1.0;
        clip->repeatContent = true;
    }
    invalidateEvaluationIndexes();
    return clipId;
}

void ofxOceanodeTimelineManager::applyMorphAnswers(const TimelineRef& ref, MorphEngine& engine) {
    if(engine.runtime == nullptr || (engine.gui.actions.empty() && engine.pendingFolders.empty())) return;
    auto& runtime = *engine.runtime;
    while(!engine.gui.actions.empty()) {
        const auto action = std::move(engine.gui.actions.front());
        engine.gui.actions.pop_front();
        // (The last answer reads the source again, which a transition refuses.)
        if(action.kind == MacroMixerGui::ActionKind::KeepOut) engine.holdOne();
        std::string error;
        MacroMixerGui::apply(runtime, action, engine.gui, error, engine.parent);
    }
    auto* sequence = findTimeline(ref);
    const auto* track = sequence != nullptr ? morphTrackOf(*sequence) : nullptr;
    for(auto it = engine.pendingFolders.begin(); it != engine.pendingFolders.end();) {
        const auto source = runtime.importedMacro(*it);
        if(source == nullptr) {
            // Still asked about; or cancelled, and the source stays out.
            if(runtime.blockedNodes().empty()) it = engine.pendingFolders.erase(it); else ++it;
            continue;
        }
        if(sequence != nullptr && track != nullptr) {
            const std::string trackId = track->id;
            double length = morphSourceLength(source->source());
            if(!(length > 0.0)) length = 8.0 * beatsPerBarOf(*sequence);
            const auto clipId = appendMorphClip(*sequence, trackId, source->id(), length);
            if(auto* clip = getClip(*sequence, trackId, clipId)) {
                clip->name = source->name();
                clip->contentDurationBeats = length;
                clip->contentStretch = 1.0;
                clip->repeatContent = true;
            }
        }
        it = engine.pendingFolders.erase(it);
    }
    if(sequence != nullptr) storeMorphState(*sequence, engine);
}

// ---- What the editor shows ----

std::string ofxOceanodeTimelineManager::morphSourceName(const ofxOceanodeTimelineSequence& sequence,
                                                        const std::string& sourceId) const {
    if(const auto* engine = findMorphEngine(sequence); engine != nullptr && engine->runtime != nullptr)
        if(const auto source = engine->runtime->macro(sourceId)) return source->name();
    // Before its engine exists (or restored): as the track saved it.
    const auto* track = morphTrackOf(sequence);
    if(track != nullptr && track->morphState.is_object() && track->morphState.contains("macros") &&
       track->morphState["macros"].is_array())
        for(const auto& entry : track->morphState["macros"]) {
            if(!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
               entry["id"].get<std::string>() != sourceId) continue;
            if(entry.contains("name") && entry["name"].is_string()) return entry["name"].get<std::string>();
        }
    return sourceId;
}

std::string ofxOceanodeTimelineManager::morphEngineStatus(const ofxOceanodeTimelineSequence& sequence) const {
    const auto* engine = findMorphEngine(sequence);
    if(engine == nullptr || engine->runtime == nullptr) return std::string();
    if(!engine->problem.empty()) return engine->problem;
    const auto& runtime = *engine->runtime;
    // Why the scene is not ready (a source that failed, a backend), else the last failure.
    if(runtime.sceneDriven() && !runtime.sceneReady()) {
        const auto status = runtime.sceneStatus();
        if(!status.empty()) return status;
    }
    return MacroMixerGui::failureText(runtime);
}

std::vector<ofxOceanodeTimelineMorphWarning> ofxOceanodeTimelineManager::morphClipWarnings(
        const ofxOceanodeTimelineSequence& sequence, const ofxOceanodeTimelineClip& clip) const {
    std::vector<ofxOceanodeTimelineMorphWarning> warnings;
    const auto* engine = findMorphEngine(sequence);
    if(engine == nullptr || engine->runtime == nullptr) return warnings;
    if(!engine->problem.empty()) { warnings.push_back({true, engine->problem}); return warnings; }
    const auto& runtime = *engine->runtime;
    const auto source = runtime.macro(clip.morphSource);
    if(source == nullptr) { warnings.push_back({true, "The track's engine does not have this clip's source"}); return warnings; }
    // The source itself: it could not be read, or failed when the scene asked for it.
    if(const auto problem = runtime.sourceProblem(clip.morphSource); !problem.empty()) warnings.push_back({false, problem});
    if(runtime.sceneDriven() && !runtime.sceneReady()) {
        const auto status = runtime.sceneStatus();
        const std::string mine = source->name() + ": ";
        if(status.compare(0, mine.size(), mine) == 0) warnings.push_back({true, status.substr(mine.size())});
    }
    // The last transition into it, as its Details recorded it (never a
    // preview: that would read the source again). Red: the graph fell back;
    // orange: a node was replaced.
    if(runtime.hasTransitionDetails(clip.morphSource))
        for(const auto& detail : runtime.sourceDetails(clip.morphSource)) {
            const bool severe = detail.kind == "fallback" || detail.kind == "skipped";
            if(!severe && detail.kind != "replaced") continue;
            const char* kind = detail.kind == "fallback" ? "Graph fallback" : detail.kind == "skipped" ? "Skipped connection mix" : "Replaced node";
            std::string text = kind;
            if(!detail.location.empty() && detail.location != "Graph") text += " (" + detail.location + ")";
            if(!detail.message.empty()) text += ": " + detail.message;
            warnings.push_back({severe, std::move(text)});
        }
    return warnings;
}

bool ofxOceanodeTimelineManager::morphHasPublisher(const ofxOceanodeTimelineSequence& sequence) const {
    TimelineRef ref;
    if(isEngine() || !refOf(sequence, ref)) return false;
    const auto found = morphPublishers.find(normalizedRef(ref));
    // (A node notes it on its update, after this frame's pass or the one before.)
    return found != morphPublishers.end() && morphFrame - found->second <= 1;
}

// ---- The Timeline nodes ----

void ofxOceanodeTimelineManager::noteMorphPublisher(const ofxOceanodeTimelineSequence& sequence) {
    TimelineRef ref;
    if(isEngine() || !refOf(sequence, ref)) return;
    morphPublishers[normalizedRef(ref)] = morphFrame;
}

const ofxOceanodeTimelineManager::MorphValues* ofxOceanodeTimelineManager::morphOutputs(
        const ofxOceanodeTimelineSequence& sequence) const {
    const auto* engine = findMorphEngine(sequence);
    return engine != nullptr ? &engine->outputs : nullptr;
}

void ofxOceanodeTimelineManager::drawMorphEngine(const ofxOceanodeTimelineSequence& sequence) {
    auto* engine = findMorphEngine(sequence);
    if(engine == nullptr || engine->runtime == nullptr || engine->drawnFrame == morphFrame) return;
    engine->drawnFrame = morphFrame;
    engine->runtime->draw();
    // Texture routers are rendered there; a Trigger was consumed by the
    // update's read and reads as none now.
    engine->outputs = MacroMixerPorts::takeOutputs(*engine->runtime);
}

// ---- Every frame ----

bool ofxOceanodeTimelineManager::morphPlayheadBeat(const TimelineRef& ref, double& beat) {
    if(clockSourceOf(ref) != ClockSource::Dependent) return timelinePlayheadBeat(ref, beat);
    // Placed as clips in another timeline: its own transport while that
    // previews it alone, else where a clip places it now. Placed nowhere at
    // the playhead, nothing of it plays.
    if(const auto transport = transportFor(ref)) {
        double at = 0.0;
        bool starts = false;
        if(transport->getState().isPlaying || transport->nextPendingAction(at, starts)) {
            const auto* engine = engineFor(ref);
            beat = engine != nullptr ? engine->playState().beatPosition : transport->getState().beatPosition;
            return true;
        }
    }
    return placedPlayheadBeat(ref, beat);
}

void ofxOceanodeTimelineManager::presentMorphScene(const TimelineRef& ref, const ofxOceanodeTimelineSequence& sequence,
                                                   const ofxOceanodeTimelineTrack& track, MorphEngine& engine) {
    // What should be live at the playhead: no clip (a gap, a muted clip, a
    // silent track), one (its source alone), or two in transition, the later
    // clip's morphIn saying how. Playing, stopped or scrubbing alike.
    MacroMixerScene scene;
    double beat = 0.0;
    engine.progress = 0.0f;
    if(engine.problem.empty() && isTrackAudible(track) && morphPlayheadBeat(ref, beat)) {
        const auto at = morphSceneAt(track, beat);
        const auto* origin = at.originClipId.empty() ? nullptr : sequence.findClip(track.id, at.originClipId);
        const auto* target = at.targetClipId.empty() ? nullptr : sequence.findClip(track.id, at.targetClipId);
        // (No chance on this track's clips: a value left in a file is not rolled.)
        const auto silent = [&](const ofxOceanodeTimelineClip* clip) {
            return clip == nullptr || clip->muted || clip->morphSource.empty();
        };
        // A silent clip plays nothing: the other one of an overlap is alone.
        if(silent(origin)) { origin = silent(target) ? nullptr : target; target = nullptr; }
        else if(silent(target)) target = nullptr;
        if(origin != nullptr) {
            scene.origin = origin->morphSource;
            if(target != nullptr) {
                const auto& transition = target->morphIn;
                scene.target = target->morphSource;
                scene.progress = MacroMixerRuntime::shape(at.position, transition.biPow);
                engine.progress = scene.progress;
                scene.morph = transition.morph;
                scene.inPlace = transition.morph && transition.inPlace;
                scene.logBlend = transition.logBlend;
                if(!engine.transitionKnown || engine.transitionClip != target->id || engine.transitionStamp != transition.stamp) {
                    readTransition(transition, engine.transitionPolicies, engine.transitionOverrides);
                    engine.transitionKnown = true;
                    engine.transitionClip = target->id;
                    engine.transitionStamp = transition.stamp;
                }
                scene.morphPolicies = engine.transitionPolicies;
                scene.routerOverrides = engine.transitionOverrides;
            } else if(track.morphPreloadSeconds > 0.0f) {
                // Alone, and another source's clip starts soon: that source
                // loads now, silent (the target of a Mix held at 0), so the
                // frame its clip starts on does not wait for it. Not for a
                // Morph in place, which needs the target not to be loaded.
                const auto clock = transportFor(ref);
                const double bpm = clock != nullptr ? std::max(1.0, static_cast<double>(clock->getState().bpm)) : 120.0;
                const double lead = track.morphPreloadSeconds * bpm / 60.0;
                const ofxOceanodeTimelineClip* upcoming = nullptr;
                for(const auto& clip : track.clips) {
                    if(clip.startBeat <= beat || clip.startBeat - beat > lead || silent(&clip) ||
                       clip.morphSource == origin->morphSource) continue;
                    if(upcoming == nullptr || clip.startBeat < upcoming->startBeat) upcoming = &clip;
                }
                if(upcoming != nullptr && !(upcoming->morphIn.morph && upcoming->morphIn.inPlace)) {
                    scene.target = upcoming->morphSource;
                    scene.progress = 0.0f;
                    scene.morph = false;
                }
            }
        }
    }
    const auto transport = transportFor(ref);
    engine.idle = scene.origin.empty() && (transport == nullptr || !transport->getState().isPlaying);
    engine.runtime->present(scene);
    // Two sources live (a transition, or one loaded ahead): no source's patch
    // stays open. In place the one graph is neither source's; on two graphs
    // the origin's goes when the transition ends.
    if(engine.runtime->transitioning())
        for(const MacroMixerRuntime::Graph* graph : {engine.runtime->origin(), engine.runtime->destination()})
            if(graph != nullptr && graph->macro != nullptr && graph->macro->isWindowVisible())
                graph->macro->setWindowVisible(false, false);
    noteMorphGraphs(engine);
}

void ofxOceanodeTimelineManager::presentMorphScenes() {
    if(isEngine() || container == nullptr) return;
    ++morphFrame;
    for(auto it = morphPublishers.begin(); it != morphPublishers.end();)
        it = morphFrame - it->second > 1 ? morphPublishers.erase(it) : std::next(it);
    // An engine whose macro is gone goes first, while the canvas it holds is
    // still there (the macro's timelines go with it, below).
    {
        std::vector<TimelineRef> gone;
        for(const auto& entry : morphEngines) {
            const auto& engine = *entry.second;
            if(engine.parent != container && (engine.keepAlive == nullptr || !macroCanvasAlive(*engine.parent)))
                gone.push_back(entry.first);
        }
        for(const auto& ref : gone) releaseMorphEngine(ref);
    }
    // The timelines with a Macro Morphing Track.
    std::vector<TimelineRef> hosts;
    for(const auto& listed : allTimelines()) {
        const auto ref = normalizedRef(listed);
        const auto* sequence = findTimeline(ref);
        if(sequence != nullptr && morphTrackOf(*sequence) != nullptr) hosts.push_back(ref);
    }
    // (No Macro Morphing Track anywhere: no source, and none of their documents.)
    if(hosts.empty() && !morphDocuments.empty()) { morphDocuments.clear(); morphDocumentKeys.clear(); }
    // No track, or no timeline, any more.
    {
        std::vector<TimelineRef> gone;
        for(const auto& entry : morphEngines)
            if(std::find(hosts.begin(), hosts.end(), entry.first) == hosts.end()) gone.push_back(entry.first);
        for(const auto& ref : gone) releaseMorphEngine(ref);
    }
    for(const auto& ref : hosts) {
        // (Found again: an engine before this one may have released a source
        // whose timelines were listed.)
        const auto* sequence = findTimeline(ref);
        const auto* track = sequence != nullptr ? morphTrackOf(*sequence) : nullptr;
        if(track == nullptr) continue;
        auto* engine = ensureMorphEngine(ref, *track, true);
        if(engine == nullptr || engine->runtime == nullptr) continue;
        // Read again by every Timeline node after this frame's update.
        engine->outputs.clear();
        applyMorphAnswers(ref, *engine);
        // (The answers may have added a clip: the track is found again.)
        sequence = findTimeline(ref);
        track = sequence != nullptr ? morphTrackOf(*sequence) : nullptr;
        if(track == nullptr || engine->runtime == nullptr) continue;
        presentMorphScene(ref, *sequence, *track, *engine);
    }
    // Graphs may have come and gone since this pass's lookups began.
    parameterCache.clear();
}

bool ofxOceanodeTimelineManager::updateMorphEngineNow(MorphEngine& engine) {
    if(engine.runtime == nullptr || engine.updatedFrame == morphFrame) return false;
    engine.updatedFrame = morphFrame;
    auto& runtime = *engine.runtime;
    // Where the MacroMixer node updates its own: the timelines of this
    // frame have written (the sources' included). The inputs are the ones
    // the Timeline nodes last wrote.
    runtime.update();
    noteMorphGraphs(engine);
    // Read once: a Trigger is consumed here and handed to every node.
    engine.outputs = MacroMixerPorts::takeOutputs(runtime);
    // A source check loads a graph (a stalled frame): only one asked
    // for (Check Status), while nothing plays and the timeline is stopped.
    if(engine.checksWanted && engine.idle) {
        if(runtime.originsToCheck() == 0) engine.checksWanted = false;
        else runtime.checkNextOrigin();
    }
    return true;
}

void ofxOceanodeTimelineManager::updateMorphEngines() {
    if(isEngine() || morphEngines.empty()) return;
    // (Held: a graph's update may reach back into the list.)
    std::vector<std::shared_ptr<MorphEngine>> engines;
    for(const auto& entry : morphEngines) {
        // An engine a Timeline node publishes is updated by that node, on
        // its own update (updateMorphEngine): its inputs are written by then.
        const auto publisher = morphPublishers.find(entry.first);
        if(publisher != morphPublishers.end() && morphFrame - publisher->second <= 1) continue;
        engines.push_back(entry.second);
    }
    bool updated = false;
    for(const auto& engine : engines)
        if(engine != nullptr && updateMorphEngineNow(*engine)) updated = true;
    // The sources' nodes updated in there, and may have made or deleted nodes
    // (a nested macro loading a preset as its timeline is scrubbed) without
    // any graph changing: no parameter looked up before is trusted by the
    // applyAutomation that follows. (An engine a Timeline node publishes
    // updates on that node's update, outside this pass: nothing to drop then.)
    if(updated) parameterCache.clear();
}

void ofxOceanodeTimelineManager::updateMorphEngine(const ofxOceanodeTimelineSequence& sequence) {
    TimelineRef ref;
    if(isEngine() || morphEngines.empty() || !refOf(sequence, ref)) return;
    const auto found = morphEngines.find(normalizedRef(ref));
    if(found == morphEngines.end() || found->second == nullptr) return;
    // (Held: a graph's update may reach back into the list.)
    const auto engine = found->second;
    updateMorphEngineNow(*engine);
}

void ofxOceanodeTimelineManager::forEachMorphGraph(const std::function<void(ofxOceanodeContainer&)>& visit) {
    if(isEngine() || !visit) return;
    for(const auto& entry : morphEngines) {
        if(entry.second == nullptr || entry.second->runtime == nullptr) continue;
        const auto* origin = entry.second->runtime->origin();
        const auto* destination = entry.second->runtime->destination();
        if(origin != nullptr && origin->container != nullptr) visit(*origin->container);
        // (In place, the one graph is both.)
        if(destination != nullptr && destination != origin && destination->container != nullptr) visit(*destination->container);
    }
}

void ofxOceanodeTimelineManager::drawMorphEngines() {
    if(isEngine() || morphEngines.empty()) return;
    std::vector<std::shared_ptr<MorphEngine>> engines;
    for(const auto& entry : morphEngines) engines.push_back(entry.second);
    for(const auto& engine : engines) {
        if(engine == nullptr || engine->runtime == nullptr || engine->drawnFrame == morphFrame) continue;
        engine->drawnFrame = morphFrame;
        engine->runtime->draw();
        // Texture routers are rendered there (see drawMorphEngine).
        engine->outputs = MacroMixerPorts::takeOutputs(*engine->runtime);
    }
}

void ofxOceanodeTimelineManager::reportMorphError(const std::string& message) {
    if(message.empty()) return;
    ofLogError("ofxOceanodeTimeline") << "Macro Morphing Track: " << message;
    morphError = message;
    morphErrorAt = ofGetElapsedTimef();
}

std::string ofxOceanodeTimelineManager::takeMorphError() {
    std::string message;
    message.swap(morphError);
    // (One no editor was open to show: not brought up long after.)
    if(ofGetElapsedTimef() - morphErrorAt > 10.0f) message.clear();
    return message;
}
