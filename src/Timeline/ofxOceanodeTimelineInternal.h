#pragma once

// Helpers shared by the timeline manager's translation units. Not part of
// the addon's API: include it only from src/Timeline/*.cpp.

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

namespace ofxOceanodeTimelineDetail {


using ofxOceanodeTimelineCurve::valueAtBeat;
using ofxOceanodeTimelineCurve::CurveInterpolationMode;
using ofxOceanodeTimelineCurve::curveInterpolationMode;
using ofxOceanodeTimelineCurve::curveSegmentShape;

constexpr double kEpsilon = 1e-9;
namespace automationText {

// Automation values travel as text (comma separated for vectors), and OF's own
// helpers are too slow and too lossy for that path, which runs every frame:
// ofSplitString(..., trim) builds a named std::locale for every token, and
// ofToString keeps six significant digits (2000003 became 2e+06). These are
// locale-independent, and a float is written with the fewest digits that read
// back to exactly the same float.
#if defined(_WIN32)
inline _locale_t numericLocale() { static const _locale_t locale = _create_locale(LC_NUMERIC, "C"); return locale; }
#else
inline locale_t numericLocale() { static const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0); return locale; }
#endif

inline double parse(const char* begin, const char* end) {
    char buffer[64];
    const size_t length = std::min(static_cast<size_t>(end - begin), sizeof(buffer) - 1);
    std::memcpy(buffer, begin, length);
    buffer[length] = '\0';
#if defined(_WIN32)
    return _strtod_l(buffer, nullptr, numericLocale());
#else
    return strtod_l(buffer, nullptr, numericLocale());
#endif
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Calls visit(begin, end) for every non-empty, trimmed comma-separated token
// (ofSplitString(text, ",", true, true) without the copies and the locales).
template<typename Visit>
inline void forEachToken(const std::string& text, Visit&& visit) {
    const char* cursor = text.data();
    const char* const end = cursor + text.size();
    while(true) {
        const char* comma = static_cast<const char*>(std::memchr(cursor, ',', static_cast<size_t>(end - cursor)));
        if(comma == nullptr) comma = end;
        const char* begin = cursor;
        const char* finish = comma;
        while(begin < finish && isSpace(*begin)) ++begin;
        while(finish > begin && isSpace(finish[-1])) --finish;
        if(begin < finish) visit(begin, finish);
        if(comma == end) return;
        cursor = comma + 1;
    }
}

inline size_t tokenCount(const std::string& text) {
    size_t count = 0;
    forEachToken(text, [&](const char*, const char*) { ++count; });
    return count;
}

// The first token, when the text holds exactly one.
inline bool singleToken(const std::string& text, const char*& begin, const char*& end) {
    size_t count = 0;
    forEachToken(text, [&](const char* b, const char* e) { if(count++ == 0) { begin = b; end = e; } });
    return count == 1;
}

inline double parse(const std::string& text) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    while(begin < end && isSpace(*begin)) ++begin;
    return parse(begin, end);
}

inline bool parseBool(const char* begin, const char* end) {
    const size_t length = static_cast<size_t>(end - begin);
    auto is = [&](const char* word) {
        if(std::strlen(word) != length) return false;
        for(size_t i = 0; i < length; ++i)
            if(std::tolower(static_cast<unsigned char>(begin[i])) != word[i]) return false;
        return true;
    };
    if(is("true")) return true;
    if(is("false")) return false;
    return parse(begin, end) != 0.0;
}

inline void appendInt(std::string& out, long long value);

inline void appendFloat(std::string& out, float value) {
    // Whole numbers (pitches, gates, indices) are most of the traffic.
    if(std::abs(value) < 1e9f && value == std::trunc(value) && !(value == 0.0f && std::signbit(value))) {
        appendInt(out, static_cast<long long>(value));
        return;
    }
    char buffer[32];
    int length = 0;
    for(int precision = 6; precision <= 9; ++precision) {
        length = std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
        for(int i = 0; i < length; ++i) if(buffer[i] == ',') buffer[i] = '.'; // a comma-decimal C locale
        if(!std::isfinite(value) || static_cast<float>(parse(buffer, buffer + length)) == value) break;
    }
    out.append(buffer, static_cast<size_t>(length));
}

inline void appendInt(std::string& out, long long value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

inline std::string formatFloat(float value) { std::string out; appendFloat(out, value); return out; }
inline std::string formatInt(long long value) { std::string out; appendInt(out, value); return out; }
} // namespace automationText

// Word-at-a-time hashing for change detection (never stored or compared
// across runs): one multiply per word, so it can run over the whole timeline
// every frame. Read it through result().
struct ChangeHash {
    uint64_t value = 0x9E3779B97F4A7C15ULL;
    static uint64_t bits(double v) { uint64_t b; std::memcpy(&b, &v, sizeof(b)); return b; }
    static uint64_t bits(float v) { uint32_t b; std::memcpy(&b, &v, sizeof(b)); return b; }
    static uint64_t rotate(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    void word(uint64_t v) { value = (value ^ v) * 0xBF58476D1CE4E5B9ULL; }
    template<typename T>
    std::enable_if_t<std::is_arithmetic<T>::value || std::is_enum<T>::value> add(T v) {
        if constexpr(std::is_floating_point<T>::value) word(bits(v));
        else word(static_cast<uint64_t>(v));
    }
    void add(const void* pointer) { word(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer))); }
    void add(const std::string& text) {
        word(text.size());
        size_t i = 0;
        for(; i + 8 <= text.size(); i += 8) { uint64_t w; std::memcpy(&w, text.data() + i, 8); word(w); }
        uint64_t tail = 0;
        std::memcpy(&tail, text.data() + i, text.size() - i);
        word(tail);
    }
    uint64_t result() const {
        uint64_t x = value;
        x ^= x >> 31; x *= 0x94D049BB133111EBULL; x ^= x >> 29;
        return x;
    }
};

