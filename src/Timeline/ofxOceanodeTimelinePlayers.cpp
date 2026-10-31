// Independent players: timelines that play on their own transport, each
// through an engine (a manager of its own evaluating an absolute-path copy),
// and the canvases' further timelines.

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"

#include <algorithm>

namespace {
bool isRootRef(const ofxOceanodeTimelineManager::TimelineRef& ref) {
    return (ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0") && ref.timeline.empty();
}

size_t canvasDepth(const std::string& canvas) {
    size_t depth = canvas.empty() ? 0 : 1;
    for(size_t at = canvas.find(" / "); at != std::string::npos; at = canvas.find(" / ", at + 3)) ++depth;
    return depth;
}
} // namespace

bool ofxOceanodeTimelinePlaySettings::operator==(const ofxOceanodeTimelinePlaySettings& other) const {
    auto samePoints = [](const auto& a, const auto& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
            return x.beat == y.beat && x.value == y.value;
        });
    };
    auto sameTensions = [](const auto& a, const auto& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
            return x.inflection == y.inflection && x.steepness == y.steepness;
        });
    };
    return name == other.name && bpm == other.bpm && followClockTempo == other.followClockTempo &&
        tempoRatio == other.tempoRatio && bpmAutomationEnabled == other.bpmAutomationEnabled &&
        bpmMinimum == other.bpmMinimum && bpmMaximum == other.bpmMaximum &&
        bpmInterpolation == other.bpmInterpolation && samePoints(bpmPoints, other.bpmPoints) &&
        sameTensions(bpmTensions, other.bpmTensions) && loopEnabled == other.loopEnabled &&
        loopStartBeat == other.loopStartBeat && loopEndBeat == other.loopEndBeat &&
        timeSignatureNumerator == other.timeSignatureNumerator &&
        timeSignatureDenominator == other.timeSignatureDenominator && quantize == other.quantize;
}

// ---- The transport a manager plays ----

std::shared_ptr<ofxOceanodeTransport> ofxOceanodeTimelineManager::playTransport() const {
    if(engineTransport != nullptr) return engineTransport;
    return container != nullptr ? container->getRootTransport() : nullptr;
}

ofxOceanodeTransportState ofxOceanodeTimelineManager::playState() const {
    const auto transport = playTransport();
    return transport != nullptr ? transport->getState() : ofxOceanodeTransportState{};
}

void ofxOceanodeTimelineManager::adoptPlaySettings(const ofxOceanodeTimelinePlaySettings& settings) {
    if(bpmAutomationEnabled != settings.bpmAutomationEnabled) setBpmAutomationEnabled(settings.bpmAutomationEnabled);
    if(bpmMinimum != settings.bpmMinimum || bpmMaximum != settings.bpmMaximum)
        setBpmRange(settings.bpmMinimum, settings.bpmMaximum);
    if(bpmInterpolation != settings.bpmInterpolation) setBpmInterpolation(settings.bpmInterpolation);
    bpmAutomationPoints = settings.bpmPoints;
    bpmCurveTensions = settings.bpmTensions;
    bpmCurveTensions.resize(bpmAutomationPoints.empty() ? 0 : bpmAutomationPoints.size() - 1);
    if(loopEnabled != settings.loopEnabled) setLoopEnabled(settings.loopEnabled);
    if(loopStartBeat != settings.loopStartBeat || loopEndBeat != settings.loopEndBeat)
        setLoopRange(settings.loopStartBeat, settings.loopEndBeat);
    if(timeSignatureNumerator != settings.timeSignatureNumerator ||
       timeSignatureDenominator != settings.timeSignatureDenominator)
        setTimeSignature(settings.timeSignatureNumerator, settings.timeSignatureDenominator);
    engineBpm = std::max(1.0f, settings.bpm);
    engineFollowsClockTempo = settings.followClockTempo;
    engineTempoRatio = std::max(0.01f, settings.tempoRatio);
}

void ofxOceanodeTimelineManager::makeAbsolute(ofxOceanodeTimelineSequence& sequence, const std::string& canvas) {
    if(canvas.empty() || canvas == "Canvas" || canvas == "0") return;
    // "Node/Param" is in the canvas itself, "Macro 2::Node/Param" further in.
    auto absolute = [&](const std::string& path) {
        return path.find("::") == std::string::npos ? canvas + "::" + path : canvas + " / " + path;
    };
    for(auto& track : sequence.tracks) {
        for(auto& binding : track.bindings) binding.parameterPath = absolute(binding.parameterPath);
        if(track.isMacroTrack()) track.macroCanvas = canvas + " / " + track.macroCanvas;
        for(auto& clip : track.clips)
            for(auto& lane : clip.lanes) {
                if(lane.targetProbability.empty()) continue;
                std::decay_t<decltype(lane.targetProbability)> moved;
                for(const auto& [path, chance] : lane.targetProbability) moved[absolute(path)] = chance;
                lane.targetProbability = std::move(moved);
            }
    }
}

