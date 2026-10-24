#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"

#include <algorithm>
#include <cmath>

float ofxOceanodeTimelineEditorView::beatToPixels(double beat) const {
    return static_cast<float>(timeline.beatToSeconds(std::max(0.0, beat), fallbackBpm) *
                              timeline.getViewState().pixelsPerSecond);
}

double ofxOceanodeTimelineEditorView::pixelsToBeat(float pixels) const {
    const double targetSeconds = std::max(0.0f, pixels) /
                                 std::max(minPixelsPerSecond, timeline.getViewState().pixelsPerSecond);
    double low = 0.0;
    double high = std::max(1.0, endBeat);
    while(timeline.beatToSeconds(high, fallbackBpm) < targetSeconds && high < 1000000.0) high *= 2.0;
    for(int iteration = 0; iteration < 40; ++iteration) {
        const double middle = (low + high) * 0.5;
        if(timeline.beatToSeconds(middle, fallbackBpm) < targetSeconds) low = middle;
        else high = middle;
    }
    return (low + high) * 0.5;
}

double ofxOceanodeTimelineEditorView::snapBeat(double beat, bool enabled, double increment) {
    if(!enabled || increment <= 0.0) return std::max(0.0, beat);
    return std::max(0.0, std::round(beat / increment) * increment);
}
