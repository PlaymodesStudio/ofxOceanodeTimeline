// Evaluation: what every binding holds at a beat (collectActiveValues), the
// beats at which it can change (collectChangeBeats), piano voicing and
// mutation, the evaluation indexes and the play context rolls.

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "Managers/PresetSaveCleanup.h"
#include "ofxOceanodeParameter.h"
#include "Nodes/ofxOceanodeNodeModel.h"
#include "Nodes/Default_Nodes/Base/baseOscillator.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <locale.h>
#include <cstring>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <type_traits>
#include "Timeline/ofxOceanodeTimelineInternal.h"

using namespace ofxOceanodeTimelineDetail;

namespace {

bool clipSourceBeat(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelinePlayContext& context,
                    double globalBeat, double& sourceBeat) {
    if(clip.muted || ofxOceanodeTimelineClipChanceSilenced(clip, context)) return false;
    if(!clipCoversBeat(clip, globalBeat)) return false;

    sourceBeat = ofxOceanodeTimelineClipTime::timelineToSourceBeat(clip, globalBeat);
    return true;
}

std::string mapNormalizedLaneValue(const ofxOceanodeTimelineLane& lane,
                                   const ofxOceanodeTimelineParameterBinding& binding,
                                   const std::string& normalizedValue) {
    const bool vectorFloat = binding.valueType == typeid(std::vector<float>).name();
    const bool vectorInt = binding.valueType == typeid(std::vector<int>).name();
    // a gain on a decibel axis
    if((binding.valueType == typeid(float).name() || vectorFloat) &&
       ofxOceanodeTimelineScale::isDecibels(lane, &binding)) {
        std::string result;
        automationText::forEachToken(normalizedValue, [&](const char* begin, const char* end) {
            if(!result.empty()) result += ",";
            automationText::appendFloat(result, ofxOceanodeTimelineScale::normalizedToGain(
                lane, static_cast<float>(automationText::parse(begin, end))));
        });
        return result.empty() ? normalizedValue : result;
    }
    if(lane.type == ofxOceanodeTimelineLaneType::Curve && (vectorFloat || vectorInt)) {
        std::string result;
        bool first = true;
        automationText::forEachToken(normalizedValue, [&](const char* begin, const char* end) {
            const float normalized = static_cast<float>(automationText::parse(begin, end));
            float mapped = lane.valueMin + normalized * (lane.valueMax - lane.valueMin);
            if(lane.curveClamp) {
                mapped = ofClamp(mapped, std::min(lane.valueMin, lane.valueMax),
                                 std::max(lane.valueMin, lane.valueMax));
            }
            if(!first) result += ",";
            first = false;
            if(vectorInt) automationText::appendInt(result, std::lround(mapped));
            else automationText::appendFloat(result, mapped);
        });
        return result.empty() ? normalizedValue : result;
    }

    const float normalized = static_cast<float>(automationText::parse(normalizedValue));
    float mapped = lane.valueMin + normalized * (lane.valueMax - lane.valueMin);
    if(lane.type == ofxOceanodeTimelineLaneType::Curve && lane.curveClamp) {
        mapped = ofClamp(mapped, std::min(lane.valueMin, lane.valueMax),
                         std::max(lane.valueMin, lane.valueMax));
    }
    if(binding.valueType == typeid(float).name()) return automationText::formatFloat(mapped);
    if(binding.valueType == typeid(int).name()) return automationText::formatInt(std::lround(mapped));
    if(binding.valueType == typeid(bool).name()) return normalized >= 0.5f ? "1" : "0";
    return normalizedValue;
}

// Clamps a combined automation value back to the target parameter's own
// min/max. Needed because Add/Multiply/Min/Max blending has no natural
// ceiling/floor of its own the way a single Replace binding does -- several
// bindings driving one parameter can sum or multiply past its range even
// though every individual contributor stayed within its lane's own bounds.
// Text and bool values have no meaningful notion of "range" here and pass
// through unchanged.
std::string clampValueToParameterRange(ofxOceanodeAbstractParameter& parameter,
                                       const std::string& value,
                                       const std::string& valueType) {
    auto clampComponent = [](double v, double lo, double hi) {
        if(lo > hi) std::swap(lo, hi); // a misconfigured/inverted range shouldn't force everything to one value
        return ofClamp(v, lo, hi);
    };
    if(valueType == typeid(float).name()) {
        auto& target = parameter.cast<float>().getParameter();
        const float raw = static_cast<float>(automationText::parse(value));
        const float clamped = clampComponent(raw, target.getMin(), target.getMax());
        return clamped == raw ? value : automationText::formatFloat(clamped); // in range: unchanged
    }
    if(valueType == typeid(int).name()) {
        auto& target = parameter.cast<int>().getParameter();
        const int raw = static_cast<int>(automationText::parse(value));
        const double clamped = clampComponent(raw, target.getMin(), target.getMax());
        return automationText::formatInt(std::lround(clamped));
    }
    if(valueType == typeid(std::vector<float>).name() || valueType == typeid(std::vector<int>).name()) {
        const bool isInt = valueType == typeid(std::vector<int>).name();
        std::vector<double> minimum;
        std::vector<double> maximum;
        if(isInt) {
            auto& target = parameter.cast<std::vector<int>>().getParameter();
            for(auto v : target.getMin()) minimum.push_back(v);
            for(auto v : target.getMax()) maximum.push_back(v);
        } else {
            auto& target = parameter.cast<std::vector<float>>().getParameter();
            for(auto v : target.getMin()) minimum.push_back(v);
            for(auto v : target.getMax()) maximum.push_back(v);
        }
        if(minimum.empty() || maximum.empty()) return value;
        std::string result;
        result.reserve(value.size());
        size_t i = 0;
        automationText::forEachToken(value, [&](const char* begin, const char* end) {
            const double clamped = clampComponent(automationText::parse(begin, end),
                                                  minimum[i % minimum.size()], maximum[i % maximum.size()]);
            if(i++ > 0) result += ",";
            if(isInt) automationText::appendInt(result, std::lround(clamped));
            else automationText::appendFloat(result, static_cast<float>(clamped));
        });
        return result;
    }
    return value;
}

// epoch: the clip start being played, so a roll differs from one play,
// seek or loop pass to the next (see ofxOceanodeTimelinePlayContext).
bool stepProbabilityPasses(const ofxOceanodeTimelineStep& step, double cycle, int seed, uint64_t epoch = 0) {
    const float probability = ofClamp(step.probability, 0.0f, 1.0f);
    if(probability <= 0.0f) return false;
    if(probability >= 1.0f) return true;

    // The result must remain stable while the playhead is inside a step. A
    // per-frame ofRandom() roll would make automation flicker at audio/frame
    // rate. This small integer hash gives one repeatable roll per step/cycle.
    const int64_t start = static_cast<int64_t>(std::llround(step.startBeat * 960.0));
    const int64_t cycleIndex = static_cast<int64_t>(std::llround(cycle));
    uint64_t hash = static_cast<uint64_t>(start) ^ (static_cast<uint64_t>(cycleIndex) * 0x9e3779b97f4a7c15ULL);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(seed)) * 0xd6e8feb86659fd93ULL;
    hash ^= epoch * 0x94d049bb133111ebULL;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    const float roll = static_cast<float>(hash & 0xffffULL) / 65535.0f;
    return roll < probability;
}

bool pianoProbabilityPasses(const ofxOceanodeTimelinePianoNote& note, double cycle, int seed, uint64_t epoch = 0) {
    const float probability = ofClamp(note.probability, 0.0f, 1.0f);
    if(probability <= 0.0f) return false;
    if(probability >= 1.0f) return true;
    const int64_t start = static_cast<int64_t>(std::llround(note.startBeat * 960.0));
    const int64_t pitch = static_cast<int64_t>(note.pitch);
    const int64_t cycleIndex = static_cast<int64_t>(std::llround(cycle));
    uint64_t hash = static_cast<uint64_t>(start) ^ (static_cast<uint64_t>(pitch) * 0x9e3779b97f4a7c15ULL);
    hash ^= static_cast<uint64_t>(cycleIndex) * 0xbf58476d1ce4e5b9ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(seed)) * 0xd6e8feb86659fd93ULL;
    hash ^= epoch * 0x94d049bb133111ebULL;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f < probability;
}

// Some note of the lane can actually change: the lane deviates at random, or
// a note has a mutation amount and its group's rules can change something.
bool pianoLaneMutates(const ofxOceanodeTimelineLane& lane) {
    return lane.randomDeviates() || std::any_of(lane.pianoNotes.begin(), lane.pianoNotes.end(), [&](const auto& note) {
        return note.mutation > 0.0f && lane.mutationStrength > 0.0f && ofxOceanodeTimelineMutationRulesFor(lane, note.mutationGroup).active();
    });
}

// A note as it plays in one repeat (cycle) of one clip start (epoch): itself
// unless its mutation roll passes; then each aspect changes, by its group's
// rules, with its own strength (a chance) and draws how far.
// Pitch moves to another allowed pitch class within the range; offset and
// length stay on the lane's grid.
ofxOceanodeTimelinePianoNote mutatedPianoNote(const ofxOceanodeTimelinePianoNote& note,
                                              const ofxOceanodeTimelineLane& lane, size_t index,
                                              int64_t cycle, uint64_t epoch, double sourceLength) {
    const float amount = ofClamp(note.mutation, 0.0f, 1.0f) * ofClamp(lane.mutationStrength, 0.0f, 1.0f);
    if(amount <= 0.0f) return note;
    const auto rules = ofxOceanodeTimelineMutationRulesFor(lane, note.mutationGroup);
    if(!rules.active()) return note;
    // splitmix64 stream, keyed on the note and the repeat being played.
    uint64_t state = static_cast<uint64_t>(std::llround(note.startBeat * 960.0)) ^
        (static_cast<uint64_t>(note.pitch) << 40) ^ (static_cast<uint64_t>(index) * 0x9e3779b97f4a7c15ULL) ^
        (static_cast<uint64_t>(cycle) * 0xbf58476d1ce4e5b9ULL) ^ (epoch * 0x94d049bb133111ebULL) ^
        (static_cast<uint64_t>(static_cast<uint32_t>(lane.probabilitySeed)) << 20);
    auto unit = [&]() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    };
    if(unit() >= amount) return note;
    auto mutated = note;
    const double grid = std::max(1.0 / 96.0, lane.beatsPerStep);
    if(unit() < rules.pitchStrength && rules.pitchRange > 0) {
        std::vector<int> candidates;
        const int range = std::min(rules.pitchRange, 48);
        const int lowest = rules.pitchDirection == 1 ? note.pitch : note.pitch - range;
        const int highest = rules.pitchDirection == 2 ? note.pitch : note.pitch + range;
        // Drifting pitch stays on the keys the roll shows.
        const bool drifting = lane.mutationDrift > 0.0f;
        const int floor = drifting ? std::clamp(lane.pianoLowPitch, 0, 127) : 0;
        const int ceiling = drifting ? std::clamp(lane.pianoHighPitch, floor, 127) : 127;
        for(int pitch = std::max(floor, lowest); pitch <= std::min(ceiling, highest); ++pitch)
            if(pitch != note.pitch && (rules.scaleMask >> (pitch % 12)) & 1) candidates.push_back(pitch);
        if(!candidates.empty())
            mutated.pitch = candidates[std::min(candidates.size() - 1, static_cast<size_t>(unit() * candidates.size()))];
    }
    // A direction: 0 either way, 1 up / later only, 2 down / earlier only.
    auto sign = [&](int direction) { return direction == 1 ? 1.0 : direction == 2 ? -1.0 : (unit() < 0.5 ? -1.0 : 1.0); };
    // Grid steps: a multiple of the quantum up to the steps (at least one quantum).
    auto stepsOf = [&](int steps, int quantum) {
        quantum = std::clamp(quantum, 1, 64);
        const int multiples = std::max(1, std::min(steps, 64) / quantum);
        return quantum * (1 + std::min(multiples - 1, static_cast<int>(unit() * multiples)));
    };
    if(unit() < rules.offsetStrength && rules.offsetSteps > 0) {
        const double shift = sign(rules.offsetDirection) * stepsOf(rules.offsetSteps, rules.offsetQuantum) * grid;
        mutated.startBeat = ofClamp(note.startBeat + shift, 0.0, std::max(0.0, sourceLength - grid));
    }
    if(unit() < rules.lengthStrength && rules.lengthSteps > 0) {
        const double change = sign(rules.lengthDirection) * stepsOf(rules.lengthSteps, rules.lengthQuantum) * grid;
        mutated.durationBeats = std::max(grid, note.durationBeats + change);
        if(lane.mutationDrift > 0.0f) mutated.durationBeats = std::min(mutated.durationBeats, std::max(grid, sourceLength));
    }
    if(unit() < rules.velocityStrength && rules.velocityRange > 0.0f)
        mutated.velocity = ofClamp(note.velocity + static_cast<float>(rules.velocityDirection == 0
            ? unit() * 2.0 - 1.0 : sign(rules.velocityDirection) * unit()) * rules.velocityRange, 0.0f, 1.0f);
    return mutated;
}

