//
//  timelineTransport.h
//  ofxOceanode
//
//  Plays, stops and restarts a timeline of the project from the patch: the
//  main one, a further one, or a macro's. A timeline other than the main one
//  has its own transport only while it plays on its own ("Independent"), so
//  the triggers do nothing for the others (marked in the dropdown).
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
