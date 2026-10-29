#!/usr/bin/env python3
"""Rolls come from a play context, not from state stored on the timeline data.

The main timeline (copy key 0) rolls exactly as before the context existed;
another copy of the same content rolls on its own; and the scheduler's look
ahead into the next loop pass leaves what the current frame plays unchanged.
"""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
// The clip chance roll as it was computed before the play context.
static bool legacySilenced(const ofxOceanodeTimelineClip& clip, uint64_t salt, uint64_t generation, uint64_t pass) {
    const float chance = ofClamp(clip.chance, 0.0f, 1.0f);
    if(chance >= 1.0f) return false;
    if(chance <= 0.0f) return true;
    uint64_t hash = std::hash<std::string>{}(clip.id) ^ salt;
    hash ^= generation * 0xbf58476d1ce4e5b9ULL;
    hash ^= pass * 0xd6e8feb86659fd93ULL;
    hash ^= hash >> 30; hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27; hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f >= chance;
}

int main() {
    ofSeedRandom();
    // ---- copy key 0 is the main timeline: identical to the old rolls ------
    ofxOceanodeTimelinePlayContext main;
    main.generation = 3; main.pass = 5; main.salt = 0x1234567890abcdefULL;
    assert(main.epoch() == ((3 * 0x9e3779b97f4a7c15ULL) ^ (5 * 0xd6e8feb86659fd93ULL) ^ main.salt));
    auto copy = main; copy.instanceKey = 7;
    assert(copy.epoch() != main.epoch());
    assert(main.nextPass().pass == 6 && main.nextPass().generation == 3 && main.pass == 5);
    int differ = 0;
    for(int i = 0; i < 2000; ++i) {
        ofxOceanodeTimelineClip clip;
        clip.id = "timeline_clip_" + std::to_string(i);
        clip.chance = 0.5f;
        assert(ofxOceanodeTimelineClipChanceSilenced(clip, main) == legacySilenced(clip, main.salt, 3, 5));
        const auto next = main.nextPass();
        assert(ofxOceanodeTimelineClipChanceSilenced(clip, next) == legacySilenced(clip, main.salt, 3, 6));
        // Another copy of the same clip rolls for itself, the same way every time.
        const bool inCopy = ofxOceanodeTimelineClipChanceSilenced(clip, copy);
        assert(inCopy == ofxOceanodeTimelineClipChanceSilenced(clip, copy));
        differ += inCopy != ofxOceanodeTimelineClipChanceSilenced(clip, main);
        // Always and never ignore the context.
        clip.chance = 1.0f; assert(!ofxOceanodeTimelineClipChanceSilenced(clip, copy));
        clip.chance = 0.0f; assert(ofxOceanodeTimelineClipChanceSilenced(clip, copy));
    }
    assert(differ > 800 && differ < 1200); // independent: about half differ

    // ---- the scheduler's next-pass look ahead writes nothing --------------
    // The same position evaluated with and without the scheduler probing past
    // the loop end must report the same clip rolls and the same frame values.
    auto frameState = [](bool scheduling) {
        ofxOceanodeContainer owner;
        ofParameter<float> value("Value", 0, 0, 1);
        ofxOceanodeParameter<float> target; target.bindParameter(value);
        owner.targets[target.getEscapedName()] = &target;
        ofxOceanodeTimelineManager model(&owner);
        std::vector<std::string> sent;
        if(scheduling)
            ofxOceanodeScheduling::registerParameterTarget(&target, &model,
                [&sent](const auto& e) { sent.push_back(e.value); return true; });
        model.setSchedulingEnabled(scheduling);
        model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(500);
        const auto t = model.createTrack("Gates");
        const auto b = model.addBinding(t, target);
        std::vector<std::string> clips;
        for(int i = 0; i < 64; ++i) {
            // Back-to-back one-beat gate clips across the loop, each 50%.
            const auto c = model.createClip(t, "Gate", i % 4, 1);
            const auto l = model.createLane(t, c, "Gate", ofxOceanodeTimelineLaneType::MultiGate);
            model.addBindingToLane(t, c, l, b);
            auto* lane = model.getLane(t, c, l);
            lane->multiRowCount = 1; lane->multiGateRows = {{{0.0, 0.5, 1.0f}}};
            model.setClipChance(t, c, 0.5f);
            clips.push_back(c);
        }
        model.setLoopEnabled(true); model.setLoopRange(0, 4);
        owner.transport->seekToBeat(3.8); owner.transport->play();
        model.update();
        std::string rolls;
        for(const auto& c : clips) rolls += model.isClipChanceSilenced(*model.getClip(t, c)) ? '1' : '0';
        rolls += model.inspectTarget(target.getEscapedName(), 3.8, true).writesValue ? "w" : "-";
        if(scheduling) {
            assert(!sent.empty()); // it did look past the wrap
            ofxOceanodeScheduling::unregisterOwner(&model);
        }
        return rolls;
    };
    // The salt is per session, so compare runs that share it: seed equally.
    ofSeedRandom(42); const auto plain = frameState(false);
    ofSeedRandom(42); const auto scheduled = frameState(true);
    assert(plain == scheduled);
    std::cout << "Play context passed: main timeline rolls unchanged, copies roll independently, look ahead leaves the frame alone.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