// The lane's random deviation (Rndm tab) of one repeat, on the notes as
// they play (mutated): octave, then pitch. A move that would leave 0..127
// goes the other way when both are allowed, else is not taken.
void deviatePianoNotes(const ofxOceanodeTimelineLane& lane, std::vector<ofxOceanodeTimelinePianoNote>& notes,
                       int64_t cycle, uint64_t epoch) {
    if(!lane.randomDeviates()) return;
    const bool onScale = lane.pianoScaleEnabled && lane.pianoScaleSnap && (lane.pianoScaleMask & 0xFFF) != 0;
    for(size_t n = 0; n < notes.size(); ++n) {
        auto& note = notes[n];
        // splitmix64 stream, keyed on the note and the repeat being played.
        uint64_t state = (static_cast<uint64_t>(n) * 0x9e3779b97f4a7c15ULL) ^
            (static_cast<uint64_t>(cycle) * 0xbf58476d1ce4e5b9ULL) ^ (epoch * 0x94d049bb133111ebULL) ^
            (static_cast<uint64_t>(static_cast<uint32_t>(lane.probabilitySeed)) << 20) ^ 0x5bd1e995a7c3f00dULL;
        auto unit = [&]() {
            uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            z ^= z >> 31;
            return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
        };
        if(unit() < lane.randomOctave && lane.randomOctaveRange > 0) {
            const int range = std::min(lane.randomOctaveRange, 4);
            const int octaves = 1 + std::min(range - 1, static_cast<int>(unit() * range));
            int sign = lane.randomOctaveDirection == 1 ? 1 : lane.randomOctaveDirection == 2 ? -1 : (unit() < 0.5 ? -1 : 1);
            int pitch = note.pitch + sign * octaves * 12;
            if((pitch < 0 || pitch > 127) && lane.randomOctaveDirection == 0) pitch = note.pitch - sign * octaves * 12;
            if(pitch >= 0 && pitch <= 127) note.pitch = pitch;
        }
        if(unit() < lane.randomPitch && lane.randomPitchRange > 0) {
            const int range = std::min(lane.randomPitchRange, 24);
            const int lowest = lane.randomPitchDirection == 1 ? note.pitch : note.pitch - range;
            const int highest = lane.randomPitchDirection == 2 ? note.pitch : note.pitch + range;
            std::vector<int> candidates;
            for(int pitch = std::max(0, lowest); pitch <= std::min(127, highest); ++pitch)
                if(pitch != note.pitch && (!onScale || (lane.pianoScaleMask >> (pitch % 12)) & 1)) candidates.push_back(pitch);
            if(!candidates.empty())
                note.pitch = candidates[std::min(candidates.size() - 1, static_cast<size_t>(unit() * candidates.size()))];
        }
    }
}

// A lane's notes as they play in one repeat. Monophonic: a note shifted onto
// another's start keeps its written timing, and a note that runs into the
// next one ends there, when either of them was shifted or lengthened.
std::vector<ofxOceanodeTimelinePianoNote> mutatedPianoNotes(const ofxOceanodeTimelineLane& lane,
                                                            const std::vector<ofxOceanodeTimelinePianoNote>& written,
                                                            int64_t cycle, uint64_t epoch, double sourceLength) {
    std::vector<ofxOceanodeTimelinePianoNote> out(written.size());
    for(size_t n = 0; n < written.size(); ++n)
        out[n] = mutatedPianoNote(written[n], lane, n, cycle, epoch, sourceLength);
    if(!lane.mutationMonophonic) return out;
    constexpr double eps = 1e-9;
    auto retimed = [&](size_t n) {
        return out[n].startBeat != written[n].startBeat || out[n].durationBeats != written[n].durationBeats;
    };
    for(size_t n = 0; n < out.size(); ++n) {
        if(out[n].startBeat == written[n].startBeat) continue;
        for(size_t m = 0; m < out.size(); ++m) {
            if(m == n || std::abs(out[m].startBeat - out[n].startBeat) > eps) continue;
            out[n].startBeat = written[n].startBeat;
            out[n].durationBeats = written[n].durationBeats;
            break;
        }
    }
    std::vector<double> ends(out.size());
    for(size_t i = 0; i < out.size(); ++i) {
        ends[i] = out[i].startBeat + out[i].durationBeats;
        for(size_t j = 0; j < out.size(); ++j) {
            if(out[j].startBeat <= out[i].startBeat + eps || out[j].startBeat >= ends[i]) continue;
            if(retimed(i) || retimed(j)) ends[i] = out[j].startBeat;
        }
    }
    for(size_t i = 0; i < out.size(); ++i)
        out[i].durationBeats = std::max(1.0 / 96.0, ends[i] - out[i].startBeat);
    return out;
}

// localBeat is the clip's source beat, which starts over in every repeat of
// the clip; clipRepeat says which repeat it is, so each one rolls its own
// probabilities (as piano notes do).
bool evaluateStepSequencer(const ofxOceanodeTimelineLane& lane,
                           double localBeat,
                           std::string& value,
                           double retriggerGap = 0.0,
                           int64_t clipRepeat = 0,
                           double contentLength = 0.0,
                           uint64_t epoch = 0) {
    if(lane.behavior == "Mute") return false;
    const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
    const double patternLength = std::max(cellLength, lane.stepCount * cellLength);
    const double patternsPerRepeat = std::max(1.0, std::ceil(contentLength / patternLength - 1e-9));
    const double cycle = static_cast<double>(clipRepeat) * patternsPerRepeat +
                         std::floor(std::max(0.0, localBeat) / patternLength);
    const double patternBeat = positiveModulo(localBeat, patternLength);
    auto startOf = [](const ofxOceanodeTimelineStep& s) { return std::max(0.0, s.startBeat); };
    auto endOf = [&](const ofxOceanodeTimelineStep& s) {
        return std::max(0.0, s.startBeat) + (s.durationBeats > kEpsilon ? s.durationBeats : cellLength);
    };
    // Only gate steps (no value, or 1) retrigger: repeated value steps (a scene
    // index, say) must not dip to 0 between them.
    auto isGate = [](const ofxOceanodeTimelineStep& s) { return s.value.empty() || s.value == "1"; };
    auto same = [&](const ofxOceanodeTimelineStep& a, const ofxOceanodeTimelineStep& b) {
        return isGate(a) && isGate(b);
    };

    const auto& steps = lane.step.steps;
    for(size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        const double start = startOf(step);
        if(patternBeat + kEpsilon < start || patternBeat >= endOf(step) - kEpsilon) continue;
        if(inRetriggerGap(steps, index, patternBeat, retriggerGap, startOf, endOf, same, patternLength)) continue;
        const bool useProbability = lane.probabilityEnabled && lane.behavior == "Probability";
        if(useProbability && !stepProbabilityPasses(step, cycle, lane.probabilitySeed, epoch)) return false;
        value = !isGate(step) ? step.value
              : step.velocity >= 1.0f ? "1"
              : automationText::formatFloat(ofClamp(step.velocity, 0.0f, 1.0f));
        return true;
    }
    return false;
}

// MultiSlider ("step value"): unlike evaluateStepSequencer above (sparse --
// only fires where a step was explicitly placed, and that step's height is
// a fire *probability*), every cell here always has a value -- the whole
// point is a dense, always-on per-step value grid, like a bar of tiny
// vertical sliders. Always returns a value; there is no "not playing" case
// short of the clip itself not being active (already handled by the
// caller's clipSourceBeat check before this is ever called).
bool evaluateMultiSlider(const ofxOceanodeTimelineLane& lane, double localBeat, std::string& value) {
    const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
    const int stepCount = std::max(1, lane.stepCount);
    const double patternLength = cellLength * stepCount;
    const double patternBeat = positiveModulo(localBeat, patternLength);
    int index = static_cast<int>(std::floor(patternBeat / cellLength));
    index = std::min(std::max(index, 0), stepCount - 1);
    const float raw = index < static_cast<int>(lane.multiSliderValues.size()) ? lane.multiSliderValues[index] : 0.0f;
    // A cell holds a REAL value in the lane's own valueMin..valueMax units
    // (that is the whole point of this lane type, and what the editor writes
    // and the previews read). Every lane evaluator here returns a normalized
    // 0..1 value that the caller maps back through the lane's range, so undo
    // the range here rather than handing the raw value over as if it were
    // already normalized -- doing that clamped anything above 1 and then
    // re-expanded it to valueMax, so a 0..127 lane answered 127 for every
    // cell a user could actually paint. No clamp: the round trip is then
    // exact for values outside the range too.
    const float span = lane.valueMax - lane.valueMin;
    value = automationText::formatFloat(std::abs(span) < 1e-9f ? 0.0f : (raw - lane.valueMin) / span);
    return true;
}

// MultiValue / MultiGate: find whichever region in this one row is active
// at localBeat (regions never overlap within a row -- both the old
// standalone nodes and this lane's own controller-side create/move
// interactions prevent that), matching valueTrack.h/gateTrack.h's own
// per-lane region scan.
bool evaluateMultiValueRow(const std::vector<ofxOceanodeTimelineValueRegion>& row, double localBeat, float& value) {
    for(const auto& region : row) {
        if(localBeat >= region.startBeat && localBeat < region.end()) {
            value = region.value;
            return true;
        }
    }
    return false;
}

// Whether one gate block reaches one target (lane.targetProbability): one
// stable roll per block, row, repeat and clip start (see stepProbabilityPasses).
bool toggleTargetPasses(const ofxOceanodeTimelineLane& lane, const std::string& path, float probability,
                        double regionStart, int row, int64_t cycle, uint64_t epoch) {
    if(probability <= 0.0f) return false;
    if(probability >= 1.0f) return true;
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a of the path
    for(const unsigned char c : path) { hash ^= c; hash *= 0x100000001b3ULL; }
    hash ^= static_cast<uint64_t>(std::llround(regionStart * 960.0)) * 0x9e3779b97f4a7c15ULL;
    hash ^= (static_cast<uint64_t>(cycle) << 8 | static_cast<uint64_t>(row & 0xff)) * 0xbf58476d1ce4e5b9ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(lane.probabilitySeed)) * 0xd6e8feb86659fd93ULL;
    hash ^= epoch * 0x94d049bb133111ebULL;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f < probability;
}

double pianoNoteEnd(const ofxOceanodeTimelinePianoNote& note) {
    return note.startBeat + std::max(1.0 / 24.0, note.durationBeats);
}

// Gives every note a voice slot (see ofxOceanodeTimelinePianoVoicing). Notes
// are taken in start order; a note takes the free slot that has been free the
// longest (so releases ring out), a new slot when the count is open (voices
// 0), or else steals the slot whose note started first, which ends there. A
// kit's slot is fixed by the key.
// sounding: when given, only those notes take a voice (the others play
// nowhere, so they neither steal a slot nor set its pitch).
void computePianoVoicing(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane& lane,
                         int voices, bool kit, ofxOceanodeTimelinePianoVoicing& out,
                         const std::vector<double>* noteEnds = nullptr,
                         const std::vector<uint8_t>* sounding = nullptr) {
    constexpr double eps = 1e-6;
    const auto& notes = lane.pianoNotes;
    const size_t count = notes.size();
    out = ofxOceanodeTimelinePianoVoicing();
    out.kit = kit;
    out.slot.assign(count, -1);
    out.end.resize(count);
    out.retrigger.assign(count, 0);
    out.wrapRetrigger.assign(count, 0);
    for(size_t i = 0; i < count; ++i) {
        out.end[i] = noteEnds == nullptr ? pianoNoteEnd(notes[i]) : (*noteEnds)[i];
        if(notes[i].probability < 1.0f || notes[i].mutation > 0.0f) out.chance = true;
    }
    if(lane.randomDeviates()) out.chance = true;

    std::vector<size_t> order(count);
    for(size_t i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if(std::abs(notes[a].startBeat - notes[b].startBeat) > eps) return notes[a].startBeat < notes[b].startBeat;
        return notes[a].pitch < notes[b].pitch;
    });

    std::vector<long long> current;   // note sounding in each slot (-1: none yet)
    std::vector<double> freeSince;    // when each slot's last note ended
    auto ensureSlot = [&](int s) {
        if(s < static_cast<int>(current.size())) return;
        current.resize(s + 1, -1);
        freeSince.resize(s + 1, -std::numeric_limits<double>::infinity());
    };
    if(voices > 0 && !kit) ensureSlot(voices - 1);

    for(const size_t n : order) {
        if(sounding != nullptr && !(*sounding)[n]) continue;
        const double start = notes[n].startBeat;
        int chosen = -1;
        if(kit) {
            const int s = notes[n].pitch - kKitBaseNote;
            if(s < 0 || (voices > 0 && s >= voices)) continue;
            ensureSlot(s);
            chosen = s;
        } else {
            for(int s = 0; s < static_cast<int>(current.size()); ++s) {
                const bool free = current[s] < 0 || out.end[current[s]] <= start + eps;
                if(!free) continue;
                const double since = current[s] < 0 ? freeSince[s] : out.end[current[s]];
                const double best = chosen < 0 ? 0.0
                    : (current[chosen] < 0 ? freeSince[chosen] : out.end[current[chosen]]);
                if(chosen < 0 || since < best - eps) chosen = s;
            }
            if(chosen < 0 && voices <= 0) {
                chosen = static_cast<int>(current.size());
                ensureSlot(chosen);
            }
            if(chosen < 0) {
                // Every slot busy: steal the one whose note started first.
                for(int s = 0; s < static_cast<int>(current.size()); ++s)
                    if(chosen < 0 || notes[current[s]].startBeat < notes[current[chosen]].startBeat - eps) chosen = s;
            }
        }
        const long long previous = current[chosen];
        if(previous >= 0) {
            if(out.end[previous] > start) out.end[previous] = std::max(notes[previous].startBeat, start);
            if(out.end[previous] >= start - eps) out.retrigger[n] = 1;
            freeSince[chosen] = out.end[previous];
        }
        current[chosen] = static_cast<long long>(n);
        out.slot[n] = chosen;
    }

    out.width = std::max(1, voices > 0 ? voices : static_cast<int>(current.size()));
    out.slotNotes.assign(std::max<size_t>(current.size(), static_cast<size_t>(out.width)), {});
    for(const size_t n : order)
        if(out.slot[n] >= 0) out.slotNotes[out.slot[n]].push_back(n);
    // A repeating clip: a slot whose last note runs into the content end
    // touches its first note of the next repeat.
    if(clip.repeatContent) {
        const double length = ofxOceanodeTimelineClipTime::sourceDuration(clip);
        for(const auto& slotNotes : out.slotNotes) {
            if(slotNotes.empty()) continue;
            const size_t first = slotNotes.front();
            if(notes[first].startBeat > eps) continue;
            for(const size_t n : slotNotes)
                if(out.end[n] >= length - eps) { out.wrapRetrigger[first] = 1; break; }
        }
    }
}

// A copy's contribution to its rolls. Never called for the main timeline
// (instanceKey 0), whose rolls stay exactly what they were before copies.
uint64_t instanceRollMix(uint64_t instanceKey) {
    uint64_t z = instanceKey + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
} // namespace

uint64_t ofxOceanodeTimelinePlayContext::epoch() const {
    uint64_t value = (generation * 0x9e3779b97f4a7c15ULL) ^ (pass * 0xd6e8feb86659fd93ULL) ^ salt;
    if(instanceKey != 0) value ^= instanceRollMix(instanceKey);
    return value;
}

