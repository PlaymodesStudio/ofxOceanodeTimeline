//
//  timelineTransport.h
//  ofxOceanode
//
//  Plays, stops and restarts a timeline of the project from the patch: any
//  of the main canvas's or a macro's. One linked to the global transport
//  plays, stops and seeks that transport; a free one its own; a dependent
//  one (placed as clips in another, marked in the dropdown) its preview.
//

#ifndef timelineTransport_h
#define timelineTransport_h

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeTimeline.h"

class timelineTransport : public ofxOceanodeNodeModel {
public:
    timelineTransport() : ofxOceanodeNodeModel("Timeline") {}

    void setup() override;
    void update(ofEventArgs &a) override;

    void presetSave(ofJson &json) override;
    void presetRecallAfterSettingParameters(ofJson &json) override;

private:
    using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
    void refreshTimelineList();

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

    ofEventListeners listeners;
};

#endif /* timelineTransport_h */
