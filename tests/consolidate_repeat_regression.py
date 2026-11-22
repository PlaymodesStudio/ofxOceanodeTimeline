#!/usr/bin/env python3
"""Consolidating a repeating clip keeps it repeating."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    TestTimeline model(&owner);
    auto near = [](double a, double e) {
        if(std::abs(a - e) > 1e-6) std::cerr << "expected " << e << " got " << a << "\n";
        assert(std::abs(a - e) <= 1e-6);
    };
    const auto t = model.createTrack("Curves");
    // Case A: a 4-beat clip repeated out to 10 beats with the Repeat tool.
    const auto a = model.createClip(t, "A", 0, 4);
    model.createLane(t, a, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.resizeClipEnd(t, a, 10, ofxOceanodeTimelineEndEdit::Repeat);
    model.consolidateClipContent(t, a);
    assert(model.getClip(t, a)->repeatContent);
    near(model.getClip(t, a)->contentDurationBeats, 4);
    near(model.getClip(t, a)->durationBeats, 10);

    // Case B: a 4-beat clip trimmed to 3 with the Repeat tool, extended to 9.
    const auto b = model.createClip(t, "B", 20, 4);
    model.createLane(t, b, "Curve", ofxOceanodeTimelineLaneType::Curve);
    model.resizeClipEnd(t, b, 3, ofxOceanodeTimelineEndEdit::Repeat);
    model.consolidateClipContent(t, b);
    assert(model.getClip(t, b)->repeatContent);
    near(model.getClip(t, b)->contentDurationBeats, 3);
    model.resizeClipEnd(t, b, 9, ofxOceanodeTimelineEndEdit::Repeat);
    assert(model.getClip(t, b)->repeatContent);
    near(model.getClip(t, b)->contentDurationBeats, 3);
    std::cout << "Consolidate repeat passed.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