// ---- Timelines by reference ----

std::string ofxOceanodeTimelineManager::timeSourceId(const TimelineRef& ref) {
    const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
    return (root ? std::string("root") : ref.canvas) + "#" + (ref.timeline.empty() ? std::string("default") : ref.timeline);
}

std::vector<ofxOceanodeTimelineManager::TimelineRef> ofxOceanodeTimelineManager::timelinesOfCanvas(const std::string& canvas) {
    std::vector<TimelineRef> refs;
    auto* base = timelineFor({canvas, std::string()});
    if(base == nullptr) return refs;
    refs.push_back({canvas, std::string()});
    for(const auto& timeline : base->timelines) refs.push_back({canvas, timeline->timelineId});
    return refs;
}

std::string ofxOceanodeTimelineManager::createTimeline(const std::string& canvas, const std::string& name) {
    auto* base = timelineFor({canvas, std::string()});
    if(base == nullptr) return std::string();
    std::string id;
    for(uint64_t number = base->timelines.size() + 2; ; ++number) {
        id = "tl_" + std::to_string(number);
        if(std::none_of(base->timelines.begin(), base->timelines.end(),
                        [&](const auto& timeline) { return timeline->timelineId == id; })) break;
    }
    auto timeline = std::make_shared<ofxOceanodeTimelineSequence>();
    timeline->base = base->base;
    timeline->timelineId = id;
    timeline->play.name = name.empty() ? "Timeline " + id.substr(3) : name;
    timeline->play.timeSignatureNumerator = timeSignatureNumerator;
    timeline->play.timeSignatureDenominator = timeSignatureDenominator;
    base->timelines.push_back(std::move(timeline));
    return id;
}

bool ofxOceanodeTimelineManager::removeTimeline(const TimelineRef& ref) {
    if(ref.timeline.empty()) return false;
    auto* base = timelineFor({ref.canvas, std::string()});
    if(base == nullptr) return false;
    setFree(ref, false);
    const auto before = base->timelines.size();
    base->timelines.erase(std::remove_if(base->timelines.begin(), base->timelines.end(),
        [&](const auto& timeline) { return timeline->timelineId == ref.timeline; }), base->timelines.end());
    invalidateEvaluationIndexes();
    return base->timelines.size() != before;
}

bool ofxOceanodeTimelineManager::renameTimeline(const TimelineRef& ref, const std::string& name) {
    auto* sequence = timelineFor(ref);
    if(sequence == nullptr) return false;
    sequence->play.name = name;
    return true;
}

std::string ofxOceanodeTimelineManager::duplicateTimeline(const TimelineRef& ref, const std::string& name) {
    auto* source = timelineFor(ref);
    if(source == nullptr) return std::string();
    const auto copy = *source; // before createTimeline may move the canvas's list
    const auto id = createTimeline(ref.canvas, name.empty() ? timelineName(ref) + " copy" : name);
    auto* duplicate = timelineFor({ref.canvas, id});
    if(duplicate == nullptr) return std::string();
    const auto play = duplicate->play;
    *duplicate = copy;
    duplicate->timelines.clear();
    duplicate->timelineId = id;
    duplicate->play.name = play.name;
    return id;
}

// ---- Free players ----

bool ofxOceanodeTimelineManager::isFree(const TimelineRef& ref) const {
    return isRootRef(ref) || freeTimelines.count(ref) > 0;
}

void ofxOceanodeTimelineManager::setFree(const TimelineRef& ref, bool free) {
    if(isRootRef(ref)) return;
    if(free) {
        freeTimelines.insert(ref);
        return;
    }
    freeTimelines.erase(ref);
    for(auto it = players.begin(); it != players.end(); ++it)
        if((*it)->ref == ref) {
            destroyPlayer(**it);
            players.erase(it);
            break;
        }
}

ofxOceanodeTimelineManager::Player* ofxOceanodeTimelineManager::playerFor(const TimelineRef& ref) const {
    for(const auto& player : players) if(player->ref == ref) return player.get();
    return nullptr;
}

std::shared_ptr<ofxOceanodeTransport> ofxOceanodeTimelineManager::transportFor(const TimelineRef& ref) const {
    if(isRootRef(ref)) return container != nullptr ? container->getRootTransport() : nullptr;
    if(auto* player = playerFor(ref)) return player->transport;
    return nullptr;
}