inline double positiveModulo(double value, double length) {
    if(length <= kEpsilon) return 0.0;
    double wrapped = std::fmod(value, length);
    if(wrapped < 0.0) wrapped += length;
    return wrapped;
}

inline std::string parameterValueForAutomation(ofxOceanodeAbstractParameter& parameter) {
    const std::string type = parameter.valueType();
    if(type == typeid(float).name()) return automationText::formatFloat(parameter.cast<float>().getParameter().get());
    if(type == typeid(int).name()) return automationText::formatInt(parameter.cast<int>().getParameter().get());
    if(type == typeid(bool).name()) return parameter.cast<bool>().getParameter().get() ? "1" : "0";
    if(type == typeid(std::string).name()) return parameter.cast<std::string>().getParameter().get();

    auto joinValues = [](const auto& values, auto append) {
        std::string result;
        for(size_t i = 0; i < values.size(); ++i) {
            if(i > 0) result += ",";
            append(result, values[i]);
        }
        return result;
    };
    if(type == typeid(std::vector<float>).name())
        return joinValues(parameter.cast<std::vector<float>>().getParameter().get(),
                          [](std::string& out, float v) { automationText::appendFloat(out, v); });
    if(type == typeid(std::vector<int>).name())
        return joinValues(parameter.cast<std::vector<int>>().getParameter().get(),
                          [](std::string& out, int v) { automationText::appendInt(out, v); });
    if(type == typeid(std::vector<bool>).name())
        return joinValues(parameter.cast<std::vector<bool>>().getParameter().get(),
                          [](std::string& out, bool v) { out += v ? '1' : '0'; });
    if(type == typeid(std::vector<std::string>).name())
        return joinValues(parameter.cast<std::vector<std::string>>().getParameter().get(),
                          [](std::string& out, const std::string& v) { out += v; });

    return parameter.isSerializable() ? parameter.toString() : std::string();
}

