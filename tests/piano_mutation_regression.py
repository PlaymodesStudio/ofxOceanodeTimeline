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
    lane->mutation.pitchRange = 12;
    lane->mutation.scaleMask = 1 << 4;
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
    lane->mutation.scaleMask = 0xFFF;
    int kept = 0;
    for(int i = 0; i < 200; ++i) { playAt(0.5); kept += std::lround(pitches.get()) == 60; }
    assert(kept > 60 && kept < 140);
    // Offset only: the note starts a grid step early or late.
    lane = model.getLane(t, c, l);
    lane->pianoNotes = {{1, 1, 60, 0.8f, 1, 1.0f}};
    lane->mutation.pitchStrength = 0.0f; lane->mutation.offsetStrength = 1.0f; lane->mutation.offsetSteps = 1; lane->beatsPerStep = 0.5;
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
    lane->mutation.offsetStrength = 0.0f;
    for(int i = 0; i < 10; ++i) { playAt(1.25); near(gates, 1); near(pitches, 60); }
    assert(!model.pianoRepeatNotes(t, c, l, 0, preview));
    // Saved with the note and the lane; older presets load without mutation.
    lane = model.getLane(t, c, l);
    lane->mutation.pitchStrength = 0.75f; lane->mutation.scaleMask = 0x0A5; lane->mutation.pitchRange = 5;
    lane->mutation.lengthStrength = 0.4f; lane->mutation.lengthMax = 3.0f;
    const auto saved = model.toJson();
    model.fromJson(saved);
    lane = model.getLane(t, c, l);
    near(lane->pianoNotes[0].mutation, 1.0);
    assert(std::abs(lane->mutation.pitchStrength - 0.75f) < 1e-6f && lane->mutation.scaleMask == 0x0A5 && lane->mutation.pitchRange == 5);
    assert(std::abs(lane->mutation.lengthStrength - 0.4f) < 1e-6f && std::abs(lane->mutation.lengthMax - 3.0f) < 1e-6f && lane->mutation.offsetStrength == 0.0f);
    auto legacy = saved;
    for(auto& track : legacy["tracks"]) for(auto& clip : track["clips"]) for(auto& laneJson : clip["lanes"]) {
        laneJson.erase("mutation");
        for(auto& note : laneJson["pianoNotes"]) note.erase("mutation");
    }
    model.fromJson(legacy);
    lane = model.getLane(t, c, l);
    near(lane->pianoNotes[0].mutation, 0.0);
    assert(lane->mutation.pitchStrength == 1.0f && lane->mutation.scaleMask == 0xFFF && lane->mutation.lengthStrength == 0.0f);
    // Monophonic: lengthened notes stop at the next note; a note shifted onto
    // another's start keeps its written timing. Off, they may overlap.
    {
        lane = model.getLane(t, c, l);
        const auto savedRules = lane->mutation;
        lane->mutation = ofxOceanodeTimelineMutationRules();
        lane->mutation.pitchStrength = 0.0f;
        lane->mutation.lengthStrength = 1.0f; lane->mutation.lengthMax = 4.0f;
        lane->beatsPerStep = 0.25;
        lane->pianoNotes = {{0, 1, 60, 0.8f, 1, 1.0f}, {1, 1, 62, 0.8f, 1, 0.0f}};
        bool overlapped = false;
        for(int i = 0; i < 40; ++i) {
            playAt(0.5);
            assert(model.pianoRepeatNotes(t, c, l, 0, preview));
            overlapped = overlapped || preview[0].startBeat + preview[0].durationBeats > 1.0 + 1e-9;
        }
        assert(overlapped);
        lane = model.getLane(t, c, l);
        lane->mutationMonophonic = true;
        for(int i = 0; i < 40; ++i) {
            playAt(0.5);
            assert(model.pianoRepeatNotes(t, c, l, 0, preview));
            assert(preview[0].startBeat + preview[0].durationBeats <= 1.0 + 1e-9);
        }
        lane = model.getLane(t, c, l);
        lane->mutation.lengthStrength = 0.0f;
        lane->mutation.offsetStrength = 1.0f; lane->mutation.offsetSteps = 1; lane->beatsPerStep = 1.0;
        lane->pianoNotes = {{0, 1, 60, 0.8f, 1, 0.0f}, {1, 1, 62, 0.8f, 1, 1.0f}};
        for(int i = 0; i < 40; ++i) {
            playAt(0.5);
            assert(model.pianoRepeatNotes(t, c, l, 0, preview));
            // Shifted back to 0 would land on the first note: it stays at 1.
            assert(std::abs(preview[1].startBeat - 1.0) < 1e-9 || std::abs(preview[1].startBeat - 2.0) < 1e-9);
            assert(preview[0].startBeat + preview[0].durationBeats <= preview[1].startBeat + 1e-9);
        }
        const auto saved = model.toJson(); model.fromJson(saved);
        assert(model.getLane(t, c, l)->mutationMonophonic);
        lane = model.getLane(t, c, l);
        lane->mutationMonophonic = false; lane->mutation = savedRules;
    }
    // Velocity: moves within +/- range, pitch untouched.
    {
        lane = model.getLane(t, c, l);
        const auto savedRules = lane->mutation;
        lane->mutation = ofxOceanodeTimelineMutationRules();
        lane->mutation.pitchStrength = 0.0f;
        lane->mutation.velocityStrength = 1.0f; lane->mutation.velocityRange = 0.2f;
        lane->pianoNotes = {{0, 3, 60, 0.8f, 1, 1.0f}};
        bool changed = false;
        for(int i = 0; i < 20; ++i) {
            playAt(0.5);
            assert(model.pianoRepeatNotes(t, c, l, 0, preview));
            assert(preview[0].pitch == 60 && preview[0].velocity >= 0.6f - 1e-6f && preview[0].velocity <= 1.0f + 1e-6f);
            changed = changed || std::abs(preview[0].velocity - 0.8f) > 1e-4f;
        }
        assert(changed);
        // Drift: each repeat mutates the previous one (+1 semitone each), up to
        // the roll's highest key; stopping starts again from the written note.
        lane = model.getLane(t, c, l);
        lane->mutation = ofxOceanodeTimelineMutationRules();
        lane->mutation.pitchRange = 1; lane->mutation.pitchDirection = 1;
        lane->pianoLowPitch = 36; lane->pianoHighPitch = 63;
        lane->mutationDrift = true;
        playAt(0.5);
        for(int repeat = 0; repeat < 6; ++repeat) {
            assert(model.pianoRepeatNotes(t, c, l, repeat, preview));
            assert(preview[0].pitch == std::min(63, 61 + repeat));
        }
        owner.transport->pause(); model.update();
        owner.transport->play(); playAt(0.5);
        assert(model.pianoRepeatNotes(t, c, l, 0, preview) && preview[0].pitch == 61);
        lane = model.getLane(t, c, l);
        lane->mutationDrift = false;
        assert(model.pianoRepeatNotes(t, c, l, 4, preview) && preview[0].pitch == 61);
        // Velocity rules, drift and the scale overlay are saved.
        lane->mutationDrift = true;
        lane->mutation.velocityStrength = 0.3f; lane->mutation.velocityRange = 0.15f;
        lane->pianoScaleEnabled = true; lane->pianoScaleMask = 0x5AD; lane->pianoScaleSnap = true;
        const auto saved = model.toJson(); model.fromJson(saved);
        lane = model.getLane(t, c, l);
        assert(lane->mutationDrift && lane->pianoScaleEnabled && lane->pianoScaleMask == 0x5AD && lane->pianoScaleSnap);
        assert(std::abs(lane->mutation.velocityStrength - 0.3f) < 1e-6f && std::abs(lane->mutation.velocityRange - 0.15f) < 1e-6f);
        lane->mutationDrift = false; lane->pianoScaleEnabled = false; lane->pianoScaleSnap = false;
        lane->pianoLowPitch = 36; lane->pianoHighPitch = 84;
        lane->mutation = savedRules;
    }
    // Groups: B uses its own keys and follows A for everything else.
    using Rules = ofxOceanodeTimelineMutationRules;
    lane = model.getLane(t, c, l);
    lane->mutation.pitchRange = 12; lane->mutation.scaleMask = 1 << 4;   // A: onto E
    ofxOceanodeTimelineMutationGroup b; b.rules.scaleMask = 1 << 7; b.overrides = Rules::ScaleMask; // B: onto G
    lane->mutationGroups = {b};
    lane->pianoNotes = {{0, 3, 60, 0.8f, 1, 1.0f, 0}, {0, 3, 48, 0.8f, 1, 1.0f, 1}};
    for(int i = 0; i < 30; ++i) {
        playAt(0.5);
        assert(model.pianoRepeatNotes(t, c, l, 0, preview) && preview.size() == 2);
        assert(preview[0].pitch == 52 || preview[0].pitch == 64);
        assert(preview[1].pitch == 43 || preview[1].pitch == 55);
    }
    {
        const auto resolved = ofxOceanodeTimelineMutationRulesFor(*model.getLane(t, c, l), 1);
        assert(resolved.scaleMask == 1 << 7 && resolved.pitchRange == 12); // range followed from A
        assert(ofxOceanodeTimelineMutationRulesFor(*model.getLane(t, c, l), 3).scaleMask == 1 << 4); // missing: A
    }
    // Direction: up only stays above the written pitch, down only below; a
    // group can override it and it is saved.
    lane = model.getLane(t, c, l);
    lane->mutation.scaleMask = 0xFFF;
    lane->mutation.pitchDirection = 1;
    for(int i = 0; i < 30; ++i) {
        playAt(0.5);
        assert(model.pianoRepeatNotes(t, c, l, 0, preview));
        assert(preview[0].pitch > 60 && preview[0].pitch <= 72);
    }
    lane = model.getLane(t, c, l);
    lane->mutationGroups[0].rules.pitchDirection = 2;
    lane->mutationGroups[0].overrides |= Rules::PitchDirection;
    lane->mutationGroups[0].rules.scaleMask = 0xFFF;
    for(int i = 0; i < 30; ++i) {
        playAt(0.5);
        assert(model.pianoRepeatNotes(t, c, l, 0, preview));
        assert(preview[0].pitch > 60 && preview[1].pitch < 48 && preview[1].pitch >= 36);
    }
    {
        const auto saved = model.toJson();
        model.fromJson(saved);
        lane = model.getLane(t, c, l);
        assert(lane->mutation.pitchDirection == 1 && lane->mutationGroups[0].rules.pitchDirection == 2);
        assert(lane->mutationGroups[0].overrides & Rules::PitchDirection);
        lane->mutation.pitchDirection = 0;
        lane->mutationGroups[0].rules.scaleMask = 1 << 7;
        lane->mutationGroups[0].overrides = Rules::ScaleMask;
        lane->mutation.scaleMask = 1 << 4;
    }
    // Groups and each note's group are saved.
    {
        const auto saved = model.toJson();
        model.fromJson(saved);
        lane = model.getLane(t, c, l);
        assert(lane->mutationGroups.size() == 1 && lane->mutationGroups[0].overrides == Rules::ScaleMask);
        assert(lane->mutationGroups[0].rules.scaleMask == 1 << 7);
        assert(lane->pianoNotes[0].mutationGroup == 0 && lane->pianoNotes[1].mutationGroup == 1);
    }
    // B follows A's pitch strength: off in A, off in B.
    lane = model.getLane(t, c, l);
    lane->mutation.pitchStrength = 0.0f;
    playAt(0.5);
    assert(!model.pianoRepeatNotes(t, c, l, 0, preview));
    // Deleting a group sends its notes back to A.
    ofxOceanodeTimelineRemoveMutationGroup(*model.getLane(t, c, l), 1);
    assert(model.getLane(t, c, l)->mutationGroups.empty() && model.getLane(t, c, l)->pianoNotes[1].mutationGroup == 0);
    std::cout << "Piano mutation groups passed: own keys, follow A, fallback, persistence, delete.\n";
    std::cout << "Piano mutation passed: amount, pitch scale, re-rolls per start, offset, inactive rules, preview and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
