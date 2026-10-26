#!/usr/bin/env python3
"""Regress schedule corrections, frame-stepped transports and piano role scheduling.

Events handed to a backend are replayed in timetag order (as scsynth executes
them) to check what the instrument actually plays after a seek or an edit.
"""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
struct Sent { char role; ofxOceanodeScheduledParameterEvent event; };

// What the server holds for one role at an instant: every event due by then,
// in timetag order (dispatch order among equal timetags).
static std::string playedAt(const std::vector<Sent>& sent, size_t from, char role, uint64_t atUs) {
    std::vector<const Sent*> events;
    for(size_t i = from; i < sent.size(); ++i)
        if(sent[i].role == role && sent[i].event.dueSteadyTimeUs <= atUs) events.push_back(&sent[i]);
    std::stable_sort(events.begin(), events.end(), [](const Sent* a, const Sent* b) {
        return a->event.dueSteadyTimeUs < b->event.dueSteadyTimeUs;
    });
    return events.empty() ? std::string() : events.back()->event.value;
}

static uint64_t firstDue(const std::vector<Sent>& sent, size_t from, char role, const std::string& value) {
    for(size_t i = from; i < sent.size(); ++i)
        if(sent[i].role == role && !sent[i].event.isCorrection && sent[i].event.value == value)
            return sent[i].event.dueSteadyTimeUs;
    return 0;
}

