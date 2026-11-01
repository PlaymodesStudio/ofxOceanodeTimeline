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
        timeSignatureDenominator == other.timeSignatureDenominator && quantize == other.quantize &&
        playOnLoad == other.playOnLoad;
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

// "Node/Param" is in the canvas itself, "Macro 2::Node/Param" further in.
std::string ofxOceanodeTimelineManager::absolutePathIn(const std::string& canvas, const std::string& path) {
    if(canvas.empty() || canvas == "Canvas" || canvas == "0") return path;
    return path.find("::") == std::string::npos ? canvas + "::" + path : canvas + " / " + path;
}

void ofxOceanodeTimelineManager::makeAbsolute(ofxOceanodeTimelineSequence& sequence, const std::string& canvas) {
    if(canvas.empty() || canvas == "Canvas" || canvas == "0") return;
    auto absolute = [&](const std::string& path) { return absolutePathIn(canvas, path); };
    for(auto& track : sequence.tracks) {
        for(auto& binding : track.bindings) binding.parameterPath = absolute(binding.parameterPath);
        if(track.isMacroTrack()) track.macroCanvas = track.macroCanvas == "." ? canvas : canvas + " / " + track.macroCanvas;
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

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::findTimeline(const TimelineRef& ref) {
    ofxOceanodeTimelineSequence* base = nullptr;
    if(ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0") base = &mainSequence;
    else if(auto* canvas = canvasContainer(ref.canvas)) {
        pruneMacroSequences();
        for(const auto& entry : macroSequences) if(entry.address == canvas) base = entry.sequence.get();
    }
    if(base == nullptr || ref.timeline.empty()) return base;
    for(auto& timeline : base->timelines) if(timeline->timelineId == ref.timeline) return timeline.get();
    return nullptr;
}

bool ofxOceanodeTimelineManager::timelineExists(const TimelineRef& ref) {
    if(isRootRef(ref)) return true;
    const auto* sequence = findTimeline(ref);
    if(sequence == nullptr) return false;
    if(!ref.timeline.empty()) return true;
    return !sequence->tracks.empty() || !sequence->timelines.empty() || isFree(ref) ||
           sequence->play != ofxOceanodeTimelinePlaySettings();
}

std::vector<ofxOceanodeTimelineManager::TimelineRef> ofxOceanodeTimelineManager::timelinesOfCanvas(const std::string& canvas) {
    std::vector<TimelineRef> refs;
    auto* base = findTimeline({canvas, std::string()});
    if(base == nullptr) return refs;
    if(timelineExists({canvas, std::string()})) refs.push_back({canvas, std::string()});
    for(const auto& timeline : base->timelines) refs.push_back({canvas, timeline->timelineId});
    return refs;
}

std::string ofxOceanodeTimelineManager::createTimeline(const std::string& canvas, const std::string& name) {
    auto* base = timelineFor({canvas, std::string()});
    if(base == nullptr) return std::string();
    std::string id;
    uint64_t number = std::max<uint64_t>(base->nextTimelineNumber, 2);
    for(;; ++number) {
        id = "tl_" + std::to_string(number);
        if(std::none_of(base->timelines.begin(), base->timelines.end(),
                        [&](const auto& timeline) { return timeline->timelineId == id; })) break;
    }
    base->nextTimelineNumber = number + 1;
    auto timeline = std::make_shared<ofxOceanodeTimelineSequence>();
    timeline->base = base->base;
    timeline->timelineId = id;
    timeline->play.name = name.empty() ? "Timeline " + id.substr(3) : name;
    timeline->play.timeSignatureNumerator = timeSignatureNumerator;
    timeline->play.timeSignatureDenominator = timeSignatureDenominator;
    base->timelines.push_back(std::move(timeline));
    return id;
}

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::addTimeline(const std::string& canvas, const std::string& name) {
    if(!timelineExists({canvas, std::string()})) {
        if(auto* own = timelineFor({canvas, std::string()})) {
            own->play.name = name;
            return {canvas, std::string()};
        }
        return {};
    }
    const auto id = createTimeline(canvas, name);
    return id.empty() ? TimelineRef{} : TimelineRef{canvas, id};
}

bool ofxOceanodeTimelineManager::removeTimeline(const TimelineRef& ref) {
    if(ref.timeline.empty()) return false;
    auto* base = timelineFor({ref.canvas, std::string()});
    if(base == nullptr) return false;
    setFree(ref, false);
    timelineWindows.erase(std::remove_if(timelineWindows.begin(), timelineWindows.end(),
        [&](const auto& window) { return normalizedRef(window.ref) == normalizedRef(ref); }), timelineWindows.end());
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
    if(isRootRef(ref)) {
        // The main timeline's tempo, loop and meter live on the manager.
        auto& own = duplicate->play;
        const auto root = container != nullptr ? container->getRootTransport() : nullptr;
        own.bpm = root != nullptr ? root->getState().bpm : 120.0f;
        own.bpmAutomationEnabled = bpmAutomationEnabled;
        own.bpmMinimum = bpmMinimum;
        own.bpmMaximum = bpmMaximum;
        own.bpmInterpolation = bpmInterpolation;
        own.bpmPoints = bpmAutomationPoints;
        own.bpmTensions = bpmCurveTensions;
        own.loopEnabled = loopEnabled;
        own.loopStartBeat = loopStartBeat;
        own.loopEndBeat = loopEndBeat;
        own.timeSignatureNumerator = timeSignatureNumerator;
        own.timeSignatureDenominator = timeSignatureDenominator;
    }
    return id;
}

// ---- Free players ----

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::normalizedRef(TimelineRef ref) {
    if(ref.canvas == "Canvas" || ref.canvas == "0") ref.canvas.clear();
    return ref;
}

bool ofxOceanodeTimelineManager::isFree(const TimelineRef& ref) const {
    return isRootRef(ref) || freeTimelines.count(normalizedRef(ref)) > 0;
}

void ofxOceanodeTimelineManager::setFree(const TimelineRef& given, bool free) {
    const auto ref = normalizedRef(given);
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
    if(transport->getState().isPlaying) {
        // Still playing towards a quantized stop: Play keeps it playing.
        double at = 0.0;
        bool starts = false;
        if(transport->nextPendingAction(at, starts) && !starts) transport->cancelPendingActions();
        return;
    }
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
        play.timeSignatureNumerator = ofClamp(play.timeSignatureNumerator, 1, 64);
        play.timeSignatureDenominator = std::max(1, play.timeSignatureDenominator);
        std::sort(play.bpmPoints.begin(), play.bpmPoints.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
        for(auto& point : play.bpmPoints) {
            point.beat = std::max(0.0, point.beat);
            point.value = ofClamp(point.value, play.bpmMinimum, play.bpmMaximum);
        }
        play.bpmTensions.resize(play.bpmPoints.empty() ? 0 : play.bpmPoints.size() - 1);
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
        player.engine->releaseWaveClips(player.engine->mainSequence, nullptr);
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
        player->engine->engineRef = ref; // it never places itself
        player->engine->engineTransport = player->transport;
        player->engine->schedulingEnabled = schedulingEnabled;
        player->engine->schedulingLookaheadMs = schedulingLookaheadMs;
        player->engine->schedulingLookaheadAdaptive = schedulingLookaheadAdaptive;
        player->engine->waveKeyPrefix = timeSourceId(ref) + "|";
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
    updatePlayOnLoad();
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
        // A macro's own timeline is what its canvas plays to: claim the
        // canvas once nothing else (a clip-following transport) holds it.
        if(player->ref.timeline.empty() && !player->overridesCanvas)
            if(auto canvas = player->canvas.lock(); canvas != nullptr && canvas->getTransportOverride() == nullptr) {
                canvas->setTransportOverride(player->transport);
                player->overridesCanvas = true;
            }
        // The copy follows its source: rebuilt when its content changed.
        const uint64_t signature = sequenceSignature(*sequence);
        engine.waveAudioProvider = waveAudioProvider; // Wave tracks play on it too, keyed apart
        if(player->source != sequence || player->sourceSignature != signature) {
            engine.releaseWaveClips(engine.mainSequence, sequence);
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
        // The toolbar's timestamping settings apply to every engine, live.
        engine.setSchedulingEnabled(schedulingEnabled);
        engine.setSchedulingLookaheadMs(schedulingLookaheadMs);
        engine.schedulingLookaheadAdaptive = schedulingLookaheadAdaptive;
        engine.retriggerGapBeats = retriggerGapBeats;
        // What the main timeline drives stays on the engine's frame path.
        engine.pathDrivenElsewhere = [this](const std::string& path) { return bindingsByPathCache.count(path) > 0; };
        engine.beginParameterCache();
        engine.evaluateAutomation();
        engine.applyAutomation();
        engine.endParameterCache();
    }
}

void ofxOceanodeTimelineManager::updatePlayOnLoad() {
    // Loaded but not playing on its own: nothing to start.
    for(auto it = playOnLoadPending.begin(); it != playOnLoadPending.end();)
        if(freeTimelines.count(normalizedRef(*it)) == 0) it = playOnLoadPending.erase(it);
        else ++it;
    for(auto& player : players) {
        const auto* sequence = timelineFor(player->ref);
        if(sequence == nullptr) continue;
        const bool loaded = playOnLoadPending.erase(player->ref) > 0;
        if(!sequence->play.playOnLoad) {
            player->startArmed = false;
            player->wasActive = -1;
            continue;
        }
        // Armed by a load or by its macro turning active; it starts (from its
        // beginning, at its quantize) only while its macros are active, and
        // stops when one turns inactive.
        const bool active = macrosActive(*player);
        const bool watched = player->wasActive >= 0;
        if(loaded || (watched && active && player->wasActive == 0)) player->startArmed = true;
        if(watched && !active && player->wasActive == 1) stopTimeline(player->ref, true, false);
        player->wasActive = active ? 1 : 0;
        if(player->startArmed && active) {
            player->startArmed = false;
            seekTimeline(player->ref, 0.0);
            playTimeline(player->ref);
        }
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
        if(timelineExists({canvas, std::string()})) add({canvas, std::string()}, canvas);
        for(const auto& timeline : entry.sequence->timelines) add({canvas, timeline->timelineId}, canvas);
    }
    for(const auto& id : registeredTimeSources)
        if(present.count(id) == 0) root->unregisterTimeSource(id);
    registeredTimeSources = std::move(present);
}

// ---- The timelines a timeline reaches through its timeline tracks ----

std::vector<ofxOceanodeTimelineManager::MacroSource> ofxOceanodeTimelineManager::collectMacroSources() const {
    std::vector<MacroSource> sources;
    if(container == nullptr) return sources;
    // An engine plays its primary's timelines.
    const auto* owner = primary != nullptr ? primary : this;
    owner->pruneMacroSequences();
    const auto& entries = owner->macroSequences;
    // A canvas's timeline as it is kept (the root's on the owner's main timeline).
    auto find = [&](const ofxOceanodeContainer* canvas, const std::string& timeline) -> const ofxOceanodeTimelineSequence* {
        const ofxOceanodeTimelineSequence* base = nullptr;
        if(canvas == container) base = &owner->mainSequence;
        else for(const auto& entry : entries) if(entry.address == canvas) base = entry.sequence.get();
        if(base == nullptr || timeline.empty()) return base;
        for(const auto& candidate : base->timelines) if(candidate->timelineId == timeline) return candidate.get();
        return nullptr;
    };
    const std::string rootKey = primary != nullptr ? timeSourceId(engineRef) : timeSourceId({});
    auto keyOf = [](const std::string& canvas, const std::string& timeline) {
        return timeSourceId({canvas, timeline});
    };
    constexpr size_t kMaximumSources = 64;
    std::vector<std::string> keys; // keyOf(source), per source
    // Breadth first: this timeline's timeline tracks, then theirs. A timeline
    // reached twice is one source, and none places one it is placed by.
    for(int parent = -1; parent < static_cast<int>(sources.size()) && sources.size() < kMaximumSources; ++parent) {
        const auto& sequence = parent < 0 ? mainSequence : *sources[static_cast<size_t>(parent)].sequence;
        const std::string baseCanvas = parent < 0 ? std::string() : sources[static_cast<size_t>(parent)].canvas;
        for(size_t trackIndex = 0; trackIndex < sequence.tracks.size(); ++trackIndex) {
            const auto& track = sequence.tracks[trackIndex];
            if(!track.isMacroTrack()) continue;
            const std::string canvas = track.macroCanvas == "."
                ? baseCanvas : baseCanvas.empty() ? track.macroCanvas : baseCanvas + " / " + track.macroCanvas;
            const std::string key = keyOf(canvas, track.macroTimeline);
            if(std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
            bool cycle = key == rootKey;
            for(int up = parent; !cycle && up >= 0; up = sources[static_cast<size_t>(up)].parent)
                cycle = keys[static_cast<size_t>(up)] == key;
            if(cycle) continue;
            const auto* canvasContainerFound = canvasContainer(canvas);
            if(canvasContainerFound == nullptr) continue;
            const auto* placed = find(canvasContainerFound, track.macroTimeline);
            if(placed == nullptr) continue; // no timeline yet: nothing to play
            sources.push_back({canvasContainerFound, canvas, track.macroTimeline, placed, parent, trackIndex});
            keys.push_back(key);
            if(sources.size() >= kMaximumSources) break;
        }
    }
    return sources;
}

// ---- Timeline tracks ----

namespace {
bool rootCanvas(const std::string& canvas) { return canvas.empty() || canvas == "Canvas" || canvas == "0"; }
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::placedTimeline(const ofxOceanodeTimelineSequence& host,
                                                                       const ofxOceanodeTimelineTrack& track) {
    if(!track.isMacroTrack()) return nullptr;
    TimelineRef hostRef;
    if(!refOf(host, hostRef)) return nullptr;
    const std::string canvas = track.macroCanvas == "." ? hostRef.canvas
        : rootCanvas(hostRef.canvas) ? track.macroCanvas : hostRef.canvas + " / " + track.macroCanvas;
    return timelineFor({canvas, track.macroTimeline});
}

bool ofxOceanodeTimelineManager::timelineReaches(const TimelineRef& from, const TimelineRef& target) {
    std::vector<TimelineRef> pending{from};
    std::set<TimelineRef> seen;
    auto normal = [](TimelineRef ref) { if(rootCanvas(ref.canvas)) ref.canvas.clear(); return ref; };
    const auto goal = normal(target);
    while(!pending.empty()) {
        const auto ref = normal(pending.back());
        pending.pop_back();
        if(ref == goal) return true;
        if(!seen.insert(ref).second || seen.size() > 256) continue;
        const auto* sequence = timelineFor(ref);
        if(sequence == nullptr) continue;
        for(const auto& track : sequence->tracks) {
            if(!track.isMacroTrack()) continue;
            const std::string canvas = track.macroCanvas == "." ? ref.canvas
                : ref.canvas.empty() ? track.macroCanvas : ref.canvas + " / " + track.macroCanvas;
            pending.push_back({canvas, track.macroTimeline});
        }
    }
    return false;
}

std::string ofxOceanodeTimelineManager::createTimelineTrack(ofxOceanodeTimelineSequence& host, const TimelineRef& placed) {
    TimelineRef hostRef;
    if(!refOf(host, hostRef) || timelineFor(placed) == nullptr) return std::string();
    if(rootCanvas(hostRef.canvas)) hostRef.canvas.clear();
    std::string relative;
    if(placed.canvas == hostRef.canvas || (rootCanvas(placed.canvas) && hostRef.canvas.empty())) relative = ".";
    else if(hostRef.canvas.empty()) relative = placed.canvas;
    else if(placed.canvas.rfind(hostRef.canvas + " / ", 0) == 0) relative = placed.canvas.substr(hostRef.canvas.size() + 3);
    else return std::string(); // a timeline outside the host's canvas
    if(timelineReaches(placed, hostRef)) return std::string(); // it would play itself
    const auto id = createTrack(host, timelineName(placed));
    if(auto* track = getTrack(host, id)) {
        track->macroCanvas = relative;
        track->macroTimeline = placed.timeline;
    }
    invalidateEvaluationIndexes();
    return id;
}

// ---- Playheads and windows ----

bool ofxOceanodeTimelineManager::timelinePlayheadBeat(const TimelineRef& ref, double& beat) {
    if(auto transport = transportFor(ref)) {
        beat = transport->getState().beatPosition;
        return true;
    }
    if(container == nullptr) return false;
    // Read from the GUI: the copies of the last update pass are enough.
    if(!evaluationIndexesValid) rebuildEvaluationIndexes();
    const double playhead = playState().beatPosition;
    std::string canvas = ref.canvas;
    if(canvas == "Canvas" || canvas == "0") canvas.clear();
    // As evaluation plays it: the first placement of its copy.
    for(const auto& placement : playedSequences(playContext, playhead, playhead))
        for(const auto& played : playedMacros)
            if(played.sequence.get() == placement.sequence && played.canvas == canvas && played.timeline == ref.timeline) {
                beat = placement.map.toSequence(playhead);
                if(played.reversed) beat = played.mirrorLength - beat; // back in the timeline as written
                return true;
            }
    return false;
}

int ofxOceanodeTimelineManager::openTimelineWindow(const TimelineRef& ref) {
    int id = 1;
    for(const auto& window : timelineWindows) id = std::max(id, window.id + 1);
    timelineWindows.push_back({id, normalizedRef(ref)});
    return id;
}

void ofxOceanodeTimelineManager::closeTimelineWindow(int id) {
    timelineWindows.erase(std::remove_if(timelineWindows.begin(), timelineWindows.end(),
                                         [&](const auto& window) { return window.id == id; }), timelineWindows.end());
}

void ofxOceanodeTimelineManager::setTimelineWindowRef(int id, const TimelineRef& ref) {
    for(auto& window : timelineWindows) if(window.id == id) window.ref = normalizedRef(ref);
}

// ---- Nodes inside a placed macro following its clips ----

void ofxOceanodeTimelineManager::setNodesFollowClips(const std::string& canvas, bool follow) {
    if(canvas.empty() || canvas == "Canvas" || canvas == "0") return;
    if(follow) clipFollowCanvases.insert(canvas);
    else clipFollowCanvases.erase(canvas);
}

void ofxOceanodeTimelineManager::releaseClipTransport(ClipTransport& entry) {
    if(auto canvas = entry.canvas.lock())
        if(canvas->getTransportOverride() == entry.transport) canvas->setTransportOverride(nullptr);
}

void ofxOceanodeTimelineManager::updateClipTransports(const ofxOceanodeTransportState& rootState) {
    for(auto it = clipTransports.begin(); it != clipTransports.end();) {
        const bool keep = clipFollowCanvases.count(it->first) > 0 && !it->second.canvas.expired() &&
                          !isFree({it->first, std::string()});
        if(keep) { ++it; continue; }
        releaseClipTransport(it->second);
        it = clipTransports.erase(it);
    }
    if(clipFollowCanvases.empty() || container == nullptr) return;
    auto root = container->getRootTransport();
    if(root == nullptr) return;
    std::vector<PlayedSequence> placements;
    bool placementsCollected = false;
    for(const auto& canvas : clipFollowCanvases) {
        if(isFree({canvas, std::string()})) continue; // its own transport wins
        auto* canvasHost = canvasContainer(canvas);
        if(canvasHost == nullptr || canvasHost == container) continue;
        const auto* own = timelineFor({canvas, std::string()}); // makes the macro's entry when it has none
        if(own == nullptr) continue;
        auto& entry = clipTransports[canvas];
        if(entry.transport == nullptr) {
            entry.transport = ofxOceanodeTransport::makeDerived(root);
            entry.address = canvasHost;
            for(const auto& sequenceEntry : macroSequences)
                if(sequenceEntry.address == canvasHost) entry.canvas = sequenceEntry.container;
            entry.transport->setDrivenState(0.0, rootState.bpm, false, true);
        }
        if(entry.canvas.expired()) continue;
        if(canvasHost->getTransportOverride() == nullptr) canvasHost->setTransportOverride(entry.transport);
        entry.transport->setBeatsPerBar(own->play.timeSignatureNumerator * (4.0 / std::max(1, own->play.timeSignatureDenominator)));
        if(!placementsCollected) {
            placements = playedSequences(playContext, rootState.beatPosition, rootState.beatPosition);
            placementsCollected = true;
        }
        const PlayedSequence* found = nullptr;
        const PlayedMacro* foundCopy = nullptr;
        for(const auto& placement : placements) {
            const auto* played = playedMacroOf(*placement.sequence);
            if(played != nullptr && played->canvas == canvas && played->timeline.empty()) {
                found = &placement;
                foundCopy = played;
                break;
            }
        }
        if(found == nullptr) {
            // No clip plays it here: it holds where it was, stopped.
            const auto held = entry.transport->getState();
            entry.transport->setDrivenState(held.beatPosition, held.bpm, false, false);
            entry.placement.clear();
            continue;
        }
        double beat = found->map.toSequence(rootState.beatPosition);
        if(foundCopy->reversed) beat = foundCopy->mirrorLength - beat; // as written
        const float bpm = static_cast<float>(rootState.bpm / std::max(1e-6, found->map.ratio));
        const std::string key = ofToString(reinterpret_cast<uintptr_t>(found->placedClip)) + "/" +
                                ofToString(found->instanceKey) + "/" + ofToString(found->passMix) + "/" +
                                ofToString(found->windowFrom);
        const bool restart = key != entry.placement || rootState.generation != entry.rootGeneration;
        entry.placement = key;
        entry.rootGeneration = rootState.generation;
        entry.transport->setDrivenState(beat, bpm, rootState.isPlaying, restart);
    }
}

void ofxOceanodeTimelineManager::releaseWaveClips(const ofxOceanodeTimelineSequence& previous,
                                                  const ofxOceanodeTimelineSequence* next) {
    if(waveAudioProvider == nullptr) return;
    for(const auto& track : previous.tracks) {
        if(!track.isWaveTrack) continue;
        const auto* kept = next == nullptr ? nullptr : next->findTrack(track.id);
        for(const auto& clip : track.clips)
            if(kept == nullptr || !kept->isWaveTrack || next->findClip(track.id, clip.id) == nullptr)
                waveAudioProvider->releaseWaveClip(waveKeyPrefix + track.id, clip.id);
    }
}

// ---- Own-tempo clips: how long their timeline lasts in host beats ----

namespace {
// A timeline's tempo at one of its beats, playing on its own tempo.
double playBpmAt(const ofxOceanodeTimelinePlaySettings& play, double beat, float clockBpm) {
    if(play.bpmAutomationEnabled && !play.bpmPoints.empty())
        return ofClamp(ofxOceanodeTimelineCurve::valueAtBeat(play.bpmPoints, play.bpmTensions, play.bpmInterpolation,
                                                            std::max(0.0, beat), play.bpm),
                       std::max(1.0f, play.bpmMinimum), std::max(1.0f, play.bpmMaximum));
    return std::max(1.0f, play.followClockTempo ? clockBpm * play.tempoRatio : play.bpm);
}
// Seconds `beats` of it last from its beat `from`, following its tempo lane.
double playSeconds(const ofxOceanodeTimelinePlaySettings& play, double from, double beats, float clockBpm) {
    if(!(play.bpmAutomationEnabled && !play.bpmPoints.empty()))
        return beats * 60.0 / playBpmAt(play, from, clockBpm);
    const int steps = std::max(1, std::min(4096, static_cast<int>(std::ceil(beats * 32.0))));
    const double step = beats / steps;
    double seconds = 0.0;
    for(int i = 0; i < steps; ++i) seconds += step * 60.0 / playBpmAt(play, from + (i + 0.5) * step, clockBpm);
    return seconds;
}
} // namespace

double ofxOceanodeTimelineManager::ownTempoFactorFor(const ofxOceanodeTimelineSequence& host,
                                                     const ofxOceanodeTimelineClip& clip,
                                                     const ofxOceanodeTimelineSequence& placed) const {
    const auto root = container != nullptr ? container->getRootTransport() : nullptr;
    const float clockBpm = std::max(1.0f, root != nullptr ? root->getState().bpm : 120.0f);
    const double content = ofxOceanodeTimelineClipTime::sourceDuration(clip);
    const double userStretch = ofxOceanodeTimelineClipTime::stretch(clip);
    // How long one pass of its content lasts at its own tempo (from where
    // the clip starts it), slowed by the clip's stretch...
    const double seconds = playSeconds(placed.play, ofxOceanodeTimelineClipTime::contentOffset(clip), content, clockBpm) * userStretch;
    // ...and how many host beats that is from the clip's start, along the
    // host's tempo: the main timeline's tempo map, or a timeline's own tempo.
    double hostBeats = 0.0;
    TimelineRef hostRef;
    const bool hostFree = &host != &mainSequence && refOf(host, hostRef) && isFree(hostRef);
    if(&host == &mainSequence || (!hostFree && !host.play.hasOwnTempo())) {
        if(bpmAutomationEnabled && !bpmAutomationPoints.empty())
            hostBeats = secondsToBeat(beatToSeconds(clip.startBeat, clockBpm) + seconds, clockBpm) - clip.startBeat;
        else hostBeats = seconds * clockBpm / 60.0;
    } else {
        // A timeline's own tempo lane: step along it until the time is spent.
        double beat = clip.startBeat, left = seconds;
        for(int guard = 0; left > 1e-12 && guard < 100000; ++guard) {
            const double step = 1.0 / 32.0;
            const double stepSeconds = step * 60.0 / playBpmAt(host.play, beat + 0.5 * step, clockBpm);
            if(stepSeconds >= left) { beat += step * left / stepSeconds; left = 0.0; }
            else { beat += step; left -= stepSeconds; }
        }
        hostBeats = beat - clip.startBeat;
    }
    return std::max(1e-6, hostBeats / std::max(1e-9, content * userStretch));
}

void ofxOceanodeTimelineManager::refreshOwnTempoFactors() {
    auto refresh = [&](ofxOceanodeTimelineSequence& host) {
        for(auto& track : host.tracks) {
            if(!track.isMacroTrack()) continue;
            const ofxOceanodeTimelineSequence* placed = nullptr;
            for(auto& clip : track.clips) {
                if(!clip.ownTempo || clip.reversed) continue;
                if(placed == nullptr) placed = placedTimeline(host, track);
                if(placed == nullptr) break;
                const double factor = ownTempoFactorFor(host, clip, *placed);
                if(std::abs(factor - clip.ownTempoFactor) <= 1e-9) continue;
                clip.ownTempoFactor = factor;
                patchOwnTempoFactor(host, track.id, clip.id, factor);
            }
        }
    };
    refresh(mainSequence);
    for(auto& timeline : mainSequence.timelines) refresh(*timeline);
    pruneMacroSequences();
    for(auto& entry : macroSequences) {
        refresh(*entry.sequence);
        for(auto& timeline : entry.sequence->timelines) refresh(*timeline);
    }
}

void ofxOceanodeTimelineManager::patchOwnTempoFactor(const ofxOceanodeTimelineSequence& host, const std::string& trackId,
                                                     const std::string& clipId, double factor) {
    auto patch = [&](ofxOceanodeTimelineSequence& copy) {
        if(auto* track = copy.findTrack(trackId))
            for(auto& clip : track->clips)
                if(clip.id == clipId && !clip.reversed) clip.ownTempoFactor = factor;
    };
    auto patchCopies = [&](std::vector<PlayedMacro>& copies, const TimelineRef& ref) {
        for(auto& played : copies)
            if(!played.reversed && played.canvas == ref.canvas && played.timeline == ref.timeline) patch(*played.sequence);
    };
    TimelineRef ref;
    if(!refOf(host, ref)) return;
    if(rootCanvas(ref.canvas)) ref.canvas.clear();
    // Its played copies here, its engine copy when it plays free, and its
    // copies played inside the engines.
    patchCopies(playedMacros, ref);
    for(auto& player : players) {
        if(player->engine == nullptr) continue;
        if(player->source == &host) patch(player->engine->mainSequence);
        patchCopies(player->engine->playedMacros, ref);
    }
}

bool ofxOceanodeTimelineManager::setClipOwnTempo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                 const std::string& clipId, bool ownTempo) {
    const auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    if(track == nullptr || clip == nullptr || !track->isMacroTrack() || clip->ownTempo == ownTempo) return false;
    const double before = clip->ownTempoScale();
    clip->ownTempo = ownTempo;
    if(ownTempo)
        if(const auto* placed = placedTimeline(sequence, *track)) clip->ownTempoFactor = ownTempoFactorFor(sequence, *clip, *placed);
    // As many repeats as before: its length scales with its tempo.
    if(clip->repeatContent) clip->durationBeats = std::max(1.0 / 24.0, clip->durationBeats * clip->ownTempoScale() / before);
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

// ---- Restoring a stretched timeline clip ----

bool ofxOceanodeTimelineManager::isClipStretched(const ofxOceanodeTimelineClip& clip) const {
    if(clip.repeatContent) return std::abs(clip.contentStretch - 1.0) > 1e-6;
    const double natural = ofxOceanodeTimelineClipTime::sourceDuration(clip) * clip.ownTempoScale();
    return std::abs(clip.durationBeats - natural) > 1e-6;
}

bool ofxOceanodeTimelineManager::restoreClipLength(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                   const std::string& clipId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr || !isClipStretched(*clip)) return false;
    if(clip->repeatContent) {
        // As many repeats, each at the timeline's own length.
        clip->durationBeats = std::max(1.0 / 24.0, clip->durationBeats / std::max(1.0 / 1024.0, clip->contentStretch));
        clip->contentStretch = 1.0;
    } else {
        clip->durationBeats = std::max(1.0 / 24.0, ofxOceanodeTimelineClipTime::sourceDuration(*clip) * clip->ownTempoScale());
        clip->contentStretch = std::max(1.0 / 1024.0, clip->durationBeats / ofxOceanodeTimelineClipTime::sourceDuration(*clip));
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}
