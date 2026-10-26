#!/usr/bin/env python3
"""Regress queued timeline events while GUI frame duration fluctuates."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

timeline = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('timeline_regression', timeline / 'tests/timeline_regression.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
cases = fixture.CASES.split('int main()')[0] + r'''
float schedulingTestFrameSeconds = 1.0f / 60.0f;
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    // Frozen like FrameStep, but scheduling only follows clock-driven transports:
    // External with no clock attached never advances the playhead.
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> value("Gate", 0.0f, 0.0f, 1.0f);
    ofxOceanodeParameter<float> parameter;
    parameter.bindParameter(value);
    owner.targets[parameter.getEscapedName()] = &parameter;
    ofxOceanodeTimelineManager manager(&owner);
    const auto track = manager.createTrack("Gate");
    const auto binding = manager.addBinding(track, parameter);
    const auto clip = manager.createClip(track, "Gate", 0.0, 2.0);
    const auto lane = manager.createLane(track, clip, "Gate", ofxOceanodeTimelineLaneType::Number);
    manager.addBindingToLane(track, clip, lane, binding);
    manager.setClipNumber(track, clip, lane, 0.0, 0.0f);
    manager.setClipNumber(track, clip, lane, 0.2, 1.0f);
    manager.setClipNumber(track, clip, lane, 0.4, 0.0f);
    std::vector<ofxOceanodeScheduledParameterEvent> events;
    ofxOceanodeScheduling::registerParameterTarget(&parameter, &manager,
        [&](const auto& event) { events.push_back(event); return true; });
    owner.transport->play();
    manager.update();
    assert(events.size() == 1 && !events.front().isCorrection);
    assert(ofToFloat(events.front().value) == 1.0f);
    const auto original = events.front().dueSteadyTimeUs;
    // The transport stays before the queued note. Drawing cost alone must
    // never cancel it, schedule a 1 ms gate-off, or repeatedly enqueue it.
    for(float frame : {0.045f, 0.012f, 0.080f, 0.016f, 0.040f, 0.017f}) {
        schedulingTestFrameSeconds = frame;
        manager.update();
    }
    if(events.size() != 1) {
        std::cerr << "Frame jitter generated " << events.size() - 1
                  << " extra note/correction events before the note was due\n";
        return 1;
    }
    assert(events.front().dueSteadyTimeUs == original);
    // A deliberate user edit must still invalidate queued content.
    const size_t beforeEdit = events.size();
    manager.setSchedulingLookaheadMs(150.0);
    manager.update(); // corrections go out on the update after the edit
    assert(std::any_of(events.begin() + beforeEdit, events.end(), [](const auto& e) { return e.isCorrection; }));
    owner.transport->pause();
    ofxOceanodeScheduling::unregisterOwner(&manager);
    std::cout << "Timeline frame jitter: no extra gates; explicit edit correction preserved.\n";
}
'''

with tempfile.TemporaryDirectory(prefix='timeline-frame-jitter-') as folder:
    source = Path(folder) / 'timeline.cpp'
    # Inject only the frame-duration input; execute the complete production
    # model, scheduler, transport and OF parameter implementation.
    source.write_text('extern float schedulingTestFrameSeconds;\n' +
        (timeline / 'src/Timeline/ofxOceanodeTimeline.cpp').read_text().replace(
            'ofGetLastFrameTime()', 'schedulingTestFrameSeconds'))
    original_run = subprocess.run
    def run(command, **kwargs):
        command = [str(source) if str(arg) == str(timeline / 'src/Timeline/ofxOceanodeTimeline.cpp')
                   else arg for arg in command]
        return original_run(command, **kwargs)
    fixture.subprocess.run = run
    fixture.main(cases=cases)