bool ofxOceanodeTimelineClipChanceSilenced(const ofxOceanodeTimelineClip& clip,
                                           const ofxOceanodeTimelinePlayContext& context) {
    const float chance = ofClamp(clip.chance, 0.0f, 1.0f);
    if(chance >= 1.0f) return false;
    if(chance <= 0.0f) return true;
    // Same mixer as the step/note rolls: stable for the whole pass,
    // so the frame path and the scheduler agree on it.
    uint64_t hash = std::hash<std::string>{}(clip.id) ^ context.salt;
    hash ^= context.generation * 0xbf58476d1ce4e5b9ULL;
    hash ^= context.pass * 0xd6e8feb86659fd93ULL;
    if(context.instanceKey != 0) hash ^= instanceRollMix(context.instanceKey);
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f >= chance;
}

ofxOceanodeTimelinePlayContext ofxOceanodeTimelineManager::playContextFor(
        const ofxOceanodeTransportState& transport, uint64_t passOffset) const {
    ofxOceanodeTimelinePlayContext context;
    context.generation = transport.generation;
    context.pass = transport.loopCount + passOffset;
    context.salt = clipChanceSalt;
    return context;
}

std::vector<ofxOceanodeTimelineManager::PlayedSequence> ofxOceanodeTimelineManager::playedSequences(
        double /*fromBeat*/, double /*toBeat*/) const {
    // The main timeline, through the identity map, as copy 0.
    return {PlayedSequence{&mainSequence, {}, 0}};
}

bool ofxOceanodeTimelineManager::isClipChanceSilenced(const ofxOceanodeTimelineClip& clip) const {
    return ofxOceanodeTimelineClipChanceSilenced(clip, playContext);
}

void ofxOceanodeTimelineManager::invalidateEvaluationIndexes() {
    evaluationIndexesValid = false;
    bindingsByPathCache.clear();
    trackByIdCache.clear();
    trackEvaluationIndexes.clear();
    pianoPitchRangesCache.clear();
    pianoInputPriorityCache.clear();
    pianoRoleGroupsCache.clear();
    zeroWhenInactiveBindingsCache.clear();
    continuouslyEvaluatedPathsCache.clear();
    stateCapturePathsCache.clear();
    playback.clearRolls();
    parameterTrackColorCache.clear();
    parameterTrackColorCacheFrame = std::numeric_limits<uint64_t>::max();
}

void ofxOceanodeTimelineManager::rebuildEvaluationIndexes() {
    bindingsByPathCache.clear();
    trackByIdCache.clear();
    trackEvaluationIndexes.clear();
    pianoPitchRangesCache.clear();
    // Roles are rediscovered below; a path no longer routed to a piano lane
    // must lose its lead and its place ahead of the other paths.
    pianoInputPriorityCache.clear();
    pianoRoleGroupsCache.clear();
    zeroWhenInactiveBindingsCache.clear();
    continuouslyEvaluatedPathsCache.clear();
    stateCapturePathsCache.clear();
    playback.clearRolls();

    size_t bindingCount = 0;
    for(const auto& track : mainSequence.tracks) bindingCount += track.bindings.size();
    bindingsByPathCache.reserve(bindingCount);
    trackByIdCache.reserve(mainSequence.tracks.size());
    trackEvaluationIndexes.reserve(mainSequence.tracks.size());

    for(auto& track : mainSequence.tracks) {
        trackByIdCache.emplace(track.id, &track);
        auto& index = trackEvaluationIndexes[&track];
        index.bindingById.reserve(track.bindings.size());
        index.clipOrder.reserve(track.clips.size());
        for(auto& binding : track.bindings) {
            index.bindingById.emplace(binding.id, &binding);
            bindingsByPathCache[binding.parameterPath].push_back(&binding);
        }
        for(size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex)
            index.clipOrder.emplace(&track.clips[clipIndex], clipIndex);
    }

    // These properties depend only on the timeline structure. Cache them once
    // for the current update instead of rediscovering them for the playhead and
    // again for every scheduler lookahead probe.
    for(const auto& track : mainSequence.tracks) {
        if(!isTrackAudible(track)) continue;
        const auto trackIt = trackEvaluationIndexes.find(&track);
        if(trackIt == trackEvaluationIndexes.end()) continue;
        const auto bindingFor = [&](const std::string& bindingId) -> const ofxOceanodeTimelineParameterBinding* {
            const auto bindingIt = trackIt->second.bindingById.find(bindingId);
            return bindingIt == trackIt->second.bindingById.end() ? nullptr : bindingIt->second;
        };
        for(const auto& binding : track.bindings)
            if(binding.hasLiveOverride) continuouslyEvaluatedPathsCache.emplace(binding.parameterPath);
        for(const auto& clip : track.clips) {
            if(clip.muted) continue;
            if(clip.isLfo) {
                if(const auto* binding = bindingFor(clip.lfoOutputBindingId))
                    continuouslyEvaluatedPathsCache.emplace(binding->parameterPath);
                continue;
            }
            for(const auto& lane : clip.lanes) {
                if(!laneAudible(clip, lane)) continue;
                if(lane.type == ofxOceanodeTimelineLaneType::Curve) {
                    for(const auto& bindingId : lane.bindingIds)
                        if(const auto* binding = bindingFor(bindingId)) {
                            continuouslyEvaluatedPathsCache.emplace(binding->parameterPath);
                            if(clip.isStateCapture) stateCapturePathsCache.emplace(binding->parameterPath);
                        }
                    continue;
                }
                if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid) {
                    // Pitch is prepared ahead of Gate, both scheduled together;
                    // outside its clips the gate is off and the pitch holds.
                    std::vector<std::string> rolePaths;
                    for(const auto& role : {std::make_pair(&lane.pianoPitchBindingId, 0),
                                           std::make_pair(&lane.drumDurationBindingId, 0),
                                           std::make_pair(&lane.drumFilterBindingId, 0),
                                           std::make_pair(&lane.drumStutterBindingId, 0),
                                           std::make_pair(&lane.pianoGateBindingId, 3)}) {
                        if(const auto* binding = bindingFor(*role.first)) {
                            pianoInputPriorityCache[binding->parameterPath] = role.second;
                            if(!binding->bypass) rolePaths.push_back(binding->parameterPath);
                        }
                    }
                    if(rolePaths.size() > 1) pianoRoleGroupsCache.push_back(std::move(rolePaths));
                    int voices = 0, instrumentVoices = 0;
                    bool kit = false;
                    pianoLaneVoicesAndKit(track, lane, voices, kit, &instrumentVoices);
                    trackIt->second.drumOutputWidth[&lane] = instrumentVoices;
                    if(const auto* binding = bindingFor(lane.pianoGateBindingId))
                        zeroWhenInactiveBindingsCache.emplace(binding);
                    continue;
                }
                if(lane.type != ofxOceanodeTimelineLaneType::PianoRoll) continue;
                // Roles come from the routing, never parameter names. A piano
                // roll may target an arbitrary instrument's published inputs.
                std::vector<std::string> rolePaths;
                for(const auto& role : {std::make_pair(&lane.pianoPitchBindingId, 0),
                                       std::make_pair(&lane.pianoVelocityBindingId, 1),
                                       std::make_pair(&lane.pianoGateBindingId, 3)}) {
                    if(const auto* binding = bindingFor(*role.first)) {
                        pianoInputPriorityCache[binding->parameterPath] = role.second;
                        if(!binding->bypass) rolePaths.push_back(binding->parameterPath);
                    }
                }
                if(rolePaths.size() > 1) pianoRoleGroupsCache.push_back(std::move(rolePaths));
                auto& voicing = trackIt->second.pianoVoicing[&lane];
                computeLaneVoicing(track, clip, lane, voicing);
                if(const auto* binding = bindingFor(lane.pianoPitchBindingId); binding != nullptr && !voicing.kit) {
                    auto& range = pianoPitchRangesCache[binding->parameterPath];
                    range.valueType = binding->valueType;
                    range.low = std::min(range.low, static_cast<int>(ofClamp(lane.pianoLowPitch, 0, 127)));
                    range.high = std::max(range.high, static_cast<int>(ofClamp(lane.pianoHighPitch, 0, 127)));
                }
                // Outside its clips the gate is off; pitch and velocity hold
                // (a release still sounding keeps its note and its level).
                if(const auto* binding = bindingFor(lane.pianoGateBindingId))
                    zeroWhenInactiveBindingsCache.emplace(binding);
            }
            // Piano lanes driving the same roles are parts of one instrument.
            // Allocate their notes together so independent bass/melody editors
            // share voices rather than replacing or adding MIDI pitches.
            std::map<std::vector<std::string>, std::vector<const ofxOceanodeTimelineLane*>> parts;
            for(const auto& lane : clip.lanes) {
                if(laneAudible(clip, lane) && lane.type == ofxOceanodeTimelineLaneType::PianoRoll)
                    parts[{lane.pianoPitchBindingId, lane.pianoGateBindingId, lane.pianoVelocityBindingId}].push_back(&lane);
            }
            for(const auto& entry : parts) {
                if(entry.second.size() < 2) continue;
                auto playback = std::make_shared<SharedPianoPlayback>();
                playback->first = entry.second.front();
                playback->lane = *playback->first;
                playback->lane.pianoNotes.clear();
                playback->lane.pianoMonophonic = false;
                std::vector<double> ends;
                // A duplicated lane starts as a copy of its source: a note of
                // another part on the same start and key is the same note, not
                // a second voice. (Within one part, every note is kept.)
                std::set<std::pair<int64_t, int>> earlierParts;
                for(const auto* part : entry.second) {
                    const auto& local = trackIt->second.pianoVoicing.at(part);
                    std::vector<std::pair<int64_t, int>> thisPart;
                    for(size_t n = 0; n < part->pianoNotes.size(); ++n) {
                        const auto& note = part->pianoNotes[n];
                        const std::pair<int64_t, int> key{std::llround(note.startBeat * 96000.0), note.pitch};
                        if(earlierParts.count(key) > 0) continue;
                        thisPart.push_back(key);
                        playback->lane.pianoNotes.push_back(note);
                        playback->noteLanes.push_back(part);
                        playback->noteIndex.push_back(n);
                        // Preserve per-part mono note stealing before sharing
                        // the instrument's overall voice limit.
                        ends.push_back(local.end[n]);
                    }
                    earlierParts.insert(thisPart.begin(), thisPart.end());
                    trackIt->second.sharedPiano.emplace(part, playback);
                }
                int voices = 0, instrumentVoices = 0;
                bool kit = false;
                pianoLaneVoicesAndKit(track, playback->lane, voices, kit, &instrumentVoices);
                computePianoVoicing(clip, playback->lane, voices, kit, playback->voicing, &ends);
                for(const auto* part : entry.second)
                    if(part->randomDeviates()) playback->voicing.chance = true;
                playback->voicing.outputWidth = std::max(playback->voicing.width, instrumentVoices);
                playback->voicing.instrument = instrumentVoices > 0;
            }
        }
    }
    automationPathOrderCache.clear();
    automationPathOrderCache.reserve(bindingsByPathCache.size());
    for(const auto& entry : bindingsByPathCache) automationPathOrderCache.push_back(entry.first);
    const auto priority = [this](const std::string& path) {
        const auto it = pianoInputPriorityCache.find(path);
        return it == pianoInputPriorityCache.end() ? 2 : it->second;
    };
    std::sort(automationPathOrderCache.begin(), automationPathOrderCache.end(), [&](const auto& a, const auto& b) {
        const int pa = priority(a), pb = priority(b);
        return pa != pb ? pa < pb : a < b;
    });
    evaluationIndexesValid = true;
    rebuildParameterTrackColorCache();
}

void ofxOceanodeTimelineManager::pianoLaneVoicesAndKit(const ofxOceanodeTimelineTrack& track,
                                                       const ofxOceanodeTimelineLane& lane,
                                                       int& voices, bool& kit, int* instrumentVoices) const {
    voices = 0;
    kit = false;
    if(instrumentVoices != nullptr) *instrumentVoices = 0;
    const ofxOceanodeTimelineNoteGroup* group = nullptr;
    for(const auto* id : {&lane.pianoGateBindingId, &lane.pianoPitchBindingId, &lane.pianoVelocityBindingId})
        if(group == nullptr) group = findNoteGroupForBinding(track, *id);
    // The instrument the lane plays: the node of its gate (or pitch) parameter,
    // when that node says this parameter is one of its note inputs.
    for(const auto* id : {&lane.pianoGateBindingId, &lane.pianoPitchBindingId}) {
        if(id->empty()) continue;
        const auto* binding = bindingInTrack(track, *id);
        auto* parameter = binding == nullptr ? nullptr : findParameterCached(binding->parameterPath);
        auto* model = parameter == nullptr ? nullptr : parameter->getNodeModel();
        ofxOceanodeTimelineInstrument instrument;
        if(model == nullptr || !model->getTimelineInstrument(instrument)) continue;
        if(instrument.gate != parameter && instrument.pitch != parameter) continue;
        voices = std::max(0, instrument.voices);
        kit = instrument.kit;
        if(instrumentVoices != nullptr) *instrumentVoices = voices;
        break;
    }
    if(group != nullptr && group->voices > 0) voices = group->voices;
    if(lane.pianoMonophonic && !kit) voices = 1;
}

void ofxOceanodeTimelineManager::computeLaneVoicing(const ofxOceanodeTimelineTrack& track,
                                                    const ofxOceanodeTimelineClip& clip,
                                                    const ofxOceanodeTimelineLane& lane,
                                                    ofxOceanodeTimelinePianoVoicing& out,
                                                    const std::vector<uint8_t>* sounding) const {
    int voices = 0, instrumentVoices = 0;
    bool kit = false;
    pianoLaneVoicesAndKit(track, lane, voices, kit, &instrumentVoices);
    computePianoVoicing(clip, lane, voices, kit, out, nullptr, sounding);
    out.outputWidth = std::max(out.width, instrumentVoices);
    out.instrument = instrumentVoices > 0;
}

