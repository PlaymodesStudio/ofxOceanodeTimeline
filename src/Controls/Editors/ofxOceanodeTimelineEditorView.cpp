#include "ofxOceanodeTimelineEditorView.h"
#include "Timeline/ofxOceanodeTimeline.h"

#include <algorithm>
#include <cmath>

float ofxOceanodeTimelineEditorView::beatToPixels(double beat) const {
    const double pixelsPerSecond = timeline.getViewState().pixelsPerSecond;
    if(!mapped())
        return static_cast<float>(timeline.beatToSeconds(std::max(0.0, beat), fallbackBpm) * pixelsPerSecond);
    // A placed sequence can start before the timeline does (a rotated
    // macro clip at beat 0): carry on at the first beat's width there.
    const double shown = mapOffset + beat * mapRatio;
    if(shown >= 0.0) return static_cast<float>(timeline.beatToSeconds(shown, fallbackBpm) * pixelsPerSecond);
    return static_cast<float>(shown * timeline.beatToSeconds(1.0, fallbackBpm) * pixelsPerSecond);
}

double ofxOceanodeTimelineEditorView::pixelsToBeat(float pixels) const {
    const double pixelsPerSecond = std::max(minPixelsPerSecond, timeline.getViewState().pixelsPerSecond);
    if(!mapped()) return timeline.secondsToBeat(std::max(0.0f, pixels) / pixelsPerSecond, fallbackBpm);
    const double shown = pixels >= 0.0f
        ? timeline.secondsToBeat(pixels / pixelsPerSecond, fallbackBpm)
        : pixels / (pixelsPerSecond * std::max(1e-9, timeline.beatToSeconds(1.0, fallbackBpm)));
    return (shown - mapOffset) / mapRatio;
}

double ofxOceanodeTimelineEditorView::snapBeat(double beat, bool enabled, double increment) {
    if(!enabled || increment <= 0.0) return std::max(0.0, beat);
    return std::max(0.0, std::round(beat / increment) * increment);
}
