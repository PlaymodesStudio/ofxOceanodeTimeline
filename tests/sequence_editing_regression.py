#!/usr/bin/env python3
"""The manager's editing operations work on any Sequence, not only the main one.

A second Sequence whose ids collide with the main timeline's is edited
through the manager; the main timeline, its playback and its per-frame
caches stay exactly as they were.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> levelValue("Level", 0, 0, 1);
    ofxOceanodeParameter<float> level; level.bindParameter(levelValue);
    owner.targets[level.getEscapedName()] = &level;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);

    // The main timeline: one track driving Level from a clip.
    const auto mainTrack = model.createTrack("Main");
    const auto mainBinding = model.addBinding(mainTrack, level);
    const auto mainClip = model.createClip(mainTrack, "Main clip", 0, 4);
    const auto mainLane = model.createLane(mainTrack, mainClip, "Level", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(mainTrack, mainClip, mainLane, mainBinding);
    model.setClipNumber(mainTrack, mainClip, mainLane, 0, 0.25f);
    owner.transport->seekToBeat(1); owner.transport->play();
    model.update(); model.applyAutomation(); near(levelValue, 0.25);
    const auto savedMain = model.toJson()["tracks"];

    // A second sequence, edited through the same operations. Its ids start
    // at the same numbers as the main timeline's, so every id collides.
    ofxOceanodeTimelineSequence other;
    const auto t = model.createTrack(other, "Pattern");
    assert(t == mainTrack); // same id, different sequence
    assert(other.tracks.size() == 1 && other.tracks[0].name == "Pattern");
    assert(model.getTrack(other, t) == &other.tracks[0]);
    assert(model.getTrack(t)->name == "Main"); // the cached main lookup is untouched
    const auto b = model.addBinding(other, t, level);
    const auto c = model.createClip(other, t, "A", 0, 2);
    assert(c == mainClip);
    const auto l = model.createLane(other, t, c, "Level", ofxOceanodeTimelineLaneType::Number);
    assert(model.addBindingToLane(other, t, c, l, b));
    assert(model.setClipNumber(other, t, c, l, 0, 0.75f));
    assert(model.setClipTiming(other, t, c, 2, 2));
    assert(model.getClip(other, t, c)->startBeat == 2);
    const auto right = model.splitClip(other, t, c, 3);
    assert(!right.empty() && other.tracks[0].clips.size() == 2);
    const auto copy = model.duplicateClip(other, t, c, t, 8);
    assert(!copy.empty() && other.tracks[0].clips.size() == 3);
    assert(!model.groupClips(other, {{t, c}, {t, copy}}).empty());
    assert(other.clipGroups.size() == 1 && model.getClipGroups().empty());
    assert(model.renameTrack(other, t, "Pattern A") && other.tracks[0].name == "Pattern A");
    assert(model.setTrackSolo(other, t, true) && other.tracks[0].solo);
    assert(model.removeClip(other, t, right) && other.tracks[0].clips.size() == 2);
    const auto second = model.duplicateTrack(other, t);
    assert(other.tracks.size() == 2 && model.getTrack(other, second) != nullptr);
    assert(model.removeTrack(other, second) && other.tracks.size() == 1);

    // The main timeline is exactly as it was and still plays the same.
    assert(model.toJson()["tracks"] == savedMain);
    assert(!model.isTrackSoloed(*model.getTrack(mainTrack)));
    levelValue = 0.9f;
    model.update(); model.applyAutomation(); near(levelValue, 0.25);
    std::cout << "Sequence editing passed: edits reach the given sequence only, with colliding ids.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