const ofxOceanodeTimelineManager::PianoRepeatPlayback& ofxOceanodeTimelineManager::repeatPlayback(
        const ofxOceanodeTimelinePlayContext& context,
        const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineClip& clip,
        const ofxOceanodeTimelineLane& lane, const SharedPianoPlayback* shared, int64_t repeat) const {
    // A shared pool is cached under its first lane; the index rebuild that
    // regroups pools also clears this cache.
    const auto& owner = shared != nullptr && shared->first != nullptr ? *shared->first : lane;
    auto& cached = playback.repeatVoicing[PlayKey{context.instanceKey, owner.id}];
    const uint64_t mutationEpoch = context.epoch();
    const std::pair<uint64_t, int64_t> key{mutationEpoch, repeat};
    if(const auto it = cached.find(key); it != cached.end()) return it->second;
    // A few repeats around the playhead (the frame's and the scheduler's),
    // of the current clip start first.
    while(cached.size() >= 8) {
        auto farthest = cached.begin();
        auto distance = [&](const auto& entry) {
            return entry.first.first != mutationEpoch ? std::numeric_limits<int64_t>::max()
                                                      : std::llabs(entry.first.second - repeat);
        };
        for(auto it = cached.begin(); it != cached.end(); ++it)
            if(distance(*it) > distance(*farthest)) farthest = it;
        cached.erase(farthest);
    }
    const double cycle = static_cast<double>(repeat);
    const double sourceLength = ofxOceanodeTimelineClipTime::sourceDuration(clip);
    // Rolls use the notes as written; voices follow the notes as they play.
    auto soundingIn = [&](const ofxOceanodeTimelineLane& part) {
        std::vector<uint8_t> sounding(part.pianoNotes.size());
        for(size_t n = 0; n < part.pianoNotes.size(); ++n)
            sounding[n] = pianoProbabilityPasses(part.pianoNotes[n], cycle, part.probabilitySeed, mutationEpoch) ? 1 : 0;
        return sounding;
    };
    auto mutate = [&](const ofxOceanodeTimelineLane& part, bool& changed) {
        ofxOceanodeTimelineLane copy = part;
        copy.pianoNotes = repeatNotes(context, part, repeat, sourceLength);
        for(size_t n = 0; n < copy.pianoNotes.size(); ++n) {
            const auto& before = part.pianoNotes[n];
            const auto& after = copy.pianoNotes[n];
            changed = changed || after.pitch != before.pitch || after.startBeat != before.startBeat ||
                      after.durationBeats != before.durationBeats || after.velocity != before.velocity;
        }
        return copy;
    };
    PianoRepeatPlayback out;
    if(shared == nullptr) {
        const auto sounding = soundingIn(lane);
        out.lane = mutate(lane, out.mutated);
        computeLaneVoicing(track, clip, out.mutated ? out.lane : lane, out.voicing, &sounding);
    } else {
        // As the indexes build a shared playback: each part's own stealing
        // first (among its sounding notes), then the instrument's voices.
        const auto& notes = shared->lane.pianoNotes;
        std::vector<uint8_t> sounding(notes.size());
        std::vector<double> ends(notes.size());
        std::unordered_map<const ofxOceanodeTimelineLane*, std::pair<ofxOceanodeTimelineLane, ofxOceanodeTimelinePianoVoicing>> parts;
        out.lane = shared->lane;
        for(size_t m = 0; m < notes.size(); ++m) {
            const auto* part = shared->noteLanes[m];
            auto [local, created] = parts.try_emplace(part);
            if(created) {
                bool partChanged = false;
                local->second.first = mutate(*part, partChanged);
                out.mutated = out.mutated || partChanged;
                const auto partSounding = soundingIn(*part);
                computeLaneVoicing(track, clip, local->second.first, local->second.second, &partSounding);
            }
            out.lane.pianoNotes[m] = local->second.first.pianoNotes[shared->noteIndex[m]];
            sounding[m] = pianoProbabilityPasses(notes[m], cycle, part->probabilitySeed, mutationEpoch) ? 1 : 0;
            ends[m] = local->second.second.end[shared->noteIndex[m]];
        }
        int voices = 0, instrumentVoices = 0;
        bool kit = false;
        pianoLaneVoicesAndKit(track, shared->lane, voices, kit, &instrumentVoices);
        computePianoVoicing(clip, out.mutated ? out.lane : shared->lane, voices, kit, out.voicing, &ends, &sounding);
        out.voicing.outputWidth = std::max(out.voicing.width, instrumentVoices);
        out.voicing.instrument = instrumentVoices > 0;
    }
    if(!out.mutated) out.lane = ofxOceanodeTimelineLane();
    out.voicing.chance = true;
    return cached.emplace(key, std::move(out)).first->second;
}

bool ofxOceanodeTimelineManager::pianoRepeatNotes(const std::string& trackId, const std::string& clipId,
                                                  const std::string& laneId, int64_t repeat,
                                                  std::vector<ofxOceanodeTimelinePianoNote>& out) const {
    const auto* track = getTrack(trackId);
    const auto* clip = getClip(trackId, clipId);
    const auto* lane = getLane(trackId, clipId, laneId);
    if(track == nullptr || clip == nullptr || lane == nullptr || !pianoLaneMutates(*lane)) return false;
    const double sourceLength = ofxOceanodeTimelineClipTime::sourceDuration(*clip);
    out = repeatNotes(playContext, *lane, repeat, sourceLength);
    return true;
}

std::vector<ofxOceanodeTimelinePianoNote> ofxOceanodeTimelineManager::repeatNotes(
        const ofxOceanodeTimelinePlayContext& context,
        const ofxOceanodeTimelineLane& lane, int64_t repeat, double sourceLength) const {
    const uint64_t mutationEpoch = context.epoch();
    // Random deviation goes on top: drift builds on the mutated notes only.
    auto deviated = [&](std::vector<ofxOceanodeTimelinePianoNote> notes) {
        deviatePianoNotes(lane, notes, repeat, mutationEpoch);
        return notes;
    };
    if(lane.mutationDrift <= 0.0f) return deviated(mutatedPianoNotes(lane, lane.pianoNotes, repeat, mutationEpoch, sourceLength));
    // Repeats are asked for in play order (the scheduler's lookahead first, then
    // the frame's), so a new one drifts from the latest; known ones are kept.
    auto& steps = playback.pianoDrift[PlayKey{context.instanceKey, lane.id}];
    const std::pair<uint64_t, int64_t> key{mutationEpoch, repeat};
    for(const auto& step : steps) if(step.first == key) return deviated(step.second);
    // Each note drifts (builds on the previous repeat) with the drift chance,
    // else mutates from how it is written.
    auto base = lane.pianoNotes;
    if(!steps.empty() && steps.back().second.size() == base.size()) {
        const auto& previous = steps.back().second;
        for(size_t n = 0; n < base.size(); ++n) {
            uint64_t z = (static_cast<uint64_t>(n) * 0x9e3779b97f4a7c15ULL) ^ (static_cast<uint64_t>(repeat) * 0xbf58476d1ce4e5b9ULL) ^
                         (mutationEpoch * 0x94d049bb133111ebULL) ^ (static_cast<uint64_t>(static_cast<uint32_t>(lane.probabilitySeed)) << 17) ^
                         0xd1f7a5c3ULL;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            z ^= z >> 31;
            if(static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0) < lane.mutationDrift) base[n] = previous[n];
        }
    }
    auto notes = mutatedPianoNotes(lane, base, repeat, mutationEpoch, sourceLength);
    steps.emplace_back(key, notes);
    if(steps.size() > 8) steps.erase(steps.begin());
    return deviated(std::move(notes));
}

bool ofxOceanodeTimelineManager::drumGridInstrument(const ofxOceanodeTimelineTrack& track,
                                                    const ofxOceanodeTimelineLane& lane,
                                                    int& voices, std::vector<std::string>& names, bool* kit,
                                                    std::vector<ofColor>* colors) const {
    voices = 0;
    names.clear();
    if(kit != nullptr) *kit = false;
    if(colors != nullptr) colors->clear();
    for(const auto* id : {&lane.pianoGateBindingId, &lane.pianoPitchBindingId}) {
        if(id->empty()) continue;
        const auto* binding = bindingInTrack(track, *id);
        auto* parameter = binding == nullptr || container == nullptr ? nullptr : container->findTimelineParameter(binding->parameterPath);
        auto* model = parameter == nullptr ? nullptr : parameter->getNodeModel();
        ofxOceanodeTimelineInstrument instrument;
        if(model == nullptr || !model->getTimelineInstrument(instrument)) continue;
        if(instrument.gate != parameter && instrument.pitch != parameter) continue;
        voices = std::max(0, instrument.voices);
        names = instrument.rowNames;
        if(kit != nullptr) *kit = instrument.kit;
        if(colors != nullptr) *colors = instrument.rowColors;
        return true;
    }
    return false;
}

bool ofxOceanodeTimelineManager::noteGroupInstrument(const std::string& trackId, const std::string& groupId,
                                                     int& voices, bool& kit, std::vector<std::string>* names) const {
    voices = 0;
    kit = false;
    const auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    const auto it = std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(it == track->noteGroups.end()) return false;
    ofxOceanodeTimelineLane probe;
    probe.pianoGateBindingId = it->gateBindingId;
    probe.pianoPitchBindingId = it->pitchBindingId;
    std::vector<std::string> found;
    const bool result = drumGridInstrument(*track, probe, voices, found, &kit);
    if(names != nullptr) *names = std::move(found);
    return result;
}
namespace {

// Cycles started in the first `sourceBeats` of an LFO clip's content: a partial
// final cycle counts as started, so the next segment begins a new cycle index.
double lfoCyclesStarted(const ofxOceanodeTimelineClip& clip, double sourceBeats) {
    if(sourceBeats <= kEpsilon) return 0.0;
    const double cycles = lfoCycles(clip, sourceBeats);
    return cycles <= 1e-9 ? 0.0 : std::ceil(cycles - 1e-9);
}

// Cycles started over a whole clip (all its repeats, including a partial last one).
double lfoClipCyclesStarted(const ofxOceanodeTimelineClip& clip) {
    using namespace ofxOceanodeTimelineClipTime;
    const double content = sourceDuration(clip);
    if(!clip.repeatContent) return lfoCyclesStarted(clip, content);
    const double duration = std::max(1.0 / 24.0, clip.durationBeats);
    const double repeatLength = cycleDuration(clip);
    const double fullRepeats = std::floor(duration / repeatLength + 1e-9);
    const double partialSource = (duration - fullRepeats * repeatLength) / stretch(clip);
    return fullRepeats * lfoCyclesStarted(clip, content) + lfoCyclesStarted(clip, partialSource);
}
} // namespace

ofxOceanodeTimelineLfoSample ofxOceanodeTimelineManager::evaluateLfoTrack(const std::string& trackId,
                                                                         double beat) const {
    ofxOceanodeTimelineLfoSample sample;
    const auto* track = getTrack(trackId);
    if(track == nullptr || !isTrackAudible(*track)) return sample;

    std::vector<const ofxOceanodeTimelineClip*> clips;
    for(const auto& clip : track->clips) if(clip.isLfo) clips.push_back(&clip);
    std::sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) { return a->startBeat < b->startBeat; });

    double cyclesBefore = 0.0;
    for(const auto* clip : clips) {
        double sourceBeat = 0.0;
        if(clipSourceBeat(*clip, playContext, beat, sourceBeat)) {
            // Earlier repeats of this clip, then the position inside the current one.
            const int64_t repeat = ofxOceanodeTimelineClipTime::cycleIndex(*clip, beat);
            cyclesBefore += static_cast<double>(repeat) *
                lfoCyclesStarted(*clip, ofxOceanodeTimelineClipTime::sourceDuration(*clip));
            const double cycles = lfoCycles(*clip, sourceBeat);
            const double whole = std::floor(cycles);
            sample.active = true;
            sample.cycle = cyclesBefore + whole;
            sample.phase = static_cast<float>(cycles - whole);
            sample.value = ofxOceanodeTimelineLfo::evaluate(*clip, sourceBeat);
            return sample;
        }
        if(beat < clip->startBeat) break; // in a gap before this clip
        cyclesBefore += lfoClipCyclesStarted(*clip);
    }
    sample.cycle = cyclesBefore;
    return sample;
}