inline std::string combineAutomationValues(const std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>>& values,
                                    const std::string& valueType) {
    if(values.empty()) return std::string();
    const bool isScalarFloat = valueType == typeid(float).name();
    const bool isScalarInt = valueType == typeid(int).name();
    const bool isScalarBool = valueType == typeid(bool).name();
    const bool isVectorFloat = valueType == typeid(std::vector<float>).name();
    const bool isVectorInt = valueType == typeid(std::vector<int>).name();
    const bool isVectorBool = valueType == typeid(std::vector<bool>).name();
    const bool isNumeric = isScalarFloat || isScalarInt || isScalarBool ||
        isVectorFloat || isVectorInt || isVectorBool;
    if(!isNumeric) {
        // Text has no meaningful arithmetic blend. Preserve layer ordering.
        return values.back().second;
    }

    auto parseValue = [](const std::string& value, bool scalar) {
        std::vector<double> result;
        if(scalar) {
            result.push_back(automationText::parse(value));
            return result;
        }
        automationText::forEachToken(value, [&](const char* begin, const char* end) {
            result.push_back(automationText::parse(begin, end));
        });
        if(result.empty()) result.push_back(automationText::parse(value));
        return result;
    };

    std::vector<std::vector<double>> numericValues;
    numericValues.reserve(values.size());
    size_t outputSize = 1;
    const bool scalar = isScalarFloat || isScalarInt || isScalarBool;
    for(const auto& value : values) {
        numericValues.push_back(parseValue(value.second, scalar));
        if(!scalar)
            outputSize = std::max(outputSize, numericValues.back().size());
    }
    auto componentAt = [](const std::vector<double>& value, size_t index) {
        // A scalar broadcasts to every channel. Different non-scalar sizes
        // wrap, matching Oceanode's usual vector-broadcasting behaviour.
        return value[index % value.size()];
    };
    // A Replace contributor is the "base" signal -- everything else
    // (Add/Multiply/Min/Max) is a modulator applied on top of it. That base
    // must win by MEANING, not by vector position: which lane got added to
    // the clip first is an authoring accident the user has no reason to
    // track, so e.g. a Curve (left at the default Replace mode) driving
    // Levels together with a piano-roll Gate set to Multiply must multiply
    // correctly whichever lane happens to be earlier in the list. Find the
    // last Replace contributor (if several plain/overwrite lanes share the
    // parameter, the most recently evaluated one is the base, matching the
    // old strictly-positional behaviour in that case) and fold every other
    // contributor onto it, in their original relative order, using each
    // one's own mode.
    int baseIndex = -1;
    for(size_t i = 0; i < values.size(); ++i) {
        if(values[i].first == ofxOceanodeTimelineAutomationMode::Replace) baseIndex = static_cast<int>(i);
    }
    // No explicit Replace contributor at all (every lane driving this
    // parameter has its own blend mode) -- fall back to the first
    // contributor as the seed. Its own mode is irrelevant here since there
    // is nothing before it to combine with (Add against 0, Multiply against
    // 1, Min/Max against +-infinity all simplify to just its value).
    if(baseIndex < 0) baseIndex = 0;
    std::vector<double> result(outputSize);
    for(size_t component = 0; component < outputSize; ++component)
        result[component] = componentAt(numericValues[static_cast<size_t>(baseIndex)], component);
    for(size_t i = 0; i < values.size(); ++i) {
        if(static_cast<int>(i) == baseIndex) continue;
        // Any OTHER Replace contributor is a superseded plain/overwrite
        // signal -- the one at baseIndex already won that contest above, so
        // an earlier Replace entry must not clobber the base or the
        // modulators folded onto it while iterating past it.
        if(values[i].first == ofxOceanodeTimelineAutomationMode::Replace) continue;
        for(size_t component = 0; component < outputSize; ++component) {
            const double contribution = componentAt(numericValues[i], component);
            switch(values[i].first) {
                case ofxOceanodeTimelineAutomationMode::Add: result[component] += contribution; break;
                case ofxOceanodeTimelineAutomationMode::Multiply: result[component] *= contribution; break;
                case ofxOceanodeTimelineAutomationMode::Min: result[component] = std::min(result[component], contribution); break;
                case ofxOceanodeTimelineAutomationMode::Max: result[component] = std::max(result[component], contribution); break;
                default: break;
            }
        }
    }

    auto appendComponent = [&](std::string& out, double value) {
        if(isScalarBool || isVectorBool) out += value >= 0.5 ? '1' : '0';
        else if(isScalarInt || isVectorInt) automationText::appendInt(out, std::lround(value));
        else automationText::appendFloat(out, static_cast<float>(value));
    };
    std::string combined;
    combined.reserve(result.size() * 8);
    for(size_t component = 0; component < result.size(); ++component) {
        if(component > 0) combined += ",";
        appendComponent(combined, result[component]);
    }
    return combined;
}

