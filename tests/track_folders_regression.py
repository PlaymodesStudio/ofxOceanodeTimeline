#!/usr/bin/env python3
"""Track folders: tree order, moving, removing, duplicating, playback rules, files.

A folder is a track holding the tracks whose parentId is its id. Its mute,
solo and latency apply to everything inside it.
"""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> av("A",0,0,1), bv("B",0,0,1), cv("C",0,0,1);
    ofxOceanodeParameter<float> a, b, c;
    a.bindParameter(av); b.bindParameter(bv); c.bindParameter(cv);
    for(auto* p : {&a,&b,&c}) owner.targets[p->getEscapedName()] = p;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);
    auto order = [&]() {
        std::string out;
        for(const auto& track : model.getTracks()) out += (out.empty() ? "" : " ") + track.name;
        return out;
    };
    auto valueTrack = [&](const std::string& name, ofxOceanodeParameter<float>& parameter, float value) {
        const auto t = model.createTrack(name);
        const auto binding = model.addBinding(t, parameter);
        const auto clip = model.createClip(t, name, 0, 8);
        const auto lane = model.createLane(t, clip, name, ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(t, clip, lane, binding); model.setClipNumber(t, clip, lane, 0, value);
        return t;
    };
    const auto ta = valueTrack("A", a, 0.25f);
    const auto tb = valueTrack("B", b, 0.5f);
    const auto tc = valueTrack("C", c, 0.75f);
    assert(order() == "A B C");

    // ---- tree order and moving ---------------------------------------------
    const auto drums = model.createFolder("Drums");
    assert(order() == "A B C Drums" && model.getTrack(drums)->isFolder);
    assert(model.createClip(drums, "nope", 0, 4).empty()); // folders hold tracks, not clips
    assert(model.moveTrack(ta, drums, 0));
    assert(order() == "B C Drums A" && model.getTrack(ta)->parentId == drums);
    assert(model.moveTrack(tc, drums, 0)); // before A, inside
    assert(order() == "B Drums C A");
    assert(model.moveTrack(drums, "", 0)); // the folder moves with its contents
    assert(order() == "Drums C A B");
    const auto kit = model.createFolder("Kit", drums);
    assert(model.moveTrack(tc, kit, 0));
    assert(order() == "Drums A Kit C B");
    assert(model.getMainSequence().depthOf(*model.getTrack(tc)) == 2);
    assert(!model.moveTrack(drums, kit, 0)); // never into itself
    assert(!model.moveTrack(drums, drums, 0));
    assert(!model.moveTrack(ta, tb, 0)); // only into folders
    assert(model.moveTrack(tb, drums, 99)); // index clamped: last inside
    assert(order() == "Drums A Kit C B");
    assert(model.getTrack(tb)->parentId == drums);

    // ---- playback through folders -----------------------------------------
    owner.transport->seekToBeat(1); owner.transport->play();
    auto frame = [&]() { model.update(); model.applyAutomation(); };
    frame(); near(av, 0.25); near(bv, 0.5); near(cv, 0.75);
    model.setTrackMuted(kit, true); // mutes C inside it
    cv = 0; frame(); near(cv, 0); near(av, 0.25);
    assert(model.isTrackMutedWithFolders(*model.getTrack(tc)) && !model.getTrack(tc)->muted);
    model.setTrackMuted(kit, false);
    model.setTrackSolo(kit, true); // solos C only
    av = 0; bv = 0; frame(); near(cv, 0.75); near(av, 0); near(bv, 0);
    model.setTrackSolo(kit, false);
    model.setTrackSolo(drums, true); // everything is inside Drums
    frame(); near(av, 0.25); near(bv, 0.5); near(cv, 0.75);
    model.setTrackSolo(drums, false);
    model.setTrackLatencyOffset(drums, 200); model.setTrackLatencyOffset(kit, 30); model.setTrackLatencyOffset(tc, 40);
    near(model.trackLatencyWithFolders(*model.getTrack(tc)), 250); // 270, clamped
    near(model.trackLatencyWithFolders(*model.getTrack(ta)), 200);

    // ---- files -----------------------------------------------------------
    const auto saved = model.toJson();
    model.fromJson(saved);
    assert(order() == "Drums A Kit C B" && model.getTrack(tc)->parentId == kit && model.getTrack(kit)->isFolder);
    auto flat = saved;
    for(auto& track : flat["tracks"]) { track.erase("isFolder"); track.erase("parentId"); }
    model.fromJson(flat); // older files: everything at the top level
    assert(std::all_of(model.getTracks().begin(), model.getTracks().end(), [](const auto& t) { return t.parentId.empty(); }));
    model.fromJson(saved);

    // ---- duplicate and remove --------------------------------------------
    const auto copy = model.duplicateTrack(kit);
    assert(!copy.empty() && model.getTrack(copy)->isFolder);
    assert(order() == "Drums A Kit C Kit copy C copy B");
    assert(model.removeFolder(copy, false)); // with its contents
    assert(order() == "Drums A Kit C B");
    assert(model.removeFolder(kit, true)); // contents move up into Drums
    assert(order() == "Drums A C B" && model.getTrack(tc)->parentId == drums);
    assert(model.removeTrack(drums)); // a plain remove: the contents go to the top level
    assert(order() == "A C B");
    std::cout << "Track folders passed: order, moving, playback through folders, files, duplicate, remove.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
