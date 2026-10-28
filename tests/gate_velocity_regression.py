#!/usr/bin/env python3
"""Regress per-step (Step Sequencer) and per-gate (Toggle) velocity.

Checks the frame path, the timestamped scheduler and JSON round trips.
"""
import timeline_regression as fixture

cases = fixture.CASES.split('int main()')[0] + r'''
struct GateFixture {
    ofParameter<float> value{"Gate", 0.0f, 0.0f, 1.0f};
    ofxOceanodeParameter<float> parameter;
    std::string track, clip, lane;
    GateFixture(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& manager, ofxOceanodeTimelineLaneType type) {
        parameter.bindParameter(value);
        owner.targets[parameter.getEscapedName()] = &parameter;
        track = manager.createTrack("Gate");
        const auto binding = manager.addBinding(track, parameter);
        clip = manager.createClip(track, "Gate", 0.0, 2.0);
        lane = manager.createLane(track, clip, "Gate", type);
        assert(manager.addBindingToLane(track, clip, lane, binding));
    }
};

int main() {
    ofSeedRandom();
    {   // Frame path: a gate step / gate region plays at its velocity.
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofxOceanodeTimelineManager manager(&owner);
        GateFixture step(owner, manager, ofxOceanodeTimelineLaneType::Step);
        auto* stepLane = manager.getLane(step.track, step.clip, step.lane);
        stepLane->beatsPerStep = 1.0; stepLane->stepCount = 2;
        assert(manager.setClipStep(step.track, step.clip, step.lane, 0.0, "1", 1.0));
        assert(manager.setClipStep(step.track, step.clip, step.lane, 1.0, "1", 1.0));
        stepLane->step.steps[0].velocity = 0.5f;
        owner.transport->seekToBeat(0.5); manager.update(); near(step.value.get(), 0.5);
        owner.transport->seekToBeat(1.5); manager.update(); near(step.value.get(), 1.0);
        // Value steps ignore velocity.
        stepLane->step.steps[1].value = "0.75"; stepLane->step.steps[1].velocity = 0.1f;
        manager.update(); near(step.value.get(), 0.75);

        GateFixture toggle(owner, manager, ofxOceanodeTimelineLaneType::MultiGate);
        auto* toggleLane = manager.getLane(toggle.track, toggle.clip, toggle.lane);
        toggleLane->multiRowCount = 1;
        toggleLane->multiGateRows = {{{0.0, 1.0, 0.25f}, {1.0, 1.0, 1.0f}}};
        owner.transport->seekToBeat(0.5); manager.update(); near(toggle.value.get(), 0.25);
        owner.transport->seekToBeat(1.5); manager.update(); near(toggle.value.get(), 1.0);

        // Both survive save/load.
        const auto json = manager.toJson();
        ofxOceanodeTimelineManager reloaded(&owner);
        reloaded.fromJson(json);
        near(reloaded.getLane(step.track, step.clip, step.lane)->step.steps[0].velocity, 0.5);
        near(reloaded.getLane(toggle.track, toggle.clip, toggle.lane)->multiGateRows[0][0].velocity, 0.25);
        ofxOceanodeScheduling::unregisterOwner(&reloaded);
        ofxOceanodeScheduling::unregisterOwner(&manager);
    }
    // Scheduler: the queued gate-on carries the velocity, timestamped at
    // the step / region start.
    for(auto type : {ofxOceanodeTimelineLaneType::Step, ofxOceanodeTimelineLaneType::MultiGate}) {
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::External);
        ofxOceanodeTimelineManager manager(&owner);
        manager.setSchedulingLookaheadAdaptive(false); // the default 120 ms window
        GateFixture gate(owner, manager, type);
        auto* lane = manager.getLane(gate.track, gate.clip, gate.lane);
        const float velocity = type == ofxOceanodeTimelineLaneType::Step ? 0.5f : 0.25f;
        if(type == ofxOceanodeTimelineLaneType::Step) {
            lane->beatsPerStep = 0.1; lane->stepCount = 20;
            assert(manager.setClipStep(gate.track, gate.clip, gate.lane, 0.1, "1", 0.1));
            lane->step.steps[0].velocity = velocity;
        } else {
            lane->multiRowCount = 1;
            lane->multiGateRows = {{{0.1, 0.1, velocity}}};
        }
        std::vector<ofxOceanodeScheduledParameterEvent> events;
        ofxOceanodeScheduling::registerParameterTarget(&gate.parameter, &manager,
            [&](const auto& event) { events.push_back(event); return true; });
        owner.transport->play();
        manager.update();
        std::vector<float> values;
        for(const auto& event : events) if(!event.isCorrection) values.push_back(ofToFloat(event.value));
        if(values.size() != 2 || std::abs(values[0] - velocity) > 1e-4f || values[1] != 0.0f) {
            std::cerr << "Scheduled gate values:";
            for(float v : values) std::cerr << " " << v;
            std::cerr << " (expected " << velocity << " 0)\n";
            return 1;
        }
        assert(events[1].dueSteadyTimeUs > events[0].dueSteadyTimeUs);
        owner.transport->pause();
        ofxOceanodeScheduling::unregisterOwner(&manager);
    }
    std::cout << "Gate velocity regression passed: step and toggle frame, schedule and JSON.\n";
}
'''

fixture.main(cases=cases)
