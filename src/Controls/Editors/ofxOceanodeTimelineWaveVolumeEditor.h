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
    // The bend of a segment being dragged (as a Curve lane's): sideways moves
    // it along the segment, up and down sets how sharp it is.
    int waveVolumeBendSegment = -1;
    std::string waveVolumeBendTrackId;
    float waveVolumeBendStartX = 0.0f, waveVolumeBendStartY = 0.0f, waveVolumeBendWidth = 1.0f;
    float waveVolumeBendStartInflection = 0.5f, waveVolumeBendStartSteepness = 1.0f;
};