// Wave clips are complete, independent audio slices: their whole source range
// [waveSourceStartBeat, waveSourceStartBeat + contentDurationBeats] is always
// mapped linearly over the visible clip. The generic repeat/crop model does not
// apply to them, and contentStretch is only a cached copy of that ratio.
inline void normalizeWaveClipMapping(ofxOceanodeTimelineClip& clip) {
    clip.repeatContent = false;
    clip.durationBeats = std::max(1.0 / 24.0, clip.durationBeats);
    clip.waveSourceStartBeat = std::max(0.0, clip.waveSourceStartBeat);
    if(clip.waveFileDurationBeats > 0.0) {
        // A slice can never extend past the end of its file; audio would
        // hold the last frame there while the waveform showed a rescaled
        // range. Allow a tiny tolerance for the rounding of split points.
        const double available = std::max(1.0 / 24.0,
            clip.waveFileDurationBeats - clip.waveSourceStartBeat);
        if(clip.contentDurationBeats > available + 1e-6) clip.contentDurationBeats = available;
    }
    clip.contentDurationBeats = std::max(1.0 / 24.0, clip.contentDurationBeats);
    clip.contentStretch = std::max(1.0 / 1024.0, clip.durationBeats / clip.contentDurationBeats);
}

// Lane solos are scoped to their clip; track solos are scoped to the timeline.
inline bool laneAudible(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane& lane) {
    return !lane.muted && (lane.solo || std::none_of(clip.lanes.begin(), clip.lanes.end(),
                                                  [](const auto& other) { return other.solo; }));
}

inline bool clipCoversBeat(const ofxOceanodeTimelineClip& clip, double globalBeat) {
    const double duration = std::max(1.0 / 24.0, clip.durationBeats);
    return globalBeat >= clip.startBeat - kEpsilon && globalBeat < clip.startBeat + duration - kEpsilon;
}

inline bool evaluateCurve(const ofxOceanodeTimelineLane& lane, double beat, std::string& value) {
    if(lane.curvePoints.empty()) return false;
    value = automationText::formatFloat(valueAtBeat(lane.curvePoints, lane.curveTensions,
                                   ofxOceanodeTimelineIsHeldValueLane(lane.type) ? "Step" : lane.curveInterpolation, beat, 0.0f));
    return true;
}

struct LfoParameterDefinition {
    const char* id;
    float minimum;
    float maximum;
    float defaultValue;
    const char* name; // lane name shown in the editor
};

// Single source for the LFO controls: evaluation ranges and the lanes a clip gets.
constexpr LfoParameterDefinition kLfoParameters[] = {
    {"frequency", 0.125f, 64.0f, 4.0f, "Frequency (beats)"},
    {"roundness", 0.0f, 1.0f, 0.5f, "Roundness"},
    {"skew", -1.0f, 1.0f, 0.0f, "Skew"},
    {"pw", 0.0f, 1.0f, 0.5f, "Pulse width"},
    {"pow", -1.0f, 1.0f, 0.0f, "Pow"},
    {"bipow", -1.0f, 1.0f, 0.0f, "BiPow"},
    {"phaseOffset", 0.0f, 1.0f, 0.0f, "Phase offset"},
    {"scale", 0.0f, 2.0f, 1.0f, "Scale"},
    {"yOffset", -1.0f, 1.0f, 0.0f, "Y offset"}
};

inline const LfoParameterDefinition* lfoParameterDefinition(const std::string& id) {
    for(const auto& definition : kLfoParameters)
        if(id == definition.id) return &definition;
    return nullptr;
}

inline bool lfoLaneIsLog(const ofxOceanodeTimelineLane& lane) {
    return lane.lfoParameter == "frequency";
}

// Linear, or logarithmic when requested and the range is strictly positive.
inline double lfoMapNormalized(float minimum, float maximum, bool logScale, float normalized) {
    const double n = ofClamp(normalized, 0.0f, 1.0f);
    if(logScale && minimum > 0.0f && maximum > minimum)
        return minimum * std::pow(static_cast<double>(maximum) / minimum, n);
    return minimum + n * (maximum - minimum);
}

// ---------------------------------------------------------------------------
// LFO phase with an automated "frequency" (= cycle length in beats).
//
// The phase is the number of cycles accumulated from source beat 0:
//     cycles(b) = integral_0^b  1 / period(x) dx
// so a frequency change bends the oscillator's speed smoothly instead of
// rescaling the whole elapsed time (sourceBeat / period), which made the
// phase jump more the further into the clip the change happened. With a
// constant period this is exactly sourceBeat / period, as before.
//
// Step and Linear segments are integrated in closed form; Log/Exp and
// Sigmoid with fixed-resolution Simpson (end-clustered), so results are deterministic and
// identical during playback, scrubbing and in the editor. Per-point running
// totals are cached per clip and rebuilt when the curve changes.
// ---------------------------------------------------------------------------

