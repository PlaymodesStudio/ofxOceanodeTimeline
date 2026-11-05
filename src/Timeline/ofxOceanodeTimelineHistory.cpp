// Undo history of timeline edits.

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "Managers/PresetSaveCleanup.h"
#include "ofxOceanodeParameter.h"
#include "Nodes/ofxOceanodeNodeModel.h"
#include "Nodes/Default_Nodes/Base/baseOscillator.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <locale.h>
#include <cstring>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <type_traits>
#include "Timeline/ofxOceanodeTimelineInternal.h"

using namespace ofxOceanodeTimelineDetail;


void ofxOceanodeTimelineManager::clearTimelineFlag(const ofxOceanodeTimelineTrack& track) {
    if(container == nullptr) return;
    for(const auto& binding : track.bindings) {
        if(auto* parameter = container->findTimelineParameter(binding.parameterPath)) parameter->setTimelined(false);
    }
}

void ofxOceanodeTimelineManager::refreshTimelineFlag(const std::string& parameterPath) {
    if(container == nullptr) return;
    // Bound by any of the root canvas's timelines (their paths are from the
    // root; a macro's timelines flag what they play from their engines).
    auto binds = [&](const ofxOceanodeTimelineSequence& sequence) {
        return std::any_of(sequence.tracks.begin(), sequence.tracks.end(), [&](const auto& track) {
            return std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& binding) {
                return binding.parameterPath == parameterPath && !binding.bypass;
            });
        });
    };
    bool isActive = false;
    for(const auto& timeline : rootTimelines.timelines) isActive = isActive || binds(*timeline);
    if(auto* parameter = container->findTimelineParameter(parameterPath))
        parameter->setTimelined(isActive);
}

// The global history follows the canvas's gesture transactions and history
// limits, but owns only the global transport's settings: markers, the tempo
// lane, loop, meter, timebase, scheduling. Replaying it must not reload the
// patch or rewind live parameter values and the transport. Each timeline
// keeps a history of its own (below).
ofJson ofxOceanodeTimelineManager::timelineEditData() const {
    auto state = toJson();
    state.erase("view");
    // The timelines (each with its own history) and the windows stay as
    // they are across a global undo.
    for(const char* key : {"timelines", "nextTimelineNumber", "windows"}) state.erase(key);
    return state;
}

ofxOceanodeTimelineManager::TimelineState ofxOceanodeTimelineManager::captureTimelineEdit() const {
    TimelineState state;
    state.data = timelineEditData();
    state.markers = markers;
    state.tempoPoints = bpmAutomationPoints;
    state.tempoTensions = bpmCurveTensions;
    state.bytes = state.data.dump().size() * 2;
    return state;
}

void ofxOceanodeTimelineManager::restoreTimelineState(const TimelineState& state) {
    // In-memory data for what the JSON would round lossily; the timelines
    // and the windows are not touched.
    markers = state.markers;
    bpmAutomationPoints = state.tempoPoints;
    bpmCurveTensions = state.tempoTensions;
    const auto& data = state.data;
    timebase = data["timebase"] == "Time" ? ofxOceanodeTimelineTimebase::Time : ofxOceanodeTimelineTimebase::Beats;
    playOnPresetLoad = data["playOnPresetLoad"];
    const auto& tempo = data["tempo"];
    bpmAutomationEnabled = tempo["enabled"];
    bpmLaneCollapsed = tempo["collapsed"];
    bpmLaneVisible = tempo["visible"];
    bpmMinimum = tempo["minimum"];
    bpmMaximum = tempo["maximum"];
    bpmInterpolation = tempo["interpolation"].get<std::string>();
    // (The loop is each timeline's own, in its play settings: its history's.)
    schedulingEnabled = data["scheduling"]["enabled"];
    schedulingLookaheadMs = data["scheduling"]["lookaheadMs"];
    schedulingLookaheadAdaptive = data["scheduling"].value("adaptive", true);
    setTimeSignature(data["timeSignature"]["numerator"], data["timeSignature"]["denominator"]);
    masterGridBeats = data.value("masterGridBeats", 0.25);
    tempoSecondsCacheValid = false;
    invalidateEvaluationIndexes();
    invalidateSchedule();
}

// ---- One timeline's history: typed snapshots of its content ----

