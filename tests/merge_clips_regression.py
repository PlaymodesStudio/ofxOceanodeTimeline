#!/usr/bin/env python3
"""Merging two clips of one track: one clip that plays what both played.

Lanes on the same parameters with the same kind of lane join into one lane;
others stay separate lanes. Repeats, stretch and content offsets are laid
out as they played, a gap holds the first clip's last value, and where the
clips overlap the later one wins, as overlapping clips do.
"""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> levelValue("Level",0,0,1), cutoffValue("Cutoff",0,0,1), gateValue("Gate",0,0,1),
                       stepValue("Steps",0,0,1), otherValue("Other",0,0,1);
    ofxOceanodeParameter<float> level, cutoff, gate, steps, other;
    level.bindParameter(levelValue); cutoff.bindParameter(cutoffValue); gate.bindParameter(gateValue);
    steps.bindParameter(stepValue); other.bindParameter(otherValue);
    for(auto* p : {&level,&cutoff,&gate,&steps,&other}) owner.targets[p->getEscapedName()] = p;
    TestTimeline model(&owner); model.setSchedulingEnabled(false);
    const auto t = model.createTrack("Merge");
    const auto lb = model.addBinding(t, level), cb = model.addBinding(t, cutoff), gb = model.addBinding(t, gate);
    const auto sb = model.addBinding(t, steps), ob = model.addBinding(t, other);

    auto lane = [&](const std::string& clip, const char* name, ofxOceanodeTimelineLaneType type, const std::string& binding) {
        const auto id = model.createLane(t, clip, name, type);
        model.addBindingToLane(t, clip, id, binding);
        return model.getLane(t, clip, id);
    };
    // First clip: 0..6, a 2-beat pattern repeating, content rotated by half a beat.
    const auto a = model.createClip(t, "A", 0, 2);
    model.setClipContentDuration(t, a, 2, true);
    model.setClipTiming(t, a, 0, 6);
    auto* aCurve = lane(a, "Cutoff", ofxOceanodeTimelineLaneType::Curve, cb);
    aCurve->curveInterpolation = "Sigmoid";
    aCurve->curvePoints = {{0, 0.1f}, {1.5, 0.9f}, {2, 0.4f}}; aCurve->curveTensions = {{0.3f, 3.0f}, {}};
    auto* aGate = lane(a, "Gate", ofxOceanodeTimelineLaneType::MultiGate, gb);
    aGate->multiRowCount = 1; aGate->multiGateRows = {{{0.25, 0.5, 1}, {1.25, 0.25, 1}}};
    auto* aSteps = lane(a, "Steps", ofxOceanodeTimelineLaneType::Step, sb);
    aSteps->beatsPerStep = 0.5; aSteps->stepCount = 2; aSteps->step.steps = {{0.0, 0.0, "1"}};
    auto* aLevel = lane(a, "Level", ofxOceanodeTimelineLaneType::Number, lb);
    model.setClipNumber(t, a, aLevel->id, 0, 0.25f);
    model.setClipContentOffset(t, a, 0.5);
    // Second clip: 8..12, its 2-beat content stretched x2, plus a lane of its own.
    const auto b = model.createClip(t, "B", 8, 4);
    model.setClipContentDuration(t, b, 2, false);
    auto* bCurve = lane(b, "Cutoff", ofxOceanodeTimelineLaneType::Curve, cb);
    bCurve->curveInterpolation = "Sigmoid";
    bCurve->curvePoints = {{0, 0.8f}, {2, 0.2f}}; bCurve->curveTensions = {{0.6f, 2.0f}};
    auto* bGate = lane(b, "Gate", ofxOceanodeTimelineLaneType::MultiGate, gb);
    bGate->multiRowCount = 1; bGate->multiGateRows = {{{0.5, 0.5, 1}}};
    auto* bSteps = lane(b, "Steps", ofxOceanodeTimelineLaneType::Step, sb);
    bSteps->beatsPerStep = 0.25; bSteps->stepCount = 4; bSteps->step.steps = {{0.25, 0.0, "1"}}; // x2: 0.5-beat cells too
    auto* bLevel = lane(b, "Level", ofxOceanodeTimelineLaneType::Number, lb);
    model.setClipNumber(t, b, bLevel->id, 0, 0.75f);
    auto* bOther = lane(b, "Other", ofxOceanodeTimelineLaneType::Number, ob);
    model.setClipNumber(t, b, bOther->id, 0, 0.5f);

    owner.transport->play();
    auto sample = [&]() {
        std::vector<std::array<float, 5>> values;
        for(double beat = 0.07; beat < 12.0; beat += 0.1) { // never on an edge
            owner.transport->seekToBeat(beat); model.update(); model.applyAutomation();
            values.push_back({levelValue.get(), cutoffValue.get(), gateValue.get(), stepValue.get(), otherValue.get()});
        }
        return values;
    };
    const auto before = sample();
    const auto merged = model.mergeClips(t, b, a); // order does not matter: the earlier clip leads
    assert(merged == a && model.getClip(t, b) == nullptr && model.getTrack(t)->clips.size() == 1);
    const auto* clip = model.getClip(t, a);
    assert(clip->startBeat == 0 && std::abs(clip->durationBeats - 12) < 1e-9 && !clip->repeatContent && clip->contentOffsetBeats == 0);
    assert(clip->lanes.size() == 5); // four joined lanes, and B's own "Other"
    const auto after = sample();
    double beat = 0.07;
    for(size_t i = 0; i < before.size(); ++i, beat += 0.1) {
        // "Other" was B's alone: before B starts it now holds B's value instead of nothing.
        const int compared = beat < 8.0 ? 4 : 5;
        for(int k = 0; k < compared; ++k) {
            // In the gap the curve holds a value: before merging, whatever the last
            // sample inside A read; merged, A's exact value at its end. Both hold.
            // Gates and steps rest in the gap inside the merged clip (before, a
            // block running to A's end left them open).
            if((k == 2 || k == 3) && beat > 6.0 && beat < 8.0) {
                if(after[i][k] != 0.0f) { std::cerr << "gap not at rest at " << beat << "\n"; std::abort(); }
                continue;
            }
            if(k == 1 && beat > 6.0 && beat < 8.0) {
                if(std::abs(after[i][k] - after[i - 1][k]) > 1e-6f && beat > 6.1) { std::cerr << "gap not held at " << beat << "\n"; std::abort(); }
                continue;
            }
            if(std::abs(before[i][k] - after[i][k]) > 0.02f) {
                std::cerr << "beat " << beat << " target " << k << ": " << before[i][k] << " vs " << after[i][k] << "\n";
                std::abort();
            }
        }
    }

    // Overlap: the later clip wins from where it starts.
    const auto c = model.createClip(t, "C", 10, 4);
    auto* cLevel = lane(c, "Level", ofxOceanodeTimelineLaneType::Number, lb);
    model.setClipNumber(t, c, cLevel->id, 0, 0.1f);
    model.mergeClips(t, a, c);
    owner.transport->seekToBeat(9); model.update(); model.applyAutomation(); near(levelValue, 0.75);
    owner.transport->seekToBeat(11); model.update(); model.applyAutomation(); near(levelValue, 0.1);
    owner.transport->seekToBeat(13.5); model.update(); model.applyAutomation(); near(levelValue, 0.1);

    // Refused: LFO clips, a clip with itself, clips of different tracks.
    const auto lfo = [&]{ const auto lfoClipId=model.createClip(t,"LFO",20,4); const auto lfoLaneId=model.createLane(t,lfoClipId,"LFO",ofxOceanodeTimelineLaneType::Lfo); const std::string lfoTarget=lb; if(!lfoTarget.empty()) model.addBindingToLane(t,lfoClipId,lfoLaneId,lfoTarget); return lfoClipId; }();
    assert(model.mergeClips(t, a, lfo).empty() && model.getClip(t, lfo));
    assert(model.mergeClips(t, a, a).empty());
    // Four clips one after the other merge at once: the shared Level number
    // lanes become one lane, the Cutoff curve on only one clip stays its own.
    const auto t4 = model.createTrack("Four");
    const auto l4 = model.addBinding(t4, level), c4 = model.addBinding(t4, cutoff);
    std::vector<std::string> four;
    for(int i = 0; i < 4; ++i) {
        const auto id = model.createClip(t4, "Q" + ofToString(i), i * 2.0, 2);
        const auto laneId = model.createLane(t4, id, "Level", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(t4, id, laneId, l4);
        model.setClipNumber(t4, id, laneId, 0, 0.1f + 0.2f * i);
        four.push_back(id);
    }
    const auto curveId = model.createLane(t4, four[2], "Cutoff", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t4, four[2], curveId, c4);
    model.getLane(t4, four[2], curveId)->curvePoints = {{0, 0.2f}, {2, 0.6f}};
    model.setTrackMuted(t, true);
    auto read4 = [&](double at) { owner.transport->seekToBeat(at); model.update(); model.applyAutomation(); };
    std::vector<std::pair<float, float>> before4;
    for(double at = 0.07; at < 8.0; at += 0.25) { read4(at); before4.push_back({levelValue.get(), cutoffValue.get()}); }
    // Refused as a whole (an LFO among them): nothing changes.
    const auto lfo4 = [&]{ const auto lfoClipId=model.createClip(t4,"LFO",20,4); const auto lfoLaneId=model.createLane(t4,lfoClipId,"LFO",ofxOceanodeTimelineLaneType::Lfo); const std::string lfoTarget=l4; if(!lfoTarget.empty()) model.addBindingToLane(t4,lfoClipId,lfoLaneId,lfoTarget); return lfoClipId; }();
    auto withLfo = four; withLfo.push_back(lfo4);
    assert(model.mergeClips(t4, withLfo).empty() && model.getTrack(t4)->clips.size() == 5);
    model.removeClip(t4, lfo4);
    const auto merged4 = model.mergeClips(t4, {four[3], four[1], four[0], four[2]}); // any order
    assert(merged4 == four[0] && model.getTrack(t4)->clips.size() == 1);
    const auto* clip4 = model.getClip(t4, merged4);
    assert(std::abs(clip4->startBeat) < 1e-9 && std::abs(clip4->durationBeats - 8) < 1e-9 && clip4->lanes.size() == 2);
    size_t i4 = 0;
    for(double at = 0.07; at < 8.0; at += 0.25, ++i4) {
        read4(at);
        near(levelValue, before4[i4].first);
        // The curve is its clip's alone: merged it also holds a value outside it.
        if(at > 4.0 && at < 6.0) near(cutoffValue, before4[i4].second);
    }
    std::cout << "Merge clips passed: joined and separate lanes, repeats, stretch, offset, gap, overlap, refusals.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
