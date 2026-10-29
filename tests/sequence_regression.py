#!/usr/bin/env python3
"""A Sequence on its own: lookups, new ids and names, independent of a manager.

The manager keeps the main timeline in one; a second Sequence (a macro's,
later) must be usable without touching it.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeTimelineSequence sequence;
    assert(sequence.findTrack("timeline_track_1") == nullptr);
    assert(ofxOceanodeTimelineSequence::makeId("timeline_track", 7) == "timeline_track_7");

    // New ids start at the counters and skip ids already in use.
    assert(sequence.makeUniqueTrackId() == "timeline_track_1");
    ofxOceanodeTimelineTrack track;
    track.id = sequence.makeUniqueTrackId(); track.name = "Drums";
    ofxOceanodeTimelineParameterBinding binding;
    binding.id = "timeline_binding_1"; binding.parameterPath = "Kick/Gate";
    track.bindings.push_back(binding);
    ofxOceanodeTimelineClip clip;
    clip.id = "timeline_clip_1"; clip.name = "Clip";
    ofxOceanodeTimelineLane lane;
    lane.id = "timeline_lane_1";
    clip.lanes.push_back(lane);
    track.clips.push_back(clip);
    sequence.tracks.push_back(track);
    assert(sequence.makeUniqueTrackId() == "timeline_track_2");
    assert(sequence.makeUniqueBindingId() == "timeline_binding_2");
    assert(sequence.makeUniqueClipId() == "timeline_clip_2");
    assert(sequence.makeUniqueLaneId() == "timeline_lane_2");
    assert(sequence.makeUniqueGroupId() == "timeline_group_1");
    sequence.nextClipNumber = 9;
    assert(sequence.makeUniqueClipId() == "timeline_clip_9");

    // Names get the first free suffix.
    assert(sequence.makeUniqueTrackName("Drums") == "Drums 2");
    assert(sequence.makeUniqueTrackName("Bass") == "Bass");
    assert(sequence.makeUniqueTrackName("") == "Timeline Track");
    assert(ofxOceanodeTimelineSequence::makeUniqueClipName(sequence.tracks[0], "Clip") == "Clip 2");

    // Lookups, mutable and const.
    const auto& constant = sequence;
    assert(sequence.findTrack("timeline_track_1") == &sequence.tracks[0]);
    assert(constant.findClip("timeline_track_1", "timeline_clip_1") == &sequence.tracks[0].clips[0]);
    assert(sequence.findLane("timeline_track_1", "timeline_clip_1", "timeline_lane_1") == &sequence.tracks[0].clips[0].lanes[0]);
    assert(constant.findBinding("timeline_track_1", "timeline_binding_1")->parameterPath == "Kick/Gate");
    assert(sequence.findClip("timeline_track_1", "missing") == nullptr);
    assert(constant.findLane("missing", "timeline_clip_1", "timeline_lane_1") == nullptr);

    // The manager's main timeline is a Sequence: what it creates is found there.
    ofxOceanodeContainer owner;
    ofxOceanodeTimelineManager model(&owner);
    const auto t = model.createTrack("Main");
    const auto c = model.createClip(t, "A", 0, 4);
    assert(model.getTracks().size() == 1 && model.getTrack(t) == &model.getTracks()[0]);
    assert(model.getClip(t, c) != nullptr && model.getClip(t, c)->name == "A");
    assert(sequence.tracks.size() == 1); // separate sequences stay separate
    std::cout << "Sequence passed: ids, names, lookups, independent of the manager.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