constexpr double kMinLfoPeriodBeats = 1.0 / 1024.0;
constexpr int kLfoSimpsonIntervals = 256; // even

struct LfoPeriodCurve {
    const LfoParameterDefinition* definition = nullptr;
    const ofxOceanodeTimelineLane* lane = nullptr;
    CurveInterpolationMode mode = CurveInterpolationMode::Linear;

    double periodForNormalized(float normalized) const {
        return std::max(kMinLfoPeriodBeats,
            lfoMapNormalized(definition->minimum, definition->maximum, lfoLaneIsLog(*lane), normalized));
    }
    double pointPeriod(size_t i) const {
        return periodForNormalized(lane->curvePoints[i].value);
    }
    // Cycles accumulated from point i-1 to fraction t (0..1) of segment i.
    double segmentCycles(size_t i, double t) const {
        const auto& p0 = lane->curvePoints[i - 1];
        const auto& p1 = lane->curvePoints[i];
        const double span = p1.beat - p0.beat;
        if(span <= kEpsilon || t <= 0.0) return 0.0;
        t = std::min(t, 1.0);
        const double P0 = pointPeriod(i - 1);
        const double P1 = pointPeriod(i);
        const bool inRange = p0.value >= 0.0f && p0.value <= 1.0f && p1.value >= 0.0f && p1.value <= 1.0f;
        if(mode == CurveInterpolationMode::Step) {
            return span * t / P0; // value holds until the next point
        }
        if(mode == CurveInterpolationMode::Linear && inRange) {
            if(lfoLaneIsLog(*lane) && definition->minimum > 0.0f) {
                // Log scale: the period is exponential in t, P(t) = P0 * r^t.
                const double r = P1 / P0;
                if(std::abs(r - 1.0) < 1e-9) return span * t / P0;
                return span / P0 * (1.0 - std::pow(r, -t)) / std::log(r);
            }
            const double dP = P1 - P0;
            if(std::abs(dP) < 1e-9) return span * t / P0;
            return span * std::log((P0 + dP * t) / P0) / dP;
        }
        const auto tension = i - 1 < lane->curveTensions.size()
            ? lane->curveTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
        auto inversePeriod = [&](double u) {
            const float shaped = curveSegmentShape(static_cast<float>(u), mode, tension);
            return 1.0 / periodForNormalized(ofLerp(p0.value, p1.value, shaped));
        };
        // Simpson in s with u = t * (1 - cos(pi s)) / 2: samples cluster at both ends,
        // where steep Log/Exp and Sigmoid shapes have their sharp corners.
        auto integrand = [&](double s) {
            const double u = t * 0.5 * (1.0 - std::cos(PI * s));
            const double du = t * 0.5 * PI * std::sin(PI * s);
            return inversePeriod(u) * du;
        };
        const double h = 1.0 / kLfoSimpsonIntervals;
        double sum = integrand(0.0) + integrand(1.0);
        for(int k = 1; k < kLfoSimpsonIntervals; ++k) {
            sum += (k % 2 == 1 ? 4.0 : 2.0) * integrand(k * h);
        }
        return span * h / 3.0 * sum;
    }
};

struct LfoCyclesCache {
    uint64_t signature = 0;
    std::vector<double> atPoint; // cycles from the first point to point i
};

inline uint64_t lfoCurveSignature(const ofxOceanodeTimelineLane& lane, const LfoParameterDefinition& definition) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over the curve data
    auto mixBytes = [&h](const void* data, size_t size) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for(size_t i = 0; i < size; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    };
    for(const auto& point : lane.curvePoints) { mixBytes(&point.beat, sizeof(point.beat)); mixBytes(&point.value, sizeof(point.value)); }
    for(const auto& tension : lane.curveTensions) { mixBytes(&tension.inflection, sizeof(float)); mixBytes(&tension.steepness, sizeof(float)); }
    mixBytes(lane.curveInterpolation.data(), lane.curveInterpolation.size());
    mixBytes(&definition.minimum, sizeof(float));
    mixBytes(&definition.maximum, sizeof(float));
    const size_t count = lane.curvePoints.size();
    mixBytes(&count, sizeof(count));
    return h;
}

