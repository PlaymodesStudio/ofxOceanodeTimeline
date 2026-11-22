#!/usr/bin/env python3
"""A clip's curves take their first value just before the clip starts.

Values are written once a frame, notes are scheduled to their exact beat:
a level curve starting at 0 must already be 0 when a note on the clip's
first beat plays. While playing, a clip starting within the next two frames
writes its curves' first values now (as a vertical step that close is);
stopped, muted or chance-silenced clips do not, nor do gate-like lanes.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    for(int mode = 0; mode < 5; ++mode) {
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<float> output("Level", 1, 0, 1), gateOutput("Gate", 0, 0, 1);
        ofxOceanodeParameter<float> target, gateTarget;
        target.bindParameter(output); gateTarget.bindParameter(gateOutput);
        owner.targets[target.getEscapedName()] = &target;
        owner.targets[gateTarget.getEscapedName()] = &gateTarget;
        TestTimeline model(&owner);
        const auto t = model.createTrack("Level"), b = model.addBinding(t, target), g = model.addBinding(t, gateTarget);
        // A fade-in from 0 at beat 4, and a step gate on at its first beat.
        const auto c = model.createClip(t, "Fade", 4, 4);
        model.getClip(t, c)->repeatContent = false;
        const auto l = model.createLane(t, c, "Curve", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(t, c, l, b);
        model.getLane(t, c, l)->curvePoints = {{0, 0}, {4, 1}};
        model.getLane(t, c, l)->curveTensions.resize(1);
        const auto s = model.createLane(t, c, "Gate", ofxOceanodeTimelineLaneType::Step);
        model.addBindingToLane(t, c, s, g);
        model.getLane(t, c, s)->beatsPerStep = 1; model.getLane(t, c, s)->stepCount = 4;
        model.setClipStep(t, c, s, 0, "1", 1);
        if(mode == 2) model.getClip(t, c)->muted = true;
        if(mode == 3) model.getClip(t, c)->chance = 0;

        owner.transport->seekToBeat(3.9);
        if(mode != 1) owner.transport->play();
        model.update(); model.applyAutomation();
        assert(output.get() == 1.0f); // nothing ahead yet
        if(mode == 1) owner.transport->seekToBeat(3.98);
        else owner.transport->advanceFrameStep(0.04); // 0.08 beats at 120 BPM
        model.update(); model.applyAutomation();
        const double beat = owner.transport->getState().beatPosition;
        assert(beat < 4.0);
        const bool expectStart = mode == 0 || mode == 4;
        if((output.get() == 0.0f) != expectStart)
            std::cerr << "mode " << mode << " at beat " << beat << ": level " << output.get() << "\n";
        assert((output.get() == 0.0f) == expectStart);
        assert(gateOutput.get() == 0.0f); // a gate never opens early
        if(mode == 4) {
            // Inside the clip it plays on as before.
            owner.transport->advanceFrameStep(1.0); // two beats
            model.update(); model.applyAutomation();
            assert(std::abs(output.get() - 0.52f) < 0.05f);
        }
    }
    std::cout << "Curve start passed: first value written just ahead while playing; not stopped, muted, "
                 "chance-silenced; gates never early; plays on inside the clip.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
