#!/usr/bin/env python3
"""ofxOceanodeTimelineTimeMap: sequence beats to main-timeline beats.

The identity map must be exact (the main timeline plays through it), maps
must round-trip, and chaining must place a nested sequence correctly.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    // Identity: bit-for-bit, so the main timeline evaluates exactly as before.
    const ofxOceanodeTimelineTimeMap identity;
    for(double beat : {0.0, 1e-9, 0.1, 1.0 / 3.0, 7.25, 1e6 + 0.1, -2.5}) {
        assert(identity.toTimeline(beat) == beat);
        assert(identity.toSequence(beat) == beat);
    }

    // A pattern placed at beat 16, at half speed: its beat 2 plays at 20.
    const ofxOceanodeTimelineTimeMap pattern{16.0, 2.0};
    near(pattern.toTimeline(2.0), 20.0);
    near(pattern.toSequence(20.0), 2.0);
    for(double beat : {0.0, 0.5, 3.75, 11.0}) near(pattern.toSequence(pattern.toTimeline(beat)), beat);

    // Nested: a sequence placed at beat 4 of that pattern, twice as fast.
    // Its beat 1 is the pattern's beat 4.5, which plays at 16 + 4.5 * 2 = 25.
    const ofxOceanodeTimelineTimeMap inner{4.0, 0.5};
    const auto nested = pattern.then(inner);
    near(nested.toTimeline(1.0), 25.0);
    near(nested.toTimeline(1.0), pattern.toTimeline(inner.toTimeline(1.0)));
    near(nested.ratio, 1.0);
    near(nested.toSequence(25.0), 1.0);
    // Chaining with the identity changes nothing.
    near(identity.then(pattern).offset, pattern.offset); near(identity.then(pattern).ratio, pattern.ratio);
    near(pattern.then(identity).offset, pattern.offset); near(pattern.then(identity).ratio, pattern.ratio);
    std::cout << "Time map passed: exact identity, round trips, chaining.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
