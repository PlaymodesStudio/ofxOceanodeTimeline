// Players: every timeline's transport and engine (a manager of its own
// evaluating an absolute-path copy), its clock source (linked to the global
// transport, free, or dependent on the timeline placing it), and the
// canvases' further timelines.

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"

#include <algorithm>

namespace {
size_t canvasDepth(const std::string& canvas) {
    size_t depth = canvas.empty() ? 0 : 1;
    for(size_t at = canvas.find(" / "); at != std::string::npos; at = canvas.find(" / ", at + 3)) ++depth;
    return depth;
}

bool rootCanvas(const std::string& canvas) { return canvas.empty() || canvas == "Canvas" || canvas == "0"; }
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
    return name == other.name && free == other.free && bpm == other.bpm && followClockTempo == other.followClockTempo &&
        tempoRatio == other.tempoRatio && bpmAutomationEnabled == other.bpmAutomationEnabled &&
        bpmMinimum == other.bpmMinimum && bpmMaximum == other.bpmMaximum &&
        bpmInterpolation == other.bpmInterpolation && samePoints(bpmPoints, other.bpmPoints) &&
        sameTensions(bpmTensions, other.bpmTensions) && loopEnabled == other.loopEnabled &&
        loopStartBeat == other.loopStartBeat && loopEndBeat == other.loopEndBeat &&
        timeSignatureNumerator == other.timeSignatureNumerator &&
        timeSignatureDenominator == other.timeSignatureDenominator && quantize == other.quantize &&
        playOnLoad == other.playOnLoad && customColor == other.customColor &&
        (!customColor || color == other.color) && nodesFollowClips == other.nodesFollowClips;
}

// ---- The transport a manager plays ----

std::shared_ptr<ofxOceanodeTransport> ofxOceanodeTimelineManager::playTransport() const {
    if(engineTransport != nullptr) return engineTransport;
    return container != nullptr ? container->getRootTransport() : nullptr;
}

ofxOceanodeTransportState ofxOceanodeTimelineManager::playState() const {
    const auto transport = playTransport();
    if(transport == nullptr) return ofxOceanodeTransportState{};
    // A derived transport loops itself; the root one never loops.
    return transport->isDerived() ? transport->getState() : loopedState(transport->getState());
}

