#!/usr/bin/env python3
"""Exercise timeline gesture history using the production model and OF runtime."""
import timeline_regression as fixture

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> output("Value", 0.0f, 0.0f, 1.0f);
    ofxOceanodeParameter<float> target; target.bindParameter(output);
    owner.targets[target.getEscapedName()] = &target;
    ofxOceanodeTimelineManager model(&owner);
    assert(!model.undoTimelineEdit() && !model.redoTimelineEdit());
    model.beginTimelineEdit();
    const auto track = model.createTrack("Track");
    const auto binding = model.addBinding(track, target);
    const auto clip = model.createClip(track, "Clip", 0.0, 4.0);
    const auto lane = model.createLane(track, clip, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(track, clip, lane, binding);
    model.getLane(track, clip, lane)->curvePoints = {{0.0, 0.2f}, {4.0, 0.8f}};
    model.endTimelineEdit();
    const auto original = model.toJson();
    assert(model.canUndoTimelineEdit() && target.isTimelined());
    owner.transport->seekToBeat(2.0); owner.transport->play();
    assert(model.undoTimelineEdit());
    assert(model.getTracks().empty() && !target.isTimelined());
    assert(owner.transport->getState().isPlaying);
    assert(owner.transport->getState().beatPosition == 2.0);
    assert(model.redoTimelineEdit());
    assert(model.toJson() == original && target.isTimelined());
    model.update(); assert(std::abs(output.get() - 0.5f) < 1e-5);

    // Repeated begin calls across drag frames retain the original baseline.
    model.beginTimelineEdit();
    model.getClip(track, clip)->startBeat = 1.0;
    model.beginTimelineEdit();
    model.getClip(track, clip)->startBeat = 2.0;
    model.beginTimelineEdit();
    model.getClip(track, clip)->startBeat = 3.0;
    model.endTimelineEdit();
    assert(model.undoTimelineEdit());
    assert(model.getClip(track, clip)->startBeat == 0.0);
    assert(model.canRedoTimelineEdit());
    // No-op gestures and navigation preserve redo, zoom and scroll.
    model.beginTimelineEdit();
    model.getViewState().scrollX = 100.0f;
    model.getViewState().pixelsPerSecond = 85.0f;
    model.endTimelineEdit();
    assert(model.canRedoTimelineEdit());
    assert(model.redoTimelineEdit());
    assert(model.getViewState().scrollX == 100.0f);
    assert(model.getViewState().pixelsPerSecond == 85.0f);
    assert(model.undoTimelineEdit());
    model.beginTimelineEdit();
    model.getLane(track, clip, lane)->curvePoints[0].value = 0.4f;
    model.endTimelineEdit();
    assert(!model.canRedoTimelineEdit());
    assert(model.undoTimelineEdit());
    assert(model.getLane(track, clip, lane)->curvePoints[0].value == 0.2f);

    // Deleted tracks/bindings, lane contents, markers and groups round-trip.
    model.beginTimelineEdit();
    const auto copy = model.duplicateClip(track, clip, track, 8.0);
    assert(!copy.empty());
    model.groupClips({{track, clip}, {track, copy}});
    model.addMarker(2.0, "Entry");
    model.setLoopRange(1.0, 7.0); model.setLoopEnabled(true);
    model.getBpmAutomationPoints() = {{0.0, 100.0f}, {4.0, 150.0f}};
    model.setBpmAutomationEnabled(true);
    model.endTimelineEdit();
    const auto grouped = model.toJson();
    assert(model.undoTimelineEdit());
    assert(model.getClipGroups().empty() && model.getMarkers().empty());
    assert(model.redoTimelineEdit());
    assert(model.toJson() == grouped);
    model.beginTimelineEdit(); model.removeTrack(track); model.endTimelineEdit();
    assert(!target.isTimelined());
    assert(model.undoTimelineEdit() && target.isTimelined());
    assert(model.toJson() == grouped);

    // Live parameter writes/preview overrides are not history content.
    model.setLiveOverride(track, binding, "0.9"); output = 0.9f;
    model.beginTimelineEdit(); model.endTimelineEdit();
    assert(model.redoTimelineEdit());
    assert(output.get() == 0.9f);
    assert(model.undoTimelineEdit());
    assert(!model.getBinding(track, binding)->hasLiveOverride);

    // Manual BPM is restored along with edits in Time timebase, without
    // moving the playhead or running the container's retiming a second time.
    model.setBpmAutomationEnabled(false);
    model.clearTimelineHistory();
    owner.setBpm(120.0f);
    model.setTimebase(ofxOceanodeTimelineTimebase::Time);
    model.beginTimelineEdit();
    model.retimeForBpmChange(120.0f, 60.0f); owner.setBpm(60.0f);
    model.endTimelineEdit();
    const double newDuration = model.getClip(track, clip)->durationBeats;
    assert(model.undoTimelineEdit());
    assert(owner.transport->getState().bpm == 120.0f);
    assert(model.getClip(track, clip)->durationBeats == newDuration * 2.0);
    assert(model.redoTimelineEdit());
    assert(owner.transport->getState().bpm == 60.0f);
    assert(model.getClip(track, clip)->durationBeats == newDuration);

    // Restoring audio clips retains their caches and never reads their source
    // files. This also covers content that the preset importer would migrate.
    model.beginTimelineEdit();
    const auto wave = model.createWaveTrack("Wave");
    const auto waveClip = model.createClip(wave, "Audio", 0.0, 4.0);
    auto* audio = model.getClip(wave, waveClip);
    audio->waveFilePath = "/missing-history-source.wav";
    audio->waveformPeaks = {0.1f, 0.5f, -0.5f};
    audio->waveFileDurationMs = 2000.0;
    audio->waveNumChannels = 2;
    model.endTimelineEdit();
    model.beginTimelineEdit(); model.removeTrack(wave); model.endTimelineEdit();
    assert(model.undoTimelineEdit());
    audio = model.getClip(wave, waveClip);
    assert(audio && audio->waveformPeaks == std::vector<float>({0.1f, 0.5f, -0.5f}));
    assert(audio->waveFileDurationMs == 2000.0 && audio->waveNumChannels == 2);
    assert(model.redoTimelineEdit());
    assert(!model.getTrack(wave));

    // Unrecorded external changes cannot be overwritten by old history.
    model.createTrack("External");
    assert(!model.undoTimelineEdit() && !model.canRedoTimelineEdit());
    model.beginTimelineEdit(); model.addMarker(1.0, "A"); model.endTimelineEdit();
    auto saved = model.toJson(); model.fromJson(saved);
    assert(!model.canUndoTimelineEdit() && !model.canRedoTimelineEdit());
    model.beginTimelineEdit(); model.addMarker(2.0, "B"); model.endTimelineEdit();
    model.clear(); assert(!model.canUndoTimelineEdit());
    owner.setBpm(120.0f);
    model.beginTimelineEdit(); owner.setBpm(90.0f); model.endTimelineEdit();
    owner.setBpm(100.0f); // external, unrecorded tempo change
    assert(!model.undoTimelineEdit());
    assert(owner.transport->getState().bpm == 100.0f);
    // Same entry cap as canvas history, including after undo/redo transfers.
    for(int i = 0; i < 105; ++i) {
        model.beginTimelineEdit(); model.addMarker(i, "Marker"); model.endTimelineEdit();
    }
    for(int i = 0; i < 100; ++i) assert(model.undoTimelineEdit());
    assert(!model.undoTimelineEdit() && model.getMarkers().size() == 5);
    for(int i = 0; i < 100; ++i) assert(model.redoTimelineEdit());
    assert(!model.redoTimelineEdit() && model.getMarkers().size() == 105);
    std::cout << "Timeline history passed: gestures, content, deletion, bindings, tempo, transport, view, branching, external edits, preset reset, limits.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