// Evaluates every binding of every track at one beat, without touching any
// parameter. evaluateAutomation() calls it for the current playhead;
// runScheduler() calls it for beats that have not been reached yet, which is
// what lets discrete events be handed to a timestamping backend early without
// a second, divergent implementation of the lane semantics.
void ofxOceanodeTimelineManager::collectActiveValues(const ofxOceanodeTimelinePlayContext& context,
                                                     double beatPosition, bool isPlaying,
                                                     bool applyPianoRanges,
                                                     ActiveValueMap& activeValues,
                                                     ofxOceanodeTimelineTargetInspection* inspection,
                                                     const std::set<std::string>* onlyPaths) const {
    if(container == nullptr) return;
    if(!evaluationIndexesValid)
        const_cast<ofxOceanodeTimelineManager*>(this)->rebuildEvaluationIndexes();
    activeValues.clear();
    activeValues.reserve(bindingsByPathCache.size());
    using LaneContributions = std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>>;
    using ClipContributions = std::unordered_map<const ofxOceanodeTimelineClip*, LaneContributions>;
    std::unordered_map<const ofxOceanodeTimelineParameterBinding*, ClipContributions> bindingClipValues;
    bindingClipValues.reserve(bindingsByPathCache.size());

    const ofxOceanodeTimelineTrack* contributionTrack = nullptr;
    const ofxOceanodeTimelineClip* contributionClip = nullptr;
    std::vector<std::string> contributionLanes;
    auto contribute = [&](const ofxOceanodeTimelineParameterBinding* binding, std::string value) {
        if(inspection != nullptr && binding->parameterPath == inspection->parameterPath)
            inspection->contributions.push_back({contributionTrack->id, binding->id,
                contributionClip->id, contributionLanes, value, false});
        bindingClipValues[binding][contributionClip].emplace_back(binding->mode, std::move(value));
    };

    auto bindingFor = [&](const ofxOceanodeTimelineTrack& track,
                          const std::string& bindingId) -> const ofxOceanodeTimelineParameterBinding* {
        if(bindingId.empty()) return nullptr;
        const auto trackIt = trackEvaluationIndexes.find(&track);
        if(trackIt == trackEvaluationIndexes.end()) return bindingInTrack(track, bindingId);
        const auto bindingIt = trackIt->second.bindingById.find(bindingId);
        return bindingIt == trackIt->second.bindingById.end() ? nullptr : bindingIt->second;
    };

    if(applyPianoRanges) {
        for(const auto& entry : pianoPitchRangesCache) {
            if(auto* parameter = findParameterCached(entry.first)) {
                if(savedPianoRanges.count(entry.first) == 0) {
                    // Remember the parameter's own range before widening it.
                    SavedParameterRange saved;
                    saved.valueType = entry.second.valueType;
                    const auto& type = entry.second.valueType;
                    if(type == typeid(float).name()) {
                        saved.floatMin = parameter->cast<float>().getParameter().getMin();
                        saved.floatMax = parameter->cast<float>().getParameter().getMax();
                    } else if(type == typeid(int).name()) {
                        saved.intMin = parameter->cast<int>().getParameter().getMin();
                        saved.intMax = parameter->cast<int>().getParameter().getMax();
                    } else if(type == typeid(std::vector<float>).name()) {
                        saved.floatVectorMin = parameter->cast<std::vector<float>>().getParameter().getMin();
                        saved.floatVectorMax = parameter->cast<std::vector<float>>().getParameter().getMax();
                    } else if(type == typeid(std::vector<int>).name()) {
                        saved.intVectorMin = parameter->cast<std::vector<int>>().getParameter().getMin();
                        saved.intVectorMax = parameter->cast<std::vector<int>>().getParameter().getMax();
                    }
                    savedPianoRanges[entry.first] = saved;
                }
                // Only ever widen the parameter's own range to fit the keys
                // (never narrow it: that clamped its cables and its slider).
                // Keys inside its own range: its own range, exactly.
                const auto& saved = savedPianoRanges[entry.first];
                const float low = entry.second.low, high = entry.second.high;
                const auto& type = entry.second.valueType;
                auto widen = [&](auto& target, const auto& ownMin, const auto& ownMax) {
                    using V = std::decay_t<decltype(ownMin)>;
                    V minimum = ownMin, maximum = ownMax;
                    if constexpr(std::is_arithmetic_v<V>) {
                        minimum = std::min<V>(ownMin, static_cast<V>(std::floor(low)));
                        maximum = std::max<V>(ownMax, static_cast<V>(std::ceil(high)));
                    } else {
                        using E = typename V::value_type;
                        for(auto& v : minimum) v = std::min<E>(v, static_cast<E>(std::floor(low)));
                        for(auto& v : maximum) v = std::max<E>(v, static_cast<E>(std::ceil(high)));
                    }
                    if(target.getMin() != minimum) target.setMin(minimum);
                    if(target.getMax() != maximum) target.setMax(maximum);
                };
                if(type == typeid(float).name())
                    widen(parameter->cast<float>().getParameter(), saved.floatMin, saved.floatMax);
                else if(type == typeid(int).name())
                    widen(parameter->cast<int>().getParameter(), saved.intMin, saved.intMax);
                else if(type == typeid(std::vector<float>).name())
                    widen(parameter->cast<std::vector<float>>().getParameter(), saved.floatVectorMin, saved.floatVectorMax);
                else if(type == typeid(std::vector<int>).name())
                    widen(parameter->cast<std::vector<int>>().getParameter(), saved.intVectorMin, saved.intVectorMax);
            }
        }
        // Give back the original range of parameters no piano roll drives any more.
        for(auto it = savedPianoRanges.begin(); it != savedPianoRanges.end();) {
            if(pianoPitchRangesCache.find(it->first) != pianoPitchRangesCache.end()) { ++it; continue; }
            if(auto* parameter = findParameterCached(it->first)) {
                const auto& saved = it->second;
                const auto& type = saved.valueType;
                if(type == typeid(float).name()) {
                    auto& p = parameter->cast<float>().getParameter();
                    p.setMin(saved.floatMin); p.setMax(saved.floatMax);
                } else if(type == typeid(int).name()) {
                    auto& p = parameter->cast<int>().getParameter();
                    p.setMin(saved.intMin); p.setMax(saved.intMax);
                } else if(type == typeid(std::vector<float>).name()) {
                    auto& p = parameter->cast<std::vector<float>>().getParameter();
                    p.setMin(saved.floatVectorMin); p.setMax(saved.floatVectorMax);
                } else if(type == typeid(std::vector<int>).name()) {
                    auto& p = parameter->cast<std::vector<int>>().getParameter();
                    p.setMin(saved.intVectorMin); p.setMax(saved.intVectorMax);
                }
            }
            it = savedPianoRanges.erase(it);
        }
    }
    const auto wantsBinding = [&](const ofxOceanodeTimelineParameterBinding* binding) {
        return binding != nullptr && (onlyPaths == nullptr || onlyPaths->count(binding->parameterPath) > 0);
    };
    // Each sequence heard at this beat, in its own beats and with the rolls
    // of its own copy.
    for(const auto& placement : playedSequences(beatPosition, beatPosition)) {
        const double beat = placement.map.toSequence(beatPosition);
        auto copyContext = context;
        copyContext.instanceKey = placement.instanceKey;
        const uint64_t copyEpoch = copyContext.epoch();
        for(const auto& track : placement.sequence->tracks) {
            if(!isTrackAudible(track)) continue;
            contributionTrack = &track;
            for(const auto& clip : track.clips) {
                contributionClip = &clip;
                contributionLanes.clear();
                double sourceBeat = 0.0;
                if(!clipSourceBeat(clip, copyContext, beat, sourceBeat)) {
                    // A clip its chance roll left out plays as an empty pass: the
                    // lanes that rest at 0 between their events (gates, toggles,
                    // steps, value regions) write that rest, instead of holding
                    // whatever the previous pass ended on through this one.
                    // Curves, numbers, dropdowns and sliders have no rest and hold,
                    // as they do between clips; piano gates already rest at 0.
                    if(!ofxOceanodeTimelineClipChanceSilenced(clip, copyContext) || clip.muted || clip.isLfo || !clipCoversBeat(clip, beat)) continue;
                    for(const auto& lane : clip.lanes) {
                        const bool multiRow = lane.type == ofxOceanodeTimelineLaneType::MultiGate ||
                                              lane.type == ofxOceanodeTimelineLaneType::MultiValue;
                        if((!multiRow && lane.type != ofxOceanodeTimelineLaneType::Step) || !laneAudible(clip, lane)) continue;
                        if(inspection != nullptr) contributionLanes = {lane.id};
                        const std::string rest = multiRow
                            ? joinAutomationValues(std::vector<std::string>(static_cast<size_t>(std::max(1, lane.multiRowCount)), "0"))
                            : "0";
                        for(const auto& bindingId : lane.bindingIds) {
                            const auto* binding = bindingFor(track, bindingId);
                            if(wantsBinding(binding) && !binding->bypass)
                                contribute(binding, multiRow ? rest : mapNormalizedLaneValue(lane, *binding, rest));
                        }
                    }
                    continue;
                }
                if(clip.isLfo) {
                    const auto* binding = bindingFor(track, clip.lfoOutputBindingId);
                    if(wantsBinding(binding) && !binding->bypass) {
                        const float normalized = ofxOceanodeTimelineLfo::evaluate(clip, sourceBeat);
                        const float mapped = clip.lfoOutputMin + normalized *
                            (clip.lfoOutputMax - clip.lfoOutputMin);
                        std::string value = binding->valueType == typeid(int).name()
                            ? automationText::formatInt(std::lround(mapped)) : automationText::formatFloat(mapped);
                        contribute(binding, std::move(value));
                    }
                    continue;
                }
                for(const auto& sourceLane : clip.lanes) {
                    if(!laneAudible(clip, sourceLane)) continue;
                    if(onlyPaths != nullptr) {
                        bool wanted = false;
                        for(const auto& id : sourceLane.bindingIds)
                            wanted = wanted || wantsBinding(bindingFor(track, id));
                        for(const auto* id : {&sourceLane.pianoPitchBindingId, &sourceLane.pianoVelocityBindingId, &sourceLane.pianoGateBindingId,
                                              &sourceLane.drumDurationBindingId, &sourceLane.drumFilterBindingId,
                                              &sourceLane.drumStutterBindingId})
                            wanted = wanted || wantsBinding(bindingFor(track, *id));
                        if(!wanted) continue;
                    }
                    // Stopped piano rolls leave pitch, gate and velocity to live input.
                    if(!isPlaying && ofxOceanodeTimelineLaneUsesNoteRoles(sourceLane.type)) continue;
                    const SharedPianoPlayback* shared = nullptr;
                    if(const auto trackIt = trackEvaluationIndexes.find(&track); trackIt != trackEvaluationIndexes.end()) {
                        const auto it = trackIt->second.sharedPiano.find(&sourceLane);
                        if(it != trackIt->second.sharedPiano.end()) shared = it->second.get();
                    }
                    if(shared != nullptr && shared->first != &sourceLane) continue;
                    const auto& lane = shared == nullptr ? sourceLane : shared->lane;
                    if(inspection != nullptr) {
                        contributionLanes = {sourceLane.id};
                        if(shared != nullptr) {
                            contributionLanes.clear();
                            for(const auto* noteLane : shared->noteLanes)
                                if(std::find(contributionLanes.begin(), contributionLanes.end(), noteLane->id) == contributionLanes.end())
                                    contributionLanes.push_back(noteLane->id);
                        }
                    }
                    if(lane.type == ofxOceanodeTimelineLaneType::Wave) {
                        // Real audio, not automation -- never touches a
                        // binding/parameter at all. Handled by the dedicated
                        // wave-audio-provider pass below instead.
                        continue;
                    } else if(lane.type == ofxOceanodeTimelineLaneType::Step ||
                              lane.type == ofxOceanodeTimelineLaneType::Curve ||
                              ofxOceanodeTimelineIsHeldValueLane(lane.type) ||
                              lane.type == ofxOceanodeTimelineLaneType::MultiSlider) {
                        std::string value;
                        const bool hasValue = lane.type == ofxOceanodeTimelineLaneType::Step
                            ? evaluateStepSequencer(lane, sourceBeat, value, laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip),
                                                    ofxOceanodeTimelineClipTime::cycleIndex(clip, beat),
                                                    ofxOceanodeTimelineClipTime::sourceDuration(clip), copyEpoch)
                            : lane.type == ofxOceanodeTimelineLaneType::MultiSlider
                            ? evaluateMultiSlider(lane, sourceBeat, value)
                            : evaluateCurve(lane, sourceBeat, value);
                        if(!hasValue && lane.type != ofxOceanodeTimelineLaneType::Step) continue;
                        if(!hasValue) value = "0";
                        for(const auto& bindingId : lane.bindingIds) {
                            if(const auto* binding = bindingFor(track, bindingId)) {
                                // A bypassed binding shouldn't contribute to a
                                // shared parameter's combined value at all.
                                if(binding->bypass) continue;
                                contribute(binding, ofxOceanodeTimelineIsHeldValueLane(lane.type)
                                        ? (binding->valueType == typeid(int).name() ||
                                           lane.type == ofxOceanodeTimelineLaneType::Dropdown
                                            ? automationText::formatInt(std::lround(automationText::parse(value))) : value)
                                        : mapNormalizedLaneValue(lane, *binding, value));
                            }
                        }
                    } else if(lane.type == ofxOceanodeTimelineLaneType::MultiValue ||
                              lane.type == ofxOceanodeTimelineLaneType::MultiGate) {
                        // Unlike Step/Curve/PianoRoll (one lane -> one scalar
                        // stream, optionally broadcast to a vector target),
                        // these are inherently multi-component: row i always
                        // feeds vector component i of whatever this lane's
                        // bindings point at, joined into one comma-separated
                        // value exactly like the piano roll's own vector-target
                        // path below already does for pitch/gate/velocity.
                        const int rowCount = std::max(1, lane.multiRowCount);
                        std::vector<std::string> components;
                        components.reserve(rowCount);
                        // Gates: the block each row is in (NaN = none), for the
                        // per-target rolls below.
                        std::vector<double> blockStarts;
                        for(int row = 0; row < rowCount; ++row) {
                            if(lane.type == ofxOceanodeTimelineLaneType::MultiValue) {
                                float rowValue = 0.0f;
                                const bool active = row < static_cast<int>(lane.multiValueRows.size()) &&
                                    evaluateMultiValueRow(lane.multiValueRows[row], sourceBeat, rowValue);
                                components.push_back(active ? automationText::formatFloat(rowValue) : "0");
                            } else {
                                float velocity = 1.0f;
                                double blockStart = 0.0;
                                const bool active = row < static_cast<int>(lane.multiGateRows.size()) &&
                                    evaluateMultiGateRow(lane.multiGateRows[row], sourceBeat,
                                                         laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip),
                                                         clip.repeatContent ? ofxOceanodeTimelineClipTime::sourceDuration(clip) : 0.0,
                                                         &velocity, &blockStart);
                                components.push_back(!active ? "0" : velocity >= 1.0f ? "1" : automationText::formatFloat(velocity));
                                blockStarts.push_back(active ? blockStart : std::numeric_limits<double>::quiet_NaN());
                            }
                        }
                        const std::string value = joinAutomationValues(components);
                        for(const auto& bindingId : lane.bindingIds) {
                            if(const auto* binding = bindingFor(track, bindingId)) {
                                if(binding->bypass) continue;
                                const auto chance = lane.type == ofxOceanodeTimelineLaneType::MultiGate
                                    ? lane.targetProbability.find(binding->parameterPath) : lane.targetProbability.end();
                                if(chance == lane.targetProbability.end() || chance->second >= 1.0f) {
                                    contribute(binding, value);
                                    continue;
                                }
                                // Blocks this target's roll leaves out rest at 0.
                                auto reached = components;
                                const int64_t cycle = ofxOceanodeTimelineClipTime::cycleIndex(clip, beat);
                                for(int row = 0; row < rowCount; ++row) {
                                    if(std::isnan(blockStarts[row])) continue;
                                    if(!toggleTargetPasses(lane, binding->parameterPath, chance->second,
                                                           blockStarts[row], row, cycle, copyEpoch)) reached[row] = "0";
                                }
                                contribute(binding, joinAutomationValues(reached));
                            }
                        }
                    } else if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid) {
                        // Drum Grid: row r drives component row.slot of the
                        // instrument's Gate (its velocity while a hit is open)
                        // and Pitch (60 + the step's offset, held after the gate
                        // closes so a tail keeps its tuning).
                        const int64_t repeat = ofxOceanodeTimelineClipTime::cycleIndex(clip, beat);
                        const double content = ofxOceanodeTimelineClipTime::sourceDuration(clip);
                        const double gap = laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip);
                        int instrumentVoices = 0;
                        if(const auto trackIt = trackEvaluationIndexes.find(&track); trackIt != trackEvaluationIndexes.end()) {
                            const auto it = trackIt->second.drumOutputWidth.find(&lane);
                            if(it != trackIt->second.drumOutputWidth.end()) instrumentVoices = it->second;
                            else { int voices = 0; bool kit = false; pianoLaneVoicesAndKit(track, lane, voices, kit, &instrumentVoices); }
                        }
                        int width = std::max(1, instrumentVoices);
                        for(const auto& row : lane.drumRows) width = std::max(width, row.slot + 1);
                        std::vector<float> gates(static_cast<size_t>(width), 0.0f);
                        std::vector<int> pitches(static_cast<size_t>(width), ofxOceanodeTimelineDrumGrid::kBasePitch);
                        std::vector<float> durations(static_cast<size_t>(width), 1.0f);
                        std::vector<float> filters(static_cast<size_t>(width), 0.0f);
                        std::vector<float> stutters(static_cast<size_t>(width), 0.0f);
                        std::vector<uint8_t> pitchSet(static_cast<size_t>(width), 0);
                        float scalarGate = 0.0f, scalarDuration = 1.0f, scalarFilter = 0.0f, scalarStutter = 0.0f;
                        int scalarPitch = ofxOceanodeTimelineDrumGrid::kBasePitch;
                        bool scalarSounding = false;
                        for(size_t r = 0; r < lane.drumRows.size(); ++r) {
                            const auto& row = lane.drumRows[r];
                            float gate = 0.0f;
                            int pitch = ofxOceanodeTimelineDrumGrid::kBasePitch;
                            float duration = 1.0f, filter = 0.0f, stutter = 0.0f;
                            const bool open = ofxOceanodeTimelineDrumGrid::rowStateAt(lane, row, sourceBeat, repeat, content,
                                                                                      gap, copyEpoch, gate, pitch, &duration, &filter, &stutter) &&
                                              isPlaying && ofxOceanodeTimelineDrumGrid::rowAudible(lane, row);
                            const size_t slot = static_cast<size_t>(std::max(0, row.slot));
                            if(open) gates[slot] = std::max(gates[slot], gate);
                            if(!pitchSet[slot] || open) { pitches[slot] = pitch; durations[slot] = duration; filters[slot] = filter; stutters[slot] = stutter; pitchSet[slot] = 1; }
                            if(r == 0) { scalarPitch = pitch; scalarDuration = duration; scalarFilter = filter; scalarStutter = stutter; }
                            if(open && !scalarSounding) {
                                scalarSounding = true; scalarGate = gate; scalarPitch = pitch; scalarDuration = duration; scalarFilter = filter;
                                scalarStutter = stutter;
                            }
                        }
                        // Role 0 pitch, 1 gate, 2 duration, 3 filter (held like the pitch;
                        // before any hit 1 and 0).
                        const std::string roleBindingIds[] = {lane.pianoPitchBindingId, lane.pianoGateBindingId,
                                                              lane.drumDurationBindingId, lane.drumFilterBindingId,
                                                              lane.drumStutterBindingId};
                        auto heldText = [&](size_t role, size_t slot) {
                            if(role == 2) return slot < durations.size() ? automationText::formatFloat(durations[slot]) : std::string("1");
                            if(role == 3) return slot < filters.size() ? automationText::formatFloat(filters[slot]) : std::string("0");
                            if(role == 4) return slot < stutters.size() ? automationText::formatFloat(stutters[slot]) : std::string("0");
                            return automationText::formatInt(slot < pitches.size() ? pitches[slot] : ofxOceanodeTimelineDrumGrid::kBasePitch);
                        };
                        for(size_t role = 0; role < 5; ++role) {
                            const auto* binding = bindingFor(track, roleBindingIds[role]);
                            if(binding == nullptr || binding->bypass) continue;
                            const auto& type = binding->valueType;
                            const bool vectorTarget = type == typeid(std::vector<float>).name() ||
                                type == typeid(std::vector<int>).name() ||
                                type == typeid(std::vector<bool>).name() ||
                                type == typeid(std::vector<std::string>).name();
                            const bool levelGate = type == typeid(float).name() || type == typeid(std::vector<float>).name();
                            auto gateText = [&](float level) -> std::string {
                                if(level <= 0.0f) return "0";
                                return !levelGate || level >= 1.0f ? std::string("1") : automationText::formatFloat(level);
                            };
                            std::string value;
                            if(vectorTarget) {
                                // As wide as the target already is, so nothing broadcasts.
                                size_t outputWidth = static_cast<size_t>(width);
                                if(auto* parameter = findParameterCached(binding->parameterPath)) {
                                    if(type == typeid(std::vector<float>).name())
                                        outputWidth = std::max(outputWidth, parameter->cast<std::vector<float>>().getParameter().get().size());
                                    else if(type == typeid(std::vector<int>).name())
                                        outputWidth = std::max(outputWidth, parameter->cast<std::vector<int>>().getParameter().get().size());
                                }
                                std::vector<std::string> slotValues;
                                slotValues.reserve(std::max<size_t>(2, outputWidth));
                                for(size_t slot = 0; slot < outputWidth; ++slot) {
                                    if(role == 1) slotValues.push_back(slot < static_cast<size_t>(width) ? gateText(gates[slot]) : std::string("0"));
                                    else slotValues.push_back(heldText(role, slot));
                                }
                                if(slotValues.size() == 1) slotValues.push_back(role == 1 ? std::string("0") : heldText(role, slotValues.size()));
                                value = joinAutomationValues(slotValues);
                            } else {
                                value = role == 1 ? gateText(scalarSounding ? scalarGate : 0.0f)
                                      : role == 2 ? automationText::formatFloat(scalarDuration)
                                      : role == 3 ? automationText::formatFloat(scalarFilter)
                                      : role == 4 ? automationText::formatFloat(scalarStutter) : automationText::formatInt(scalarPitch);
                            }
                            contribute(binding, std::move(value));
                        }
                    } else {
                        // Piano roll: every note plays in its own voice slot (see
                        // computePianoVoicing), so a held note keeps its voice.
                        ofxOceanodeTimelinePianoVoicing localVoicing;
                        const ofxOceanodeTimelinePianoVoicing* voicing = shared == nullptr ? nullptr : &shared->voicing;
                        if(const auto trackIt = trackEvaluationIndexes.find(&track); trackIt != trackEvaluationIndexes.end()) {
                            const auto it = trackIt->second.pianoVoicing.find(&lane);
                            if(it != trackIt->second.pianoVoicing.end() && it->second.slot.size() == lane.pianoNotes.size())
                                voicing = &it->second;
                        }
                        if(voicing == nullptr) {
                            // Notes edited since the indexes were built.
                            computeLaneVoicing(track, clip, lane, localVoicing);
                            voicing = &localVoicing;
                        }
                        const int64_t pianoRepeat = ofxOceanodeTimelineClipTime::cycleIndex(clip, beat);
                        const double pianoCycle = static_cast<double>(pianoRepeat);
                        // With chance notes, only those sounding in this repeat
                        // hold a voice: a failed note neither cuts the one before
                        // it nor moves the slot's pitch. Mutating notes play as
                        // this repeat changed them.
                        const auto* playedNotes = &lane.pianoNotes;
                        if(voicing->chance) {
                            const auto& played = repeatPlayback(copyContext, track, clip, lane, shared, pianoRepeat);
                            voicing = &played.voicing;
                            if(played.mutated) playedNotes = &played.lane.pianoNotes;
                        }
                        const auto& notes = *playedNotes;
                        const double noteGap = laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip);
                        // Where a note's gate opens: after the retrigger gap when
                        // its slot was still busy, so the instrument sees an edge.
                        auto openAt = [&](size_t n) {
                            const bool gap = voicing->retrigger[n] || (voicing->wrapRetrigger[n] && pianoCycle > 0.0);
                            return notes[n].startBeat + (gap ? noteGap : 0.0);
                        };
                        const int slotCount = static_cast<int>(voicing->slotNotes.size());
                        const int width = voicing->width;
                        // Per slot: the note sounding now (-1: none), and the note
                        // whose pitch/velocity the slot holds (the last one opened;
                        // before any, the previous repeat's last or the first one).
                        std::vector<long long> sounding(width, -1), held(width, -1);
                        for(int slot = 0; slot < std::min(width, slotCount); ++slot) {
                            const auto& slotNotes = voicing->slotNotes[slot];
                            if(slotNotes.empty()) continue;
                            // In start order, and a note never opens before it
                            // starts: skip straight past every note starting later.
                            const auto startsLater = std::upper_bound(slotNotes.begin(), slotNotes.end(), sourceBeat,
                                [&](double beat, size_t n) { return beat < notes[n].startBeat; });
                            for(auto it = std::make_reverse_iterator(startsLater); it != slotNotes.rend(); ++it) {
                                if(openAt(*it) > sourceBeat) continue;
                                held[slot] = static_cast<long long>(*it);
                                if(isPlaying && sourceBeat < voicing->end[*it] &&
                                   pianoProbabilityPasses(lane.pianoNotes[*it], pianoCycle,
                                       shared == nullptr ? lane.probabilitySeed : shared->noteLanes[*it]->probabilitySeed,
                                       copyEpoch))
                                    sounding[slot] = static_cast<long long>(*it);
                                break;
                            }
                            if(held[slot] < 0)
                                held[slot] = static_cast<long long>(clip.repeatContent ? slotNotes.back() : slotNotes.front());
                        }
                        // A scalar target plays the latest note (mono, last-note priority).
                        long long latestSounding = -1, latestHeld = -1;
                        for(int slot = 0; slot < width; ++slot) {
                            auto later = [&](long long a, long long b) {
                                return b < 0 || (a >= 0 && openAt(static_cast<size_t>(a)) > openAt(static_cast<size_t>(b)));
                            };
                            if(sounding[slot] >= 0 && later(sounding[slot], latestSounding)) latestSounding = sounding[slot];
                            if(held[slot] >= 0 && later(held[slot], latestHeld)) latestHeld = held[slot];
                        }

                        // An instrument's gate is a level: without a velocity input
                        // it carries the velocity. Other targets get 1 (a trigger).
                        const bool gateCarriesVelocity = voicing->instrument &&
                            bindingFor(track, lane.pianoVelocityBindingId) == nullptr;
                        const std::string roleBindingIds[] = {
                            lane.pianoPitchBindingId,
                            lane.pianoGateBindingId,
                            lane.pianoVelocityBindingId
                        };
                        for(size_t role = 0; role < 3; ++role) {
                            const auto* binding = bindingFor(track, roleBindingIds[role]);
                            if(binding == nullptr || binding->bypass) continue;
                            const auto& type = binding->valueType;
                            const bool vectorTarget = type == typeid(std::vector<float>).name() ||
                                type == typeid(std::vector<int>).name() ||
                                type == typeid(std::vector<bool>).name() ||
                                type == typeid(std::vector<std::string>).name();
                            const bool levelGate = gateCarriesVelocity &&
                                (type == typeid(float).name() || type == typeid(std::vector<float>).name());
                            // One slot's value for this role.
                            auto valueOf = [&](long long soundingNote, long long heldNote) -> std::string {
                                if(role == 1) {
                                    if(soundingNote < 0) return "0";
                                    return levelGate ? automationText::formatFloat(notes[soundingNote].velocity) : std::string("1");
                                }
                                if(heldNote < 0) return std::string();
                                return role == 0 ? automationText::formatInt(notes[heldNote].pitch) : automationText::formatFloat(notes[heldNote].velocity);
                            };
                            std::string value;
                            if(vectorTarget) {
                                // As wide as the target already is, so a short list
                                // (a mono lane on a 4-voice synth) is never broadcast
                                // to every voice: the extra voices stay closed.
                                size_t outputWidth = static_cast<size_t>(std::max(width, voicing->outputWidth));
                                if(auto* parameter = findParameterCached(binding->parameterPath)) {
                                    if(type == typeid(std::vector<float>).name())
                                        outputWidth = std::max(outputWidth, parameter->cast<std::vector<float>>().getParameter().get().size());
                                    else if(type == typeid(std::vector<int>).name())
                                        outputWidth = std::max(outputWidth, parameter->cast<std::vector<int>>().getParameter().get().size());
                                }
                                std::vector<std::string> slotValues;
                                slotValues.reserve(outputWidth);
                                for(size_t slot = 0; slot < outputWidth; ++slot) {
                                    std::string slotValue = slot < static_cast<size_t>(width)
                                        ? valueOf(sounding[slot], held[slot]) : std::string(role == 1 ? "0" : "");
                                    // A slot with no notes: like slot 0 (nothing sounds there).
                                    if(slotValue.empty()) slotValue = slotValues.empty() ? (role == 0 ? "60" : "0") : slotValues.front();
                                    slotValues.push_back(std::move(slotValue));
                                }
                                if(slotValues.size() == 1) slotValues.push_back(role == 1 ? "0" : slotValues.front());
                                value = joinAutomationValues(slotValues);
                            } else {
                                value = valueOf(latestSounding, latestSounding >= 0 ? latestSounding : latestHeld);
                                if(value.empty()) continue;
                                if(role == 2) {
                                    const auto note = latestSounding >= 0 ? latestSounding : latestHeld;
                                    const auto& valueLane = shared != nullptr && note >= 0
                                        ? *shared->noteLanes[static_cast<size_t>(note)] : lane;
                                    value = mapNormalizedLaneValue(valueLane, *binding, value);
                                }
                            }
                            contribute(binding, std::move(value));
                        }
                    }
                }
            }
        }
    }

    // Lanes inside one clip use the binding's blend mode (for example, a
    // Multiply binding shared by a Curve and a piano Gate evaluates as
    // curve * gate). Where clips on one row overlap, the last one wins: the
    // clip that starts latest (then the later one in the track) is the one
    // heard. Finally, expose one contribution per binding for
    // cross-track/cross-binding blending.
    for(const auto& track : mainSequence.tracks) {
        const auto trackIndexIt = trackEvaluationIndexes.find(&track);
        for(const auto& binding : track.bindings) {
            if(!wantsBinding(&binding)) continue;
            const bool inspectBinding = inspection != nullptr && binding.parameterPath == inspection->parameterPath;
            if(inspectBinding) inspection->bindings.push_back({track.id, binding.id, {}, binding.mode,
                binding.bypass, binding.hasLiveOverride, false});
            if(binding.bypass) continue;
            const auto clipsIt = bindingClipValues.find(&binding);
            std::string value;
            if(binding.hasLiveOverride) value = binding.liveOverrideValue;
            else if(clipsIt != bindingClipValues.end()) {
                const LaneContributions* winner = nullptr;
                const ofxOceanodeTimelineClip* winningClip = nullptr;
                std::pair<double, size_t> winnerOrder{-1.0, 0};
                for(const auto& clipEntry : clipsIt->second) {
                    size_t clipIndex = 0;
                    if(trackIndexIt != trackEvaluationIndexes.end()) {
                        const auto orderIt = trackIndexIt->second.clipOrder.find(clipEntry.first);
                        if(orderIt != trackIndexIt->second.clipOrder.end()) clipIndex = orderIt->second;
                    }
                    const std::pair<double, size_t> order{clipEntry.first->startBeat, clipIndex};
                    if(winner == nullptr || order > winnerOrder) {
                        winner = &clipEntry.second;
                        winningClip = clipEntry.first;
                        winnerOrder = order;
                    }
                }
                if(winner != nullptr) value = combineAutomationValues(*winner, binding.valueType);
                if(inspectBinding && winningClip != nullptr)
                    for(auto& contribution : inspection->contributions)
                        if(contribution.trackId == track.id && contribution.bindingId == binding.id)
                            contribution.winningClip = contribution.clipId == winningClip->id;
                if(value.empty()) continue;
            }
            else if(isPlaying && zeroWhenInactiveBindingsCache.count(&binding) > 0) value = "0";
            else continue;
            if(inspectBinding) {
                inspection->bindings.back().value = value;
                inspection->bindings.back().active = true;
            }
            activeValues[binding.parameterPath].emplace_back(binding.mode, std::move(value));
        }
    }

}

