#!/usr/bin/env python3
"""Encapsulating timeline-driven nodes: keep their tracks, or move them in.

Uses real nodes and the core's own encapsulation and canvas undo (native
headless fixture). Keeping: bindings follow the nodes into the macro, and
undo and redo follow them back and forth. Moving: tracks driving only the
encapsulated nodes move into the macro's own timeline, a macro track plays
it from where they were, playback stays the same, a track also driving a
node outside stays; undo brings the tracks back, redo moves them again.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
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
    // Encapsulation copies through a clipboard folder: keep it out of the repository.
    const auto workFolder = std::filesystem::temp_directory_path() / "oceanode-encapsulate-timeline";
    std::filesystem::create_directories(workFolder);
    std::filesystem::current_path(workFolder);
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    registry->registerModel<Probe>("Test");
    registry->registerModel<ofxOceanodeNodeMacro>("Macro");
    registry->registerModel<router<float>>("Router", "f", 0.f, -100.f, 100.f);
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types); owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    model.setSchedulingEnabled(false);
    auto& inNode = owner->createNode(std::make_unique<Probe>(), 1);
    auto& outNode = owner->createNode(std::make_unique<Probe>(), 2);
    auto param = [](ofxOceanodeNode& node) -> ofxOceanodeAbstractParameter& {
        return static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Level"));
    };
    // "Inside": drives only the node to encapsulate; "Both": also one outside.
    const auto inside = model.createTrack("Inside");
    const auto ib = model.addBinding(inside, param(inNode));
    const auto ic = model.createClip(inside, "Ramp", 4, 4);
    const auto il = model.createLane(inside, ic, "Level", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(inside, ic, il, ib);
    model.setClipNumber(inside, ic, il, 0, 0.75f);
    const auto both = model.createTrack("Both");
    model.addBinding(both, param(inNode));
    model.addBinding(both, param(outNode));
    const auto before = model.toJson()["tracks"];
    check(owner->hasAutomatedParameters({&inNode}), "The node to encapsulate is automated");

    auto levelAt = [&](double beat) {
        owner->getTransport()->seekToBeat(beat);
        model.update(); model.applyAutomation();
    };
    auto insideProbe = [&]() -> Probe* {
        for(auto* node : owner->getAllModules())
            if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel()))
                for(auto* inner : macro->getContainer()->getAllModules())
                    if(auto* probe = dynamic_cast<Probe*>(&inner->getNodeModel())) return probe;
        return nullptr;
    };
    owner->getTransport()->play();

    // ---- Keep on the timeline: paths follow, and follow back on undo.
    auto* macroNode = owner->encapsulateNodes({&inNode}, "Kept", false);
    check(macroNode != nullptr, "Encapsulated");
    auto* macro = macroNode == nullptr ? nullptr : static_cast<ofxOceanodeNodeMacro*>(&macroNode->getNodeModel());
    const std::string canvas = macro ? macro->getContainer()->getCanvasID() : std::string();
    check(model.getTrack(inside) != nullptr && model.getTrack(inside)->bindings[0].parameterPath == canvas + "::Probe_1/Level",
          "Kept: the binding points inside the macro");
    levelAt(5.0);
    check(insideProbe() && std::abs(insideProbe()->level.get() - 0.75f) < 1e-4f, "Kept: it still plays");
    check(owner->undoCanvasEdit(), "Undo");
    check(model.toJson()["tracks"] == before, "Undo: the bindings point at the node again");
    check(owner->redoCanvasEdit(), "Redo");
    check(model.getTrack(inside)->bindings[0].parameterPath == canvas + "::Probe_1/Level", "Redo: inside again");
    check(owner->undoCanvasEdit(), "Undo again");
    check(model.toJson()["tracks"] == before, "...and back");

    // ---- Move into the macro.
    auto* node = owner->getParameterGroupNodesMap().at("Probe_1");
    macroNode = owner->encapsulateNodes({node}, "Moved", true);
    check(macroNode != nullptr, "Encapsulated with the move");
    macro = static_cast<ofxOceanodeNodeMacro*>(&macroNode->getNodeModel());
    auto* macroSequence = model.getMacroSequence(*macro->getContainer());
    check(model.getTrack(inside) == nullptr, "Moved: the track left the main timeline");
    check(macroSequence && macroSequence->tracks.size() == 1 && macroSequence->tracks[0].id == inside &&
          macroSequence->tracks[0].bindings[0].parameterPath == "Probe_1/Level", "...into the macro's timeline, relative to it");
    check(model.getTrack(both) != nullptr, "A track also driving a node outside stays");
    std::string macroTrack;
    for(const auto& track : model.getTracks()) if(track.isMacroTrack()) macroTrack = track.id;
    check(!macroTrack.empty() && model.getTracks().front().id == macroTrack, "A macro track takes its place");
    const auto* macroClip = macroTrack.empty() ? nullptr : &model.getTrack(macroTrack)->clips.at(0);
    check(macroClip && macroClip->startBeat == 0.0 && std::abs(macroClip->durationBeats - 8.0) < 1e-9, "...with one clip to the end of the moved clips");
    insideProbe()->level = 0.0f;
    levelAt(5.0);
    check(std::abs(insideProbe()->level.get() - 0.75f) < 1e-4f, "Moved: it plays the same");
    check(owner->undoCanvasEdit(), "Undo the move");
    check(model.toJson()["tracks"] == before, "Undo: tracks back where they were, as they were");
    check(owner->redoCanvasEdit(), "Redo the move");
    macro = nullptr;
    for(auto* n : owner->getAllModules()) if(auto* m = dynamic_cast<ofxOceanodeNodeMacro*>(&n->getNodeModel())) macro = m;
    macroSequence = macro ? model.getMacroSequence(*macro->getContainer()) : nullptr;
    size_t macroTracks = 0;
    for(const auto& track : model.getTracks()) macroTracks += track.isMacroTrack() ? 1 : 0;
    check(model.getTrack(inside) == nullptr && macroTracks == 1 && macroSequence && macroSequence->tracks.size() == 1,
          "Redo: moved again, nothing doubled");
    insideProbe()->level = 0.0f;
    levelAt(5.0);
    check(std::abs(insideProbe()->level.get() - 0.75f) < 1e-4f, "Redo: it plays the same");
    check(owner->undoCanvasEdit() && model.toJson()["tracks"] == before, "Undo once more");

    // Nothing automated: no prompt needed.
    check(!owner->hasAutomatedParameters({}), "No nodes: nothing automated");
    owner->getTransport()->stop();
    if(failures > 0) return 1;
    std::cout << "Encapsulate timeline passed: keep (paths follow, undo/redo), move into the macro "
                 "(relative paths, macro track and clip, mixed track stays, same playback, undo/redo without doubles).\n";
}
'''

# The core's encapsulation, spliced in as its own macro undo test does.
_production = native.fixture.native_production
def production():
    result = _production()
    source = (native.CORE / 'src/Managers/ofxOceanodeContainer.cpp').read_text()
    for signature in ['void ofxOceanodeContainer::encapsulateSelectedNodes(',
                      'bool ofxOceanodeContainer::hasAutomatedParameters(',
                      'ofxOceanodeNode* ofxOceanodeContainer::encapsulateNodes(',
                      'vector<ofxOceanodeContainer::ExternalConnection> ofxOceanodeContainer::analyzeExternalConnections(',
                      'void ofxOceanodeContainer::createRoutersAndReconnect(',
                      'bool ofxOceanodeContainer::isNodeInList(',
                      'string ofxOceanodeContainer::generateRouterName(',
                      'ofxOceanodeNode* ofxOceanodeContainer::getNodeFromParameter(',
                      'ofxOceanodeAbstractParameter* ofxOceanodeContainer::findInternalParameter(',
                      'string ofxOceanodeContainer::extractNodeType(',
                      'void ofxOceanodeContainer::ensureMacroParametersExposed(',
                      'bool ofxOceanodeContainer::copySelectedModulesWithConnections(',
                      'bool ofxOceanodeContainer::cutSelectedModulesWithConnections(',
                      'bool ofxOceanodeContainer::pasteModulesAndConnectionsInPosition(',
                      'vector<int> ofxOceanodeContainer::getSelectedCommentIndices()',
                      'void ofxOceanodeContainer::collectMovedTimelinePaths(',
                      'std::string ofxOceanodeContainer::timelineCanvasPrefix(',
                      'std::string ofxOceanodeContainer::timelineMacroCanvas(']:
        body = native.block(source, signature)
        if body not in result: result += body + '\n'
    return result
native.fixture.native_production = production

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
