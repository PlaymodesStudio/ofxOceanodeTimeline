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
    const bool isActive = std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) {
        return std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& binding) {
            return binding.parameterPath == parameterPath && !binding.bypass;
        });
    });
    if(auto* parameter = container->findTimelineParameter(parameterPath))
        parameter->setTimelined(isActive);
}

// Timeline history follows the canvas's gesture transactions and history
// limits, but owns only timeline data. Replaying it must not reload the patch
// or rewind live parameter values and the transport.
ofJson ofxOceanodeTimelineManager::timelineEditData() const {
    auto state = toJson();
    state.erase("view");
    return state;
}

ofxOceanodeTimelineManager::TimelineState ofxOceanodeTimelineManager::captureTimelineEdit() const {
    TimelineState state;
    state.data = timelineEditData();
    state.tracks = tracks;
    state.groups = clipGroups;
    state.markers = markers;
    state.tempoPoints = bpmAutomationPoints;
    state.tempoTensions = bpmCurveTensions;
    state.nextTrack = nextTrackNumber;
    state.nextBinding = nextBindingNumber;
    state.nextClip = nextClipNumber;
    state.nextLane = nextLaneNumber;
    state.nextGroup = nextGroupNumber;
    // Include the typed copy and audio caches in the history budget.
    state.bytes = state.data.dump().size() * 2;
    for(auto& track : state.tracks) {
        for(auto& binding : track.bindings) {
            binding.hasLiveOverride = false;
            binding.liveOverrideValue.clear();
        }
        for(const auto& clip : track.clips)
            state.bytes += clip.waveformPeaks.size() * sizeof(float);
    }
    return state;
}

void ofxOceanodeTimelineManager::restoreTimelineState(const TimelineState& state) {
    // Use in-memory data instead of the preset importer: that importer migrates
    // and normalizes content and reopens audio files. History must restore exact
    // editor data, including waveform caches for files no longer on disk.
    const auto oldView = viewState;
    clear();
    tracks = state.tracks;
    clipGroups = state.groups;
    markers = state.markers;
    bpmAutomationPoints = state.tempoPoints;
    bpmCurveTensions = state.tempoTensions;
    nextTrackNumber = state.nextTrack;
    nextBindingNumber = state.nextBinding;
    nextClipNumber = state.nextClip;
    nextLaneNumber = state.nextLane;
    nextGroupNumber = state.nextGroup;
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
    loopEnabled = data["loop"]["enabled"];
    loopStartBeat = data["loop"]["startBeat"];
    loopEndBeat = data["loop"]["endBeat"];
    schedulingEnabled = data["scheduling"]["enabled"];
    schedulingLookaheadMs = data["scheduling"]["lookaheadMs"];
    schedulingLookaheadAdaptive = data["scheduling"].value("adaptive", true);
    setTimeSignature(data["timeSignature"]["numerator"], data["timeSignature"]["denominator"]);
    masterGridBeats = data.value("masterGridBeats", 0.25);
    viewState = oldView;
    syncLoopToTransport();
    for(const auto& track : tracks)
        for(const auto& binding : track.bindings) refreshTimelineFlag(binding.parameterPath);
}

void ofxOceanodeTimelineManager::beginTimelineEdit() {
    if(replayingTimelineEdit || pendingTimelineEdit) return;
    pendingTimelineEdit = std::make_unique<TimelineEdit>();
    pendingTimelineEdit->before = captureTimelineEdit();
    if(container != nullptr && container->getTransport() != nullptr &&
       !container->getTransport()->hasExternalClock() && !bpmAutomationEnabled) {
        pendingTimelineEdit->manualTempo = true;
        pendingTimelineEdit->beforeBpm = container->getTransportState().bpm;
    }
}

void ofxOceanodeTimelineManager::endTimelineEdit() {
    if(!pendingTimelineEdit) return;
    auto edit = std::move(pendingTimelineEdit);
    edit->after = captureTimelineEdit();
    edit->manualTempo = edit->manualTempo && !bpmAutomationEnabled &&
        container != nullptr && !container->getTransport()->hasExternalClock();
    if(edit->manualTempo) {
        edit->afterBpm = container->getTransportState().bpm;
        edit->manualTempo = std::abs(edit->beforeBpm - edit->afterBpm) > 0.0001f;
    }
    if(edit->before.data == edit->after.data && !edit->manualTempo) return;
    edit->bytes = edit->before.bytes + edit->after.bytes;
    timelineRedo.clear();
    timelineUndo.push_back(std::move(*edit));
    trimTimelineHistory();
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
}

bool ofxOceanodeTimelineManager::stepTimelineHistory(bool backwards) {
    endTimelineEdit();
    auto& source = backwards ? timelineUndo : timelineRedo;
    auto& destination = backwards ? timelineRedo : timelineUndo;
    if(source.empty()) return false;
    const auto& edit = source.back();
    const bool tempoMismatch = edit.manualTempo && container != nullptr &&
        (container->getTransport() == nullptr || container->getTransport()->hasExternalClock() ||
         std::abs(container->getTransportState().bpm - (backwards ? edit.afterBpm : edit.beforeBpm)) > 0.0001f);
    if(tempoMismatch || timelineEditData() != (backwards ? edit.after.data : edit.before.data)) {
        // An unrecorded edit (e.g. parameter removal from the canvas) must
        // never be overwritten by restoring an older timeline wholesale.
        clearTimelineHistory();
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
        clearTimelineHistory();
        throw;
    }
    destination.push_back(std::move(source.back()));
    source.pop_back();
    return true;
}

bool ofxOceanodeTimelineManager::undoTimelineEdit() { return stepTimelineHistory(true); }
bool ofxOceanodeTimelineManager::redoTimelineEdit() { return stepTimelineHistory(false); }
