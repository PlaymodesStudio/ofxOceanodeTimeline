#!/usr/bin/env python3
"""Verify the piano roll's note tools (ofxOceanodeTimelinePianoTools): the tab, the window, what a tool is lent.

With ofxOceanodeSuperCollider beside this addon, its Arpeggiator (the tool
this hook was made for) runs here too: its window, its button, the notes it
writes and rewrites. Headless ImGui, as editor_regression.
"""
import editor_regression as editors
import timeline_regression as fixture
from of_environment import of_root

SC = fixture.ADDON.parent / "ofxOceanodeSuperCollider/src"
ARPEGGIATOR = SC / "ui/scPianoRollArpeggiator.h"

PREAMBLE = editors.CASES.split('int main()')[0]
if ARPEGGIATOR.is_file():
    # (before the preamble's last include: the fixture goes in after that one)
    at = PREAMBLE.index('#include "Nodes/ofxOceanodeNodeModel.h"')
    PREAMBLE = (PREAMBLE[:at] + '#define OFXOCEANODESC_HAS_TIMELINE 1\n#define HAS_ARPEGGIATOR 1\n'
                '#include "ui/scPianoRollArpeggiator.h"\n' + PREAMBLE[at:])

CASES = PREAMBLE + r'''
int main() {
    ofSeedRandom();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 900);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    ofxOceanodeContainer owner;
    TestTimeline model(&owner);
    const auto trackId = model.createTrack("Tools");
    const auto clipId = model.createClip(trackId, "Chords", 0, 8);
    const auto laneId = model.createLane(trackId, clipId, "Roll", ofxOceanodeTimelineLaneType::PianoRoll);
    const auto otherId = model.createLane(trackId, clipId, "Other roll", ofxOceanodeTimelineLaneType::PianoRoll);
    ofxOceanodeTimelineLaneEditors editors;
    auto& piano = static_cast<ofxOceanodeTimelinePianoRollEditor&>(editors.forType(ofxOceanodeTimelineLaneType::PianoRoll));
    auto frame = [&](const std::string& lane, bool focused, ImVec2 mouse = ImVec2(1150, 880), bool down = false, bool tab = false) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 400));
        ImGui::Begin("Editors", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const ofxOceanodeTimelineEditorView view(model, 120.0f, 8.0, true, 0.25);
        std::string focusedLane = focused ? lane : "another-lane";
        ofxOceanodeTimelineLaneEditorContext context{model, owner, model.editSequence(), *model.getTrack(trackId),
                                                    *model.getClip(trackId, clipId), *model.getLane(trackId, clipId, lane), view,
                                                    focusedLane, focused, {}};
        piano.drawToolWindows(context);   // (the shell: before anything of the lane, shown or not)
        if(tab && piano.extraTabLabel() != nullptr) piano.drawExtraTab(context);
        auto* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(ImVec2(170, 100), ImVec2(590, 370), true);
        piano.drawCanvas(context, {ImVec2(0, 100), ImVec2(590, 370), ImVec2(230, 100), 230, 230, 590, false, 1.5});
        dl->PopClipRect();
        ImGui::End();
        ImGui::Render();
    };
    auto* lane = model.getLane(trackId, clipId, laneId);
    lane->pianoNotes = {{0, 2, 60, 0.8f, 1}, {0, 2, 64, 0.8f, 1}, {0, 2, 67, 0.8f, 1}};

    // No tool, no tab.
    assert(ofxOceanodeTimelinePianoTools().empty() && piano.extraTabLabel() == nullptr);
    int draws = 0;
    bool write = false;
    ofxOceanodeTimelineLane* lent = nullptr;
    std::set<int> lentSelection;
    double lentBeats = 0;
    ofxOceanodeTimelineRegisterPianoTool({"Echo", "Ech", "Repeats the selection", [&](ofxOceanodeTimelinePianoToolContext& context) {
        ++draws;
        lent = &context.lane;
        lentSelection = context.selection;
        lentBeats = context.contentBeats;
        if(!write) return;
        context.lane.pianoNotes.push_back({4, 1, 72, 0.5f, 1});
        context.selection = {static_cast<int>(context.lane.pianoNotes.size()) - 1};
    }});
    // One tool: the tab takes its label. Its window is shut until asked for.
    assert(std::string(piano.extraTabLabel()) == "Ech");
    frame(laneId, true, ImVec2(1150, 880), false, true);
    assert(draws == 0);
    // Open: the tool is lent the lane, its selection (stale indices dropped) and the content's length.
    piano.pianoToolLanes["Echo"] = laneId;
    piano.pianoSelectionLaneId = laneId;
    piano.pianoSelectedNoteIndices = {0, 2, 99};
    frame(laneId, true);
    assert(draws == 1 && lent == lane && lentSelection == std::set<int>({0, 2}) && lentBeats == 8);
    // What it writes is in the roll, and what it selects is the roll's selection.
    write = true;
    frame(laneId, true);
    write = false;
    assert(lane->pianoNotes.size() == 4 && lane->pianoNotes.back().pitch == 72);
    assert(piano.pianoSelectedNoteIndices == std::set<int>({3}));
    // One window: while its lane is not in focus it stays on it, without that lane's selection...
    draws = 0;
    frame(otherId, false);
    assert(draws == 0);
    frame(laneId, false);
    assert(draws == 1 && lent == lane);
    // ...and it follows the piano roll that takes the focus.
    frame(otherId, true);
    assert(draws == 2 && lent == model.getLane(trackId, clipId, otherId) && lentSelection.empty());
    frame(laneId, false);
    assert(draws == 2);
    // Registering a name again replaces the tool; a second tool makes the tab "Tools".
    ofxOceanodeTimelineRegisterPianoTool({"Echo", "E2", "", nullptr});
    assert(ofxOceanodeTimelinePianoTools().size() == 1 && std::string(piano.extraTabLabel()) == "E2");
    frame(otherId, true);   // (a tool without contents: an empty window)
    ofxOceanodeTimelineRegisterPianoTool({"Other", "Oth", "", nullptr});
    assert(std::string(piano.extraTabLabel()) == "Tools");
    frame(laneId, true, ImVec2(1150, 880), false, true);
    piano.pianoToolLanes.clear();
    ofxOceanodeTimelinePianoTools().clear();

#ifdef HAS_ARPEGGIATOR
    // ofxOceanodeSuperCollider's Arpeggiator: three notes held 2 beats become 16ths, up.
    scPianoRollArpeggiator::registerTool();
    assert(std::string(piano.extraTabLabel()) == "Arp");
    lane->beatsPerStep = 0.25;
    lane->pianoNotes = {{0, 2, 64, 0.8f, 1}, {0, 2, 60, 0.8f, 1}, {0, 2, 67, 0.8f, 1}, {6, 1, 50, 0.8f, 1}};
    const auto chords = lane->pianoNotes;
    piano.pianoToolLanes["Arpeggiator"] = laneId;
    piano.pianoSelectedNoteIndices = {0, 1, 2};
    // Its window, at a known place; its first button is "Arpeggiate 3 selected notes".
    const char* window = "Arpeggiator###pianoToolArpeggiator";
    for(int i = 0; i < 2; ++i) {
        frame(laneId, true);
        ImGui::SetWindowPos(window, ImVec2(620, 20));
        ImGui::SetWindowSize(window, ImVec2(400, 800));
    }
    frame(laneId, true);
    assert(lane->pianoNotes.size() == 4);   // nothing is written before it is asked
    const auto& style = ImGui::GetStyle();
    const float line = ImGui::GetFontSize();
    const ImVec2 button(820, 20 + line + style.FramePadding.y * 2 + style.WindowPadding.y + line + style.ItemSpacing.y + line * 0.5f + style.FramePadding.y);
    frame(laneId, true, button);
    frame(laneId, true, button, true);
    frame(laneId, true, button);
    auto pitches = [&]() {
        std::vector<int> out;
        for(int index : piano.pianoSelectedNoteIndices) out.push_back(lane->pianoNotes[index].pitch);
        return out;
    };
    // The bass note it was not given stays; the chord is now 8 notes, selected.
    assert(lane->pianoNotes.size() == 9 && lane->pianoNotes[0].pitch == 50);
    assert(pitches() == std::vector<int>({60, 64, 67, 60, 64, 67, 60, 64}));
    assert(lane->pianoNotes[1].startBeat == 0 && lane->pianoNotes[2].startBeat == 0.25 && lane->pianoNotes[1].durationBeats == 0.25);
    // While those notes are there, a setting rewrites them from the chord (here: the lane's grid as the rate).
    lane->beatsPerStep = 0.5;
    frame(laneId, true);
    assert(lane->pianoNotes.size() == 5 && pitches() == std::vector<int>({60, 64, 67, 60}));
    scPianoRollArpeggiator::state().settings.order = arpbox::ROLL_DOWN;
    frame(laneId, true);
    assert(pitches() == std::vector<int>({67, 64, 60, 67}));
    // A note of them edited by hand: the tool lets go, and a setting changes nothing more.
    lane->pianoNotes[2].pitch = 61;
    scPianoRollArpeggiator::state().settings.order = arpbox::ROLL_UP;
    frame(laneId, true);
    assert(lane->pianoNotes.size() == 5 && lane->pianoNotes[2].pitch == 61 && !scPianoRollArpeggiator::state().active);
    assert(scPianoRollArpeggiator::state().chords.size() == 3 && chords.size() == 4);
#else
    std::cout << "ofxOceanodeSuperCollider not found: its Arpeggiator was not run\n";
#endif
    ImGui::DestroyContext();
    return 0;
}
'''

if __name__ == "__main__":
    imgui = of_root() / "addons/ofxImGui/libs/imgui/src"
    folder = fixture.ADDON / "src/Controls/Editors"
    sources = sorted(path for path in folder.glob("*.cpp") if path.name != "ofxOceanodeTimelineEditorView.cpp")
    sources += [imgui / name for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")]
    fixture.main(cases=CASES, extra_sources=sources,
                 extra_includes=[imgui, of_root() / "addons/ofxImGui/src", of_root() / "addons/ofxOceanode/src/Nodes"] +
                                ([SC] if ARPEGGIATOR.is_file() else []))
