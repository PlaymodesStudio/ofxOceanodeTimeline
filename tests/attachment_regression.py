#!/usr/bin/env python3
"""Exercise the extracted service against real core container/node lifecycles.

Uses the core's native headless fixture for canvas/scope services, and real OF
parameters, transport, macro host forwarding and timeline implementation.
"""
from pathlib import Path
import os
import sys
ADDON = Path(__file__).resolve().parents[1]
CORE = Path(os.environ.get('OCEANODE_CORE_ROOT', ADDON.parent / 'ofxOceanode'))
sys.path.insert(0, str(CORE / 'tests'))
import macro_mixer_regression as fixture
from new_preset_regression import block

original = fixture.native_production

def production():
    result = original().replace("void ofxOceanodeContainer::flushPendingScopeSave() { assert(false); }",
                                "void ofxOceanodeContainer::flushPendingScopeSave() { assert(!scopeSavePending); }")
    result = result.replace('std::string ofxOceanodeContainer::getTimelineParameterPath(ofxOceanodeAbstractParameter&) const { assert(false); return {}; }', '')
    source = (CORE / 'src/Managers/ofxOceanodeContainer.cpp').read_text()
    start = source.index('namespace {\nconst char* const kTimelineCanvasSeparator')
    end = source.index('std::string ofxOceanodeContainer::getTimelineParameterPath', start)
    result += source[start:end]
    for signature in ('std::string ofxOceanodeContainer::getTimelineParameterPath(',
                      'ofxOceanodeAbstractParameter* ofxOceanodeContainer::findTimelineParameter(',
                      'ofxOceanodeAbstractParameter* ofxOceanodeContainer::findParameter('):
        result += block(source, signature) + '\n'
    for signature in ('ofxOceanodeContainer::ParsedParameterPath ofxOceanodeContainer::parseParameterPath(',
                      'ofxOceanodeContainer::ResolvedParameter ofxOceanodeContainer::resolveParameterFromPath(',
                      'ofxOceanodeContainer* ofxOceanodeContainer::getContainerForCanvasID(',
                      'const ofxOceanodeContainer* ofxOceanodeContainer::getContainerForCanvasID('):
        result += block(source, signature) + '\n'
    model_source = (CORE / 'src/Nodes/ofxOceanodeNodeModel.cpp').read_text()
    for signature in ('string ofxOceanodeNodeModel::getParents()',
                      'ofxOceanodeTransportState ofxOceanodeNodeModel::getTransportState()'):
        if signature not in result:
            result += block(model_source, signature) + '\n'
    result += 'bool ofxOceanodeContainer::showParameterInCanvas(ofxOceanodeAbstractParameter&) { assert(false); return false; }\n'
    return result

fixture.native_production = production
fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
class AutomationProbe : public ofxOceanodeNodeModel {
public:
    AutomationProbe() : ofxOceanodeNodeModel("Automation Probe") {}
    ofParameter<float> value;
    float observed = -1;
    void setup() override { addParameter(value.set("Value", 0.f, 0.f, 100.f)); }
    void update(ofEventArgs&) override { observed = value.get(); value = 99.f; }
};
int main(int argc, char** argv) {
    assert(argc == 3);
    ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    registry->registerModel<AutomationProbe>("Test");
    auto transport = std::make_shared<ofxOceanodeTransport>();
    transport->setDriverMode(TransportDriverMode::FrameStep);
    ofxOceanodeContainer root(registry, types, transport); root.setCanvasID("Canvas");
    assert(!root.getAutomationExtension());
    bool missing = false;
    try { root.getTimelineManager(); } catch(const std::logic_error&) { missing = true; }
    assert(missing);
    auto& timeline = ofxOceanodeTimeline::attach(root);
    assert(&ofxOceanodeTimeline::attach(root) == &timeline);
    ofxOceanodeContainer child(registry, types, transport);
    child.setTimelineHost(&root);
    assert(child.getAutomationExtension() == root.getAutomationExtension());
    assert(&ofxOceanodeTimeline::attach(child) == &timeline);
    assert(&child.getTimelineManager() == &timeline);
    const auto& constChild = child;
    assert(&constChild.getTimelineManager() == &timeline);

    auto& node = root.createNode(std::make_unique<AutomationProbe>(), 1);
    auto& probe = static_cast<AutomationProbe&>(node.getNodeModel());
    auto& parameter = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Value"));
    assert(root.findTimelineParameter(root.getTimelineParameterPath(parameter)) == &parameter);
    const auto track = timeline.createTrack("Probe");
    const auto binding = timeline.addBinding(track, parameter);
    const auto clip = timeline.createClip(track, "Held", 0., 4.);
    const auto lane = timeline.createLane(track, clip, "Value", ofxOceanodeTimelineLaneType::Number);
    timeline.addBindingToLane(track, clip, lane, binding);
    timeline.setClipNumber(track, clip, lane, 0., 42.f);
    transport->seekToBeat(1.); root.update();
    assert(probe.observed == 42.f); // Automation precedes node update.
    assert(probe.value.get() == 42.f); // It is reasserted after node writes.
    probe.value = 5.f; child.update();
    assert(probe.value.get() == 5.f); // Forwarding never causes a second evaluation.
    root.update(); assert(probe.value.get() == 42.f);
    std::unordered_set<ofxOceanodeAbstractParameter*> published;
    root.getAutomationExtension()->collectBoundParameters(published);
    assert(published.count(&parameter));

    timeline.setTimebase(ofxOceanodeTimelineTimebase::Time);
    root.setBpm(60.f);
    assert(std::abs(transport->getState().beatPosition - 0.5) < 1e-6);
    assert(std::abs(timeline.getClip(track, clip)->durationBeats - 2.) < 1e-6);
    const auto saved = timeline.toJson();
    const auto folder = std::string(argv[2]);
    assert(root.getAutomationExtension()->savePreset(folder));
    timeline.clear(); assert(timeline.getTracks().empty());
    assert(root.getAutomationExtension()->loadPreset(folder));
    assert(timeline.toJson()["tracks"] == saved["tracks"]);
    // Verify the actual container save/load phases also invoke the service.
    assert(root.savePreset(folder + "/core"));
    timeline.clear();
    assert(root.loadPreset(folder + "/core"));
    assert(timeline.toJson()["tracks"] == saved["tracks"]);
    auto* restoredParameter = root.findTimelineParameter(timeline.getTrack(track)->bindings.front().parameterPath);
    root.update(); // Loaded bindings become active on the first evaluation.
    assert(restoredParameter && restoredParameter->isTimelined());
    root.getAutomationExtension()->removeParameterBindings(*restoredParameter);
    assert(!restoredParameter->isTimelined());
    root.clearContainer(); assert(timeline.getTracks().empty());
    std::cout << "Timeline attachment passed: optional core, idempotence, macro sharing, update order, retiming, persistence, cleanup.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [ADDON / 'src', *sorted({p.parent for p in (ADDON / 'src').rglob('*.h')})]
    fixture.main(extra_sources=sources, extra_includes=includes)