struct Instrument {
    ofParameter<float> pv{"Pitch",48,0,127}, gv{"Gate",0,0,1}, vv{"Velocity",0,0,1};
    ofxOceanodeParameter<float> pitch, gate, velocity;
    std::vector<Sent> sent;
    std::string track, pb, gb, vb, clip, lane;
    Instrument(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& model, double clipLength) {
        pitch.bindParameter(pv); gate.bindParameter(gv); velocity.bindParameter(vv);
        for(auto* p : {&pitch, &gate, &velocity}) owner.targets[p->getEscapedName()] = p;
        track = model.createTrack("Instrument");
        pb = model.addBinding(track, pitch); gb = model.addBinding(track, gate); vb = model.addBinding(track, velocity);
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
        // Seek while events of the old position are queued: the corrections
        // for them must not cut or retune the note scheduled at the new one.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        Instrument synth(owner, model, 8);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes =
            {{0.5,0.25,60,0.8f,1}, {4.1,0.8,72,0.8f,1}};
        model.setSchedulingLookaheadMs(500);
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update(); model.update();
        assert(firstDue(synth.sent, 0, 'g', "1") != 0); // the first note is queued
        const size_t mark = synth.sent.size();
        owner.transport->seekToBeat(4.0);
        model.update();
        const uint64_t on = firstDue(synth.sent, mark, 'g', "1");
        assert(on != 0);
        // 0.8 beats at 120 BPM: the note sounds for 400 ms.
        for(uint64_t at : {on + 1000, on + 150000, on + 300000, on + 390000}) {
            if(playedAt(synth.sent, 0, 'g', at) != "1" || playedAt(synth.sent, 0, 'p', at) != "72") {
                std::cerr << "After a seek, +" << (at - on) / 1000 << " ms into the new note the server plays gate "
                          << playedAt(synth.sent, 0, 'g', at) << " pitch " << playedAt(synth.sent, 0, 'p', at) << "\n";
                ++failures; break;
            }
        }
        if(failures == 0) assert(playedAt(synth.sent, 0, 'g', on + 410000) == "0");
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // An edit while playing is corrected on the next update, with the
        // value due at the correction's own instant.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        Instrument synth(owner, model, 8);
        auto* lane = model.getLane(synth.track, synth.clip, synth.lane);
        lane->pianoNotes = {{0.4,0.2,60,0.8f,1}};
        model.setSchedulingLookaheadMs(500);
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update();
        const size_t mark = synth.sent.size();
        // Replace the queued note by one that starts sooner and lasts longer.
        lane->pianoNotes = {{0.1,0.6,67,0.8f,1}};
        model.invalidateSchedule();
        model.update();
        const uint64_t on = firstDue(synth.sent, mark, 'g', "1");
        assert(on != 0);
        for(uint64_t at : {on + 1000, on + 200000, on + 290000}) {
            // Pitch already held 67 before the note (the clip's first note), so
            // the frame path set it and nothing was scheduled for it.
            const auto pitch = playedAt(synth.sent, 0, 'p', at);
            if(playedAt(synth.sent, 0, 'g', at) != "1" || (pitch != "67" && !(pitch.empty() && synth.pv.get() == 67))) {
                std::cerr << "After an edit, +" << (at - on) / 1000 << " ms into the new note the server plays gate "
                          << playedAt(synth.sent, 0, 'g', at) << " pitch " << playedAt(synth.sent, 0, 'p', at) << "\n";
                ++failures; break;
            }
        }
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // A frame-stepped transport does not follow the clock: no events are
        // handed ahead, the frame path plays the notes.
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofxOceanodeTimelineManager model(&owner);
        Instrument synth(owner, model, 4);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes = {{0.1,0.5,60,0.8f,1}};
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update();
        // To beat 1/3, inside the note.
        for(int frame = 0; frame < 10; ++frame) { owner.transport->advanceFrameStep(1.0 / 60.0); model.update(); }
        if(!synth.sent.empty()) {
            std::cerr << "Frame-stepped playback handed " << synth.sent.size() << " events to the backend\n";
            ++failures;
        }
        else assert(synth.gv.get() == 1.0f && synth.pv.get() == 60.0f);
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // A curve sharing Pitch keeps Pitch on the frame path; Gate must
        // then stay there too, or it would open with the previous pitch.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        Instrument synth(owner, model, 4);
        model.getLane(synth.track, synth.clip, synth.lane)->pianoNotes = {{0.1,0.2,60,0.8f,1}, {0.4,0.2,64,0.8f,1}};
        const auto bend = model.createLane(synth.track, synth.clip, "Bend", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(synth.track, synth.clip, bend, synth.pb);
        model.setSchedulingLookaheadMs(500);
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update(); model.update();
        for(const auto& s : synth.sent) {
            if(s.role == 'g' || s.role == 'v') {
                std::cerr << "Scheduled " << s.role << " although Pitch follows a curve on the frame path\n";
                ++failures; break;
            }
        }
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {
        // A parameter no longer routed to a piano lane loses the pitch lead.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        Instrument synth(owner, model, 4);
        ofParameter<float> qv("Other", 0, 0, 10);
        ofxOceanodeParameter<float> other; other.bindParameter(qv);
        owner.targets[other.getEscapedName()] = &other;
        const auto ob = model.addBinding(synth.track, other);
        std::vector<ofxOceanodeScheduledParameterEvent> otherSent;
        ofxOceanodeScheduling::registerParameterTarget(&other, &model,
            [&](const auto& e) { otherSent.push_back(e); return true; });
        const auto steps = model.createClip(synth.track, "Steps", 0, 4);
        const auto numbers = model.createLane(synth.track, steps, "Values", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(synth.track, steps, numbers, synth.pb);
        model.addBindingToLane(synth.track, steps, numbers, ob);
        model.setClipNumber(synth.track, steps, numbers, 0.0, 1.0f);
        model.setClipNumber(synth.track, steps, numbers, 0.4, 5.0f);
        model.update();
        // Removing a lane rebuilds the indexes without invalidating them.
        model.removeLane(synth.track, synth.clip, synth.lane);
        model.setSchedulingLookaheadMs(500);
        owner.transport->seekToBeat(0); owner.transport->play();
        model.update();
        uint64_t pitchDue = 0, otherDue = 0;
        for(const auto& s : synth.sent) if(s.role == 'p' && s.event.value == "5") pitchDue = s.event.dueSteadyTimeUs;
        for(const auto& e : otherSent) if(e.value == "5") otherDue = e.dueSteadyTimeUs;
        assert(pitchDue != 0 && otherDue != 0);
        if(pitchDue != otherDue) {
            std::cerr << "Former piano pitch still led by " << int64_t(otherDue) - int64_t(pitchDue) << " us\n";
            ++failures;
        }
        owner.transport->pause(); model.update();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    if(failures > 0) return 1;
    std::cout << "PASS: seek/edit corrections carry the value due at their instant; frame-stepped playback "
                 "keeps the frame path; piano roles schedule together; stale pitch lead cleared.\n";
}
'''
fixture.main(cases=cases)