ofxOceanodeTimelineManager::SequenceSnapshot ofxOceanodeTimelineManager::captureSequence(const ofxOceanodeTimelineSequence& sequence) const {
    SequenceSnapshot snapshot;
    snapshot.data = sequenceContentToJson(sequence);
    snapshot.tracks = sequence.tracks;
    snapshot.groups = sequence.clipGroups;
    snapshot.play = sequence.play;
    snapshot.nextTrack = sequence.nextTrackNumber;
    snapshot.nextBinding = sequence.nextBindingNumber;
    snapshot.nextClip = sequence.nextClipNumber;
    snapshot.nextLane = sequence.nextLaneNumber;
    snapshot.nextGroup = sequence.nextGroupNumber;
    // The typed copy and its audio caches count in the history budget.
    snapshot.bytes = snapshot.data.dump().size() * 2;
    for(auto& track : snapshot.tracks) {
        for(auto& binding : track.bindings) {
            binding.hasLiveOverride = false;
            binding.liveOverrideValue.clear();
        }
        for(const auto& clip : track.clips) snapshot.bytes += clip.waveformPeaks.size() * sizeof(float);
    }
    return snapshot;
}

void ofxOceanodeTimelineManager::restoreSequence(ofxOceanodeTimelineSequence& sequence, const SequenceSnapshot& snapshot) {
    // In-memory data instead of the codec: that migrates and normalizes
    // content and reopens audio files. History restores exact editor data,
    // including waveform caches for files no longer on disk.
    invalidateSchedule(); // correct what was queued while the bindings still exist
    for(const auto& track : sequence.tracks) clearTimelineFlag(track);
    const bool free = sequence.play.free;
    sequence.tracks = snapshot.tracks;
    sequence.clipGroups = snapshot.groups;
    sequence.play = snapshot.play;
    sequence.play.free = free; // the clock source is not undone (its player would change hands)
    sequence.nextTrackNumber = snapshot.nextTrack;
    sequence.nextBindingNumber = snapshot.nextBinding;
    sequence.nextClipNumber = snapshot.nextClip;
    sequence.nextLaneNumber = snapshot.nextLane;
    sequence.nextGroupNumber = snapshot.nextGroup;
    invalidateEvaluationIndexes();
    std::set<std::string> paths;
    for(const auto& track : sequence.tracks)
        for(const auto& binding : track.bindings) paths.insert(binding.parameterPath);
    for(const auto& path : paths) refreshTimelineFlag(path);
}

uint64_t ofxOceanodeTimelineManager::openGesture() {
    const bool anyPending = pendingTimelineEdit != nullptr ||
        std::any_of(sequenceHistories.begin(), sequenceHistories.end(), [](const auto& entry) { return entry.second.pending != nullptr; });
    if(!anyPending) currentGesture = ++gestureSerial;
    return currentGesture;
}

void ofxOceanodeTimelineManager::beginGlobalEdit() {
    if(replayingTimelineEdit || pendingTimelineEdit) return;
    const uint64_t gesture = openGesture();
    pendingTimelineEdit = std::make_unique<TimelineEdit>();
    pendingTimelineEdit->gesture = gesture;
    pendingTimelineEdit->before = captureTimelineEdit();
    if(container != nullptr && container->getTransport() != nullptr &&
       !container->getTransport()->hasExternalClock() && !bpmAutomationEnabled) {
        pendingTimelineEdit->manualTempo = true;
        pendingTimelineEdit->beforeBpm = container->getTransportState().bpm;
    }
}

bool ofxOceanodeTimelineManager::endGlobalEdit() {
    if(!pendingTimelineEdit) return false;
    auto edit = std::move(pendingTimelineEdit);
    edit->after = captureTimelineEdit();
    edit->manualTempo = edit->manualTempo && !bpmAutomationEnabled &&
        container != nullptr && !container->getTransport()->hasExternalClock();
    if(edit->manualTempo) {
        edit->afterBpm = container->getTransportState().bpm;
        edit->manualTempo = std::abs(edit->beforeBpm - edit->afterBpm) > 0.0001f;
    }
    if(edit->before.data == edit->after.data && !edit->manualTempo) return false;
    edit->bytes = edit->before.bytes + edit->after.bytes;
    timelineRedo.clear();
    timelineUndo.push_back(std::move(*edit));
    trimTimelineHistory();
    return true;
}

