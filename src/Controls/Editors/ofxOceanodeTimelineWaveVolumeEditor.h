#pragma once
#include <string>

class ofxOceanodeTimelineManager;
class ofxOceanodeTimelineEditorView;
struct ofxOceanodeTimelineTrack;
class ofxOceanodeTimelineSequence;

class ofxOceanodeTimelineWaveVolumeEditor {
public:
    // Origin of beat zero in the arrangement, including horizontal scroll.
    // The properties panel may have a different width on narrow windows.
    void draw(ofxOceanodeTimelineManager& timeline, ofxOceanodeTimelineSequence& sequence, ofxOceanodeTimelineTrack& track,
              const ofxOceanodeTimelineEditorView& view, float width, float height,
              double beatPosition, float timelineOriginX);

private:
    int waveVolumeDragPointIndex = -1;
    std::string waveVolumeDragTrackId;
    int waveVolumeValuePointIndex = -1;
    float waveVolumeNumericValue = 1.0f;
};
