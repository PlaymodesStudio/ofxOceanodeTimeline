#!/usr/bin/env python3
"""Run production lane editors with headless ImGui and real timeline data.

Exercises dispatch, property/canvas/preview drawing, input focus, point and
pattern edits, and balanced draw-list clipping under ASan/UBSan. No GPU/window.
"""
import timeline_regression as fixture
from of_environment import of_root

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Controls/Editors/ofxOceanodeTimelineLaneEditors.h"
#include "Controls/Editors/ofxOceanodeTimelineLfoEditor.h"
#include "Controls/Editors/ofxOceanodeTimelineLaneEditorUi.h"
#include "imgui.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
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