void ofxOceanodeTimelineManager::trimTimelineHistory() {
    constexpr size_t maxEntries = 100;
    constexpr size_t maxBytes = 64 * 1024 * 1024;
    size_t bytes = 0;
    for(const auto* stack : {&timelineUndo, &timelineRedo})
        for(const auto& edit : *stack) bytes += edit.bytes;
    while(timelineUndo.size() + timelineRedo.size() > maxEntries || bytes > maxBytes) {
        auto& stack = timelineUndo.size() > 1 || timelineRedo.empty() ? timelineUndo : timelineRedo;
        if(stack.empty()) break;
        bytes -= stack.front().bytes;
        stack.pop_front();
    }
}

void ofxOceanodeTimelineManager::clearTimelineHistory() {
    pendingTimelineEdit.reset();
    timelineUndo.clear();
    timelineRedo.clear();
    sequenceHistories.clear();
}

bool ofxOceanodeTimelineManager::stepTimelineHistory(bool backwards) {
    endGlobalEdit();
    auto& source = backwards ? timelineUndo : timelineRedo;
    auto& destination = backwards ? timelineRedo : timelineUndo;
    if(source.empty()) return false;
    const auto& edit = source.back();
    const bool tempoMismatch = edit.manualTempo && container != nullptr &&
        (container->getTransport() == nullptr || container->getTransport()->hasExternalClock() ||
         std::abs(container->getTransportState().bpm - (backwards ? edit.afterBpm : edit.beforeBpm)) > 0.0001f);
    if(tempoMismatch || timelineEditData() != (backwards ? edit.after.data : edit.before.data)) {
        // An unrecorded change must never be overwritten by an older state.
        pendingTimelineEdit.reset();
        timelineUndo.clear();
        timelineRedo.clear();
        return false;
    }
    const auto& target = backwards ? edit.before : edit.after;
    replayingTimelineEdit = true;
    try {
        // Set manual tempo before restoring clip positions: the container's
        // Time timebase retiming must not rescale an already restored snapshot.
        if(edit.manualTempo && container != nullptr &&
           !container->getTransport()->hasExternalClock())
            container->setBpm(backwards ? edit.beforeBpm : edit.afterBpm, false);
        restoreTimelineState(target);
        replayingTimelineEdit = false;
    } catch(...) {
        replayingTimelineEdit = false;
        pendingTimelineEdit.reset();
        timelineUndo.clear();
        timelineRedo.clear();
        throw;
    }
    destination.push_back(std::move(source.back()));
    source.pop_back();
    return true;
}

bool ofxOceanodeTimelineManager::undoGlobalEdit() { return stepTimelineHistory(true); }
bool ofxOceanodeTimelineManager::redoGlobalEdit() { return stepTimelineHistory(false); }

// ---- The edit target's history, with the global one ----

ofxOceanodeTimelineSequence& ofxOceanodeTimelineManager::editSequence() {
    if(isEngine()) return playedSequence;
    auto* sequence = timelineFor(editTarget);
    return sequence != nullptr ? *sequence : playedSequence;
}

const ofxOceanodeTimelineSequence& ofxOceanodeTimelineManager::editSequence() const {
    if(isEngine()) return playedSequence;
    const auto* sequence = findTimeline(editTarget);
    return sequence != nullptr ? *sequence : playedSequence;
}

void ofxOceanodeTimelineManager::beginTimelineEdit() {
    beginGlobalEdit();
    beginTimelineEdit(editSequence());
}

bool ofxOceanodeTimelineManager::endTimelineEdit() {
    const bool global = endGlobalEdit();
    return endTimelineEdit(editSequence()) || global;
}

bool ofxOceanodeTimelineManager::canUndoTimelineEdit() const { return canUndoTimelineEdit(editSequence()) || canUndoGlobalEdit(); }
bool ofxOceanodeTimelineManager::canRedoTimelineEdit() const { return canRedoTimelineEdit(editSequence()) || canRedoGlobalEdit(); }

bool ofxOceanodeTimelineManager::undoTimelineEdit() {
    endTimelineEdit(); // an open gesture is a step
    // The latest gesture: its step in each history it changed (the
    // timeline's first; one refused, for an unrecorded change since,
    // refuses the gesture).
    const auto& history = sequenceHistories[&editSequence()];
    const uint64_t sequence = history.undo.empty() ? 0 : history.undo.back().second.gesture;
    const uint64_t global = timelineUndo.empty() ? 0 : timelineUndo.back().gesture;
    if(sequence == 0 && global == 0) return false;
    if(sequence >= global && !undoTimelineEdit(editSequence())) return false;
    if(global >= sequence && !undoGlobalEdit()) return false;
    return true;
}