ofxOceanodeTransportState ofxOceanodeTimelineManager::loopedState(ofxOceanodeTransportState state) const {
    state.loopEnabled = loopEnabled;
    state.loopStartBeat = loopStartBeat;
    state.loopEndBeat = loopEndBeat;
    state.loopCount = 0;
    if(!state.loopActive() || state.beatPosition < loopStartBeat) return state;
    // Past the loop start, the position wraps in the loop: the pass it is
    // in counts as a loop wrap (clip starts roll again, the scheduler looks
    // past the end into the next pass), a seek that changes it as a jump.
    const double length = loopEndBeat - loopStartBeat;
    const double passes = std::floor((state.beatPosition - loopStartBeat) / length);
    state.loopCount = static_cast<uint64_t>(std::max(0.0, passes));
    state.beatPosition = loopStartBeat + (state.beatPosition - loopStartBeat - passes * length);
    if(state.beatPosition >= loopEndBeat) state.beatPosition = loopStartBeat; // rounding at the end
    // Seconds of play follow the beat, as a transport's do on a wrap.
    state.seconds = beatToSeconds(state.beatPosition, state.bpm);
    return state;
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

ofxOceanodeTimelineCanvas* ofxOceanodeTimelineManager::findCanvasTimelines(const std::string& canvas) const {
    if(rootCanvas(canvas)) return const_cast<ofxOceanodeTimelineCanvas*>(&rootTimelines);
    for(const auto& entry : sequenceEntries())
        if(entry.address != nullptr && !entry.container.expired() && entry.address->getCanvasID() == canvas)
            return entry.timelines.get();
    return nullptr;
}

ofxOceanodeTimelineCanvas* ofxOceanodeTimelineManager::canvasTimelines(const std::string& canvas) {
    if(rootCanvas(canvas)) return &rootTimelines;
    auto* found = canvasContainer(canvas);
    return found != nullptr && found != container ? getMacroTimelines(*found) : nullptr;
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::findTimeline(const TimelineRef& ref) const {
    if(ref.timeline.empty()) return nullptr;
    const auto* canvas = findCanvasTimelines(ref.canvas);
    return canvas != nullptr ? canvas->find(ref.timeline) : nullptr;
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::timelineFor(const TimelineRef& ref) {
    if(ref.timeline.empty()) return nullptr;
    auto* canvas = canvasTimelines(ref.canvas);
    return canvas != nullptr ? canvas->find(ref.timeline) : nullptr;
}

std::vector<ofxOceanodeTimelineManager::TimelineRef> ofxOceanodeTimelineManager::timelinesOfCanvas(const std::string& canvas) {
    std::vector<TimelineRef> refs;
    const auto* list = canvasTimelines(canvas);
    if(list == nullptr) return refs;
    const std::string id = rootCanvas(canvas) ? std::string() : canvas;
    for(const auto& timeline : list->timelines) refs.push_back({id, timeline->timelineId});
    return refs;
}

std::string ofxOceanodeTimelineManager::suggestTimelineName(const std::string& canvas) const {
    const auto* list = findCanvasTimelines(canvas);
    uint64_t number = list != nullptr ? std::max<uint64_t>(list->nextTimelineNumber, 1) : 1;
    if(list != nullptr)
        while(list->find("tl_" + std::to_string(number)) != nullptr) ++number;
    return "Timeline " + std::to_string(number);
}

std::string ofxOceanodeTimelineManager::createTimeline(const std::string& canvas, const std::string& name) {
    auto* list = canvasTimelines(canvas);
    if(list == nullptr) return std::string();
    std::string id;
    uint64_t number = std::max<uint64_t>(list->nextTimelineNumber, 1);
    for(;; ++number) {
        id = "tl_" + std::to_string(number);
        if(list->find(id) == nullptr) break;
    }
    list->nextTimelineNumber = number + 1;
    auto timeline = std::make_shared<ofxOceanodeTimelineSequence>();
    timeline->base = list->base;
    timeline->timelineId = id;
    timeline->play.name = name.empty() ? "Timeline " + std::to_string(number) : name;
    timeline->play.timeSignatureNumerator = timeSignatureNumerator;
    timeline->play.timeSignatureDenominator = timeSignatureDenominator;
    list->timelines.push_back(std::move(timeline));
    invalidateEvaluationIndexes();
    return id;
}

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::addTimeline(const std::string& canvas, const std::string& name) {
    const auto id = createTimeline(canvas, name);
    return id.empty() ? TimelineRef{} : TimelineRef{rootCanvas(canvas) ? std::string() : canvas, id};
}

bool ofxOceanodeTimelineManager::removeTimeline(const TimelineRef& given) {
    const auto ref = normalizedRef(given);
    auto* list = canvasTimelines(ref.canvas);
    if(list == nullptr || list->find(ref.timeline) == nullptr) return false;
    if(isMetronomeEnabled(ref)) setMetronomeEnabled(false);
    timelineWindows.erase(std::remove_if(timelineWindows.begin(), timelineWindows.end(),
        [&](const auto& window) { return normalizedRef(window.ref) == ref; }), timelineWindows.end());
    for(auto it = players.begin(); it != players.end(); ++it)
        if((*it)->ref == ref) {
            destroyPlayer(**it);
            players.erase(it);
            break;
        }
    liveOverrides.erase(ref);
    playOnLoadPending.erase(ref);
    auto* sequence = list->find(ref.timeline);
    sequenceHistories.erase(sequence);
    for(const auto& track : sequence->tracks) clearTimelineFlag(track);
    list->timelines.erase(std::remove_if(list->timelines.begin(), list->timelines.end(),
        [&](const auto& timeline) { return timeline->timelineId == ref.timeline; }), list->timelines.end());
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
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
    const bool global = followsGlobalTransport(*source);
    const auto id = createTimeline(ref.canvas, name.empty() ? timelineName(ref) + " copy" : name);
    auto* duplicate = timelineFor({ref.canvas, id});
    if(duplicate == nullptr) return std::string();
    const auto play = duplicate->play;
    *duplicate = copy;
    duplicate->timelineId = id;
    duplicate->play.name = play.name;
    duplicate->play.nodesFollowClips = false; // one per canvas: the source keeps it
    if(global) {
        // Linked to the global transport: its tempo and meter are the
        // global ones, on the manager (its loop is its own: copied).
        auto& own = duplicate->play;
        const auto root = container != nullptr ? container->getRootTransport() : nullptr;
        own.bpm = root != nullptr ? root->getState().bpm : 120.0f;
        own.bpmAutomationEnabled = bpmAutomationEnabled;
        own.bpmMinimum = bpmMinimum;
        own.bpmMaximum = bpmMaximum;
        own.bpmInterpolation = bpmInterpolation;
        own.bpmPoints = bpmAutomationPoints;
        own.bpmTensions = bpmCurveTensions;
        own.timeSignatureNumerator = timeSignatureNumerator;
        own.timeSignatureDenominator = timeSignatureDenominator;
    }
    return id;
}

// ---- Clock sources ----

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::normalizedRef(TimelineRef ref) {
    if(ref.canvas == "Canvas" || ref.canvas == "0") ref.canvas.clear();
    return ref;
}

namespace {
// Within one canvas's timelines, whether `from` plays `target` through its
// same-canvas timeline tracks (the only way one timeline of a macro reaches
// another: tracks into the macros inside it never come back up).
bool reachesWithinCanvas(const ofxOceanodeTimelineCanvas& list, const std::string& from, const std::string& target) {
    std::vector<std::string> pending{from};
    std::set<std::string> seen;
    while(!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if(id == target) return true;
        if(!seen.insert(id).second) continue;
        const auto* sequence = list.find(id);
        if(sequence == nullptr) continue;
        for(const auto& track : sequence->tracks)
            if(track.isMacroTrack() && track.macroCanvas == ".") pending.push_back(track.macroTimeline);
    }
    return false;
}
}

void ofxOceanodeTimelineManager::refreshPlacedTimelines() const {
    placedTimelines.clear();
    inheritedPlacements.clear();
    placedTimelinesValid = true;
    // Every timeline track of every timeline: the timeline it places, and
    // (the first time) which timeline places it.
    auto scan = [&](const ofxOceanodeTimelineSequence& host, const std::string& canvas, const std::string& timelineId) {
        for(const auto& track : host.tracks) {
            if(!track.isMacroTrack()) continue;
            const std::string target = track.macroCanvas == "." ? canvas
                : canvas.empty() ? track.macroCanvas : canvas + " / " + track.macroCanvas;
            placedTimelines.emplace(TimelineRef{target, track.macroTimeline}, TimelineRef{canvas, timelineId});
        }
    };
    for(const auto& timeline : rootTimelines.timelines) scan(*timeline, std::string(), timeline->timelineId);
    pruneMacroSequences();
    for(const auto& entry : macroSequences) {
        const std::string canvas = entry.address->getCanvasID();
        for(const auto& timeline : entry.timelines->timelines) scan(*timeline, canvas, timeline->timelineId);
    }
    // A macro placed from outside is placed as a whole: its timelines no
    // track places follow the clips placing the first of its timelines a
    // timeline outside it places (not one that, through its own timeline
    // tracks, plays that timeline: it would place itself).
    for(const auto& entry : macroSequences) {
        const std::string canvas = entry.address->getCanvasID();
        for(const auto& placed : entry.timelines->timelines) {
            const TimelineRef placedRef{canvas, placed->timelineId};
            const auto host = placedTimelines.find(placedRef);
            if(host == placedTimelines.end() || host->second.canvas == canvas) continue;
            for(const auto& timeline : entry.timelines->timelines) {
                const TimelineRef ref{canvas, timeline->timelineId};
                if(ref == placedRef || placedTimelines.count(ref) > 0) continue;
                if(reachesWithinCanvas(*entry.timelines, ref.timeline, placedRef.timeline)) continue;
                placedTimelines.emplace(ref, host->second);
                inheritedPlacements.emplace(ref, placedRef);
            }
        }
    }
}

bool ofxOceanodeTimelineManager::placedBy(const TimelineRef& given, TimelineRef& host) {
    if(!placedTimelinesValid) refreshPlacedTimelines();
    const auto found = placedTimelines.find(normalizedRef(given));
    if(found == placedTimelines.end()) return false;
    host = found->second;
    return true;
}

ofxOceanodeTimelineManager::ClockSource ofxOceanodeTimelineManager::clockSourceOf(const TimelineRef& given) {
    const auto ref = normalizedRef(given);
    TimelineRef host;
    if(placedBy(ref, host)) return ClockSource::Dependent;
    const auto* sequence = findTimeline(ref);
    return sequence != nullptr && sequence->play.free ? ClockSource::Free : ClockSource::Global;
}

bool ofxOceanodeTimelineManager::followsGlobalTransport(const ofxOceanodeTimelineSequence& sequence) const {
    if(sequence.play.free) return false;
    TimelineRef ref;
    if(!refOf(sequence, ref)) return false;
    if(!placedTimelinesValid) refreshPlacedTimelines();
    return placedTimelines.count(normalizedRef(ref)) == 0;
}

ofxOceanodeTimelineManager* ofxOceanodeTimelineManager::engineFor(const TimelineRef& given) const {
    const auto* player = playerFor(normalizedRef(given));
    return player != nullptr ? player->engine.get() : nullptr;
}

ofxOceanodeTimelineManager* ofxOceanodeTimelineManager::enginePlaying(const ofxOceanodeTimelineSequence& sequence) const {
    if(isEngine()) return isPlayedSequence(sequence) ? const_cast<ofxOceanodeTimelineManager*>(this) : nullptr;
    TimelineRef ref;
    if(!refOf(sequence, ref)) return nullptr;
    return engineFor(ref);
}

void ofxOceanodeTimelineManager::noteLiveOverride(const TimelineRef& ref, const std::string& trackId,
                                                  const std::string& bindingId, const std::string& value) {
    liveOverrides[normalizedRef(ref)][{trackId, bindingId}] = value;
}

void ofxOceanodeTimelineManager::forgetLiveOverride(const TimelineRef& ref, const std::string& trackId, const std::string& bindingId) {
    const auto found = liveOverrides.find(normalizedRef(ref));
    if(found == liveOverrides.end()) return;
    found->second.erase({trackId, bindingId});
    if(found->second.empty()) liveOverrides.erase(found);
}

void ofxOceanodeTimelineManager::reapplyTransients(Player& player) {
    auto& engine = *player.engine;
    // The old copy's id-keyed state: its bindings are gone with it. The
    // path-keyed state (last applied values, sounding gates) stays, so a
    // stopped state-capture edit is found again on the next pass.
    engine.stateCaptureManualOverrideKeys.clear();
    engine.playback.drivenSolo.clear();
    const auto found = liveOverrides.find(player.ref);
    if(found == liveOverrides.end()) return;
    for(const auto& [key, value] : found->second) engine.setLiveOverride(key.first, key.second, value);
}

bool ofxOceanodeTimelineManager::isFree(const TimelineRef& given) const {
    const auto* sequence = findTimeline(normalizedRef(given));
    return sequence != nullptr && sequence->play.free;
}

void ofxOceanodeTimelineManager::setFree(const TimelineRef& given, bool free) {
    const auto ref = normalizedRef(given);
    auto* sequence = timelineFor(ref);
    if(sequence == nullptr || sequence->play.free == free) return;
    if(free && followsGlobalTransport(*sequence)) {
        // Off the global transport: it starts from the global tempo and
        // meter (its loop was its own already).
        auto& play = sequence->play;
        const auto root = container != nullptr ? container->getRootTransport() : nullptr;
        play.bpm = root != nullptr ? root->getState().bpm : 120.0f;
        play.bpmAutomationEnabled = bpmAutomationEnabled;
        play.bpmMinimum = bpmMinimum;
        play.bpmMaximum = bpmMaximum;
        play.bpmInterpolation = bpmInterpolation;
        play.bpmPoints = bpmAutomationPoints;
        play.bpmTensions = bpmCurveTensions;
        play.timeSignatureNumerator = timeSignatureNumerator;
        play.timeSignatureDenominator = timeSignatureDenominator;
    }
    sequence->play.free = free;
}

ofxOceanodeTimelineManager::Player* ofxOceanodeTimelineManager::playerFor(const TimelineRef& ref) const {
    for(const auto& player : players) if(player->ref == ref) return player.get();
    return nullptr;
}

std::shared_ptr<ofxOceanodeTransport> ofxOceanodeTimelineManager::transportFor(const TimelineRef& given) const {
    // Its player's; before its player exists (the first update pass after
    // it was made), a timeline linked to the global transport is on that
    // transport already.
    const auto ref = normalizedRef(given);
    if(auto* player = playerFor(ref)) return player->transport;
    if(!isEngine() && container != nullptr)
        if(const auto* sequence = findTimeline(ref); sequence != nullptr && followsGlobalTransport(*sequence))
            return container->getRootTransport();
    return nullptr;
}

float ofxOceanodeTimelineManager::hostBpmFor(const TimelineRef& ref) {
    const auto root = container != nullptr ? container->getRootTransport() : nullptr;
    const float fallback = root != nullptr ? root->getState().bpm : 120.0f;
    TimelineRef host;
    if(!placedBy(ref, host)) return fallback;
    // Through transports: a host dependent itself previews at its own host's tempo.
    if(auto transport = transportFor(host)) return std::max(1.0f, transport->getState().bpm);
    if(const auto* sequence = findTimeline(host); sequence != nullptr && sequence->play.free) {
        const auto& play = sequence->play;
        if(play.bpmAutomationEnabled && !play.bpmPoints.empty()) return std::max(1.0f, play.bpmPoints.front().value);
        return std::max(1.0f, play.followClockTempo ? fallback * play.tempoRatio : play.bpm);
    }
    return fallback;
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
    // Linked to the global transport: that transport, at once.
    if(!transport->isDerived()) { transport->play(); return; }
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
    if(!transport->isDerived()) { if(rewind) transport->stop(); else transport->pause(); return; }
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
    const auto* sequence = timelineFor(ref);
    return sequence != nullptr ? &sequence->play : nullptr;
}

bool ofxOceanodeTimelineManager::customTimelineColor(const TimelineRef& ref, ofColor& color) {
    const auto* sequence = timelineFor(ref);
    if(sequence == nullptr || !sequence->play.customColor) return false;
    color = sequence->play.color;
    return true;
}

void ofxOceanodeTimelineManager::setTimelineColor(const TimelineRef& ref, const ofColor& color) {
    if(auto* sequence = timelineFor(ref)) {
        sequence->play.customColor = true;
        sequence->play.color = color;
    }
}

void ofxOceanodeTimelineManager::resetTimelineColor(const TimelineRef& ref) {
    if(auto* sequence = timelineFor(ref)) sequence->play.customColor = false;
}

void ofxOceanodeTimelineManager::setPlaySettings(const TimelineRef& ref, const ofxOceanodeTimelinePlaySettings& settings) {
    if(auto* sequence = timelineFor(ref)) {
        // The clock source changes through setFree only (a copy of the
        // settings taken earlier never toggles it).
        const bool free = sequence->play.free;
        sequence->play = settings;
        auto& play = sequence->play;
        play.free = free;
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

// ---- Players ----

void ofxOceanodeTimelineManager::destroyPlayer(Player& player) {
    if(player.overridesCanvas)
        if(auto canvas = player.canvas.lock())
            if(canvas->getTransportOverride() == player.transport) canvas->setTransportOverride(nullptr);
    if(player.engine != nullptr) {
        // Correct whatever its backends were handed ahead.
        const auto state = player.engine->playState();
        player.engine->sendScheduleCorrections(state, ofxOceanodeScheduling::steadyNowUs());
        player.engine->updatePianoGateOwnership(false);
        player.engine->releaseWaveClips(player.engine->playedSequence, nullptr);
    }
    // Its own transport stops with it; the global one is not its to stop.
    if(player.transport != nullptr && player.transport->isDerived()) {
        player.transport->cancelPendingActions();
        player.transport->pause();
    }
}

void ofxOceanodeTimelineManager::syncPlayerList() {
    if(isEngine() || container == nullptr) return;
    auto root = container->getRootTransport();
    if(root == nullptr) return;
    refreshPlacedTimelines();
    // Every timeline there is, and how it plays.
    std::vector<std::pair<TimelineRef, ClockSource>> wanted;
    for(const auto& listed : allTimelines()) {
        const auto ref = normalizedRef(listed);
        wanted.push_back({ref, clockSourceOf(ref)});
    }
    // Gone, or on another kind of transport now (the global one, or one of its own).
    for(auto it = players.begin(); it != players.end();) {
        auto& player = **it;
        const bool canvasGone = !player.ref.canvas.empty() && player.canvas.expired();
        const auto found = std::find_if(wanted.begin(), wanted.end(), [&](const auto& entry) { return entry.first == player.ref; });
        const bool keep = !canvasGone && found != wanted.end() && timelineFor(player.ref) != nullptr &&
                          ((found->second == ClockSource::Global) == (player.source == ClockSource::Global));
        if(keep) {
            player.source = found->second;
            ++it;
            continue;
        }
        destroyPlayer(player);
        it = players.erase(it);
    }
    // New ones.
    for(const auto& [ref, source] : wanted) {
        if(playerFor(ref) != nullptr) continue;
        auto* sequence = timelineFor(ref);
        auto* canvas = canvasContainer(ref.canvas);
        if(sequence == nullptr || canvas == nullptr) continue;
        auto player = std::make_unique<Player>();
        player->ref = ref;
        player->source = source;
        const bool global = source == ClockSource::Global;
        player->transport = global ? root : ofxOceanodeTransport::makeDerived(root);
        if(!global) player->transport->setBpm(source == ClockSource::Free ? sequence->play.bpm : hostBpmFor(ref));
        player->engine = std::make_unique<ofxOceanodeTimelineManager>(container);
        player->engine->primary = this;
        player->engine->engineRef = ref; // it never places itself
        player->engine->engineTransport = player->transport;
        player->engine->engineSharesRoot = global;
        player->engine->schedulingEnabled = schedulingEnabled;
        player->engine->schedulingLookaheadMs = schedulingLookaheadMs;
        player->engine->schedulingLookaheadAdaptive = schedulingLookaheadAdaptive;
        player->engine->waveKeyPrefix = timeSourceId(ref) + "|";
        // The same chance rolls as the primary shows for the timeline.
        player->engine->clipChanceSalt = clipChanceSalt;
        player->engine->playContext.salt = clipChanceSalt;
        if(canvas != container)
            for(const auto& entry : macroSequences)
                if(entry.address == canvas) player->canvas = entry.container;
        players.push_back(std::move(player));
    }
}

void ofxOceanodeTimelineManager::syncPlayer(Player& player) {
    auto* sequence = timelineFor(player.ref);
    if(sequence == nullptr || player.engine == nullptr || container == nullptr) return;
    auto& engine = *player.engine;
    // The copy follows its source: rebuilt when its content changed. Its
    // schedule stays: an edit through the setters invalidates it on the
    // engine too (invalidateSchedule), and the scheduler repairs a direct
    // change to the content as it always did.
    const uint64_t signature = sequenceSignature(*sequence);
    engine.waveAudioProvider = waveAudioProvider; // Wave tracks play on it too, keyed apart
    if(player.sourceSequence != sequence || player.sourceSignature != signature) {
        engine.releaseWaveClips(engine.playedSequence, sequence);
        engine.playedSequence = *sequence;
        engine.playedSequence.base = nullptr;
        makeAbsolute(engine.playedSequence, rootCanvas(player.ref.canvas) ? std::string() : player.ref.canvas);
        engine.invalidateEvaluationIndexes();
        player.sourceSequence = sequence;
        player.sourceSignature = signature;
        reapplyTransients(player);
    }
    switch(player.source) {
        case ClockSource::Global: {
            // The global transport's tempo (and tempo lane, to look ahead
            // along it) and meter, read, never set; its own loop (the
            // engine wraps the global position in it: the transport
            // itself never loops).
            auto settings = sequence->play;
            settings.bpmAutomationEnabled = bpmAutomationEnabled;
            settings.bpmMinimum = bpmMinimum;
            settings.bpmMaximum = bpmMaximum;
            settings.bpmInterpolation = bpmInterpolation;
            settings.bpmPoints = bpmAutomationPoints;
            settings.bpmTensions = bpmCurveTensions;
            settings.followClockTempo = false;
            const auto root = container->getRootTransport();
            settings.bpm = root != nullptr ? root->getState().bpm : 120.0f;
            settings.timeSignatureNumerator = timeSignatureNumerator;
            settings.timeSignatureDenominator = timeSignatureDenominator;
            engine.adoptPlaySettings(settings);
            break;
        }
        case ClockSource::Free:
            engine.adoptPlaySettings(sequence->play);
            break;
        case ClockSource::Dependent: {
            // Its host's tempo now; its own loop and meter.
            auto settings = sequence->play;
            settings.bpmAutomationEnabled = false;
            settings.followClockTempo = false;
            settings.bpm = hostBpmFor(player.ref);
            engine.adoptPlaySettings(settings);
            break;
        }
    }
    // The toolbar's timestamping settings apply to every engine, live.
    engine.setSchedulingEnabled(schedulingEnabled);
    engine.setSchedulingLookaheadMs(schedulingLookaheadMs);
    engine.schedulingLookaheadAdaptive = schedulingLookaheadAdaptive;
    engine.retriggerGapBeats = retriggerGapBeats;
}

ofxOceanodeTimelineManager* ofxOceanodeTimelineManager::syncedEngine(const TimelineRef& given) {
    if(isEngine()) return nullptr;
    const auto ref = normalizedRef(given);
    if(ref.timeline.empty() || findTimeline(ref) == nullptr) return nullptr;
    auto* player = playerFor(ref);
    if(player == nullptr) { syncPlayerList(); player = playerFor(ref); }
    if(player == nullptr || player->engine == nullptr) return nullptr;
    syncPlayer(*player);
    return player->engine.get();
}

ofxOceanodeTimelineManager* ofxOceanodeTimelineManager::syncedEnginePlaying(const ofxOceanodeTimelineSequence& sequence) {
    if(isEngine()) return enginePlaying(sequence);
    TimelineRef ref;
    return refOf(sequence, ref) ? syncedEngine(ref) : nullptr;
}

void ofxOceanodeTimelineManager::updatePlayers() {
    if(isEngine() || container == nullptr) return;
    if(container->getRootTransport() == nullptr) return;
    syncPlayerList();
    updatePlayOnLoad();
    if(players.empty()) return;
    // Inner canvases first: an outer timeline's value lands last.
    std::stable_sort(players.begin(), players.end(), [](const auto& a, const auto& b) {
        return canvasDepth(a->ref.canvas) > canvasDepth(b->ref.canvas);
    });
    // First every engine's copy and settings, then (knowing which engines
    // run and what they drive) each engine's pass.
    std::vector<Player*> running;
    for(auto& player : players) {
        auto* sequence = timelineFor(player->ref);
        if(sequence == nullptr) continue;
        // A macro canvas's timeline playing free is what its canvas plays
        // to: claim the canvas once nothing else (a clip-following transport,
        // another free timeline of it) holds it; let it go when it is no
        // longer free.
        if(!player->ref.canvas.empty()) {
            auto canvas = player->canvas.lock();
            if(player->source == ClockSource::Free) {
                if(!player->overridesCanvas && canvas != nullptr && canvas->getTransportOverride() == nullptr) {
                    canvas->setTransportOverride(player->transport);
                    player->overridesCanvas = true;
                }
            } else if(player->overridesCanvas) {
                if(canvas != nullptr && canvas->getTransportOverride() == player->transport) canvas->setTransportOverride(nullptr);
                player->overridesCanvas = false;
            }
        }
        // A dependent timeline's preview runs only while it plays (and once
        // more after it stops, so what it held is released); idle, its
        // transport still keeps its host's tempo.
        if(player->source == ClockSource::Dependent) {
            const auto state = player->transport->getState();
            double at = 0.0;
            bool starts = false;
            const bool pending = player->transport->nextPendingAction(at, starts);
            const bool runs = state.isPlaying || pending || player->ranLastFrame;
            player->ranLastFrame = state.isPlaying || pending;
            if(!runs) {
                const float hostBpm = hostBpmFor(player->ref);
                if(std::abs(hostBpm - state.bpm) > 1e-4f) player->transport->setBpm(hostBpm);
                continue;
            }
        } else player->ranLastFrame = true;
        syncPlayer(*player);
        running.push_back(player.get());
    }
    // On a path several timelines drive, the value that lands last is the
    // last engine's (inner canvases first, so an outer timeline's): that
    // engine hands the path's events ahead; the others leave it to the
    // frame path. A dependent timeline's preview, applied after all of
    // them, is still left to its host's schedule as before.
    lastDriverForPath.clear();
    for(auto* player : running) {
        auto& engine = *player->engine;
        engine.ensureEvaluationIndexes();
        for(const auto& entry : engine.bindingsByPathCache) lastDriverForPath[entry.first] = &engine;
    }
    for(auto* player : running) {
        auto& engine = *player->engine;
        engine.pathDrivenElsewhere = [this, &engine](const std::string& path) {
            const auto found = lastDriverForPath.find(path);
            return found != lastDriverForPath.end() && found->second != &engine;
        };
        engine.beginParameterCache();
        engine.evaluateAutomation();
        engine.applyAutomation();
        engine.endParameterCache();
    }
}

void ofxOceanodeTimelineManager::updatePlayOnLoad() {
    // Loaded but not playing on its own: nothing to start.
    for(auto it = playOnLoadPending.begin(); it != playOnLoadPending.end();) {
        const auto* player = playerFor(normalizedRef(*it));
        if(player == nullptr || player->source != ClockSource::Free) {
            // Not a free timeline (or no player yet this frame): a timeline
            // that is not free drops out, one still coming waits.
            if(!isFree(*it)) { it = playOnLoadPending.erase(it); continue; }
        }
        ++it;
    }
    for(auto& player : players) {
        const auto* sequence = timelineFor(player->ref);
        if(sequence == nullptr || player->source != ClockSource::Free) continue;
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
    // The shape of the list: the players (each with its transport and clock
    // source), the timelines there are.
    size_t shape = players.size() * 7919 + rootTimelines.timelines.size() + macroSequences.size() * 31;
    for(const auto& player : players)
        shape = shape * 1099511628211ULL ^ (reinterpret_cast<uintptr_t>(player->transport.get()) + static_cast<size_t>(player->source));
    for(const auto& ref : allTimelines()) shape = shape * 31 + std::hash<std::string>()(ref.canvas + "#" + ref.timeline);
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
        // Linked to the global transport, it is that transport's position:
        // not listed (nodes follow the global transport itself), still found
        // by its id.
        info.hidden = transport != nullptr && !transport->isDerived();
        if(!info.selectable) info.note = "Not playing yet.";
        else if(clockSourceOf(ref) == ClockSource::Dependent)
            info.note = "Placed in another timeline: this follows its own transport, which previews it alone.";
        root->registerTimeSource(info, transport);
        present.insert(id);
        // Nodes saved before version 15 follow a canvas's then-default
        // timeline as "<canvas>#default": its first timeline now, by that
        // id too (listed with no label of its own).
        if(ref.timeline == "tl_1") {
            info.id = timeSourceId({ref.canvas, std::string()});
            info.hidden = true;
            root->registerTimeSource(info, transport);
            present.insert(info.id);
        }
    };
    const std::string rootGroup = container->getCanvasID();
    for(const auto& ref : allTimelines()) add(normalizedRef(ref), rootCanvas(ref.canvas) ? rootGroup : ref.canvas);
    for(const auto& id : registeredTimeSources)
        if(present.count(id) == 0) root->unregisterTimeSource(id);
    registeredTimeSources = std::move(present);
}

// ---- The timelines a timeline reaches through its timeline tracks ----

std::vector<ofxOceanodeTimelineManager::MacroSource> ofxOceanodeTimelineManager::collectMacroSources() const {
    std::vector<MacroSource> sources;
    if(container == nullptr) return sources;
    // The primary plays nothing itself: only an engine places timelines.
    if(!isEngine()) return sources;
    // An engine plays its primary's timelines.
    const auto* owner = primary;
    owner->pruneMacroSequences();
    const auto& entries = owner->macroSequences;
    // A canvas's timeline as it is kept (the root's on the owner).
    auto find = [&](const ofxOceanodeContainer* canvas, const std::string& timeline) -> const ofxOceanodeTimelineSequence* {
        const ofxOceanodeTimelineCanvas* list = nullptr;
        if(canvas == container) list = &owner->rootTimelines;
        else for(const auto& entry : entries) if(entry.address == canvas) list = entry.timelines.get();
        return list != nullptr ? list->find(timeline) : nullptr;
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
        const auto& sequence = parent < 0 ? playedSequence : *sources[static_cast<size_t>(parent)].sequence;
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
            // The rest of a macro placed from outside: its timelines no track
            // places follow this one's clips (refreshPlacedTimelines).
            if(track.macroCanvas == ".") continue;
            if(!owner->placedTimelinesValid) owner->refreshPlacedTimelines();
            const TimelineRef placedRef{canvasContainerFound->getCanvasID(), track.macroTimeline};
            for(const auto& [ref, from] : owner->inheritedPlacements) {
                if(from != placedRef) continue;
                const std::string siblingKey = keyOf(canvas, ref.timeline);
                if(std::find(keys.begin(), keys.end(), siblingKey) != keys.end()) continue;
                const auto* sibling = find(canvasContainerFound, ref.timeline);
                if(sibling == nullptr) continue;
                MacroSource source{canvasContainerFound, canvas, ref.timeline, sibling, parent, trackIndex};
                source.inheritsFrom = track.macroTimeline;
                sources.push_back(source);
                keys.push_back(siblingKey);
                if(sources.size() >= kMaximumSources) break;
            }
        }
    }
    return sources;
}

// ---- Timeline tracks ----

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::placedTimeline(const ofxOceanodeTimelineSequence& host,
                                                                       const ofxOceanodeTimelineTrack& track) {
    if(!track.isMacroTrack()) return nullptr;
    TimelineRef hostRef;
    if(!refOf(host, hostRef)) return nullptr;
    const std::string canvas = track.macroCanvas == "." ? hostRef.canvas
        : rootCanvas(hostRef.canvas) ? track.macroCanvas : hostRef.canvas + " / " + track.macroCanvas;
    return timelineFor({canvas, track.macroTimeline});
}

std::vector<ofxOceanodeTimelineSequence*> ofxOceanodeTimelineManager::carriedTimelines(
        const ofxOceanodeTimelineSequence& host, const ofxOceanodeTimelineTrack& track) {
    std::vector<ofxOceanodeTimelineSequence*> result;
    auto* placed = placedTimeline(host, track);
    if(placed == nullptr) return result;
    result.push_back(placed);
    TimelineRef placedRef;
    if(track.macroCanvas == "." || !refOf(*placed, placedRef)) return result;
    placedRef = normalizedRef(placedRef);
    if(!placedTimelinesValid) refreshPlacedTimelines();
    const auto* list = findCanvasTimelines(placedRef.canvas);
    if(list == nullptr) return result;
    for(const auto& timeline : list->timelines) {
        const auto found = inheritedPlacements.find({placedRef.canvas, timeline->timelineId});
        if(found != inheritedPlacements.end() && found->second == placedRef) result.push_back(timeline.get());
    }
    return result;
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

std::string ofxOceanodeTimelineManager::createMacroClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                        double startBeat, double durationBeats) {
    const auto* track = getTrack(sequence, trackId);
    if(track == nullptr || !track->isMacroTrack()) return std::string();
    auto* macroSequence = placedTimeline(sequence, *track);
    // One pass of the placed timeline, in beats: it follows the host's tempo
    // beat for beat (a bar of its own meter at least).
    const double length = macroSequence == nullptr ? beatsPerBarOf(sequence) : macroTimelineLength(*macroSequence);
    const auto id = createClip(sequence, trackId, track->name, startBeat, durationBeats > 0.0 ? durationBeats : length);
    if(auto* clip = getClip(sequence, trackId, id)) {
        clip->contentDurationBeats = length;
        clip->contentStretch = 1.0;
        clip->repeatContent = true;
    }
    invalidateEvaluationIndexes();
    return id;
}

std::string ofxOceanodeTimelineManager::createMacroClip(const std::string& trackId, double startBeat, double durationBeats) {
    return createMacroClip(editSequence(), trackId, startBeat, durationBeats);
}

// ---- Playheads and windows ----

template<typename Fn>
void ofxOceanodeTimelineManager::forEachPlayingManager(Fn&& fn) {
    // Asked between passes too (a playhead before the first update): the
    // players there should be, each engine with a copy of its timeline. A
    // timeline on a transport runs whenever it exists; a dependent one's
    // preview only while it plays.
    if(!isEngine()) syncPlayerList();
    for(auto& player : players) {
        if(player->engine == nullptr) continue;
        if(player->source == ClockSource::Dependent && !player->ranLastFrame) continue;
        if(!isEngine()) syncPlayer(*player); // current with its timeline (edits since the last pass)
        // The state as the engine plays it: the global position wrapped in
        // the timeline's loop.
        fn(*player->engine, player->engine->playState());
    }
}

bool ofxOceanodeTimelineManager::placedPlayheadBeat(const TimelineRef& given, double& beat) {
    if(container == nullptr) return false;
    const auto ref = normalizedRef(given);
    bool found = false;
    // Read from the GUI: the copies of the last update pass are enough. The
    // first placement (as evaluation plays it) by a playing timeline, else
    // by a stopped one at its playhead.
    for(int pass = 0; pass < 2 && !found; ++pass)
        forEachPlayingManager([&](ofxOceanodeTimelineManager& manager, const ofxOceanodeTransportState& state) {
            if(found || (pass == 0) != state.isPlaying) return;
            if(!manager.evaluationIndexesValid) manager.rebuildEvaluationIndexes();
            const double playhead = state.beatPosition;
            for(const auto& placement : manager.playedSequences(manager.playContext, playhead, playhead)) {
                for(const auto& played : manager.playedMacros)
                    if(played.sequence.get() == placement.sequence && played.canvas == ref.canvas && played.timeline == ref.timeline) {
                        beat = placement.map.toSequence(playhead);
                        if(played.reversed) beat = played.mirrorLength - beat; // back in the timeline as written
                        found = true;
                        return;
                    }
            }
        });
    return found;
}

bool ofxOceanodeTimelineManager::heldPlayheadBeat(const TimelineRef& given, double& beat) {
    if(container == nullptr) return false;
    const auto ref = normalizedRef(given);
    bool found = false;
    // Past the end of the clips placing it, its playhead stays where the
    // last of them left it (a precomposition holds its last frame): the
    // latest clip ended before the playhead, on a macro track of a timeline
    // playing there, by a playing timeline first.
    for(int pass = 0; pass < 2 && !found; ++pass)
        forEachPlayingManager([&](ofxOceanodeTimelineManager& manager, const ofxOceanodeTransportState& state) {
            if(found || (pass == 0) != state.isPlaying) return;
            if(!manager.evaluationIndexesValid) manager.rebuildEvaluationIndexes();
            const double playhead = state.beatPosition;
            double latestEnd = -std::numeric_limits<double>::infinity();
            for(const auto& outer : manager.playedSequences(manager.playContext, playhead, playhead)) {
                const double at = outer.map.toSequence(playhead);
                const auto outerContext = outer.contextFrom(manager.playContext);
                for(const auto& track : outer.sequence->tracks) {
                    if(!track.isMacroTrack() || !manager.isTrackAudible(track)) continue;
                    for(const auto& clip : track.clips) {
                        if(clip.muted || clip.isLfo || ofxOceanodeTimelineClipChanceSilenced(clip, outerContext)) continue;
                        const double end = clip.startBeat + std::max(1.0 / 24.0, clip.durationBeats);
                        if(end > at + 1e-9 || end <= latestEnd) continue;
                        const auto* played = manager.playedMacroAt(track, clip.reversed);
                        if(played == nullptr || played->canvas != ref.canvas) continue;
                        // The clip places it, or carries it with the placed
                        // timeline of its macro (placed as a whole).
                        if(played->timeline != ref.timeline) {
                            const auto* carried = manager.playedMacroAt(ref.canvas, ref.timeline, clip.reversed);
                            if(carried == nullptr || carried->inheritsFrom != played->timeline) continue;
                            played = carried;
                        }
                        // Its last beat as ofxOceanodeTimelineClipTime plays
                        // it: a whole repeat ends at the content's end, not
                        // back at its start.
                        const double stretch = std::max(1e-9, ofxOceanodeTimelineClipTime::effectiveStretch(clip));
                        const double content = ofxOceanodeTimelineClipTime::sourceDuration(clip);
                        const double elapsed = end - clip.startBeat;
                        const double cycle = clip.repeatContent ? content * stretch : elapsed;
                        double inCycle = std::fmod(elapsed, std::max(1e-9, cycle));
                        if(inCycle < 1e-9) inCycle = cycle;
                        double position = inCycle / stretch + ofxOceanodeTimelineClipTime::contentOffset(clip);
                        if(position > content + 1e-6) position -= content;
                        const double mirrorShift = clip.reversed ? played->mirrorLength - content : 0.0;
                        beat = clip.reversed ? played->mirrorLength - (position + mirrorShift) : position;
                        latestEnd = end;
                        found = true;
                    }
                }
            }
        });
    return found;
}

bool ofxOceanodeTimelineManager::timelinePlayheadBeat(const TimelineRef& given, double& beat) {
    const auto ref = normalizedRef(given);
    auto transport = transportFor(ref);
    if(clockSourceOf(ref) == ClockSource::Dependent) {
        // Dependent: its own transport while it previews, else the timeline
        // placing it drives its playhead (playing or seeked).
        bool previewing = false;
        if(transport != nullptr) {
            double at = 0.0;
            bool starts = false;
            previewing = transport->getState().isPlaying || transport->nextPendingAction(at, starts);
        }
        if(!previewing && (placedPlayheadBeat(ref, beat) || heldPlayheadBeat(ref, beat))) return true;
    }
    if(transport != nullptr) {
        // Where its engine plays: the global transport's position wrapped
        // in the timeline's own loop (its own transport's as it is).
        const auto* engine = engineFor(ref);
        beat = engine != nullptr ? engine->playState().beatPosition : transport->getState().beatPosition;
        return true;
    }
    return false;
}

const ofxOceanodeTimelineManager::TimelineWindow* ofxOceanodeTimelineManager::windowShowing(const TimelineRef& given) const {
    const auto ref = normalizedRef(given);
    const TimelineWindow* found = nullptr;
    for(const auto& window : timelineWindows) {
        if(!(normalizedRef(window.ref) == ref)) continue;
        if(found == nullptr || (!found->hostLabel.empty() && window.hostLabel.empty())) found = &window;
    }
    return found;
}

int ofxOceanodeTimelineManager::openTimelineWindow(const TimelineRef& ref) {
    int id = 1;
    for(const auto& window : timelineWindows) id = std::max(id, window.id + 1);
    TimelineWindow window;
    window.id = id;
    window.ref = normalizedRef(ref);
    window.view = viewState; // the preset's zoom and scroll to start from
    timelineWindows.push_back(window);
    return id;
}

void ofxOceanodeTimelineManager::closeTimelineWindow(int id) {
    timelineWindows.erase(std::remove_if(timelineWindows.begin(), timelineWindows.end(),
                                         [&](const auto& window) { return window.id == id; }), timelineWindows.end());
}

void ofxOceanodeTimelineManager::setHostedTimelineWindow(const TimelineRef& given, bool open, const std::string& hostLabel) {
    const auto ref = normalizedRef(given);
    const auto shows = [&](const TimelineWindow& window) { return normalizedRef(window.ref) == ref; };
    if(!open) {
        timelineWindows.erase(std::remove_if(timelineWindows.begin(), timelineWindows.end(), shows), timelineWindows.end());
        return;
    }
    for(auto& window : timelineWindows) if(shows(window)) { window.hostLabel = hostLabel; return; }
    const int id = openTimelineWindow(ref);
    for(auto& window : timelineWindows) if(window.id == id) window.hostLabel = hostLabel;
}

void ofxOceanodeTimelineManager::setTimelineWindowRef(int id, const TimelineRef& ref) {
    for(auto& window : timelineWindows) if(window.id == id) window.ref = normalizedRef(ref);
}

// ---- Nodes inside a placed macro following its clips ----

bool ofxOceanodeTimelineManager::nodesFollowClips(const TimelineRef& ref) {
    if(rootCanvas(ref.canvas)) return false;
    const auto* sequence = findTimeline(normalizedRef(ref));
    return sequence != nullptr && sequence->play.nodesFollowClips;
}

void ofxOceanodeTimelineManager::setNodesFollowClips(const TimelineRef& given, bool follow) {
    const auto ref = normalizedRef(given);
    if(rootCanvas(ref.canvas)) return;
    auto* list = canvasTimelines(ref.canvas);
    if(list == nullptr || list->find(ref.timeline) == nullptr) return;
    // One timeline of a canvas: its nodes can follow one clock.
    for(auto& timeline : list->timelines) timeline->play.nodesFollowClips = follow && timeline->timelineId == ref.timeline;
}

bool ofxOceanodeTimelineManager::clipFollowTimeline(const std::string& canvas, TimelineRef& ref) {
    if(rootCanvas(canvas)) return false;
    const auto* list = findCanvasTimelines(canvas);
    if(list == nullptr) return false;
    for(const auto& timeline : list->timelines)
        if(timeline->play.nodesFollowClips) { ref = {canvas, timeline->timelineId}; return true; }
    return false;
}

void ofxOceanodeTimelineManager::releaseClipTransport(ClipTransport& entry) {
    if(auto canvas = entry.canvas.lock())
        if(canvas->getTransportOverride() == entry.transport) canvas->setTransportOverride(nullptr);
}

void ofxOceanodeTimelineManager::updateClipTransports(const ofxOceanodeTransportState& rootState) {
    // The macros whose nodes follow their clips: the timeline of each.
    std::vector<TimelineRef> following;
    pruneMacroSequences();
    for(const auto& entry : macroSequences) {
        TimelineRef ref;
        if(clipFollowTimeline(entry.address->getCanvasID(), ref)) following.push_back(ref);
    }
    auto followed = [&](const std::string& canvas) -> const TimelineRef* {
        for(const auto& ref : following) if(ref.canvas == canvas) return &ref;
        return nullptr;
    };
    for(auto it = clipTransports.begin(); it != clipTransports.end();) {
        const auto* ref = followed(it->first);
        const bool keep = ref != nullptr && !it->second.canvas.expired() && clockSourceOf(*ref) != ClockSource::Free;
        if(keep) { ++it; continue; }
        releaseClipTransport(it->second);
        it = clipTransports.erase(it);
    }
    if(following.empty() || container == nullptr) return;
    auto root = container->getRootTransport();
    if(root == nullptr) return;
    // Where each timeline plays placements now: each engine, at its own position.
    struct Host { ofxOceanodeTimelineManager* manager; ofxOceanodeTransportState state; std::vector<PlayedSequence> placements; bool collected = false; };
    std::vector<Host> hosts;
    forEachPlayingManager([&](ofxOceanodeTimelineManager& manager, const ofxOceanodeTransportState& state) {
        hosts.push_back({&manager, state, {}});
    });
    for(const auto& ref : following) {
        const std::string& canvas = ref.canvas;
        if(clockSourceOf(ref) == ClockSource::Free) continue; // its own transport wins
        auto* canvasHost = canvasContainer(canvas);
        if(canvasHost == nullptr || canvasHost == container) continue;
        const auto* own = timelineFor(ref);
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
        // The first placement of its copy, by a playing timeline first.
        const PlayedSequence* found = nullptr;
        const PlayedMacro* foundCopy = nullptr;
        const Host* foundHost = nullptr;
        for(int pass = 0; pass < 2 && found == nullptr; ++pass)
            for(auto& host : hosts) {
                if(found != nullptr) break;
                if((pass == 0) != host.state.isPlaying) continue;
                if(!host.collected) {
                    host.placements = host.manager->playedSequences(host.manager->playContext, host.state.beatPosition, host.state.beatPosition);
                    host.collected = true;
                }
                for(const auto& placement : host.placements) {
                    const auto* played = host.manager->playedMacroOf(*placement.sequence);
                    if(played != nullptr && played->canvas == canvas && played->timeline == ref.timeline) {
                        found = &placement;
                        foundCopy = played;
                        foundHost = &host;
                        break;
                    }
                }
            }
        if(found == nullptr) {
            // No clip plays it here: it holds where it was, stopped.
            const auto held = entry.transport->getState();
            entry.transport->setDrivenState(held.beatPosition, held.bpm, false, false);
            entry.placement.clear();
            continue;
        }
        const auto& hostState = foundHost->state;
        double beat = found->map.toSequence(hostState.beatPosition);
        if(foundCopy->reversed) beat = foundCopy->mirrorLength - beat; // as written
        const float bpm = static_cast<float>(hostState.bpm / std::max(1e-6, found->map.ratio));
        const std::string key = ofToString(reinterpret_cast<uintptr_t>(found->placedClip)) + "/" +
                                ofToString(found->instanceKey) + "/" + ofToString(found->passMix) + "/" +
                                ofToString(found->windowFrom);
        const bool restart = key != entry.placement || hostState.generation != entry.rootGeneration;
        entry.placement = key;
        entry.rootGeneration = hostState.generation;
        entry.transport->setDrivenState(beat, bpm, hostState.isPlaying, restart);
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

// ---- Restoring a stretched timeline clip ----

bool ofxOceanodeTimelineManager::isClipStretched(const ofxOceanodeTimelineClip& clip) const {
    if(clip.repeatContent) return std::abs(clip.contentStretch - 1.0) > 1e-6;
    return std::abs(clip.durationBeats - ofxOceanodeTimelineClipTime::sourceDuration(clip)) > 1e-6;
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
        clip->durationBeats = std::max(1.0 / 24.0, ofxOceanodeTimelineClipTime::sourceDuration(*clip));
        clip->contentStretch = 1.0;
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

// ---- Timelines by reference: where each one lives ----

std::string ofxOceanodeTimelineManager::canvasIdOf(const ofxOceanodeContainer& canvas) const {
    if(&canvas == container || rootCanvas(canvas.getCanvasID())) return std::string();
    return canvas.getCanvasID();
}

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::refForCanvas(const ofxOceanodeContainer& canvas,
                                                                                 const std::string& timeline) const {
    return {canvasIdOf(canvas), timeline};
}

bool ofxOceanodeTimelineManager::refOf(const ofxOceanodeTimelineSequence& sequence, TimelineRef& ref) const {
    auto within = [&](const ofxOceanodeTimelineCanvas& list, const std::string& canvas) {
        for(const auto& timeline : list.timelines)
            if(timeline.get() == &sequence) { ref = {canvas, timeline->timelineId}; return true; }
        return false;
    };
    if(within(rootTimelines, std::string())) return true;
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        if(within(*entry.timelines, entry.address->getCanvasID())) return true;
    return false;
}

std::string ofxOceanodeTimelineManager::timelineName(const TimelineRef& ref) const {
    const auto* sequence = findTimeline(ref);
    if(sequence != nullptr && !sequence->play.name.empty()) return sequence->play.name;
    // "tl_7": "Timeline 7".
    if(ref.timeline.rfind("tl_", 0) == 0) return "Timeline " + ref.timeline.substr(3);
    return ref.timeline;
}

std::vector<ofxOceanodeTimelineManager::TimelineRef> ofxOceanodeTimelineManager::allTimelines() const {
    std::vector<TimelineRef> refs;
    for(const auto& timeline : rootTimelines.timelines) refs.push_back({std::string(), timeline->timelineId});
    pruneMacroSequences();
    std::vector<std::pair<std::string, const ofxOceanodeTimelineCanvas*>> macros;
    for(const auto& entry : macroSequences) macros.push_back({entry.address->getCanvasID(), entry.timelines.get()});
    std::sort(macros.begin(), macros.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for(const auto& [canvas, list] : macros)
        for(const auto& timeline : list->timelines) refs.push_back({canvas, timeline->timelineId});
    return refs;
}

std::vector<std::pair<ofxOceanodeTimelineManager::TimelineRef, ofxOceanodeTimelineSequence*>> ofxOceanodeTimelineManager::allTimelineSequences() const {
    std::vector<std::pair<TimelineRef, ofxOceanodeTimelineSequence*>> result;
    for(const auto& ref : allTimelines())
        if(auto* sequence = findTimeline(ref)) result.push_back({ref, sequence});
    return result;
}

bool ofxOceanodeTimelineManager::isLiveSequence(const ofxOceanodeTimelineSequence& sequence) const {
    TimelineRef ref;
    return refOf(sequence, ref); // a timeline of the root's or a macro's canvas
}