// Resolves the value one parameter should hold, given every binding that
// points at it and a set of evaluated lane contributions. Shared by
// applyAutomation() (this frame's value) and by the scheduler (the value at a
// beat that has not been reached yet) so the two can never disagree.
bool ofxOceanodeTimelineManager::computeParameterValue(
        const std::vector<ofxOceanodeTimelineParameterBinding*>& bindings,
        const ActiveValueMap& activeValues,
        ofxOceanodeAbstractParameter* parameter,
        std::string& outValue) const {
    if(parameter == nullptr) return false;
    const ofxOceanodeTimelineParameterBinding* defaultBinding = nullptr;
    for(const auto* binding : bindings) {
        if(binding != nullptr && !binding->bypass) { defaultBinding = binding; break; }
    }
    if(defaultBinding == nullptr) return false;
    const auto valuesIt = activeValues.find(defaultBinding->parameterPath);
    // Outside every clip the parameter holds its last value: nothing is
    // written, so it can also be changed by hand between clips.
    if(valuesIt == activeValues.end()) return false;
    std::string value = combineAutomationValues(valuesIt->second, defaultBinding->valueType);
    value = broadcastScalarNumericVectorValue(*parameter, value, defaultBinding->valueType);
    if(defaultBinding->clampToParameterRange)
        value = clampValueToParameterRange(*parameter, value, defaultBinding->valueType);
    outValue = std::move(value);
    return true;
}

