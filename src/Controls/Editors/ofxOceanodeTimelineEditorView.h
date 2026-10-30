#pragma once

class ofxOceanodeTimelineManager;

// Frame-local coordinate and grid policy shared by arrangement and editors.
// Pixel offsets are relative to beat zero; callers own their screen origin.
// This borrows the model and never retains widget or controller state.
// A view of a sequence placed on the timeline shown (a macro's, unfolded
// under its macro track) maps its beats there: sequence beat b is drawn at
// timeline beat mapOffset + b * mapRatio. Snapping stays in its own beats.
class ofxOceanodeTimelineEditorView {
public:
    static constexpr double ppq = 24.0;
    static constexpr float minPixelsPerSecond = 0.25f;

    ofxOceanodeTimelineEditorView(const ofxOceanodeTimelineManager& timeline,
                                 float fallbackBpm, double endBeat,
                                 bool snapEnabled, double snapBeats,
                                 double mapOffset = 0.0, double mapRatio = 1.0)
    : timeline(timeline), fallbackBpm(fallbackBpm), endBeat(endBeat),
      snapEnabled(snapEnabled), snapBeats(snapBeats),
      mapOffset(mapOffset), mapRatio(mapRatio > 0.0 ? mapRatio : 1.0) {}

    float beatToPixels(double beat) const;
    double pixelsToBeat(float pixels) const;
    static double snapBeat(double beat, bool enabled, double increment);
    double snapBeat(double beat) const { return snapBeat(beat, snapEnabled, snapBeats); }
    double displayGridBeats() const { return snapBeats > 0.0 ? snapBeats : 1.0; }
    double editIncrement() const { return snapBeats > 0.0 ? snapBeats : 1.0 / ppq; }

    const ofxOceanodeTimelineManager& timeline;
    const float fallbackBpm;
    const double endBeat;

private:
    const bool snapEnabled;
    const double snapBeats;
    const double mapOffset;
    const double mapRatio;
    bool mapped() const { return mapOffset != 0.0 || mapRatio != 1.0; }
};
