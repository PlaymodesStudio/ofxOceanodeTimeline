#!/usr/bin/env python3
"""Reversing a clip plays what it played backwards, at the same speed.

Every lane type is sampled across the clip before and after reverseClip: the
value at a beat afterwards is the value at its mirror beforehand. Covered:
sigmoid and linear curves, Log / Exp (fitted, so within a tolerance), held
numbers, steps (a pattern that fits the content, and one that does not),
toggles, value regions, a multi slider and piano gates, on a one-shot clip
and on a repeating clip with a partial last repeat and a content offset.
Reversing twice restores what it played; LFO clips are refused.
"""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    std::vector<ofParameter<float>> values(8);
    std::vector<ofxOceanodeParameter<float>> parameters(8);
    const char* names[] = {"Sigmoid", "Linear", "Power", "Held", "Steps", "Toggle", "Region", "Slider"};
    for(int i = 0; i < 8; ++i) {
        values[i].set(names[i], 0, -10, 10);
        parameters[i].bindParameter(values[i]);
        owner.targets[parameters[i].getEscapedName()] = &parameters[i];
    }
    ofParameter<float> gateValue("Gate", 0, 0, 1), pitchValue("Pitch", 60, 0, 127);
    ofxOceanodeParameter<float> gate, pitch;
    gate.bindParameter(gateValue); pitch.bindParameter(pitchValue);
    owner.targets[gate.getEscapedName()] = &gate; owner.targets[pitch.getEscapedName()] = &pitch;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);

    auto build = [&](double start, double duration, double content, bool repeat, double offset, double stepPattern) {
        const auto t = model.createTrack("Reverse");
        std::vector<std::string> b;
        for(auto& p : parameters) b.push_back(model.addBinding(t, p));
        const auto gb = model.addBinding(t, gate), pb = model.addBinding(t, pitch);
        const auto c = model.createClip(t, "Clip", start, duration);
        model.setClipContentDuration(t, c, content, repeat);
        model.setClipTiming(t, c, start, duration);
        auto lane = [&](const char* name, ofxOceanodeTimelineLaneType type, const std::string& binding) {
            const auto id = model.createLane(t, c, name, type);
            model.addBindingToLane(t, c, id, binding);
            return model.getLane(t, c, id);
        };
        auto* sigmoid = lane("Sigmoid", ofxOceanodeTimelineLaneType::Curve, b[0]);
        sigmoid->curveInterpolation = "Sigmoid";
        sigmoid->curvePoints = {{0.3, 0.1f}, {1.7, 0.9f}, {2.6, 0.2f}};
        sigmoid->curveTensions = {{0.25f, 3.0f}, {0.7f, 0.5f}};
        auto* linear = lane("Linear", ofxOceanodeTimelineLaneType::Curve, b[1]);
        linear->curvePoints = {{0.0, 0.0f}, {1.0, 1.0f}, {1.0, 0.3f}, {content, 0.6f}}; // a jump at 1
        auto* power = lane("Power", ofxOceanodeTimelineLaneType::Curve, b[2]);
        power->curveInterpolation = "Log / Exp";
        power->curvePoints = {{0.0, 0.0f}, {content, 1.0f}};
        power->curveTensions = {{0.5f, 3.0f}};
        auto* held = lane("Held", ofxOceanodeTimelineLaneType::Number, b[3]);
        model.setClipNumber(t, c, held->id, 0.0, 0.2f);
        model.setClipNumber(t, c, held->id, 0.8, 0.7f);
        model.setClipNumber(t, c, held->id, 2.1, 0.4f);
        auto* steps = lane("Steps", ofxOceanodeTimelineLaneType::Step, b[4]);
        steps->beatsPerStep = 0.5; steps->stepCount = static_cast<int>(stepPattern / 0.5);
        steps->step.steps = {{0.0, 0.0, "1"}, {1.0, 0.5, "1"}};
        auto* toggle = lane("Toggle", ofxOceanodeTimelineLaneType::MultiGate, b[5]);
        toggle->multiRowCount = 1; toggle->multiGateRows = {{{0.25, 1.0, 1}, {2.0, 0.5, 1}}};
        auto* region = lane("Region", ofxOceanodeTimelineLaneType::MultiValue, b[6]);
        region->multiRowCount = 1; region->multiValueRows = {{{0.5, 1.5, 0.8f}}};
        auto* slider = lane("Slider", ofxOceanodeTimelineLaneType::MultiSlider, b[7]);
        slider->beatsPerStep = 0.5; slider->stepCount = 4; slider->multiSliderValues = {1, 2, 3, 4};
        slider->valueMin = 0; slider->valueMax = 10;
        const auto piano = model.createLane(t, c, "Notes", ofxOceanodeTimelineLaneType::PianoRoll);
        auto* notes = model.getLane(t, c, piano);
        notes->pianoGateBindingId = gb; notes->pianoPitchBindingId = pb;
        notes->pianoNotes = {{0.25, 0.5, 64}, {1.25, 1.0, 67}};
        model.setClipContentOffset(t, c, offset);
        return std::make_pair(t, c);
    };

    owner.transport->play();
    auto sample = [&](double beat) {
        owner.transport->seekToBeat(beat); model.update(); model.applyAutomation();
        std::array<float, 10> out{};
        for(int i = 0; i < 8; ++i) out[i] = values[i].get();
        out[8] = gateValue.get(); out[9] = gateValue.get() > 0 ? pitchValue.get() : -1.0f;
        return out;
    };
    auto check = [&](const char* name, double start, double duration, double content, bool repeat, double offset, double pattern) {
        const auto [t, c] = build(start, duration, content, repeat, offset, pattern);
        std::vector<double> beats;
        // Away from 16th boundaries (gates and steps change there).
        for(double local = 0.12; local < duration; local += 0.25) beats.push_back(start + local);
        std::vector<std::array<float, 10>> before;
        for(double beat : beats) before.push_back(sample(beat));
        assert(model.reverseClip(t, c));
        const double end = start + duration;
        for(size_t i = 0; i < beats.size(); ++i) {
            const auto after = sample(start + end - beats[i]); // the mirror beat
            for(int k = 0; k < 10; ++k) {
                const float tolerance = k == 2 ? 0.06f : 0.02f; // Log / Exp is fitted
                if(std::abs(after[k] - before[i][k]) > tolerance) {
                    std::cerr << name << ": target " << k << " at " << beats[i] << ": " << before[i][k] << " forward vs "
                              << after[k] << " reversed\n";
                    std::abort();
                }
            }
        }
        // Twice: forwards again.
        assert(model.reverseClip(t, c));
        for(size_t i = 0; i < beats.size(); ++i) {
            const auto again = sample(beats[i]);
            for(int k = 0; k < 10; ++k) if(std::abs(again[k] - before[i][k]) > (k == 2 ? 0.12f : 0.02f)) {
                std::cerr << name << " twice: target " << k << " at " << beats[i] << "\n"; std::abort();
            }
        }
        model.removeTrack(t);
    };
    check("one-shot", 4, 3, 3, false, 0, 1);
    check("stretched one-shot", 4, 6, 3, false, 0, 1);
    check("repeating, partial last repeat, offset", 4, 7, 3, true, 0.75, 1);
    check("partial step pattern", 4, 3, 3, false, 0, 2);

    // LFO clips are refused.
    const auto t = model.createTrack("LFO");
    const auto lb = model.addBinding(t, parameters[0]);
    const auto lfo = model.createLfoClip(t, lb, "LFO", 0, 4);
    assert(!model.reverseClip(t, lfo));
    std::cout << "Reverse clip passed: curves (sigmoid, linear with a jump, Log / Exp), held numbers, steps, toggles, regions, "
                 "multi slider, piano gates; one-shot, stretched, repeating with offset, partial pattern; twice; LFO refused.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
