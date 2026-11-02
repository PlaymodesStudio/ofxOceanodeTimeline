//
//  timelineTransport.cpp
//  ofxOceanode
//

#include "timelineTransport.h"
#include "ofxOceanodeContainer.h"

void timelineTransport::setup() {
    color = ofColor::red;
    description = "Plays, stops and restarts the selected timeline. Stop goes back to its start, "
                  "Restart plays it again from its start. Play and Stop follow the timeline's quantize "
                  "setting. A timeline other than the main one only responds while it plays on its own "
                  "(Independent in its settings); the others are marked as not independent.";

    timelineRefs = {TimelineRef{}};
    timelineNames = {"Main"};
    // Not saved as an index: the timeline reference is saved in presetSave instead.
    timelineSelectorParam = addParameterDropdown(timelineSelector, "Timeline", 0, timelineNames,
        ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
    addParameter(play.set("Play"));
    addParameter(stop.set("Stop"));
    addParameter(restart.set("Restart"));

    listeners.push(timelineSelector.newListener([this](int &index) {
        if(updatingSelector) return;
        if(index >= 0 && index < static_cast<int>(timelineRefs.size())) selectedRef = timelineRefs[index];
    }));
    auto manager = [this]() -> ofxOceanodeTimelineManager* {
        return hostContainer != nullptr ? &hostContainer->getTimelineManager() : nullptr;
    };
    // As the timeline toolbar does: Stop goes back to the start.
    listeners.push(play.newListener([this, manager]() {
        if(auto* timeline = manager()) timeline->playTimeline(selectedRef);
    }));
    listeners.push(stop.newListener([this, manager]() {
        if(auto* timeline = manager()) timeline->stopTimeline(selectedRef, true);
    }));
    listeners.push(restart.newListener([this, manager]() {
        if(auto* timeline = manager()) {
            timeline->seekTimeline(selectedRef, 0.0);
            timeline->playTimeline(selectedRef);
        }
    }));
}

void timelineTransport::refreshTimelineList() {
    if(hostContainer == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    std::vector<TimelineRef> refs;
    std::vector<std::string> names;
    for(const auto& ref : manager.allTimelines()) {
        const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
        std::string name = (root ? std::string() : ref.canvas + " > ") + manager.timelineName(ref);
        if(manager.transportFor(ref) == nullptr) name += " (not independent)";
        refs.push_back(ofxOceanodeTimelineManager::normalizedRef(ref));
        names.push_back(name);
    }
    // A saved timeline that is not there (yet: the timeline loads after node
    // presets) stays selected, shown as missing.
    if(std::find(refs.begin(), refs.end(), selectedRef) == refs.end()) {
        refs.push_back(selectedRef);
        names.push_back("Missing timeline");
    }
    if(refs != timelineRefs || names != timelineNames) {
        timelineRefs = refs;
        timelineNames = names;
        // The selection is kept by reference: a shorter list does not retarget it.
        updatingSelector = true;
        timelineSelector.setMax(static_cast<int>(timelineNames.size()) - 1);
        if(timelineSelectorParam) timelineSelectorParam->setDropdownOptions(timelineNames);
        updatingSelector = false;
    }
    // Keep the dropdown showing the selected timeline (by reference).
    const int index = static_cast<int>(std::find(timelineRefs.begin(), timelineRefs.end(), selectedRef) - timelineRefs.begin());
    if(timelineSelector.get() != index) {
        updatingSelector = true;
        timelineSelector = index;
        updatingSelector = false;
    }
}

void timelineTransport::update(ofEventArgs &a) {
    refreshTimelineList();
}

void timelineTransport::presetSave(ofJson &json) {
    json["timelineCanvas"] = selectedRef.canvas;
    json["timelineId"] = selectedRef.timeline;
}

void timelineTransport::presetRecallAfterSettingParameters(ofJson &json) {
    if(json.contains("timelineCanvas") && json["timelineCanvas"].is_string() &&
       json.contains("timelineId") && json["timelineId"].is_string()) {
        selectedRef = ofxOceanodeTimelineManager::normalizedRef(
            {json["timelineCanvas"].get<std::string>(), json["timelineId"].get<std::string>()});
    }
}
