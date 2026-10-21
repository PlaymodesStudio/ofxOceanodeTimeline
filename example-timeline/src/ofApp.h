#pragma once
#include "ofMain.h"
#include "ofxOceanode.h"
#include "ofxOceanodeTimeline.h"
class ofApp : public ofBaseApp {
public:
    ofxOceanode oceanode;
    void setup() override {
        oceanode.setup();
        ofxOceanodeTimeline::setup(oceanode);
    }
    void update() override { oceanode.update(); }
    void draw() override { oceanode.draw(); }
    void exit() override { oceanode.exit(); }
};
