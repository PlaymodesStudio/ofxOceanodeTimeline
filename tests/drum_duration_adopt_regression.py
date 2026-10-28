#!/usr/bin/env python3
"""A kit note group saved before its instrument had a Duration input adopts it.

Uses a real node (core's native headless fixture): the group's Drum Grid then
writes the steps' durations into the instrument's Duration vector, without an
undo entry, and a role the user set or cleared is never overridden.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
class KitSynth : public ofxOceanodeNodeModel {
public:
    KitSynth() : ofxOceanodeNodeModel("Kit Synth") {}
    ofParameter<std::vector<float>> pitch, gate, duration, filter;
    bool offersDuration = false; // an older build of the node had no Duration
    void setup() override {
        addParameter(pitch.set("Pitch", {60, 60, 60, 60}, {0}, {127}));
        addParameter(gate.set("Gate", {0, 0, 0, 0}, {0}, {1}));
        addParameter(duration.set("Duration", {1, 1, 1, 1}, {0.1f}, {24}));
        addParameter(filter.set("Filter", {0, 0, 0, 0}, {-1}, {1}));
    }
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override {
        out = {};
        out.pitch = &static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Pitch"));
        out.gate = &static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Gate"));
        if(offersDuration) out.duration = &static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Duration"));
        if(offersDuration) out.filter = &static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Filter"));
        out.voices = 4; out.kit = true;
        out.rowNames = {"Kick", "Snare", "Hat", "Clap"};
        out.rowColors = {ofColor(255, 80, 40), ofColor(40, 200, 120)}; // shorter than voices
        return true;
    }
};
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
int main(int, char**) {
    ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    auto& node = owner->createNode(std::make_unique<KitSynth>(), 1);
    auto& synth = static_cast<KitSynth&>(node.getNodeModel());
    auto& pitch = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Pitch"));
    auto& gate = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Gate"));
    model.setSchedulingEnabled(false);

    // The project as it was saved by the older node: group without Duration.
    const auto t = model.createTrack("Drums"), pb = model.addBinding(t, pitch), gb = model.addBinding(t, gate);
    const auto g = model.createNoteGroup(t, "Kit", pb, gb, "");
    std::string laneId;
    const auto c = model.createNoteGroupClip(t, g, "Beat", 0, 4, &laneId, ofxOceanodeTimelineLaneType::DrumGrid);
    auto* lane = model.getLane(t, c, laneId);
    check(lane != nullptr && lane->drumRows.size() == 4 && lane->drumRows[0].name == "Kick", "Kit rows from the instrument");
    { ofxOceanodeTimelineDrumStep h; h.velocity = 1.0f; h.duration = 8.0f; h.filter = -0.5f; lane->drumRows[0].steps = {h}; }
    { ofxOceanodeTimelineDrumStep h; h.velocity = 1.0f; h.duration = 4.0f; lane->drumRows[1].steps = {h}; }
    { ofxOceanodeTimelineDrumStep h; h.velocity = 1.0f; h.duration = 2.0f; lane->drumRows[2].steps = {h}; }
    auto saved = model.toJson();
    check(saved["tracks"][0]["noteGroups"][0]["duration"] == "", "Saved without a Duration role");
    // Files from before the flag existed have no "durationDecided" at all.
    saved["tracks"][0]["noteGroups"][0].erase("durationDecided");
    saved["tracks"][0]["noteGroups"][0].erase("filterDecided");
    saved["tracks"][0]["noteGroups"][0].erase("filter");

    // The node is updated: it now offers Duration. Load the old project.
    synth.offersDuration = true;
    model.clear();
    model.fromJson(saved);
    model.clearTimelineHistory();
    owner->getTransport()->seekToBeat(0.05);
    owner->getTransport()->play();
    model.update();
    const auto* group = model.getNoteGroup(t, g);
    lane = model.getLane(t, c, laneId);
    check(group != nullptr && !group->durationBindingId.empty() && group->durationDecided, "The group adopted Duration");
    const auto* binding = group == nullptr ? nullptr : model.getBinding(t, group->durationBindingId);
    check(binding != nullptr && binding->parameterPath == owner->getTimelineParameterPath(
              static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Duration"))),
          "Bound to the instrument's Duration parameter");
    check(lane != nullptr && group != nullptr && lane->drumDurationBindingId == group->durationBindingId,
          "Its Drum Grid lane drives Duration");
    const auto& d = synth.duration.get();
    check(d.size() == 4 && d[0] == 8.0f && d[1] == 4.0f && d[2] == 2.0f && d[3] == 1.0f,
          "Duration vector carries the steps' values (8 4 2 1), got " + ofToString(d));
    check(group != nullptr && !group->filterBindingId.empty() && group->filterDecided && lane != nullptr &&
          lane->drumFilterBindingId == group->filterBindingId, "The group adopted Filter as well");
    check(synth.filter.get().size() == 4 && synth.filter.get()[0] == -0.5f && synth.filter.get()[1] == 0.0f,
          "Filter vector carries the steps' values (-0.5 0 ...)");
    check(!model.canUndoTimelineEdit(), "Adoption is not an undo entry");
    // Idempotent: one binding, nothing changes on later frames.
    const auto adopted = model.toJson();
    model.update(); model.update();
    check(model.toJson()["tracks"] == adopted["tracks"], "Adoption runs once");
    size_t durationBindings = 0;
    for(const auto& b : model.getTrack(t)->bindings) if(b.parameterPath == binding->parameterPath) ++durationBindings;
    check(durationBindings == 1, "One Duration binding");
    // Saved, it stays decided.
    check(adopted["tracks"][0]["noteGroups"][0]["durationDecided"] == true, "Saved as decided");

    // Undo keeps working across an adoption that happens mid-session.
    model.clear();
    synth.offersDuration = false;
    model.fromJson(saved);
    model.clearTimelineHistory();
    model.update();
    check(model.getNoteGroup(t, g)->durationBindingId.empty(), "No Duration while the node offers none");
    model.beginTimelineEdit();
    model.getLane(t, c, laneId)->drumSwing = 0.25f;
    model.endTimelineEdit();
    check(model.canUndoTimelineEdit(), "A user edit is recorded");
    synth.offersDuration = true;
    model.update();
    check(!model.getNoteGroup(t, g)->durationBindingId.empty(), "Adopted once the node offers it");
    check(model.undoTimelineEdit(), "Undo still applies after the adoption");
    check(model.getLane(t, c, laneId)->drumSwing == 0.0f, "Undo reverted the user edit");
    check(!model.getNoteGroup(t, g)->durationBindingId.empty() && !model.getLane(t, c, laneId)->drumDurationBindingId.empty(),
          "...and kept the adopted Duration");

    // A role the user cleared stays cleared.
    model.setNoteGroupDuration(t, g, "");
    model.update(); model.update();
    check(model.getNoteGroup(t, g)->durationBindingId.empty() && model.getLane(t, c, laneId)->drumDurationBindingId.empty(),
          "A cleared Duration role is not re-adopted");
    const auto cleared = model.toJson();
    model.clear();
    model.fromJson(cleared);
    model.update();
    check(model.getNoteGroup(t, g)->durationBindingId.empty(), "...also after save and load");
    // Rows take their pad's colour from the instrument, read each time (a
    // reordered kit recolours its rows); slots without one use the fallback.
    {
        int voices = 0; bool kit = false;
        std::vector<std::string> names;
        std::vector<ofColor> colors;
        const auto* row = model.getLane(t, c, laneId);
        check(model.drumGridInstrument(*model.getTrack(t), *row, voices, names, &kit, &colors) && colors.size() == 2,
              "The instrument's row colours reach the timeline");
        const ofColor fallback(10, 20, 30);
        check(ofxOceanodeTimelineDrumGrid::rowColor(colors, row->drumRows[0], fallback) == ofColor(255, 80, 40) &&
              ofxOceanodeTimelineDrumGrid::rowColor(colors, row->drumRows[1], fallback) == ofColor(40, 200, 120) &&
              ofxOceanodeTimelineDrumGrid::rowColor(colors, row->drumRows[2], fallback) == fallback,
              "Each row picks its slot's colour, else the fallback");
        auto moved = row->drumRows[0];
        moved.slot = 1;
        check(ofxOceanodeTimelineDrumGrid::rowColor(colors, moved, fallback) == ofColor(40, 200, 120), "The colour follows the slot");
        check(model.toJson().dump().find("255") == std::string::npos || model.toJson().dump().find("rowColor") == std::string::npos,
              "Colours are not saved with the lane");
    }
    owner->getTransport()->stop();
    if(failures > 0) return 1;
    std::cout << "Drum duration adoption passed: old kit groups adopt the instrument's Duration on load and when it appears, "
                 "steps reach the Duration vector, no undo entry, history intact, cleared roles respected.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
