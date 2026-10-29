#!/usr/bin/env python3
"""A clip's content offset: the content plays rotated, from the offset on.

Covers the clip-time conversion, playback, the change beats the scheduler
walks across the wrap, edits that re-lay content (they bake the offset in
and must sound the same), persistence, and clips that refuse an offset.
"""
import timeline_regression as fixture

# Private access only to read the scheduler's change beats directly.
CASES = '#define private public\n' + fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    namespace CT = ofxOceanodeTimelineClipTime;

    // ---- clip time: 4-beat content starting at its beat 1 -----------------
    ofxOceanodeTimelineClip clip;
    clip.startBeat = 0; clip.durationBeats = 8; clip.contentDurationBeats = 4; clip.contentStretch = 1;
    clip.contentOffsetBeats = 1;
    near(CT::timelineToSourceBeat(clip, 0.0), 1.0);
    near(CT::timelineToSourceBeat(clip, 2.5), 3.5);
    near(CT::timelineToSourceBeat(clip, 3.0), 0.0); // the wrap
    near(CT::timelineToSourceBeat(clip, 3.5), 0.5);
    near(CT::timelineToSourceBeat(clip, 4.0), 1.0); // next repeat starts at the offset again
    near(CT::sourceToTimelineBeat(clip, 1.0, 0), 0.0);
    near(CT::sourceToTimelineBeat(clip, 0.0, 0), 3.0);
    near(CT::sourceToTimelineBeat(clip, 4.0, 0), 3.0); // content end is the wrap too
    near(CT::sourceToTimelineBeat(clip, 0.5, 1), 7.5);
    clip.contentOffsetBeats = 5; near(CT::contentOffset(clip), 1.0); // kept inside the content
    clip.contentOffsetBeats = -1; near(CT::contentOffset(clip), 0.0);
    clip.contentOffsetBeats = 0;
    near(CT::timelineToSourceBeat(clip, 5.5), 1.5);

    // ---- playback: a gate on for content [0, 1) ----------------------------
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> gateValue("Gate", 0, 0, 1), levelValue("Level", 0, 0, 1);
    ofxOceanodeParameter<float> gate, level;
    gate.bindParameter(gateValue); level.bindParameter(levelValue);
    for(auto* p : {&gate, &level}) owner.targets[p->getEscapedName()] = p;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    const auto t = model.createTrack("Gates");
    const auto b = model.addBinding(t, gate);
    const auto c = model.createClip(t, "Gate", 0, 8);
    model.setClipContentDuration(t, c, 4, true);
    const auto l = model.createLane(t, c, "Gate", ofxOceanodeTimelineLaneType::MultiGate);
    model.addBindingToLane(t, c, l, b);
    auto* lane = model.getLane(t, c, l);
    lane->multiRowCount = 1; lane->multiGateRows = {{{0, 1, 1}}};
    owner.transport->play();
    auto gateAt = [&](double beat) {
        owner.transport->seekToBeat(beat); model.update(); model.applyAutomation();
        return gateValue.get();
    };
    near(gateAt(0.5), 1); near(gateAt(3.5), 0);
    assert(model.setClipContentOffset(t, c, 1));
    assert(!model.setClipContentOffset(t, c, 5)); // the same offset, kept inside the content
    near(model.getClip(t, c)->contentOffsetBeats, 1);
    near(gateAt(0.5), 0); near(gateAt(3.5), 1); near(gateAt(7.5), 1); near(gateAt(4.5), 0);

    // ---- change beats across the wrap -------------------------------------
    // The scheduler walks only scheduled paths: mark this one as such.
    model.fastRetriggerPaths.insert(gate.getEscapedName());
    std::vector<double> beats;
    model.collectChangeBeats(model.playContextFor(owner.getTransportState()), 0.0, 8.0, true, beats);
    auto has = [&](double beat) {
        return std::any_of(beats.begin(), beats.end(), [&](double b) { return std::abs(b - beat) < 1e-6; });
    };
    assert(has(3.0) && has(4.0) && has(7.0) && has(8.0)); // gate on at the wrap, off at each repeat's start

    // ---- re-laying content keeps the sound --------------------------------
    std::vector<float> before;
    for(double beat = 0.25; beat < 8; beat += 0.5) before.push_back(gateAt(beat));
    auto expectSame = [&](const char* what) {
        size_t i = 0;
        for(double beat = 0.25; beat < 8; beat += 0.5, ++i)
            if(std::abs(gateAt(beat) - before[i]) > 1e-5) {
                std::cerr << what << ": beat " << beat << " changed\n"; std::abort();
            }
    };
    const auto right = model.splitClip(t, c, 2.0);
    assert(!right.empty());
    near(model.getClip(t, c)->contentOffsetBeats, 0); // baked into the content
    expectSame("split");
    assert(model.setClipContentOffset(t, right, 2));
    before.clear();
    for(double beat = 0.25; beat < 8; beat += 0.5) before.push_back(gateAt(beat));
    model.setClipContentDuration(t, right, 4, true); // same length: the offset stays
    near(model.getClip(t, right)->contentOffsetBeats, 2);
    expectSame("same content length");
    model.consolidateClipContent(t, right);
    near(model.getClip(t, right)->contentOffsetBeats, 0);
    expectSame("consolidate");

    // ---- persistence and refusals -----------------------------------------
    assert(model.setClipContentOffset(t, c, 0.5));
    auto saved = model.toJson();
    model.fromJson(saved);
    near(model.getClip(t, c)->contentOffsetBeats, 0.5);
    for(auto& track : saved["tracks"]) for(auto& savedClip : track["clips"]) savedClip.erase("contentOffsetBeats");
    model.fromJson(saved);
    near(model.getClip(t, c)->contentOffsetBeats, 0);
    const auto lfoTrack = model.createTrack("LFO");
    const auto lfoBinding = model.addBinding(lfoTrack, level);
    const auto lfo = model.createLfoClip(lfoTrack, lfoBinding, "LFO", 0, 4);
    assert(!lfo.empty() && !model.setClipContentOffset(lfoTrack, lfo, 1));
    const auto waveTrack = model.createWaveTrack("Audio");
    const auto waveClip = model.createClip(waveTrack, "Audio", 0, 4);
    if(!waveClip.empty()) assert(!model.setClipContentOffset(waveTrack, waveClip, 1));
    std::cout << "Content offset passed: clip time, playback, change beats across the wrap, baking, persistence, refusals.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
