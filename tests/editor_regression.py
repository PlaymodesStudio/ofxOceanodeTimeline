#!/usr/bin/env python3
"""Run production lane editors with headless ImGui and real timeline data.

Exercises dispatch, property/canvas/preview drawing, input focus, point and
pattern edits, and balanced draw-list clipping under ASan/UBSan. No GPU/window.
"""
import timeline_regression as fixture
from of_environment import of_root

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include <map>
#include <set>
#define private public
#include "Controls/Editors/ofxOceanodeTimelinePianoRollEditor.h"
#undef private
#include "Controls/Editors/ofxOceanodeTimelineLaneEditors.h"
#include "Controls/Editors/ofxOceanodeTimelineLfoEditor.h"
#include "Controls/Editors/ofxOceanodeTimelineLaneEditorUi.h"
#include "imgui.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
// the piano roll asks Global Pitch (core) about its lane's instrument, by node key
#include "Nodes/ofxOceanodeNodeModel.h"
std::string ofxOceanodeNodeModel::getParents() { return canvasID; }
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
    ofxOceanodeTimelineManager model(&owner);
    const auto trackId = model.createTrack("Editors");
    ofxOceanodeTimelineLaneEditors editors;
    ofxOceanodeTimelineLfoEditor lfoEditor;
    ofxOceanodeTimelineClipEditorState shell;
    std::vector<double> expectedPianoGridBeats;
    auto beginFrame = [&](ImVec2 mouse, bool down) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 850));
        ImGui::Begin("Editors", nullptr, ImGuiWindowFlags_NoDecoration);
    };
    auto endFrame = [&]() { ImGui::End(); ImGui::Render(); };
    auto frame = [&](const std::string& clipId, const std::string& laneId,
                     bool focused, ImVec2 mouse = ImVec2(900, 700), bool down = false,
                     bool properties = false) {
        beginFrame(mouse, down);
        auto& track = *model.getTrack(trackId);
        auto& clip = *model.getClip(trackId, clipId);
        auto& lane = *model.getLane(trackId, clipId, laneId);
        const ofxOceanodeTimelineEditorView view(model, 120.0f, 8.0, true, 0.25);
        std::string focusedLane = focused ? laneId : "another-lane";
        ofxOceanodeTimelineLaneEditorContext context{model, owner, track, clip, lane, view,
                                                    focusedLane, focused, {}};
        auto& editor = editors.forType(lane.type);
        if(properties) {
            ImGui::BeginChild("Properties", ImVec2(210, 650));
            editor.drawProperties(context);
            ImGui::EndChild();
        }
        ImGui::SetCursorScreenPos(ImVec2(230, 100));
        ImGui::InvisibleButton("Canvas", ImVec2(640, 270));
        auto* dl = ImGui::GetWindowDrawList();
        const int depth = dl->_ClipRectStack.Size;
        dl->PushClipRect(ImVec2(170, 100), ImVec2(870, 370), true);
        editor.drawCanvas(context, {ImVec2(0, 100), ImVec2(870, 370), ImVec2(230, 100),
                                   230, 230, 870, ImGui::IsItemHovered(), 1.5});
        for(double beat : expectedPianoGridBeats) {
            const float x = 230 + view.beatToPixels(beat);
            const bool bar = std::fmod(beat, model.getBeatsPerBar()) < 0.001;
            const bool quarter = std::fmod(beat, 1.0) < 0.001;
            const ImU32 color = bar ? ofxOceanodeTimelineLaneEditorUi::kBar :
                quarter ? IM_COL32(92, 92, 92, 145) : ofxOceanodeTimelineLaneEditorUi::kGrid;
            bool found = false;
            for(const auto& vertex : dl->VtxBuffer)
                if(vertex.col == color && std::abs(vertex.pos.x - x) < 2.1f &&
                   std::abs(vertex.pos.y - 108.0f) < 1.0f) found = true;
            assert(found); // absolute timeline lines, including the meter's bar
        }
        assert(dl->_ClipRectStack.Size == depth + 1);
        dl->PopClipRect();
        const auto before = model.toJson();
        editor.drawPreview({model, owner, track, clip, &lane, view,
                            ImVec2(230, 400), ImVec2(870, 450), 230, 870, 0, 2});
        assert(dl->_ClipRectStack.Size == depth);
        assert(model.toJson() == before); // miniatures never mutate the project
        endFrame();
    };
    beginFrame(ImVec2(900, 700), false); endFrame();
    // Draw each concrete editor, including repeated/stretched clip content.
    for(auto type : {ofxOceanodeTimelineLaneType::Step, ofxOceanodeTimelineLaneType::Curve,
                    ofxOceanodeTimelineLaneType::PianoRoll, ofxOceanodeTimelineLaneType::Number,
                    ofxOceanodeTimelineLaneType::Dropdown, ofxOceanodeTimelineLaneType::MultiSlider,
                    ofxOceanodeTimelineLaneType::MultiValue, ofxOceanodeTimelineLaneType::MultiGate}) {
        const auto clipId = model.createClip(trackId, "Content", 0, 8);
        const auto laneId = model.createLane(trackId, clipId, "Content", type);
        auto& clip = *model.getClip(trackId, clipId);
        clip.contentDurationBeats = 2;
        clip.contentStretch = 1.5;
        auto& lane = *model.getLane(trackId, clipId, laneId);
        lane.curvePoints = {{0, 0.2f}, {1, 0.8f}};
        lane.pianoNotes = {{0, 0.5, 60, 0.8f, 1.0f}};
        lane.multiValueRows.resize(4); lane.multiValueRows[0].push_back({0, 1, 0.5f});
        lane.multiGateRows.resize(4); lane.multiGateRows[0].push_back({0, 1});
        model.setClipStep(trackId, clipId, laneId, 0, "1", 0.25);
        frame(clipId, laneId, true, ImVec2(900, 700), false, true);
        frame(clipId, laneId, false);
    }
    // A moved, stretched piano clip must not shift or rescale timeline lines.
    const auto pianoClip = model.createClip(trackId, "Grid", 0.37, 8);
    const auto pianoLane = model.createLane(trackId, pianoClip, "Grid", ofxOceanodeTimelineLaneType::PianoRoll);
    model.getClip(trackId, pianoClip)->contentStretch = 1.7;
    model.getClip(trackId, pianoClip)->contentDurationBeats = 2;
    model.getLane(trackId, pianoClip, pianoLane)->beatsPerStep = 1;
    model.setTimeSignature(7, 8);
    expectedPianoGridBeats = {0.25, 0.5, 1.0, 3.5};
    frame(pianoClip, pianoLane, false);
    expectedPianoGridBeats.clear();
    model.setTimeSignature(4, 4);
    // Right-clicking keyboard keys selects every matching pitch, even notes
    // outside the visible source window, without changing content or auditioning.
    auto* pitchLane=model.getLane(trackId,pianoClip,pianoLane);
    pitchLane->pianoLowPitch=60; pitchLane->pianoHighPitch=63;
    pitchLane->pianoNotes={{0,0.5,60,0.8f,1},{1,0.5,61,0.8f,1},
        {2,0.5,60,0.8f,1},{20,0.5,60,0.8f,1}};
    auto& pianoEditor=static_cast<ofxOceanodeTimelinePianoRollEditor&>(editors.forType(ofxOceanodeTimelineLaneType::PianoRoll));
    auto selectPitch=[&](float y,bool focused=true) {
        const ImVec2 point(220,y);
        frame(pianoClip,pianoLane,focused,point,false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right,true); frame(pianoClip,pianoLane,focused,point,false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right,false); frame(pianoClip,pianoLane,true,point,false);
    };
    const auto pitchBefore=model.toJson();
    // Roll 108..278 (VEL, PROB and MUT strips below it): 42.5 px per pitch,
    // so 60, 61 and 62 are centred at 256.75, 214.25 and 171.75.
    selectPitch(256.75f,false);
    assert((pianoEditor.pianoSelectedNoteIndices==std::set<int>{0,2,3}));
    assert(pianoEditor.pianoSelectionLaneId==pianoLane && !pianoEditor.pianoKeyboardPreviewActive);
    io.AddKeyEvent(ImGuiMod_Shift,true); selectPitch(214.25f);
    assert((pianoEditor.pianoSelectedNoteIndices==std::set<int>{0,1,2,3}));
    io.AddKeyEvent(ImGuiMod_Shift,false); selectPitch(214.25f);
    assert((pianoEditor.pianoSelectedNoteIndices==std::set<int>{1}));
    selectPitch(171.75f); assert(pianoEditor.pianoSelectedNoteIndices.empty());
    assert(model.toJson()==pitchBefore);
    // Mouse input is routed only to the focused editor.
    const auto stepClip = model.createClip(trackId, "Step", 0, 4);
    const auto stepLane = model.createLane(trackId, stepClip, "Step", ofxOceanodeTimelineLaneType::Step);
    frame(stepClip, stepLane, false);
    frame(stepClip, stepLane, false, ImVec2(250, 200), true);
    assert(model.getLane(trackId, stepClip, stepLane)->step.steps.empty());
    frame(stepClip, stepLane, true, ImVec2(250, 200), false);
    frame(stepClip, stepLane, true, ImVec2(250, 200), true);
    assert(!model.getLane(trackId, stepClip, stepLane)->step.steps.empty());
    frame(stepClip, stepLane, true, ImVec2(250, 200), false);
    const auto curveClip = model.createClip(trackId, "Curve", 0, 4);
    const auto curveLane = model.createLane(trackId, curveClip, "Curve", ofxOceanodeTimelineLaneType::Curve);
    frame(curveClip, curveLane, true);
    const size_t initialPoints = model.getLane(trackId, curveClip, curveLane)->curvePoints.size();
    frame(curveClip, curveLane, true, ImVec2(270, 230), true);
    assert(model.getLane(trackId, curveClip, curveLane)->curvePoints.size() > initialPoints);
    frame(curveClip, curveLane, true, ImVec2(270, 230), false);
    const auto sliderClip = model.createClip(trackId, "Slider", 0, 4);
    const auto sliderLane = model.createLane(trackId, sliderClip, "Slider", ofxOceanodeTimelineLaneType::MultiSlider);
    frame(sliderClip, sliderLane, true);
    frame(sliderClip, sliderLane, true, ImVec2(250, 200), true);
    const auto& values = model.getLane(trackId, sliderClip, sliderLane)->multiSliderValues;
    assert(std::any_of(values.begin(), values.end(), [](float v) { return v > 0; }));
    frame(sliderClip, sliderLane, true, ImVec2(250, 200), false);
    editors.clearSelection();
    editors.forgetTrack(trackId);
    // LFO owns its selection and reports shell requests without a controller.
    const auto lfoClip = model.createLfoClip(trackId, "", "LFO", 0, 4);
    for(bool folded : {false, true}) {
        if(folded) shell.foldedClipEditors.insert(lfoClip);
        beginFrame(ImVec2(1000, 800), false);
        auto& clip = *model.getClip(trackId, lfoClip);
        const ofxOceanodeTimelineEditorView view(model, 120.0f, 8.0, true, 0.25);
        shell.editorTrackId = trackId; shell.editorClipId = lfoClip;
        assert(!lfoEditor.draw(model, owner, *model.getTrack(trackId), clip, view, shell, 1000, 1));
        lfoEditor.drawPreview({model, owner, *model.getTrack(trackId), clip, nullptr, view,
                              ImVec2(230, 750), ImVec2(870, 800), 230, 870, 0, 2});
        assert(!lfoEditor.hasPointSelection(lfoClip));
        endFrame();
    }

    // VEL / PROB / MUT strips sit 26 px higher than VEL / PROB alone did
    // (MUT takes the bottom). VEL / PROB painting follows the swept mouse segment, not just the
    // initially grabbed note. Fast strokes include skipped handles and chords.
    const auto paintClip=model.createClip(trackId,"Paint",0,4);
    const auto paintLane=model.createLane(trackId,paintClip,"Paint",ofxOceanodeTimelineLaneType::PianoRoll);
    model.getClip(trackId,paintClip)->repeatContent=false;
    model.getLane(trackId,paintClip,paintLane)->pianoNotes={
        {0,0.5,60,0.9f,1},{1,0.5,62,0.9f,1},{2,0.5,64,0.9f,1},
        {3,0.5,65,0.9f,1},{3.5,0.5,67,0.9f,1},{1,0.5,69,0.9f,1}};
    const ofxOceanodeTimelineEditorView paintView(model,120,8,true,0.25);
    auto px=[&](double beat){return 233.0f+paintView.beatToPixels(beat);};
    auto paintFrame=[&](float x,float y,bool down){frame(paintClip,paintLane,true,ImVec2(x,y),down);};
    auto close=[](float a,float b){if(std::abs(a-b)>=0.001f) std::cerr<<"Paint expected "<<b<<", got "<<a<<"\n"; assert(std::abs(a-b)<0.001f);};
    paintFrame(px(0),304.0f,false); paintFrame(px(0),304.0f,true);
    paintFrame(px(3),291.0f,true); paintFrame(px(3),291.0f,false);
    auto& painted=model.getLane(trackId,paintClip,paintLane)->pianoNotes;
    for(int i=0;i<4;++i) close(painted[i].velocity,(4.0f+i*(13.0f/3))/22.0f);
    close(painted[4].velocity,0.9f); close(painted[5].velocity,(4.0f+13.0f/3)/22.0f);
    for(const auto& note:painted) close(note.probability,1);
    // Reverse strokes interpolate probability without changing velocity.
    paintFrame(px(3),336.0f,false); paintFrame(px(3),336.0f,true);
    paintFrame(px(0),318.0f,true); paintFrame(px(0),318.0f,false);
    for(int i=0;i<4;++i) { close(painted[i].probability,(20.0f-i*6.0f)/23.0f); close(painted[i].velocity,(4.0f+i*(13.0f/3))/22.0f); }
    close(painted[4].probability,1); close(painted[5].probability,painted[1].probability);
    // Starting between handles still paints subsequent notes. A release ends
    // the gesture, so later mouse movement cannot change a value.
    paintFrame(px(0.3),297.0f,false); paintFrame(px(0.3),297.0f,true);
    paintFrame(px(3),297.0f,true); paintFrame(px(3),297.0f,false);
    close(painted[0].velocity,4.0f/22.0f);
    for(int i=1;i<4;++i) close(painted[i].velocity,0.5f);
    const auto afterPaint=model.toJson(); paintFrame(px(1),288.0f,false); assert(model.toJson()==afterPaint);
    // Repeated, stretched copies edit their common source notes.
    model.getClip(trackId,paintClip)->contentDurationBeats=1;
    model.getClip(trackId,paintClip)->contentStretch=2;
    model.getClip(trackId,paintClip)->repeatContent=true;
    painted={{0.2,0.2,60,0.9f,1},{0.8,0.2,64,0.9f,1}};
    paintFrame(px(2.4),297.0f,false); paintFrame(px(2.4),297.0f,true);
    paintFrame(px(3.6),297.0f,true); paintFrame(px(3.6),297.0f,false);
    close(painted[0].velocity,0.5f); close(painted[1].velocity,0.5f);

    // Dragging a selected handle scales selected notes from a frozen snapshot,
    // preserving their ratios even across frames, saturation and horizontal travel.
    model.getClip(trackId,paintClip)->repeatContent=false;
    model.getClip(trackId,paintClip)->contentDurationBeats=4;
    model.getClip(trackId,paintClip)->contentStretch=1;
    painted={{0,0.5,60,0.4f,0.4f},{1,0.5,64,0.2f,0.2f},{2,0.5,67,0.7f,0.7f}};

    pianoEditor.pianoSelectionLaneId=paintLane;
    pianoEditor.pianoSelectedNoteIndices={0,1};
    paintFrame(px(0),299.0f,false); paintFrame(px(0),299.0f,true);
    close(painted[0].velocity,0.4f); close(painted[1].velocity,0.2f);
    paintFrame(px(2),295.0f,true);
    close(painted[0].velocity,0.4f+4.0f/22); close(painted[1].velocity,0.2f+2.0f/22); close(painted[2].velocity,0.7f);
    paintFrame(px(2),290.0f,true);
    close(painted[0].velocity,0.4f+9.0f/22); close(painted[1].velocity,0.2f+4.5f/22);
    paintFrame(px(2),290.0f,false);
    paintFrame(px(0),329.0f,false); paintFrame(px(0),329.0f,true);
    paintFrame(px(2),324.0f,true); paintFrame(px(2),324.0f,false);
    close(painted[0].probability,0.4f+5.0f/23); close(painted[1].probability,0.2f+2.5f/23); close(painted[2].probability,0.7f);
    close(painted[0].velocity,0.4f+9.0f/22); close(painted[1].velocity,0.2f+4.5f/22);
    // Zero-valued anchors use a shared offset and can be raised from zero.
    painted[0].velocity=0; painted[1].velocity=0.2f;
    paintFrame(px(0),307,false); paintFrame(px(0),307,true);
    paintFrame(px(0),296,true); paintFrame(px(0),296,false);
    close(painted[0].velocity,0.5f); close(painted[1].velocity,0.7f);
    {
        // MUT (342..364) paints the mutation amount and leaves VEL / PROB alone.
        const auto velocities=painted[0].velocity, probabilities=painted[0].probability;
        const auto selection=pianoEditor.pianoSelectedNoteIndices;
        pianoEditor.pianoSelectedNoteIndices.clear(); // paint, not scale a selection
        paintFrame(px(0),360.0f,false); paintFrame(px(0),360.0f,true);
        paintFrame(px(0),360.0f,false);
        close(painted[0].mutation,4.0f/19.0f);
        close(painted[0].velocity,velocities); close(painted[0].probability,probabilities);
        pianoEditor.pianoSelectedNoteIndices=selection;
    }
    // Delete is owned by the focused selection even with the mouse outside the roll.
    io.AddKeyEvent(ImGuiKey_Delete,true);
    paintFrame(950,700,false);
    assert(painted.size()==1 && painted.front().pitch==67);
    assert(model.getClip(trackId,paintClip));
    io.AddKeyEvent(ImGuiKey_Delete,false); paintFrame(950,700,false);

    // Live edit sound uses real routed pitch/gate values and releases when the
    // gesture ends, including when the editor closes while a note is held.
    ofParameter<float> monitorPitch("Monitor Pitch",72,0,127), monitorGate("Monitor Gate",0,0,1);
    ofxOceanodeParameter<float> mp,mg; mp.bindParameter(monitorPitch); mg.bindParameter(monitorGate);
    owner.targets[mp.getEscapedName()]=&mp; owner.targets[mg.getEscapedName()]=&mg;
    const auto mpb=model.addBinding(trackId,mp), mgb=model.addBinding(trackId,mg);
    const auto mc=model.createClip(trackId,"Monitor",0,4);
    const auto ml=model.createLane(trackId,mc,"Monitor",ofxOceanodeTimelineLaneType::PianoRoll);
    model.addBindingToLane(trackId,mc,ml,mpb); model.addBindingToLane(trackId,mc,ml,mgb);
    auto* monitor=model.getLane(trackId,mc,ml);
    monitor->pianoLowPitch=60; monitor->pianoHighPitch=63;
    assert(!monitor->pianoLiveEditSound);
    monitor->pianoLiveEditSound=true;
    // Pitch rows as in the keyboard test above: 60 at y 256, 61 at y 214.
    auto monitorFrame=[&](float x,float y,bool down){frame(mc,ml,true,ImVec2(x,y),down); model.update();};
    monitorFrame(265,256,false); monitorFrame(265,256,true);
    close(monitorPitch,60); close(monitorGate,1);
    monitorFrame(265,256,false); close(monitorGate,0);
    monitorFrame(270,256,true); close(monitorGate,1);
    monitorFrame(300,214,true); close(monitorPitch,61); close(monitorGate,1);
    monitorFrame(300,214,false); close(monitorGate,0);
    monitor->pianoLiveEditSound=false;
    monitorFrame(310,214,true); model.update(); close(monitorGate,0);
    monitorFrame(310,214,false);
    monitor->pianoLiveEditSound=true;
    monitorFrame(400,256,true); close(monitorGate,1);
    pianoEditor.updateMonitoring(model,""); model.update(); close(monitorGate,0);
    monitorFrame(400,256,false);
    const auto monitorSaved=model.toJson(); model.fromJson(monitorSaved);
    assert(model.getLane(trackId,mc,ml)->pianoLiveEditSound);
    auto monitorLegacy=monitorSaved;
    for(auto& tr:monitorLegacy["tracks"]) for(auto& clip:tr["clips"]) for(auto& ln:clip["lanes"]) ln.erase("pianoLiveEditSound");
    model.fromJson(monitorLegacy); assert(!model.getLane(trackId,mc,ml)->pianoLiveEditSound);
    ImGui::DestroyContext();
    std::cout << "Editor regression passed: all lane types, LFO, properties, previews, clip-stack balance, focus routing, step/curve/slider input.\n";
}
'''

if __name__ == "__main__":
    imgui = of_root() / "addons/ofxImGui/libs/imgui/src"
    editors = fixture.ADDON / "src/Controls/Editors"
    sources = sorted(path for path in editors.glob("*.cpp")
                     if path.name != "ofxOceanodeTimelineEditorView.cpp")
    sources += [imgui / name for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")]
    fixture.main(cases=CASES, extra_sources=sources,
                 extra_includes=[imgui, of_root() / "addons/ofxImGui/src"])
