#!/usr/bin/env python3
"""Beat Transport reads any transport: the global one by default, or a timeline's.

Runs the core's production Beat Transport node in a real container with the
timeline attached: its Transport dropdown lists the global transport first,
then every timeline with a transport; picking a free timeline makes its
outputs (beat, BPM, playing, bar) follow that timeline while the global
transport is stopped, and a deleted timeline shows as missing.
"""
import attachment_regression as native
from new_preset_regression import block

base_production = native.production


def production():
    result = base_production()
    model_source = (native.CORE / 'src/Nodes/ofxOceanodeNodeModel.cpp').read_text()
    for signature in ('void ofxOceanodeNodeModel::addTimeSourceParameter()',
                      'std::string ofxOceanodeNodeModel::getTimeSourceId()',
                      'void ofxOceanodeNodeModel::setTimeSourceId(',
                      'bool ofxOceanodeNodeModel::isTimeSourceMissing()',
                      'ofxOceanodeFrameTransportState ofxOceanodeNodeModel::getFrameTransportState()'):
        if signature not in result:
            result += block(model_source, signature) + '\n'
    return result


native.fixture.native_production = production
native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/Default_Nodes/beatTransport.h"
#include <cassert>
#include <cmath>
#include <iostream>
static void near(double a, double b, int line) {
    if(std::abs(a - b) > 1e-3) { std::cerr << "Line " << line << ": expected " << b << ", got " << a << "\n"; std::abort(); }
}
#define NEAR(a, b) near((a), (b), __LINE__)
int main(int argc, char** argv) {
    assert(argc == 3);
    ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto transport = std::make_shared<ofxOceanodeTransport>();
    transport->setDriverMode(TransportDriverMode::FrameStep);
    ofxOceanodeContainer root(registry, types, transport); root.setCanvasID("Canvas");
    auto& timeline = ofxOceanodeTimeline::attach(root);
    timeline.setSchedulingEnabled(false);
    auto& node = root.createNode(std::make_unique<beatTransport>(), 1);
    auto& beats = static_cast<beatTransport&>(node.getNodeModel());
    auto& group = beats.getParameterGroup();
    auto param = [&](const std::string& name) -> ofxOceanodeAbstractParameter& {
        return dynamic_cast<ofxOceanodeAbstractParameter&>(group.get(name));
    };
    auto& selector = param("Transport").cast<int>().getParameter();
    auto& beatOut = param("Beat Transport").cast<float>().getParameter();
    auto& bpmOut = param("BPM").cast<float>().getParameter();
    auto& playingOut = param("Playing").cast<int>().getParameter();
    auto& barOut = param("Bar").cast<int>().getParameter();
    ofEventArgs args;
    auto frame = [&](double seconds) {
        transport->advanceFrameStep(seconds);
        transport->latchFrameState();
        timeline.update();
        beats.update(args);
    };

    // The global transport by default.
    frame(0.0);
    assert(beats.getTimeSourceId() == ofxOceanodeNodeModel::TimeSourceGlobal);
    assert(selector.get() == 0 && beats.getTransport() == transport);
    transport->setBpm(120.0f);
    transport->play();
    frame(0.5);
    NEAR(beatOut.get(), 1.0); NEAR(bpmOut.get(), 120.0); assert(playingOut.get() == 1);
    transport->stop();
    frame(0.0);

    // A free timeline at 60 BPM in 3/4: listed after the global transport.
    const auto drums = timeline.addTimeline("", "Drums");
    timeline.setFree(drums, true);
    auto settings = *timeline.playSettings(drums);
    settings.bpm = 60.0f;
    settings.quantize = 0;
    settings.timeSignatureNumerator = 3;
    timeline.setPlaySettings(drums, settings);
    frame(0.0);
    auto drumsTransport = timeline.transportFor(drums);
    assert(drumsTransport != nullptr && drumsTransport->isDerived());
    assert(selector.getMax() >= 1);
    selector = 1; // Drums
    frame(0.0);
    assert(beats.getTimeSourceId() == ofxOceanodeTimelineManager::timeSourceId(drums)); // saved by id
    assert(beats.getTransport() == drumsTransport);

    // It follows Drums while the global transport stays stopped.
    timeline.playTimeline(drums, false);
    for(int i = 0; i < 40; ++i) frame(0.1); // 4 s at 60 BPM: 4 beats, bar 2 of 3/4
    assert(!transport->getState().isPlaying);
    assert(playingOut.get() == 1);
    NEAR(bpmOut.get(), 60.0);
    assert(beatOut.get() > 3.5f && beatOut.get() < 4.5f);
    assert(barOut.get() == 2);
    // Its Stop stops Drums, not the global transport.
    param("Stop").cast<void>().getParameter().trigger();
    frame(0.1);
    assert(!drumsTransport->getState().isPlaying && playingOut.get() == 0);

    // Back to the global transport.
    selector = 0;
    frame(0.0);
    assert(beats.getTimeSourceId() == ofxOceanodeNodeModel::TimeSourceGlobal && beats.getTransport() == transport);

    // A deleted timeline: kept as the choice, shown as missing, outputs stopped.
    selector = 1;
    frame(0.0);
    assert(beats.getTransport() == drumsTransport);
    timeline.removeTimeline(drums);
    frame(0.0); frame(0.0);
    assert(beats.isTimeSourceMissing() && beats.getTransport() == nullptr);
    assert(playingOut.get() == 0);
    assert(selector.get() == selector.getMax()); // the "Missing transport" entry, last
    std::cout << "Beat Transport source passed: global by default, a free timeline's transport (beat, BPM, meter, Stop), back to global, missing timeline.\n";
    return 0;
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp',
                'src/Nodes/Default_Nodes/beatTransport.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
