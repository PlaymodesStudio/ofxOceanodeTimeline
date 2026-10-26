#!/usr/bin/env python3
"""Regress gate release after undo/load/bypass, per-repeat step probability,
failed-probability notes, duplicated piano lanes and non-finite values on load."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
struct Synth {
    ofParameter<float> pv{"Pitch", 48, 0, 127}, gv{"Gate", 0, 0, 1};
    ofxOceanodeParameter<float> pitch, gate;
    std::string track, pb, gb, group;
    Synth(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& model) {
        pitch.bindParameter(pv); gate.bindParameter(gv);
        owner.targets[pitch.getEscapedName()] = &pitch; owner.targets[gate.getEscapedName()] = &gate;
        track = model.createTrack("Synth");
        pb = model.addBinding(track, pitch); gb = model.addBinding(track, gate);
        group = model.createNoteGroup(track, "Notes", pb, gb, "");
    }
};

int main() {
    ofSeedRandom();
    int failures = 0;
    auto check = [&](bool ok, const std::string& message) { if(!ok) { std::cerr << message << "\n"; ++failures; } };
    {
        // A gate held open by a clip is released when an undo removes the clip.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Synth synth(owner, model);
        model.beginTimelineEdit();
        std::string lane;
        const auto clip = model.createNoteGroupClip(synth.track, synth.group, "Phrase", 0, 4, &lane);
        model.getLane(synth.track, clip, lane)->pianoNotes = {{0, 4, 60, 0.8f, 1}};
        model.endTimelineEdit();
        owner.transport->setDriverMode(TransportDriverMode::External);
        owner.transport->seekToBeat(1.0); owner.transport->play();
        model.update();
        assert(synth.gv.get() == 1.0f);
        assert(model.undoTimelineEdit() && model.getClip(synth.track, clip) == nullptr);
        model.update(); model.update();
        check(synth.gv.get() == 0.0f, "Gate stayed open after undoing its clip");
    }
    {
        // ... and when a loaded timeline no longer has it.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Synth synth(owner, model);
        const auto empty = model.toJson();
        std::string lane;
        const auto clip = model.createNoteGroupClip(synth.track, synth.group, "Phrase", 0, 4, &lane);
        model.getLane(synth.track, clip, lane)->pianoNotes = {{0, 4, 60, 0.8f, 1}};
        owner.transport->setDriverMode(TransportDriverMode::External);
        owner.transport->seekToBeat(1.0); owner.transport->play();
        model.update();
        assert(synth.gv.get() == 1.0f);
        model.fromJson(empty);
        model.update(); model.update();
        check(synth.gv.get() == 0.0f, "Gate stayed open after loading a timeline without its clip");
    }
    {
        // ... and when its binding is bypassed mid-note.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Synth synth(owner, model);
        std::string lane;
        const auto clip = model.createNoteGroupClip(synth.track, synth.group, "Phrase", 0, 4, &lane);
        model.getLane(synth.track, clip, lane)->pianoNotes = {{0, 4, 60, 0.8f, 1}};
        owner.transport->setDriverMode(TransportDriverMode::External);
        owner.transport->seekToBeat(1.0); owner.transport->play();
        model.update();
        assert(synth.gv.get() == 1.0f);
        model.getBinding(synth.track, synth.gb)->bypass = true;
        model.update(); model.update();
        check(synth.gv.get() == 0.0f, "Gate stayed open after bypassing it");
        // Bypassed, it is left alone afterwards (live input may drive it).
        synth.gv = 1.0f;
        model.update();
        check(synth.gv.get() == 1.0f, "A bypassed gate kept being written");
    }
    {
        // A step's probability is rolled again in every repeat of its clip.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        ofParameter<float> gv("Step Gate", 0, 0, 1);
        ofxOceanodeParameter<float> gate; gate.bindParameter(gv);
        owner.targets[gate.getEscapedName()] = &gate;
        const auto track = model.createTrack("Steps");
        const auto binding = model.addBinding(track, gate);
        const auto clip = model.createClip(track, "Pattern", 0, 64);
        auto* clipData = model.getClip(track, clip);
        clipData->contentDurationBeats = 4; clipData->repeatContent = true;
        const auto lane = model.createLane(track, clip, "Gate", ofxOceanodeTimelineLaneType::Step);
        model.addBindingToLane(track, clip, lane, binding);
        model.setClipStep(track, clip, lane, 0.0, "1", 0.25);
        model.getLane(track, clip, lane)->step.steps[0].probability = 0.5f;
        int fired = 0;
        for(int repeat = 0; repeat < 16; ++repeat) {
            const auto inspection = model.inspectTarget(gate.getEscapedName(), repeat * 4 + 0.1, true);
            if(inspection.writesValue && inspection.value == "1") ++fired;
        }
        check(fired > 0 && fired < 16, "A 50% step rolled the same in all 16 repeats (" + ofToString(fired) + " fired)");
    }
    {
        // A note that does not sound neither steals the voice nor moves the pitch.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        Synth synth(owner, model);
        model.setNoteGroupVoices(synth.track, synth.group, 1);
        std::string lane;
        const auto clip = model.createNoteGroupClip(synth.track, synth.group, "Phrase", 0, 4, &lane);
        model.getLane(synth.track, clip, lane)->pianoNotes = {{0, 2, 60, 0.8f, 1}, {1, 1, 64, 0.8f, 0.0f}};
        auto gate = model.inspectTarget(synth.gate.getEscapedName(), 1.5, true);
        auto pitch = model.inspectTarget(synth.pitch.getEscapedName(), 1.5, true);
        check(gate.value == "1" && pitch.value == "60",
              "A never-playing note cut the held one: gate " + gate.value + ", pitch " + pitch.value);
        // Chance notes: whichever way the roll goes, the voice keeps sounding.
        auto* clipData = model.getClip(synth.track, clip);
        clipData->durationBeats = 64; clipData->contentDurationBeats = 4; clipData->repeatContent = true;
        model.getLane(synth.track, clip, lane)->pianoNotes[1].probability = 0.5f;
        int held = 0, played = 0;
        for(int repeat = 0; repeat < 16; ++repeat) {
            gate = model.inspectTarget(synth.gate.getEscapedName(), repeat * 4 + 1.5, true);
            pitch = model.inspectTarget(synth.pitch.getEscapedName(), repeat * 4 + 1.5, true);
            if(gate.value != "1") { check(false, "Repeat " + ofToString(repeat) + ": gate closed by a note that failed its roll"); break; }
            if(pitch.value == "60") ++held; else if(pitch.value == "64") ++played;
        }
        check(held > 0 && played > 0, "Expected both outcomes of a 50% note over 16 repeats");
    }
    {
        // A duplicated piano lane does not double its notes, and rolls on its own seed.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        ofParameter<std::vector<float>> pv("Poly Pitch", {60, 60, 60, 60}, {0}, {127}), gv("Poly Gate", {0, 0, 0, 0}, {0}, {1});
        ofxOceanodeParameter<std::vector<float>> pitch, gate;
        pitch.bindParameter(pv); gate.bindParameter(gv);
        owner.targets[pitch.getEscapedName()] = &pitch; owner.targets[gate.getEscapedName()] = &gate;
        const auto track = model.createTrack("Poly");
        const auto pb = model.addBinding(track, pitch), gb = model.addBinding(track, gate);
        const auto group = model.createNoteGroup(track, "Notes", pb, gb, "");
        model.setNoteGroupVoices(track, group, 4);
        std::string lane;
        const auto clip = model.createNoteGroupClip(track, group, "Phrase", 0, 4, &lane);
        model.getLane(track, clip, lane)->pianoNotes = {{0, 1, 60, 0.8f, 1}, {0, 1, 67, 0.8f, 1}};
        const auto copy = model.duplicateLane(track, clip, lane);
        check(model.getLane(track, clip, copy)->probabilitySeed != model.getLane(track, clip, lane)->probabilitySeed,
              "A duplicated lane kept its source's probability seed");
        const auto inspection = model.inspectTarget(gate.getEscapedName(), 0.5, true);
        int open = 0;
        for(const auto& token : ofSplitString(inspection.value, ",", true, true)) if(ofToFloat(token) > 0) ++open;
        check(open == 2, "A duplicated lane doubled its notes: " + ofToString(open) + " voices open for 2 notes");
        // Edited apart, both parts play.
        model.getLane(track, clip, copy)->pianoNotes = {{0, 1, 72, 0.8f, 1}};
        const auto edited = model.inspectTarget(gate.getEscapedName(), 0.5, true);
        open = 0;
        for(const auto& token : ofSplitString(edited.value, ",", true, true)) if(ofToFloat(token) > 0) ++open;
        check(open == 3, "Edited lanes should sound all 3 notes, got " + ofToString(open));
    }
    {
        // A non-finite value (saved as null) does not drop its track.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        ofParameter<float> value("Value", 0, 0, 10);
        ofxOceanodeParameter<float> target; target.bindParameter(value);
        owner.targets[target.getEscapedName()] = &target;
        const auto track = model.createTrack("Numbers");
        const auto binding = model.addBinding(track, target);
        const auto clip = model.createClip(track, "Numbers", 0, 4);
        const auto lane = model.createLane(track, clip, "Value", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(track, clip, lane, binding);
        model.setClipNumber(track, clip, lane, 0.0, 3.0f);
        model.setClipNumber(track, clip, lane, 2.0, 5.0f);
        model.getLane(track, clip, lane)->curvePoints[1].value = std::numeric_limits<float>::quiet_NaN();
        auto saved = ofJson::parse(model.toJson().dump()); // NaN is written as null
        ofxOceanodeTimelineManager loaded(&owner);
        loaded.fromJson(saved);
        check(loaded.getTracks().size() == 1 && loaded.getLane(track, clip, lane) != nullptr,
              "A null value dropped its whole track on load");
        if(const auto* loadedLane = loaded.getLane(track, clip, lane))
            check(loadedLane->curvePoints.size() == 2 && loadedLane->curvePoints[0].value == 3.0f &&
                  std::isfinite(loadedLane->curvePoints[1].value), "The track's other values did not survive");
    }
    if(failures > 0) return 1;
    std::cout << "PASS: gates released after undo, load and bypass; steps re-roll per repeat; failed notes "
                 "keep off voices; duplicated lanes do not double; null values load.\n";
}
'''
fixture.main(cases=cases)
