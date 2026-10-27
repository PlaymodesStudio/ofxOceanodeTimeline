#!/usr/bin/env python3
"""Verify piano note mutation: per-note amount, lane rules, re-rolled per repeat and per start."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> pitches("Pitch", 72, 0, 127), gates("Gate", 0, 0, 1);
    ofxOceanodeParameter<float> pitch, gate;
    pitch.bindParameter(pitches); gate.bindParameter(gates);
    owner.targets[pitch.getEscapedName()] = &pitch;
    owner.targets[gate.getEscapedName()] = &gate;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    const auto t = model.createTrack("Instrument");
    const auto pb = model.addBinding(t, pitch), gb = model.addBinding(t, gate);
    const auto group = model.createNoteGroup(t, "Notes", pb, gb, "");
    std::string l;
    const auto c = model.createNoteGroupClip(t, group, "Phrase", 0, 4, &l);
    auto* lane = model.getLane(t, c, l);
    lane->pianoNotes = {{0, 3, 60, 0.8f, 1}};
    auto playAt = [&](double beat) { owner.transport->seekToBeat(beat); model.update(); model.applyAutomation(); };
    owner.transport->play();
    // No mutation amount: the note plays as written.
    for(int i = 0; i < 20; ++i) { playAt(0.5); near(pitches, 60); near(gates, 1); }
    // Always mutates, pitch only, onto E within an octave: 52 or 64.
    lane = model.getLane(t, c, l);
    lane->pianoNotes[0].mutation = 1.0f;
    lane->mutationPitchRange = 12;
    lane->mutationScaleMask = 1 << 4;
    std::set<int> heard;
    for(int i = 0; i < 40; ++i) {
        playAt(0.5);
        const int p = static_cast<int>(std::lround(pitches.get()));
        assert(p == 52 || p == 64);
        near(gates, 1);
        // Stable while playing through the same start.
        model.update(); model.applyAutomation(); assert(std::lround(pitches.get()) == p);
        heard.insert(p);
    }
    assert(heard.size() == 2); // re-rolled at each start
    std::vector<ofxOceanodeTimelinePianoNote> preview;
    assert(model.pianoRepeatNotes(t, c, l, 0, preview) && preview.size() == 1);
    assert(preview[0].pitch == 52 || preview[0].pitch == 64);
    // 50%: some starts keep the written note, some change it.
    lane = model.getLane(t, c, l);
    lane->pianoNotes[0].mutation = 0.5f;
    lane->mutationScaleMask = 0xFFF;
    int kept = 0;
    for(int i = 0; i < 200; ++i) { playAt(0.5); kept += std::lround(pitches.get()) == 60; }
    assert(kept > 60 && kept < 140);
    // Offset only: the note starts a grid step early or late.
    lane = model.getLane(t, c, l);
    lane->pianoNotes = {{1, 1, 60, 0.8f, 1, 1.0f}};
    lane->mutationPitchStrength = 0.0f; lane->mutationOffsetStrength = 1.0f; lane->mutationOffsetSteps = 1; lane->beatsPerStep = 0.5;
    int early = 0, late = 0;
    for(int i = 0; i < 40; ++i) {
        playAt(0.75);
        // This start's note (the preview rolls the same start as playback).
        assert(model.pianoRepeatNotes(t, c, l, 0, preview));
        const bool movedEarly = std::abs(preview[0].startBeat - 0.5) < 1e-9;
        assert(movedEarly || std::abs(preview[0].startBeat - 1.5) < 1e-9); // never unchanged
        near(preview[0].durationBeats, 1.0);
        assert((gates.get() > 0.5f) == movedEarly);
        early += movedEarly; late += !movedEarly;
    }
    assert(early > 0 && late > 0);
    // Rules with nothing to change leave the note alone.
    lane = model.getLane(t, c, l);
    lane->mutationOffsetStrength = 0.0f;
    for(int i = 0; i < 10; ++i) { playAt(1.25); near(gates, 1); near(pitches, 60); }
    assert(!model.pianoRepeatNotes(t, c, l, 0, preview));
    // Saved with the note and the lane; older presets load without mutation.
    lane = model.getLane(t, c, l);
    lane->mutationPitchStrength = 0.75f; lane->mutationScaleMask = 0x0A5; lane->mutationPitchRange = 5;
    lane->mutationLengthStrength = 0.4f; lane->mutationLengthMax = 3.0f;
    const auto saved = model.toJson();
    model.fromJson(saved);
    lane = model.getLane(t, c, l);
    near(lane->pianoNotes[0].mutation, 1.0);
    assert(std::abs(lane->mutationPitchStrength - 0.75f) < 1e-6f && lane->mutationScaleMask == 0x0A5 && lane->mutationPitchRange == 5);
    assert(std::abs(lane->mutationLengthStrength - 0.4f) < 1e-6f && std::abs(lane->mutationLengthMax - 3.0f) < 1e-6f && lane->mutationOffsetStrength == 0.0f);
    auto legacy = saved;
    for(auto& track : legacy["tracks"]) for(auto& clip : track["clips"]) for(auto& laneJson : clip["lanes"]) {
        laneJson.erase("mutation");
        for(auto& note : laneJson["pianoNotes"]) note.erase("mutation");
    }
    model.fromJson(legacy);
    lane = model.getLane(t, c, l);
    near(lane->pianoNotes[0].mutation, 0.0);
    assert(lane->mutationPitchStrength == 1.0f && lane->mutationScaleMask == 0xFFF && lane->mutationLengthStrength == 0.0f);
    std::cout << "Piano mutation passed: amount, pitch scale, re-rolls per start, offset, inactive rules, preview and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
