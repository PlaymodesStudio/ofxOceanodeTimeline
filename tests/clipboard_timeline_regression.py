#!/usr/bin/env python3
"""Copying timeline-driven nodes and pasting them into another preset.

Uses real nodes and the core's own clipboard (native headless fixture). The
tracks driving the copied nodes are saved beside them; a paste asked by the
user into another preset offers them (never in the preset they were copied
from, never for a duplicate or an encapsulation), and accepting makes a new
timeline driving the pasted nodes only, playing as the original did.
"""
import encapsulate_timeline_regression as base

native = base.native
native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "ofxOceanodeShared.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#include "Controls/ofxOceanodeTimelineController.h"
#include "imgui_internal.h"
class Probe : public ofxOceanodeNodeModel {
public:
    Probe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> level;
    void setup() override { addParameter(level.set("Level", 0, 0, 1)); }
};
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
int main(int, char**) {
    ofDisableDataPath(); ofSeedRandom();
    const auto workFolder = std::filesystem::temp_directory_path() / "oceanode-clipboard-timeline";
    std::filesystem::remove_all(workFolder);
    std::filesystem::create_directories(workFolder);
    std::filesystem::current_path(workFolder);
    const auto file = std::filesystem::current_path() / "clipboardPreset" / "clipboardTimelines.json";
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    registry->registerModel<Probe>("Test");
    registry->registerModel<ofxOceanodeNodeMacro>("Macro");
    registry->registerModel<router<float>>("Router", "f", 0.f, -100.f, 100.f);
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    using Request = ofxOceanodeTimelineManager::ClipboardPasteRequest;
    ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/1--Source");
    const auto sourceRef = model.addTimeline("", "Verse");
    model.setEditTarget(sourceRef);
    model.setSchedulingEnabled(false);
    auto& copiedNode = owner->createNode(std::make_unique<Probe>(), 1);
    auto& otherNode = owner->createNode(std::make_unique<Probe>(), 2);
    auto param = [](ofxOceanodeNode& node) -> ofxOceanodeAbstractParameter& {
        return static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Level"));
    };
    // "Own": drives only the copied node; "Both": also another; "Other": only the other.
    const auto own = model.createTrack("Own");
    const auto ob = model.addBinding(own, param(copiedNode));
    const auto oc = model.createClip(own, "Ramp", 4, 4);
    const auto ol = model.createLane(own, oc, "Level", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(own, oc, ol, ob);
    model.setClipNumber(own, oc, ol, 0, 0.75f);
    const auto both = model.createTrack("Both");
    model.addBinding(both, param(copiedNode));
    model.addBinding(both, param(otherNode));
    const auto other = model.createTrack("Other");
    model.addBinding(other, param(otherNode));
    const auto sourceTracks = model.toJson()["timelines"][0]["tracks"];

    // ---- Copy: the tracks driving what was copied, whichever node that is.
    owner->saveClipboardModulesAndConnections({&otherNode}, {0, 0});
    {
        const auto saved = ofLoadJson(file);
        check(saved["timelines"].size() == 1 && saved["timelines"][0]["sequence"]["tracks"].size() == 2,
              "The other node's two tracks are kept");
    }
    owner->saveClipboardModulesAndConnections({&copiedNode}, {0, 0});
    check(std::filesystem::exists(file), "The copied node's automation is saved beside it");
    const auto saved = ofLoadJson(file);
    check(saved["timelines"].size() == 1 && saved["timelines"][0]["name"] == "Verse", "One timeline, by its name");
    const auto savedTracks = saved["timelines"][0]["sequence"]["tracks"];
    check(savedTracks.size() == 2, "Only the tracks driving the copied node (Own, Both)");
    check(model.toJson()["timelines"][0]["tracks"] == sourceTracks, "Copying changes nothing here");

    // ---- Pasted where it was copied: no question.
    Request request;
    check(owner->pasteModulesAndConnectionsInPosition({10, 10}, false, true), "Pasted in the same preset");
    check(!model.consumeClipboardPasteRequest(request), "Same preset: nothing offered");

    // ---- Another preset: offered for the user's paste only.
    owner->clearContainer();
    ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/2--Destination");
    auto& resident = owner->createNode(std::make_unique<Probe>(), 1); // takes the copied node's name
    check(owner->pasteModulesAndConnectionsInPosition({10, 10}, false), "Pasted without the offer (a duplicate, an encapsulation)");
    check(!model.consumeClipboardPasteRequest(request), "Not the user's paste: nothing offered");
    check(owner->pasteModulesAndConnectionsInPosition({20, 20}, false, true), "Pasted by the user");
    check(model.consumeClipboardPasteRequest(request), "Another preset: offered");
    check(!model.consumeClipboardPasteRequest(request) , "...once");
    check(request.canvas.empty() && request.names == std::vector<std::string>{"Verse"} && request.tracks == 2,
          "The request names the timeline and its two tracks");
    check(model.allTimelines().empty(), "Nothing is made before the answer");
    // Two pastes here: the user's is the second, the third Probe.
    check(request.paths.size() == 1 && request.paths.count("Probe_1/Level") == 1 &&
          request.paths.at("Probe_1/Level") == "Probe_3/Level", "The copied node's parameter, onto the node the user pasted");
    Probe* pasted = nullptr;
    const std::string pastedName = "Probe_3";
    for(auto* node : owner->getAllModules())
        if(node->getParameters().getEscapedName() == pastedName) pasted = dynamic_cast<Probe*>(&node->getNodeModel());
    check(pasted != nullptr, "The pasted node has a name of its own");

    // ---- Accepted: a new timeline driving the pasted node only.
    const auto made = model.pasteClipboardAutomation(request);
    check(made.size() == 1 && model.timelineName(made[0]) == "Verse", "A new timeline, named as the copied one");
    const auto* sequence = made.empty() ? nullptr : model.findTimeline(made[0]);
    check(sequence != nullptr && sequence->tracks.size() == 2, "With the two tracks");
    if(sequence != nullptr) {
        for(const auto& track : sequence->tracks) {
            check(track.bindings.size() == 1 && track.bindings[0].parameterPath == pastedName + "/Level",
                  track.name + ": bound to the pasted node only");
            check(track.parentId.empty(), track.name + ": at the top level");
        }
        const auto* ownTrack = sequence->findTrack(own);
        check(ownTrack != nullptr && ownTrack->clips.size() == 1 && ownTrack->clips[0].lanes.size() == 1 &&
              ownTrack->clips[0].lanes[0].bindingIds.size() == 1, "Its clip and lane came along, still driving it");
    }
    check(param(resident).isTimelined() == false, "The node that was here, with the copied one's name, is left alone");
    owner->getTransport()->play();
    owner->getTransport()->seekToBeat(5.0);
    model.update(); model.applyAutomation();
    check(pasted != nullptr && std::abs(pasted->level.get() - 0.75f) < 1e-4f, "It plays on the pasted node");
    check(std::abs(static_cast<Probe&>(resident.getNodeModel()).level.get()) < 1e-6f, "...and not on the other");
    owner->getTransport()->stop();

    // ---- Nothing automated: no file, no question.
    owner->saveClipboardModulesAndConnections({&resident}, {0, 0});
    check(!std::filesystem::exists(file), "Unautomated nodes leave no automation behind");
    ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/3--Third");
    check(owner->pasteModulesAndConnectionsInPosition({30, 30}, false, true), "Pasted elsewhere");
    check(!model.consumeClipboardPasteRequest(request), "Nothing to offer");

    // ---- A macro: copied whole, driven from the timeline around it.
    struct Source { ofxOceanodeNode* macroNode; ofxOceanodeNodeMacro* macro; std::string canvas; };
    auto buildSource = [&](const std::string& preset) {
        owner->clearContainer();
        ofxOceanodeShared::setCurrentPresetPath(preset);
        model.setEditTarget(model.addTimeline("", "Around"));
        auto& inner = owner->createNode(std::make_unique<Probe>(), 1);
        const auto track = model.createTrack("Into the macro");
        const auto binding = model.addBinding(track, param(inner));
        const auto clip = model.createClip(track, "Ramp", 4, 4);
        const auto lane = model.createLane(track, clip, "Level", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(track, clip, lane, binding);
        model.setClipNumber(track, clip, lane, 0, 0.5f);
        Source source{owner->encapsulateNodes({&inner}, "Kept", false), nullptr, std::string()};
        check(source.macroNode != nullptr, "Encapsulated");
        source.macro = source.macroNode ? dynamic_cast<ofxOceanodeNodeMacro*>(&source.macroNode->getNodeModel()) : nullptr;
        if(source.macro) source.canvas = source.macro->getContainer()->getCanvasID();
        return source;
    };
    auto probeIn = [&](ofxOceanodeContainer& canvas, const std::string& name) -> Probe* {
        for(auto* node : canvas.getAllModules())
            if(node->getParameters().getEscapedName() == name) return dynamic_cast<Probe*>(&node->getNodeModel());
        return nullptr;
    };
    auto macroNamed = [&](const std::string& canvas) -> ofxOceanodeNodeMacro* {
        for(auto* node : owner->getAllModules())
            if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel()))
                if(macro->getContainer() && macro->getContainer()->getCanvasID() == canvas) return macro;
        return nullptr;
    };
    {
        auto source = buildSource("./Presets/Bank/5--Macro source");
        owner->saveClipboardModulesAndConnections({source.macroNode}, {0, 0});
        const auto kept = ofLoadJson(file);
        check(kept["timelines"].size() == 1 && kept["timelines"][0]["sequence"]["tracks"].size() == 1 &&
              kept["timelines"][0]["sequence"]["tracks"][0]["bindings"][0]["parameterPath"] == source.canvas + "::Probe_1/Level",
              "A copied macro: the track driving what is inside it is kept");
        owner->clearContainer();
        ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/6--Macro destination");
        owner->createNode(registry->create("Macro")); // takes the copied macro's number
        check(owner->pasteModulesAndConnectionsInPosition({0, 0}, false, true), "The macro pasted in another preset");
        check(model.consumeClipboardPasteRequest(request), "...is offered its timeline");
        const std::string from = source.canvas + "::Probe_1/Level";
        const std::string to = request.paths.count(from) > 0 ? request.paths.at(from) : std::string();
        check(!to.empty() && to != from && to.find("::Probe_1/Level") != std::string::npos,
              "What is inside the copied macro, onto what is inside the pasted one (" + to + ")");
        const auto pastedTimelines = model.pasteClipboardAutomation(request);
        const auto* around = pastedTimelines.empty() ? nullptr : model.findTimeline(pastedTimelines[0]);
        check(around != nullptr && around->tracks.size() == 1 && around->tracks[0].bindings.size() == 1 &&
              around->tracks[0].bindings[0].parameterPath == to, "The pasted timeline drives the pasted macro's node");
        auto* pastedMacro = macroNamed(to.substr(0, to.find("::")));
        auto* innerProbe = pastedMacro ? probeIn(*pastedMacro->getContainer(), "Probe_1") : nullptr;
        owner->getTransport()->play();
        owner->getTransport()->seekToBeat(5.0);
        model.update(); model.applyAutomation();
        check(innerProbe != nullptr && std::abs(innerProbe->level.get() - 0.5f) < 1e-4f, "...and plays inside it");
        owner->getTransport()->stop();
    }

    // ---- Copied from inside a macro (its automation on the timeline around
    // it), pasted into a macro of another preset: a timeline of that macro.
    {
        auto source = buildSource("./Presets/Bank/7--Inside source");
        auto& inside = *source.macro->getContainer();
        ofxOceanodeNode* innerNode = nullptr;
        for(auto* node : inside.getAllModules()) if(node->getParameters().getEscapedName() == "Probe_1") innerNode = node;
        check(innerNode != nullptr, "The node inside the macro");
        inside.saveClipboardModulesAndConnections({innerNode}, {0, 0});
        const auto kept = ofLoadJson(file);
        check(kept["timelines"].size() == 1 && kept["timelines"][0]["name"] == "Around" &&
              kept["timelines"][0]["sequence"]["tracks"][0]["bindings"][0]["parameterPath"] == "Probe_1/Level",
              "Copied from inside: the outer timeline's track, its path relative to the macro");
        ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/8--Inside destination");
        check(inside.pasteModulesAndConnectionsInPosition({0, 0}, false, true), "Pasted into a macro");
        check(model.consumeClipboardPasteRequest(request), "...offered");
        check(request.canvas == source.canvas && request.paths.count("Probe_1/Level") == 1 &&
              request.paths.at("Probe_1/Level") == "Probe_2/Level", "Paths relative to the macro pasted into");
        const auto before = model.timelinesOfCanvas(source.canvas).size();
        const auto pastedTimelines = model.pasteClipboardAutomation(request);
        check(pastedTimelines.size() == 1 && pastedTimelines[0].canvas == source.canvas &&
              model.timelinesOfCanvas(source.canvas).size() == before + 1, "A new timeline of that macro");
        const auto* inner = pastedTimelines.empty() ? nullptr : model.findTimeline(pastedTimelines[0]);
        check(inner != nullptr && inner->tracks.size() == 1 && inner->tracks[0].bindings.size() == 1 &&
              inner->tracks[0].bindings[0].parameterPath == "Probe_2/Level", "...driving the pasted node");
        check(model.timelinesOfCanvas("").size() == 1, "The timeline around it is not doubled");
    }

    // ---- The question, as the app draws it: Escape pastes nodes only, Enter the timeline too.
    {
        auto source = buildSource("./Presets/Bank/9--Asked source");
        owner->saveClipboardModulesAndConnections({source.macroNode}, {0, 0});
        ofxOceanodeShared::setCurrentPresetPath("./Presets/Bank/10--Asked destination");
        ImGui::CreateContext(); auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 1000); io.DeltaTime = 1.f / 60;
        unsigned char* atlas; int aw, ah; io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
        ofxOceanodeTimelineController ui(owner);
        auto frame = [&]() {
            io.AddMousePosEvent(-100, -100); io.AddMouseButtonEvent(0, false);
            ImGui::NewFrame(); ui.drawBeforeWindows(); ui.drawPopups(); ImGui::Render();
        };
        auto press = [&](ImGuiKey key) { io.AddKeyEvent(key, true); frame(); io.AddKeyEvent(key, false); frame(); frame(); };
        auto asked = [&]() {
            auto* dialog = ImGui::FindWindowByName("Paste Timeline?##clipboardTimeline");
            return dialog != nullptr && dialog->Active && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0;
        };
        frame(); frame();
        check(!asked(), "No question before a paste");
        const auto timelinesBefore = model.allTimelines().size();
        check(owner->pasteModulesAndConnectionsInPosition({0, 0}, false, true), "Pasted, the app running");
        frame(); frame(); frame();
        check(asked(), "The question is shown");
        check(model.allTimelines().size() == timelinesBefore, "...and nothing is made meanwhile");
        press(ImGuiKey_Escape);
        check(!asked() && model.allTimelines().size() == timelinesBefore, "Escape: nodes only");
        check(owner->pasteModulesAndConnectionsInPosition({0, 0}, false, true), "Pasted again");
        frame(); frame(); frame();
        check(asked(), "Asked again");
        const auto windowsBefore = model.getTimelineWindows().size();
        press(ImGuiKey_Enter);
        check(!asked() && model.allTimelines().size() == timelinesBefore + 1, "Enter: the timeline is pasted too");
        check(model.getTimelineWindows().size() == windowsBefore + 1, "...and shown in a window of its own");
        ImGui::DestroyContext();
    }
    if(failures > 0) return 1;
    std::cout << "Clipboard timeline passed: tracks saved with copied nodes, offered only for the user's paste "
                 "into another preset, pasted as a new timeline driving the pasted nodes; copied macros, copies "
                 "from inside a macro, pastes into a macro; the question (Escape, Enter).\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
