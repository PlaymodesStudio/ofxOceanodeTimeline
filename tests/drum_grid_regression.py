#!/usr/bin/env python3
"""Regress the Drum Grid lane: kit rows driving an instrument's Gate/Pitch/Duration vectors.

Hits sit at beat positions; the grid only decides where new hits snap. Checks
hits and velocity, held pitch and duration, per-row lengths in beats
(polymeter), swing, grid changes leaving playback alone, dotted grids, ratchets
as separate rising edges (frame path and scheduler),
probability determinism by seed, mute/solo, JSON round trip and that older
JSON (no Drum Grid) loads and saves unchanged.
"""
import timeline_regression as fixture

cases = fixture.CASES.split('int main()')[0] + r'''
#include <set>
static int failures = 0;
static void check(bool condition, const std::string& message) {
    if(!condition) { std::cerr << "FAIL: " << message << "\n"; ++failures; }
}
static std::vector<float> values(const std::string& text) {
    std::vector<float> out;
    for(const auto& token : ofSplitString(text, ",", true, true)) out.push_back(ofToFloat(token));
    return out;
}
using DG = ofxOceanodeTimelineLaneType;

struct Kit {
    ofParameter<std::vector<float>> gateValue{"Kit Gate", {0, 0, 0, 0}, {0}, {1}};
    ofParameter<std::vector<float>> pitchValue{"Kit Pitch", {60, 60, 60, 60}, {0}, {127}};
    ofParameter<std::vector<float>> durationValue{"Kit Duration", {1, 1, 1, 1}, {0.1f}, {24}};
    ofParameter<std::vector<float>> filterValue{"Kit Filter", {0, 0, 0, 0}, {-1}, {1}};
    ofxOceanodeParameter<std::vector<float>> gate, pitch, duration, filter;
    std::string track, clip, lane, group, gateBinding, pitchBinding, durationBinding, filterBinding;
    Kit(ofxOceanodeContainer& owner, ofxOceanodeTimelineManager& model, double content = 4.0) {
        gate.bindParameter(gateValue); pitch.bindParameter(pitchValue); duration.bindParameter(durationValue);
        filter.bindParameter(filterValue);
        owner.targets[filter.getEscapedName()] = &filter;
        owner.targets[gate.getEscapedName()] = &gate;
        owner.targets[pitch.getEscapedName()] = &pitch;
        owner.targets[duration.getEscapedName()] = &duration;
        track = model.createTrack("Drums");
        pitchBinding = model.addBinding(track, pitch);
        gateBinding = model.addBinding(track, gate);
        durationBinding = model.addBinding(track, duration);
        group = model.createNoteGroup(track, "Kit", pitchBinding, gateBinding, "");
        model.setNoteGroupDuration(track, group, durationBinding);
        filterBinding = model.addBinding(track, filter);
        model.setNoteGroupFilter(track, group, filterBinding);
        clip = model.createNoteGroupClip(track, group, "Beat", 0.0, content, &lane, DG::DrumGrid);
    }
    ofxOceanodeTimelineLane& get(ofxOceanodeTimelineManager& model) { return *model.getLane(track, clip, lane); }
    std::vector<float> gates(ofxOceanodeTimelineManager& model, double beat) {
        return values(model.inspectTarget(gate.getEscapedName(), beat, true).value);
    }
    std::vector<float> pitches(ofxOceanodeTimelineManager& model, double beat) {
        return values(model.inspectTarget(pitch.getEscapedName(), beat, true).value);
    }
    std::vector<float> durations(ofxOceanodeTimelineManager& model, double beat) {
        return values(model.inspectTarget(duration.getEscapedName(), beat, true).value);
    }
    std::vector<float> filters(ofxOceanodeTimelineManager& model, double beat) {
        return values(model.inspectTarget(filter.getEscapedName(), beat, true).value);
    }
};

using Step = ofxOceanodeTimelineDrumStep;
static Step hit(double beat, double length, float gate, float velocity = 0.8f, int pitch = 0, float probability = 1.0f,
                int ratchet = 1, float duration = 1.0f, float filter = 0.0f) {
    Step s; s.beat = beat; s.length = length; s.gate = gate; s.velocity = velocity; s.pitch = pitch;
    s.probability = probability; s.ratchet = ratchet; s.duration = duration; s.filter = filter; return s;
}
// The first, index-based Drum Grid's probability roll (to prove migrated
// lanes roll exactly as before).
static bool oldRoll(float probability, int slot, int stepIndex, int64_t cycle, int seed, uint64_t epoch) {
    if(probability <= 0.0f) return false;
    if(probability >= 1.0f) return true;
    uint64_t hash = static_cast<uint64_t>(static_cast<uint32_t>(stepIndex) + 1) * 0x9e3779b97f4a7c15ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(slot) + 1) * 0x632be59bd9b4e019ULL;
    hash ^= static_cast<uint64_t>(cycle) * 0xbf58476d1ce4e5b9ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(seed)) * 0xd6e8feb86659fd93ULL;
    hash ^= epoch * 0x94d049bb133111ebULL;
    hash ^= hash >> 30; hash *= 0xbf58476d1ce4e5b9ULL; hash ^= hash >> 27; hash *= 0x94d049bb133111ebULL; hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f < probability;
}
static bool openAt(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row, double beat,
                   int64_t repeat = 0, double content = 4.0, double gap = 0.0, uint64_t epoch = 0, float* level = nullptr) {
    float g = 0; int p = 0;
    const bool open = ofxOceanodeTimelineDrumGrid::rowStateAt(lane, row, beat, repeat, content, gap, epoch, g, p);
    if(level) *level = g;
    return open;
}

int main() {
    ofSeedRandom();
    {   // Creation from a note group, evaluation, polymeter in beats, swing, mute/solo.
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Kit kit(owner, model);
        auto& lane = kit.get(model);
        check(lane.type == DG::DrumGrid, "Note group clip is not a Drum Grid");
        check(lane.pianoGateBindingId == kit.gateBinding && lane.pianoPitchBindingId == kit.pitchBinding,
              "Drum Grid did not take the group's gate/pitch roles");
        check(lane.drumRows.size() == 4 && lane.drumRows[2].slot == 2 && lane.drumRows[2].name == "Row 3",
              "Default rows: 4 rows on slots 0..3 named Row n");
        check(lane.drumRows[0].length == 4.0 && lane.drumRows[0].steps.empty(), "Default row: as long as the clip (4 beats), no hits");
        check(lane.drumStepFollow && lane.drumSwingBeats == 0.25, "New lanes follow the master grid; swing on 16ths");
        check(lane.drumDurationBindingId == kit.durationBinding, "Drum Grid takes the group's Duration role");
        check(lane.drumFilterBindingId == kit.filterBinding, "Drum Grid takes the group's Filter role");
        lane.drumRows.resize(3);
        // Row 0: kick at 0 (vel 0.5, +2 st, x0.5) and at beat 2 (-3 st).
        lane.drumRows[0].steps = {hit(0.0, 0.25, 0.5f, 0.5f, 2, 1.0f, 1, 0.5f, -0.4f), hit(2.0, 0.25, 0.5f, 1.0f, -3, 1.0f, 1, 1.0f, 0.25f)};
        // Row 1: three 16ths long (0.75 beats), a hit at 0 -> every 0.75 beats.
        lane.drumRows[1].length = 0.75;
        lane.drumRows[1].steps = {hit(0.0, 0.25, 0.5f, 0.8f, 0, 1.0f, 1, 4.0f)};

        auto g = kit.gates(model, 0.05);
        check(g.size() == 4, "Gate vector keeps the parameter's width (4), got " + ofToString(g.size()));
        check(std::abs(g[0] - 0.5f) < 1e-4f, "A hit plays at its velocity");
        check(g[1] > 0.0f && g[2] == 0.0f && g[3] == 0.0f, "Only rows with a hit open");
        auto p = kit.pitches(model, 0.05);
        check(p.size() == 4 && p[0] == 62.0f && p[1] == 60.0f && p[2] == 60.0f, "Pitch is 60 + offset; untouched rows 60");
        g = kit.gates(model, 0.2);
        p = kit.pitches(model, 0.2);
        check(g[0] == 0.0f, "Gate closes after length x gate (0.125)");
        check(p[0] == 62.0f, "Pitch is held after the gate closes");
        g = kit.gates(model, 2.05);
        p = kit.pitches(model, 2.05);
        check(g[0] == 1.0f && p[0] == 57.0f, "Second hit: velocity 1, pitch 57");
        check(kit.pitches(model, 3.9)[0] == 57.0f, "Pitch holds through the rest of the bar");
        auto d = kit.durations(model, 0.05);
        check(d.size() == 4 && std::abs(d[0] - 0.5f) < 1e-6f && d[1] == 4.0f && d[2] == 1.0f && d[3] == 1.0f,
              "Duration component = the hit's value; rows without hits 1");
        check(std::abs(kit.durations(model, 0.2)[0] - 0.5f) < 1e-6f, "Duration is held after the gate closes");
        check(kit.durations(model, 2.05)[0] == 1.0f, "The next hit brings its own duration");
        // Filter: the hit's value (LP 40 % here), held after the gate; rows never played 0.
        auto f = kit.filters(model, 0.05);
        check(f.size() == 4 && std::abs(f[0] + 0.4f) < 1e-6f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 0.0f,
              "Filter component = the hit's value; rows without hits 0 (open)");
        check(std::abs(kit.filters(model, 0.2)[0] + 0.4f) < 1e-6f, "Filter is held after the gate closes");
        check(std::abs(kit.filters(model, 2.05)[0] - 0.25f) < 1e-6f, "The next hit brings its own filter (HP 25 %)");
        // Polymeter: row 1 repeats every 0.75 beats.
        for(double beat : {0.0, 0.75, 1.5, 2.25, 3.0}) check(kit.gates(model, beat + 0.01)[1] > 0.0f, "0.75-beat row hits at " + ofToString(beat));
        for(double beat : {0.26, 0.51, 1.01}) check(kit.gates(model, beat)[1] == 0.0f, "0.75-beat row is silent at " + ofToString(beat));
        // Swing delays hits on odd 16ths; a hit off the 16th grid is not swung.
        lane.drumRows[2].steps = {hit(0.25, 0.25, 0.5f), hit(1.3, 0.1, 0.5f)};
        check(kit.gates(model, 0.26)[2] > 0.0f, "Unswung odd 16th opens on the grid");
        lane.drumSwing = 0.5f;
        check(kit.gates(model, 0.26)[2] == 0.0f && kit.gates(model, 0.38)[2] > 0.0f, "Swing delays odd 16ths by half a 16th");
        check(kit.gates(model, 1.31)[2] > 0.0f, "Hits off the swing grid are not swung");
        lane.drumSwing = 0.0f;
        // Mute and solo.
        lane.drumRows[0].mute = true;
        check(kit.gates(model, 0.05)[0] == 0.0f, "A muted row stays closed");
        lane.drumRows[0].mute = false;
        lane.drumRows[1].solo = true;
        g = kit.gates(model, 0.05);
        check(g[0] == 0.0f && g[1] > 0.0f, "Solo silences the other rows");
        lane.drumRows[1].solo = false;
        lane.muted = true;
        g = kit.gates(model, 0.05);
        check(g.empty() || g[0] == 0.0f, "A muted lane plays nothing");
        lane.muted = false;
        // A piano roll of the same group does not drive Duration.
        std::string pianoLane;
        const auto pianoClip = model.createNoteGroupClip(kit.track, kit.group, "Keys", 8.0, 4.0, &pianoLane);
        const auto* piano = model.getLane(kit.track, pianoClip, pianoLane);
        check(piano->drumDurationBindingId.empty() &&
              std::find(piano->bindingIds.begin(), piano->bindingIds.end(), kit.durationBinding) == piano->bindingIds.end(),
              "Piano lanes leave Duration alone");
        model.removeClip(kit.track, pianoClip);
        // The frame path writes the vectors.
        owner.transport->seekToBeat(0.05);
        owner.transport->play();
        model.update();
        check(kit.gateValue.get().size() == 4 && std::abs(kit.gateValue.get()[0] - 0.5f) < 1e-4f &&
              kit.pitchValue.get()[0] == 62.0f, "update() writes Gate/Pitch vectors");
        check(kit.durationValue.get().size() == 4 && std::abs(kit.durationValue.get()[0] - 0.5f) < 1e-6f &&
              kit.durationValue.get()[1] == 4.0f, "update() writes the Duration vector");
        check(kit.filterValue.get().size() == 4 && std::abs(kit.filterValue.get()[0] + 0.4f) < 1e-6f,
              "update() writes the Filter vector");
        owner.transport->pause();

        // The grid is editing only: 1/32 hits stay put when it becomes 1/16.
        model.setMasterGridBeats(0.125);
        check(model.drumStepBeats(lane) == 0.125, "A following lane edits on the master grid");
        lane.drumRows[2].steps.clear();
        ofxOceanodeTimelineDrumGrid::addStep(lane.drumRows[2], hit(0.125, 0.125, 0.5f)); // a 1/32 off the 16ths
        ofxOceanodeTimelineDrumGrid::addStep(lane.drumRows[2], hit(0.375, 0.125, 0.5f));
        std::vector<float> before;
        for(double beat = 0.0; beat < 4.0; beat += 1.0 / 64.0) before.push_back(kit.gates(model, beat)[2]);
        model.setMasterGridBeats(0.25);
        check(model.drumStepBeats(lane) == 0.25, "...and follows a change of it");
        std::vector<float> after;
        for(double beat = 0.0; beat < 4.0; beat += 1.0 / 64.0) after.push_back(kit.gates(model, beat)[2]);
        check(before == after, "Changing the grid changes no playback");
        check(lane.drumRows[2].steps[0].beat == 0.125 && lane.drumRows[2].steps[1].beat == 0.375, "...nor any position");
        lane.drumStepFollow = false;
        lane.beatsPerStep = 1.0 / 6.0;
        check(std::abs(model.drumStepBeats(lane) - 1.0 / 6.0) < 1e-12, "A fixed lane keeps its own grid (16th triplets)");
        model.setMasterGridBeats(0.5);
        check(std::abs(model.drumStepBeats(lane) - 1.0 / 6.0) < 1e-12, "...whatever the master");
        lane.drumStepFollow = true;
        model.setMasterGridBeats(0.0);
        check(model.drumStepBeats(lane) == 0.25, "Master None: a following lane edits in 16ths");
        // Dotted 8ths: hits placed on a 0.75-beat grid land at 0, 0.75, 1.5, 2.25.
        model.setMasterGridBeats(0.75);
        auto& dotted = lane.drumRows[2];
        dotted.steps.clear();
        dotted.length = 3.0;
        for(int k = 0; k < 4; ++k) ofxOceanodeTimelineDrumGrid::addStep(dotted, hit(k * model.drumStepBeats(lane), model.drumStepBeats(lane), 0.5f));
        for(double beat : {0.0, 0.75, 1.5, 2.25, 3.0}) check(kit.gates(model, beat + 0.01)[2] > 0.0f, "Dotted 8th hit at " + ofToString(beat));
        for(double beat : {0.5, 1.2, 1.9}) check(kit.gates(model, beat)[2] == 0.0f, "Dotted 8th gap at " + ofToString(beat));
        // The master grid is saved with the timeline (missing: 1/16).
        const auto json = model.toJson();
        check(json["masterGridBeats"] == 0.75, "Master grid saved");
        auto noGrid = json; noGrid.erase("masterGridBeats");
        ofxOceanodeTimelineManager loaded(&owner);
        loaded.fromJson(json);
        check(loaded.getMasterGridBeats() == 0.75, "Master grid loads");
        loaded.fromJson(noGrid);
        check(loaded.getMasterGridBeats() == 0.25, "Older files: 1/16");
        ofxOceanodeScheduling::unregisterOwner(&loaded);
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {   // Shortening a row hides hits, never deletes them: 16 -> 3 -> 16 steps.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Kit kit(owner, model);
        auto& row = kit.get(model).drumRows[0];
        for(int i = 0; i < 16; ++i) ofxOceanodeTimelineDrumGrid::addStep(row, hit(i * 0.25, 0.25, 0.5f, 0.3f + i * 0.04f, i - 8));
        auto sample = [&]() {
            std::vector<float> out;
            for(double beat = 0.0; beat < 4.0; beat += 1.0 / 32.0) {
                out.push_back(kit.gates(model, beat + 1e-7)[0]);
                out.push_back(kit.pitches(model, beat + 1e-7)[0]);
            }
            return out;
        };
        const auto full = sample();
        kit.get(model).drumRows[0].length = 0.75;
        ofxOceanodeTimelineDrumGrid::normalizeRow(kit.get(model).drumRows[0]);
        check(kit.get(model).drumRows[0].steps.size() == 16, "Shortening keeps every hit");
        // At 3 steps only the first three play, cycling every 0.75 beats.
        for(double beat : {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 3.0}) check(kit.gates(model, beat + 0.01)[0] > 0.0f, "3-step row hits at " + ofToString(beat));
        check(std::abs(kit.gates(model, 0.76)[0] - 0.3f) < 1e-4f && kit.pitches(model, 0.76)[0] == 52.0f, "...its first hit again at 0.75");
        check(std::abs(kit.gates(model, 0.51)[0] - 0.38f) < 1e-4f, "...the third hit at 0.5, nothing of the hidden ones");
        // Hidden hits survive save and load.
        const auto json = model.toJson();
        ofxOceanodeTimelineManager loaded(&owner);
        loaded.fromJson(json);
        const auto* reloaded = loaded.getLane(kit.track, kit.clip, kit.lane);
        check(reloaded != nullptr && reloaded->drumRows[0].steps.size() == 16 && reloaded->drumRows[0].length == 0.75,
              "Hidden hits survive save/load");
        kit.get(model).drumRows[0].length = 4.0;
        check(sample() == full, "Back to 16 steps: the same hits, identical playback");
        ofxOceanodeScheduling::unregisterOwner(&loaded);
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {   // Ratchets and back-to-back hits always give fresh rising edges.
        ofxOceanodeTimelineLane lane;
        lane.type = DG::DrumGrid;
        lane.drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(1, {"Hat"}, 1.0);
        auto& row = lane.drumRows[0];
        row.steps = {hit(0.0, 0.25, 0.5f, 0.7f, 0, 1.0f, 4)};
        std::vector<ofxOceanodeTimelineDrumHit> hits;
        const double gap = 1.0 / 160.0;
        ofxOceanodeTimelineDrumGrid::rowHits(lane, row, 0.0, 0.249, 0, 4.0, gap, 0, hits);
        check(hits.size() == 4, "Ratchet 4 divides the hit's length into 4 hits, got " + ofToString(hits.size()));
        for(size_t i = 0; i < hits.size(); ++i) {
            check(std::abs(hits[i].start - 0.0625 * i) < 1e-9 && hits[i].open < hits[i].close, "Each sub-hit opens on its place");
            if(i > 0) check(hits[i - 1].close < hits[i].open, "Sub-hits are separate rising edges");
        }
        // Full gate: sub-hits touch, so each one after the first opens after the gap.
        row.steps[0].gate = 1.0f;
        hits.clear();
        ofxOceanodeTimelineDrumGrid::rowHits(lane, row, 0.0, 0.249, 0, 4.0, gap, 0, hits);
        check(hits.size() == 4, "Full-gate ratchet still 4 hits");
        for(size_t i = 1; i < hits.size(); ++i) {
            check(std::abs(hits[i].open - (hits[i].start + gap)) < 1e-9, "Touching sub-hit opens after the gap");
            float level = 1.0f;
            check(!openAt(lane, row, hits[i].start + gap * 0.5, 0, 4.0, gap), "The gate is low inside the retrigger gap");
            check(openAt(lane, row, hits[i].start + gap * 1.5, 0, 4.0, gap, 0, &level) && std::abs(level - 0.7f) < 1e-6f,
                  "...and high again after it");
        }
        // Back-to-back full-length hits: between hits, across the pattern wrap and a clip repeat.
        row.steps.clear();
        for(int k = 0; k < 4; ++k) row.steps.push_back(hit(k * 0.25, 0.25, 1.0f));
        check(!openAt(lane, row, 0.25 + gap * 0.5, 0, 4.0, gap), "Back-to-back hits re-open after a gap");
        check(!openAt(lane, row, 1.0 + gap * 0.5, 0, 4.0, gap), "The pattern wrap re-opens after a gap");
        check(!openAt(lane, row, gap * 0.5, 1, 4.0, gap), "A clip repeat re-opens after a gap");
        check(openAt(lane, row, gap * 0.5, 0, 4.0, gap), "The very first hit opens at once");
    }
    {   // Probability: deterministic by seed (and epoch), re-rolled per pattern cycle.
        ofxOceanodeTimelineLane lane;
        lane.type = DG::DrumGrid;
        lane.drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(1, {}, 1.0);
        auto& row = lane.drumRows[0];
        row.steps = {hit(0.0, 0.25, 0.5f, 1.0f, 0, 0.5f)};
        auto roll = [&](int seed, uint64_t epoch) {
            lane.probabilitySeed = seed;
            std::vector<bool> out;
            for(int cycle = 0; cycle < 64; ++cycle) out.push_back(openAt(lane, row, cycle + 0.01, 0, 64.0, 0.0, epoch));
            return out;
        };
        const auto a = roll(1234, 7), b = roll(1234, 7), c = roll(999, 7), d = roll(1234, 8);
        check(a == b, "Same seed and start: same rolls");
        check(a != c, "Another seed: another pattern");
        check(a != d, "Another start (epoch): rolled again");
        const int played = static_cast<int>(std::count(a.begin(), a.end(), true));
        check(played > 16 && played < 48, "50% plays about half of 64 cycles, got " + ofToString(played));
        lane.probabilityEnabled = false;
        const auto always = roll(1234, 7);
        check(std::count(always.begin(), always.end(), true) == 64, "Probability off: every hit plays");
        lane.probabilityEnabled = true;
        lane.probabilitySeed = 1234;
        int repeated = 0;
        for(int repeat = 0; repeat < 32; ++repeat) repeated += openAt(lane, row, 0.01, repeat, 1.0, 0.0, 7);
        check(repeated > 0 && repeated < 32, "Clip repeats re-roll probability");
    }
    {   // Scheduler: ratchet sub-hits are queued as separate gate-ons with the
        // velocity; Duration leads its gate like the pitch.
        ofxOceanodeContainer owner;
        owner.transport->setDriverMode(TransportDriverMode::External);
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingLookaheadAdaptive(false); // the default 120 ms window
        Kit kit(owner, model);
        auto& lane = kit.get(model);
        // Two sub-hits at 0.125 and 0.1875 beats; a later x1 hit so the row starts at x1.
        lane.drumRows[0].steps = {hit(0.125, 0.125, 0.5f, 0.6f, 0, 1.0f, 2, 2.0f, 0.5f), hit(1.5, 0.125, 0.5f, 0.6f)};
        std::vector<ofxOceanodeScheduledParameterEvent> events, durationEvents, filterEvents;
        ofxOceanodeScheduling::registerParameterTarget(&kit.filter, &model,
            [&](const auto& event) { filterEvents.push_back(event); return true; });
        ofxOceanodeScheduling::registerParameterTarget(&kit.gate, &model,
            [&](const auto& event) { events.push_back(event); return true; });
        ofxOceanodeScheduling::registerParameterTarget(&kit.duration, &model,
            [&](const auto& event) { durationEvents.push_back(event); return true; });
        owner.transport->play();
        model.update();
        std::vector<float> first;
        for(const auto& event : events) if(!event.isCorrection) first.push_back(values(event.value)[0]);
        int risingEdges = 0;
        float previous = 0.0f;
        for(float v : first) { if(previous == 0.0f && v > 0.0f) { ++risingEdges; check(std::abs(v - 0.6f) < 1e-4f, "Scheduled gate carries the velocity"); } previous = v; }
        check(risingEdges == 2, "Two ratchet sub-hits scheduled as two rising edges, got " + ofToString(risingEdges));
        for(size_t i = 1; i < events.size(); ++i) check(events[i].dueSteadyTimeUs > events[i - 1].dueSteadyTimeUs, "Events in time order");
        uint64_t firstGateOn = 0, durationDue = 0;
        for(const auto& event : events) if(!event.isCorrection && values(event.value)[0] > 0.0f) { firstGateOn = event.dueSteadyTimeUs; break; }
        for(const auto& event : durationEvents) if(!event.isCorrection && values(event.value)[0] == 2.0f) { durationDue = event.dueSteadyTimeUs; break; }
        check(firstGateOn > 0 && durationDue > 0, "Gate and Duration were both scheduled");
        check(durationDue + 1500 <= firstGateOn && durationDue + 2500 >= firstGateOn,
              "Duration leads its gate by the pitch lead: " + ofToString(static_cast<long long>(firstGateOn) - static_cast<long long>(durationDue)) + " us");
        uint64_t filterDue = 0;
        for(const auto& event : filterEvents) if(!event.isCorrection && values(event.value)[0] == 0.5f) { filterDue = event.dueSteadyTimeUs; break; }
        check(filterDue > 0 && filterDue + 1500 <= firstGateOn && filterDue + 2500 >= firstGateOn,
              "Filter is scheduled with the pitch lead too");
        owner.transport->pause();
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {   // Files of the first, index-based Drum Grid play exactly as before.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Kit kit(owner, model);
        auto json = model.toJson();
        auto& laneJson = json["tracks"][0]["clips"][0]["lanes"][0];
        laneJson["beatsPerStep"] = 0.25;
        ofJson steps = ofJson::array();
        for(int i = 0; i < 20; ++i) steps.push_back({{"on", false}});
        steps[0] = {{"on", true}, {"velocity", 0.9}, {"pitch", 3}, {"probability", 1.0}, {"ratchet", 1}, {"nudge", 0.0}};
        steps[1] = {{"on", true}, {"velocity", 0.7}};                         // odd: swung
        steps[4] = {{"on", true}, {"nudge", 0.2}};                            // nudged
        steps[6] = {{"on", true}, {"probability", 0.5}};                      // rolls as before
        steps[9] = {{"on", true}, {"ratchet", 2}};
        steps[18] = {{"on", true}};                                           // past the length: never played
        laneJson["drumGrid"] = {{"swing", 0.5}, {"rows", {{{"slot", 0}, {"name", "Kick"}, {"length", 12}, {"gate", 0.5},
                                                           {"steps", steps}}}}};
        ofxOceanodeTimelineManager loaded(&owner);
        loaded.fromJson(json);
        const auto* lane = loaded.getLane(kit.track, kit.clip, kit.lane);
        check(lane != nullptr && lane->drumRows.size() == 1, "Old Drum Grid lane loads");
        if(lane != nullptr && lane->drumRows.size() == 1) {
            const auto& row = lane->drumRows[0];
            check(lane->drumStepFollow && lane->drumSwingBeats == 0.25,
                  "Old lanes follow the master grid for editing and keep swinging on their old step");
            check(row.length == 3.0 && row.steps.size() == 6, "12 steps of 1/16 = 3 beats; 5 playing hits and the step past "
                  "the length kept hidden (got " + ofToString(row.steps.size()) + ")");
            check(row.steps.back().beat == 4.5 && ofxOceanodeTimelineDrumGrid::stepHidden(row, row.steps.back()),
                  "The step past the length is a hidden hit at its old place");
            // Gate pattern of the old evaluation, sampled every 1/64 beat over two clip repeats.
            auto oldOpen = [&](double beat, int64_t repeat) {
                const double cell = 0.25, rowLength = 3.0;
                const int64_t pattern = static_cast<int64_t>(std::floor(beat / rowLength));
                const double pos = beat - pattern * rowLength;
                const int64_t cycle = repeat * 2 + pattern; // 4-beat content, 3-beat row: 2 patterns per repeat
                struct Old { int i; double nudge; float probability; int ratchet; };
                for(const Old o : {Old{0, 0.0, 1.0f, 1}, Old{1, 0.0, 1.0f, 1}, Old{4, 0.2, 1.0f, 1}, Old{6, 0.0, 0.5f, 1}, Old{9, 0.0, 1.0f, 2}}) {
                    const double offset = ofClamp((o.i & 1 ? 0.5 : 0.0) + o.nudge, -0.5, 0.95);
                    const double start = (o.i + offset) * cell;
                    const double sub = cell / o.ratchet;
                    for(int r = 0; r < o.ratchet; ++r) {
                        const double s = start + r * sub;
                        if(pattern * rowLength + s >= 4.0) continue;
                        if(pos >= s && pos < s + sub * 0.5 && oldRoll(o.probability, 0, o.i, cycle, lane->probabilitySeed, 3)) return true;
                    }
                }
                return false;
            };
            // Following the master can't change what plays: try a few masters.
            loaded.setMasterGridBeats(0.125);
            check(loaded.drumStepBeats(*lane) == 0.125, "A migrated lane edits on the master grid (1/32)");
            int mismatches = 0;
            for(int64_t repeat = 0; repeat < 2; ++repeat)
                for(double beat = 0.0; beat < 4.0; beat += 1.0 / 64.0)
                    if(openAt(*lane, row, beat + 1e-7, repeat, 4.0, 0.0, 3) != oldOpen(beat + 1e-7, repeat)) ++mismatches;
            loaded.setMasterGridBeats(0.75);
            for(int64_t repeat = 0; repeat < 2; ++repeat)
                for(double beat = 0.0; beat < 4.0; beat += 1.0 / 64.0)
                    if(openAt(*lane, row, beat + 1e-7, repeat, 4.0, 0.0, 3) != oldOpen(beat + 1e-7, repeat)) ++mismatches;
            check(mismatches == 0, "Migrated lane plays exactly as the old one, whatever the master: " + ofToString(mismatches) + " differences");
        }
        const auto saved = loaded.toJson();
        const auto& grid = saved["tracks"][0]["clips"][0]["lanes"][0]["drumGrid"];
        check(grid["format"] == 2 && grid["rows"][0].contains("hits") && !grid["rows"][0].contains("steps"),
              "Saved in the new shape (format 2, hits)");
        ofxOceanodeScheduling::unregisterOwner(&loaded);
        ofxOceanodeScheduling::unregisterOwner(&model);
    }
    {   // JSON: round trip, old files unchanged, malformed / unknown data defaults.
        ofxOceanodeContainer owner;
        ofxOceanodeTimelineManager model(&owner);
        model.setSchedulingEnabled(false);
        Kit kit(owner, model);
        auto& lane = kit.get(model);
        lane.drumSwing = 0.25f;
        lane.drumSwingBeats = 0.5;
        lane.drumStepFollow = false;
        lane.beatsPerStep = 0.375;
        lane.drumRows[1].name = "Snare";
        lane.drumRows[1].length = 1.25;
        lane.drumRows[1].gate = 0.3f;
        lane.drumRows[1].mute = true;
        lane.drumRows[2].solo = true;
        lane.drumRows[1].steps = {hit(0.1875, 0.0625, 0.4f, 0.4f, -7, 0.25f, 3, 16.0f, -0.75f)};
        const auto json = model.toJson();
        ofxOceanodeTimelineManager loaded(&owner);
        loaded.fromJson(json);
        const auto* back = loaded.getLane(kit.track, kit.clip, kit.lane);
        check(back != nullptr && back->type == DG::DrumGrid, "Drum Grid lane type survives JSON");
        if(back != nullptr) {
            check(std::abs(back->drumSwing - 0.25f) < 1e-6f && back->drumSwingBeats == 0.5 && !back->drumStepFollow &&
                  back->beatsPerStep == 0.375 && back->drumRows.size() == 4, "Swing, grid and rows survive");
            const auto& row = back->drumRows[1];
            const auto& step = row.steps[0];
            check(row.name == "Snare" && row.length == 1.25 && std::abs(row.gate - 0.3f) < 1e-6f && row.mute && !row.solo &&
                  back->drumRows[2].solo, "Row fields survive");
            check(step.beat == 0.1875 && step.length == 0.0625 && std::abs(step.gate - 0.4f) < 1e-6f &&
                  std::abs(step.velocity - 0.4f) < 1e-6f && step.pitch == -7 &&
                  std::abs(step.probability - 0.25f) < 1e-6f && step.ratchet == 3 && step.duration == 16.0f &&
                  step.filter == -0.75f, "Hit fields survive");
            check(back->drumFilterBindingId == kit.filterBinding &&
                  loaded.getNoteGroup(kit.track, kit.group)->filterBindingId == kit.filterBinding, "Filter role survives");
            check(back->drumDurationBindingId == kit.durationBinding, "Duration role survives");
            check(loaded.getNoteGroup(kit.track, kit.group) != nullptr &&
                  loaded.getNoteGroup(kit.track, kit.group)->durationBindingId == kit.durationBinding, "Group Duration survives");
            check(back->pianoGateBindingId == kit.gateBinding && back->pianoPitchBindingId == kit.pitchBinding, "Roles survive");
        }
        check(loaded.toJson() == json, "Save after load is identical");
        check(json.dump().find("\"laneType\":\"DrumGrid\"") != std::string::npos, "Saved as laneType DrumGrid");
        // Out of range / malformed values clamp to defaults.
        auto edited = json;
        for(auto& track : edited["tracks"]) for(auto& clip : track["clips"]) for(auto& laneJson : clip["lanes"]) {
            if(!laneJson.contains("drumGrid")) continue;
            laneJson["drumGrid"]["swing"] = 9.0;
            laneJson["drumGrid"]["rows"][0]["length"] = 500;
            laneJson["drumGrid"]["rows"][0]["gate"] = "loud";
            laneJson["drumGrid"]["rows"][0]["hits"] = {{{"beat", 1.0}, {"ratchet", 99}, {"pitch", 80}},
                                                      {{"beat", 2.0}, {"duration", 99.0}, {"filter", 5.0}},
                                                      {{"beat", 3.0}, {"duration", -1.0}, {"length", "long"}, {"filter", -9.0}}};
            laneJson["drumGrid"]["rows"][1]["hits"] = "none";
        }
        ofxOceanodeTimelineManager clamped(&owner);
        clamped.fromJson(edited);
        if(const auto* c = clamped.getLane(kit.track, kit.clip, kit.lane)) {
            check(c->drumSwing == 0.75f && c->drumRows[0].length == 64.0 && c->drumRows[0].gate == 0.5f, "Lane/row values clamp");
            const auto& step = c->drumRows[0].steps[0];
            check(step.ratchet == 8 && step.pitch == 24 && std::abs(step.velocity - 0.8f) < 1e-6f &&
                  step.probability == 1.0f && step.duration == 1.0f && step.length == 0.25, "Hit values clamp; missing ones default");
            check(c->drumRows[0].steps[1].duration == 24.0f && std::abs(c->drumRows[0].steps[2].duration - 0.1f) < 1e-6f &&
                  c->drumRows[0].steps[2].length == 0.25, "Duration clamps to 0.1..24");
            check(c->drumRows[0].steps[0].filter == 0.0f && c->drumRows[0].steps[1].filter == 1.0f &&
                  c->drumRows[0].steps[2].filter == -1.0f, "Filter: missing 0, clamps to -1..1");
            check(c->drumRows[1].steps.empty(), "Malformed hits load empty");
        } else check(false, "Clamped lane missing");
        // Older files: no Drum Grid anywhere, so nothing new is written.
        ofxOceanodeTimelineManager old(&owner);
        const auto t = old.createTrack("Old");
        const auto c = old.createClip(t, "Old", 0, 4);
        old.createLane(t, c, "Steps", DG::Step);
        old.createLane(t, c, "Piano", DG::PianoRoll);
        const auto oldJson = old.toJson();
        check(oldJson.dump().find("drumGrid") == std::string::npos, "Other lanes write no drumGrid data");
        ofxOceanodeTimelineManager reloaded(&owner);
        reloaded.fromJson(oldJson);
        check(reloaded.toJson() == oldJson, "Old JSON loads and saves unchanged");
        auto stripped = json;
        for(auto& track : stripped["tracks"]) for(auto& clip : track["clips"]) for(auto& laneJson : clip["lanes"]) laneJson.erase("drumGrid");
        ofxOceanodeTimelineManager bare(&owner);
        bare.fromJson(stripped);
        if(const auto* l = bare.getLane(kit.track, kit.clip, kit.lane))
            check(l->type == DG::DrumGrid && l->drumRows.empty() &&
                  bare.inspectTarget(kit.gate.getEscapedName(), 0.1, true).value.find_first_not_of("0, ") == std::string::npos,
                  "Drum Grid without rows loads and plays nothing");
        auto unknown = oldJson;
        unknown["tracks"][0]["clips"][0]["lanes"][0]["laneType"] = "SomethingNew";
        ofxOceanodeTimelineManager future(&owner);
        future.fromJson(unknown);
        check(future.getTracks()[0].clips[0].lanes[0].type == DG::Step, "Unknown lane types load as Step");
        // Splitting a clip keeps each row's phase.
        auto& splitLane = kit.get(model);
        for(auto& row : splitLane.drumRows) { row.steps.clear(); row.solo = row.mute = false; row.length = 4.0; }
        splitLane.drumSwing = 0.0f;
        splitLane.drumRows[1].steps = {hit(1.5, 0.25, 0.5f)};
        model.getClip(kit.track, kit.clip)->repeatContent = false;
        const auto right = model.splitClip(kit.track, kit.clip, 1.0);
        check(!right.empty(), "Drum clip splits");
        if(const auto* rightClip = model.getClip(kit.track, right))
            check(rightClip->lanes[0].drumRows[1].steps.size() == 1 && rightClip->lanes[0].drumRows[1].steps[0].beat == 0.5,
                  "The right part continues the row from the cut");
        ofxOceanodeScheduling::unregisterOwner(&model);
        for(auto* m : {&loaded, &clamped, &old, &reloaded, &bare, &future}) ofxOceanodeScheduling::unregisterOwner(m);
    }
    if(failures > 0) return 1;
    std::cout << "Drum Grid regression passed: hits at beat positions, velocity, held pitch and duration, polymeter in beats, "
                 "swing, grid changes keep playback, dotted grid, mute/solo, ratchet edges (frame and schedule), "
                 "probability by seed, old index-based JSON plays identically, JSON round trip, old JSON unchanged.\n";
}

'''

fixture.main(cases=cases)
