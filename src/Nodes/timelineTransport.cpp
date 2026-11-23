//
//  timelineTransport.cpp
//  ofxOceanode
//

#include "timelineTransport.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeAutomationExtension.h"
#include "ofxOceanodeCustomGuiExtension.h"

void timelineTransport::setup() {
    color = ofColor::red;
    description = "Plays, stops and restarts the selected timeline. Stop goes back to its start, "
                  "Restart plays it again from its start. Play and Stop follow the timeline's quantize "
                  "setting on its own transport (free, or the preview of a dependent one); a timeline "
                  "linked to the global transport plays, stops and seeks that transport at once.";

    timelineRefs = {TimelineRef{}};
    timelineNames = {"(none)"};
    // Not saved as an index: the timeline reference is saved in presetSave instead.
    timelineSelectorParam = addParameterDropdown(timelineSelector, "Timeline", 0, timelineNames,
        ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
    addParameter(play.set("Play"));
    addParameter(stop.set("Stop"));
    addParameter(restart.set("Restart"));

    listeners.push(timelineSelector.newListener([this](int &index) {
        if(updatingSelector) return;
        if(index >= 0 && index < static_cast<int>(timelineRefs.size())) selectedRef = timelineRefs[index];
        lastGoToMarker = -1; // (another timeline: any number jumps again)
    }));
    // The manager, while a timeline is selected (none: nothing to play).
    auto manager = [this]() -> ofxOceanodeTimelineManager* {
        if(hostContainer == nullptr || selectedRef.timeline.empty()) return nullptr;
        return &hostContainer->getTimelineManager();
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
    // Go To Marker: the playhead to the marker of that number (0 the
    // earliest) of the timeline's Marker Track, as Restart seeks. Only a
    // change jumps, so an input set every frame does not hold the playhead.
    goToMarker.set("Go To Marker", -1, -1, 0);
    listeners.push(goToMarker.newListener([this, manager](int &index) {
        if(index == lastGoToMarker) return;
        lastGoToMarker = index;
        auto* timeline = manager();
        if(timeline == nullptr || index < 0) return;
        const auto* sequence = timeline->findTimeline(selectedRef);
        double beat = 0.0;
        if(sequence != nullptr && ofxOceanodeTimelineManager::trackMarkerBeat(*sequence, index, beat))
            timeline->seekTimeline(selectedRef, beat);
    }));
    // A Trigger Input router fires in the track's engine as it arrives,
    // from whichever node it comes.
    routers.onTriggerInput = [this, manager](const std::string& id) {
        auto* timeline = manager();
        if(timeline == nullptr) return;
        if(const auto* sequence = timeline->findTimeline(selectedRef))
            if(auto* engine = timeline->morphEngine(*sequence)) engine->triggerInput(id);
    };
}

void timelineTransport::refreshTimelineList() {
    if(hostContainer == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    std::vector<TimelineRef> refs{TimelineRef{}};
    std::vector<std::string> names{"(none)"};
    for(const auto& ref : manager.allTimelines()) {
        const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
        std::string name = (root ? std::string() : ref.canvas + " > ") + manager.timelineName(ref);
        if(manager.clockSourceOf(ref) == ofxOceanodeTimelineManager::ClockSource::Dependent) name += " (dependent)";
        refs.push_back(ofxOceanodeTimelineManager::normalizedRef(ref));
        names.push_back(name);
    }
    // A saved timeline that is not there (yet: the timeline loads after node
    // presets) stays selected, shown as missing.
    if(!selectedRef.timeline.empty() && std::find(refs.begin(), refs.end(), selectedRef) == refs.end()) {
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

void timelineTransport::updateRouters() {
    if(hostContainer == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    const auto* sequence = selectedRef.timeline.empty() ? nullptr : manager.findTimeline(selectedRef);
    // A saved timeline that is not there (yet: the timeline loads after node
    // presets) keeps the ports restored with the node, and their cables.
    if(sequence == nullptr && !selectedRef.timeline.empty()) return;
    // Another timeline, none, or one without a Macro Morphing Track: no routers.
    auto* engine = sequence != nullptr ? manager.morphEngine(*sequence) : nullptr;
    if(engine == nullptr) {
        routers.clear();
        routersEngine = nullptr;
        return;
    }
    manager.noteMorphPublisher(*sequence);
    // The ports match the track's routers: reinstalled when they were
    // edited, and when the engine is another one (the timeline has loaded,
    // an undo restored it, another timeline was selected).
    const auto& contract = engine->contract().routers;
    const auto& ports = routers.ports();
    bool same = routersEngine == engine && ports.size() == contract.size();
    for(size_t i = 0; same && i < ports.size(); ++i)
        same = ports[i].id == contract[i].id && ports[i].name == contract[i].name &&
               ports[i].type == contract[i].type && ports[i].direction == contract[i].direction;
    if(!same) {
        routers.install(*engine);
        // An input nothing is connected to shows the engine's value (the one
        // saved with the track), instead of writing its own over it.
        const auto& inputs = engine->inputs();
        for(const auto& port : routers.ports()) {
            if(port.direction != "Input" || port.type == MacroMixerMixing::triggerType) continue;
            const auto value = inputs.find(port.id);
            auto* parameter = routers.parameter(port.id);
            if(value == inputs.end() || parameter == nullptr || parameter->hasInConnection()) continue;
            // (Borrowed and registered values only arrive through a cable.)
            if(std::holds_alternative<ofTexture*>(value->second) ||
               std::holds_alternative<MacroMixerMixing::Object>(value->second)) continue;
            MacroMixerMixing::writePort(port.type, *parameter, value->second);
        }
        routersEngine = engine;
    }
    // Only what changed on this node's ports is written: with several nodes
    // on one timeline, the last change of any of them wins.
    routers.syncInputs(*engine, true);
    // The engine updates here, with the inputs just written (once per
    // frame: the first Timeline node of the timeline to update).
    manager.updateMorphEngine(*sequence);
    // The engine is read once per frame by the timeline; every node
    // publishes that reading (a Trigger reaches them all).
    if(const auto* outputs = manager.morphOutputs(*sequence)) routers.publish(*outputs);
}

void timelineTransport::installMarkerPort() {
    if(goToMarkerParam) return;
    addSeparator("MARKERS");
    goToMarkerParam = addParameter(goToMarker, ofxOceanodeParameterFlags_DisableOutConnection |
                                   ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
    parameterGroupChanged.notify();
}

void timelineTransport::removeMarkerPort() {
    if(!goToMarkerParam) return;
    auto& parameter = static_cast<ofxOceanodeAbstractParameter&>(*goToMarkerParam);
    // As a router port goes: its bindings, custom GUI entries and cables first.
    if(hostContainer != nullptr && parameter.isTimelined())
        if(auto* extension = hostContainer->getAutomationExtension()) extension->removeParameterBindings(parameter);
    if(hostContainer != nullptr && hostContainer->customGuiContainsParameterAnywhere(parameter))
        if(auto* extension = hostContainer->getCustomGuiExtension()) extension->removeParameterBindings(parameter);
    parameter.removeAllConnections();
    removeParameter(parameter.getName());
    removeSeparator("MARKERS");
    goToMarkerParam.reset();
    lastGoToMarker = -1;
    parameterGroupChanged.notify();
}

void timelineTransport::updateMarkerPort() {
    if(hostContainer == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    const auto* sequence = selectedRef.timeline.empty() ? nullptr : manager.findTimeline(selectedRef);
    // A saved timeline that is not there (yet: the timeline loads after node
    // presets) keeps the port restored with the node, and its cable.
    if(sequence == nullptr && !selectedRef.timeline.empty()) return;
    const auto* track = sequence != nullptr ? ofxOceanodeTimelineManager::markerTrackOf(*sequence) : nullptr;
    if(track == nullptr) { removeMarkerPort(); return; }
    installMarkerPort();
    // Its range: the markers there are (-1: none).
    const int last = std::max(0, static_cast<int>(track->trackMarkers.size()) - 1);
    if(goToMarker.getMax() != last) goToMarker.setMax(last);
}

void timelineTransport::update(ofEventArgs &a) {
    refreshTimelineList();
    updateRouters();
    updateMarkerPort();
}

void timelineTransport::draw(ofEventArgs &a) {
    // The track's sources draw with the patch (their textures are rendered
    // here), once per frame whichever node asks first; then the textures go
    // out. The other outputs went out on update: written again here, every
    // node connected to them would run twice a frame.
    if(hostContainer == nullptr || selectedRef.timeline.empty() || routersEngine == nullptr) return;
    auto& manager = hostContainer->getTimelineManager();
    const auto* sequence = manager.findTimeline(selectedRef);
    if(sequence == nullptr) return;
    manager.drawMorphEngine(*sequence);
    if(const auto* outputs = manager.morphOutputs(*sequence)) routers.publish(*outputs, true);
}

void timelineTransport::presetSave(ofJson &json) {
    json["timelineCanvas"] = selectedRef.canvas;
    json["timelineId"] = selectedRef.timeline;
    json["timelineFormat"] = 15; // an empty id means none (before 15: the canvas's default timeline)
    // The router ports as they are, so their cables find them on load.
    if(!routers.ports().empty()) json["morphRouters"] = routers.savePorts();
    // Go To Marker, so its cable finds it on load.
    if(goToMarkerParam) json["markerPort"] = true;
}

void timelineTransport::loadBeforeConnections(ofJson &json) {
    // Node presets and cables load before the timeline: the saved router
    // ports are installed here, for the cables, and matched against the
    // track's routers once the timeline has loaded (updateRouters).
    if(!json.contains("morphRouters") || !routers.restorePorts(json["morphRouters"])) routers.clear();
    routersEngine = nullptr;
    // Go To Marker the same way (its timeline's Marker Track is checked once loaded).
    if(json.value("markerPort", false)) installMarkerPort();
}

void timelineTransport::presetRecallAfterSettingParameters(ofJson &json) {
    if(json.contains("timelineCanvas") && json["timelineCanvas"].is_string() &&
       json.contains("timelineId") && json["timelineId"].is_string()) {
        selectedRef = ofxOceanodeTimelineManager::normalizedRef(
            {json["timelineCanvas"].get<std::string>(), json["timelineId"].get<std::string>()});
        // Saved before timelines had ids of their own: the canvas's then-default one, "tl_1" now.
        if(selectedRef.timeline.empty() && json.value("timelineFormat", 0) < 15) selectedRef.timeline = "tl_1";
    }
}