// Cycles from source beat 0 to sourceBeat.
inline double lfoCycles(const ofxOceanodeTimelineClip& clip, double sourceBeat) {
    const auto* definition = lfoParameterDefinition("frequency");
    const ofxOceanodeTimelineLane* frequencyLane = nullptr;
    for(const auto& lane : clip.lanes) {
        if(laneAudible(clip, lane) && lane.lfoParameter == "frequency") { frequencyLane = &lane; break; }
    }
    if(definition != nullptr && frequencyLane != nullptr && frequencyLane->curveInterpolation == "Value") {
        const double period = std::max<double>(kMinLfoPeriodBeats, lfoMapNormalized(definition->minimum,
            definition->maximum, true, frequencyLane->lfoValue));
        return sourceBeat / period; // constant frequency
    }
    if(definition == nullptr || frequencyLane == nullptr || frequencyLane->curvePoints.empty()) {
        const double period = definition == nullptr ? 4.0 : std::max<double>(kMinLfoPeriodBeats, definition->defaultValue);
        return sourceBeat / period;
    }

    LfoPeriodCurve curve;
    curve.definition = definition;
    curve.lane = frequencyLane;
    curve.mode = curveInterpolationMode(frequencyLane->curveInterpolation);
    const auto& points = frequencyLane->curvePoints;
    if(points.size() == 1) return sourceBeat / curve.pointPeriod(0);

    // Running totals per point, cached per clip (rebuilt only when the curve changes).
    static thread_local std::map<std::string, LfoCyclesCache> caches;
    if(caches.size() > 256) caches.clear();
    LfoCyclesCache& cache = caches[clip.id];
    const uint64_t signature = lfoCurveSignature(*frequencyLane, *definition);
    if(cache.signature != signature || cache.atPoint.size() != points.size()) {
        cache.signature = signature;
        cache.atPoint.assign(points.size(), 0.0);
        for(size_t i = 1; i < points.size(); ++i) {
            cache.atPoint[i] = cache.atPoint[i - 1] + curve.segmentCycles(i, 1.0);
        }
    }

    // G(b): cycles from the first point to b (signed); constant period outside the points.
    auto G = [&](double b) {
        if(b <= points.front().beat) return (b - points.front().beat) / curve.pointPeriod(0);
        if(b >= points.back().beat) {
            const size_t last = points.size() - 1;
            return cache.atPoint[last] + (b - points.back().beat) / curve.pointPeriod(last);
        }
        const auto right = std::lower_bound(points.begin() + 1, points.end(), b,
            [](const ofxOceanodeTimelineCurvePoint& point, double targetBeat) {
                return point.beat < targetBeat;
            });
        const size_t i = static_cast<size_t>(right - points.begin());
        const double span = std::max(1e-9, points[i].beat - points[i - 1].beat);
        return cache.atPoint[i - 1] + curve.segmentCycles(i, (b - points[i - 1].beat) / span);
    };
    return G(sourceBeat) - G(0.0);
}

// Curve and Step lanes deliberately produce one scalar stream. When their
// destination is a numeric vector, keep the parameter's current width and
// broadcast that scalar to every component instead of shrinking the vector
// to one item. Explicit multi-component lanes already return comma-separated
// values and pass through untouched.
inline std::string broadcastScalarNumericVectorValue(ofxOceanodeAbstractParameter& parameter,
                                              const std::string& value,
                                              const std::string& valueType) {
    const bool floatVector = valueType == typeid(std::vector<float>).name();
    const bool intVector = valueType == typeid(std::vector<int>).name();
    const bool boolVector = valueType == typeid(std::vector<bool>).name();
    if(!floatVector && !intVector && !boolVector) return value;
    const char* tokenBegin = nullptr;
    const char* tokenEnd = nullptr;
    if(!automationText::singleToken(value, tokenBegin, tokenEnd)) return value;
    const size_t tokenLength = static_cast<size_t>(tokenEnd - tokenBegin);

    size_t componentCount = 0;
    if(floatVector) componentCount = parameter.cast<std::vector<float>>().getParameter().get().size();
    else if(intVector) componentCount = parameter.cast<std::vector<int>>().getParameter().get().size();
    else componentCount = parameter.cast<std::vector<bool>>().getParameter().get().size();
    if(componentCount <= 1) return value;

    std::string result;
    result.reserve((tokenLength + 1) * componentCount);
    for(size_t component = 0; component < componentCount; ++component) {
        if(component > 0) result += ",";
        result.append(tokenBegin, tokenLength);
    }
    return result;
}

