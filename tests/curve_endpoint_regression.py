#!/usr/bin/env python3
"""Curve endpoints reach their exact value once when playback crosses a clip end."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    struct Timing { bool repeat; double duration, stretch, offset; float expected; };
    for(const auto timing : std::vector<Timing>{
            {true, 4, 1, 0, 1}, {true, 8, 1, 0, 1},
            {true, 6, 1, 0, 0.5f}, {true, 8, 2, 0, 1},
            {false, 8, 1, 0, 1}, {true, 4, 1, 1, 0.25f}}) {
        for(bool exactBoundary : {false, true}) {
            ofxOceanodeContainer owner;
            owner.transport->setDriverMode(TransportDriverMode::FrameStep);
            ofParameter<float> output("Level", 0, 0, 1);
            ofxOceanodeParameter<float> target; target.bindParameter(output);
            owner.targets[target.getEscapedName()] = &target;
            TestTimeline model(&owner);
            const auto t = model.createTrack("Curve"), b = model.addBinding(t, target);
            const auto c = model.createClip(t, "Ramp", 1, timing.duration);
            const auto l = model.createLane(t, c, "Curve", ofxOceanodeTimelineLaneType::Curve);
            model.addBindingToLane(t, c, l, b);
            auto& clip = *model.getClip(t, c);
            clip.repeatContent = timing.repeat; clip.contentDurationBeats = 4;
            clip.contentStretch = timing.stretch; clip.contentOffsetBeats = timing.offset;
            model.getLane(t, c, l)->curvePoints = {{0, 0}, {4, 1}};
            const double end = 1 + timing.duration;
            owner.transport->seekToBeat(end - 0.01);
            owner.transport->play(); model.update();
            assert(output.get() != timing.expected);
            owner.transport->advanceFrameStep(exactBoundary ? 0.005 : 0.02);
            model.update();
            if(output.get() != timing.expected)
                std::cerr << "Endpoint: duration=" << timing.duration << " stretch=" << timing.stretch
                          << " offset=" << timing.offset << " exact=" << exactBoundary
                          << " expected=" << timing.expected << " got=" << output.get() << "\n";
            assert(output.get() == timing.expected); // exact, not a near-end frame sample
            model.applyAutomation(); assert(output.get() == timing.expected);
            output = 0.37f; model.update(); assert(output.get() == 0.37f);
            owner.transport->advanceFrameStep(0.1); model.update();
            assert(output.get() == 0.37f); // the endpoint is written once, leaving manual control
        }
    }
    for(int mode = 0; mode < 9; ++mode) {
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<float> output("Level", 0.37f, 0, 1);
        ofxOceanodeParameter<float> target; target.bindParameter(output);
        owner.targets[target.getEscapedName()] = &target;
        TestTimeline model(&owner);
        const auto t = model.createTrack("Curve"), b = model.addBinding(t, target);
        const auto c = model.createClip(t, "Ramp", 1, 4);
        const auto l = model.createLane(t, c, "Curve", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(t, c, l, b);
        if(mode == 0) model.getClip(t, c)->muted = true;
        if(mode == 1) model.getClip(t, c)->chance = 0;
        if(mode == 2) model.getBinding(t, b)->bypass = true;
        if(mode == 3) model.getLane(t, c, l)->muted = true;
        if(mode == 4 || mode == 5) {
            // The clip active now wins over a terminal sample, even when it
            // starts earlier (an underlying clip resuming after an overlap).
            const auto next = model.createClip(t, "Other", mode == 4 ? 5 : 0, 10);
            const auto lane = model.createLane(t, next, "Curve", ofxOceanodeTimelineLaneType::Curve);
            model.addBindingToLane(t, next, lane, b);
            model.getLane(t, next, lane)->curvePoints = {{0, 0.2f}, {10, 0.2f}};
        }
        if(mode == 8) {
            const auto gate = model.createLane(t, c, "Gate", ofxOceanodeTimelineLaneType::PianoRoll);
            model.addBindingToLane(t, c, gate, b);
            model.getBinding(t, b)->mode = ofxOceanodeTimelineAutomationMode::Multiply;
            auto& lane = *model.getLane(t, c, gate);
            lane.pianoPitchBindingId.clear(); lane.pianoVelocityBindingId.clear();
            lane.pianoGateBindingId = b;
            lane.pianoNotes = {{0, 4, 60, 0.8f, 1}};
        }
        owner.transport->seekToBeat(mode == 7 ? 0 : 4.99);
        owner.transport->play(); model.update();
        const float before = output.get();
        if(mode == 6) owner.transport->seekToBeat(5.1); // a seek leaves manual hold alone
        else owner.transport->advanceFrameStep(mode == 7 ? 2.6 : 0.02);
        model.update();
        if(mode < 4 || mode == 6) assert(output.get() == before);
        else if(mode == 4 || mode == 5) near(output.get(), 0.2f);
        else if(mode == 7) assert(output.get() == 1.0f); // a frame skipped the entire clip
        else assert(output.get() == 0.0f); // a shared gate still releases
    }
    std::cout << "Curve endpoints passed: exact/overshot ends, repeats, stretch, trims, offsets, "
                 "manual holds, seeks, skipped clips, active-clip priority and gate release.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
