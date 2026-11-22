#!/usr/bin/env python3
"""Trimming a step-grid clip longer adds empty time; its pattern does not loop on."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    TestTimeline model(&owner);
    const auto t = model.createTrack("Grids");
    const auto c = model.createClip(t, "Grid", 0, 4);
    const auto stepLane = model.createLane(t, c, "Steps", ofxOceanodeTimelineLaneType::Step);
    const auto sliderLane = model.createLane(t, c, "Slider", ofxOceanodeTimelineLaneType::MultiSlider);
    const auto drumLane = model.createLane(t, c, "Drums", ofxOceanodeTimelineLaneType::DrumGrid);
    auto* clip = model.getClip(t, c);
    for(auto& lane : clip->lanes) {
        if(lane.id == stepLane) {
            // A one-beat pattern (4 cells) looping through the 4-beat clip.
            lane.beatsPerStep = 0.25; lane.stepCount = 4;
            ofxOceanodeTimelineStep step; step.startBeat = 0.0; step.value = "1";
            lane.step.steps = {step};
        } else if(lane.id == sliderLane) {
            lane.beatsPerStep = 0.25; lane.stepCount = 4;
            lane.multiSliderValues = {0.1f, 0.2f, 0.3f, 0.4f};
        } else if(lane.id == drumLane) {
            if(lane.drumRows.empty()) lane.drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(1, {}, 1.0);
            for(auto& row : lane.drumRows) {
                row.length = 1.0;
                ofxOceanodeTimelineDrumStep hit; hit.beat = 0.0;
                row.steps = {hit};
            }
        }
    }
    clip->repeatContent = true; // as the right-edge drag sets it
    assert(model.resizeClipEnd(t, c, 8, ofxOceanodeTimelineEndEdit::Trim));
    clip = model.getClip(t, c);
    assert(!clip->repeatContent);
    assert(std::abs(clip->contentDurationBeats - 8.0) < 1e-6);
    for(const auto& lane : clip->lanes) {
        if(lane.id == stepLane) {
            assert(lane.stepCount == 32);
            assert(lane.step.steps.size() == 4);
            for(size_t i = 0; i < 4; ++i) assert(std::abs(lane.step.steps[i].startBeat - double(i)) < 1e-6);
        } else if(lane.id == sliderLane) {
            assert(lane.stepCount == 32);
            assert(lane.multiSliderValues.size() == 32);
            assert(std::abs(lane.multiSliderValues[13] - 0.2f) < 1e-6);
            assert(lane.multiSliderValues[20] == 0.0f);
        } else if(lane.id == drumLane) {
            for(const auto& row : lane.drumRows) {
                assert(std::abs(row.length - 8.0) < 1e-6);
                assert(row.steps.size() == 4);
                assert(std::abs(row.steps.back().beat - 3.0) < 1e-6);
            }
        }
    }
    // Shortening and extending again changes nothing more.
    assert(model.resizeClipEnd(t, c, 6, ofxOceanodeTimelineEndEdit::Trim));
    assert(model.resizeClipEnd(t, c, 8, ofxOceanodeTimelineEndEdit::Trim));
    for(const auto& lane : model.getClip(t, c)->lanes)
        if(lane.id == stepLane) { assert(lane.stepCount == 32); assert(lane.step.steps.size() == 4); }
    std::cout << "Trim extend grid passed.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
