#!/usr/bin/env python3
"""Regress the per-frame fast paths: exact automation values, no redundant
parameter notifications, rebuild-on-change indexes and the tempo-map inverse."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
#include "Controls/Editors/ofxOceanodeTimelineEditorView.h"
int main() {
    ofSeedRandom();
    int failures = 0;
    {
        // Values reach their parameter exactly: no 6-digit rounding.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        ofParameter<float> position("Position", 0, 0, 4000000), fine("Fine", 0, 0, 1);
        ofParameter<std::vector<float>> levels("Levels", {0, 0}, {0}, {1});
        ofxOceanodeParameter<float> positionHandle, fineHandle;
        ofxOceanodeParameter<std::vector<float>> levelsHandle;
        positionHandle.bindParameter(position); fineHandle.bindParameter(fine); levelsHandle.bindParameter(levels);
        for(ofxOceanodeAbstractParameter* p : std::initializer_list<ofxOceanodeAbstractParameter*>{&positionHandle, &fineHandle, &levelsHandle})
            owner.targets[p->getEscapedName()] = p;
        const auto track = model.createTrack("Values");
        const auto pb = model.addBinding(track, positionHandle), fb = model.addBinding(track, fineHandle),
                   lb = model.addBinding(track, levelsHandle);
        const auto clip = model.createClip(track, "Values", 0, 4);
        const auto numbers = model.createLane(track, clip, "Position", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(track, clip, numbers, pb);
        model.setClipNumber(track, clip, numbers, 0.0, 2000003.0f);
        const auto fineLane = model.createLane(track, clip, "Fine", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(track, clip, fineLane, fb);
        model.setClipNumber(track, clip, fineLane, 0.0, 0.1234567f);
        const auto rows = model.createLane(track, clip, "Levels", ofxOceanodeTimelineLaneType::MultiValue);
        model.addBindingToLane(track, clip, rows, lb);
        auto* rowLane = model.getLane(track, clip, rows);
        rowLane->multiRowCount = 2;
        rowLane->multiValueRows = {{{0.0, 4.0, 0.3333333f}}, {{0.0, 4.0, 0.7071068f}}};
        // Count notifications while the values hold.
        int positionSets = 0, levelSets = 0;
        auto positionListener = position.newListener([&](float&) { ++positionSets; });
        auto levelListener = levels.newListener([&](std::vector<float>&) { ++levelSets; });
        owner.transport->setDriverMode(TransportDriverMode::External); // frozen
        owner.transport->seekToBeat(1.0); owner.transport->play();
        for(int frame = 0; frame < 10; ++frame) { model.update(); model.applyAutomation(); }
        if(position.get() != 2000003.0f) { std::cerr << "Position arrived as " << position.get() << "\n"; ++failures; }
        if(fine.get() != 0.1234567f) { std::cerr << "Fine arrived as " << fine.get() << "\n"; ++failures; }
        if(levels.get() != std::vector<float>({0.3333333f, 0.7071068f})) { std::cerr << "Levels lost precision\n"; ++failures; }
        if(positionSets != 1 || levelSets != 1) {
            std::cerr << "A held value was set again: position " << positionSets << ", levels " << levelSets << " times\n";
            ++failures;
        }
    }
    {
        // Notes written in place (as the editors do) are picked up on the
        // next update without an explicit invalidation.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        ofParameter<float> pv("Pitch", 48, 0, 127), gv("Gate", 0, 0, 1);
        ofxOceanodeParameter<float> pitch, gate;
        pitch.bindParameter(pv); gate.bindParameter(gv);
        owner.targets[pitch.getEscapedName()] = &pitch; owner.targets[gate.getEscapedName()] = &gate;
        const auto track = model.createTrack("Synth");
        const auto pb = model.addBinding(track, pitch), gb = model.addBinding(track, gate);
        const auto group = model.createNoteGroup(track, "Notes", pb, gb, "");
        std::string lane;
        const auto clip = model.createNoteGroupClip(track, group, "Phrase", 0, 8, &lane);
        model.getLane(track, clip, lane)->pianoNotes = {{0.0, 1.0, 60, 0.8f, 1}};
        owner.transport->setDriverMode(TransportDriverMode::External);
        owner.transport->seekToBeat(2.5); owner.transport->play();
        model.update();
        assert(gv.get() == 0.0f);
        model.getLane(track, clip, lane)->pianoNotes.push_back({2.0, 1.0, 67, 0.8f, 1});
        model.update();
        if(gv.get() != 1.0f || pv.get() != 67.0f) {
            std::cerr << "A note added in place was not picked up (gate " << gv.get() << ", pitch " << pv.get() << ")\n";
            ++failures;
        }
        model.getLane(track, clip, lane)->pianoNotes.back().pitch = 72;
        model.update();
        if(pv.get() != 72.0f) { std::cerr << "A pitch edited in place was not picked up\n"; ++failures; }
    }
    {
        // The tempo map's inverse agrees with it on every interpolation, and
        // the editor view converts through it.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setBpmAutomationEnabled(true);
        model.getBpmAutomationPoints() = {{2, 90}, {16, 150}, {32, 96}, {48, 132}, {48, 70}, {64, 120}};
        model.getBpmCurveTensions().assign(5, {0.3f, 2.5f});
        for(const char* interpolation : {"Step", "Linear", "Log / Exp", "Sigmoid"}) {
            model.setBpmInterpolation(interpolation);
            double worst = 0.0;
            for(double beat = 0.0; beat <= 80.0; beat += 0.37)
                worst = std::max(worst, std::abs(model.secondsToBeat(model.beatToSeconds(beat, 120), 120) - beat));
            if(worst > 1e-6) { std::cerr << interpolation << " inverse is off by " << worst << " beats\n"; ++failures; }
            ofxOceanodeTimelineEditorView view(model, 120.0f, 80.0, false, 0.0);
            for(double beat : {0.0, 3.5, 17.25, 47.9, 50.0, 79.0}) {
                const double back = view.pixelsToBeat(view.beatToPixels(beat));
                if(std::abs(back - beat) > 0.01) { std::cerr << interpolation << " pixels round trip " << beat << " -> " << back << "\n"; ++failures; }
            }
        }
        // An edit in place (as the tempo editor does) refreshes the map.
        const double before = model.beatToSeconds(40.0, 120);
        model.getBpmAutomationPoints()[2].value = 200;
        if(std::abs(model.beatToSeconds(40.0, 120) - before) < 1e-6) { std::cerr << "Tempo edit in place was ignored\n"; ++failures; }
    }
    if(failures > 0) return 1;
    std::cout << "PASS: exact automation values; held values notify once; in-place note edits picked up; "
                 "tempo inverse exact on every interpolation.\n";
}
'''
fixture.main(cases=cases, extra_includes=[base/'src'])
