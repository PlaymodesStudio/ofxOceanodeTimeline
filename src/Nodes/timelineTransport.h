//
//  timelineTransport.h
//  ofxOceanode
//
//  Plays, stops and restarts a timeline of the project from the patch: any
//  of the main canvas's or a macro's. One linked to the global transport
//  plays, stops and seeks that transport; a free one its own; a dependent
//  one (placed as clips in another, marked in the dropdown) its preview.
//  When the timeline has a Macro Morphing Track, the node also publishes
//  that track's routers, under a ROUTERS separator: Input routers as its
//  inputs, Output routers as its outputs. Several nodes may point at one
//  timeline: each has every router.
//

#ifndef timelineTransport_h
#define timelineTransport_h

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeTimeline.h"
#include "MacroMixerPorts.h"

class timelineTransport : public ofxOceanodeNodeModel {
public:
    timelineTransport() : ofxOceanodeNodeModel("Timeline") {}

    void setup() override;
    void update(ofEventArgs &a) override;
    void draw(ofEventArgs &a) override;

    void presetSave(ofJson &json) override;
    void loadBeforeConnections(ofJson &json) override;
    void presetRecallAfterSettingParameters(ofJson &json) override;

private:
    using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
    void refreshTimelineList();
    // The router ports follow the selected timeline's Macro Morphing Track
    // (none: no ports); inputs go to its engine, its outputs come back.
    void updateRouters();

    ofParameter<int> timelineSelector;
    std::shared_ptr<ofxOceanodeParameter<int>> timelineSelectorParam;
    ofParameter<void> play;
    ofParameter<void> stop;
    ofParameter<void> restart;

    // The timeline is remembered by reference (names can change, and the
    // timeline loads after node presets); the dropdown index is derived from
    // it every frame. Defaults to the main timeline.
    TimelineRef selectedRef;
    std::vector<TimelineRef> timelineRefs;
    std::vector<std::string> timelineNames;
    bool updatingSelector = false;

    // The routers of the selected timeline's Macro Morphing Track. The port
    // list is saved with the node and installed again before the cables
    // load (the timeline loads after them), then matched against the
    // track's routers once the timeline is there: a router that keeps its
    // id, type and direction keeps its port and cables.
    MacroMixerPorts routers{*this, "ROUTERS"};
    // The engine the ports were last matched against (compared only).
    const MacroMixerRuntime* routersEngine = nullptr;

    ofEventListeners listeners;
};

#endif /* timelineTransport_h */
