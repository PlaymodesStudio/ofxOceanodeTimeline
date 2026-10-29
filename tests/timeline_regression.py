#!/usr/bin/env python3
"""Exercise the full production timeline model using real OF parameters and JSON.

The container adapter supplies only transport and parameter lookup. No canvas,
audio backend, hardware MIDI, or application event loop is exercised. Requires
the sibling macOS openFrameworks libraries, or OCEANODE_TEST_OF_ROOT.
"""
from pathlib import Path
import os
import subprocess
import tempfile

import sys
ADDON = Path(__file__).resolve().parents[1]
CORE = Path(os.environ.get("OCEANODE_CORE_ROOT", ADDON.parent / "ofxOceanode"))
sys.path.insert(0, str(CORE / "tests"))
from of_environment import ROOT, of_root

CONTAINER = r'''
#pragma once
#include "ofMain.h"
#include "ofxOceanodeTransport.h"
#include "ofxOceanodeParameter.h"
class ofxOceanodeContainer {
public:
    std::shared_ptr<ofxOceanodeTransport> transport = std::make_shared<ofxOceanodeTransport>();
    std::map<std::string, ofxOceanodeAbstractParameter*> targets;
    auto getTransport() const { return transport; }
    auto getTransportState() const { return transport->getState(); }
    void setBpm(float bpm, bool = true) { transport->setBpm(bpm); }
    std::string getTimelineParameterPath(ofxOceanodeAbstractParameter& parameter) const {
        return parameter.getEscapedName();
    }
    // A flat graph: every parameter is in this one container.
    std::string getTimelineParameterPath(ofxOceanodeAbstractParameter& parameter, const ofxOceanodeContainer&) const {
        return parameter.getEscapedName();
    }
    ofxOceanodeAbstractParameter* findTimelineParameter(const std::string& path) const {
        auto it = targets.find(path);
        return it == targets.end() ? nullptr : it->second;
    }
};
'''

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include "Controls/Editors/ofxOceanodeTimelineEditorView.h"
#include <cassert>
#include <cmath>
#include <iostream>
// This fixture has no connections or scopes. Parameter destruction is isolated.
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
static void checkNear(double actual, double expected, int line) {
    if(std::abs(actual - expected) >= 1e-5) {
        std::cerr << "Line " << line << ": expected " << expected << ", got " << actual << "\n";
        std::abort();
    }
}
#define near(actual, expected) checkNear(actual, expected, __LINE__)
int main() {
    ofSeedRandom(); // OF normally constructs its random engine during app startup.
    // Duplicate all editable lane types in place, retaining their entire
    // serialized content/settings but giving each an independent identity.
    {
        ofxOceanodeTimelineManager copies;
        const auto t = copies.createTrack("Copies");
        for(auto type : {ofxOceanodeTimelineLaneType::Step, ofxOceanodeTimelineLaneType::Curve,
                        ofxOceanodeTimelineLaneType::PianoRoll, ofxOceanodeTimelineLaneType::Number,
                        ofxOceanodeTimelineLaneType::Dropdown, ofxOceanodeTimelineLaneType::MultiSlider,
                        ofxOceanodeTimelineLaneType::MultiValue, ofxOceanodeTimelineLaneType::MultiGate}) {
            const auto c = copies.createClip(t, "Copy", 0, 4);
            const auto l = copies.createLane(t, c, "Source", type);
            const auto tail = copies.createLane(t, c, "Tail");
            auto* source = copies.getLane(t, c, l);
            source->pianoNotes = {{0, 1, 36, 0.3f, 0.7f}};
            source->multiValueRows = {{{0, 1, 0.4f}}};
            source->multiGateRows = {{{0, 1}}};
            source->multiSliderValues = {0.2f, 0.7f};
            source->curvePoints = {{0, 0.2f}, {1, 0.8f}};
            source->valueQuantizeSteps = 7;
            const auto d = copies.duplicateLane(t, c, l);
            assert(!d.empty() && d != l);
            const auto& lanes = copies.getClip(t, c)->lanes;
            assert(lanes.size() == 3 && lanes[0].id == l && lanes[1].id == d && lanes[2].id == tail);
            const auto json = copies.toJson()["tracks"][0]["clips"].back()["lanes"];
            auto expected = json[0]; expected["id"] = d;
            // A duplicate rolls on its own seed.
            assert(json[1]["probabilitySeed"] != json[0]["probabilitySeed"]);
            expected["probabilitySeed"] = json[1]["probabilitySeed"];
            assert(json[1] == expected);
            copies.getLane(t, c, d)->pianoNotes[0].pitch = 72;
            assert(copies.getLane(t, c, l)->pianoNotes[0].pitch == 36);
            const auto saved = copies.toJson(); copies.fromJson(saved);
            assert(copies.getClip(t, c)->lanes[1].id == d);
            assert(copies.getLane(t, c, d)->pianoNotes[0].pitch == 72);
            assert(copies.duplicateLane(t, c, "missing").empty());
        }
        const auto lfo = copies.createLfoClip(t, "", "LFO", 0, 4);
        assert(copies.duplicateLane(t, lfo, copies.getClip(t, lfo)->lanes.front().id).empty());
        assert(copies.duplicateLane("missing", "missing", "missing").empty());
    }
    // Independent bass/melody piano lanes share one set of note bindings
    // and one voice pool. Neither lane replaces the other's held notes.
    {
        ofxOceanodeContainer partsOwner;
        // Frozen, and clock-driven so its notes are scheduled (see note_order_regression).
        partsOwner.transport->setDriverMode(TransportDriverMode::External);
        ofParameter<std::vector<float>> pitches("Part Pitch", {60, 60, 60, 60}, {0}, {127});
        ofParameter<std::vector<float>> gates("Part Gate", {0, 0, 0, 0}, {0}, {1});
        ofParameter<std::vector<float>> velocities("Part Velocity", {0, 0, 0, 0}, {0}, {1});
        ofxOceanodeParameter<std::vector<float>> pitch, gate, velocity;
        pitch.bindParameter(pitches); gate.bindParameter(gates); velocity.bindParameter(velocities);
        for(auto* parameter : {&pitch, &gate, &velocity})
            partsOwner.targets[parameter->getEscapedName()] = parameter;
        ofxOceanodeTimelineManager parts(&partsOwner);
        parts.setSchedulingLookaheadAdaptive(false); // the default 120 ms window
        const auto t = parts.createTrack("Parts");
        const auto pb = parts.addBinding(t, pitch), gb = parts.addBinding(t, gate), vb = parts.addBinding(t, velocity);
        const auto group = parts.createNoteGroup(t, "Instrument", pb, gb, vb);
        parts.setNoteGroupVoices(t, group, 4);
        std::string bass;
        const auto c = parts.createNoteGroupClip(t, group, "Phrase", 0, 4, &bass);
        parts.getLane(t, c, bass)->pianoNotes = {{0, 1, 36, 0.3f, 1}};
        const auto melody = parts.duplicateLane(t, c, bass);
        assert(parts.getLane(t, c, melody)->bindingIds == parts.getLane(t, c, bass)->bindingIds);
        assert(parts.getTrack(t)->bindings.size() == 3);
        parts.getLane(t, c, melody)->pianoNotes = {{0, 2, 72, 0.9f, 1}};
        auto evaluate = [&](double beat) {
            partsOwner.transport->seekToBeat(beat);
            partsOwner.transport->play(); parts.update();
        };
        evaluate(0.5);
        near(pitches.get()[0], 36); near(pitches.get()[1], 72);
        near(gates.get()[0], 1); near(gates.get()[1], 1); near(gates.get()[2], 0);
        near(velocities.get()[0], 0.3); near(velocities.get()[1], 0.9);
        evaluate(1.5);
        near(gates.get()[0], 0); near(gates.get()[1], 1);
        evaluate(2.5); near(gates.get()[0], 0); near(gates.get()[1], 0);
        const auto saved = parts.toJson(); parts.fromJson(saved);
        evaluate(0.5); near(gates.get()[0], 1); near(gates.get()[1], 1);
        parts.setClipTiming(t, c, 0, 8);
        evaluate(4.5); near(gates.get()[0], 1); near(gates.get()[1], 1);
        parts.setClipTiming(t, c, 0, 4);
        parts.getLane(t, c, melody)->pianoNotes[0].probability = 0;
        evaluate(0.5); near(gates.get()[0], 1); near(gates.get()[1], 0);
        parts.getLane(t, c, melody)->pianoNotes[0].probability = 1;
        // Mono is local to a part: a bass replacement leaves melody sounding.
        parts.getLane(t, c, bass)->pianoMonophonic = true;
        parts.getLane(t, c, bass)->pianoNotes = {{0, 2, 36, 0.3f, 1}, {0.5, 1.5, 40, 0.4f, 1}};
        evaluate(0.75);
        int active = 0;
        for(size_t i = 0; i < gates.get().size(); ++i) if(gates.get()[i] > 0) {
            assert(pitches.get()[i] == 40 || pitches.get()[i] == 72); ++active;
        }
        assert(active == 2);
        // Group voice limits still apply across the two parts.
        parts.setNoteGroupVoices(t, group, 1);
        evaluate(0.75); near(pitches.get()[0], 40); near(gates.get()[0], 1);
        near(gates.get()[1], 0);
        parts.setNoteGroupVoices(t, group, 4);
        std::vector<ofxOceanodeScheduledParameterEvent> events;
        ofxOceanodeScheduling::registerParameterTarget(&gate, &parts,
            [&](const auto& event) { events.push_back(event); return true; });
        evaluate(1.95);
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            if(event.isCorrection) return false;
            const auto values = ofSplitString(event.value, ",");
            return values.size() >= 2 && ofToFloat(values[0]) == 0 && ofToFloat(values[1]) == 0;
        }));
        partsOwner.transport->pause();
        ofxOceanodeScheduling::unregisterOwner(&parts);
        parts.removeLane(t, c, melody);
        evaluate(0.75); near(pitches.get()[0], 40); near(gates.get()[0], 1);
    }
    // Render the real metronome without opening a device. Verify accents,
    // denominator beats, discontinuities, loop wraps and silence on pause.
    for(unsigned int sampleRate : {44100u, 48000u}) {
        auto clock = std::make_shared<ofxOceanodeTransport>();
        ofxOceanodeTimelineMetronome click(clock);
        click.setMeter(3, 8);
        ofSoundBuffer audio;
        audio.allocate(sampleRate / 20, 2);
        audio.setSampleRate(sampleRate);
        ofxOceanodeTransportState state;
        state.isPlaying = true;
        state.driverMode = TransportDriverMode::FrameStep;
        auto render = [&](double beat) {
            state.beatPosition = beat;
            click.render(audio, state, state.steadyTimeUs);
            double energy = 0;
            for(size_t i = 0; i < audio.size(); i += 2) {
                near(audio[i], audio[i + 1]);
                energy += audio[i] * audio[i];
            }
            return energy;
        };
        const double accent = render(0);
        assert(accent > 0);
        near(render(0), 0); // same beat never retriggers
        const double weak = render(0.5);
        assert(weak > 0 && weak < accent);
        near(render(1.0), weak);
        near(render(1.5), accent); // next 3/8 bar
        state.generation++;
        near(render(0.25), 0); // seeking between beats stays quiet
        near(render(0.5), weak);
        state.loopCount++;
        near(render(0), accent);
        state.loopCount++;
        near(render(0.25), 0); // fractional loop start is not a beat
        near(render(0.5), weak);
        state.isPlaying = false;
        near(render(0), 0);
        state.isPlaying = true;
        near(render(0), accent);
        // A real-time buffer straddling a beat starts the click at that sample.
        state.generation++;
        state.driverMode = TransportDriverMode::RealTime;
        state.beatPosition = 0.48;
        click.render(audio, state, state.steadyTimeUs);
        const size_t boundaryFrame = sampleRate / 100;
        for(size_t i = 0; i < boundaryFrame * 2; ++i) near(audio[i], 0);
        assert(std::abs(audio[(boundaryFrame + 2) * 2]) > 0);
    }
    // All editors and arrangement gestures share one tempo-aware coordinate
    // mapping. Cover the inverse beyond its initial search bound as well as
    // an automated tempo map, where a fixed pixels-per-beat formula fails.
    {
        ofxOceanodeTimelineManager model(nullptr);
        model.getViewState().pixelsPerSecond = 80.0f;
        ofxOceanodeTimelineEditorView view(model, 120.0f, 4.0, true, 0.25);
        near(view.beatToPixels(2.0), 80.0);
        near(view.pixelsToBeat(80.0f), 2.0);
        near(view.beatToPixels(-2.0), 0.0);
        near(view.pixelsToBeat(-10.0f), 0.0);
        near(view.snapBeat(1.37), 1.25);
        near(view.snapBeat(-1.0), 0.0);
        near(view.editIncrement(), 0.25);
        near(view.displayGridBeats(), 0.25);
        ofxOceanodeTimelineEditorView unsnapped(model, 120.0f, 4.0, false, 0.25);
        near(unsnapped.snapBeat(1.37), 1.37);
        near(unsnapped.displayGridBeats(), 0.25); // magnet does not hide the grid
        ofxOceanodeTimelineEditorView noGrid(model, 120.0f, 4.0, true, 0.0);
        near(noGrid.snapBeat(1.37), 1.37);
        near(noGrid.editIncrement(), 1.0 / 24.0);
        near(noGrid.displayGridBeats(), 1.0);
        model.setBpmRange(30.0f, 300.0f);
        model.getBpmAutomationPoints() = {{0.0, 60.0f}, {8.0, 180.0f}};
        model.setBpmInterpolation("Linear");
        model.setBpmAutomationEnabled(true);
        for(double beat : {0.0, 0.25, 2.5, 8.0, 32.0, 128.0})
            near(view.pixelsToBeat(view.beatToPixels(beat)), beat);
    }
    // Number lanes hold absolute values between arbitrarily placed entries.
    {
        ofxOceanodeContainer numberOwner;
        // Frozen, and clock-driven so its notes are scheduled (see note_order_regression).
        numberOwner.transport->setDriverMode(TransportDriverMode::External);
        ofParameter<float> output("Number", 0.0f, -100.0f, 100.0f);
        ofxOceanodeParameter<float> target; target.bindParameter(output);
        numberOwner.targets[target.getEscapedName()] = &target;
        ofxOceanodeTimelineManager numbers(&numberOwner);
        numbers.setSchedulingLookaheadAdaptive(false); // the default 120 ms window
        const auto t = numbers.createTrack("Numbers");
        const auto b = numbers.addBinding(t, target);
        const auto c = numbers.createClip(t, "Numbers", 0.0, 4.0);
        numbers.setClipContentDuration(t, c, 4.0, false);
        const auto l = numbers.createLane(t, c, "Number", ofxOceanodeTimelineLaneType::Number);
        numbers.addBindingToLane(t, c, l, b);
        assert(numbers.setClipNumber(t, c, l, 0.0, -12.5f));
        assert(numbers.setClipNumber(t, c, l, 1.375, 42.25f));
        assert(numbers.setClipNumber(t, c, l, 3.0, 7.0f));
        assert(numbers.setClipNumber(t, c, l, 3.0, 8.0f));
        assert(!numbers.setClipNumber(t, c, l, -1.0, 0.0f));
        assert(!numbers.setClipNumber(t, c, l, 2.0, std::numeric_limits<float>::infinity()));
        assert(numbers.getLane(t, c, l)->curvePoints.size() == 3);
        auto evaluate = [&](double beat, float expected) {
            numberOwner.transport->seekToBeat(beat); numbers.update();
            if(std::abs(output.get() - expected) >= 1e-5) std::cerr << "Number beat " << beat << "\n";
            near(output.get(), expected);
        };
        evaluate(0.0, -12.5f); evaluate(1.374, -12.5f);
        evaluate(1.375, 42.25f); evaluate(2.9, 42.25f); evaluate(3.5, 8.0f);
        evaluate(0.5, -12.5f); // seeking backwards recomputes the held value
        std::vector<ofxOceanodeScheduledParameterEvent> events;
        ofxOceanodeScheduling::registerParameterTarget(&target, &numbers,
            [&](const auto& event) { events.push_back(event); return true; });
        numberOwner.transport->seekToBeat(1.3);
        numberOwner.transport->play(); numbers.update();
        assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return !event.isCorrection && std::abs(ofToFloat(event.value) - 42.25f) < 1e-5;
        }));
        numberOwner.transport->pause();
        ofxOceanodeScheduling::unregisterOwner(&numbers);
        const auto savedNumbers = numbers.toJson();
        numbers.fromJson(savedNumbers);
        assert(numbers.getLane(t, c, l)->type == ofxOceanodeTimelineLaneType::Number);
        evaluate(2.0, 42.25f);
        const auto right = numbers.splitClip(t, c, 2.0);
        assert(!right.empty());
        evaluate(1.9, 42.25f); evaluate(2.0, 42.25f); evaluate(3.5, 8.0f);
        numbers.getClip(t, right)->repeatContent = true;
        numbers.setClipTiming(t, right, 2.0, 4.0);
        evaluate(4.25, 42.25f); evaluate(5.5, 8.0f);
    }
    // Dropdown lanes select a dropdown parameter's options over time.
    {
        ofxOceanodeContainer dropdownOwner;
        dropdownOwner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<int> mode("Mode", 0, 0, 3);
        ofxOceanodeParameter<int> target; target.bindParameter(mode);
        target.setDropdownOptions({"Off", "Slow", "Fast", "Strobe"});
        dropdownOwner.targets[target.getEscapedName()] = &target;
        assert(ofxOceanodeTimelineDropdownOptions(target).size() == 4);
        ofxOceanodeTimelineManager dropdowns(&dropdownOwner);
        const auto t = dropdowns.createTrack("Modes");
        const auto b = dropdowns.addBinding(t, target);
        assert(dropdowns.getBinding(t, b)->laneType == ofxOceanodeTimelineLaneType::Dropdown);
        const auto c = dropdowns.createClip(t, "Modes", 0.0, 4.0);
        dropdowns.setClipContentDuration(t, c, 4.0, false);
        const auto l = dropdowns.createLane(t, c, "Mode", ofxOceanodeTimelineLaneType::Dropdown);
        dropdowns.addBindingToLane(t, c, l, b);
        assert(dropdowns.setClipNumber(t, c, l, 0.0, 2.0f));
        assert(dropdowns.setClipNumber(t, c, l, 1.0, 3.0f));
        assert(dropdowns.setClipNumber(t, c, l, 2.5, 1.0f));
        auto evaluate = [&](double beat, int expected) {
            dropdownOwner.transport->seekToBeat(beat); dropdowns.update();
            if(mode.get() != expected) std::cerr << "Dropdown beat " << beat << ": " << mode.get() << "\n";
            assert(mode.get() == expected);
        };
        evaluate(0.5, 2); evaluate(1.0, 3); evaluate(2.4, 3); evaluate(3.0, 1);
        const auto saved = dropdowns.toJson();
        dropdowns.fromJson(saved);
        assert(dropdowns.getLane(t, c, l)->type == ofxOceanodeTimelineLaneType::Dropdown);
        assert(dropdowns.getLane(t, c, l)->curveInterpolation == "Step");
        evaluate(1.5, 3);
        // switching a Number lane to Dropdown rounds its values to indices
        assert(dropdowns.setClipLaneType(t, c, l, ofxOceanodeTimelineLaneType::Number));
        dropdowns.getLane(t, c, l)->curvePoints[0].value = 1.6f;
        assert(dropdowns.setClipLaneType(t, c, l, ofxOceanodeTimelineLaneType::Dropdown));
        near(dropdowns.getLane(t, c, l)->curvePoints[0].value, 2.0);
        const auto right = dropdowns.splitClip(t, c, 2.0);
        assert(!right.empty());
        evaluate(2.2, 3); evaluate(3.0, 1);
    }
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> value("Value", 0.0f, 0.0f, 127.0f);
    ofxOceanodeParameter<float> parameter;
    parameter.bindParameter(value);
    owner.targets[parameter.getEscapedName()] = &parameter;
    ofxOceanodeTimelineManager timeline(&owner);
    const auto track = timeline.createTrack("Notes");
    const auto binding = timeline.addBinding(track, parameter);
    const auto clip = timeline.createClip(track, "Phrase", 0.0, 2.0);
    const auto lane = timeline.createLane(track, clip, "Pitch", ofxOceanodeTimelineLaneType::Step);
    assert(!binding.empty() && !clip.empty() && !lane.empty());
    assert(timeline.addBindingToLane(track, clip, lane, binding));
    assert(timeline.setClipStep(track, clip, lane, 0.0, "64", 1.0));
    assert(timeline.setClipStep(track, clip, lane, 1.0, "72", 1.0));
    owner.transport->seekToBeat(0.5); timeline.update(); near(value.get(), 64.0);
    owner.transport->seekToBeat(1.5); timeline.update(); near(value.get(), 72.0);

    // A second track shares the parameter without stealing its original binding.
    const auto addTrack = timeline.createTrack("Transpose");
    const auto addBinding = timeline.addBinding(addTrack, parameter, ofxOceanodeTimelineAutomationMode::Add);
    const auto addClip = timeline.createClip(addTrack, "Offset", 0.0, 2.0);
    const auto addLane = timeline.createLane(addTrack, addClip, "Offset", ofxOceanodeTimelineLaneType::Step);
    assert(timeline.addBindingToLane(addTrack, addClip, addLane, addBinding));
    assert(timeline.setClipStep(addTrack, addClip, addLane, 0.0, "2", 2.0));
    owner.transport->seekToBeat(0.5); timeline.update(); near(value.get(), 66.0);
    timeline.setLoopRange(0.0, 2.0); timeline.setLoopEnabled(true);
    owner.transport->seekToBeat(1.9); owner.transport->play();
    owner.transport->advanceFrameStep(0.2); timeline.update();
    near(owner.transport->getState().beatPosition, 0.3); near(value.get(), 66.0);
    owner.transport->pause();

    // Capture manually edited states, then interpolate them after seeking.
    ofParameter<float> level("Level", 0.25f, 0.0f, 1.0f);
    ofxOceanodeParameter<float> levelParameter; levelParameter.bindParameter(level);
    owner.targets[levelParameter.getEscapedName()] = &levelParameter;
    const auto captureTrack = timeline.createTrack("Captured level");
    const auto captureBinding = timeline.addBinding(captureTrack, levelParameter);
    assert(timeline.captureStateAsCurvePoints(captureTrack, captureBinding, 0.0) == 1);
    level = 0.75f;
    assert(timeline.captureStateAsCurvePoints(captureTrack, captureBinding, 2.0) == 1);
    owner.transport->seekToBeat(1.0); timeline.update();
    near(level.get(), 0.5);

    const auto lfoTrack = timeline.createTrack("LFO");
    const auto lfo = timeline.createLfoClip(lfoTrack, "", "Modulator", 0.0, 4.0);
    assert(!lfo.empty() && timeline.getClip(lfoTrack, lfo)->isLfo);
    assert(timeline.evaluateLfoTrack(lfoTrack, 0.5).active);
    const auto waveTrack = timeline.createWaveTrack("Audio");
    assert(timeline.getTrack(waveTrack)->isWaveTrack);
    timeline.addWaveTrackVolumePoint(waveTrack, 0.0, 0.0f);
    timeline.addWaveTrackVolumePoint(waveTrack, 4.0, 1.0f);
    near(timeline.evaluateWaveTrackVolume(waveTrack, 2.0), 0.5);
    timeline.setTimeSignature(7, 8); near(timeline.getBeatsPerBar(), 3.5);
    timeline.addMarker(1.0, "Entry");
    timeline.getViewState().scrollX = 123.0f;
    timeline.getViewState().pixelsPerSecond = 80.0f;
    assert(!timeline.groupClips({{track, clip}, {addTrack, addClip}}).empty());

    // Save/load preserves clip data, groups, bindings, captures and editor state.
    const auto saved = timeline.toJson();
    ofxOceanodeTimelineManager restored(&owner); restored.fromJson(saved);
    assert(restored.getTracks().size() == timeline.getTracks().size());
    assert(restored.getClipGroups().size() == 1 && restored.getMarkers().size() == 1);
    assert(restored.getTrack(track)->bindings.front().parameterPath == parameter.getEscapedName());
    assert(restored.getTrack(captureTrack)->clips.front().isStateCapture);
    assert(restored.getClip(lfoTrack, lfo)->isLfo && restored.getTrack(waveTrack)->isWaveTrack);
    near(restored.getViewState().scrollX, 123.0); near(restored.getBeatsPerBar(), 3.5);
    owner.transport->seekToBeat(0.5); restored.update(); near(value.get(), 66.0);
    assert(restored.removeTrack(addTrack)); restored.update(); near(value.get(), 64.0);
    restored.requestNoteGroupSetup(&parameter);
    restored.clear(); assert(!parameter.isTimelined() && !levelParameter.isTimelined());
    assert(restored.consumePendingNoteGroupSetup() == nullptr);
    std::cout << "Timeline regression passed: lane duplication, shared piano voices, automation, dropdown lanes, additive bindings, transport loops, state capture, LFO, wave envelope, JSON persistence, groups, markers, signature, cleanup.\n";
}
'''




def main(*, cases=CASES, extra_sources=(), extra_includes=()):
    of = of_root()
    libdir = of / "libs/openFrameworksCompiled/lib/macos"
    if not (libdir / "openFrameworks.a").is_file():
        raise RuntimeError("Set OCEANODE_TEST_OF_ROOT to a macOS OF tree with compiled libraries")
    includes = [ROOT / "src", ROOT / "src/Managers", ADDON / "src/Timeline", ADDON / "src"]
    includes += sorted({path.parent for path in (of / "libs/openFrameworks").rglob("*.h")})
    includes += [path for path in (of / "libs").glob("*/include") if path.is_dir()]
    includes += [of / "libs/freetype/include/freetype", of / "libs/cairo/include/cairo"]
    includes += list(extra_includes)
    libraries = [str(path) for path in sorted(libdir.glob("*.a"))
                 if "openframeworks" not in path.name.lower()]
    frameworks = ["Accelerate", "AppKit", "ApplicationServices", "AudioToolbox",
                  "AVFoundation", "Cocoa", "CoreAudio", "CoreFoundation", "CoreMedia",
                  "CoreServices", "CoreVideo", "IOKit", "OpenGL", "QuartzCore", "Security"]
    with tempfile.TemporaryDirectory(prefix="oceanode-timeline-") as folder:
        folder = Path(folder)
        (folder / "Managers").mkdir()
        (folder / "Managers/ofxOceanodeContainer.h").write_text(CONTAINER)
        (folder / "ofxOceanodeContainer.h").write_text('#include "Managers/ofxOceanodeContainer.h"\n')
        # The core's parameter destructor now unlinks dropdown peers out of
        # line. Include that production definition without linking unrelated
        # canvas/connection/scope services into this isolated fixture.
        parameter_source = (ROOT / "src/ofxOceanodeParameter.cpp").read_text()
        destructor_start = parameter_source.index("ofxOceanodeAbstractParameter::~ofxOceanodeAbstractParameter()")
        destructor_end = parameter_source.index("\nvoid ofxOceanodeAbstractParameter::", destructor_start)
        # Macro timelines walk real macro nodes, which this mock container has
        # none of: no macro timeline is ever played here.
        (folder / "macro_stub.cpp").write_text(
            '#include "Timeline/ofxOceanodeTimeline.h"\n'
            'std::vector<ofxOceanodeTimelineManager::MacroSource> '
            'ofxOceanodeTimelineManager::collectMacroSources() const { return {}; }\n'
            'ofxOceanodeAbstractParameter* ofxOceanodeTimelineManager::macroNodeParameter('
            'const ofxOceanodeContainer&, const std::string&) const { return nullptr; }\n')
        cpp = folder / "regression.cpp"
        cpp.write_text(cases + "\n" + parameter_source[destructor_start:destructor_end])
        binary = folder / "regression"
        # The container adapter omits NodeModel RTTI; keep the other UB checks.
        command = [os.environ.get("CXX", "clang++"), "-std=c++17", "-O0", "-g", "-w",
                   "-fsanitize=address,undefined", "-fno-sanitize=vptr", "-fno-omit-frame-pointer", "-DOF_NO_FMOD",
                   "-I" + str(folder), *["-I" + str(path) for path in includes], str(cpp),
                   *[str(path) for path in sorted((ADDON / "src/Timeline").glob("*.cpp"))
                     if path.name != "ofxOceanodeTimelineMacroSequences.cpp"], str(folder / "macro_stub.cpp"),
                   str(ADDON / "src/Controls/Editors/ofxOceanodeTimelineEditorView.cpp"),
                   str(ROOT / "src/Managers/ofxOceanodeScheduling.cpp"),
                   str(ROOT / "src/Managers/ofxOceanodeTransport.cpp"),
                   str(ROOT / "src/Nodes/Default_Nodes/Base/baseOscillator.cpp"),
                   str(of / "libs/openFrameworks/types/ofParameter.cpp"),
                   str(of / "libs/openFrameworks/types/ofParameterGroup.cpp"),
                   *[str(path) for path in extra_sources],
                   str(libdir / "openFrameworks.a"), *libraries, "-Wl,-dead_strip"]
        for framework in frameworks:
            command += ["-framework", framework]
        subprocess.run(command + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