inline std::string joinAutomationValues(const std::vector<std::string>& values) {
    std::string result;
    for(size_t i = 0; i < values.size(); ++i) {
        if(i > 0) result += ", ";
        result += values[i];
    }
    return result;
}

// Retrigger rule shared by steps, piano notes and gate blocks: an event that
// starts exactly where an equal one ends (back to back) is off for the first
// `gap` source beats, so the two read as two triggers. When an equal event
// overlaps the join instead, they merge (no gap).
template<typename Events, typename StartOf, typename EndOf, typename Same>
inline bool inRetriggerGap(const Events& events, size_t index, double beat, double gap,
                    StartOf startOf, EndOf endOf, Same same, double wrapLength = 0.0) {
    if(gap <= 0.0) return false;
    const double start = startOf(events[index]);
    if(beat >= start + gap) return false;
    bool touching = false;
    for(size_t other = 0; other < events.size(); ++other) {
        if(other == index || !same(events[other], events[index])) continue;
        const double otherStart = startOf(events[other]);
        const double otherEnd = endOf(events[other]);
        if(otherStart < start - 1e-6 && otherEnd > start + 1e-6) return false; // overlapping: merge
        if(std::abs(otherEnd - start) <= 1e-6) touching = true;
        // Pattern wrap: an event ending at the pattern end touches one at 0.
        if(wrapLength > 0.0 && start <= 1e-6 && std::abs(otherEnd - wrapLength) <= 1e-6) touching = true;
    }
    return touching;
}

inline bool evaluateMultiGateRow(const std::vector<ofxOceanodeTimelineGateRegion>& row, double localBeat,
                          double retriggerGap = 0.0, double wrapLength = 0.0, float* velocity = nullptr,
                          double* regionStart = nullptr) {
    auto startOf = [](const ofxOceanodeTimelineGateRegion& r) { return r.startBeat; };
    auto endOf = [](const ofxOceanodeTimelineGateRegion& r) { return r.end(); };
    auto same = [](const ofxOceanodeTimelineGateRegion&, const ofxOceanodeTimelineGateRegion&) { return true; };
    for(size_t index = 0; index < row.size(); ++index) {
        const auto& region = row[index];
        if(localBeat < region.startBeat || localBeat >= region.end()) continue;
        if(inRetriggerGap(row, index, localBeat, retriggerGap, startOf, endOf, same, wrapLength)) continue;
        if(velocity) *velocity = ofClamp(region.velocity, 0.0f, 1.0f);
        if(regionStart) *regionStart = region.startBeat;
        return true;
    }
    return false;
}

// Source beats per timeline beat of a clip (to express the retrigger gap,
// a timeline duration, in the clip's own beats).
inline double sourcePerTimelineBeat(const ofxOceanodeTimelineClip& clip) {
    if(clip.repeatContent) return 1.0 / std::max(1.0 / 1024.0, clip.contentStretch);
    return std::max(1.0 / 24.0, clip.contentDurationBeats) / std::max(1.0 / 24.0, clip.durationBeats);
}

// A binding of a track we are already iterating (no track lookup by id).
inline const ofxOceanodeTimelineParameterBinding* bindingInTrack(const ofxOceanodeTimelineTrack& track,
                                                          const std::string& bindingId) {
    if(bindingId.empty()) return nullptr;
    for(const auto& binding : track.bindings) if(binding.id == bindingId) return &binding;
    return nullptr;
}

// Kit keys: C1 (36) plays slot 0, like a General MIDI drum map.
constexpr int kKitBaseNote = 36;
} // namespace ofxOceanodeTimelineDetail
namespace ofxOceanodeTimelineDrumGrid {
namespace ofxOceanodeTimelineDetail {

inline int64_t patternsPerRepeat(double length, double contentLength) {
    return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(contentLength / length - 1e-9)));
}
} // namespace ofxOceanodeTimelineDetail
using namespace ofxOceanodeTimelineDetail;
} // namespace ofxOceanodeTimelineDrumGrid
