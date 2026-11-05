#!/usr/bin/env python3
"""Several timelines on one canvas, each with its clock source.

A timeline is linked to the global transport by default (it plays with the
root transport, at the global tempo), plays free on a transport of its own
(started, stopped and seeked by itself at its own tempo, on the shared
clock), or depends on the timeline placing it as clips (it plays where they
place it, at that timeline's tempo, and previews alone). Each is listed as a
time source, keeps its tempo, loop and meter, and saves its clock source and
play on load with its own settings. No timeline exists until one is made.
"""
import timeline_regression as fixture

CASES = '#include <filesystem>\n' + fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> levelValue("Level", 0, 0, 1), drumValue("Drum", 0, 0, 1);
    ofxOceanodeParameter<float> level; level.bindParameter(levelValue);
    ofxOceanodeParameter<float> drum; drum.bindParameter(drumValue);
    owner.targets[level.getEscapedName()] = &level;
    owner.targets[drum.getEscapedName()] = &drum;
    TestTimeline model(&owner);
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

    using ClockSource = ofxOceanodeTimelineManager::ClockSource;
    // Linked to the global transport by default: it plays with the root
    // transport, at the global tempo, and is that transport as a time source.
    assert(!model.isFree(drums) && model.clockSourceOf(drums) == ClockSource::Global);
    frame(0.1);
    assert(model.transportFor(drums) == owner.transport);
    assert(owner.transport->findTimeSource(ofxOceanodeTimelineManager::timeSourceId(drums)) == owner.transport);
    assert(owner.transport->findTimeSource("root#default") == owner.transport);
    owner.transport->seekToBeat(3.0); owner.transport->latchFrameState(); model.update();
    near(drumValue, 0.9f);
    near(levelValue, 0.25f);
    double shownBeat = -1.0;
    assert(model.timelinePlayheadBeat(drums, shownBeat)); near(shownBeat, 3.0);
    owner.transport->seekToBeat(0.0); owner.transport->latchFrameState(); model.update();
    near(drumValue, 0.5f);

    // Free: its own transport, at its own tempo, unquantized (set once it
    // is free: freeing it starts it from the global tempo).
    model.setFree(drums, true);
    auto settings = *model.playSettings(drums);
    settings.bpm = 60.0f;
    settings.quantize = 0;
    model.setPlaySettings(drums, settings);
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
    model.playTimeline(model.getEditTarget());
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

    // Saved: the timeline, its play settings and that it plays free (with
    // its own settings, no longer in a list of players).
    const auto json = model.toJson();
    assert(json.contains("timelines") && json["timelines"].size() == 2); // the fixture's "Timeline 1", then Drums
    assert(!json.contains("players") && json["timelines"][1]["play"].value("free", false));
    assert(json["play"].contains("tempo") && json["play"].contains("loop")); // the root default's, whole
    ofxOceanodeContainer other;
    TestTimeline restored(&other);
    restored.fromJson(json);
    assert(restored.timelinesOfCanvas("").size() == 2);
    assert(restored.isFree(drums) && restored.clockSourceOf(drums) == ClockSource::Free);
    {
        // An older file listed the free timelines apart: read into their settings.
        auto legacy = json;
        legacy["timelines"][1]["play"].erase("free");
        legacy["players"] = ofJson::array({{{"canvas", ""}, {"timeline", id}}});
        TestTimeline fromLegacy(&other);
        fromLegacy.fromJson(legacy);
        assert(fromLegacy.isFree(drums));
        // Any timeline can go: removed, the canvas keeps the rest.
        assert(fromLegacy.timelineExists({"", "tl_1"}) && !fromLegacy.getTracks().empty());
        assert(fromLegacy.removeTimeline({"", "tl_1"}));
        assert(!fromLegacy.timelineExists({"", "tl_1"}) && fromLegacy.findTimeline({"", "tl_1"}) == nullptr);
        assert(fromLegacy.timelinesOfCanvas("").size() == 1 && fromLegacy.allTimelines().size() == 1);
    }
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
    assert(!model.sequenceToJson(*pattern)["play"].contains("playOnLoad")); // off: not written

    // Play on load: after its preset loads, a free timeline set to it starts
    // from its beginning by itself; one not set to it stays stopped.
    {
        settings.playOnLoad = true;
        settings.quantize = 0;
        model.setPlaySettings(drums, settings);
        model.sequenceFromJson(copy, model.sequenceToJson(*pattern));
        assert(copy.play.playOnLoad && copy.play == pattern->play);
        const auto still = model.createTimeline("", "Still");
        model.setFree({"", still}, true);
        frame(0.1);
        const auto folder = std::filesystem::temp_directory_path() / ("oceanode-play-on-load-" + ofToString(ofGetSystemTimeMicros()));
        std::filesystem::create_directories(folder);
        assert(model.savePreset(folder.string()));
        ofxOceanodeContainer loadedOwner;
        loadedOwner.transport->setDriverMode(TransportDriverMode::FrameStep);
        TestTimeline loaded(&loadedOwner);
        loaded.setSchedulingEnabled(false);
        assert(loaded.loadPreset(folder.string()));
        assert(loaded.playSettings(drums)->playOnLoad && !loaded.playSettings({"", still})->playOnLoad);
        for(int i = 0; i < 5; ++i) {
            loadedOwner.transport->advanceFrameStep(0.1);
            loadedOwner.transport->latchFrameState();
            loaded.update();
        }
        assert(loaded.transportFor(drums) != nullptr && loaded.transportFor(drums)->getState().isPlaying);
        assert(loaded.transportFor(drums)->getState().beatPosition < 1.0);
        assert(loaded.transportFor({"", still}) != nullptr && !loaded.transportFor({"", still})->getState().isPlaying);
        assert(!loadedOwner.transport->getState().isPlaying); // the main timeline is not set to play on load
        // Stopped by hand, it stays stopped: only a load starts it.
        loaded.stopTimeline(drums, true, false);
        for(int i = 0; i < 3; ++i) {
            loadedOwner.transport->advanceFrameStep(0.1);
            loadedOwner.transport->latchFrameState();
            loaded.update();
        }
        assert(!loaded.transportFor(drums)->getState().isPlaying);
        std::filesystem::remove_all(folder);
        model.removeTimeline({"", still});
        settings.playOnLoad = false;
        model.setPlaySettings(drums, settings);
    }

    // Added from a parameter's menu: on the root canvas, always a further one.
    const auto pads = model.addTimeline("", "Pads");
    assert(pads.canvas.empty() && !pads.timeline.empty() && model.timelineExists(pads));
    assert(model.timelineName(pads) == "Pads" && model.findTimeline(pads) == model.timelineFor(pads));
    assert(!model.timelineExists({"", "tl_unknown"}) && model.findTimeline({"", "tl_unknown"}) == nullptr);
    assert(model.timelinesOfCanvas("").size() == 3);
    assert(model.removeTimeline(pads));
    // Duplicated, renamed and removed.
    const auto twin = model.duplicateTimeline(drums, "Drums 2");
    assert(!twin.empty() && model.timelineFor({"", twin})->tracks.size() == 1);
    assert(model.renameTimeline({"", twin}, "Fills") && model.timelineName({"", twin}) == "Fills");
    assert(model.removeTimeline({"", twin}) && model.timelinesOfCanvas("").size() == 2);

    // No longer free: back on the global transport.
    model.setFree(drums, false);
    for(int i = 0; i < 20; ++i) frame(0.05);
    assert(model.transportFor(drums) == owner.transport);
    assert(owner.transport->findTimeSource(ofxOceanodeTimelineManager::timeSourceId(drums)) == owner.transport);

    // ---- The root canvas's first timeline is a timeline like any other ----
    const auto mainRef = model.getEditTarget();
    {
        // Free, it leaves the global transport: an engine plays it on its own.
        const auto second = model.createClip(mainTrack, "Main clip 2", 2, 2);
        const auto secondLane = model.createLane(mainTrack, second, "Level", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(mainTrack, second, secondLane, mainBinding);
        model.setClipNumber(mainTrack, second, secondLane, 0, 0.75f);
        owner.transport->stop(); owner.transport->latchFrameState();
        frame(0.1); near(levelValue, 0.25f);
        auto mainSettings = *model.playSettings(mainRef);
        mainSettings.quantize = 0;
        model.setPlaySettings(mainRef, mainSettings);
        model.setFree(mainRef, true);
        assert(model.clockSourceOf(mainRef) == ClockSource::Free);
        frame(0.1);
        auto mainTransport = model.transportFor(mainRef);
        assert(mainTransport != nullptr && mainTransport != owner.transport && mainTransport->isDerived());
        near(model.playSettings(mainRef)->bpm, owner.transport->getState().bpm); // it started from the global tempo
        model.seekTimeline(mainRef, 3.0);
        frame(0.1);
        near(levelValue, 0.75f);
        near(owner.transport->getState().beatPosition, 0.0); // the global transport did not move
        // Linked again: the global transport's position.
        model.setFree(mainRef, false);
        frame(0.1);
        assert(model.transportFor(mainRef) == owner.transport);
        near(levelValue, 0.25f);
        assert(model.removeClip(mainTrack, second));
    }
    // ---- No timeline until one is made ----
    {
        ofxOceanodeContainer blank;
        ofxOceanodeTimelineManager none(&blank);
        assert(!none.timelineExists({"", "tl_1"}) && none.allTimelines().empty() && none.timelinesOfCanvas("").empty());
        const auto first = none.addTimeline("", "First");
        assert(first.canvas.empty() && first.timeline == "tl_1" && none.timelineExists(first));
        assert(none.timelineName(first) == "First" && none.allTimelines().size() == 1);
        const auto next = none.addTimeline("", "Second");
        assert(next.timeline == "tl_2" && none.allTimelines().size() == 2);
        // A removed timeline's id is never given to another.
        assert(none.removeTimeline(next) && none.addTimeline("", "Third").timeline == "tl_3");
        // The name a new one gets, and its own when it has one.
        assert(none.suggestTimelineName("") == "Timeline 4" && none.timelineName({"", "tl_3"}) == "Third");
        {
            // A document written before version 15: its top-level timeline becomes "tl_1",
            // empty references to it name it, a track placing a macro's then-default one too.
            ofJson old = none.toJson();
            old["version"] = 14;
            old["tracks"] = ofJson::array({{{"id", "timeline_track_9"}, {"name", "Old"},
                                           {"macroCanvas", "Macro 1"}, {"macroGate", false}}});
            old["play"] = {{"name", "Main"}, {"created", true}};
            old.erase("timelines");
            old["windows"] = ofJson::array({{{"id", 1}, {"canvas", ""}, {"timeline", ""}}});
            old["players"] = ofJson::array({{{"canvas", ""}, {"timeline", ""}}});
            old["clipFollow"] = ofJson::array({"Macro 1"});
            ofxOceanodeTimelineManager upgraded(&blank);
            upgraded.fromJson(old);
            assert(upgraded.timelinesOfCanvas("").size() == 1 && upgraded.timelineExists({"", "tl_1"}));
            assert(upgraded.timelineName({"", "tl_1"}) == "Main" && upgraded.isFree({"", "tl_1"}));
            const auto* track = upgraded.getTrack(*upgraded.findTimeline({"", "tl_1"}), "timeline_track_9");
            assert(track != nullptr && track->macroTimeline == "tl_1");
            assert(upgraded.getTimelineWindows().size() == 1 && upgraded.getTimelineWindows().front().ref.timeline == "tl_1");
            ofJson empty = {{"version", 14}, {"tracks", ofJson::array()}};
            ofxOceanodeTimelineManager nothing(&blank);
            nothing.fromJson(empty);
            assert(nothing.allTimelines().empty()); // a default timeline never used was none
        }
    }
    // ---- A timeline placed in another as a clip, stretched ----
    {
        ofxOceanodeContainer nest;
        nest.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<float> fillValue("Fill", 0, 0, 1), hitValue("Hit", 0, 0, 1);
        ofxOceanodeParameter<float> fill; fill.bindParameter(fillValue);
        ofxOceanodeParameter<float> hit; hit.bindParameter(hitValue);
        nest.targets[fill.getEscapedName()] = &fill;
        nest.targets[hit.getEscapedName()] = &hit;
        TestTimeline nested(&nest);
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
        auto& mainTimeline = nested.editSequence();
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
        assert(nested.createTimelineTrack(*fillSequence, nested.getEditTarget()).empty());
        assert(nested.createTimelineTrack(*fillSequence, fills).empty());
        assert(nested.timelineReaches(nested.getEditTarget(), fills));
        nest.transport->seekToBeat(5.0); // Fills beat 0.5
        nest.transport->play();
        nest.transport->latchFrameState();
        nested.update();
        near(fillValue, 0.3f);
        nest.transport->seekToBeat(7.0); // Fills beat 1.5
        nest.transport->latchFrameState();
        nested.update();
        near(fillValue, 0.7f);
        auto at = [&](double beat) {
            nest.transport->seekToBeat(beat);
            nest.transport->latchFrameState();
            nested.update();
        };
        // Placed, Fills depends on the main timeline: its Free setting is
        // kept but not used, it follows the host's tempo beat for beat
        // (through the clip's stretch), and its playhead is where the host
        // places it while the host plays.
        ofxOceanodeTimelineManager::TimelineRef host;
        assert(nested.clockSourceOf(fills) == ClockSource::Dependent);
        assert(nested.placedBy(fills, host) && host == nested.getEditTarget());
        nested.setFree(fills, true);
        assert(nested.isFree(fills) && nested.clockSourceOf(fills) == ClockSource::Dependent);
        auto fillSettings = *nested.playSettings(fills);
        fillSettings.bpm = 60.0f; // ignored while dependent
        fillSettings.quantize = 0;
        nested.setPlaySettings(fills, fillSettings);
        at(5.5); near(fillValue, 0.3f);  // Fills beat 0.75
        at(6.5); near(fillValue, 0.7f);  // Fills beat 1.25
        double placedBeat = -1.0;
        assert(nested.timelinePlayheadBeat(fills, placedBeat)); near(placedBeat, 1.25);
        // Its own transport previews it at the host's tempo (the global one
        // here), and never plays the host.
        auto fillsTransport = nested.transportFor(fills);
        assert(fillsTransport != nullptr && fillsTransport->isDerived());
        near(fillsTransport->getState().bpm, 120.0); near(nested.hostBpmFor(fills), 120.0);
        nest.transport->setBpm(90.0f);
        at(6.5);
        near(fillsTransport->getState().bpm, 90.0);
        nest.transport->setBpm(120.0f);
        nest.transport->pause();
        nest.transport->latchFrameState();
        nested.update();
        nested.seekTimeline(fills, 0.0);
        nested.playTimeline(fills, false);
        assert(!nest.transport->getState().isPlaying);
        auto step = [&](double seconds) {
            nest.transport->advanceFrameStep(seconds);
            nest.transport->latchFrameState();
            nested.update();
        };
        for(int i = 0; i < 3; ++i) step(0.1);   // 0.6 beats at 120
        assert(fillsTransport->getState().isPlaying && !nest.transport->getState().isPlaying);
        near(fillValue, 0.3f);
        for(int i = 0; i < 4; ++i) step(0.1);   // 1.4 beats
        near(fillValue, 0.7f);
        assert(nested.timelinePlayheadBeat(fills, placedBeat) && placedBeat > 1.3 && placedBeat < 1.5);
        nested.stopTimeline(fills, true, false);
        step(0.1);
        assert(!fillsTransport->getState().isPlaying);
        nest.transport->play();
        at(5.0); // the host plays: its placement drives the playhead again
        assert(nested.timelinePlayheadBeat(fills, placedBeat)); near(placedBeat, 0.5);
        // Past the clip's end its playhead holds where the clip left it: the
        // end of its content, not back at 0.
        at(9.0);
        assert(nested.timelinePlayheadBeat(fills, placedBeat)); near(placedBeat, 2.0);
        clip->contentOffsetBeats = 0.5;
        at(9.0);
        assert(nested.timelinePlayheadBeat(fills, placedBeat)); near(placedBeat, 0.5);
        clip->contentOffsetBeats = 0.0;
        at(5.0);
        // A new clip placing a timeline lasts one pass of it: the end of its
        // last clip, at least a bar of its own meter.
        fillSettings.timeSignatureNumerator = 3;
        nested.setPlaySettings(fills, fillSettings);
        {
            const auto made = nested.createMacroClip(mainTimeline, placing, 20.0);
            const auto* madeClip = nested.getClip(mainTimeline, placing, made);
            assert(madeClip != nullptr);
            near(madeClip->durationBeats, 3.0); near(madeClip->contentDurationBeats, 3.0);
            assert(nested.removeClip(mainTimeline, placing, made));
            const auto longer = nested.createClip(*fillSequence, ft, "Tail", 4, 2);
            const auto again = nested.createMacroClip(mainTimeline, placing, 20.0);
            near(nested.getClip(mainTimeline, placing, again)->durationBeats, 6.0);
            assert(nested.removeClip(mainTimeline, placing, again) && nested.removeClip(*fillSequence, ft, longer));
        }
        fillSettings.timeSignatureNumerator = 4;
        nested.setPlaySettings(fills, fillSettings);
        // Stretched (Cmd+drag on its right edge), then restored: natural
        // speed, as many repeats.
        clip = nested.getClip(mainTimeline, placing, placedClip);
        clip->contentStretch = 2.0;
        clip->durationBeats = 8.0;
        assert(nested.isClipStretched(*clip));
        assert(nested.restoreClipLength(mainTimeline, placing, placedClip));
        near(clip->contentStretch, 1.0);
        near(clip->durationBeats, 4.0);
        assert(!nested.isClipStretched(*clip));
        clip->contentStretch = 2.0;
        clip->durationBeats = 4.0;
        // Nested: Fills places Hits, which then depends on Fills and plays
        // beat for beat with it (so with the main timeline through Fills).
        fillSettings.bpm = 120.0f;
        nested.setPlaySettings(fills, fillSettings);
        const auto hitsId = nested.createTimeline("", "Hits");
        const ofxOceanodeTimelineManager::TimelineRef hits{"", hitsId};
        auto* hitSequence = nested.timelineFor(hits);
        const auto ht = nested.createTrack(*hitSequence, "Hit");
        const auto hb = nested.addBinding(*hitSequence, ht, hit);
        const auto ha = nested.createClip(*hitSequence, ht, "A", 0, 1);
        const auto hal = nested.createLane(*hitSequence, ht, ha, "Hit", ofxOceanodeTimelineLaneType::Number);
        nested.addBindingToLane(*hitSequence, ht, ha, hal, hb);
        nested.setClipNumber(*hitSequence, ht, ha, hal, 0, 0.2f);
        const auto hbClip = nested.createClip(*hitSequence, ht, "B", 1, 1);
        const auto hbl = nested.createLane(*hitSequence, ht, hbClip, "Hit", ofxOceanodeTimelineLaneType::Number);
        nested.addBindingToLane(*hitSequence, ht, hbClip, hbl, hb);
        nested.setClipNumber(*hitSequence, ht, hbClip, hbl, 0, 0.8f);
        auto hitSettings = *nested.playSettings(hits);
        hitSettings.bpm = 240.0f; // its own tempo: unused once placed
        nested.setPlaySettings(hits, hitSettings);
        const auto hitTrack = nested.createTimelineTrack(*fillSequence, hits);
        assert(!hitTrack.empty());
        const auto hitClip = nested.createClip(*fillSequence, hitTrack, "Hits", 0, 4);
        auto* placedHits = nested.getClip(*fillSequence, hitTrack, hitClip);
        placedHits->contentDurationBeats = 2.0;
        placedHits->contentStretch = 1.0;
        placedHits->repeatContent = true;
        assert(nested.clockSourceOf(hits) == ClockSource::Dependent);
        assert(nested.placedBy(hits, host) && host == fills);
        nest.transport->setBpm(120.0f);
        at(4.9); near(hitValue, 0.2f);  // Fills 0.45: Hits beat 0.45
        at(6.45); near(hitValue, 0.8f); // Fills 1.225: Hits beat 1.225
        nest.transport->setBpm(90.0f);
        at(6.45); near(hitValue, 0.8f); // beat for beat, whatever the tempo
        nest.transport->setBpm(120.0f);
        nested.update();
        near(nested.hostBpmFor(hits), 120.0); // Fills' tempo, the global one through it
        // Saved with which timeline the track plays.
        const auto saved = nested.toJson();
        ofxOceanodeContainer reloadedOwner;
        TestTimeline reloaded(&reloadedOwner);
        reloaded.fromJson(saved);
        const auto* reloadedTrack = reloaded.getTrack(reloaded.editSequence(), placing);
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
        TestTimeline scheduled(&clocked);
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
        scheduled.setFree(hits, true); // (freed first: freeing starts from the global tempo)
        auto hitSettings = *scheduled.playSettings(hits);
        hitSettings.bpm = 60.0f;
        hitSettings.quantize = 0;
        scheduled.setPlaySettings(hits, hitSettings);
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
    std::cout << "Multi-timeline regression passed: clock sources (global, free, dependent), quantized launch, time sources, persistence, play on load, nested timelines, the root default as any timeline, no timeline until made.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
