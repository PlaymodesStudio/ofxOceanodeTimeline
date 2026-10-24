#pragma once

class ofxOceanodeTimelineManager;
class ofxOceanodeTimelineEditorView;
struct ofxOceanodeTimelineTrack;

class ofxOceanodeTimelineWaveVolumeEditor {
public:
    // Origin of beat zero in the arrangement, including horizontal scroll.
    // The properties panel may have a different width on narrow windows.
    void draw(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineTrack& track,
              const ofxOceanodeTimelineEditorView& view, float width, float height,
              double beatPosition, float timelineOriginX);

private:
    int waveVolumeDragPointIndex = -1;
    int waveVolumeValuePointIndex = -1;
    float waveVolumeNumericValue = 1.0f;
};
