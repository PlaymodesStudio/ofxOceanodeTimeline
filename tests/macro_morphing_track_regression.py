#!/usr/bin/env python3
"""The Macro Morphing Track's model: one per timeline, clips that play a
source, overlaps as transitions.

No engine and no playback: the track, its clips' sources and transitions,
appending with an overlap given in seconds, the overlaps and the scene at a
beat, the overlap rules under every way of timing a clip, the operations a
morph track refuses, the JSON round trip, undo and a file with two morph tracks.
"""
import timeline_regression as fixture
CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
static bool nearly(double a, double b) { return std::abs(a - b) < 1e-6; }
// The overlap rules, checked from scratch: in start order, starts and ends
// both advance (no clip inside another) and a clip never reaches the one two
// before it (at most two play at any beat).
static bool valid(const ofxOceanodeTimelineTrack& track) {
    std::vector<const ofxOceanodeTimelineClip*> order;
    for(const auto& clip : track.clips) order.push_back(&clip);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->startBeat < b->startBeat; });
    auto end = [](const ofxOceanodeTimelineClip* clip) { return clip->startBeat + clip->durationBeats; };
    for(size_t i = 1; i < order.size(); ++i) {
        if(!(order[i]->startBeat > order[i - 1]->startBeat) || !(end(order[i]) > end(order[i - 1]))) return false;
        if(i > 1 && order[i]->startBeat < end(order[i - 2]) - 1e-9) return false;
    }
    return true;
}
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setBpm(120.0f);
    ofParameter<float> level("Level", 0, 0, 1);
    ofxOceanodeParameter<float> lp; lp.bindParameter(level);
    owner.targets[lp.getEscapedName()] = &lp;
    TestTimeline model(&owner);
    const double offset = ofxOceanodeTimelineManager::kMorphMinOffsetBeats;
    const auto repeat = ofxOceanodeTimelineEndEdit::Repeat;

    // ---- One per timeline ----
    const auto mt = model.createMorphTrack("Morph");
    check(!mt.empty() && model.getTrack(mt) != nullptr && model.getTrack(mt)->isMorphTrack, "A Macro Morphing Track is made");
    check(!model.getTrack(mt)->isMacroTrack() && model.getTrack(mt)->macroCanvas.empty(), "...which is not a macro track");
    check(model.morphTrackOf(model.editSequence()) == model.getTrack(mt), "morphTrackOf finds it");
    check(model.createMorphTrack("Second").empty() && model.getTracks().size() == 1, "A second one is refused");
    check(model.duplicateTrack(mt).empty() && model.getTracks().size() == 1, "...and so is its duplicate");
    const auto plain = model.createTrack("Plain");
    check(!model.getTrack(plain)->isMorphTrack && model.morphTrackOf(model.editSequence()) == model.getTrack(mt), "An ordinary track is not one");
    check(model.addBinding(mt, lp).empty() && model.getTrack(mt)->bindings.empty(), "A morph track takes no binding");
    check(model.createMorphClip(plain, "src_a", 0, 8).empty(), "A morph clip goes on a morph track only");

    // ---- Appending: 4 s before the last clip's end, at the tempo there ----
    auto clip = [&](const std::string& id) { return model.getClip(mt, id); };
    const auto a = model.appendMorphClip(mt, "src_a", 32);
    check(!a.empty() && nearly(clip(a)->startBeat, 0) && nearly(clip(a)->durationBeats, 32), "The first clip starts at beat 0");
    check(clip(a)->morphSource == "src_a" && clip(a)->lanes.empty() && clip(a)->morphIn.isDefault(), "...playing its source, default transition, no lanes");
    check(model.createLane(mt, a, "Lane", ofxOceanodeTimelineLaneType::Curve).empty() && clip(a)->lanes.empty(), "A morph clip takes no lane");
    const auto b = model.appendMorphClip(mt, "src_b", 32);
    check(!b.empty() && nearly(clip(b)->startBeat, 24) && nearly(clip(b)->durationBeats, 32), "At 120 BPM 4 s are 8 beats: the second starts at 24");
    owner.transport->setBpm(60.0f);
    const auto c = model.appendMorphClip(mt, "src_c", 32);
    check(!c.empty() && nearly(clip(c)->startBeat, 52), "At 60 BPM 4 s are 4 beats: the third starts at 52");
    const auto d = model.appendMorphClip(mt, "src_d", 4);
    check(!d.empty() && nearly(clip(d)->startBeat, 82) && nearly(clip(d)->durationBeats, 4), "The overlap is at most half of the shorter clip");
    const auto e = model.appendMorphClip(mt, "src_e", 8, 0.0);
    check(!e.empty() && nearly(clip(e)->startBeat, 86), "No overlap: the clip starts where the last one ends");
    check(model.removeClip(mt, e), "(removed again)");
    owner.transport->setBpm(120.0f);
    check(model.appendMorphClip(mt, "", 8).empty() && model.getTrack(mt)->clips.size() == 4, "A clip needs a source");
    check(valid(*model.getTrack(mt)), "Appended clips keep the overlap rules");

    // ---- Overlaps: neighbouring pairs in start order ----
    {
        const auto overlaps = ofxOceanodeTimelineManager::morphOverlaps(*model.getTrack(mt));
        check(overlaps.size() == 3, "Three transitions between four clips");
        if(overlaps.size() == 3) {
            check(overlaps[0].earlierClipId == a && overlaps[0].laterClipId == b && nearly(overlaps[0].fromBeat, 24) && nearly(overlaps[0].toBeat, 32), "a into b over 24..32");
            check(overlaps[1].earlierClipId == b && overlaps[1].laterClipId == c && nearly(overlaps[1].fromBeat, 52) && nearly(overlaps[1].toBeat, 56), "b into c over 52..56");
            check(overlaps[2].earlierClipId == c && overlaps[2].laterClipId == d && nearly(overlaps[2].fromBeat, 82) && nearly(overlaps[2].toBeat, 84), "c into d over 82..84");
        }
        check(ofxOceanodeTimelineManager::morphOverlaps(*model.getTrack(plain)).empty(), "No transitions on an ordinary track");
    }

    // ---- The scene at a beat ----
    auto scene = [&](double beat) { return ofxOceanodeTimelineManager::morphSceneAt(*model.getTrack(mt), beat); };
    check(scene(10).originClipId == a && scene(10).targetClipId.empty() && scene(10).position == 0.0f, "Inside a clip: that clip alone");
    check(scene(0).originClipId == a, "A clip plays from its start");
    check(scene(24).originClipId == a && scene(24).targetClipId == b && nearly(scene(24).position, 0), "The overlap starts at position 0");
    check(scene(28).originClipId == a && scene(28).targetClipId == b && nearly(scene(28).position, 0.5), "Half way through it: 0.5");
    check(scene(31).targetClipId == b && nearly(scene(31).position, 0.875), "...and linear");
    check(scene(32).originClipId == b && scene(32).targetClipId.empty(), "At the earlier clip's end the later one plays alone");
    check(scene(86).originClipId.empty() && scene(86).targetClipId.empty(), "A clip does not play at its end");
    check(scene(85.5).originClipId == d, "...but does just before it");
    check(scene(200).originClipId.empty(), "Nothing after the last clip");
    check(model.removeClip(mt, d), "(the last clip removed)");
    const auto g = model.createMorphClip(mt, "src_d", 90, 4);
    check(!g.empty() && nearly(clip(g)->startBeat, 90) && nearly(clip(g)->durationBeats, 4), "A clip placed after a gap");
    check(scene(87).originClipId.empty() && scene(87).targetClipId.empty(), "In a gap nothing plays");
    check(ofxOceanodeTimelineManager::morphOverlaps(*model.getTrack(mt)).size() == 2, "...and a gap is no transition");

    // ---- The overlap rules: a = 0..32, b = 24..56, c = 52..84, g = 90..94 ----
    auto track = [&]() -> const ofxOceanodeTimelineTrack& { return *model.getTrack(mt); };
    auto endOf = [&](const std::string& id) { return clip(id)->startBeat + clip(id)->durationBeats; };
    // Moving (the length is kept).
    check(model.setClipTiming(mt, b, 10, 32) && nearly(clip(b)->startBeat, 10) && nearly(clip(b)->durationBeats, 32) && valid(track()), "A valid move goes where asked");
    check(model.setClipTiming(mt, b, -5, 32) && nearly(clip(b)->startBeat, offset) && nearly(clip(b)->durationBeats, 32) && valid(track()),
          "Moved before its neighbour's start, a clip stops just after it");
    check(model.setClipTiming(mt, b, 70, 32) && nearly(clip(b)->startBeat, 62 - offset) && nearly(clip(b)->durationBeats, 32) && valid(track()),
          "Moved past the next clip, it goes beyond it, as near as it fits at its length");
    check(model.setClipTiming(mt, b, 24, 32) && nearly(clip(b)->startBeat, 24), "(back)");
    check(model.setClipTiming(mt, c, 20, 32) && nearly(clip(c)->startBeat, 32) && valid(track()), "A clip never reaches the clip two before it");
    check(scene(33).originClipId == b && scene(33).targetClipId == c, "...so two clips play there, not three");
    check(model.setClipTiming(mt, c, 52, 32) && nearly(clip(c)->startBeat, 52), "(back)");
    // Reordering: a move goes past a neighbour when it fits beyond it at its
    // length and that is nearer to where it was asked to go.
    check(model.setClipTiming(mt, c, 100, 32) && nearly(clip(c)->startBeat, 100) && nearly(clip(c)->durationBeats, 32) && valid(track()),
          "Dragged past the last clip into free space, a clip lands where asked");
    check(ofxOceanodeTimelineManager::morphOverlaps(track()).size() == 1 && scene(101).originClipId == c && scene(92).originClipId == g,
          "...now after it: only a and b still overlap");
    check(model.setClipTiming(mt, c, 52, 32) && nearly(clip(c)->startBeat, 52) && valid(track()) &&
          ofxOceanodeTimelineManager::morphOverlaps(track()).size() == 2, "...and dragged back past it, to where it was");
    check(model.setClipTiming(mt, g, 40, 4) && nearly(clip(g)->startBeat, 80 + offset) && nearly(clip(g)->durationBeats, 4) && valid(track()),
          "A clip is not taken between two clips that overlap each other: it stops on its side");
    check(model.setClipTiming(mt, g, 90, 4) && nearly(clip(g)->startBeat, 90), "(back)");
    check(model.setClipTiming(mt, a, 40, 32) && clip(a)->startBeat < clip(b)->startBeat && valid(track()), "The first clip stays first");
    check(model.setClipTiming(mt, a, 0, 32) && nearly(clip(a)->startBeat, 0), "(back)");
    check(model.setClipTiming(mt, g, 60, 4) && clip(g)->startBeat > clip(c)->startBeat && endOf(g) > endOf(c) && valid(track()),
          "A short clip is not left inside a long one");
    check(model.setClipTiming(mt, g, 90, 4) && nearly(clip(g)->startBeat, 90), "(back)");
    // Timing with a new length.
    check(model.setClipTiming(mt, b, 24, 200) && nearly(clip(b)->startBeat, 24) && nearly(endOf(b), 84 - offset) && valid(track()),
          "A longer clip ends before the next one does");
    check(model.setClipTiming(mt, b, 24, 32) && nearly(endOf(b), 56), "(back)");
    // Resizing the end.
    check(model.resizeClipEnd(mt, a, 70, repeat) && nearly(clip(a)->durationBeats, 52) && valid(track()), "An end stops at the start of the clip two after");
    check(model.resizeClipEnd(mt, a, 32, repeat) && nearly(clip(a)->durationBeats, 32), "(back)");
    check(model.resizeClipEnd(mt, b, 4, repeat) && nearly(endOf(b), 32 + offset) && valid(track()), "An end stays after the end of the clip before");
    check(model.resizeClipEnd(mt, b, 32, repeat) && nearly(endOf(b), 56), "(back)");
    check(model.resizeClipEnd(mt, g, 40, repeat) && nearly(clip(g)->durationBeats, 40) && valid(track()), "The last clip grows freely");
    check(model.resizeClipEnd(mt, g, 4, repeat) && nearly(clip(g)->durationBeats, 4), "(back)");
    // Trimming the start.
    check(model.trimClipStart(mt, c, 10) && nearly(clip(c)->startBeat, 32) && nearly(endOf(c), 84) && valid(track()), "A start stops at the end of the clip two before");
    check(model.trimClipStart(mt, c, 52) && nearly(clip(c)->startBeat, 52) && nearly(endOf(c), 84), "(back)");
    check(model.trimClipStart(mt, b, 0) && nearly(clip(b)->startBeat, offset) && nearly(endOf(b), 56) && valid(track()), "A start stays after the start of the clip before");
    check(model.trimClipStart(mt, b, 24) && nearly(clip(b)->startBeat, 24) && nearly(endOf(b), 56), "(back)");
    check(model.trimClipStart(mt, b, 54) && nearly(clip(b)->startBeat, 52 - offset) && nearly(endOf(b), 56) && valid(track()), "...and before the start of the clip after");
    check(model.trimClipStart(mt, b, 24) && nearly(clip(b)->startBeat, 24) && valid(track()), "(back)");
    // New clips are held to the rules too.
    check(model.createMorphClip(mt, "src_x", 26, 2).empty() && track().clips.size() == 4, "No clip between two that overlap");
    // Other tracks are not held to them.
    const auto p1 = model.createClip(plain, "One", 0, 16), p2 = model.createClip(plain, "Two", 4, 4);
    check(!p2.empty() && nearly(model.getClip(plain, p2)->startBeat, 4) && nearly(model.getClip(plain, p2)->durationBeats, 4), "An ordinary track keeps a clip inside another");
    check(model.setClipTiming(plain, p2, 2, 2) && nearly(model.getClip(plain, p2)->startBeat, 2) && nearly(model.getClip(plain, p1)->durationBeats, 16), "...wherever it is moved");

    // ---- What a morph track refuses, and what it does its own way ----
    check(model.mergeClips(mt, a, b).empty() && track().clips.size() == 4, "Clips do not merge");
    check(model.mergeClips(mt, std::vector<std::string>{a, b}).empty(), "...nor several at once");
    check(!model.reverseClip(mt, a) && !clip(a)->reversed, "A clip does not reverse");
    check(model.duplicateClip(mt, g, plain, 0).empty() && model.duplicateClip(plain, p1, mt, 120).empty(), "Clips do not cross to or from other tracks");
    check(model.duplicateClip(mt, a, mt, 10).empty() && track().clips.size() == 4, "A duplicate with no valid place is refused");
    const auto copy = model.duplicateClip(mt, g, mt, 100);
    check(!copy.empty() && clip(copy)->morphSource == "src_d" && nearly(clip(copy)->startBeat, 100) && nearly(clip(copy)->durationBeats, 4) && valid(track()),
          "A duplicate plays the same source, placed by the rules");
    check(model.removeClip(mt, copy), "(removed again)");
    // The transition into b, then b cut where it plays alone (32..52).
    ofxOceanodeTimelineMorphTransition transition;
    transition.morph = true; transition.biPow = 0.5f;
    transition.morphPolicies = {{"Level", "Jump"}};
    transition.routerOverrides = {{"out_1", {{"policy", "Add"}}}};
    check(model.setMorphTransition(model.editSequence(), mt, b, transition) && clip(b)->morphIn.morph && clip(b)->morphIn.stamp != 0, "A transition is set on the later clip");
    check(!model.setMorphTransition(model.editSequence(), plain, p1, transition), "...of a morph track only");
    check(model.splitClip(mt, a, 28).empty() && track().clips.size() == 4, "A clip is not cut inside an overlap");
    const auto right = model.splitClip(mt, b, 40);
    check(!right.empty() && nearly(clip(b)->startBeat, 24) && nearly(endOf(b), 40) && nearly(clip(right)->startBeat, 40) && nearly(endOf(right), 56), "A cut clip's halves meet at the cut");
    check(clip(b)->morphSource == "src_b" && clip(right)->morphSource == "src_b", "...both playing the source");
    check(clip(b)->morphIn.morph && nearly(clip(b)->morphIn.biPow, 0.5) && clip(right)->morphIn.isDefault(), "...the left one keeping the transition, the right one with the default");
    {
        const auto overlaps = ofxOceanodeTimelineManager::morphOverlaps(track());
        check(overlaps.size() == 2 && overlaps[0].laterClipId == b && overlaps[1].earlierClipId == right && overlaps[1].laterClipId == c, "...and no transition between them");
        check(scene(40).originClipId == right && scene(40).targetClipId.empty() && scene(39.5).originClipId == b, "A hard switch at the cut");
    }
    check(valid(track()), "The rules hold after the cut");

    // ---- JSON round trip ----
    const ofJson state = {{"routers", ofJson::array({{{"id", "out_1"}, {"type", "float"}}})}, {"sources", {{"src_a", {{"path", "Macros/A"}}}}}};
    check(model.setMorphState(model.editSequence(), mt, state) && model.getTrack(mt)->morphStateStamp != 0, "The engine's state is kept on the track");
    check(!model.setMorphState(model.editSequence(), plain, state) && model.getTrack(plain)->morphState.is_null(), "...of a morph track only");
    model.getTrack(mt)->macroUnfolded = true;
    model.getTrack(mt)->macroFoldedTimelines = {"src_a"};
    const auto saved = model.toJson();
    {
        const auto& morphJson = saved["tracks"][0];
        const auto& plainJson = saved["tracks"][1];
        check(morphJson.value("isMorphTrack", false) && morphJson["morphState"].is_string() &&
              ofJson::parse(morphJson["morphState"].get<std::string>()) == state && morphJson.value("macroUnfolded", false),
              "The track's fields are written (the engine's state as text: its null values survive loading)");
        check(!plainJson.contains("isMorphTrack") && !plainJson.contains("morphState") && !plainJson["clips"][0].contains("morphSource") &&
              !plainJson["clips"][0].contains("morphIn"), "...and left out of an ordinary track");
        bool defaultOmitted = false, written = false;
        for(const auto& clipJson : morphJson["clips"]) {
            if(clipJson.value("id", std::string()) == a) defaultOmitted = !clipJson.contains("morphIn") && clipJson.value("morphSource", std::string()) == "src_a";
            if(clipJson.value("id", std::string()) == b)
                written = clipJson.contains("morphIn") && clipJson["morphIn"].value("morph", false) && !clipJson["morphIn"].contains("logBlend") &&
                          clipJson["morphIn"]["morphPolicies"] == transition.morphPolicies && clipJson["morphIn"]["routerOverrides"] == transition.routerOverrides;
        }
        check(defaultOmitted, "A default transition is left out");
        check(written, "A transition is written without its default fields");
    }
    model.fromJson(saved);
    check(model.toJson() == saved, "Saved and loaded: the same document");
    {
        const auto* loaded = model.getTrack(mt);
        check(loaded != nullptr && loaded->isMorphTrack && loaded->morphState == state && loaded->macroUnfolded &&
              loaded->macroFoldedTimelines == std::vector<std::string>{"src_a"}, "The track comes back");
        const auto* loadedClip = model.getClip(mt, b);
        check(loadedClip != nullptr && loadedClip->morphSource == "src_b" && loadedClip->morphIn.morph && nearly(loadedClip->morphIn.biPow, 0.5) &&
              nearly(loadedClip->morphIn.logBlend, 1.0) && loadedClip->morphIn.morphPolicies == transition.morphPolicies &&
              loadedClip->morphIn.routerOverrides == transition.routerOverrides, "...with its clips' sources and transitions");
        check(model.getClip(mt, a) != nullptr && model.getClip(mt, a)->morphIn.isDefault(), "...and the defaults");
        check(model.getTrack(plain) != nullptr && !model.getTrack(plain)->isMorphTrack, "An ordinary track stays one");
        check(model.createMorphTrack("Second").empty(), "A loaded timeline still has one morph track");
    }
    // A clip file: back onto the morph track, never onto a track of its own.
    {
        std::string error;
        const auto document = model.exportClip(mt, g);
        check(!document.is_null() && document["track"].value("isMorphTrack", false) && !document["track"].contains("morphState"), "A clip file leaves the engine's state behind");
        const size_t tracks = model.getTracks().size();
        check(model.importClip(document, "", 120, {}, &error).second.empty() && !error.empty() && model.getTracks().size() == tracks, "A morph clip makes no track of its own");
        check(model.importClip(document, plain, 120, {}, &error).second.empty() && !error.empty(), "...and goes on no ordinary track");
        check(model.importClip(model.exportClip(plain, p1), mt, 120, {}, &error).second.empty() && !error.empty(), "An ordinary clip goes on no morph track");
        const auto imported = model.importClip(document, mt, 120, {}, &error);
        check(imported.first == mt && !imported.second.empty() && model.getClip(mt, imported.second)->morphSource == "src_d" &&
              nearly(model.getClip(mt, imported.second)->startBeat, 120) && valid(*model.getTrack(mt)), "A morph clip goes back on the morph track");
        check(model.removeClip(mt, imported.second), "(removed again)");
    }

    // ---- Undo and redo ----
    model.beginTimelineEdit();
    check(model.setClipTiming(mt, g, 96, 4), "A clip moved in an edit");
    check(model.endTimelineEdit(), "...is an undo step");
    check(model.undoTimelineEdit() && nearly(model.getClip(mt, g)->startBeat, 90), "Undo puts the clip back");
    check(model.redoTimelineEdit() && nearly(model.getClip(mt, g)->startBeat, 96), "Redo moves it again");
    model.beginTimelineEdit();
    transition.logBlend = 0.25f;
    model.setMorphTransition(model.editSequence(), mt, b, transition);
    check(model.endTimelineEdit(), "A transition edit is an undo step");
    check(model.undoTimelineEdit() && nearly(model.getClip(mt, b)->morphIn.logBlend, 1.0) && model.getClip(mt, b)->morphIn.morph, "...undone");
    model.beginTimelineEdit();
    model.setMorphState(model.editSequence(), mt, {{"routers", ofJson::array()}});
    check(model.endTimelineEdit(), "An engine state edit is an undo step");
    check(model.undoTimelineEdit() && model.getTrack(mt)->morphState == state, "...undone");
    model.beginTimelineEdit();
    model.setClipTiming(mt, a, 0, 32);
    check(!model.endTimelineEdit(), "A timing that changes nothing is no undo step");

    // ---- A file with two morph tracks: the first stays one ----
    {
        auto document = model.sequenceToJson(model.editSequence());
        auto second = document["tracks"][0];
        check(second.value("isMorphTrack", false), "(the morph track is the document's first)");
        second["id"] = "timeline_track_second";
        second["name"] = "Second Morph";
        document["tracks"].push_back(second);
        ofxOceanodeTimelineSequence loaded;
        model.sequenceFromJson(loaded, document);
        size_t morphTracks = 0;
        for(const auto& loadedTrack : loaded.tracks) if(loadedTrack.isMorphTrack) ++morphTracks;
        check(loaded.tracks.size() == 3 && morphTracks == 1, "One morph track is kept");
        check(!loaded.tracks.empty() && loaded.tracks.front().isMorphTrack && loaded.tracks.front().morphState == state, "...the first");
        const auto* demoted = loaded.findTrack("timeline_track_second");
        check(demoted != nullptr && !demoted->isMorphTrack && demoted->morphState.is_null() && !demoted->macroUnfolded, "The other loads as an ordinary track");
        check(demoted != nullptr && demoted->clips.size() == loaded.tracks.front().clips.size() &&
              std::all_of(demoted->clips.begin(), demoted->clips.end(), [](const auto& item) { return item.morphSource.empty() && item.morphIn.isDefault(); }),
              "...with its clips, without their sources");
        check(model.morphTrackOf(loaded) == &loaded.tracks.front(), "morphTrackOf finds the one kept");
    }

    if(failures > 0) return 1;
    std::cout << "Macro morphing track passed: one per timeline, append with an overlap in seconds, overlaps, scene at a beat, "
                 "overlap rules (move, resize, trim, new clips), refusals, split, duplicate, JSON round trip, clip files, undo/redo, two morph tracks in a file.\n";
}
'''
if __name__=='__main__': fixture.main(cases=CASES)
