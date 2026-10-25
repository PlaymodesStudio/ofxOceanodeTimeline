#!/usr/bin/env python3
"""Check target explanations against playback and shared end-edit semantics."""
import timeline_regression as fixture

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <cmath>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
void near(double a, double b) { assert(std::abs(a-b) < 1e-5); }
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    ofParameter<float> value("Output", 0.1f, 0.0f, 1.0f);
    ofxOceanodeParameter<float> target; target.bindParameter(value);
    owner.targets[target.getEscapedName()] = &target;
    ofxOceanodeTimelineManager model(&owner);
    auto t = model.createTrack("Main");
    auto b = model.addBinding(t, target, ofxOceanodeTimelineAutomationMode::Add);
    auto c = model.createClip(t, "Phrase", 0, 4);
    auto lane = [&](const std::string& clip, float amount) {
        auto l = model.createLane(t, clip, "Held", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(t, clip, l, b);
        model.setClipNumber(t, clip, l, 0, amount);
        return l;
    };
    const auto l1 = lane(c, 0.2f), l2 = lane(c, 0.3f);
    auto check = [&](double beat) {
        owner.transport->seekToBeat(beat);
        const auto before = model.toJson();
        const float previous = value.get();
        const auto explanation = model.inspectTarget(target.getEscapedName(), beat, false);
        assert(before == model.toJson()); near(value.get(), previous);
        model.update();
        if(explanation.writesValue) near(value.get(), ofToFloat(explanation.value));
        return explanation;
    };
    auto result = check(0.5);
    assert(result.writesValue && result.contributions.size() == 2 && result.bindings.size() == 1);
    assert(result.contributions[0].winningClip && result.contributions[1].winningClip);
    near(ofToFloat(result.value), 0.5);
    assert(result.contributions[0].laneIds == std::vector<std::string>{l1});
    assert(result.contributions[1].laneIds == std::vector<std::string>{l2});
    const auto later = model.createClip(t, "Accent", 1, 2); lane(later, 0.8f);
    result = check(1.5);
    near(ofToFloat(result.value), 0.8);
    assert(std::count_if(result.contributions.begin(), result.contributions.end(), [](const auto& item) { return item.winningClip; }) == 1);
    // Equal start times use later clip order, regardless of draw/selection state.
    const auto tie = model.createClip(t, "Tie", 1, 2); lane(tie, 0.9f);
    result = check(1.5); near(ofToFloat(result.value), 0.9);
    assert(result.contributions.back().clipId == tie && result.contributions.back().winningClip);
    auto t2 = model.createTrack("Offset");
    auto b2 = model.addBinding(t2, target, ofxOceanodeTimelineAutomationMode::Add);
    auto c2 = model.createClip(t2, "Offset", 0, 4);
    auto l3 = model.createLane(t2, c2, "Offset", ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(t2, c2, l3, b2); model.setClipNumber(t2, c2, l3, 0, 0.5f);
    result = check(1.5); assert(result.clamped && result.clampEnabled);
    near(ofToFloat(result.unclampedValue), 1.4); near(ofToFloat(result.value), 1);
    model.setBindingClamp(t, b, false);
    result = model.inspectTarget(target.getEscapedName(), 1.5, false);
    assert(!result.clampEnabled && !result.clamped); near(ofToFloat(result.value), 1.4);
    model.setBindingClamp(t, b, true);
    model.getBinding(t2, b2)->bypass = true;
    result = check(1.5); assert(result.bindings.back().bypass && !result.bindings.back().active);
    model.getBinding(t, b)->hasLiveOverride = true;
    model.getBinding(t, b)->liveOverrideValue = "0.4";
    result = check(1.5); assert(result.bindings.front().liveOverride); near(ofToFloat(result.value), 0.4);
    assert(std::none_of(result.contributions.begin(), result.contributions.end(), [](const auto& item) { return item.winningClip; }));
    model.getBinding(t, b)->hasLiveOverride = false;
    result = check(9); assert(!result.writesValue);
    owner.targets.clear();
    result = model.inspectTarget(target.getEscapedName(), 1.5, false);
    assert(result.missingTarget && !result.writesValue && !result.contributions.empty());
    owner.targets[target.getEscapedName()] = &target;
    model.getBinding(t, b)->bypass = true;
    result = check(1.5); assert(!result.writesValue);
    // LFO controls are not direct target contributions: only its result is.
    const auto lt = model.createTrack("Oscillator");
    const auto lb = model.addBinding(lt, target);
    const auto lc = model.createLfoClip(lt, lb, "LFO", 0, 4);
    result = check(1.5);
    assert(result.contributions.size() == 1 && result.contributions[0].clipId == lc && result.contributions[0].laneIds.empty());
    model.removeTrack(lt);
    // Every blend mode keeps its exact playback semantics in the trace.
    model.getBinding(t, b)->bypass = false;
    model.getBinding(t2, b2)->bypass = false;
    for(auto mode : {ofxOceanodeTimelineAutomationMode::Replace, ofxOceanodeTimelineAutomationMode::Add,
                     ofxOceanodeTimelineAutomationMode::Multiply, ofxOceanodeTimelineAutomationMode::Min,
                     ofxOceanodeTimelineAutomationMode::Max}) {
        model.setBindingMode(t, b, mode); model.setBindingMode(t2, b2, mode);
        check(0.5); check(1.5);
    }
    // Shared piano sources are explained as one voice pool, never as two
    // independently combined gate contributions.
    {
        ofParameter<std::vector<float>> pitches("Pitches", {60,60}, {0,0}, {127,127});
        ofParameter<std::vector<float>> gates("Gates", {0,0}, {0,0}, {1,1});
        ofxOceanodeParameter<std::vector<float>> pitchTarget, gateTarget;
        pitchTarget.bindParameter(pitches); gateTarget.bindParameter(gates);
        owner.targets[pitchTarget.getEscapedName()] = &pitchTarget;
        owner.targets[gateTarget.getEscapedName()] = &gateTarget;
        ofxOceanodeTimelineManager pianoModel(&owner);
        auto pt = pianoModel.createTrack("Instrument");
        auto pb = pianoModel.addBinding(pt, pitchTarget), gb = pianoModel.addBinding(pt, gateTarget);
        auto pc = pianoModel.createClip(pt, "Parts", 0, 4);
        std::vector<std::string> parts;
        for(int key : {48,72}) {
            auto part = pianoModel.createLane(pt, pc, "Part", ofxOceanodeTimelineLaneType::PianoRoll);
            parts.push_back(part);
            pianoModel.addBindingToLane(pt, pc, part, pb); pianoModel.addBindingToLane(pt, pc, part, gb);
            pianoModel.getLane(pt, pc, part)->pianoNotes = {{0, 2, key, 1, 1}};
        }
        auto trace = pianoModel.inspectTarget(gateTarget.getEscapedName(), 1, true);
        assert(trace.contributions.size() == 1 && trace.contributions.front().laneIds == parts);
        assert(trace.writesValue && trace.contributions.front().winningClip);
        owner.transport->seekToBeat(1); owner.transport->play(); pianoModel.update();
        assert(gates.get().size() >= 2 && gates.get()[0] == 1 && gates.get()[1] == 1);
        trace = pianoModel.inspectTarget(gateTarget.getEscapedName(), 5, true);
        assert(trace.writesValue && trace.contributions.empty());
        owner.transport->seekToBeat(5); pianoModel.update();
        assert(gates.get()[0] == 0 && gates.get()[1] == 0);
        owner.transport->pause();
        pianoModel.clear();
        owner.targets.erase(pitchTarget.getEscapedName()); owner.targets.erase(gateTarget.getEscapedName());
    }
    // The same end operation preserves source content for every editor type.
    auto* clip = model.getClip(t, c);
    clip->durationBeats = 8; clip->contentDurationBeats = 4; clip->contentStretch = 2; clip->repeatContent = true;
    const auto source = model.getLane(t, c, l1)->curvePoints;
    model.beginTimelineEdit();
    assert(model.resizeClipEnd(t, c, 16, ofxOceanodeTimelineEndEdit::Repeat));
    model.endTimelineEdit();
    near(clip->contentDurationBeats, 4); near(clip->contentStretch, 2); assert(clip->repeatContent);
    assert(model.undoTimelineEdit()); clip = model.getClip(t, c); near(clip->durationBeats, 8);
    assert(model.redoTimelineEdit()); clip = model.getClip(t, c); near(clip->durationBeats, 16);
    assert(model.resizeClipEnd(t, c, 6, ofxOceanodeTimelineEndEdit::Trim));
    near(clip->contentDurationBeats, 3); near(clip->contentStretch, 2); assert(!clip->repeatContent);
    assert(model.resizeClipEnd(t, c, 12, ofxOceanodeTimelineEndEdit::Stretch));
    near(clip->contentDurationBeats, 3); near(clip->contentStretch, 4); assert(!clip->repeatContent);
    assert(model.getLane(t, c, l1)->curvePoints.size() == source.size());
    assert(model.getLane(t, c, l1)->curvePoints.front().value == source.front().value);
    clip->durationBeats = 0.125; clip->contentDurationBeats = 1024;
    clip->repeatContent = false; clip->contentStretch = 1;
    near(ofxOceanodeTimelineClipTime::effectiveStretch(*clip),
         ofxOceanodeTimelineClipTime::sourceToTimelineBeat(*clip, 1) - clip->startBeat);
    clip->durationBeats = 6; clip->contentDurationBeats = 3;
    near(ofxOceanodeTimelineClipTime::effectiveStretch(*clip), 2);
    assert(model.resizeClipEnd(t, c, 12, ofxOceanodeTimelineEndEdit::Repeat));
    near(clip->contentStretch, 2); near(clip->contentDurationBeats, 3);
    const auto saved = model.toJson();
    model.fromJson(saved); assert(model.toJson() == saved);
    assert(!model.resizeClipEnd(t, c, std::numeric_limits<double>::infinity(), ofxOceanodeTimelineEndEdit::Trim));
    assert(!model.resizeClipEnd(t, "missing", 4, ofxOceanodeTimelineEndEdit::Repeat));
    const auto wt = model.createWaveTrack("Audio");
    const auto wc = model.createClip(wt, "Sample", 0, 4);
    auto* wave = model.getClip(wt, wc);
    wave->contentDurationBeats = 4; wave->waveFileDurationBeats = 4;
    for(auto operation : {ofxOceanodeTimelineEndEdit::Repeat, ofxOceanodeTimelineEndEdit::Trim, ofxOceanodeTimelineEndEdit::Stretch}) {
        assert(model.resizeClipEnd(wt, wc, 8, operation));
        near(wave->contentDurationBeats, 4); near(wave->contentStretch, 2); assert(!wave->repeatContent);
    }
    std::cout << "Clarity regression passed: exact read-only contributions, overlap/ties, blending, clamp, overrides, bypass, missing targets, LFO, repeat/trim/stretch, history and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
