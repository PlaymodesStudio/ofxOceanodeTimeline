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
#include "Controls/Editors/ofxOceanodeTimelineDrumGridEditor.h"
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
    {
        // A point added inside a shaped segment keeps its bend on both sides,
        // in Sigmoid and in Log/Exp.
        for(const char* mode : {"Sigmoid", "Log / Exp"}) {
            ofxOceanodeTimelineLane curve;
            curve.type = ofxOceanodeTimelineLaneType::Curve;
            curve.curveInterpolation = mode;
            curve.curvePoints = {{0.0, 0.0f}, {4.0, 1.0f}, {8.0, 0.0f}};
            curve.curveTensions = {{0.3f, 4.0f}, {0.7f, 0.4f}};
            const auto original = curve;
            auto at = [](const ofxOceanodeTimelineLane& lane, double beat) {
                return ofxOceanodeTimelineCurve::valueAtBeat(lane.curvePoints, lane.curveTensions, lane.curveInterpolation, beat);
            };
            const float onCurve = at(original, 1.5);
            const int index = ofxOceanodeTimelineLaneEditorUi::insertCurvePoint(curve, {1.5, onCurve});
            assert(index == 1 && curve.curvePoints.size() == 4 && curve.curveTensions.size() == 3);
            // Neither half fell back to a straight line, and the untouched segment kept its shape.
            for(int s = 0; s < 2; ++s)
                assert(std::abs(curve.curveTensions[s].steepness - 1.0f) > 0.05f ||
                       std::abs(curve.curveTensions[s].inflection - 0.5f) > 0.05f);
            assert(curve.curveTensions[2].inflection == 0.7f && curve.curveTensions[2].steepness == 0.4f);
            // The curve still follows the original shape closely on both sides of the new point.
            for(double beat : {0.4, 0.9, 1.2, 2.0, 2.8, 3.5, 5.0, 7.0})
                if(std::abs(at(curve, beat) - at(original, beat)) > 0.03f) {
                    std::cerr << mode << ": at beat " << beat << " " << at(curve, beat) << " vs " << at(original, beat) << "\n";
                    std::abort();
                }
        }
        // A gate block drawn over others joins them; blocks that only touch stay apart.
        std::vector<ofxOceanodeTimelineGateRegion> gates{{4, 2, 0.5f}, {0, 1, 1.0f}, {1, 1, 0.8f}, {5, 2, 0.3f}, {5.5, 0.5, 1.0f}};
        ofxOceanodeTimelineMergeOverlappingGates(gates);
        assert(gates.size() == 3);
        assert(gates[0].startBeat == 0 && gates[0].durationBeats == 1);
        assert(gates[1].startBeat == 1 && gates[1].durationBeats == 1); // back to back: retriggers
        assert(gates[2].startBeat == 4 && gates[2].durationBeats == 3 && gates[2].velocity == 0.5f);
        // Linear stays linear; a point after the last one adds a neutral segment.
        ofxOceanodeTimelineLane line;
        line.curveInterpolation = "Linear";
        line.curvePoints = {{0.0, 0.0f}, {4.0, 1.0f}};
        line.curveTensions = {{}};
        ofxOceanodeTimelineLaneEditorUi::insertCurvePoint(line, {2.0, 0.5f});
        ofxOceanodeTimelineLaneEditorUi::insertCurvePoint(line, {6.0, 0.0f});
        assert(line.curveTensions.size() == 3);
        for(const auto& tension : line.curveTensions) assert(tension.inflection == 0.5f && tension.steepness == 1.0f);
    }
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
            if(editor.extraTabLabel() != nullptr) editor.drawExtraTab(context);
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
                    ofxOceanodeTimelineLaneType::MultiValue, ofxOceanodeTimelineLaneType::MultiGate,
                    ofxOceanodeTimelineLaneType::DrumGrid}) {
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
    // The velocity strip (312..360 here) sets a sounding step's velocity,
    // and a drag there leaves its probability alone.
    const float stepProbability = model.getLane(trackId, stepClip, stepLane)->step.steps[0].probability;
    frame(stepClip, stepLane, true, ImVec2(250, 336), true);
    frame(stepClip, stepLane, true, ImVec2(250, 336), false);
    assert(std::abs(model.getLane(trackId, stepClip, stepLane)->step.steps[0].velocity - 0.5f) < 0.001f);
    assert(model.getLane(trackId, stepClip, stepLane)->step.steps[0].probability == stepProbability);
    // Toggle: the same strip (316..364) under each gate.
    const auto toggleClip = model.createClip(trackId, "Toggle", 0, 4);
    const auto toggleLane = model.createLane(trackId, toggleClip, "Toggle", ofxOceanodeTimelineLaneType::MultiGate);
    model.getLane(trackId, toggleClip, toggleLane)->multiRowCount = 1;
    model.getLane(trackId, toggleClip, toggleLane)->multiGateRows = {{{0.0, 1.0}}};
    frame(toggleClip, toggleLane, true, ImVec2(250, 340), false);
    frame(toggleClip, toggleLane, true, ImVec2(250, 340), true);
    frame(toggleClip, toggleLane, true, ImVec2(250, 340), false);
    const auto& toggleRow = model.getLane(trackId, toggleClip, toggleLane)->multiGateRows[0];
    assert(toggleRow.size() == 1 && std::abs(toggleRow[0].velocity - 0.5f) < 0.001f);
    assert(toggleRow[0].startBeat == 0.0 && toggleRow[0].durationBeats == 1.0);
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
    {
        // Drum Grid: 4 rows of 43 px from y 104 (row 1 from 147, row 2 from 190,
        // row 3 from 233), 1/16 cells of 17.5 px from x 230; under them the
        // strip: parameter tabs (282..301), bars (303..358).
        const auto drumClip = model.createClip(trackId, "Drums", 0, 4);
        const auto drumLane = model.createLane(trackId, drumClip, "Drums", ofxOceanodeTimelineLaneType::DrumGrid);
        auto drums = [&]() -> ofxOceanodeTimelineLane& { return *model.getLane(trackId, drumClip, drumLane); };
        auto click = [&](ImVec2 at) {
            frame(drumClip, drumLane, true, at, false);
            frame(drumClip, drumLane, true, at, true);
            frame(drumClip, drumLane, true, at, false);
        };
        assert(drums().drumRows.size() == 4 && drums().drumRows[0].length == 4.0);
        frame(drumClip, drumLane, false, ImVec2(238, 120), false);
        frame(drumClip, drumLane, false, ImVec2(238, 120), true);   // unfocused: no edit
        frame(drumClip, drumLane, false, ImVec2(238, 120), false);
        assert(drums().drumRows[0].steps.empty());
        click(ImVec2(238, 120));                                    // click adds a hit on the cell
        assert(drums().drumRows[0].steps.size() == 1 && drums().drumRows[0].steps[0].beat == 0.0 &&
               drums().drumRows[0].steps[0].length == 0.25);
        click(ImVec2(238, 120));                                    // ...and removes it
        assert(drums().drumRows[0].steps.empty());
        frame(drumClip, drumLane, true, ImVec2(238, 170), true);    // row 1, cell 0
        frame(drumClip, drumLane, true, ImVec2(300, 170), true);    // paint through cell 4
        frame(drumClip, drumLane, true, ImVec2(300, 170), false);
        assert(drums().drumRows[1].steps.size() == 5);
        for(int i = 0; i <= 4; ++i) assert(drums().drumRows[1].steps[i].beat == i * 0.25);
        assert(drums().drumRows[0].steps.empty());
        // Velocity strip of the focused row (row 1): half height = 0.5, on the hit under x.
        frame(drumClip, drumLane, true, ImVec2(256, 330.4f), true);
        frame(drumClip, drumLane, true, ImVec2(256, 330.4f), false);
        assert(std::abs(drums().drumRows[1].steps[1].velocity - 0.5f) < 0.02f);
        assert(std::abs(drums().drumRows[1].steps[0].velocity - 0.8f) < 1e-6f);
        // Tabs carry full words, laid out from the zone's left edge: pick Duration.
        float tabX = 232.0f, durationTab = 0.0f;
        for(const char* name : {"Velocity", "Pitch", "Probability", "Ratchet", "Gate", "Duration"}) {
            const float w = ImGui::CalcTextSize(name).x + 16.0f;
            if(std::string(name) == "Duration") durationTab = tabX + w * 0.5f;
            tabX += w + 3.0f;
        }
        click(ImVec2(durationTab, 291.0f));
        // Log bar over x0.1 .. x24: x2 sits at log2(20) / log2(240) of the height.
        const float twoY = 358.0f - std::log2(20.0f) / std::log2(240.0f) * 55.2f;
        frame(drumClip, drumLane, true, ImVec2(256, twoY), true);
        frame(drumClip, drumLane, true, ImVec2(256, twoY), false);
        assert(drums().drumRows[1].steps[1].duration == 2.0f);
        assert(drums().drumRows[1].steps[1].velocity < 0.6f); // still the velocity painted before
        // Filter tab (after Duration): up is high-pass, double-click a bar opens it.
        {
            float filterTab = 0.0f, x = 232.0f;
            for(const char* name : {"Velocity", "Pitch", "Probability", "Ratchet", "Gate", "Duration", "Filter"}) {
                const float w = ImGui::CalcTextSize(name).x + 16.0f;
                if(std::string(name) == "Filter") filterTab = x + w * 0.5f;
                x += w + 3.0f;
            }
            click(ImVec2(filterTab, 291.0f));
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), true);
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), false);
            assert(drums().drumRows[1].steps[1].filter > 0.7f && drums().drumRows[1].steps[1].filter < 0.95f);
            frame(drumClip, drumLane, true, ImVec2(256, 340.0f), true);   // below the centre: low-pass
            frame(drumClip, drumLane, true, ImVec2(256, 340.0f), false);
            assert(drums().drumRows[1].steps[1].filter < -0.3f);
            frame(drumClip, drumLane, true, ImVec2(256, 331.0f), true);   // near the centre: snaps open
            frame(drumClip, drumLane, true, ImVec2(256, 331.0f), false);
            assert(drums().drumRows[1].steps[1].filter == 0.0f);
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), true);
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), false);
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), true);   // double-click: open
            frame(drumClip, drumLane, true, ImVec2(256, 308.0f), false);
            assert(drums().drumRows[1].steps[1].filter == 0.0f);
            // Stutter (after Filter): notes on 0..127 from the bottom, semitone
            // snap, the bottom and a double-click are off.
            float stutterTab = 0.0f, sx = 232.0f;
            for(const char* name : {"Velocity", "Pitch", "Probability", "Ratchet", "Gate", "Duration", "Filter", "Stutter"}) {
                const float w = ImGui::CalcTextSize(name).x + 16.0f;
                if(std::string(name) == "Stutter") stutterTab = sx + w * 0.5f;
                sx += w + 3.0f;
            }
            click(ImVec2(stutterTab, 291.0f));
            const float halfY = 358.0f - 0.5f * (358.0f - 302.8f);       // note 63.5 -> 64
            frame(drumClip, drumLane, true, ImVec2(256, halfY), true);
            frame(drumClip, drumLane, true, ImVec2(256, halfY), false);
            const float note = drums().drumRows[1].steps[1].stutter;
            assert(note >= 62.0f && note <= 65.0f && note == std::round(note));
            click(ImVec2(256, 357.0f));                                   // the bottom pixels: off
            assert(drums().drumRows[1].steps[1].stutter == 0.0f);
            frame(drumClip, drumLane, true, ImVec2(256, halfY), false);
            frame(drumClip, drumLane, true, ImVec2(256, halfY), true);
            frame(drumClip, drumLane, true, ImVec2(256, halfY), false);
            frame(drumClip, drumLane, true, ImVec2(256, halfY), true);    // double-click: off
            frame(drumClip, drumLane, true, ImVec2(256, halfY), false);
            assert(drums().drumRows[1].steps[1].stutter == 0.0f);
        }
        // Dragging a hit sideways moves it by grid steps: 1.0 -> 1.5.
        frame(drumClip, drumLane, true, ImVec2(305, 170), false);
        frame(drumClip, drumLane, true, ImVec2(305, 170), true);
        frame(drumClip, drumLane, true, ImVec2(340, 170), true);
        frame(drumClip, drumLane, true, ImVec2(340, 170), false);
        assert(drums().drumRows[1].steps.size() == 5 && drums().drumRows[1].steps.back().beat == 1.5);
        // A hit off the grid (a 1/32 on a 1/16 grid) is drawn and edited where
        // it is: clicking its cell removes it rather than stacking another.
        ofxOceanodeTimelineDrumStep offGrid;
        offGrid.beat = 0.125; offGrid.length = 0.125;
        ofxOceanodeTimelineDrumGrid::addStep(drums().drumRows[2], offGrid);
        frame(drumClip, drumLane, true, ImVec2(900, 700), false);
        click(ImVec2(240, 210));                                    // beat 0.14, inside cell 0
        assert(drums().drumRows[2].steps.empty());
        ofxOceanodeTimelineDrumGrid::addStep(drums().drumRows[2], offGrid);
        click(ImVec2(234, 210));                                    // beat 0.06: the cell still holds it
        assert(drums().drumRows[2].steps.empty());
        // A following lane picks up a master change at once: on 1/32, a click
        // at beat 0.15 places a hit at 0.125 (no stale 1/16 step).
        model.setMasterGridBeats(0.125);
        click(ImVec2(240.5f, 120));                                 // row 0, beat 0.15
        assert(drums().drumRows[0].steps.size() == 1 && drums().drumRows[0].steps[0].beat == 0.125 &&
               drums().drumRows[0].steps[0].length == 0.125);
        // ...and moves by 1/32 steps: dragged three 1/32 cells (3 x 8.75 px) to
        // the right it lands on 0.125 + 3 x 0.125 = 0.5.
        frame(drumClip, drumLane, true, ImVec2(241.0f, 120), false);
        frame(drumClip, drumLane, true, ImVec2(241.0f, 120), true);
        frame(drumClip, drumLane, true, ImVec2(241.0f + 3.0f * 8.75f, 120), true);
        frame(drumClip, drumLane, true, ImVec2(241.0f + 3.0f * 8.75f, 120), false);
        assert(drums().drumRows[0].steps.size() == 1 && std::abs(drums().drumRows[0].steps[0].beat - 0.5) < 1e-9);
        drums().drumRows[0].steps.clear();
        // A dotted-8th grid places hits at 0.75-beat steps.
        model.setMasterGridBeats(0.75);
        click(ImVec2(286, 250));                                    // row 3, beat 0.8 -> cell [0.75, 1.5)
        assert(drums().drumRows[3].steps.size() == 1 && drums().drumRows[3].steps[0].beat == 0.75 &&
               drums().drumRows[3].steps[0].length == 0.75);
        // ...and the 1/16 hits of row 1 keep their places.
        for(int i = 0; i < 4; ++i) assert(drums().drumRows[1].steps[i].beat == i * 0.25);
        model.setMasterGridBeats(0.25);
        // Row length on the canvas: drag row 0's end-of-cycle handle (at the
        // clip end, x 510) from 16 to 5 sixteenths (x 317.5): 1.25 beats.
        drums().drumRows[0].steps.clear();
        ofxOceanodeTimelineDrumStep kick; kick.beat = 0.0; kick.length = 0.25;
        ofxOceanodeTimelineDrumGrid::addStep(drums().drumRows[0], kick);
        frame(drumClip, drumLane, true, ImVec2(510, 120), false);
        frame(drumClip, drumLane, true, ImVec2(510, 120), true);
        frame(drumClip, drumLane, true, ImVec2(330, 120), true);
        frame(drumClip, drumLane, true, ImVec2(317.5f, 120), true);
        frame(drumClip, drumLane, true, ImVec2(317.5f, 120), false);
        assert(std::abs(drums().drumRows[0].length - 1.25) < 1e-9);
        {
            float level = 0; int pitch = 0;
            auto open = [&](double beat) {
                return ofxOceanodeTimelineDrumGrid::rowStateAt(drums(), drums().drumRows[0], beat, 0, 4.0, 0.0, 0, level, pitch);
            };
            assert(open(0.01) && open(1.26) && open(2.51) && !open(1.01) && !open(2.0));
        }
        // ...and its thick end line is drawn there in the row's colour (the track's here).
        {
            frame(drumClip, drumLane, true, ImVec2(900, 700), false);
            const auto& c = model.getTrack(trackId)->color;
            const ImU32 lineColor = IM_COL32(c.r, c.g, c.b, 255);
            bool found = false;
            for(int list = 0; list < ImGui::GetDrawData()->CmdListsCount; ++list)
                for(const auto& vertex : ImGui::GetDrawData()->CmdLists[list]->VtxBuffer)
                    if(vertex.col == lineColor && std::abs(vertex.pos.x - 317.5f) < 2.0f && vertex.pos.y > 104 && vertex.pos.y < 148)
                        found = true;
            assert(found);
        }
        // The header's length field: - and + change it by one grid step; the
        // field drags; hits past the end are kept.
        {
            const float headerLeft = 230.0f - ofxOceanodeTimelineDrumGridEditor::headerWidth() + 2.0f;
            const float controlsX = headerLeft + 7.0f;
            const float controlsY = 104.0f + 2.0f + ImGui::GetTextLineHeight() + 2.0f + ImGui::GetTextLineHeight() * 0.5f;
            const float minusWidth = ImGui::CalcTextSize("-").x + 6.0f;
            const float minusX = controlsX + minusWidth * 0.5f;
            const float fieldX = controlsX + minusWidth + 2.0f + ofxOceanodeTimelineDrumGridEditor::lengthFieldWidth() * 0.5f;
            const float plusX = controlsX + minusWidth + 2.0f + ofxOceanodeTimelineDrumGridEditor::lengthFieldWidth() + 2.0f +
                                (ImGui::CalcTextSize("+").x + 6.0f) * 0.5f;
            click(ImVec2(plusX, controlsY));
            assert(std::abs(drums().drumRows[0].length - 1.5) < 1e-9);
            for(int i = 0; i < 4; ++i) click(ImVec2(minusX, controlsY));
            assert(std::abs(drums().drumRows[0].length - 0.5) < 1e-9);
            // 16 hits, shortened to 3 steps and back: nothing lost.
            auto& row = drums().drumRows[0];
            row.steps.clear();
            for(int i = 0; i < 16; ++i) {
                ofxOceanodeTimelineDrumStep s; s.beat = i * 0.25; s.length = 0.25; s.velocity = 0.5f + i * 0.01f;
                ofxOceanodeTimelineDrumGrid::addStep(row, s);
            }
            row.length = 1.0;
            click(ImVec2(minusX, controlsY));
            assert(std::abs(drums().drumRows[0].length - 0.75) < 1e-9 && drums().drumRows[0].steps.size() == 16);
            frame(drumClip, drumLane, true, ImVec2(fieldX, controlsY), false);
            frame(drumClip, drumLane, true, ImVec2(fieldX, controlsY), true);
            for(int i = 1; i <= 26; ++i) frame(drumClip, drumLane, true, ImVec2(fieldX + i * 5.0f, controlsY), true);
            frame(drumClip, drumLane, true, ImVec2(fieldX + 130.0f, controlsY), false);
            assert(drums().drumRows[0].length > 0.75 + 1e-9);         // dragged longer
            drums().drumRows[0].length = 4.0;
            assert(drums().drumRows[0].steps.size() == 16);
            for(int i = 0; i < 16; ++i) assert(drums().drumRows[0].steps[i].beat == i * 0.25 &&
                                               std::abs(drums().drumRows[0].steps[i].velocity - (0.5f + i * 0.01f)) < 1e-6f);
        }
        frame(drumClip, drumLane, true, ImVec2(900, 700), false, true); // properties + Row tab
    }
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
