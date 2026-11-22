#!/usr/bin/env python3
"""Consolidating lays a clip's content out as it plays.

A repeating clip's passes become copies of their own, so each can be
edited without changing the others, and a stretched clip's content is
scaled so the clip plays at 1x. Either way the clip plays the same.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    TestTimeline model(&owner);
    auto near = [](double a, double e) {
        if(std::abs(a - e) > 1e-6) std::cerr << "expected " << e << " got " << a << "\n";
        assert(std::abs(a - e) <= 1e-6);
    };
    const auto t = model.createTrack("Notes");

    // A 4-beat clip with one note, repeated out to 10 beats.
    const auto a = model.createClip(t, "A", 0, 4);
    const auto pianoLane = model.createLane(t, a, "Piano", ofxOceanodeTimelineLaneType::PianoRoll);
    const auto curveLane = model.createLane(t, a, "Curve", ofxOceanodeTimelineLaneType::Curve);
    {
        auto* piano = model.getLane(t, a, pianoLane);
        ofxOceanodeTimelinePianoNote note; note.startBeat = 1; note.durationBeats = 1; note.pitch = 60;
        piano->pianoNotes = {note};
        auto* curve = model.getLane(t, a, curveLane);
        curve->curvePoints = {{0.0, 0.0f}, {4.0, 1.0f}};
        curve->curveTensions.resize(1);
    }
    model.resizeClipEnd(t, a, 10, ofxOceanodeTimelineEndEdit::Repeat);
    assert(model.getClip(t, a)->repeatContent);
    assert(model.consolidateClipContent(t, a));
    const auto* clip = model.getClip(t, a);
    assert(!clip->repeatContent);
    near(clip->durationBeats, 10);
    near(clip->contentDurationBeats, 10);
    near(clip->contentStretch, 1);
    // One note per pass (the third pass shows beats 8-10, the note at 9).
    const auto& notes = model.getLane(t, a, pianoLane)->pianoNotes;
    assert(notes.size() == 3);
    near(notes[0].startBeat, 1); near(notes[1].startBeat, 5); near(notes[2].startBeat, 9);
    // The ramp restarts each pass and stops where the clip ends.
    const auto* curve = model.getLane(t, a, curveLane);
    // (0 -> 1 per pass, the jump back kept: 0:0 4:1 4:0 8:1 8:0 10:0.5)
    assert(curve->curvePoints.size() == 6);
    assert(!curve->curvePoints.empty());
    near(curve->curvePoints.front().beat, 0); near(curve->curvePoints.front().value, 0);
    near(curve->curvePoints.back().beat, 10); near(curve->curvePoints.back().value, 0.5);
    assert(curve->curveTensions.size() + 1 == curve->curvePoints.size());
    // Each pass is editable on its own.
    model.getLane(t, a, pianoLane)->pianoNotes[1].pitch = 72;
    assert(model.getLane(t, a, pianoLane)->pianoNotes[0].pitch == 60);
    assert(model.getLane(t, a, pianoLane)->pianoNotes[2].pitch == 60);
    // Consolidating again changes nothing.
    assert(!model.consolidateClipContent(t, a));

    // A stretched repeating clip becomes a 1x clip: its content scaled.
    const auto s = model.createClip(t, "S", 40, 4);
    const auto sLane = model.createLane(t, s, "Piano", ofxOceanodeTimelineLaneType::PianoRoll);
    const auto sSteps = model.createLane(t, s, "Steps", ofxOceanodeTimelineLaneType::Step);
    {
        ofxOceanodeTimelinePianoNote note; note.startBeat = 1; note.durationBeats = 1;
        model.getLane(t, s, sLane)->pianoNotes = {note};
        auto* steps = model.getLane(t, s, sSteps);
        steps->beatsPerStep = 1; steps->stepCount = 4;
        model.setClipStep(t, s, sSteps, 2, "1", 0.5);
    }
    model.getClip(t, s)->contentStretch = 2; // 4 source beats play over 8
    model.getClip(t, s)->durationBeats = 16;
    assert(model.consolidateClipContent(t, s));
    near(model.getClip(t, s)->contentDurationBeats, 16);
    near(model.getClip(t, s)->contentStretch, 1);
    assert(!model.getClip(t, s)->repeatContent);
    const auto& sNotes = model.getLane(t, s, sLane)->pianoNotes;
    assert(sNotes.size() == 2);
    near(sNotes[0].startBeat, 2); near(sNotes[0].durationBeats, 2);
    near(sNotes[1].startBeat, 10); near(sNotes[1].durationBeats, 2);
    const auto* steps = model.getLane(t, s, sSteps);
    near(steps->beatsPerStep, 2);
    assert(steps->stepCount == 8);
    assert(steps->step.steps.size() == 2);
    near(steps->step.steps[0].startBeat, 4); near(steps->step.steps[0].durationBeats, 1);
    near(steps->step.steps[1].startBeat, 12);

    // A stretched one-shot clip too.
    const auto o = model.createClip(t, "O", 60, 4);
    const auto oLane = model.createLane(t, o, "Piano", ofxOceanodeTimelineLaneType::PianoRoll);
    {
        ofxOceanodeTimelinePianoNote note; note.startBeat = 3; note.durationBeats = 1;
        model.getLane(t, o, oLane)->pianoNotes = {note};
    }
    model.getClip(t, o)->repeatContent = false;
    model.getClip(t, o)->durationBeats = 6; // 4 source beats over 6
    assert(model.consolidateClipContent(t, o));
    near(model.getClip(t, o)->contentDurationBeats, 6);
    near(model.getClip(t, o)->contentStretch, 1);
    near(model.getLane(t, o, oLane)->pianoNotes[0].startBeat, 4.5);
    near(model.getLane(t, o, oLane)->pianoNotes[0].durationBeats, 1.5);
    assert(!model.consolidateClipContent(t, o)); // already 1x

    // A clip with an LFO lane keeps its speed: its stretch stays.
    const auto l = model.createClip(t, "L", 80, 4);
    model.createLane(t, l, "LFO", ofxOceanodeTimelineLaneType::Lfo);
    model.getClip(t, l)->repeatContent = false;
    model.getClip(t, l)->durationBeats = 8;
    model.consolidateClipContent(t, l);
    near(ofxOceanodeTimelineClipTime::effectiveStretch(*model.getClip(t, l)), 2);

    // A one-shot clip whose pattern lanes loop inside it (a 4-cell slider and
    // a 2-beat step pattern over 10 beats): each becomes one pattern as long
    // as the content.
    const auto g = model.createClip(t, "G", 100, 10);
    model.getClip(t, g)->repeatContent = false;
    const auto gSlider = model.createLane(t, g, "Slider", ofxOceanodeTimelineLaneType::MultiSlider);
    const auto gSteps = model.createLane(t, g, "Steps", ofxOceanodeTimelineLaneType::Step);
    {
        auto* slider = model.getLane(t, g, gSlider);
        slider->beatsPerStep = 1; slider->stepCount = 4; slider->multiSliderValues = {1, 2, 3, 4};
        auto* steps = model.getLane(t, g, gSteps);
        steps->beatsPerStep = 1; steps->stepCount = 2;
        model.setClipStep(t, g, gSteps, 0, "1", 0.5);
    }
    assert(model.consolidateClipContent(t, g));
    near(model.getClip(t, g)->contentDurationBeats, 10);
    {
        const auto* slider = model.getLane(t, g, gSlider);
        assert(slider->stepCount == 10);
        const std::vector<float> expected = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2};
        assert(slider->multiSliderValues == expected);
        const auto* steps = model.getLane(t, g, gSteps);
        assert(steps->stepCount == 10 && steps->step.steps.size() == 5);
        for(size_t i = 0; i < 5; ++i) near(steps->step.steps[i].startBeat, 2.0 * i);
    }
    // Each pass is its own now; consolidating again changes nothing.
    model.getLane(t, g, gSlider)->multiSliderValues[5] = 9;
    assert(model.getLane(t, g, gSlider)->multiSliderValues[1] == 2);
    assert(!model.consolidateClipContent(t, g));

    // A repeating clip showing less than one pass: its visible part.
    const auto b = model.createClip(t, "B", 20, 4);
    model.createLane(t, b, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.resizeClipEnd(t, b, 3, ofxOceanodeTimelineEndEdit::Repeat);
    assert(model.consolidateClipContent(t, b));
    assert(!model.getClip(t, b)->repeatContent);
    near(model.getClip(t, b)->contentDurationBeats, 3);
    near(model.getClip(t, b)->durationBeats, 3);
    std::cout << "Consolidate repeat passed: repeats unrolled, passes edited apart, stretch baked to 1x (repeating and one-shot), LFO clips keep their speed, cropped repeat.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