ofxOceanodeTimelineTargetInspection ofxOceanodeTimelineManager::inspectTarget(
        const std::string& path, double beat, bool isPlaying) const {
    ofxOceanodeTimelineTargetInspection result;
    result.parameterPath = path;
    if(container == nullptr) { result.missingTarget = true; return result; }
    // UI edits can change public model objects between updates. Rebuild only
    // derived indexes, never parameter ranges or the scheduler's state.
    const_cast<ofxOceanodeTimelineManager*>(this)->ensureEvaluationIndexes();
    ActiveValueMap values;
    collectActiveValues(playContext, beat, isPlaying, false, values, &result);
    auto* parameter = findParameterCached(path);
    result.missingTarget = parameter == nullptr;
    const auto it = bindingsByPathCache.find(path);
    if(it == bindingsByPathCache.end()) return result;
    const ofxOceanodeTimelineParameterBinding* authoritative = nullptr;
    for(const auto* binding : it->second)
        if(!binding->bypass) { authoritative = binding; break; }
    if(authoritative != nullptr) {
        result.clampEnabled = authoritative->clampToParameterRange;
        const auto active = values.find(path);
        if(active != values.end()) {
            result.unclampedValue = combineAutomationValues(active->second, authoritative->valueType);
            if(parameter != nullptr)
                result.unclampedValue = broadcastScalarNumericVectorValue(*parameter, result.unclampedValue, authoritative->valueType);
        }
    }
    result.writesValue = computeParameterValue(it->second, values, parameter, result.value);
    result.clamped = result.writesValue && result.value != result.unclampedValue;
    return result;
}


uint64_t ofxOceanodeTimelineManager::lanePlaybackSignature(const ofxOceanodeTimelineLane& lane) {
    ChangeHash hash;
    hash.add(lane.type);
    hash.add(lane.muted); hash.add(lane.solo); hash.add(lane.drivesTrackSolo);
    for(const auto& [path, probability] : lane.targetProbability) { hash.add(path); hash.add(probability); }
    hash.add(lane.bindingIds.size());
    for(const auto& id : lane.bindingIds) hash.add(id);
    hash.add(lane.step.steps.size());
    for(const auto& step : lane.step.steps) {
        hash.add(step.startBeat); hash.add(step.durationBeats); hash.add(step.value); hash.add(step.probability); hash.add(step.velocity);
    }
    hash.add(lane.stepCount); hash.add(lane.beatsPerStep); hash.add(lane.valueMin); hash.add(lane.valueMax);
    hash.add(lane.probabilityEnabled); hash.add(lane.behavior); hash.add(lane.probabilitySeed);
    hash.add(lane.pianoLowPitch); hash.add(lane.pianoHighPitch);
    hash.add(lane.pianoPitchBindingId); hash.add(lane.pianoGateBindingId); hash.add(lane.pianoVelocityBindingId);
    hash.add(lane.drumDurationBindingId); hash.add(lane.drumFilterBindingId); hash.add(lane.drumStutterBindingId);
    hash.add(lane.pianoMonophonic); hash.add(lane.probabilitySeed); hash.add(lane.mutationMonophonic); hash.add(lane.mutationDrift); hash.add(lane.mutationStrength);
    hash.add(lane.pianoScaleEnabled); hash.add(lane.pianoScaleMask); hash.add(lane.pianoScaleSnap);
    hash.add(lane.randomOctave); hash.add(lane.randomOctaveRange); hash.add(lane.randomOctaveDirection);
    hash.add(lane.randomPitch); hash.add(lane.randomPitchRange); hash.add(lane.randomPitchDirection);
    auto hashRules = [&](const ofxOceanodeTimelineMutationRules& rules) {
        hash.add(rules.pitchStrength); hash.add(rules.pitchRange); hash.add(rules.pitchDirection); hash.add(rules.scaleMask);
        hash.add(rules.offsetStrength); hash.add(rules.offsetSteps); hash.add(rules.lengthStrength); hash.add(rules.lengthSteps); hash.add(rules.lengthQuantum); hash.add(rules.lengthDirection);
        hash.add(rules.velocityStrength); hash.add(rules.velocityRange); hash.add(rules.offsetQuantum);
        hash.add(rules.offsetDirection); hash.add(rules.velocityDirection);
    };
    hashRules(lane.mutation);
    hash.add(lane.mutationGroups.size());
    for(const auto& group : lane.mutationGroups) { hashRules(group.rules); hash.add(group.overrides); }
    hash.add(lane.curveClamp); hash.add(lane.curveInterpolation); hash.add(lane.curveScale);
    hash.add(lane.curvePoints.size());
    for(const auto& point : lane.curvePoints) { hash.add(point.beat); hash.add(point.value); }
    hash.add(lane.curveTensions.size());
    for(const auto& tension : lane.curveTensions) { hash.add(tension.inflection); hash.add(tension.steepness); }
    hash.add(lane.pianoNotes.size());
    {
        // The bulk of most sessions. Four independent lanes so the multiplies
        // overlap instead of each waiting for the previous one.
        uint64_t lanes[4] = {1, 2, 3, 4};
        const auto& notes = lane.pianoNotes;
        for(size_t n = 0; n < notes.size(); ++n) {
            const auto& note = notes[n];
            const uint64_t a = ChangeHash::bits(note.startBeat) ^ ChangeHash::rotate(ChangeHash::bits(note.durationBeats), 29);
            const uint64_t b = (static_cast<uint64_t>(static_cast<uint32_t>(note.pitch)) << 32 | ChangeHash::bits(note.velocity)) ^
                               ChangeHash::rotate(ChangeHash::bits(note.probability), 13) ^
                               ChangeHash::rotate(ChangeHash::bits(note.mutation), 53) ^
                               (static_cast<uint64_t>(static_cast<uint32_t>(note.mutationGroup)) << 7);
            uint64_t& h = lanes[n & 3];
            h = (h ^ a ^ ChangeHash::rotate(b, 41)) * 0xBF58476D1CE4E5B9ULL;
        }
        for(const uint64_t h : lanes) hash.word(h);
    }
    hash.add(lane.multiRowCount);
    hash.add(lane.multiValueRows.size());
    for(const auto& row : lane.multiValueRows) {
        hash.add(row.size());
        for(const auto& region : row) { hash.add(region.startBeat); hash.add(region.durationBeats); hash.add(region.value); }
    }
    hash.add(lane.multiGateRows.size());
    for(const auto& row : lane.multiGateRows) {
        hash.add(row.size());
        for(const auto& region : row) { hash.add(region.startBeat); hash.add(region.durationBeats); hash.add(region.velocity); }
    }
    hash.add(lane.multiValueInteger);
    hash.add(lane.multiSliderValues.size());
    for(const float v : lane.multiSliderValues) hash.add(v);
    hash.add(lane.valueQuantizeSteps); hash.add(lane.valueSnap);
    hash.add(lane.lfoParameter); hash.add(lane.lfoValue);
    hash.add(lane.drumSwing); hash.add(lane.drumSwingBeats);
    hash.add(lane.drumRows.size());
    for(const auto& row : lane.drumRows) {
        hash.add(row.slot); hash.add(row.length); hash.add(row.mute); hash.add(row.solo);
        hash.add(row.steps.size());
        for(const auto& step : row.steps) {
            hash.add(step.beat); hash.add(step.length); hash.add(step.gate); hash.add(step.velocity); hash.add(step.pitch);
            hash.add(step.probability); hash.add(step.ratchet); hash.add(step.duration); hash.add(step.filter); hash.add(step.stutter);
        }
    }
    return hash.result();
}

// Everything rebuildEvaluationIndexes() reads: the model (editors write it in
// place, so it is hashed rather than tracked), where its containers live (the
// indexes hold pointers into them), and the instrument each piano lane plays
// (voice count and kit come from the node, not the timeline).
uint64_t ofxOceanodeTimelineManager::evaluationSignature() const {
    ChangeHash hash;
    hash.add(mainSequence.tracks.data()); hash.add(mainSequence.tracks.size());
    for(const auto& track : mainSequence.tracks) {
        hash.add(track.id); hash.add(track.isWaveTrack); hash.add(track.muted); hash.add(isTrackSoloed(track));
        hash.add(track.bindings.data()); hash.add(track.bindings.size());
        for(const auto& binding : track.bindings) {
            hash.add(binding.id); hash.add(binding.parameterPath); hash.add(binding.valueType);
            hash.add(binding.mode); hash.add(binding.bypass); hash.add(binding.hasLiveOverride);
        }
        hash.add(track.noteGroups.size());
        for(const auto& group : track.noteGroups) {
            hash.add(group.id); hash.add(group.pitchBindingId); hash.add(group.gateBindingId);
            hash.add(group.velocityBindingId); hash.add(group.durationBindingId); hash.add(group.filterBindingId); hash.add(group.stutterBindingId); hash.add(group.voices);
        }
        hash.add(track.clips.data()); hash.add(track.clips.size());
        for(const auto& clip : track.clips) {
            hash.add(clip.id); hash.add(clip.startBeat); hash.add(clip.durationBeats);
            hash.add(clip.contentDurationBeats); hash.add(clip.contentStretch); hash.add(clip.repeatContent); hash.add(clip.contentOffsetBeats);
            hash.add(clip.muted); hash.add(clip.chance); hash.add(clip.isLfo); hash.add(clip.isStateCapture); hash.add(clip.lfoOutputBindingId);
            hash.add(clip.lanes.data()); hash.add(clip.lanes.size());
            for(const auto& lane : clip.lanes) {
                hash.word(lanePlaybackSignature(lane));
                if(!ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)) continue;
                int voices = 0, instrumentVoices = 0;
                bool kit = false;
                pianoLaneVoicesAndKit(track, lane, voices, kit, &instrumentVoices);
                hash.add(voices); hash.add(kit); hash.add(instrumentVoices);
            }
        }
    }
    return hash.result();
}

void ofxOceanodeTimelineManager::ensureEvaluationIndexes() {
    const uint64_t signature = evaluationSignature();
    if(evaluationIndexesValid && signature == evaluationIndexesSignature) return;
    rebuildEvaluationIndexes();
    evaluationIndexesSignature = signature;
}

