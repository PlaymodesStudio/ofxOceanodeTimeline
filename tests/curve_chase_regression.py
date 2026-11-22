#!/usr/bin/env python3
"""Curves chase their value into the gaps between clips.

When playback starts, after a seek or a loop wrap, a curve binding no clip
covers takes the value its timeline implies at the playhead: the last value
of the previous clip, or else the first value of the next one. Written once
(the gap stays free for manual control afterwards), never while stopped,
never from muted clips, never for gate-like lanes.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> output("Level", 1, 0, 1), gateOutput("Gate", 0, 0, 1);
    ofxOceanodeParameter<float> target, gateTarget;
    target.bindParameter(output); gateTarget.bindParameter(gateOutput);
    owner.targets[target.getEscapedName()] = &target;
    owner.targets[gateTarget.getEscapedName()] = &gateTarget;
    TestTimeline model(&owner);
    const auto t = model.createTrack("Level"), b = model.addBinding(t, target), g = model.addBinding(t, gateTarget);
    // A: beats 8-10, a curve 0.2 -> 0.3. B: beats 16-20, a fade-in 0 -> 1
    // with a step gate on its first beat.
    const auto a = model.createClip(t, "A", 8, 2);
    model.getClip(t, a)->repeatContent = false;
    const auto al = model.createLane(t, a, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t, a, al, b);
    model.getLane(t, a, al)->curvePoints = {{0, 0.2f}, {2, 0.3f}};
    model.getLane(t, a, al)->curveTensions.resize(1);
    const auto c = model.createClip(t, "Fade", 16, 4);
    model.getClip(t, c)->repeatContent = false;
    const auto l = model.createLane(t, c, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t, c, l, b);
    model.getLane(t, c, l)->curvePoints = {{0, 0}, {4, 1}};
    model.getLane(t, c, l)->curveTensions.resize(1);
    const auto s = model.createLane(t, c, "Gate", ofxOceanodeTimelineLaneType::Step);
    model.addBindingToLane(t, c, s, g);
    model.getLane(t, c, s)->beatsPerStep = 1; model.getLane(t, c, s)->stepCount = 4;
    model.setClipStep(t, c, s, 0, "1", 1);
    auto frame = [&](double seconds) {
        if(seconds > 0.0) owner.transport->advanceFrameStep(seconds);
        model.update(); model.applyAutomation();
    };
    auto approx = [](float value, float expected) { return std::abs(value - expected) < 0.01f; };

    // Stopped: nothing is chased.
    owner.transport->seekToBeat(2);
    frame(0);
    assert(output.get() == 1.0f);

    // Play from before any clip: A's first value (the nearest next clip).
    owner.transport->play();
    frame(0);
    if(!approx(output.get(), 0.2f)) std::cerr << "before A: " << output.get() << "\n";
    assert(approx(output.get(), 0.2f));
    assert(gateOutput.get() == 0.0f);

    // Written once: the gap is free again while playing on through it.
    output = 0.7f;
    frame(0.1);
    assert(output.get() == 0.7f);

    // A seek into the gap between A and B: A's last value, not B's first.
    output = 1.0f;
    owner.transport->seekToBeat(12);
    frame(0);
    if(!approx(output.get(), 0.3f)) std::cerr << "between A and B: " << output.get() << "\n";
    assert(approx(output.get(), 0.3f));

    // Inside a clip the clip wins.
    owner.transport->seekToBeat(18);
    frame(0);
    assert(approx(output.get(), 0.5f));

    // A muted next clip offers nothing: before A muted, B is the next one.
    assert(model.setClipMuted(t, a, true));
    output = 1.0f;
    owner.transport->seekToBeat(2);
    frame(0);
    if(!approx(output.get(), 0.0f)) std::cerr << "A muted: " << output.get() << "\n";
    assert(approx(output.get(), 0.0f));
    assert(gateOutput.get() == 0.0f); // a gate never opens early

    // Pause and play again: chased again.
    owner.transport->pause();
    frame(0);
    output = 1.0f;
    frame(0);
    assert(output.get() == 1.0f);
    owner.transport->play();
    frame(0);
    assert(approx(output.get(), 0.0f));

    // A loop wrap from inside B back into the gap after A: A's last value.
    assert(model.setClipMuted(t, a, false));
    model.setLoop(true, 12.0, 20.0);
    owner.transport->seekToBeat(19.9);
    frame(0);
    frame(0.02); // continuous play inside B
    assert(output.get() > 0.9f);
    frame(0.1); // crosses 20: the timeline wraps it to ~12.1
    // (Past B without the wrap it would hold B's end, 1.)
    if(!approx(output.get(), 0.3f)) std::cerr << "after wrap: " << output.get() << "\n";
    assert(approx(output.get(), 0.3f));

    std::cout << "Curve chase passed: gaps take the previous clip's last value, else the next clip's first; "
                 "once per play/seek/loop wrap; not stopped, muted or for gates; clips covering the beat win.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