bool ofxOceanodeTimelineManager::redoTimelineEdit() {
    endTimelineEdit();
    // The next gesture forward (the earliest undone): the global step first, as recorded.
    const auto& history = sequenceHistories[&editSequence()];
    constexpr uint64_t none = ~0ULL;
    const uint64_t sequence = history.redo.empty() ? none : history.redo.back().second.gesture;
    const uint64_t global = timelineRedo.empty() ? none : timelineRedo.back().gesture;
    if(sequence == none && global == none) return false;
    if(global <= sequence && !redoGlobalEdit()) return false;
    if(sequence <= global && !redoTimelineEdit(editSequence())) return false;
    return true;
}

// A timeline's history: snapshots of the timeline alone, one entry per
// gesture that changed it.
void ofxOceanodeTimelineManager::beginTimelineEdit(const ofxOceanodeTimelineSequence& sequence) {
    if(replayingTimelineEdit) return;
    const uint64_t gesture = openGesture();
    auto& history = sequenceHistories[&sequence];
    if(!history.pending) {
        history.pending = std::make_unique<SequenceSnapshot>(captureSequence(sequence));
        history.pending->gesture = gesture;
    }
}

bool ofxOceanodeTimelineManager::endTimelineEdit(const ofxOceanodeTimelineSequence& sequence) {
    const auto found = sequenceHistories.find(&sequence);
    if(found == sequenceHistories.end() || !found->second.pending) return false;
    auto& history = found->second;
    auto before = std::move(*history.pending);
    history.pending.reset();
    auto after = captureSequence(sequence);
    if(before.data == after.data) return false;
    after.gesture = before.gesture;
    history.redo.clear();
    history.undo.emplace_back(std::move(before), std::move(after));
    // The entries and their typed copies within a budget.
    constexpr size_t maxEntries = 100;
    constexpr size_t maxBytes = 64 * 1024 * 1024;
    size_t bytes = 0;
    for(const auto& step : history.undo) bytes += step.first.bytes + step.second.bytes;
    while(history.undo.size() > maxEntries || (bytes > maxBytes && history.undo.size() > 1)) {
        bytes -= history.undo.front().first.bytes + history.undo.front().second.bytes;
        history.undo.pop_front();
    }
    return true;
}

bool ofxOceanodeTimelineManager::canUndoTimelineEdit(const ofxOceanodeTimelineSequence& sequence) const {
    const auto found = sequenceHistories.find(&sequence);
    return found != sequenceHistories.end() && !found->second.undo.empty();
}

bool ofxOceanodeTimelineManager::canRedoTimelineEdit(const ofxOceanodeTimelineSequence& sequence) const {
    const auto found = sequenceHistories.find(&sequence);
    return found != sequenceHistories.end() && !found->second.redo.empty();
}

bool ofxOceanodeTimelineManager::undoTimelineEdit(ofxOceanodeTimelineSequence& sequence) {
    endTimelineEdit(sequence);
    auto& history = sequenceHistories[&sequence];
    if(history.undo.empty()) return false;
    // An unrecorded change since must not be overwritten by an older snapshot.
    if(sequenceContentToJson(sequence) != history.undo.back().second.data) { history.undo.clear(); history.redo.clear(); return false; }
    replayingTimelineEdit = true;
    restoreSequence(sequence, history.undo.back().first);
    replayingTimelineEdit = false;
    history.redo.push_back(std::move(history.undo.back()));
    history.undo.pop_back();
    return true;
}

bool ofxOceanodeTimelineManager::redoTimelineEdit(ofxOceanodeTimelineSequence& sequence) {
    endTimelineEdit(sequence);
    auto& history = sequenceHistories[&sequence];
    if(history.redo.empty()) return false;
    if(sequenceContentToJson(sequence) != history.redo.back().first.data) { history.undo.clear(); history.redo.clear(); return false; }
    replayingTimelineEdit = true;
    restoreSequence(sequence, history.redo.back().second);
    replayingTimelineEdit = false;
    history.undo.push_back(std::move(history.redo.back()));
    history.redo.pop_back();
    return true;
}