void ofxOceanodeTimelineManager::collectChangeBeats(const ofxOceanodeTimelinePlayContext& context,
                                                    double fromBeat, double toBeat,
                                                    bool inclusiveStart,
                                                    std::vector<double>& outBeats) const {
    constexpr size_t kMaximumChangeBeats = 4096;
    if(toBeat < fromBeat) return;
    auto addTimelineBeat = [&](double beat) {
        if(outBeats.size() >= kMaximumChangeBeats) return;
        const bool afterStart = inclusiveStart ? beat >= fromBeat - kEpsilon : beat > fromBeat + kEpsilon;
        if(!afterStart || beat > toBeat + kEpsilon) return;
        outBeats.push_back(beat);
    };

    // Only lanes driving a scheduled path can produce an event; the beats of
    // every other lane would each cost a full evaluation for nothing.
    const auto bindingScheduled = [&](const ofxOceanodeTimelineTrack& track, const std::string& bindingId) {
        if(bindingId.empty()) return false;
        const auto trackIt = trackEvaluationIndexes.find(&track);
        if(trackIt == trackEvaluationIndexes.end()) return true; // be conservative
        const auto it = trackIt->second.bindingById.find(bindingId);
        return it != trackIt->second.bindingById.end() && fastRetriggerPaths.count(it->second->parameterPath) > 0;
    };
    const auto laneScheduled = [&](const ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineLane& lane) {
        if(ofxOceanodeTimelineLaneUsesNoteRoles(lane.type))
            return bindingScheduled(track, lane.pianoPitchBindingId) || bindingScheduled(track, lane.pianoGateBindingId) ||
                   bindingScheduled(track, lane.drumDurationBindingId) || bindingScheduled(track, lane.drumFilterBindingId) ||
                   bindingScheduled(track, lane.drumStutterBindingId) ||
                   bindingScheduled(track, lane.pianoVelocityBindingId);
        return std::any_of(lane.bindingIds.begin(), lane.bindingIds.end(),
                           [&](const std::string& id) { return bindingScheduled(track, id); });
    };
    // Each sequence heard in the window, walked in its own beats; the beats
    // it reports go back to the main timeline through its map.
    for(const auto& placement : playedSequences(fromBeat, toBeat)) {
        const auto& map = placement.map;
        const double windowFrom = map.toSequence(fromBeat);
        const double windowTo = map.toSequence(toBeat);
        const auto addSequenceBeat = [&](double beat) { addTimelineBeat(map.toTimeline(beat)); };
        auto copyContext = context;
        copyContext.instanceKey = placement.instanceKey;
        const uint64_t copyEpoch = copyContext.epoch();
        for(const auto& track : placement.sequence->tracks) {
            if(track.isWaveTrack || !isTrackAudible(track)) continue; // audio, not automation
            for(const auto& clip : track.clips) {
                if(clip.muted || clip.isLfo) continue;
                if(std::none_of(clip.lanes.begin(), clip.lanes.end(),
                                [&](const auto& lane) { return laneScheduled(track, lane); })) continue;
                const double clipStart = clip.startBeat;
                const double clipDuration = std::max(1.0 / 24.0, clip.durationBeats);
                const double clipEnd = clipStart + clipDuration;
                // A clip edge is itself a change: automation stops (or starts)
                // contributing there.
                addSequenceBeat(clipStart);
                addSequenceBeat(clipEnd);
                if(clipEnd < windowFrom - kEpsilon || clipStart > windowTo + kEpsilon) continue;

                const double windowStart = std::max(windowFrom, clipStart);
                const double windowEnd = std::min(windowTo, clipEnd);
                if(windowEnd < windowStart) continue;
                const double sourceLength = ofxOceanodeTimelineClipTime::sourceDuration(clip);
                const double clipStretch = ofxOceanodeTimelineClipTime::stretch(clip);
                const double cycleLength = ofxOceanodeTimelineClipTime::cycleDuration(clip);

                // One pass per repeat cycle the window touches; each pass maps a
                // source-beat range back to timeline beats with the clip's own
                // conversion, so stretch/repeat live in exactly one place.
                const int64_t firstCycle = clip.repeatContent
                    ? static_cast<int64_t>(std::floor((windowStart - clipStart) / std::max(kEpsilon, cycleLength))) : 0;
                const int64_t lastCycle = clip.repeatContent
                    ? static_cast<int64_t>(std::floor((windowEnd - clipStart) / std::max(kEpsilon, cycleLength))) : 0;
                if(lastCycle - firstCycle > 64) continue; // pathological; frame path still covers it

                for(int64_t cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                    double sourceFrom = 0.0;
                    double sourceTo = sourceLength;
                    if(clip.repeatContent) {
                        sourceFrom = (windowStart - clipStart - static_cast<double>(cycle) * cycleLength) / clipStretch;
                        sourceTo = (windowEnd - clipStart - static_cast<double>(cycle) * cycleLength) / clipStretch;
                    } else {
                        sourceFrom = (windowStart - clipStart) * sourceLength / clipDuration;
                        sourceTo = (windowEnd - clipStart) * sourceLength / clipDuration;
                    }
                    sourceFrom = std::max(0.0, sourceFrom);
                    sourceTo = std::min(sourceLength, sourceTo);
                    if(sourceTo < sourceFrom) continue;
                    // Those are play positions in the repeat. With a content offset the
                    // content plays rotated and wraps at its end: walk it in one range,
                    // or two either side of the wrap.
                    const double contentOffset = ofxOceanodeTimelineClipTime::contentOffset(clip);
                    std::pair<double, double> contentRanges[2] = {{sourceFrom, sourceTo}, {0.0, -1.0}};
                    if(contentOffset > 0.0) {
                        double from = sourceFrom + contentOffset, to = sourceTo + contentOffset;
                        if(from >= sourceLength) { from -= sourceLength; to -= sourceLength; }
                        contentRanges[0] = {from, std::min(to, sourceLength)};
                        if(to > sourceLength) contentRanges[1] = {0.0, to - sourceLength};
                    }
                    for(const auto& contentRange : contentRanges) {
                        if(contentRange.second < contentRange.first) continue;
                        sourceFrom = contentRange.first;
                        sourceTo = contentRange.second;

                        auto addSourceBeat = [&](double sourceBeat) {
                            if(sourceBeat < sourceFrom - kEpsilon || sourceBeat > sourceTo + kEpsilon) return;
                            addSequenceBeat(ofxOceanodeTimelineClipTime::sourceToTimelineBeat(clip, sourceBeat, cycle));
                        };
                        // The start of a repetition restarts the pattern (and re-rolls
                        // per-cycle probabilities), so it is always a candidate. In a
                        // rotated clip it is the content at the offset, and content
                        // beat 0 (the wrap) restarts the pattern too.
                        addSourceBeat(0.0);
                        if(contentOffset > 0.0) addSourceBeat(contentOffset);

                        for(const auto& sourceLane : clip.lanes) {
                            if(!laneAudible(clip, sourceLane)) continue;
                            const SharedPianoPlayback* shared = nullptr;
                            if(const auto trackIt = trackEvaluationIndexes.find(&track); trackIt != trackEvaluationIndexes.end()) {
                                const auto it = trackIt->second.sharedPiano.find(&sourceLane);
                                if(it != trackIt->second.sharedPiano.end()) shared = it->second.get();
                            }
                            if(shared != nullptr && shared->first != &sourceLane) continue;
                            const auto& lane = shared == nullptr ? sourceLane : shared->lane;
                            if(!laneScheduled(track, lane)) continue;
                            switch(lane.type) {
                                case ofxOceanodeTimelineLaneType::Number:
                                case ofxOceanodeTimelineLaneType::Dropdown:
                                    for(const auto& point : lane.curvePoints) addSourceBeat(point.beat);
                                    break;
                                case ofxOceanodeTimelineLaneType::Curve:
                                case ofxOceanodeTimelineLaneType::Wave:
                                    // Continuous, or not automation at all.
                                    break;
                                case ofxOceanodeTimelineLaneType::Step: {
                                    const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
                                    const double patternLength = std::max(cellLength, lane.stepCount * cellLength);
                                    const int64_t firstPattern = static_cast<int64_t>(std::floor(sourceFrom / patternLength));
                                    const int64_t lastPattern = static_cast<int64_t>(std::floor(sourceTo / patternLength));
                                    if(lastPattern - firstPattern > 64) break;
                                    for(int64_t pattern = firstPattern; pattern <= lastPattern; ++pattern) {
                                        const double base = static_cast<double>(pattern) * patternLength;
                                        addSourceBeat(base);
                                        for(const auto& step : lane.step.steps) {
                                            const double start = std::max(0.0, step.startBeat);
                                            const double duration = step.durationBeats > kEpsilon ? step.durationBeats : cellLength;
                                            addSourceBeat(base + start);
                                            addSourceBeat(base + start + laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip));
                                            addSourceBeat(base + start + duration);
                                        }
                                    }
                                    break;
                                }
                                case ofxOceanodeTimelineLaneType::MultiSlider: {
                                    // Dense grid: every cell boundary is a change.
                                    const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
                                    const int64_t firstCell = static_cast<int64_t>(std::floor(sourceFrom / cellLength));
                                    const int64_t lastCell = static_cast<int64_t>(std::floor(sourceTo / cellLength)) + 1;
                                    if(lastCell - firstCell > 512) break;
                                    for(int64_t cell = firstCell; cell <= lastCell; ++cell)
                                        addSourceBeat(static_cast<double>(cell) * cellLength);
                                    break;
                                }
                                case ofxOceanodeTimelineLaneType::MultiValue: {
                                    for(const auto& row : lane.multiValueRows)
                                        for(const auto& region : row) {
                                            addSourceBeat(region.startBeat);
                                            addSourceBeat(region.end());
                                        }
                                    break;
                                }
                                case ofxOceanodeTimelineLaneType::MultiGate: {
                                    for(const auto& row : lane.multiGateRows)
                                        for(const auto& region : row) {
                                            addSourceBeat(region.startBeat);
                                            addSourceBeat(region.startBeat + laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip));
                                            addSourceBeat(region.end());
                                        }
                                    break;
                                }
                                case ofxOceanodeTimelineLaneType::DrumGrid: {
                                    // Every hit: its start (pitch), its gate opening
                                    // (after the retrigger gap) and its close.
                                    const double gap = laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip);
                                    std::vector<ofxOceanodeTimelineDrumHit> hits;
                                    for(const auto& row : lane.drumRows) {
                                        hits.clear();
                                        // Hits started before the window may open or close inside it.
                                        double reach = 0.75 * lane.drumSwingBeats;
                                        for(const auto& step : row.steps) reach = std::max(reach, step.length + 0.75 * lane.drumSwingBeats);
                                        ofxOceanodeTimelineDrumGrid::rowHits(lane, row, sourceFrom - reach - 1e-6,
                                                                             sourceTo, cycle, sourceLength, gap, copyEpoch, hits);
                                        for(const auto& hit : hits) {
                                            addSourceBeat(hit.start);
                                            addSourceBeat(hit.open);
                                            addSourceBeat(hit.close);
                                        }
                                    }
                                    break;
                                }
                                case ofxOceanodeTimelineLaneType::PianoRoll: {
                                    // A note stolen by a later one ends early (its voicing end).
                                    const ofxOceanodeTimelinePianoVoicing* voicing = shared == nullptr ? nullptr : &shared->voicing;
                                    if(const auto trackIt = trackEvaluationIndexes.find(&track); trackIt != trackEvaluationIndexes.end()) {
                                        const auto it = trackIt->second.pianoVoicing.find(&lane);
                                        if(it != trackIt->second.pianoVoicing.end() && it->second.end.size() == lane.pianoNotes.size())
                                            voicing = &it->second;
                                    }
                                    // Mutating notes move per repeat: this repeat's.
                                    const auto* notes = &lane.pianoNotes;
                                    if(voicing != nullptr && voicing->chance) {
                                        const auto& played = repeatPlayback(copyContext, track, clip, lane, shared, cycle);
                                        voicing = &played.voicing;
                                        if(played.mutated) notes = &played.lane.pianoNotes;
                                    }
                                    for(size_t n = 0; n < notes->size(); ++n) {
                                        const auto& note = (*notes)[n];
                                        addSourceBeat(note.startBeat);
                                        addSourceBeat(note.startBeat + laneRetriggerGapBeats(track, lane) * sourcePerTimelineBeat(clip));
                                        addSourceBeat(pianoNoteEnd(note));
                                        if(voicing != nullptr) addSourceBeat(voicing->end[n]);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::sort(outBeats.begin(), outBeats.end());
    outBeats.erase(std::unique(outBeats.begin(), outBeats.end(),
                               [](double a, double b) { return std::abs(a - b) <= kEpsilon; }),
                   outBeats.end());
}

void ofxOceanodeTimelineManager::releaseSilencedRests(const ofxOceanodeTransportState& transport) {
    std::unordered_map<std::string, std::string> sounding;
    std::unordered_set<std::string> silenced;
    if(transport.isPlaying) {
        for(const auto& track : mainSequence.tracks) {
            if(track.isWaveTrack) continue;
            const bool audible = isTrackAudible(track);
            for(const auto& clip : track.clips) {
                if(clip.isLfo || !clipCoversBeat(clip, transport.beatPosition)) continue;
                for(const auto& lane : clip.lanes) {
                    const bool multiRow = lane.type == ofxOceanodeTimelineLaneType::MultiGate ||
                                          lane.type == ofxOceanodeTimelineLaneType::MultiValue;
                    if(!multiRow && lane.type != ofxOceanodeTimelineLaneType::Step) continue;
                    const bool heard = audible && !clip.muted && laneAudible(clip, lane);
                    const std::string rest = multiRow
                        ? joinAutomationValues(std::vector<std::string>(static_cast<size_t>(std::max(1, lane.multiRowCount)), "0"))
                        : "0";
                    for(const auto& bindingId : lane.bindingIds) {
                        const auto* binding = getBinding(track.id, bindingId);
                        if(binding == nullptr || binding->bypass) continue;
                        if(heard) sounding[binding->parameterPath] = multiRow ? rest : mapNormalizedLaneValue(lane, *binding, rest);
                        else silenced.insert(binding->parameterPath);
                    }
                }
            }
        }
    }
    // Heard last frame, silenced now and written by nothing else: rest once.
    for(const auto& entry : soundingRestPaths) {
        if(sounding.count(entry.first) > 0 || silenced.count(entry.first) == 0) continue;
        if(activeAutomationValues.count(entry.first) == 0)
            activeAutomationValues[entry.first].emplace_back(ofxOceanodeTimelineAutomationMode::Replace, entry.second);
    }
    soundingRestPaths = std::move(sounding);
}
