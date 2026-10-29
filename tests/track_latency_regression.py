#!/usr/bin/env python3
"""Regress the per-track latency correction: a track's offset moves when its
scheduled events are due (later or earlier, even beyond the lookahead), the
playhead bookkeeping stays on musical time (no repairs while playing), and the
offset is saved, loaded and clamped."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
#include <thread>
struct Sent { char role; ofxOceanodeScheduledParameterEvent event; uint64_t handedUs; };

struct Instrument {
    ofParameter<float> pv, gv, vv;
    ofxOceanodeParameter<float> pitch, gate, velocity;
    std::vector<Sent> sent;
    std::string track, clip, lane;
    Instrument(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& model, const std::string& name)
    : pv(name + " Pitch", 48, 0, 127), gv(name + " Gate", 0, 0, 1), vv(name + " Velocity", 0, 0, 1) {
        pitch.bindParameter(pv); gate.bindParameter(gv); velocity.bindParameter(vv);
        for(auto* p : {&pitch, &gate, &velocity}) owner.targets[p->getEscapedName()] = p;
        track = model.createTrack(name);
        const auto pb = model.addBinding(track, pitch), gb = model.addBinding(track, gate), vb = model.addBinding(track, velocity);
        const auto group = model.createNoteGroup(track, "Notes", pb, gb, vb);
        clip = model.createNoteGroupClip(track, group, "Phrase", 0, 4, &lane);
        model.getLane(track, clip, lane)->pianoNotes = {{1.0, 0.25, 60, 0.8f, 1}}; // 500 ms in at 120 BPM
        for(auto entry : {std::make_pair(&pitch,'p'), std::make_pair(&velocity,'v'), std::make_pair(&gate,'g')})
            ofxOceanodeScheduling::registerParameterTarget(entry.first, &model,
                [this, role = entry.second](const auto& e) {
                    sent.push_back({role, e, ofxOceanodeScheduling::steadyNowUs()}); return true; });
    }
    const Sent* gateOn() const {
        for(const auto& s : sent) if(s.role == 'g' && !s.event.isCorrection && ofToFloat(s.event.value) > 0) return &s;
        return nullptr;
    }
    bool corrected() const {
        return std::any_of(sent.begin(), sent.end(), [](const Sent& s) { return s.event.isCorrection; });
    }
};

int main() {
    ofSeedRandom();
    int failures = 0;
    auto run = [&](float offsetMs, double lookaheadMs, const char* what) {
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(lookaheadMs);
        Instrument plain(owner, model, "Plain"), shifted(owner, model, "Shifted");
        model.setTrackLatencyOffset(shifted.track, offsetMs);
        owner.transport->seekToBeat(0); owner.transport->play();
        // The fixture's playhead does not advance by itself: everything is
        // scheduled from beat 0, and these frames let the events fall due so
        // promotion and the repair checks run on them.
        for(int i = 0; i < 45; ++i) { model.update(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
        const Sent* a = plain.gateOn();
        const Sent* b = shifted.gateOn();
        if(a == nullptr || b == nullptr) { std::cerr << what << ": a gate was never scheduled\n"; ++failures; }
        else {
            const int64_t diffUs = int64_t(b->event.dueSteadyTimeUs) - int64_t(a->event.dueSteadyTimeUs);
            if(std::llabs(diffUs - int64_t(offsetMs * 1000)) > 2) {
                std::cerr << what << ": shifted track due " << diffUs << " us from the plain one, wanted "
                          << offsetMs * 1000 << "\n"; ++failures;
            }
            if(b->event.dueSteadyTimeUs + 1000 < b->handedUs) {
                std::cerr << what << ": early event handed over " << (b->handedUs - b->event.dueSteadyTimeUs)
                          << " us after it was due\n"; ++failures;
            }
        }
        if(plain.corrected() || shifted.corrected()) {
            std::cerr << what << ": the offset made the scheduler repair its own events\n"; ++failures;
        }
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    };
    // Window reaching the note: only the due times move.
    run(+20.0f, 700, "+20 ms");
    run(-20.0f, 700, "-20 ms");
    run(+200.0f, 700, "+200 ms");
    // The note (500 ms ahead) is beyond a 350 ms window: only the window's
    // extension by the early offset reaches it, and it must be handed over
    // before it is due.
    run(-200.0f, 350, "-200 ms beyond a 350 ms window");
    {
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        const auto track = model.createTrack("Saved");
        assert(model.setTrackLatencyOffset(track, -12.5f));
        assert(!model.setTrackLatencyOffset(track, -12.5f));
        ofxOceanodeTimelineManager copy(&owner);
        copy.fromJson(model.toJson());
        if(copy.getTracks().empty() || copy.getTracks().front().latencyOffsetMs != -12.5f) {
            std::cerr << "Latency offset not saved/loaded\n"; ++failures;
        }
        model.setTrackLatencyOffset(track, 9999.0f);
        if(model.getTracks().front().latencyOffsetMs != kMaxTrackLatencyOffsetMs) {
            std::cerr << "Latency offset not clamped\n"; ++failures;
        }
        auto json = model.toJson();
        json["tracks"][0]["latencyOffsetMs"] = -9999.0f;
        ofxOceanodeTimelineManager clamped(&owner);
        clamped.fromJson(json);
        if(clamped.getTracks().front().latencyOffsetMs != -kMaxTrackLatencyOffsetMs) {
            std::cerr << "Loaded latency offset not clamped\n"; ++failures;
        }
    }
    if(failures > 0) return 1;
    std::cout << "PASS: track latency offsets move scheduled due times exactly (+/-, also beyond the window), "
                 "cause no repairs, and are saved, loaded and clamped.\n";
}
'''
fixture.main(cases=cases)
