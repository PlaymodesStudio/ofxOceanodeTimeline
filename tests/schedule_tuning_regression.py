#!/usr/bin/env python3
"""Regress schedule invalidation by clip/lane edits, the scheduled retrigger gap
and the adaptive lookahead window."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
struct Sent { char role; ofxOceanodeScheduledParameterEvent event; };

struct Instrument {
    ofParameter<float> pv{"Pitch",48,0,127}, gv{"Gate",0,0,1}, vv{"Velocity",0,0,1};
    ofxOceanodeParameter<float> pitch, gate, velocity;
    std::vector<Sent> sent;
    std::string track, clip, lane;
    Instrument(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& model, double clipLength) {
        pitch.bindParameter(pv); gate.bindParameter(gv); velocity.bindParameter(vv);
        for(auto* p : {&pitch, &gate, &velocity}) owner.targets[p->getEscapedName()] = p;
        track = model.createTrack("Instrument");
        const auto pb = model.addBinding(track, pitch), gb = model.addBinding(track, gate), vb = model.addBinding(track, velocity);
        const auto group = model.createNoteGroup(track, "Notes", pb, gb, vb);
        clip = model.createNoteGroupClip(track, group, "Phrase", 0, clipLength, &lane);
        for(auto entry : {std::make_pair(&pitch,'p'), std::make_pair(&velocity,'v'), std::make_pair(&gate,'g')})
            ofxOceanodeScheduling::registerParameterTarget(entry.first, &model,
                [this, role = entry.second](const auto& e) { sent.push_back({role, e}); return true; });
    }
};

int main() {
    ofSeedRandom();
    int failures = 0;
    {
        // Dragging a clip while its notes are queued corrects them.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(500);
        Instrument synth(owner, model, 4);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes = {{0.2,0.4,60,0.8f,1}};
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update();
        const size_t mark = synth.sent.size();
        assert(mark > 0);
        model.setClipTiming(synth.track, synth.clip, 2.0, 4.0);
        model.update();
        const bool corrected = std::any_of(synth.sent.begin() + mark, synth.sent.end(), [](const Sent& s) {
            return s.role == 'g' && s.event.isCorrection && ofToFloat(s.event.value) == 0;
        });
        if(!corrected) { std::cerr << "A clip moved while playing left its queued note uncorrected\n"; ++failures; }
        // Editing in place changes the playback signature; renaming does not.
        auto* lane = model.getLane(synth.track, synth.clip, synth.lane);
        const auto signature = ofxOceanodeTimelineManager::lanePlaybackSignature(*lane);
        lane->name = "Renamed";
        assert(ofxOceanodeTimelineManager::lanePlaybackSignature(*lane) == signature);
        lane->pianoNotes[0].pitch = 61;
        assert(ofxOceanodeTimelineManager::lanePlaybackSignature(*lane) != signature);
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // Back-to-back notes on a scheduled lane retrigger with a gate-off of
        // a few milliseconds, not the frame path's >= 10 ms.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(500);
        Instrument synth(owner, model, 4);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes = {{0.25,0.5,60,0.8f,1}, {0.75,0.5,60,0.8f,1}};
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update(); model.update();
        std::vector<const Sent*> gates;
        for(const auto& s : synth.sent) if(s.role == 'g' && !s.event.isCorrection) gates.push_back(&s);
        // on, off at the join, on again after the gap
        if(gates.size() < 3 || ofToFloat(gates[1]->event.value) != 0 || ofToFloat(gates[2]->event.value) <= 0) {
            std::cerr << "Expected on/off/on gates, got " << gates.size() << "\n"; ++failures;
        } else {
            const auto gapUs = gates[2]->event.dueSteadyTimeUs - gates[1]->event.dueSteadyTimeUs;
            if(gapUs < 2000 || gapUs > 5000) {
                std::cerr << "Scheduled retrigger gap is " << gapUs << " us\n"; ++failures;
            }
        }
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // The frame path keeps the long gap: just after the join the gate is
        // still off when nothing is scheduled.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Instrument synth(owner, model, 4);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes = {{0.25,0.5,60,0.8f,1}, {0.75,0.5,60,0.8f,1}};
        owner.transport->seekToBeat(0.755); owner.transport->play(); model.update();
        if(synth.gv.get() != 0.0f) { std::cerr << "Frame path lost its retrigger gap\n"; ++failures; }
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // The window follows recent frame intervals, capped by the setting;
        // a fixed window is the setting itself.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        const double adaptive = model.effectiveSchedulingLookaheadMs();
        if(adaptive < 50.0 || adaptive > 120.0) {
            std::cerr << "Adaptive lookahead " << adaptive << " ms\n"; ++failures;
        }
        model.setSchedulingLookaheadMs(40);
        assert(model.effectiveSchedulingLookaheadMs() == 40);
        model.setSchedulingLookaheadMs(500);
        assert(model.effectiveSchedulingLookaheadMs() < 500);
        model.setSchedulingLookaheadAdaptive(false);
        assert(model.effectiveSchedulingLookaheadMs() == 500);
        const auto saved = model.toJson();
        ofxOceanodeTimelineManager copy(&owner);
        copy.fromJson(saved);
        assert(!copy.isSchedulingLookaheadAdaptive() && copy.getSchedulingLookaheadMs() == 500);
    }
    if(failures > 0) return 1;
    std::cout << "PASS: clip moves correct queued notes; lane signature tracks playback only; scheduled "
                 "retrigger gap of a few ms; adaptive lookahead.\n";
}
'''
fixture.main(cases=cases)
