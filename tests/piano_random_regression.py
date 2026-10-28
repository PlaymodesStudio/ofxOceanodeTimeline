#!/usr/bin/env python3
"""Verify the Rndm tab: random octave and pitch deviation of piano notes, directions, persistence."""
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
    auto heardOver = [&](int starts) {
        std::set<int> heard;
        for(int i = 0; i < starts; ++i) {
            playAt(0.5);
            const int p = static_cast<int>(std::lround(pitches.get()));
            near(gates, 1);
            // Stable while playing through the same start.
            model.update(); model.applyAutomation(); assert(std::lround(pitches.get()) == p);
            heard.insert(p);
        }
        return heard;
    };
    owner.transport->play();
    // Off by default: the note plays as written.
    assert(heardOver(20) == std::set<int>({60}));
    // Octave, always, up to 2 octaves either way.
    lane = model.getLane(t, c, l);
    lane->randomOctave = 1.0f; lane->randomOctaveRange = 2;
    assert(heardOver(120) == std::set<int>({36, 48, 72, 84}));
    // Up only, then down only.
    lane = model.getLane(t, c, l);
    lane->randomOctaveDirection = 1;
    assert(heardOver(60) == std::set<int>({72, 84}));
    lane = model.getLane(t, c, l);
    lane->randomOctaveDirection = 2; lane->randomOctaveRange = 1;
    assert(heardOver(30) == std::set<int>({48}));
    // Half the time: some starts keep the written note.
    lane = model.getLane(t, c, l);
    lane->randomOctave = 0.5f;
    int kept = 0;
    for(int i = 0; i < 200; ++i) { playAt(0.5); kept += std::lround(pitches.get()) == 60; }
    assert(kept > 60 && kept < 140);
    // Pitch, always, up to 2 semitones down; the preview shows the same notes.
    lane = model.getLane(t, c, l);
    lane->randomOctave = 0.0f; lane->randomPitch = 1.0f; lane->randomPitchRange = 2; lane->randomPitchDirection = 2;
    assert(heardOver(60) == std::set<int>({58, 59}));
    std::vector<ofxOceanodeTimelinePianoNote> preview;
    playAt(0.5);
    assert(model.pianoRepeatNotes(t, c, l, 0, preview) && preview.size() == 1);
    assert(preview[0].pitch == std::lround(pitches.get()));
    // Snapping to the lane's scale: only scale keys (C major: 57, 59, 60 + 2 -> 62).
    lane = model.getLane(t, c, l);
    lane->randomPitchRange = 3; lane->randomPitchDirection = 0;
    lane->pianoScaleEnabled = true; lane->pianoScaleSnap = true; lane->pianoScaleMask = 0xAB5;
    assert(heardOver(80) == std::set<int>({57, 59, 62}));
    // Saved with the lane; older presets load with no deviation.
    lane = model.getLane(t, c, l);
    lane->randomOctave = 0.3f; lane->randomOctaveRange = 3; lane->randomOctaveDirection = 1;
    lane->randomPitch = 0.7f; lane->randomPitchRange = 5; lane->randomPitchDirection = 2;
    const auto saved = model.toJson();
    model.fromJson(saved);
    lane = model.getLane(t, c, l);
    assert(std::abs(lane->randomOctave - 0.3f) < 1e-6f && lane->randomOctaveRange == 3 && lane->randomOctaveDirection == 1);
    assert(std::abs(lane->randomPitch - 0.7f) < 1e-6f && lane->randomPitchRange == 5 && lane->randomPitchDirection == 2);
    auto legacy = saved;
    for(auto& track : legacy["tracks"]) for(auto& clip : track["clips"]) for(auto& laneJson : clip["lanes"])
        laneJson.erase("randomDeviation");
    model.fromJson(legacy);
    lane = model.getLane(t, c, l);
    assert(!lane->randomDeviates() && lane->randomOctave == 0.0f && lane->randomPitch == 0.0f);
    assert(!model.pianoRepeatNotes(t, c, l, 0, preview));
    std::cout << "Piano random deviation passed: octave and pitch, directions, chance, scale snap, preview and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