namespace {
// The next instant on the root timeline's grid while it plays, else the
// clock's, at least `lead` seconds ahead.
double launchInstant(const std::shared_ptr<ofxOceanodeTransport>& transport, int quantize, double lead) {
    auto* root = transport->getRoot();
    const double now = transport->clockSecondsAt(ofxOceanodeTransport::steadyNowUs());
    if(quantize <= 0) return now + lead;
    std::shared_ptr<ofxOceanodeTransport> reference = root->getState().isPlaying
        ? std::shared_ptr<ofxOceanodeTransport>(transport, root) : transport->getClockTransport();
    if(reference == nullptr) return now + lead;
    const double grid = quantize == 1 ? 1.0 : std::max(0.25, reference->getState().beatsPerBar);
    return reference->nextGridClockSeconds(grid, lead);
}
} // namespace

void ofxOceanodeTimelineManager::playTimeline(const TimelineRef& ref, bool quantized) {
    auto transport = transportFor(ref);
    if(transport == nullptr) return;
    if(isRootRef(ref)) { transport->play(); return; }
    if(transport->getState().isPlaying) return;
    const auto* sequence = timelineFor(ref);
    const int quantize = quantized && sequence != nullptr ? sequence->play.quantize : 0;
    transport->cancelPendingActions();
    // Far enough ahead for the scheduler to hand its first events over in time.
    const double lead = std::min(0.25, std::max(0.02, effectiveSchedulingLookaheadMs() / 2000.0));
    transport->playAt(launchInstant(transport, quantize, lead));
}

void ofxOceanodeTimelineManager::stopTimeline(const TimelineRef& ref, bool rewind, bool quantized) {
    auto transport = transportFor(ref);
    if(transport == nullptr) return;
    if(isRootRef(ref)) { if(rewind) transport->stop(); else transport->pause(); return; }
    transport->cancelPendingActions();
    const auto* sequence = timelineFor(ref);
    const int quantize = quantized && sequence != nullptr ? sequence->play.quantize : 0;
    if(!transport->getState().isPlaying || quantize <= 0) {
        if(rewind) transport->stop(); else transport->pause();
        return;
    }
    transport->stopAt(launchInstant(transport, quantize, 0.0), rewind);
}

void ofxOceanodeTimelineManager::seekTimeline(const TimelineRef& ref, double beat) {
    if(auto transport = transportFor(ref)) transport->seekToBeat(std::max(0.0, beat));
}

const ofxOceanodeTimelinePlaySettings* ofxOceanodeTimelineManager::playSettings(const TimelineRef& ref) {
    if(isRootRef(ref)) return nullptr;
    const auto* sequence = timelineFor(ref);
    return sequence != nullptr ? &sequence->play : nullptr;
}

void ofxOceanodeTimelineManager::setPlaySettings(const TimelineRef& ref, const ofxOceanodeTimelinePlaySettings& settings) {
    if(isRootRef(ref)) return;
    if(auto* sequence = timelineFor(ref)) {
        sequence->play = settings;
        auto& play = sequence->play;
        play.bpm = ofClamp(play.bpm, 1.0f, 999.0f);
        play.tempoRatio = ofClamp(play.tempoRatio, 0.01f, 64.0f);
        play.quantize = ofClamp(play.quantize, 0, 2);
        play.loopStartBeat = std::max(0.0, play.loopStartBeat);
        play.loopEndBeat = std::max(play.loopStartBeat + 1.0 / 24.0, play.loopEndBeat);
    }
}

void ofxOceanodeTimelineManager::destroyPlayer(Player& player) {
    if(player.overridesCanvas)
        if(auto canvas = player.canvas.lock())
            if(canvas->getTransportOverride() == player.transport) canvas->setTransportOverride(nullptr);
    if(player.engine != nullptr) {
        // Correct whatever its backends were handed ahead.
        const auto state = player.engine->playState();
        player.engine->sendScheduleCorrections(state, ofxOceanodeScheduling::steadyNowUs());
        player.engine->updatePianoGateOwnership(false);
    }
    if(player.transport != nullptr) {
        player.transport->cancelPendingActions();
        player.transport->pause();
    }
}

