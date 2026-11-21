#!/usr/bin/env python3
"""Copying tracks and clips from one timeline and pasting them into another.

Uses real nodes (native headless fixture). A copied track keeps its targets,
clips, note group and clip groups in the timeline pasted into, and outlives
the timeline it came from. Copied clips land on another timeline's track,
which takes the targets it lacks, or on new tracks. Pasting into a macro's
timeline rebases the paths onto the macro's canvas.
"""
import encapsulate_timeline_regression as base

native = base.native
native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
class Probe : public ofxOceanodeNodeModel {
public:
    Probe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> pitch, gate;
    void setup() override { addParameter(pitch.set("Pitch", 60, 0, 127)); addParameter(gate.set("Gate", 0, 0, 1)); }
};
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
int main(int, char**) {
    ofDisableDataPath(); ofSeedRandom();
    const auto workFolder = std::filesystem::temp_directory_path() / "oceanode-track-clipboard";
    std::filesystem::create_directories(workFolder);
    std::filesystem::current_path(workFolder);
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    registry->registerModel<Probe>("Test");
    registry->registerModel<ofxOceanodeNodeMacro>("Macro");
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    model.setSchedulingEnabled(false);
    auto& node = owner->createNode(std::make_unique<Probe>(), 1);
    auto param = [&](const std::string& name) -> ofxOceanodeAbstractParameter& {
        return static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get(name));
    };
    using Kind = ofxOceanodeTimelineManager::TrackClipboard::Kind;

    const auto mainRef = model.addTimeline("", "Timeline 1");
    const auto pipeRef = model.addTimeline("", "steampipe");
    auto& pipe = *model.findTimeline(pipeRef);
    // steampipe: an instrument track with a piano roll clip and a grouped curve clip.
    const auto t = model.createTrack(pipe, "Marimba");
    const auto pb = model.addBinding(pipe, t, param("Pitch"));
    const auto gb = model.addBinding(pipe, t, param("Gate"));
    const auto group = model.createNoteGroup(pipe, t, "Marimba", pb, gb, "");
    const auto phrase = model.createNoteGroupClip(pipe, t, group, "Phrase", 4, 8);
    model.getClip(pipe, t, phrase)->lanes.front().pianoNotes = {{0, 1, 64, 0.7f, 1}, {2, 1, 67, 0.7f, 1}};
    const auto swell = model.createClip(pipe, t, "Swell", 12, 4);
    const auto lane = model.createLane(pipe, t, swell, "Gate", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(pipe, t, swell, lane, gb);
    model.getLane(pipe, t, swell, lane)->curvePoints = {{0, 0.2f}, {4, 0.8f}};
    model.groupClips(pipe, {{t, phrase}, {t, swell}});
    const auto pipeBefore = model.toJson();

    // ---- Copy the track, paste it into Timeline 1.
    check(model.copyTracks(pipe, {t}) == 1 && model.trackClipboard().kind == Kind::Tracks, "One track copied");
    check(model.toJson() == pipeBefore, "Copying changes nothing");
    auto* main = model.findTimeline(mainRef);
    const auto mainEmpty = model.sequenceToJson(*main);
    model.beginTimelineEdit(*main);
    const auto made = model.pasteTracks(*main, std::string());
    check(model.endTimelineEdit(*main), "The paste is one undo step");
    const auto mainPasted = model.sequenceToJson(*main);
    check(model.undoTimelineEdit(*main) && model.sequenceToJson(*main) == mainEmpty, "Undo removes the pasted track");
    check(model.redoTimelineEdit(*main) && model.sequenceToJson(*main) == mainPasted, "Redo brings it back");
    check(made.size() == 1, "One track pasted");
    const auto* pasted = made.empty() ? nullptr : model.getTrack(*main, made[0]);
    check(pasted != nullptr && pasted->name == "Marimba", "Named as the copied one");
    if(pasted != nullptr) {
        check(pasted->bindings.size() == 2 && pasted->bindings[0].parameterPath == "Probe_1/Pitch" &&
              pasted->bindings[1].parameterPath == "Probe_1/Gate" && !pasted->bindings[0].missingTarget,
              "Its targets, found");
        check(pasted->noteGroups.size() == 1 && pasted->clips.size() == 2, "Its note group and clips");
        const auto& piano = pasted->clips[0].lanes.front();
        check(piano.pianoNotes.size() == 2 && piano.pianoNotes[1].pitch == 67, "The notes");
        check(piano.pianoPitchBindingId == pasted->noteGroups[0].pitchBindingId &&
              piano.pianoGateBindingId == pasted->noteGroups[0].gateBindingId, "The piano roll plays the pasted group");
        check(pasted->clips[1].lanes.front().curvePoints.size() == 2 &&
              pasted->clips[1].lanes.front().bindingIds == std::vector<std::string>{pasted->bindings[1].id},
              "The curve drives the pasted Gate");
        const auto* clipGroup = model.getGroupForClip(*main, pasted->id, pasted->clips[0].id);
        check(clipGroup != nullptr && clipGroup->members.size() == 2, "The clip group comes along");
        check(pasted->clips[0].startBeat == 4 && pasted->clips[1].startBeat == 12, "At the same beats");
    }
    // ---- steampipe removed: Timeline 1 keeps all of it.
    const auto mainTrack = made.empty() ? std::string() : made[0];
    check(model.removeTimeline(pipeRef), "steampipe removed");
    main = model.findTimeline(mainRef);
    check(main != nullptr && model.getTrack(*main, mainTrack) != nullptr &&
          model.getTrack(*main, mainTrack)->clips.size() == 2, "Timeline 1 still has the pasted track");
    // A second paste after the first: its own ids, after it.
    const auto again = model.pasteTracks(*main, mainTrack);
    check(again.size() == 1 && again[0] != mainTrack && main->tracks.size() == 2 && main->tracks[1].id == again[0],
          "Pasted again, after the selected track");

    // ---- Clips onto a track that lacks their targets.
    const auto other = model.createTrack(*main, "Empty");
    check(model.copyClips(*main, {{mainTrack, model.getTrack(*main, mainTrack)->clips[0].id},
                                  {mainTrack, model.getTrack(*main, mainTrack)->clips[1].id}}) == 2, "Two clips copied");
    check(model.trackClipboard().firstBeat == 4, "From the first one's beat");
    const auto beforeClips = model.sequenceToJson(*main);
    model.beginTimelineEdit(*main);
    const auto clips = model.pasteClips(*main, other, 32);
    check(model.endTimelineEdit(*main), "The clip paste is one undo step");
    const auto afterClips = model.sequenceToJson(*main);
    check(model.undoTimelineEdit(*main) && model.sequenceToJson(*main) == beforeClips,
          "Undo removes the clips and the targets the track took");
    check(model.redoTimelineEdit(*main) && model.sequenceToJson(*main) == afterClips, "Redo pastes them again");
    const auto* empty = model.getTrack(*main, other);
    check(clips.size() == 2 && empty != nullptr && empty->clips.size() == 2, "Pasted onto the track");
    if(empty != nullptr && empty->clips.size() == 2) {
        check(empty->bindings.size() == 2 && empty->noteGroups.size() == 1, "It took their targets and note group");
        check(empty->clips[0].startBeat == 32 && empty->clips[1].startBeat == 40, "Their spacing kept");
        check(empty->clips[0].lanes.front().pianoPitchBindingId == empty->noteGroups[0].pitchBindingId,
              "The piano roll plays the track's new group");
        check(model.getGroupForClip(*main, other, empty->clips[0].id) != nullptr, "Grouped again");
    }
    // Onto a track that has the targets: no second binding for one parameter.
    const auto more = model.pasteClips(*main, other, 64);
    check(more.size() == 2 && model.getTrack(*main, other)->bindings.size() == 2 &&
          model.getTrack(*main, other)->noteGroups.size() == 1, "Its own targets reused");
    // Onto no track: a new one.
    const size_t tracksBefore = main->tracks.size();
    const auto fresh = model.pasteClips(*main, std::string(), 0);
    check(fresh.size() == 2 && main->tracks.size() == tracksBefore + 1 &&
          main->tracks.back().clips.front().startBeat == 0, "Onto a new track");
    // Audio clips never land on an automation track.
    const auto wave = model.createWaveTrack(*main, "Audio");
    model.createClip(*main, wave, "Slice", 0, 2);
    model.copyClips(*main, {{wave, model.getTrack(*main, wave)->clips[0].id}});
    std::string skipped;
    check(model.pasteClips(*main, other, 0, &skipped).empty() && skipped == "Audio", "Audio refused by automation");

    // ---- Into a macro's timeline: the paths rebased onto its canvas.
    auto& macroNode = owner->createNode(registry->create("Macro"), 1);
    auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&macroNode.getNodeModel());
    check(macro != nullptr && macro->getContainer() != nullptr, "A macro");
    if(macro != nullptr && macro->getContainer() != nullptr) {
        const std::string canvas = macro->getContainer()->getCanvasID();
        auto& inner = macro->getContainer()->createNode(std::make_unique<Probe>(), 1);
        (void)inner;
        const auto innerRef = model.addTimeline(canvas, "Inside");
        auto* insideSequence = model.findTimeline(innerRef);
        check(insideSequence != nullptr, "The macro's timeline");
        if(insideSequence != nullptr) {
            // A root track driving the node inside the macro.
            const auto rootTrack = model.createTrack(*main, "Inner");
            auto& innerPitch = static_cast<ofxOceanodeAbstractParameter&>(inner.getParameters().get("Pitch"));
            model.addBinding(*main, rootTrack, innerPitch);
            const std::string rootPath = model.getTrack(*main, rootTrack)->bindings[0].parameterPath;
            model.copyTracks(*main, {rootTrack});
            const auto down = model.pasteTracks(*insideSequence, std::string());
            const auto* moved = down.empty() ? nullptr : model.getTrack(*insideSequence, down[0]);
            check(moved != nullptr && moved->bindings.size() == 1 && moved->bindings[0].parameterPath == "Probe_1/Pitch" &&
                  !moved->bindings[0].missingTarget, "Rebased from " + rootPath + " onto the macro's canvas");
            // And back up: the root's node seen from the macro is outside it.
            model.copyTracks(*main, {mainTrack});
            const auto up = model.pasteTracks(*insideSequence, std::string());
            const auto* outside = up.empty() ? nullptr : model.getTrack(*insideSequence, up[0]);
            check(outside != nullptr && outside->bindings[0].parameterPath == "../Probe_1/Pitch" &&
                  !outside->bindings[0].missingTarget, "A root target is outside the macro");
        }
    }
    if(failures > 0) return 1;
    std::cout << "Track clipboard passed: tracks pasted into another timeline with targets, clips, notes and groups, "
                 "kept after the source timeline is removed; clips onto a track lacking their targets, onto one "
                 "having them, onto a new track; audio refused by automation; paths rebased into a macro.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
