//
//  lfoTrack.cpp
//  ofxOceanode
//

#include "lfoTrack.h"
#include "ofxOceanodeTimeline.h"
#include "ofxOceanodeContainer.h"

void lfoTrack::setup() {
    addTimeSourceParameter(); // which timeline's beat it follows (inspector)
    color = ofColor::red;
    description = "Outputs the LFO clips of a timeline track. Value is the LFO as drawn (0..1), "
                  "Phase the raw 0..1 ramp, Cycle the whole cycles since the timeline start "
                  "(continuous across clips and repeats) and Reset fires on each wrap during playback. "
                  "Connect Phase -> Phase and Cycle -> Step of random nodes in Sync To Transport mode "
                  "for results that are identical when scrubbing.";

    timelineRefs = {TimelineRef{}};
    timelineNames = {"None"};
    trackIds = {""};
    trackNames = {"None"};
    // Not saved as indices: the timeline reference and track id are saved in presetSave instead.
    timelineSelectorParam = addParameterDropdown(timelineSelector, "Timeline", 0, timelineNames,
        ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
    trackSelectorParam = addParameterDropdown(trackSelector, "Track", 0, trackNames,
        ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
    addOutputParameter(valueOut.set("Value", 0, 0, 1));
    addOutputParameter(phaseOut.set("Phase", 0, 0, 1));
    addOutputParameter(cycleOut.set("Cycle", 0, 0, FLT_MAX));
    addOutputParameter(resetOut.set("Reset"));
    addOutputParameter(activeOut.set("Active", false));

    listeners.push(timelineSelector.newListener([this](int &index) {
        if(updatingSelector) return;
        if(index >= 0 && index < static_cast<int>(timelineRefs.size()) && !(timelineRefs[index] == selectedRef)) {
            selectedRef = timelineRefs[index];
            selectedTrackId.clear(); // track ids are per timeline
            hasLastCycle = false;
        }
    }));
    listeners.push(trackSelector.newListener([this](int &index) {
        if(updatingSelector) return;
        if(index >= 0 && index < static_cast<int>(trackIds.size())) {
            selectedTrackId = trackIds[index];
            hasLastCycle = false;
        }
    }));
}

void lfoTrack::refreshTimelineList() {
    if(hostContainer == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    std::vector<TimelineRef> refs{TimelineRef{}};
    std::vector<std::string> names{"None"};
    for(const auto& ref : manager.allTimelines()) {
        const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
        refs.push_back(ofxOceanodeTimelineManager::normalizedRef(ref));
        names.push_back((root ? std::string() : ref.canvas + " > ") + manager.timelineName(ref));
    }
    // A saved timeline not there (yet: the timeline loads after node presets) stays selected.
    if(!selectedRef.timeline.empty() && std::find(refs.begin(), refs.end(), selectedRef) == refs.end()) {
        refs.push_back(selectedRef);
        names.push_back("Missing timeline");
    }
    if(refs != timelineRefs || names != timelineNames) {
        timelineRefs = refs;
        timelineNames = names;
        updatingSelector = true;
        timelineSelector.setMax(static_cast<int>(timelineNames.size()) - 1);
        if(timelineSelectorParam) timelineSelectorParam->setDropdownOptions(timelineNames);
        updatingSelector = false;
    }
    const int index = static_cast<int>(std::find(timelineRefs.begin(), timelineRefs.end(), selectedRef) - timelineRefs.begin());
    if(timelineSelector.get() != index) {
        updatingSelector = true;
        timelineSelector = index;
        updatingSelector = false;
    }
}

void lfoTrack::refreshTrackList() {
    if(hostContainer == nullptr) return;
    std::vector<std::string> ids = {""};
    std::vector<std::string> names = {"None"};
    if(const auto* sequence = hostContainer->getTimelineManager().findTimeline(selectedRef))
        for(const auto &track : sequence->tracks) {
            if(track.isWaveTrack) continue;
            ids.push_back(track.id);
            names.push_back(track.name);
        }
    if(ids != trackIds || names != trackNames) {
        trackIds = ids;
        trackNames = names;
        updatingSelector = true;
        trackSelector.setMax(static_cast<int>(trackNames.size()) - 1);
        if(trackSelectorParam) trackSelectorParam->setDropdownOptions(trackNames);
        updatingSelector = false;
    }
    // Keep the dropdown showing the selected track (by id).
    int index = 0;
    for(size_t i = 1; i < trackIds.size(); ++i) {
        if(trackIds[i] == selectedTrackId) { index = static_cast<int>(i); break; }
    }
    if(trackSelector.get() != index) {
        updatingSelector = true;
        trackSelector = index;
        updatingSelector = false;
    }
}

void lfoTrack::update(ofEventArgs &a) {
    refreshTimelineList();
    refreshTrackList();
    if(hostContainer == nullptr) return;

    const auto frameState = getFrameTransportState();
    auto& timeline = hostContainer->getTimelineManager();
    const auto* sequence = timeline.findTimeline(selectedRef);
    auto evaluate = [&](double beat) {
        return sequence != nullptr ? timeline.evaluateLfoTrack(*sequence, selectedTrackId, beat) : ofxOceanodeTimelineLfoSample{};
    };
    const auto sample = evaluate(frameState.current.beatPosition);

    // Cycle before Phase (as the Phasor does): phase-driven receivers store the step
    // and compute once when the phase arrives. Cycle is only sent when it changes.
    const float cycle = static_cast<float>(sample.cycle);
    if(cycle != cycleOut.get()) cycleOut = cycle;
    phaseOut = sample.phase;
    valueOut = sample.value;
    if(activeOut.get() != sample.active) activeOut = sample.active;

    // Reset on each wrap during continuous forward playback only (not on seeks
    // or stop), so scrubbing does not spray triggers. A loop wrap is continuous:
    // it fires when a cycle completes before the loop end, or the loop restarts
    // on the start of a cycle, or a cycle completes after the loop start.
    if(ofxOceanodeTimeUtils::didLoopWrap(frameState)) {
        const auto& state = frameState.current;
        const double eps = ofxOceanodeTimeUtils::StepEpsilon;
        const auto beforeEnd = evaluate(state.loopEndBeat - eps);
        const auto atStart = evaluate(state.loopStartBeat);
        const bool fire = (hasLastCycle && beforeEnd.active && beforeEnd.cycle > lastCycle) ||
            (atStart.active && atStart.phase <= 1e-4f) ||
            (sample.active && atStart.active && sample.cycle > atStart.cycle);
        if(fire) resetOut.trigger();
    } else {
        const bool continuous = !ofxOceanodeTimeUtils::didTransportDiscontinuity(frameState) &&
            frameState.current.beatPosition > frameState.previous.beatPosition;
        if(hasLastCycle && continuous && sample.active && sample.cycle > lastCycle) {
            resetOut.trigger();
        }
    }
    lastCycle = sample.cycle;
    hasLastCycle = true;
}

void lfoTrack::presetSave(ofJson &json) {
    json["timelineCanvas"] = selectedRef.canvas;
    json["timelineId"] = selectedRef.timeline;
    json["trackId"] = selectedTrackId;
}

void lfoTrack::presetRecallAfterSettingParameters(ofJson &json) {
    if(json.contains("timelineCanvas") && json["timelineCanvas"].is_string() &&
       json.contains("timelineId") && json["timelineId"].is_string()) {
        selectedRef = ofxOceanodeTimelineManager::normalizedRef(
            {json["timelineCanvas"].get<std::string>(), json["timelineId"].get<std::string>()});
    } else {
        // Saved before the node chose a timeline: a track of the main canvas's
        // then only timeline, "tl_1" now.
        selectedRef = {std::string(), "tl_1"};
    }
    if(json.contains("trackId") && json["trackId"].is_string()) {
        selectedTrackId = json["trackId"].get<std::string>();
        hasLastCycle = false;
    }
}
