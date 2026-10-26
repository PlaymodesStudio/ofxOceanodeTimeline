#!/usr/bin/env python3
"""Per-frame cost of the timeline on a large session, built optimized (-O2, no
sanitizers). Not part of the regression suite: it prints timings.

    python3 tests/perf_benchmark.py

Eight polyphonic instruments (400 notes each, scheduled), a curve and a
large-valued number lane per track, and a curved tempo map.
"""
from pathlib import Path
import subprocess
import sys
base = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(base / 'tests'))
import timeline_regression as fixture

cases = fixture.CASES.split('int main()')[0] + r'''
#include <chrono>
#include <random>
#include <thread>
#include "Controls/Editors/ofxOceanodeTimelineEditorView.h"
struct Instrument {
    ofParameter<std::vector<float>> pv, gv, vv;
    ofParameter<float> cutoff, position;
    ofxOceanodeParameter<std::vector<float>> pitch, gate, velocity;
    ofxOceanodeParameter<float> cutoffHandle, positionHandle;
};
static double microseconds(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}
static void report(const char* name, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    double sum = 0; for(double s : samples) sum += s;
    std::printf("%-28s mean %8.1f us   median %8.1f us   p95 %8.1f us\n", name, sum / samples.size(),
                samples[samples.size() / 2], samples[samples.size() * 95 / 100]);
}
int main() {
    ofSeedRandom();
    std::mt19937 rng(1234);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    ofxOceanodeContainer owner;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingLookaheadAdaptive(false);
    model.setSchedulingLookaheadMs(120);
    model.setBpmAutomationEnabled(true);
    model.getBpmAutomationPoints() = {{0, 120}, {16, 150}, {32, 96}, {48, 132}, {64, 120}};
    model.getBpmCurveTensions().assign(4, {0.5f, 2.0f});
    model.setBpmInterpolation("Sigmoid");
    std::vector<std::unique_ptr<Instrument>> instruments;
    for(int t = 0; t < 8; ++t) {
        auto inst = std::make_unique<Instrument>();
        const std::string n = ofToString(t);
        inst->pv.set("Pitch " + n, std::vector<float>(8, 60), {0}, {127});
        inst->gv.set("Gate " + n, std::vector<float>(8, 0), {0}, {1});
        inst->vv.set("Velocity " + n, std::vector<float>(8, 0), {0}, {1});
        inst->cutoff.set("Cutoff " + n, 0, 0, 1);
        inst->position.set("Position " + n, 0, 0, 4000000);
        inst->pitch.bindParameter(inst->pv); inst->gate.bindParameter(inst->gv); inst->velocity.bindParameter(inst->vv);
        inst->cutoffHandle.bindParameter(inst->cutoff); inst->positionHandle.bindParameter(inst->position);
        for(ofxOceanodeAbstractParameter* p : std::initializer_list<ofxOceanodeAbstractParameter*>{
                &inst->pitch, &inst->gate, &inst->velocity, &inst->cutoffHandle, &inst->positionHandle})
            owner.targets[p->getEscapedName()] = p;
        const auto track = model.createTrack("Instrument " + n);
        const auto pb = model.addBinding(track, inst->pitch), gb = model.addBinding(track, inst->gate),
                   vb = model.addBinding(track, inst->velocity);
        const auto group = model.createNoteGroup(track, "Notes", pb, gb, vb);
        model.setNoteGroupVoices(track, group, 8);
        std::string lane;
        const auto clip = model.createNoteGroupClip(track, group, "Phrase", 0, 64, &lane);
        auto& notes = model.getLane(track, clip, lane)->pianoNotes;
        for(int i = 0; i < 400; ++i)
            notes.push_back({std::floor(uniform(0, 63.5) * 4) / 4, 0.25 * (1 + int(uniform(0, 4))),
                             int(uniform(36, 84)), float(uniform(0.3, 1.0)), 1.0f});
        const auto cb = model.addBinding(track, inst->cutoffHandle);
        const auto curve = model.createLane(track, clip, "Cutoff", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(track, clip, curve, cb);
        const auto ob = model.addBinding(track, inst->positionHandle);
        const auto numbers = model.createLane(track, clip, "Position", ofxOceanodeTimelineLaneType::Number);
        model.addBindingToLane(track, clip, numbers, ob);
        model.setClipNumber(track, clip, numbers, 0.0, 2000003.0f);
        for(auto* p : {&inst->pitch, &inst->gate, &inst->velocity})
            ofxOceanodeScheduling::registerParameterTarget(p, &model, [](const auto&) { return true; });
        instruments.push_back(std::move(inst));
    }
    owner.transport->seekToBeat(0); owner.transport->play();
    model.update();
    std::vector<double> frames;
    for(int frame = 0; frame < 300; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const auto start = std::chrono::steady_clock::now();
        model.update();
        frames.push_back(microseconds(std::chrono::steady_clock::now() - start));
    }
    report("update() while playing", frames);
    {
        ofxOceanodeTimelineEditorView view(model, 120.0f, 64.0, false, 0.0);
        std::vector<double> samples;
        for(int r = 0; r < 50; ++r) {
            const auto start = std::chrono::steady_clock::now();
            double sink = 0;
            for(int i = 0; i < 100; ++i) sink += view.pixelsToBeat(i * 37.0f);
            samples.push_back(microseconds(std::chrono::steady_clock::now() - start));
            if(sink < 0) std::printf("x");
        }
        report("100 x pixelsToBeat", samples);
    }
    {
        std::vector<double> samples;
        const auto path = instruments[0]->gate.getEscapedName();
        for(int r = 0; r < 50; ++r) {
            const auto start = std::chrono::steady_clock::now();
            model.inspectTarget(path, 10.0, true);
            samples.push_back(microseconds(std::chrono::steady_clock::now() - start));
        }
        report("inspectTarget()", samples);
    }
    std::printf("Number lane value 2000003 applied as %.1f\n", instruments[0]->position.get());
    ofxOceanodeScheduling::unregisterOwner(&model);
}
'''

real_run = subprocess.run
def optimized_run(command, **kwargs):
    if command and str(command[0]).endswith('clang++'):
        command = [arg for arg in command
                   if not str(arg).startswith('-fsanitize') and arg != '-fno-omit-frame-pointer']
        command = ['-O2' if arg == '-O0' else arg for arg in command]
        command = [arg for arg in command if arg != '-g']
    return real_run(command, **kwargs)
fixture.subprocess.run = optimized_run
fixture.main(cases=cases, extra_includes=[base / 'src'])