void ofxOceanodeTimelineManager::updatePlayers() {
    if(isEngine() || container == nullptr) return;
    auto root = container->getRootTransport();
    if(root == nullptr) return;
    // Gone, or no longer free.
    for(auto it = players.begin(); it != players.end();) {
        auto& player = **it;
        const bool canvasGone = !player.ref.canvas.empty() && player.canvas.expired();
        if(canvasGone || freeTimelines.count(player.ref) == 0 || timelineFor(player.ref) == nullptr) {
            destroyPlayer(player);
            it = players.erase(it);
        } else ++it;
    }
    // New ones.
    for(const auto& ref : freeTimelines) {
        if(isRootRef(ref) || playerFor(ref) != nullptr) continue;
        auto* sequence = timelineFor(ref);
        auto* canvas = canvasContainer(ref.canvas);
        if(sequence == nullptr || canvas == nullptr) continue;
        auto player = std::make_unique<Player>();
        player->ref = ref;
        player->transport = ofxOceanodeTransport::makeDerived(root);
        player->transport->setBpm(sequence->play.bpm);
        player->engine = std::make_unique<ofxOceanodeTimelineManager>(container);
        player->engine->primary = this;
        player->engine->engineTransport = player->transport;
        player->engine->schedulingEnabled = schedulingEnabled;
        player->engine->schedulingLookaheadMs = schedulingLookaheadMs;
        player->engine->schedulingLookaheadAdaptive = schedulingLookaheadAdaptive;
        if(canvas != container) {
            for(const auto& entry : macroSequences)
                if(entry.address == canvas) player->canvas = entry.container;
            // A macro's own timeline playing free is what its canvas plays to.
            if(ref.timeline.empty() && canvas->getTransportOverride() == nullptr) {
                canvas->setTransportOverride(player->transport);
                player->overridesCanvas = true;
            }
        }
        players.push_back(std::move(player));
    }
    if(players.empty()) return;
    // Inner canvases first: an outer timeline's value lands last.
    std::stable_sort(players.begin(), players.end(), [](const auto& a, const auto& b) {
        return canvasDepth(a->ref.canvas) > canvasDepth(b->ref.canvas);
    });
    ensureEvaluationIndexes();
    for(auto& player : players) {
        auto* sequence = timelineFor(player->ref);
        if(sequence == nullptr) continue;
        auto& engine = *player->engine;
        // The copy follows its source: rebuilt when its content changed.
        const uint64_t signature = sequenceSignature(*sequence);
        if(player->source != sequence || player->sourceSignature != signature) {
            engine.mainSequence = *sequence;
            engine.mainSequence.timelines.clear();
            engine.mainSequence.base = nullptr;
            makeAbsolute(engine.mainSequence, isRootRef({player->ref.canvas, std::string()}) ? std::string() : player->ref.canvas);
            engine.invalidateEvaluationIndexes();
            engine.invalidateSchedule();
            player->source = sequence;
            player->sourceSignature = signature;
        }
        engine.adoptPlaySettings(sequence->play);
        engine.retriggerGapBeats = retriggerGapBeats;
        // What the main timeline drives stays on the engine's frame path.
        engine.pathDrivenElsewhere = [this](const std::string& path) { return bindingsByPathCache.count(path) > 0; };
        engine.beginParameterCache();
        engine.evaluateAutomation();
        engine.applyAutomation();
        engine.endParameterCache();
    }
}

void ofxOceanodeTimelineManager::updateTimeSources() {
    if(isEngine() || container == nullptr) return;
    auto root = container->getRootTransport();
    if(root == nullptr) return;
    // Names come from the macro nodes (a walk of the graph): refresh a few
    // times a second, and at once when the players change.
    const uint64_t frame = ofGetFrameNum();
    const size_t shape = players.size() * 7919 + freeTimelines.size() * 104729 + mainSequence.timelines.size();
    if(shape == timeSourcesShape && frame < timeSourcesFrame + 15 && frame >= timeSourcesFrame) return;
    timeSourcesShape = shape;
    timeSourcesFrame = frame;
    std::set<std::string> present;
    auto add = [&](const TimelineRef& ref, const std::string& group) {
        const auto id = timeSourceId(ref);
        auto transport = transportFor(ref);
        ofxOceanodeTransport::TimeSourceInfo info;
        info.id = id;
        info.label = timelineName(ref);
        info.group = group;
        info.selectable = transport != nullptr;
        if(!info.selectable) info.note = "Not playing on its own: it only plays where clips place it.";
        root->registerTimeSource(info, transport);
        present.insert(id);
    };
    const std::string rootGroup = container->getCanvasID();
    add({std::string(), std::string()}, rootGroup);
    for(const auto& timeline : mainSequence.timelines) add({std::string(), timeline->timelineId}, rootGroup);
    pruneMacroSequences();
    for(const auto& entry : macroSequences) {
        const std::string canvas = entry.address->getCanvasID();
        add({canvas, std::string()}, canvas);
        for(const auto& timeline : entry.sequence->timelines) add({canvas, timeline->timelineId}, canvas);
    }
    for(const auto& id : registeredTimeSources)
        if(present.count(id) == 0) root->unregisterTimeSource(id);
    registeredTimeSources = std::move(present);
}
