#!/usr/bin/env python3
"""Several timelines on one canvas, each playing on its own transport.

A further timeline on the root canvas plays free: started, stopped and
seeked by itself at its own tempo while the main timeline is stopped, on the
shared clock. It is listed as a time source, keeps its tempo, loop and meter,
and saves with which timelines play free.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> levelValue("Level", 0, 0, 1), drumValue("Drum", 0, 0, 1);
    ofxOceanodeParameter<float> level; level.bindParameter(levelValue);
    ofxOceanodeParameter<float> drum; drum.bindParameter(drumValue);
    owner.targets[level.getEscapedName()] = &level;
    owner.targets[drum.getEscapedName()] = &drum;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    auto frame = [&](double seconds) {
        owner.transport->advanceFrameStep(seconds);
        owner.transport->latchFrameState();
        model.update();
    };

    // The main timeline drives Level.
    const auto mainTrack = model.createTrack("Main");
    const auto mainBinding = model.addBinding(mainTrack, level);
    const auto mainClip = model.createClip(mainTrack, "Main clip", 0, 4);
    const auto mainLane = model.createLane(mainTrack, mainClip, "Level", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(mainTrack, mainClip, mainLane, mainBinding);
    model.setClipNumber(mainTrack, mainClip, mainLane, 0, 0.25f);

    // A further timeline on the root canvas drives Drum: 0.5, then 0.9 from beat 2.
    const auto id = model.createTimeline("", "Drums");
    assert(!id.empty());
    const ofxOceanodeTimelineManager::TimelineRef drums{"", id};
    auto* pattern = model.timelineFor(drums);
    assert(pattern != nullptr && pattern->timelineId == id && pattern->play.name == "Drums");
    assert(model.timelinesOfCanvas("").size() == 2);
    assert(model.timelineName(drums) == "Drums");
    ofxOceanodeTimelineManager::TimelineRef found;
    assert(model.refOf(*pattern, found) && found == drums);
    const auto t = model.createTrack(*pattern, "Kick");
    const auto b = model.addBinding(*pattern, t, drum);
    const auto first = model.createClip(*pattern, t, "A", 0, 2);
    const auto firstLane = model.createLane(*pattern, t, first, "Drum", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(*pattern, t, first, firstLane, b);
    model.setClipNumber(*pattern, t, first, firstLane, 0, 0.5f);
    const auto second = model.createClip(*pattern, t, "B", 2, 2);
    const auto secondLane = model.createLane(*pattern, t, second, "Drum", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(*pattern, t, second, secondLane, b);
    model.setClipNumber(*pattern, t, second, secondLane, 0, 0.9f);

    // Not free yet: no transport, listed but not selectable.
    assert(!model.isFree(drums) && model.transportFor(drums) == nullptr);
    frame(0.1);
    assert(owner.transport->findTimeSource(ofxOceanodeTimelineManager::timeSourceId(drums)) == nullptr);
    assert(owner.transport->findTimeSource("root#default") == owner.transport);

    // Free: its own transport, at its own tempo, unquantized.
    auto settings = *model.playSettings(drums);
    settings.bpm = 60.0f;
    settings.quantize = 0;
    model.setPlaySettings(drums, settings);
    model.setFree(drums, true);
    frame(0.1);
    auto drumsTransport = model.transportFor(drums);
    assert(drumsTransport != nullptr && drumsTransport != owner.transport && drumsTransport->isDerived());
    for(int i = 0; i < 20; ++i) frame(0.05); // the time-source list refreshes a few times a second
    assert(owner.transport->findTimeSource(ofxOceanodeTimelineManager::timeSourceId(drums)) == drumsTransport);

    // Root stopped, Drums playing: Drum follows Drums' beat only.
    model.playTimeline(drums);
    for(int i = 0; i < 10; ++i) frame(0.1);
    assert(drumsTransport->getState().isPlaying);
    assert(!owner.transport->getState().isPlaying);
    near(owner.transport->getState().beatPosition, 0.0);
    near(drumValue, 0.5f);
    near(levelValue, 0.25f); // the main timeline holds its value at beat 0
    for(int i = 0; i < 20; ++i) frame(0.1); // past beat 2 at 60 BPM
    assert(drumsTransport->getState().beatPosition > 2.0 && drumsTransport->getState().beatPosition < 3.0);
    near(drumValue, 0.9f);
    near(std::abs(drumsTransport->getState().bpm - 60.0f) < 1e-3 ? 1.0 : 0.0, 1.0);

    // Stopped and rewound by itself; the main timeline plays on its own.
    model.stopTimeline(drums, true, false);
    frame(0.1);
    assert(!drumsTransport->getState().isPlaying);
    near(drumsTransport->getState().beatPosition, 0.0);
    near(drumValue, 0.5f);
    model.playTimeline({"", ""});
    for(int i = 0; i < 5; ++i) frame(0.1);
    assert(owner.transport->getState().isPlaying && !drumsTransport->getState().isPlaying);
    model.seekTimeline(drums, 3.0);
    frame(0.1);
    near(drumValue, 0.9f);

    // Quantized: starts on the next bar of the playing main timeline.
    settings.quantize = 2;
    model.setPlaySettings(drums, settings);
    model.seekTimeline(drums, 0.0);
    model.playTimeline(drums);
    double at = 0.0; bool starts = false;
    assert(drumsTransport->nextPendingAction(at, starts) && starts);
    const auto rootState = owner.transport->getState();
    const double barBeat = std::ceil(rootState.beatPosition / 4.0 - 1e-9) * 4.0;
    near(at, rootState.clockSeconds + (barBeat - rootState.beatPosition) * 60.0 / rootState.bpm);

    // Saved: the timeline, its play settings and that it plays free.
    const auto json = model.toJson();
    assert(json.contains("timelines") && json["timelines"].size() == 1);
    assert(json.contains("players") && json["players"].size() == 1);
    ofxOceanodeContainer other;
    ofxOceanodeTimelineManager restored(&other);
    restored.fromJson(json);
    assert(restored.timelinesOfCanvas("").size() == 2);
    assert(restored.isFree(drums));
    assert(restored.timelineName(drums) == "Drums");
    near(restored.playSettings(drums)->bpm, 60.0);
    assert(restored.playSettings(drums)->quantize == 2);
    assert(restored.timelineFor(drums)->tracks.size() == 1);
    // A timeline's own JSON carries its tempo, loop and meter.
    settings.loopEnabled = true; settings.loopEndBeat = 8.0; settings.timeSignatureNumerator = 3;
    model.setPlaySettings(drums, settings);
    ofxOceanodeTimelineSequence copy;
    model.sequenceFromJson(copy, model.sequenceToJson(*pattern));
    assert(copy.play == pattern->play);

    // Duplicated, renamed and removed.
    const auto twin = model.duplicateTimeline(drums, "Drums 2");
    assert(!twin.empty() && model.timelineFor({"", twin})->tracks.size() == 1);
    assert(model.renameTimeline({"", twin}, "Fills") && model.timelineName({"", twin}) == "Fills");
    assert(model.removeTimeline({"", twin}) && model.timelinesOfCanvas("").size() == 2);

    // No longer free: its transport goes, its source is listed as unselectable.
    model.setFree(drums, false);
    assert(model.transportFor(drums) == nullptr);
    for(int i = 0; i < 20; ++i) frame(0.05);
    assert(owner.transport->findTimeSource(ofxOceanodeTimelineManager::timeSourceId(drums)) == nullptr);
    // ---- A timeline placed in another as a clip, stretched ----
    {
        ofxOceanodeContainer nest;
        nest.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<float> fillValue("Fill", 0, 0, 1);
        ofxOceanodeParameter<float> fill; fill.bindParameter(fillValue);
        nest.targets[fill.getEscapedName()] = &fill;
        ofxOceanodeTimelineManager nested(&nest);
        nested.setSchedulingEnabled(false);
        const auto fillsId = nested.createTimeline("", "Fills");
        const ofxOceanodeTimelineManager::TimelineRef fills{"", fillsId};
        auto* fillSequence = nested.timelineFor(fills);
        const auto ft = nested.createTrack(*fillSequence, "Fill");
        const auto fb = nested.addBinding(*fillSequence, ft, fill);
        const auto fa = nested.createClip(*fillSequence, ft, "Low", 0, 1);
        const auto fal = nested.createLane(*fillSequence, ft, fa, "Fill", ofxOceanodeTimelineLaneType::Number);
        nested.addBindingToLane(*fillSequence, ft, fa, fal, fb);
        nested.setClipNumber(*fillSequence, ft, fa, fal, 0, 0.3f);
        const auto fh = nested.createClip(*fillSequence, ft, "High", 1, 1);
        const auto fhl = nested.createLane(*fillSequence, ft, fh, "Fill", ofxOceanodeTimelineLaneType::Number);
        nested.addBindingToLane(*fillSequence, ft, fh, fhl, fb);
        nested.setClipNumber(*fillSequence, ft, fh, fhl, 0, 0.7f);
        // On the main timeline at beat 4, at half speed (2 beats per beat of Fills).
        auto& mainTimeline = nested.getMainSequence();
        const auto placing = nested.createTimelineTrack(mainTimeline, fills);
        assert(!placing.empty());
        assert(nested.getTrack(mainTimeline, placing)->macroCanvas == ".");
        assert(nested.placedTimeline(mainTimeline, *nested.getTrack(mainTimeline, placing)) == fillSequence);
        const auto placedClip = nested.createClip(mainTimeline, placing, "Fills", 4, 4);
        auto* clip = nested.getClip(mainTimeline, placing, placedClip);
        clip->contentDurationBeats = 2.0;
        clip->contentStretch = 2.0;
        clip->repeatContent = true;
        // Neither may play itself, directly or through the other.
        assert(nested.createTimelineTrack(*fillSequence, {"", ""}).empty());
        assert(nested.createTimelineTrack(*fillSequence, fills).empty());
        assert(nested.timelineReaches({"", ""}, fills));
        nest.transport->seekToBeat(5.0); // Fills beat 0.5
        nest.transport->play();
        nest.transport->latchFrameState();
        nested.update();
        near(fillValue, 0.3f);
        nest.transport->seekToBeat(7.0); // Fills beat 1.5
        nest.transport->latchFrameState();
        nested.update();
        near(fillValue, 0.7f);
        // Saved with which timeline the track plays.
        const auto saved = nested.toJson();
        ofxOceanodeContainer reloadedOwner;
        ofxOceanodeTimelineManager reloaded(&reloadedOwner);
        reloaded.fromJson(saved);
        const auto* reloadedTrack = reloaded.getTrack(reloaded.getMainSequence(), placing);
        assert(reloadedTrack != nullptr && reloadedTrack->macroCanvas == "." && reloadedTrack->macroTimeline == fillsId);
    }

    // ---- A quantized launch hands its first events over timestamped ----
    {
        ofxOceanodeContainer clocked;
        // A frozen clock (External with no source), so the instants are exact.
        clocked.transport->setDriverMode(TransportDriverMode::External);
        ofParameter<float> gateValue("Gate", 0, 0, 1);
        ofxOceanodeParameter<float> gate; gate.bindParameter(gateValue);
        clocked.targets[gate.getEscapedName()] = &gate;
        ofxOceanodeTimelineManager scheduled(&clocked);
        scheduled.setSchedulingLookaheadAdaptive(false); // the 120 ms window
        std::vector<ofxOceanodeScheduledParameterEvent> events;
        ofxOceanodeScheduling::registerParameterTarget(&gate, &scheduled, [&](const auto& event) {
            events.push_back(event);
            return true;
        });
        const auto lane = scheduled.createTimeline("", "Hits");
        const ofxOceanodeTimelineManager::TimelineRef hits{"", lane};
        auto* sequence = scheduled.timelineFor(hits);
        const auto track = scheduled.createTrack(*sequence, "Hit");
        const auto binding = scheduled.addBinding(*sequence, track, gate);
        // A hit 0.05 beats (50 ms at 60 BPM) after the start.
        const auto clip = scheduled.createClip(*sequence, track, "Hit", 0.05, 1);
        const auto clipLane = scheduled.createLane(*sequence, track, clip, "Gate", ofxOceanodeTimelineLaneType::Number);
        scheduled.addBindingToLane(*sequence, track, clip, clipLane, binding);
        scheduled.setClipNumber(*sequence, track, clip, clipLane, 0, 1.0f);
        auto hitSettings = *scheduled.playSettings(hits);
        hitSettings.bpm = 60.0f;
        hitSettings.quantize = 0;
        scheduled.setPlaySettings(hits, hitSettings);
        scheduled.setFree(hits, true);
        scheduled.update();
        auto hitsTransport = scheduled.transportFor(hits);
        assert(hitsTransport != nullptr);
        scheduled.playTimeline(hits);
        double startClock = 0.0; bool startsNow = false;
        assert(hitsTransport->nextPendingAction(startClock, startsNow) && startsNow);
        clocked.transport->syncRealTime();
        scheduled.update();
        // Not playing yet, but its first hit is already handed over, exactly
        // 50 ms after the start instant.
        assert(!hitsTransport->getState().isPlaying);
        assert(!events.empty());
        const uint64_t startUs = hitsTransport->steadyUsAtClock(startClock);
        const int64_t error = static_cast<int64_t>(events.front().dueSteadyTimeUs) - static_cast<int64_t>(startUs + 50000);
        if(std::llabs(error) > 2) { std::cerr << "launch event off by " << error << " us\n"; std::abort(); }
        assert(events.front().value == "1");
        ofxOceanodeScheduling::unregisterOwner(&scheduled);
    }
    std::cout << "Multi-timeline regression passed: further timelines, free players, own tempo, quantized launch, time sources, persistence, nested timelines.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
