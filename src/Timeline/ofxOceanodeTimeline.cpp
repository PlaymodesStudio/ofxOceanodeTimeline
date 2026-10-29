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

namespace ofxOceanodeTimelineCurve {

CurveInterpolationMode curveInterpolationMode(const std::string& name) {
    if(name == "Step") return CurveInterpolationMode::Step;
    if(name == "Log / Exp") return CurveInterpolationMode::LogExp;
    if(name == "Sigmoid") return CurveInterpolationMode::Sigmoid;
    return CurveInterpolationMode::Linear;
}

float sigmoidFlex(float x, float inflection, float steepness) {
    x = ofClamp(x, 0.0f, 1.0f);
    inflection = ofClamp(inflection, 0.01f, 0.99f);
    steepness = ofClamp(steepness, 0.1f, 10.0f);
    // The neutral sigmoid must be an exact identity. Besides avoiding tiny
    // floating-point bends, this makes newly-created curve segments truly
    // linear in both evaluation and drawing.
    if(std::abs(inflection - 0.5f) <= 1e-6f && std::abs(steepness - 1.0f) <= 1e-6f) return x;
    constexpr float epsilon = 0.0001f;
    if(x < epsilon) return 0.0f;
    if(x > 1.0f - epsilon) return 1.0f;
    const float xSafe = ofClamp(x, epsilon, 1.0f - epsilon);
    const float pSafe = ofClamp(inflection, epsilon, 1.0f - epsilon);
    const float a = std::pow(xSafe / pSafe, steepness);
    const float b = std::pow((1.0f - xSafe) / (1.0f - pSafe), steepness);
    const float denominator = a + b;
    return denominator < epsilon ? 0.5f : a / denominator;
}

float curveSegmentShape(float x, CurveInterpolationMode interpolation,
                        const ofxOceanodeTimelineCurveTension& tension) {
    x = ofClamp(x, 0.0f, 1.0f);
    switch(interpolation) {
        case CurveInterpolationMode::Step: return x >= 1.0f ? 1.0f : 0.0f;
        case CurveInterpolationMode::Linear: return x;
        case CurveInterpolationMode::LogExp:
            // steepness is clamped symmetrically around 1 (0.1 <-> 10) so the
            // "logarithmic" (steepness < 1) and "exponential" (steepness > 1)
            // ends are true reciprocals of each other and bend by comparable
            // amounts. A lower bound closer to 0 makes pow(x, steepness)
            // degenerate into a near-vertical rise right at x=0 followed by a
            // flat plateau, which reads as "broken" rather than "logarithmic".
            return std::pow(x, ofClamp(tension.steepness, 0.1f, 10.0f));
        case CurveInterpolationMode::Sigmoid:
            return sigmoidFlex(x, tension.inflection, tension.steepness);
    }
    return x;
}

float valueAtBeat(const std::vector<ofxOceanodeTimelineCurvePoint>& points,
                  const std::vector<ofxOceanodeTimelineCurveTension>& tensions,
                  const std::string& interpolation, double beat, float fallback) {
    if(points.empty()) return fallback;
    if(points.size() == 1) return points.front().value;
    if(beat <= points.front().beat) return points.front().value;
    if(beat >= points.back().beat) return points.back().value;
    const auto mode = curveInterpolationMode(interpolation);
    const auto right = std::lower_bound(points.begin() + 1, points.end(), beat,
        [](const ofxOceanodeTimelineCurvePoint& point, double targetBeat) {
            return point.beat < targetBeat;
        });
    const size_t i = static_cast<size_t>(right - points.begin());
    const double span = std::max(1e-9, points[i].beat - points[i - 1].beat);
    const float t = static_cast<float>(ofClamp((beat - points[i - 1].beat) / span, 0.0, 1.0));
    const auto tension = i - 1 < tensions.size() ? tensions[i - 1] : ofxOceanodeTimelineCurveTension{};
    return ofLerp(points[i - 1].value, points[i].value, curveSegmentShape(t, mode, tension));
}

} // namespace ofxOceanodeTimelineCurve

namespace {
using ofxOceanodeTimelineCurve::valueAtBeat;
using ofxOceanodeTimelineCurve::CurveInterpolationMode;
using ofxOceanodeTimelineCurve::curveInterpolationMode;
using ofxOceanodeTimelineCurve::curveSegmentShape;

constexpr double kEpsilon = 1e-9;

// Automation values travel as text (comma separated for vectors), and OF's own
// helpers are too slow and too lossy for that path, which runs every frame:
// ofSplitString(..., trim) builds a named std::locale for every token, and
// ofToString keeps six significant digits (2000003 became 2e+06). These are
// locale-independent, and a float is written with the fewest digits that read
// back to exactly the same float.
namespace automationText {
#if defined(_WIN32)
_locale_t numericLocale() { static const _locale_t locale = _create_locale(LC_NUMERIC, "C"); return locale; }
#else
locale_t numericLocale() { static const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0); return locale; }
#endif

double parse(const char* begin, const char* end) {
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

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Calls visit(begin, end) for every non-empty, trimmed comma-separated token
// (ofSplitString(text, ",", true, true) without the copies and the locales).
template<typename Visit>
void forEachToken(const std::string& text, Visit&& visit) {
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

size_t tokenCount(const std::string& text) {
    size_t count = 0;
    forEachToken(text, [&](const char*, const char*) { ++count; });
    return count;
}

// The first token, when the text holds exactly one.
bool singleToken(const std::string& text, const char*& begin, const char*& end) {
    size_t count = 0;
    forEachToken(text, [&](const char* b, const char* e) { if(count++ == 0) { begin = b; end = e; } });
    return count == 1;
}

double parse(const std::string& text) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    while(begin < end && isSpace(*begin)) ++begin;
    return parse(begin, end);
}

bool parseBool(const char* begin, const char* end) {
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

void appendInt(std::string& out, long long value);

void appendFloat(std::string& out, float value) {
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

void appendInt(std::string& out, long long value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

std::string formatFloat(float value) { std::string out; appendFloat(out, value); return out; }
std::string formatInt(long long value) { std::string out; appendInt(out, value); return out; }
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

// Bumped when the shape of the document changes in a way a reader has to
// know about. Every field is parsed with its own default, so an older file
// loads without any version test; this exists to notice a file written by a
// NEWER build, where a silent partial load would be the wrong answer.
// 6 clip groups, 7 wave tracks, 8 self-contained LFO clips,
// 9 preset-backed editor zoom and horizontal position,
// 10 state-capture curve clips, 11 persisted timeline timebase, 12 Number lanes.
constexpr float kDefaultTimelinePixelsPerSecond = 140.0f;
constexpr float kMinTimelinePixelsPerSecond = 0.25f;
constexpr float kMaxTimelinePixelsPerSecond = 600.0f;

double positiveModulo(double value, double length) {
    if(length <= kEpsilon) return 0.0;
    double wrapped = std::fmod(value, length);
    if(wrapped < 0.0) wrapped += length;
    return wrapped;
}

std::string resolveTimelineAudioPath(const std::string& input) {
    if(input.empty()) return {};
    if(ofFile::doesFileExist(input)) return input;
    const std::string dataPath = ofToDataPath(input, true);
    if(ofFile::doesFileExist(dataPath)) return dataPath;
    const std::string samplePath = ofToDataPath("Supercollider/Samples/" + input, true);
    if(ofFile::doesFileExist(samplePath)) return samplePath;
    return input;
}

std::string parameterValueForAutomation(ofxOceanodeAbstractParameter& parameter) {
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

std::string combineAutomationValues(const std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>>& values,
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

std::string sumAutomationValues(const std::vector<std::string>& values,
                                const std::string& valueType) {
    std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>> additions;
    additions.reserve(values.size());
    for(const auto& value : values)
        additions.emplace_back(ofxOceanodeTimelineAutomationMode::Add, value);
    return combineAutomationValues(additions, valueType);
}

// Wave clips are complete, independent audio slices: their whole source range
// [waveSourceStartBeat, waveSourceStartBeat + contentDurationBeats] is always
// mapped linearly over the visible clip. The generic repeat/crop model does not
// apply to them, and contentStretch is only a cached copy of that ratio.
void normalizeWaveClipMapping(ofxOceanodeTimelineClip& clip) {
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

// Reads a WAV file's channel count, duration and a fixed-size peak cache.
// PCM 8/16/24/32 only, first fmt/data chunks used -- the same reader, with
// the same limitations, as the standalone Wave Track node this mirrors, so
// both caches look identical. Pure file IO with no SuperCollider
// dependency, safe to call whether or not an audio provider is registered.
// filePath is in/out: it is rewritten when the file is found through the
// data/Samples search path rather than as given.
bool readWaveformCache(std::string& filePath, int& outNumChannels,
                       double& outDurationMs, std::vector<float>& outPeaks) {
    outNumChannels = 0;
    outDurationMs = 0.0;
    outPeaks.clear();
    if(filePath.empty()) return true;

    const std::string resolvedPath = resolveTimelineAudioPath(filePath);
    if(resolvedPath != filePath && ofFile::doesFileExist(resolvedPath))
        filePath = resolvedPath;

    ofFile file(filePath, ofFile::ReadOnly, true);
    if(!file.is_open()) {
        ofLogWarning("ofxOceanodeTimeline") << "Could not open wave file: " << filePath;
        return false;
    }

    // Minimal RIFF/WAVE reader -- same approach and same limitations
    // (PCM 16/24/32-bit only, first fmt/data chunks used) as the old
    // standalone Wave Track node's getFileInfo/loadWaveformCache
    // (ofxOceanodeSuperCollider/waveTrack.h), which this mirrors closely
    // enough to keep the two waveform caches visually identical.
    char riff[4], wave[4];
    uint32_t riffSize = 0;
    if(!file.read((char*)&riff, 4) || !file.read((char*)&riffSize, 4) || !file.read((char*)&wave, 4)) {
        ofLogWarning("ofxOceanodeTimeline") << "Invalid or truncated wave header: " << filePath;
        file.close();
        return false;
    }
    if(std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
        ofLogWarning("ofxOceanodeTimeline") << "Selected file is not a RIFF/WAVE file: " << filePath;
        file.close();
        return false;
    }

    int numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint16_t formatType = 0;
    uint32_t dataSize = 0;
    bool haveFmt = false, haveData = false;
    std::streamoff dataStart = 0;

    while(true) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        if(!file.read((char*)&chunkId, 4)) break;
        if(!file.read((char*)&chunkSize, 4)) break;

        if(std::strncmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint16_t channels = 0, bits = 0;
            uint32_t srate = 0, byteRate = 0;
            uint16_t blockAlign = 0;
            file.read((char*)&formatType, 2);
            file.read((char*)&channels, 2);
            file.read((char*)&srate, 4);
            file.read((char*)&byteRate, 4);
            file.read((char*)&blockAlign, 2);
            file.read((char*)&bits, 2);
            numChannels = channels;
            sampleRate = srate;
            bitsPerSample = bits;
            haveFmt = true;
            if(chunkSize > 16) {
                const uint32_t extraBytes = chunkSize - 16;
                // WAVE_FORMAT_EXTENSIBLE wraps the real PCM/float subtype
                // in a 40-byte fmt chunk. Recover that subtype so a 32-bit
                // PCM file is not interpreted as IEEE float samples.
                if(formatType == 0xFFFE && extraBytes >= 24) {
                    uint16_t extensionSize = 0, validBits = 0, subtype = 0;
                    uint32_t channelMask = 0;
                    file.read((char*)&extensionSize, 2);
                    file.read((char*)&validBits, 2);
                    file.read((char*)&channelMask, 4);
                    file.read((char*)&subtype, 2);
                    formatType = subtype;
                    if(extraBytes > 10) file.seekg(extraBytes - 10, std::ios::cur);
                } else {
                    file.seekg(extraBytes, std::ios::cur);
                }
            }
        } else if(std::strncmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataStart = file.tellg();
            haveData = true;
            break;
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }
        if(chunkSize & 1u) file.seekg(1, std::ios::cur);
    }

    if(!haveFmt || !haveData || sampleRate == 0 || numChannels <= 0 || numChannels > 16 || bitsPerSample == 0) {
        ofLogWarning("ofxOceanodeTimeline") << "Wave file has no usable fmt/data chunks: " << filePath;
        file.close();
        return false;
    }

    const int bytesPerSample = bitsPerSample / 8;
    if((bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
       bytesPerSample <= 0) {
        ofLogWarning("ofxOceanodeTimeline") << "Unsupported wave bit depth (" << bitsPerSample
            << ") in " << filePath;
        file.close();
        return false;
    }
    const int totalFrames = bytesPerSample > 0 ? static_cast<int>(dataSize / (bytesPerSample * numChannels)) : 0;
    outDurationMs = totalFrames > 0
        ? static_cast<double>(totalFrames) / sampleRate * 1000.0 : 0.0;
    outNumChannels = numChannels;

    constexpr int kPointsPerChannel = 2000;
    const int framesPerPoint = std::max(1, totalFrames / kPointsPerChannel);
    outPeaks.assign(static_cast<size_t>(numChannels) * kPointsPerChannel, 0.0f);

    file.seekg(dataStart, std::ios::beg);
    std::vector<char> frameBuf(static_cast<size_t>(bytesPerSample) * numChannels);
    std::vector<float> minVals(numChannels);
    std::vector<float> maxVals(numChannels);
    for(int pt = 0; pt < kPointsPerChannel; ++pt) {
        const int startFrame = pt * framesPerPoint;
        const int endFrame = std::min(startFrame + framesPerPoint, totalFrames);
        std::fill(minVals.begin(), minVals.end(), 1.0f);
        std::fill(maxVals.begin(), maxVals.end(), -1.0f);
        for(int f = startFrame; f < endFrame; ++f) {
            if(!file.read(frameBuf.data(), frameBuf.size())) break;
            for(int ch = 0; ch < numChannels; ++ch) {
                float sample = 0.0f;
                const size_t offset = static_cast<size_t>(ch) * bytesPerSample;
                if(bitsPerSample == 8) {
                    const auto raw = static_cast<unsigned char>(frameBuf[offset]);
                    sample = (static_cast<float>(raw) - 128.0f) / 128.0f;
                } else if(bitsPerSample == 16) {
                    int16_t raw = 0;
                    std::memcpy(&raw, frameBuf.data() + offset, 2);
                    sample = raw / 32768.0f;
                } else if(bitsPerSample == 24) {
                    int32_t raw = 0;
                    std::memcpy(&raw, frameBuf.data() + offset, 3);
                    if(raw & 0x800000) raw |= (int32_t)0xFF000000;
                    sample = raw / 8388608.0f;
                } else if(bitsPerSample == 32) {
                    if(formatType == 1) {
                        int32_t raw = 0;
                        std::memcpy(&raw, frameBuf.data() + offset, 4);
                        sample = raw / 2147483648.0f;
                    } else {
                        std::memcpy(&sample, frameBuf.data() + offset, 4);
                    }
                }
                if(sample < minVals[ch]) minVals[ch] = sample;
                if(sample > maxVals[ch]) maxVals[ch] = sample;
            }
        }
        for(int ch = 0; ch < numChannels; ++ch) {
            const float peak = std::abs(maxVals[ch]) >= std::abs(minVals[ch]) ? maxVals[ch] : minVals[ch];
            outPeaks[static_cast<size_t>(ch) * kPointsPerChannel + pt] = peak;
        }
    }
    file.close();
    return true;
}

// Lane solos are scoped to their clip; track solos are scoped to the timeline.
bool laneAudible(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane& lane) {
    return !lane.muted && (lane.solo || std::none_of(clip.lanes.begin(), clip.lanes.end(),
                                                  [](const auto& other) { return other.solo; }));
}

bool clipCoversBeat(const ofxOceanodeTimelineClip& clip, double globalBeat) {
    const double duration = std::max(1.0 / 24.0, clip.durationBeats);
    return globalBeat >= clip.startBeat - kEpsilon && globalBeat < clip.startBeat + duration - kEpsilon;
}

bool clipSourceBeat(const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelinePlayContext& context,
                    double globalBeat, double& sourceBeat) {
    if(clip.muted || ofxOceanodeTimelineClipChanceSilenced(clip, context)) return false;
    if(!clipCoversBeat(clip, globalBeat)) return false;

    sourceBeat = ofxOceanodeTimelineClipTime::timelineToSourceBeat(clip, globalBeat);
    return true;
}

bool evaluateCurve(const ofxOceanodeTimelineLane& lane, double beat, std::string& value) {
    if(lane.curvePoints.empty()) return false;
    value = automationText::formatFloat(valueAtBeat(lane.curvePoints, lane.curveTensions,
                                   ofxOceanodeTimelineIsHeldValueLane(lane.type) ? "Step" : lane.curveInterpolation, beat, 0.0f));
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

constexpr float kFrequencyGridFactors[3] = {1.0f, 2.0f / 3.0f, 1.5f}; // straight, triplet, dotted

const LfoParameterDefinition* lfoParameterDefinition(const std::string& id) {
    for(const auto& definition : kLfoParameters)
        if(id == definition.id) return &definition;
    return nullptr;
}

bool lfoLaneIsLog(const ofxOceanodeTimelineLane& lane) {
    return lane.lfoParameter == "frequency";
}

// Linear, or logarithmic when requested and the range is strictly positive.
double lfoMapNormalized(float minimum, float maximum, bool logScale, float normalized) {
    const double n = ofClamp(normalized, 0.0f, 1.0f);
    if(logScale && minimum > 0.0f && maximum > minimum)
        return minimum * std::pow(static_cast<double>(maximum) / minimum, n);
    return minimum + n * (maximum - minimum);
}
float lfoUnmapValue(float minimum, float maximum, bool logScale, float value) {
    if(logScale && minimum > 0.0f && maximum > minimum) {
        const double v = std::max<double>(value, minimum);
        return ofClamp(static_cast<float>(std::log(v / minimum) / std::log(static_cast<double>(maximum) / minimum)), 0.0f, 1.0f);
    }
    if(std::abs(maximum - minimum) < 1e-9f) return 0.0f;
    return ofClamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
}

float normalizedLfoLaneValue(const ofxOceanodeTimelineLane& lane, double beat,
                             float fallback) {
    if(lane.curveInterpolation == "Value") return lane.lfoValue;
    return valueAtBeat(lane.curvePoints, lane.curveTensions,
                       lane.curveInterpolation, beat, fallback);
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

uint64_t lfoCurveSignature(const ofxOceanodeTimelineLane& lane, const LfoParameterDefinition& definition) {
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
double lfoCycles(const ofxOceanodeTimelineClip& clip, double sourceBeat) {
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

} // namespace

namespace ofxOceanodeTimelineLfo {

float laneValueFromNormalized(const ofxOceanodeTimelineLane& lane, float normalized) {
    return static_cast<float>(lfoMapNormalized(lane.valueMin, lane.valueMax, lfoLaneIsLog(lane), normalized));
}

float laneNormalizedFromValue(const ofxOceanodeTimelineLane& lane, float value) {
    return lfoUnmapValue(lane.valueMin, lane.valueMax, lfoLaneIsLog(lane), value);
}

const std::vector<float>& frequencyGridValues(int mode) {
    struct Grids {
        std::vector<float> values[3];
        Grids() {
            const auto* definition = lfoParameterDefinition("frequency");
            const float lo = definition != nullptr ? definition->minimum : 0.125f;
            const float hi = definition != nullptr ? definition->maximum : 64.0f;
            for(int m = 0; m < 3; ++m) {
                for(int exponent = -4; exponent <= 7; ++exponent) {
                    const float value = std::ldexp(1.0f, exponent) * kFrequencyGridFactors[m];
                    if(value >= lo - 1e-6f && value <= hi + 1e-6f) values[m].push_back(value);
                }
            }
        }
    };
    static const Grids grids;
    return grids.values[std::max(0, std::min(2, mode))];
}

std::string frequencyGridLabel(float beats, int mode) {
    const int m = std::max(0, std::min(2, mode));
    const float base = beats / kFrequencyGridFactors[m]; // the straight length it derives from
    std::string label = base < 1.0f ? "1/" + ofToString(static_cast<int>(std::lround(1.0f / base)))
                                    : ofToString(static_cast<int>(std::lround(base)));
    if(m == 1) label += "T";
    else if(m == 2) label += ".";
    return label;
}

float snapFrequencyValue(float beats, int mode) {
    // Nearest grid value in log distance (musically: nearest note length of the mode).
    const auto& grid = frequencyGridValues(mode);
    float best = grid.front();
    double bestDistance = 1e30;
    for(float g : grid) {
        const double distance = std::abs(std::log(std::max(1e-6f, beats) / g));
        if(distance < bestDistance) { bestDistance = distance; best = g; }
    }
    return best;
}

float evaluateParameter(const ofxOceanodeTimelineClip& clip,
                        const std::string& parameter, double sourceBeat,
                        float fallback) {
    const auto* definition = lfoParameterDefinition(parameter);
    if(definition == nullptr) return fallback;
    for(const auto& lane : clip.lanes) {
        if(!laneAudible(clip, lane) || lane.lfoParameter != parameter) continue;
        const float defaultNormalized = lfoUnmapValue(definition->minimum, definition->maximum,
                                                      lfoLaneIsLog(lane), definition->defaultValue);
        const float normalized = normalizedLfoLaneValue(lane, sourceBeat, defaultNormalized);
        return static_cast<float>(lfoMapNormalized(definition->minimum, definition->maximum,
                                                   lfoLaneIsLog(lane), normalized));
    }
    return fallback;
}

double cyclesAt(const ofxOceanodeTimelineClip& clip, double sourceBeat) {
    return lfoCycles(clip, sourceBeat);
}

float evaluate(const ofxOceanodeTimelineClip& clip, double sourceBeat) {
    // Phase from the integrated frequency curve (see lfoCycles), wrapped in double
    // precision before handing it to the oscillator.
    const double cycles = lfoCycles(clip, sourceBeat);
    const double phase = cycles - std::floor(cycles);
    baseOscillator oscillator;
    oscillator.phaseOffset_Param = evaluateParameter(clip, "phaseOffset", sourceBeat, 0.0f);
    oscillator.pow_Param = evaluateParameter(clip, "pow", sourceBeat, 0.0f);
    oscillator.pulseWidth_Param = evaluateParameter(clip, "pw", sourceBeat, 0.5f);
    oscillator.quant_Param = 0;
    oscillator.scale_Param = evaluateParameter(clip, "scale", sourceBeat, 1.0f);
    oscillator.offset_Param = evaluateParameter(clip, "yOffset", sourceBeat, 0.0f);
    oscillator.randomAdd_Param = 0.0f;
    oscillator.biPow_Param = evaluateParameter(clip, "bipow", sourceBeat, 0.0f);
    oscillator.waveSelect_Param = 0;
    oscillator.amplitude_Param = 1.0f;
    oscillator.invert_Param = 0.0f;
    oscillator.skew_Param = evaluateParameter(clip, "skew", sourceBeat, 0.0f);
    oscillator.roundness_Param = evaluateParameter(clip, "roundness", sourceBeat, 0.5f);
    return oscillator.computeFunc(static_cast<float>(phase));
}

} // namespace ofxOceanodeTimelineLfo

namespace {

// Sets the parameter to an automation value. Returns false, without notifying
// anyone, when it already holds that value: the comparison is on the typed
// value, so nothing has to be formatted back to text to find that out.
bool applyAutomationValue(ofxOceanodeAbstractParameter& parameter,
                          const std::string& value,
                          const std::string& valueType) {
    auto setIfChanged = [](auto& target, auto&& next) {
        if(target.get() == next) return false;
        target.set(next);
        return true;
    };
    if(valueType == typeid(float).name())
        return setIfChanged(parameter.cast<float>().getParameter(), static_cast<float>(automationText::parse(value)));
    if(valueType == typeid(int).name())
        return setIfChanged(parameter.cast<int>().getParameter(), static_cast<int>(automationText::parse(value)));
    if(valueType == typeid(bool).name()) {
        const char* begin = value.data();
        const char* end = begin + value.size();
        while(begin < end && automationText::isSpace(*begin)) ++begin;
        while(end > begin && automationText::isSpace(end[-1])) --end;
        return setIfChanged(parameter.cast<bool>().getParameter(), automationText::parseBool(begin, end));
    }
    if(valueType == typeid(std::string).name())
        return setIfChanged(parameter.cast<std::string>().getParameter(), value);
    if(valueType == typeid(std::vector<float>).name()) {
        std::vector<float> result;
        automationText::forEachToken(value, [&](const char* begin, const char* end) {
            result.push_back(static_cast<float>(automationText::parse(begin, end)));
        });
        return setIfChanged(parameter.cast<std::vector<float>>().getParameter(), result);
    }
    if(valueType == typeid(std::vector<int>).name()) {
        std::vector<int> result;
        automationText::forEachToken(value, [&](const char* begin, const char* end) {
            result.push_back(static_cast<int>(automationText::parse(begin, end)));
        });
        return setIfChanged(parameter.cast<std::vector<int>>().getParameter(), result);
    }
    if(valueType == typeid(std::vector<bool>).name()) {
        std::vector<bool> result;
        automationText::forEachToken(value, [&](const char* begin, const char* end) {
            result.push_back(automationText::parseBool(begin, end));
        });
        return setIfChanged(parameter.cast<std::vector<bool>>().getParameter(), result);
    }
    if(valueType == typeid(std::vector<std::string>).name()) {
        std::vector<std::string> result;
        automationText::forEachToken(value, [&](const char* begin, const char* end) { result.emplace_back(begin, end); });
        return setIfChanged(parameter.cast<std::vector<std::string>>().getParameter(), result);
    }
    if(parameterValueForAutomation(parameter) == value) return false;
    parameter.fromString(value);
    return true;
}

// Curve and Step lanes deliberately produce one scalar stream. When their
// destination is a numeric vector, keep the parameter's current width and
// broadcast that scalar to every component instead of shrinking the vector
// to one item. Explicit multi-component lanes already return comma-separated
// values and pass through untouched.
std::string broadcastScalarNumericVectorValue(ofxOceanodeAbstractParameter& parameter,
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

std::string joinAutomationValues(const std::vector<std::string>& values) {
    std::string result;
    for(size_t i = 0; i < values.size(); ++i) {
        if(i > 0) result += ", ";
        result += values[i];
    }
    return result;
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

// Drum Grid lane data. Format 2: hits at beat positions. Files without a
// format hold the first, index-based shape (steps x beatsPerStep), converted
// on load so they play exactly as before.
ofJson drumGridToJson(const ofxOceanodeTimelineLane& lane) {
    ofJson json;
    json["format"] = 2;
    json["stepFollow"] = lane.drumStepFollow;
    json["swing"] = lane.drumSwing;
    json["swingBeats"] = lane.drumSwingBeats;
    json["rows"] = ofJson::array();
    for(const auto& row : lane.drumRows) {
        ofJson rowJson = {{"slot", row.slot}, {"name", row.name}, {"length", row.length}, {"gate", row.gate},
                          {"mute", row.mute}, {"solo", row.solo}};
        rowJson["hits"] = ofJson::array();
        for(const auto& step : row.steps)
            rowJson["hits"].push_back({{"beat", step.beat}, {"length", step.length}, {"gate", step.gate},
                                       {"velocity", step.velocity}, {"pitch", step.pitch},
                                       {"probability", step.probability}, {"ratchet", step.ratchet},
                                       {"duration", step.duration}, {"filter", step.filter},
                                       {"stutter", step.stutter}});
        json["rows"].push_back(std::move(rowJson));
    }
    return json;
}

// Missing or malformed fields take their defaults.
void drumGridFromJson(const ofJson& json, ofxOceanodeTimelineLane& lane) {
    auto number = [](const ofJson& object, const char* key, double fallback) {
        const auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    };
    auto flag = [](const ofJson& object, const char* key, bool fallback) {
        const auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
    };
    const bool indexed = number(json, "format", 1.0) < 2.0;
    const double cell = std::max(1.0 / 24.0, lane.beatsPerStep);
    lane.drumSwing = ofClamp(static_cast<float>(number(json, "swing", 0.0)), 0.0f, 0.75f);
    // The first shape swung odd steps of its own step length: swing keeps it.
    // Editing follows the master grid (it never changes what plays).
    lane.drumSwingBeats = indexed ? cell : ofClamp(number(json, "swingBeats", 0.25), 1.0 / 96.0, 4.0);
    lane.drumStepFollow = indexed ? true : flag(json, "stepFollow", true);
    lane.drumRows.clear();
    const auto rowsIt = json.find("rows");
    if(rowsIt == json.end() || !rowsIt->is_array()) return;
    for(const auto& rowJson : *rowsIt) {
        if(!rowJson.is_object() || lane.drumRows.size() >= static_cast<size_t>(ofxOceanodeTimelineDrumGrid::kMaxRows)) continue;
        ofxOceanodeTimelineDrumRow row;
        row.slot = static_cast<int>(number(rowJson, "slot", static_cast<double>(lane.drumRows.size())));
        const auto nameIt = rowJson.find("name");
        row.name = nameIt != rowJson.end() && nameIt->is_string() ? nameIt->get<std::string>() : std::string();
        row.gate = static_cast<float>(number(rowJson, "gate", 0.5));
        row.mute = flag(rowJson, "mute", false);
        row.solo = flag(rowJson, "solo", false);
        auto readCommon = [&](const ofJson& stepJson, ofxOceanodeTimelineDrumStep& step) {
            step.velocity = static_cast<float>(number(stepJson, "velocity", 0.8));
            step.pitch = static_cast<int>(std::lround(number(stepJson, "pitch", 0.0)));
            step.probability = static_cast<float>(number(stepJson, "probability", 1.0));
            step.ratchet = static_cast<int>(number(stepJson, "ratchet", 1));
            step.duration = static_cast<float>(number(stepJson, "duration", 1.0));
            step.filter = static_cast<float>(number(stepJson, "filter", 0.0));
            step.stutter = static_cast<float>(number(stepJson, "stutter", 0.0));
        };
        if(indexed) {
            // Steps x beatsPerStep: an on step i plays at i steps (its swing
            // still applied by the lane's swing grid); a nudged step keeps the
            // exact place it played at. Steps past the row's length (which never
            // played) are kept as hidden hits, unnudged so they stay hidden.
            const int length = std::max(1, std::min(64, static_cast<int>(number(rowJson, "length", 16))));
            row.length = length * cell;
            const float rowGate = ofClamp(row.gate, 0.05f, 1.0f);
            const auto stepsIt = rowJson.find("steps");
            if(stepsIt != rowJson.end() && stepsIt->is_array()) {
                int index = 0;
                for(const auto& stepJson : *stepsIt) {
                    const int i = index++;
                    if(!stepJson.is_object() || !flag(stepJson, "on", false)) continue;
                    ofxOceanodeTimelineDrumStep step;
                    readCommon(stepJson, step);
                    const double nudge = ofClamp(number(stepJson, "nudge", 0.0), -0.5, 0.5);
                    if(std::abs(nudge) > 1e-9 && i < length) {
                        const double swing = (i & 1) ? lane.drumSwing : 0.0;
                        step.beat = (i + ofClamp(swing + nudge, -0.5, 0.95)) * cell;
                    } else step.beat = i * cell;
                    step.length = cell;
                    step.gate = rowGate;
                    row.steps.push_back(step);
                }
            }
        } else {
            row.length = number(rowJson, "length", 4.0);
            const auto hitsIt = rowJson.find("hits");
            if(hitsIt != rowJson.end() && hitsIt->is_array()) {
                for(const auto& stepJson : *hitsIt) {
                    if(!stepJson.is_object()) continue;
                    ofxOceanodeTimelineDrumStep step;
                    readCommon(stepJson, step);
                    step.beat = number(stepJson, "beat", 0.0);
                    step.length = number(stepJson, "length", 0.25);
                    step.gate = static_cast<float>(number(stepJson, "gate", row.gate));
                    row.steps.push_back(step);
                }
            }
        }
        ofxOceanodeTimelineDrumGrid::normalizeRow(row);
        lane.drumRows.push_back(std::move(row));
    }
}

ofJson mutationRulesToJson(const ofxOceanodeTimelineMutationRules& rules) {
    return {{"pitch", rules.pitchStrength}, {"pitchRange", rules.pitchRange}, {"pitchDirection", rules.pitchDirection},
            {"scaleMask", rules.scaleMask}, {"offset", rules.offsetStrength},
            {"offsetSteps", rules.offsetSteps}, {"offsetQuantum", rules.offsetQuantum},
            {"offsetDirection", rules.offsetDirection}, {"velocityDirection", rules.velocityDirection},
            {"length", rules.lengthStrength},
            {"lengthSteps", rules.lengthSteps}, {"lengthQuantum", rules.lengthQuantum},
            {"lengthDirection", rules.lengthDirection}, {"velocity", rules.velocityStrength},
            {"velocityRange", rules.velocityRange}};
}

ofxOceanodeTimelineMutationRules mutationRulesFromJson(const ofJson& json) {
    ofxOceanodeTimelineMutationRules rules;
    rules.pitchStrength = ofClamp(json.value("pitch", 1.0f), 0.0f, 1.0f);
    rules.pitchRange = ofClamp(json.value("pitchRange", 7), 0, 48);
    rules.pitchDirection = std::clamp(json.value("pitchDirection", 0), 0, 2);
    rules.scaleMask = json.value("scaleMask", 0xFFF) & 0xFFF;
    rules.offsetStrength = ofClamp(json.value("offset", 0.0f), 0.0f, 1.0f);
    rules.offsetSteps = ofClamp(json.value("offsetSteps", 1), 0, 64);
    rules.offsetQuantum = std::clamp(json.value("offsetQuantum", 1), 1, 64);
    rules.offsetDirection = std::clamp(json.value("offsetDirection", 0), 0, 2);
    rules.velocityDirection = std::clamp(json.value("velocityDirection", 0), 0, 2);
    rules.lengthStrength = ofClamp(json.value("length", 0.0f), 0.0f, 1.0f);
    rules.lengthSteps = std::clamp(json.value("lengthSteps", 2), 0, 64);
    rules.lengthQuantum = std::clamp(json.value("lengthQuantum", 1), 1, 64);
    rules.lengthDirection = std::clamp(json.value("lengthDirection", 0), 0, 2);
    rules.velocityStrength = ofClamp(json.value("velocity", 0.0f), 0.0f, 1.0f);
    rules.velocityRange = ofClamp(json.value("velocityRange", 0.25f), 0.0f, 1.0f);
    return rules;
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

// Retrigger rule shared by steps, piano notes and gate blocks: an event that
// starts exactly where an equal one ends (back to back) is off for the first
// `gap` source beats, so the two read as two triggers. When an equal event
// overlaps the join instead, they merge (no gap).
template<typename Events, typename StartOf, typename EndOf, typename Same>
bool inRetriggerGap(const Events& events, size_t index, double beat, double gap,
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

bool evaluateMultiGateRow(const std::vector<ofxOceanodeTimelineGateRegion>& row, double localBeat,
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

// Whether one toggle block reaches one target (lane.targetProbability): one
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

// Source beats per timeline beat of a clip (to express the retrigger gap,
// a timeline duration, in the clip's own beats).
double sourcePerTimelineBeat(const ofxOceanodeTimelineClip& clip) {
    if(clip.repeatContent) return 1.0 / std::max(1.0 / 1024.0, clip.contentStretch);
    return std::max(1.0 / 24.0, clip.contentDurationBeats) / std::max(1.0 / 24.0, clip.durationBeats);
}

// A binding of a track we are already iterating (no track lookup by id).
const ofxOceanodeTimelineParameterBinding* bindingInTrack(const ofxOceanodeTimelineTrack& track,
                                                          const std::string& bindingId) {
    if(bindingId.empty()) return nullptr;
    for(const auto& binding : track.bindings) if(binding.id == bindingId) return &binding;
    return nullptr;
}

double pianoNoteEnd(const ofxOceanodeTimelinePianoNote& note) {
    return note.startBeat + std::max(1.0 / 24.0, note.durationBeats);
}

// Kit keys: C1 (36) plays slot 0, like a General MIDI drum map.
constexpr int kKitBaseNote = 36;

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
}

namespace ofxOceanodeTimelineClipTime {

double sourceDuration(const ofxOceanodeTimelineClip& clip) {
    return std::max(1.0 / 24.0, clip.contentDurationBeats);
}

double stretch(const ofxOceanodeTimelineClip& clip) {
    return std::max(1.0 / 1024.0, clip.contentStretch);
}

double effectiveStretch(const ofxOceanodeTimelineClip& clip) {
    return clip.repeatContent ? stretch(clip) : clip.durationBeats / sourceDuration(clip);
}

double cycleDuration(const ofxOceanodeTimelineClip& clip) {
    return sourceDuration(clip) * stretch(clip);
}

double sourceToTimelineBeat(const ofxOceanodeTimelineClip& clip,
                            double sourceBeat, int64_t cycle) {
    const double content = sourceDuration(clip);
    if(clip.repeatContent)
        return clip.startBeat + (static_cast<double>(cycle) * content + sourceBeat) * stretch(clip);
    return clip.startBeat + sourceBeat * clip.durationBeats / content;
}

double timelineToSourceBeat(const ofxOceanodeTimelineClip& clip,
                            double timelineBeat) {
    const double localBeat = std::max(0.0, timelineBeat - clip.startBeat);
    if(clip.repeatContent)
        return positiveModulo(localBeat, cycleDuration(clip)) / stretch(clip);
    return localBeat * sourceDuration(clip) /
        std::max(1.0 / 24.0, clip.durationBeats);
}

int64_t cycleIndex(const ofxOceanodeTimelineClip& clip, double timelineBeat) {
    if(!clip.repeatContent) return 0;
    return static_cast<int64_t>(std::floor(
        std::max(0.0, timelineBeat - clip.startBeat) / cycleDuration(clip)));
}

} // namespace ofxOceanodeTimelineClipTime

namespace ofxOceanodeTimelineDrumGrid {

void normalizeRow(ofxOceanodeTimelineDrumRow& row) {
    row.slot = std::max(0, std::min(row.slot, 1023));
    row.length = std::isfinite(row.length) ? ofClamp(row.length, kMinLength, kMaxLength) : 4.0;
    row.gate = std::isfinite(row.gate) ? ofClamp(row.gate, 0.05f, 1.0f) : 0.5f;
    auto finite = [](auto value, auto fallback) { return std::isfinite(value) ? value : fallback; };
    for(auto& step : row.steps) {
        // Before 0: a nudged migrated hit. At or past the length: hidden.
        step.beat = ofClamp(finite(step.beat, 0.0), -row.length, kMaxLength);
        step.length = ofClamp(finite(step.length, 0.25), 1.0 / 96.0, 16.0);
        step.gate = ofClamp(finite(step.gate, 0.5f), 0.05f, 1.0f);
        step.velocity = ofClamp(finite(step.velocity, 0.8f), 0.0f, 1.0f);
        step.pitch = std::max(-24, std::min(step.pitch, 24));
        step.probability = ofClamp(finite(step.probability, 1.0f), 0.0f, 1.0f);
        step.ratchet = std::max(1, std::min(step.ratchet, kMaxRatchet));
        step.duration = ofClamp(finite(step.duration, 1.0f), 0.1f, kMaxDuration);
        step.filter = ofClamp(finite(step.filter, 0.0f), -1.0f, 1.0f);
        step.stutter = ofClamp(finite(step.stutter, 0.0f), 0.0f, 127.0f);
    }
    std::stable_sort(row.steps.begin(), row.steps.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    if(row.steps.size() > static_cast<size_t>(kMaxHitsPerRow)) row.steps.resize(static_cast<size_t>(kMaxHitsPerRow));
}

std::vector<ofxOceanodeTimelineDrumRow> defaultRows(int count, const std::vector<std::string>& names, double lengthBeats) {
    std::vector<ofxOceanodeTimelineDrumRow> rows;
    count = std::max(1, std::min(count, kMaxRows));
    for(int i = 0; i < count; ++i) {
        ofxOceanodeTimelineDrumRow row;
        row.slot = i;
        row.name = i < static_cast<int>(names.size()) && !names[i].empty() ? names[i] : "Row " + std::to_string(i + 1);
        row.length = lengthBeats;
        normalizeRow(row);
        rows.push_back(std::move(row));
    }
    return rows;
}

bool rowAudible(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row) {
    if(row.mute) return false;
    const bool anySolo = std::any_of(lane.drumRows.begin(), lane.drumRows.end(), [](const auto& r) { return r.solo; });
    return !anySolo || row.solo;
}

namespace {
// The hit's index on the lane's swing grid, when it sits on that grid.
bool swingIndex(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumStep& step, int64_t& index) {
    const double grid = std::max(1.0 / 96.0, lane.drumSwingBeats);
    const double k = std::round(step.beat / grid);
    if(std::abs(step.beat - k * grid) > 1e-6) return false;
    index = static_cast<int64_t>(k);
    return true;
}
}

double swingOffset(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumStep& step) {
    int64_t index = 0;
    if(lane.drumSwing <= 0.0f || !swingIndex(lane, step, index) || (index & 1) == 0) return 0.0;
    return ofClamp(lane.drumSwing, 0.0f, 0.75f) * std::max(1.0 / 96.0, lane.drumSwingBeats);
}

bool probabilityPasses(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumStep& step, int slot,
                       int64_t cycle, uint64_t epoch) {
    const float probability = ofClamp(step.probability, 0.0f, 1.0f);
    if(probability <= 0.0f) return false;
    if(probability >= 1.0f) return true;
    // One stable roll per hit and pattern cycle (and clip start, through
    // epoch), keyed by its place: its swing-grid index when it is on that
    // grid (as the first, index-based Drum Grid did), else its position.
    int64_t index = 0;
    const uint64_t key = swingIndex(lane, step, index)
        ? static_cast<uint64_t>(static_cast<uint32_t>(index) + 1)
        : 0x8000000000000000ULL ^ static_cast<uint64_t>(std::llround(step.beat * 9600.0));
    uint64_t hash = key * 0x9e3779b97f4a7c15ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(slot) + 1) * 0x632be59bd9b4e019ULL;
    hash ^= static_cast<uint64_t>(cycle) * 0xbf58476d1ce4e5b9ULL;
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(lane.probabilitySeed)) * 0xd6e8feb86659fd93ULL;
    hash ^= epoch * 0x94d049bb133111ebULL;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return static_cast<float>(hash & 0xffffULL) / 65535.0f < probability;
}

double effectiveStepBeats(const ofxOceanodeTimelineLane& lane, double masterGridBeats) {
    const double beats = lane.drumStepFollow ? (masterGridBeats > 0.0 ? masterGridBeats : 0.25) : lane.beatsPerStep;
    return std::max(1.0 / 24.0, beats);
}

int stepInRange(const ofxOceanodeTimelineDrumRow& row, double fromBeat, double toBeat, double preferBeat) {
    int best = -1;
    for(size_t i = 0; i < row.steps.size(); ++i) {
        const double beat = row.steps[i].beat;
        if(beat < fromBeat - 1e-9 || beat >= toBeat - 1e-9) continue;
        if(best < 0 || std::abs(beat - preferBeat) < std::abs(row.steps[static_cast<size_t>(best)].beat - preferBeat))
            best = static_cast<int>(i);
    }
    return best;
}

ofColor rowColor(const std::vector<ofColor>& colors, const ofxOceanodeTimelineDrumRow& row, const ofColor& fallback) {
    if(row.slot >= 0 && row.slot < static_cast<int>(colors.size()) && colors[static_cast<size_t>(row.slot)].a > 0)
        return colors[static_cast<size_t>(row.slot)];
    return fallback;
}

int addStep(ofxOceanodeTimelineDrumRow& row, const ofxOceanodeTimelineDrumStep& step) {
    if(row.steps.size() >= static_cast<size_t>(kMaxHitsPerRow)) return -1;
    const auto it = std::upper_bound(row.steps.begin(), row.steps.end(), step.beat,
                                     [](double beat, const auto& s) { return beat < s.beat; });
    return static_cast<int>(row.steps.insert(it, step) - row.steps.begin());
}

namespace {
int64_t patternsPerRepeat(double length, double contentLength) {
    return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(contentLength / length - 1e-9)));
}

// The raw sub-hits of pattern `pattern` (counted from beat 0 of `repeat`), in
// that repeat's source beats; open = start, before any retrigger gap. A
// negative pattern belongs to the previous repeat (shifted back by one content
// length) so a pattern's wrap sees what played just before it.
void patternHits(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
                 int64_t pattern, int64_t repeat, double contentLength, uint64_t epoch,
                 std::vector<ofxOceanodeTimelineDrumHit>& out) {
    if(lane.behavior == "Mute" || row.steps.empty()) return;
    const double length = ofClamp(row.length, kMinLength, kMaxLength);
    const int64_t perRepeat = patternsPerRepeat(length, contentLength);
    double shift = 0.0;
    while(pattern < 0) {
        if(repeat <= 0) return;
        --repeat;
        pattern += perRepeat;
        shift -= contentLength;
    }
    if(pattern >= perRepeat) return;
    const int64_t cycle = repeat * perRepeat + pattern;
    const bool roll = lane.probabilityEnabled && lane.behavior != "Always";
    const double base = static_cast<double>(pattern) * length;
    for(size_t i = 0; i < row.steps.size(); ++i) {
        const auto& step = row.steps[i];
        if(step.velocity <= 0.0f || step.beat >= length - 1e-9) continue; // hidden past the length
        if(roll && !probabilityPasses(lane, step, row.slot, cycle, epoch)) continue;
        const double hitStart = base + step.beat + swingOffset(lane, step);
        const int ratchet = std::max(1, std::min(step.ratchet, kMaxRatchet));
        const double sub = std::max(1.0 / 96.0, step.length) / ratchet;
        const double open = sub * ofClamp(step.gate, 0.05f, 1.0f);
        for(int r = 0; r < ratchet; ++r) {
            double start = hitStart + r * sub;
            double close = start + open;
            start = std::max(0.0, start);
            close = std::min(close, contentLength);
            if(start >= contentLength - 1e-9 || close <= start + 1e-9) continue;
            ofxOceanodeTimelineDrumHit hit;
            hit.start = hit.open = start + shift;
            hit.close = close + shift;
            hit.velocity = ofClamp(step.velocity, 0.0f, 1.0f);
            hit.pitch = kBasePitch + std::max(-24, std::min(step.pitch, 24));
            hit.duration = ofClamp(step.duration, 0.1f, kMaxDuration);
            hit.filter = ofClamp(step.filter, -1.0f, 1.0f);
            hit.stutter = ofClamp(step.stutter, 0.0f, 127.0f);
            hit.step = static_cast<int>(i);
            out.push_back(hit);
        }
    }
}

// Hits of patterns [firstPattern, lastPattern] in start order, each opening
// after the retrigger gap when the hit before it had not been closed for that
// long, so a repeated hit is always a fresh rising edge.
void resolvedHits(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
                  int64_t firstPattern, int64_t lastPattern, int64_t repeat, double contentLength, double gap,
                  uint64_t epoch, std::vector<ofxOceanodeTimelineDrumHit>& hits) {
    hits.clear();
    // Two patterns earlier than asked: hits may sit up to a pattern outside
    // their own, and the hit before the first one decides its gap.
    for(int64_t p = firstPattern - 2; p <= lastPattern + 1; ++p) patternHits(lane, row, p, repeat, contentLength, epoch, hits);
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    for(size_t i = 1; i < hits.size(); ++i) {
        auto& previous = hits[i - 1];
        auto& hit = hits[i];
        if(gap > 0.0 && previous.close > hit.start - gap) {
            hit.open = hit.start + gap;
            // A gate shorter than the gap still opens.
            if(hit.close < hit.open + gap) hit.close = hit.open + gap;
        }
        // The next hit closes the one before it.
        if(previous.close > hit.start) previous.close = hit.start;
    }
}
} // namespace

bool rowStateAt(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
                double sourceBeat, int64_t repeat, double contentLength, double gap,
                uint64_t epoch, float& gate, int& heldPitch, float* heldDuration, float* heldFilter,
                float* heldStutter) {
    gate = 0.0f;
    heldPitch = kBasePitch;
    if(heldDuration != nullptr) *heldDuration = 1.0f;
    if(heldFilter != nullptr) *heldFilter = 0.0f;
    if(heldStutter != nullptr) *heldStutter = 0.0f;
    if(row.steps.empty()) return false;
    const double length = ofClamp(row.length, kMinLength, kMaxLength);
    const int64_t p = static_cast<int64_t>(std::floor(sourceBeat / length));
    thread_local std::vector<ofxOceanodeTimelineDrumHit> hits;
    resolvedHits(lane, row, p, p, repeat, contentLength, gap, epoch, hits);
    const ofxOceanodeTimelineDrumHit* current = nullptr;
    for(const auto& hit : hits) {
        if(hit.start > sourceBeat + 1e-9) break;
        current = &hit;
    }
    if(current != nullptr) {
        heldPitch = current->pitch;
        if(heldDuration != nullptr) *heldDuration = current->duration;
        if(heldFilter != nullptr) *heldFilter = current->filter;
        if(heldStutter != nullptr) *heldStutter = current->stutter;
        if(sourceBeat >= current->open - 1e-9 && sourceBeat < current->close - 1e-9) {
            gate = current->velocity;
            return true;
        }
        return false;
    }
    // Nothing near: hold the last hit played in earlier patterns, else the
    // row's last placed hit (a loop holds what the previous pass ended on).
    for(int64_t q = p - 3; q >= p - 8; --q) {
        hits.clear();
        patternHits(lane, row, q, repeat, contentLength, epoch, hits);
        if(!hits.empty()) {
            std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
            heldPitch = hits.back().pitch;
            if(heldDuration != nullptr) *heldDuration = hits.back().duration;
            if(heldFilter != nullptr) *heldFilter = hits.back().filter;
            if(heldStutter != nullptr) *heldStutter = hits.back().stutter;
            return false;
        }
        if(q < 0 && repeat <= 0) break;
    }
    // The last audible one (hidden hits sit at the end).
    for(auto it = row.steps.rbegin(); it != row.steps.rend(); ++it) {
        if(stepHidden(row, *it)) continue;
        heldPitch = kBasePitch + std::max(-24, std::min(it->pitch, 24));
        if(heldDuration != nullptr) *heldDuration = ofClamp(it->duration, 0.1f, kMaxDuration);
        if(heldFilter != nullptr) *heldFilter = ofClamp(it->filter, -1.0f, 1.0f);
        if(heldStutter != nullptr) *heldStutter = ofClamp(it->stutter, 0.0f, 127.0f);
        break;
    }
    return false;
}

void rowHits(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineDrumRow& row,
             double fromBeat, double toBeat, int64_t repeat, double contentLength, double gap,
             uint64_t epoch, std::vector<ofxOceanodeTimelineDrumHit>& out) {
    if(toBeat < fromBeat || row.steps.empty()) return;
    const double length = ofClamp(row.length, kMinLength, kMaxLength);
    const int64_t first = static_cast<int64_t>(std::floor(fromBeat / length));
    const int64_t last = std::min<int64_t>(static_cast<int64_t>(std::floor(toBeat / length)), first + 4096);
    std::vector<ofxOceanodeTimelineDrumHit> hits;
    resolvedHits(lane, row, first, last, repeat, contentLength, gap, epoch, hits);
    for(const auto& hit : hits)
        if(hit.start >= fromBeat - 1e-9 && hit.start <= toBeat + 1e-9) out.push_back(hit);
}

} // namespace ofxOceanodeTimelineDrumGrid

void ofxOceanodeTimelineStepLane::sortSteps() {
    std::sort(steps.begin(), steps.end(), [](const auto& a, const auto& b) {
        return a.startBeat < b.startBeat;
    });
}

void ofxOceanodeTimelineStepLane::setStep(double startBeat, const std::string& value,
                                          double durationBeats, float probability) {
    startBeat = std::max(0.0, startBeat);
    durationBeats = std::max(0.0, durationBeats);
    probability = ofClamp(probability, 0.0f, 1.0f);
    for(auto& step : steps) {
        if(std::abs(step.startBeat - startBeat) <= kEpsilon) {
            step.value = value;
            step.durationBeats = durationBeats;
            step.probability = probability;
            sortSteps();
            return;
        }
    }
    steps.push_back({startBeat, durationBeats, value, probability});
    sortSteps();
}

bool ofxOceanodeTimelineStepLane::removeStep(double startBeat, double epsilon) {
    const auto oldSize = steps.size();
    steps.erase(std::remove_if(steps.begin(), steps.end(), [&](const auto& step) {
        return std::abs(step.startBeat - startBeat) <= epsilon;
    }), steps.end());
    return steps.size() != oldSize;
}

ofJson ofxOceanodeTimelineStepLane::toJson() const {
    ofJson json;
    json["steps"] = ofJson::array();
    for(const auto& step : steps) {
        json["steps"].push_back({
            {"startBeat", step.startBeat},
            {"durationBeats", step.durationBeats},
            {"value", step.value},
            {"probability", step.probability},
            {"velocity", step.velocity}
        });
    }
    return json;
}

void ofxOceanodeTimelineStepLane::fromJson(const ofJson& json) {
    steps.clear();
    if(json.contains("steps") && json["steps"].is_array()) {
        for(const auto& stepJson : json["steps"]) {
            if(!stepJson.is_object() || !stepJson.contains("startBeat") || !stepJson.contains("value")) continue;
            ofxOceanodeTimelineStep step;
            step.startBeat = std::max(0.0, stepJson.value("startBeat", 0.0));
            step.durationBeats = std::max(0.0, stepJson.value("durationBeats", 0.0));
            step.value = stepJson.value("value", std::string());
            step.probability = ofClamp(stepJson.value("probability", 1.0f), 0.0f, 1.0f);
            step.velocity = ofClamp(stepJson.value("velocity", 1.0f), 0.0f, 1.0f);
            steps.push_back(std::move(step));
        }
    }
    sortSteps();
}

ofxOceanodeTimelineManager::ofxOceanodeTimelineManager(ofxOceanodeContainer* owner) : container(owner),
    clipChanceSalt(static_cast<uint64_t>(ofRandom(1.0f, 16777216.0f)) * 0x9e3779b97f4a7c15ULL) {
    playContext.salt = clipChanceSalt;
}

namespace {
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

bool ofxOceanodeTimelineManager::isClipChanceSilenced(const ofxOceanodeTimelineClip& clip) const {
    return ofxOceanodeTimelineClipChanceSilenced(clip, playContext);
}

bool ofxOceanodeTimelineManager::setMetronomeEnabled(bool enabled) {
    if(!enabled) {
        metronome.reset();
        return true;
    }
    if(metronome) return true;
    if(container == nullptr || container->getTransport() == nullptr) return false;
    auto output = std::make_unique<ofxOceanodeTimelineMetronome>(container->getTransport());
    output->setMeter(timeSignatureNumerator, timeSignatureDenominator);
    if(!output->start()) {
        ofLogWarning("ofxOceanodeTimeline") << "Could not open metronome audio output";
        return false;
    }
    metronome = std::move(output);
    return true;
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

void ofxOceanodeTimelineManager::rebuildParameterTrackColorCache() const {
    parameterTrackColorCache.clear();
    size_t bindingCount = 0;
    for(const auto& track : tracks) bindingCount += track.bindings.size();
    parameterTrackColorCache.reserve(bindingCount);
    // Preserve the previous first-track-wins behaviour for parameters that
    // are published to more than one track.
    for(const auto& track : tracks) {
        for(const auto& binding : track.bindings)
            parameterTrackColorCache.emplace(binding.parameterPath, track.color);
    }
    parameterTrackColorCacheFrame = static_cast<uint64_t>(ofGetFrameNum());
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
    for(const auto& track : tracks) bindingCount += track.bindings.size();
    bindingsByPathCache.reserve(bindingCount);
    trackByIdCache.reserve(tracks.size());
    trackEvaluationIndexes.reserve(tracks.size());

    for(auto& track : tracks) {
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
    for(const auto& track : tracks) {
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

void ofxOceanodeTimelineManager::setContainer(ofxOceanodeContainer* owner) {
    if(container == owner) return;
    for(const auto& track : tracks) clearTimelineFlag(track);
    container = owner;
    invalidateEvaluationIndexes();
    std::set<std::string> parameterPaths;
    for(const auto& track : tracks)
        for(const auto& binding : track.bindings)
            parameterPaths.insert(binding.parameterPath);
    for(const auto& path : parameterPaths) refreshTimelineFlag(path);
}

std::string ofxOceanodeTimelineManager::makeId(const char* prefix, uint64_t number) {
    return std::string(prefix) + "_" + ofToString(number);
}

std::string ofxOceanodeTimelineManager::makeUniqueTrackName(const std::string& requestedName) const {
    const std::string base = requestedName.empty() ? "Timeline Track" : requestedName;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) { return track.name == candidate; });
    };
    if(!exists(base)) return base;
    for(int suffix = 2; ; ++suffix) {
        const std::string candidate = base + " " + ofToString(suffix);
        if(!exists(candidate)) return candidate;
    }
}

std::string ofxOceanodeTimelineManager::makeUniqueClipName(const ofxOceanodeTimelineTrack& track,
                                                           const std::string& requestedName) const {
    const std::string base = requestedName.empty() ? "Clip" : requestedName;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(track.clips.begin(), track.clips.end(), [&](const auto& clip) {
            return clip.name == candidate;
        });
    };
    if(!exists(base)) return base;
    for(int suffix = 2; ; ++suffix) {
        const std::string candidate = base + " " + ofToString(suffix);
        if(!exists(candidate)) return candidate;
    }
}

std::string ofxOceanodeTimelineManager::makeUniqueTrackId() const {
    std::string id;
    uint64_t number = nextTrackNumber;
    do {
        id = makeId("timeline_track", number++);
    } while(getTrack(id) != nullptr);
    return id;
}

std::string ofxOceanodeTimelineManager::makeUniqueBindingId() const {
    std::string id;
    uint64_t number = nextBindingNumber;
    do {
        id = makeId("timeline_binding", number++);
        bool used = false;
        for(const auto& track : tracks) {
            used = std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& binding) { return binding.id == id; });
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineManager::makeUniqueClipId() const {
    std::string id;
    uint64_t number = nextClipNumber;
    do {
        id = makeId("timeline_clip", number++);
        bool used = false;
        for(const auto& track : tracks) {
            used = std::any_of(track.clips.begin(), track.clips.end(), [&](const auto& clip) {
                return clip.id == id;
            });
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineManager::makeUniqueLaneId() const {
    std::string id;
    uint64_t number = nextLaneNumber;
    do {
        id = makeId("timeline_lane", number++);
        bool used = false;
        for(const auto& track : tracks) {
            for(const auto& clip : track.clips) {
                used = std::any_of(clip.lanes.begin(), clip.lanes.end(), [&](const auto& lane) {
                    return lane.id == id;
                });
                if(used) break;
            }
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineManager::makeUniqueGroupId() const {
    std::string id;
    uint64_t number = nextGroupNumber;
    do {
        id = makeId("timeline_group", number++);
    } while(std::any_of(clipGroups.begin(), clipGroups.end(), [&](const auto& group) { return group.id == id; }));
    return id;
}

std::string ofxOceanodeTimelineManager::createTrack(const std::string& requestedName) {
    ofxOceanodeTimelineTrack track;
    track.id = makeUniqueTrackId();
    track.name = makeUniqueTrackName(requestedName);
    static const ofColor defaultColors[] = {
        ofColor(65, 165, 245, 255),
        ofColor(75, 190, 125, 255),
        ofColor(235, 165, 65, 255),
        ofColor(190, 95, 220, 255),
        ofColor(235, 90, 95, 255)
    };
    track.color = defaultColors[tracks.size() % (sizeof(defaultColors) / sizeof(defaultColors[0]))];
    ++nextTrackNumber;
    tracks.push_back(std::move(track));
    invalidateEvaluationIndexes();
    return tracks.back().id;
}

std::string ofxOceanodeTimelineManager::insertTrackCopy(ofxOceanodeTimelineTrack copy,
    const std::string& name, size_t index) {
    const auto id = createTrack(name);
    copy.name = getTrack(id)->name;
    copy.id = id;
    *getTrack(id) = std::move(copy);
    auto& duplicate = *getTrack(id);
    std::map<std::string, std::string> bindings;
    for(auto& binding : duplicate.bindings) {
        const auto old = binding.id;
        binding.id = makeUniqueBindingId(); ++nextBindingNumber;
        bindings[old] = binding.id;
        binding.hasLiveOverride = false;
        binding.liveOverrideValue.clear();
    }
    auto remap = [&](std::string& binding) {
        const auto found = bindings.find(binding);
        binding = found == bindings.end() ? std::string() : found->second;
    };
    size_t noteNumber = 1;
    for(auto& group : duplicate.noteGroups) {
        group.id = id + "_notes_" + ofToString(noteNumber++);
        remap(group.pitchBindingId); remap(group.gateBindingId); remap(group.velocityBindingId); remap(group.durationBindingId);
        remap(group.filterBindingId); remap(group.stutterBindingId);
    }
    for(auto& clip : duplicate.clips) {
        clip.id = makeUniqueClipId(); ++nextClipNumber;
        remap(clip.lfoOutputBindingId);
        for(auto& lane : clip.lanes) {
            lane.id = makeUniqueLaneId(); ++nextLaneNumber;
            for(auto& binding : lane.bindingIds) remap(binding);
            lane.bindingIds.erase(std::remove(lane.bindingIds.begin(), lane.bindingIds.end(), std::string()), lane.bindingIds.end());
            remap(lane.pianoPitchBindingId); remap(lane.pianoGateBindingId); remap(lane.pianoVelocityBindingId);
            remap(lane.drumDurationBindingId); remap(lane.drumFilterBindingId); remap(lane.drumStutterBindingId);
        }
    }
    std::rotate(tracks.begin() + index, tracks.end() - 1, tracks.end());
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return id;
}

std::string ofxOceanodeTimelineManager::duplicateTrack(const std::string& trackId) {
    const std::string sourceId = trackId; // caller may pass the track's own id by reference
    const auto* source = getTrack(sourceId);
    if(source == nullptr) return {};
    const auto groups = clipGroups;
    std::vector<std::string> originals;
    for(const auto& clip : source->clips) originals.push_back(clip.id);
    const size_t index = static_cast<size_t>(source - tracks.data()) + 1;
    const auto id = insertTrackCopy(*source, source->name + " copy", index);
    std::map<std::string, std::string> clips;
    const auto& copied = getTrack(id)->clips;
    for(size_t i = 0; i < originals.size(); ++i) clips[originals[i]] = copied[i].id;
    // Independent groups only; never link copies back to another track.
    for(const auto& group : groups) {
        std::vector<std::pair<std::string, std::string>> members;
        for(const auto& member : group.members) {
            const auto found = clips.find(member.second);
            if(member.first == sourceId && found != clips.end()) members.emplace_back(id, found->second);
        }
        if(members.size() > 1) groupClips(members);
    }
    return id;
}

std::vector<std::pair<std::string, std::string>> ofxOceanodeTimelineManager::breakClipLanesIntoTracks(
    const std::string& trackId, const std::string& clipId) {
    const std::string sourceId = trackId, sourceClipId = clipId;
    const auto* source = getTrack(sourceId);
    const auto* sourceClip = getClip(sourceId, sourceClipId);
    if(source == nullptr || sourceClip == nullptr || source->isWaveTrack ||
       sourceClip->isLfo || sourceClip->lanes.size() < 2) return {};
    size_t index = static_cast<size_t>(source - tracks.data()) + 1;
    // Copy track settings without copying all the other clips for every lane.
    ofxOceanodeTimelineTrack settings;
    settings.name = source->name;
    settings.color = source->color;
    settings.collapsed = source->collapsed;
    settings.bindings = source->bindings;
    settings.noteGroups = source->noteGroups;
    auto clip = *sourceClip;
    auto lanes = std::move(clip.lanes);
    clip.lanes.clear();
    std::vector<std::pair<std::string, std::string>> result;
    for(const auto& lane : lanes) {
        auto copy = settings;
        if(!ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)) copy.noteGroups.clear();
        std::set<std::string> used(lane.bindingIds.begin(), lane.bindingIds.end());
        for(const auto* role : {&lane.pianoPitchBindingId, &lane.pianoGateBindingId, &lane.pianoVelocityBindingId,
                                &lane.drumDurationBindingId, &lane.drumFilterBindingId, &lane.drumStutterBindingId})
            if(!role->empty()) used.insert(*role);
        copy.bindings.erase(std::remove_if(copy.bindings.begin(), copy.bindings.end(),
            [&](const auto& binding) { return !used.count(binding.id); }), copy.bindings.end());
        for(auto& group : copy.noteGroups) {
            for(auto* role : {&group.pitchBindingId, &group.gateBindingId, &group.velocityBindingId, &group.durationBindingId,
                              &group.filterBindingId, &group.stutterBindingId})
                if(!used.count(*role)) role->clear();
        }
        copy.noteGroups.erase(std::remove_if(copy.noteGroups.begin(), copy.noteGroups.end(),
            [](const auto& group) { return group.members().empty(); }), copy.noteGroups.end());
        copy.clips.push_back(clip);
        copy.clips.front().lanes.push_back(lane);
        const auto id = insertTrackCopy(std::move(copy), settings.name + " / " + lane.name, index++);
        result.emplace_back(id, getTrack(id)->clips.front().id);
    }
    removeClip(sourceId, sourceClipId);
    return result;
}

std::string ofxOceanodeTimelineManager::createWaveTrack(const std::string& requestedName) {
    const auto trackId = createTrack(requestedName);
    if(auto* track = getTrack(trackId)) {
        track->isWaveTrack = true;
        track->waveTrackHeight = 96.0f;
        track->waveVolumeAutomationEnabled = true;
        track->waveVolumePoints = {{0.0, track->waveVolume}, {4.0, track->waveVolume}};
        track->waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
    }
    return trackId;
}

std::string ofxOceanodeTimelineManager::createWaveVolumeLane(const std::string& trackId,
                                                             const std::string& clipId) {
    const auto* track = getTrack(trackId);
    auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || !track->isWaveTrack) return {};
    // Compatibility shim for older callers. Volume belongs to the whole
    // Wave Track now, so never create a lane inside this clip.
    auto* editableTrack = getTrack(trackId);
    editableTrack->waveVolumeAutomationEnabled = true;
    if(editableTrack->waveVolumePoints.empty()) {
        const double endBeat = std::max(1.0 / 24.0, clip->startBeat + clip->durationBeats);
        editableTrack->waveVolumePoints = {{0.0, editableTrack->waveVolume},
                                           {endBeat, editableTrack->waveVolume}};
        editableTrack->waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
    }
    return "track-volume:" + trackId;
}

float ofxOceanodeTimelineManager::evaluateWaveClipVolume(const std::string& trackId,
                                                         const std::string& clipId,
                                                         double beat) const {
    const auto* track = getTrack(trackId);
    const auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || !track->isWaveTrack) return 1.0f;
    return evaluateWaveTrackVolume(trackId, beat);
}

float ofxOceanodeTimelineManager::evaluateWaveTrackVolume(const std::string& trackId, double beat) const {
    const auto* track = getTrack(trackId);
    if(track == nullptr || !track->isWaveTrack) return 1.0f;
    // The automation toggle is authoritative: with it off the track plays at
    // its plain gain, whatever points happen to be stored.
    if(!track->waveVolumeAutomationEnabled || track->waveVolumePoints.empty())
        return ofClamp(track->waveVolume, 0.0f, 4.0f);
    return ofClamp(valueAtBeat(track->waveVolumePoints, track->waveVolumeTensions,
                               track->waveVolumeInterpolation, beat, track->waveVolume), 0.0f, 4.0f);
}

std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(const std::string& trackId) {
    static std::vector<ofxOceanodeTimelineCurvePoint> empty;
    auto* track = getTrack(trackId);
    return track != nullptr ? track->waveVolumePoints : empty;
}

const std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(const std::string& trackId) const {
    static const std::vector<ofxOceanodeTimelineCurvePoint> empty;
    const auto* track = getTrack(trackId);
    return track != nullptr ? track->waveVolumePoints : empty;
}

bool ofxOceanodeTimelineManager::addWaveTrackVolumePoint(const std::string& trackId, double beat, float value) {
    auto* track = getTrack(trackId);
    if(track == nullptr || !track->isWaveTrack) return false;
    track->waveVolumeAutomationEnabled = true;
    track->waveVolumePoints.push_back({std::max(0.0, beat), ofClamp(value, 0.0f, 4.0f)});
    std::sort(track->waveVolumePoints.begin(), track->waveVolumePoints.end(),
              [](const auto& a, const auto& b) { return a.beat < b.beat; });
    std::vector<ofxOceanodeTimelineCurveTension> tensions;
    if(track->waveVolumePoints.size() > 1)
        tensions.assign(track->waveVolumePoints.size() - 1, ofxOceanodeTimelineCurveTension{});
    track->waveVolumeTensions = std::move(tensions);
    return true;
}

bool ofxOceanodeTimelineManager::removeWaveTrackVolumePoint(const std::string& trackId, double beat, double epsilon) {
    auto* track = getTrack(trackId);
    if(track == nullptr || !track->isWaveTrack) return false;
    const auto oldSize = track->waveVolumePoints.size();
    track->waveVolumePoints.erase(std::remove_if(track->waveVolumePoints.begin(), track->waveVolumePoints.end(),
        [&](const auto& point) { return std::abs(point.beat - beat) <= epsilon; }), track->waveVolumePoints.end());
    track->waveVolumeTensions.resize(track->waveVolumePoints.size() > 1 ? track->waveVolumePoints.size() - 1 : 0);
    return oldSize != track->waveVolumePoints.size();
}

bool ofxOceanodeTimelineManager::renameTrack(const std::string& trackId, const std::string& requestedName) {
    auto* track = getTrack(trackId);
    if(track == nullptr || requestedName.empty()) return false;
    if(track->name == requestedName) return true;
    track->name = makeUniqueTrackName(requestedName);
    return true;
}

void ofxOceanodeTimelineManager::requestTrackRename(const std::string& trackId, bool isNewTrack) {
    if(getTrack(trackId) != nullptr) {
        pendingTrackRenameId = trackId;
        pendingTrackRenameIsNew = isNewTrack;
    }
}

bool ofxOceanodeTimelineManager::consumePendingTrackRename(std::string& trackId, bool* isNewTrack) {
    if(pendingTrackRenameId.empty()) return false;
    trackId = pendingTrackRenameId;
    if(isNewTrack != nullptr) *isNewTrack = pendingTrackRenameIsNew;
    pendingTrackRenameId.clear();
    pendingTrackRenameIsNew = false;
    return true;
}

bool ofxOceanodeTimelineManager::removeTrack(const std::string& trackId) {
    invalidateSchedule(); // correct queued events while the track still exists
    auto it = std::find_if(tracks.begin(), tracks.end(), [&](const auto& track) { return track.id == trackId; });
    if(it == tracks.end()) return false;
    std::set<std::string> affectedPaths;
    for(const auto& binding : it->bindings) {
        affectedPaths.insert(binding.parameterPath);
        stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + binding.id);
        stateCaptureLastAppliedValues.erase(binding.parameterPath);
    }
    // Every clip on this track is about to disappear along with it -- drop
    // it from any group first so removeClipFromGroups' bookkeeping
    // (dissolving a group that drops below two members) runs the same way
    // it would from removeClip, instead of leaving a group half-full of
    // references to a track that no longer exists. Any Wave lane also
    // needs its audio backend released the same way, or a registered
    // provider would keep a synth/buffer alive for a lane that no longer
    // exists anywhere in this manager.
    for(const auto& clip : it->clips) {
        removeClipFromGroups(trackId, clip.id);
        releaseWaveClipIfNeeded(trackId, clip.id);
    }
    tracks.erase(it);
    invalidateEvaluationIndexes();
    for(const auto& path : affectedPaths) refreshTimelineFlag(path);
    return true;
}

ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(const std::string& trackId) {
    if(evaluationIndexesValid) {
        const auto cached = trackByIdCache.find(trackId);
        if(cached != trackByIdCache.end()) return cached->second;
    }
    auto it = std::find_if(tracks.begin(), tracks.end(), [&](auto& track) { return track.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(const std::string& trackId) const {
    if(evaluationIndexesValid) {
        const auto cached = trackByIdCache.find(trackId);
        if(cached != trackByIdCache.end()) return cached->second;
    }
    auto it = std::find_if(tracks.begin(), tracks.end(), [&](const auto& track) { return track.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

std::string ofxOceanodeTimelineManager::addBinding(const std::string& trackId,
                                                   ofxOceanodeAbstractParameter& parameter,
                                                   ofxOceanodeTimelineAutomationMode mode) {
    auto* track = getTrack(trackId);
    if(track == nullptr || track->isWaveTrack || container == nullptr || !isStepLaneCompatible(parameter)) return std::string();

    const std::string path = container->getTimelineParameterPath(parameter);
    // A parameter may be published to several tracks. Each track owns an
    // independent binding (and blend mode), while duplicate bindings inside
    // one track would only create ambiguous rows and are therefore rejected.
    if(std::any_of(track->bindings.begin(), track->bindings.end(), [&](const auto& existing) {
        return existing.parameterPath == path;
    })) return std::string();

    ofxOceanodeTimelineParameterBinding binding;
    binding.id = makeUniqueBindingId();
    binding.parameterPath = path;
    binding.valueType = parameter.valueType();
    binding.defaultValue = parameterValueForAutomation(parameter);
    binding.mode = mode;
    if(auto* model = parameter.getNodeModel()) binding.valueScale = model->getTimelineValueScale(parameter);
    // a toggle's clips default to a Toggle lane (a one-row gate)
    if(binding.valueType == typeid(bool).name()) binding.laneType = ofxOceanodeTimelineLaneType::MultiGate;
    // and a dropdown's to a Dropdown lane
    if(!ofxOceanodeTimelineDropdownOptions(parameter).empty()) binding.laneType = ofxOceanodeTimelineLaneType::Dropdown;
    track->bindings.push_back(binding);
    invalidateEvaluationIndexes();

    // No clip or lane is created here, whether this is the track's first
    // binding or its fifth -- a binding just joins the track's pool of
    // automatable parameters. The user creates a clip explicitly (right-click
    // a row -> "New clip here", or the clip-creation dialog) and only then
    // does it get a lane pointed at one of these bindings. Previously the
    // very first binding on a brand-new track got a starter clip/lane
    // automatically, which was inconsistent with every binding added after
    // it (and surprising: it meant the *order* parameters were bound in
    // silently decided whether a clip appeared).
    parameter.setTimelined(true);
    ++nextBindingNumber;
    return binding.id;
}

bool ofxOceanodeTimelineManager::requestParameterLane(const std::string& trackId,
                                                        ofxOceanodeAbstractParameter& parameter) {
    const auto* track = getTrack(trackId);
    if(track == nullptr || track->isWaveTrack || container == nullptr ||
       !isStepLaneCompatible(parameter) || (parameter.getFlags() & ofxOceanodeParameterFlags_ReadOnly)) return false;
    const auto path = container->getTimelineParameterPath(parameter);
    std::string bindingId;
    for(const auto& binding : track->bindings)
        if(binding.parameterPath == path) { bindingId = binding.id; break; }
    if(bindingId.empty()) bindingId = addBinding(trackId, parameter);
    if(bindingId.empty()) return false;
    const auto request = std::make_pair(trackId, bindingId);
    if(std::find(pendingParameterLanes.begin(), pendingParameterLanes.end(), request) == pendingParameterLanes.end())
        pendingParameterLanes.push_back(request);
    return true;
}

bool ofxOceanodeTimelineManager::bindClipTarget(const std::string& trackId, const std::string& clipId,
    const std::string& laneId, const std::string& path, ofxOceanodeTimelineTargetRole role) {
    auto* track = getTrack(trackId);
    auto* clip = getClip(trackId, clipId);
    auto* lane = getLane(trackId, clipId, laneId);
    auto* parameter = container == nullptr ? nullptr : container->findTimelineParameter(path);
    if(track == nullptr || track->isWaveTrack || clip == nullptr || parameter == nullptr ||
       !isStepLaneCompatible(*parameter) || (parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly)) return false;
    using Role = ofxOceanodeTimelineTargetRole;
    if(role == Role::LfoOutput) { if(!clip->isLfo) return false; }
    else if(lane == nullptr || clip->isLfo || (role != Role::Lane && !ofxOceanodeTimelineLaneUsesNoteRoles(lane->type))) return false;
    std::string bindingId;
    for(const auto& binding : track->bindings) if(binding.parameterPath == path) bindingId = binding.id;
    if(bindingId.empty()) bindingId = addBinding(trackId, *parameter);
    if(bindingId.empty()) return false;
    if(role == Role::LfoOutput) {
        clip->lfoOutputBindingId = bindingId;
        if(parameter->valueType() == typeid(float).name()) {
            clip->lfoOutputMin = parameter->cast<float>().getParameter().getMin();
            clip->lfoOutputMax = parameter->cast<float>().getParameter().getMax();
        } else if(parameter->valueType() == typeid(int).name()) {
            clip->lfoOutputMin = parameter->cast<int>().getParameter().getMin();
            clip->lfoOutputMax = parameter->cast<int>().getParameter().getMax();
        }
    } else if(role == Role::Lane) {
        addBindingToLane(trackId, clipId, laneId, bindingId);
    } else {
        if((role == Role::Duration || role == Role::Filter || role == Role::Stutter) && lane->type != ofxOceanodeTimelineLaneType::DrumGrid) return false;
        std::string* selected = role == Role::Pitch ? &lane->pianoPitchBindingId
            : role == Role::Gate ? &lane->pianoGateBindingId
            : role == Role::Duration ? &lane->drumDurationBindingId
            : role == Role::Filter ? &lane->drumFilterBindingId
            : role == Role::Stutter ? &lane->drumStutterBindingId : &lane->pianoVelocityBindingId;
        const std::string previous = *selected;
        for(auto* other : {&lane->pianoPitchBindingId, &lane->pianoGateBindingId, &lane->pianoVelocityBindingId,
                           &lane->drumDurationBindingId, &lane->drumFilterBindingId, &lane->drumStutterBindingId})
            if(other != selected && *other == bindingId) other->clear();
        *selected = bindingId;
        addBindingToLane(trackId, clipId, laneId, bindingId);
        if(!previous.empty() && previous != bindingId && previous != lane->pianoPitchBindingId &&
           previous != lane->pianoGateBindingId && previous != lane->pianoVelocityBindingId &&
           previous != lane->drumDurationBindingId && previous != lane->drumFilterBindingId &&
           previous != lane->drumStutterBindingId)
            removeBindingFromLane(trackId, clipId, laneId, previous);
    }
    invalidateEvaluationIndexes(); invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::isParameterBound(const ofxOceanodeAbstractParameter& parameter) const {
    if(container == nullptr) return false;
    const std::string path = container->getTimelineParameterPath(const_cast<ofxOceanodeAbstractParameter&>(parameter));
    if(parameterTrackColorCacheFrame != static_cast<uint64_t>(ofGetFrameNum()))
        rebuildParameterTrackColorCache();
    return parameterTrackColorCache.find(path) != parameterTrackColorCache.end();
}

bool ofxOceanodeTimelineManager::getParameterTrackColor(const ofxOceanodeAbstractParameter& parameter, ofColor& color) const {
    if(container == nullptr) return false;
    const std::string path = container->getTimelineParameterPath(const_cast<ofxOceanodeAbstractParameter&>(parameter));
    if(parameterTrackColorCacheFrame != static_cast<uint64_t>(ofGetFrameNum()))
        rebuildParameterTrackColorCache();
    const auto it = parameterTrackColorCache.find(path);
    if(it == parameterTrackColorCache.end()) return false;
    color = it->second;
    return true;
}

std::vector<std::string> ofxOceanodeTimelineDropdownOptions(ofxOceanodeAbstractParameter& parameter) {
    if(parameter.valueType() == typeid(int).name()) return parameter.cast<int>().getDropdownOptions();
    if(parameter.valueType() == typeid(float).name()) return parameter.cast<float>().getDropdownOptions();
    return {};
}

bool ofxOceanodeTimelineManager::isStepLaneCompatible(const ofxOceanodeAbstractParameter& parameter) const {
    const std::string type = parameter.valueType();
    return type == typeid(float).name() ||
           type == typeid(int).name() ||
           type == typeid(bool).name() ||
           type == typeid(std::string).name() ||
           type == typeid(std::vector<float>).name() ||
           type == typeid(std::vector<int>).name() ||
           type == typeid(std::vector<bool>).name() ||
           type == typeid(std::vector<std::string>).name();
}

bool ofxOceanodeTimelineManager::moveBinding(const std::string& trackId, const std::string& bindingId, int newIndex) {
    auto* track = getTrack(trackId);
    if(track == nullptr || track->bindings.empty()) return false;
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(),
                           [&](const auto& binding) { return binding.id == bindingId; });
    if(it == track->bindings.end()) return false;
    const int from = static_cast<int>(it - track->bindings.begin());
    const int to = std::max(0, std::min(newIndex, static_cast<int>(track->bindings.size()) - 1));
    if(from == to) return false;
    auto binding = std::move(*it);
    track->bindings.erase(it);
    track->bindings.insert(track->bindings.begin() + to, std::move(binding));
    normalizeNoteGroups(*track);
    invalidateEvaluationIndexes();
    return true;
}


bool ofxOceanodeTimelineManager::setBindingOrder(const std::string& trackId, const std::vector<std::string>& orderedBindingIds) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    std::vector<ofxOceanodeTimelineParameterBinding> ordered;
    ordered.reserve(track->bindings.size());
    for(const auto& id : orderedBindingIds) {
        auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](const auto& b) { return b.id == id; });
        if(it == track->bindings.end()) continue;
        ordered.push_back(std::move(*it));
        track->bindings.erase(it);
    }
    for(auto& remaining : track->bindings) ordered.push_back(std::move(remaining));
    track->bindings = std::move(ordered);
    normalizeNoteGroups(*track);
    invalidateEvaluationIndexes();
    return true;
}

const ofxOceanodeTimelineNoteGroup* ofxOceanodeTimelineManager::findNoteGroupForBinding(
    const ofxOceanodeTimelineTrack& track, const std::string& bindingId) {
    for(const auto& group : track.noteGroups) if(group.contains(bindingId)) return &group;
    return nullptr;
}

ofxOceanodeTimelineNoteGroup* ofxOceanodeTimelineManager::getNoteGroup(const std::string& trackId, const std::string& groupId) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return nullptr;
    for(auto& group : track->noteGroups) if(group.id == groupId) return &group;
    return nullptr;
}

void ofxOceanodeTimelineManager::normalizeNoteGroups(ofxOceanodeTimelineTrack& track) {
    auto hasBinding = [&](const std::string& id) {
        return std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& b) { return b.id == id; });
    };
    for(auto& group : track.noteGroups) {
        for(auto* role : {&group.pitchBindingId, &group.gateBindingId, &group.velocityBindingId, &group.durationBindingId,
                          &group.filterBindingId, &group.stutterBindingId})
            if(!role->empty() && !hasBinding(*role)) role->clear();
        if(group.durationBindingId == group.pitchBindingId || group.durationBindingId == group.gateBindingId ||
           group.durationBindingId == group.velocityBindingId) group.durationBindingId.clear();
        if(group.filterBindingId == group.pitchBindingId || group.filterBindingId == group.gateBindingId ||
           group.filterBindingId == group.velocityBindingId || group.filterBindingId == group.durationBindingId)
            group.filterBindingId.clear();
        if(group.stutterBindingId == group.pitchBindingId || group.stutterBindingId == group.gateBindingId ||
           group.stutterBindingId == group.velocityBindingId || group.stutterBindingId == group.durationBindingId ||
           group.stutterBindingId == group.filterBindingId)
            group.stutterBindingId.clear();
        if(group.gateBindingId == group.pitchBindingId) group.gateBindingId.clear();
        if(group.velocityBindingId == group.pitchBindingId || group.velocityBindingId == group.gateBindingId)
            group.velocityBindingId.clear();
    }
    track.noteGroups.erase(std::remove_if(track.noteGroups.begin(), track.noteGroups.end(),
                                          [](const auto& group) { return group.members().empty(); }),
                           track.noteGroups.end());
    for(const auto& group : track.noteGroups) {
        const auto members = group.members();
        size_t insertAt = track.bindings.size();
        for(size_t i = 0; i < track.bindings.size(); ++i) {
            if(group.contains(track.bindings[i].id)) { insertAt = i; break; }
        }
        std::vector<ofxOceanodeTimelineParameterBinding> extracted;
        for(const auto& id : members) {
            auto it = std::find_if(track.bindings.begin(), track.bindings.end(), [&](const auto& b) { return b.id == id; });
            if(it == track.bindings.end()) continue;
            if(static_cast<size_t>(it - track.bindings.begin()) < insertAt) --insertAt;
            extracted.push_back(std::move(*it));
            track.bindings.erase(it);
        }
        insertAt = std::min(insertAt, track.bindings.size());
        track.bindings.insert(track.bindings.begin() + insertAt,
                              std::make_move_iterator(extracted.begin()), std::make_move_iterator(extracted.end()));
    }
}

void ofxOceanodeTimelineManager::syncNoteGroupLanes(ofxOceanodeTimelineTrack& track, const ofxOceanodeTimelineNoteGroup& group,
                                                    const std::vector<std::string>& affectedBindingIds) {
    const auto members = group.members();
    for(auto& clip : track.clips) {
        for(auto& lane : clip.lanes) {
            if(!ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)) continue;
            const bool uses = std::any_of(lane.bindingIds.begin(), lane.bindingIds.end(), [&](const auto& id) {
                return std::find(affectedBindingIds.begin(), affectedBindingIds.end(), id) != affectedBindingIds.end();
            });
            if(!uses) continue;
            lane.bindingIds = members;
            lane.pianoPitchBindingId = group.pitchBindingId;
            lane.pianoGateBindingId = group.gateBindingId;
            lane.pianoVelocityBindingId = group.velocityBindingId;
            lane.drumDurationBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group.durationBindingId : std::string();
            lane.drumFilterBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group.filterBindingId : std::string();
            lane.drumStutterBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group.stutterBindingId : std::string();
            if(lane.type != ofxOceanodeTimelineLaneType::DrumGrid)
                for(const auto* kitOnly : {&group.durationBindingId, &group.filterBindingId, &group.stutterBindingId})
                    if(!kitOnly->empty())
                        lane.bindingIds.erase(std::remove(lane.bindingIds.begin(), lane.bindingIds.end(), *kitOnly),
                                              lane.bindingIds.end());
        }
    }
}

std::string ofxOceanodeTimelineManager::createNoteGroup(const std::string& trackId, const std::string& name,
                                                        const std::string& pitchBindingId, const std::string& gateBindingId,
                                                        const std::string& velocityBindingId) {
    auto* track = getTrack(trackId);
    if(track == nullptr || track->isWaveTrack) return std::string();
    ofxOceanodeTimelineNoteGroup group;
    uint64_t number = 1;
    do {
        group.id = makeId("timeline_notes", number++);
    } while(std::any_of(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == group.id; }));
    group.name = name.empty() ? std::string("Notes") : name;
    track->noteGroups.push_back(group);
    const std::string id = group.id;
    if(!setNoteGroupRoles(trackId, id, pitchBindingId, gateBindingId, velocityBindingId)) return std::string();
    return getNoteGroup(trackId, id) != nullptr ? id : std::string();
}

bool ofxOceanodeTimelineManager::setNoteGroupRoles(const std::string& trackId, const std::string& groupId,
                                                   const std::string& pitchBindingId, const std::string& gateBindingId,
                                                   const std::string& velocityBindingId) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    auto groupIt = std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(groupIt == track->noteGroups.end()) return false;
    const std::vector<std::string> newMembers = {pitchBindingId, gateBindingId, velocityBindingId};
    // A binding leaves any other group it was in (that group's lanes stop using it).
    for(auto& other : track->noteGroups) {
        if(other.id == groupId) continue;
        const auto before = other.members();
        bool changed = false;
        for(auto* role : {&other.pitchBindingId, &other.gateBindingId, &other.velocityBindingId}) {
            if(!role->empty() && std::find(newMembers.begin(), newMembers.end(), *role) != newMembers.end()) {
                role->clear();
                changed = true;
            }
        }
        if(changed) syncNoteGroupLanes(*track, other, before);
    }
    auto& group = *std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    std::vector<std::string> affected = group.members();
    group.pitchBindingId = pitchBindingId;
    group.gateBindingId = gateBindingId;
    group.velocityBindingId = velocityBindingId;
    const auto members = group.members();
    affected.insert(affected.end(), members.begin(), members.end());
    const auto groupCopy = group;
    syncNoteGroupLanes(*track, groupCopy, affected);
    normalizeNoteGroups(*track);
    invalidateEvaluationIndexes();
    return true;
}

namespace {
// The kit-only roles of a note group (Drum Grid lanes write them): the
// binding field, its "decided" flag and the lane field.
struct KitRole {
    std::string ofxOceanodeTimelineNoteGroup::* group;
    bool ofxOceanodeTimelineNoteGroup::* decided;
    std::string ofxOceanodeTimelineLane::* lane;
};
const KitRole kDurationRole{&ofxOceanodeTimelineNoteGroup::durationBindingId, &ofxOceanodeTimelineNoteGroup::durationDecided,
                            &ofxOceanodeTimelineLane::drumDurationBindingId};
const KitRole kFilterRole{&ofxOceanodeTimelineNoteGroup::filterBindingId, &ofxOceanodeTimelineNoteGroup::filterDecided,
                          &ofxOceanodeTimelineLane::drumFilterBindingId};
const KitRole kStutterRole{&ofxOceanodeTimelineNoteGroup::stutterBindingId, &ofxOceanodeTimelineNoteGroup::stutterDecided,
                           &ofxOceanodeTimelineLane::drumStutterBindingId};
}

bool ofxOceanodeTimelineManager::setNoteGroupDuration(const std::string& trackId, const std::string& groupId,
                                                      const std::string& durationBindingId) {
    return setNoteGroupKitRole(trackId, groupId, durationBindingId, 0);
}

bool ofxOceanodeTimelineManager::setNoteGroupFilter(const std::string& trackId, const std::string& groupId,
                                                    const std::string& filterBindingId) {
    return setNoteGroupKitRole(trackId, groupId, filterBindingId, 1);
}

bool ofxOceanodeTimelineManager::setNoteGroupStutter(const std::string& trackId, const std::string& groupId,
                                                     const std::string& stutterBindingId) {
    return setNoteGroupKitRole(trackId, groupId, stutterBindingId, 2);
}

bool ofxOceanodeTimelineManager::setNoteGroupKitRole(const std::string& trackId, const std::string& groupId,
                                                     const std::string& bindingId, int which) {
    const auto& role = which == 0 ? kDurationRole : which == 1 ? kFilterRole : kStutterRole;
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    auto it = std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(it == track->noteGroups.end()) return false;
    if(!bindingId.empty() && it->contains(bindingId) && (*it).*role.group != bindingId) return false;
    // A binding belongs to one group only.
    if(const auto* other = findNoteGroupForBinding(*track, bindingId); other != nullptr && other->id != groupId) return false;
    std::vector<std::string> affected = it->members();
    (*it).*role.group = bindingId;
    (*it).*role.decided = true;
    const auto members = it->members();
    affected.insert(affected.end(), members.begin(), members.end());
    const auto groupCopy = *it;
    syncNoteGroupLanes(*track, groupCopy, affected);
    normalizeNoteGroups(*track);
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::renameNoteGroup(const std::string& trackId, const std::string& groupId, const std::string& name) {
    auto* group = getNoteGroup(trackId, groupId);
    if(group == nullptr || name.empty()) return false;
    group->name = name;
    return true;
}

bool ofxOceanodeTimelineManager::removeNoteGroup(const std::string& trackId, const std::string& groupId, bool removeBindings) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    auto it = std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(it == track->noteGroups.end()) return false;
    const auto members = it->members();
    track->noteGroups.erase(it);
    if(removeBindings) {
        for(const auto& id : members) removeBinding(trackId, id);
    }
    return true;
}

bool ofxOceanodeTimelineManager::setNoteGroupVoices(const std::string& trackId, const std::string& groupId, int voices) {
    auto* group = getNoteGroup(trackId, groupId);
    if(group == nullptr) return false;
    group->voices = std::max(0, std::min(voices, 128));
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
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

std::string ofxOceanodeTimelineManager::createNoteGroupClip(const std::string& trackId, const std::string& groupId,
                                                            const std::string& requestedName, double startBeat,
                                                            double durationBeats, std::string* laneIdOut,
                                                            ofxOceanodeTimelineLaneType laneType) {
    const auto* groupPtr = getNoteGroup(trackId, groupId);
    if(groupPtr == nullptr) return std::string();
    const auto group = *groupPtr;
    const std::string clipId = createClip(trackId, requestedName.empty() ? group.name : requestedName, startBeat, durationBeats);
    if(clipId.empty()) return clipId;
    if(laneType == ofxOceanodeTimelineLaneType::DrumGrid) {
        const std::string laneId = createLane(trackId, clipId, group.name, ofxOceanodeTimelineLaneType::DrumGrid);
        if(laneId.empty()) return clipId;
        if(auto* lane = getLane(trackId, clipId, laneId)) {
            lane->pianoPitchBindingId = group.pitchBindingId;
            lane->pianoGateBindingId = group.gateBindingId;
            lane->pianoVelocityBindingId = group.velocityBindingId;
            lane->drumDurationBindingId = group.durationBindingId;
            lane->drumFilterBindingId = group.filterBindingId;
            lane->drumStutterBindingId = group.stutterBindingId;
            lane->bindingIds = group.members();
            int voices = 0;
            std::vector<std::string> names;
            const auto* track = getTrack(trackId);
            if(track == nullptr || !drumGridInstrument(*track, *lane, voices, names) || voices <= 0) voices = 4;
            if(group.voices > 0) voices = group.voices;
            const auto* clip = getClip(trackId, clipId);
            // Rows as long as the clip's content (at most 16 beats).
            const double length = std::min(16.0, clip == nullptr ? 4.0 : clip->contentDurationBeats);
            lane->drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(voices, names, length);
        }
        invalidateEvaluationIndexes();
        if(laneIdOut != nullptr) *laneIdOut = laneId;
        return clipId;
    }
    const std::string laneId = createLane(trackId, clipId, group.name, ofxOceanodeTimelineLaneType::PianoRoll);
    if(laneId.empty()) return clipId;
    if(auto* lane = getLane(trackId, clipId, laneId)) {
        lane->pianoPitchBindingId = group.pitchBindingId;
        lane->pianoGateBindingId = group.gateBindingId;
        lane->pianoVelocityBindingId = group.velocityBindingId;
        lane->bindingIds = group.members();
        // A piano roll does not drive a kit's Duration.
        if(!group.filterBindingId.empty())
            lane->bindingIds.erase(std::remove(lane->bindingIds.begin(), lane->bindingIds.end(), group.filterBindingId),
                                   lane->bindingIds.end());
        if(!group.stutterBindingId.empty())
            lane->bindingIds.erase(std::remove(lane->bindingIds.begin(), lane->bindingIds.end(), group.stutterBindingId),
                                   lane->bindingIds.end());
        if(!group.durationBindingId.empty())
            lane->bindingIds.erase(std::remove(lane->bindingIds.begin(), lane->bindingIds.end(), group.durationBindingId),
                                   lane->bindingIds.end());
        // Show the keys the pitch parameter can actually play, when that is a
        // note range narrower than the whole keyboard; a kit shows its pads.
        lane->pianoLowPitch = 36;
        lane->pianoHighPitch = 84;
        const auto* track = getTrack(trackId);
        int voices = 0;
        bool kit = false;
        if(track != nullptr) pianoLaneVoicesAndKit(*track, *lane, voices, kit);
        if(kit) {
            lane->pianoLowPitch = 36;
            lane->pianoHighPitch = 36 + std::max(12, voices) - 1;
        } else if(const auto* pitchBinding = getBinding(trackId, group.pitchBindingId)) {
            if(auto* parameter = container == nullptr ? nullptr : container->findTimelineParameter(pitchBinding->parameterPath)) {
                float minimum = 0.0f, maximum = 0.0f;
                bool numeric = true;
                const auto& type = pitchBinding->valueType;
                // A piano roll may have widened it already: use the parameter's own range.
                const auto saved = savedPianoRanges.find(pitchBinding->parameterPath);
                const bool hasSaved = saved != savedPianoRanges.end();
                auto lowest = [](const auto& v) { return v.empty() ? 0.0f : static_cast<float>(*std::min_element(v.begin(), v.end())); };
                auto highest = [](const auto& v) { return v.empty() ? 0.0f : static_cast<float>(*std::max_element(v.begin(), v.end())); };
                if(type == typeid(float).name()) {
                    minimum = hasSaved ? saved->second.floatMin : parameter->cast<float>().getParameter().getMin();
                    maximum = hasSaved ? saved->second.floatMax : parameter->cast<float>().getParameter().getMax();
                } else if(type == typeid(int).name()) {
                    minimum = hasSaved ? saved->second.intMin : parameter->cast<int>().getParameter().getMin();
                    maximum = hasSaved ? saved->second.intMax : parameter->cast<int>().getParameter().getMax();
                } else if(type == typeid(std::vector<float>).name()) {
                    minimum = lowest(hasSaved ? saved->second.floatVectorMin : parameter->cast<std::vector<float>>().getParameter().getMin());
                    maximum = highest(hasSaved ? saved->second.floatVectorMax : parameter->cast<std::vector<float>>().getParameter().getMax());
                } else if(type == typeid(std::vector<int>).name()) {
                    minimum = lowest(hasSaved ? saved->second.intVectorMin : parameter->cast<std::vector<int>>().getParameter().getMin());
                    maximum = highest(hasSaved ? saved->second.intVectorMax : parameter->cast<std::vector<int>>().getParameter().getMax());
                } else numeric = false;
                if(numeric && minimum >= 0.0f && maximum <= 127.0f && maximum - minimum >= 12.0f && maximum - minimum <= 72.0f) {
                    lane->pianoLowPitch = static_cast<int>(std::ceil(minimum));
                    lane->pianoHighPitch = static_cast<int>(std::floor(maximum));
                }
            }
        }
    }
    if(laneIdOut != nullptr) *laneIdOut = laneId;
    return clipId;
}

namespace ofxOceanodeTimelineScale {
bool isDecibels(const ofxOceanodeTimelineLane& lane, const ofxOceanodeTimelineParameterBinding* binding) {
    if(lane.type != ofxOceanodeTimelineLaneType::Curve || lane.valueMax <= 0.0f) return false;
    if(lane.curveScale == "dB") return true;
    if(!lane.curveScale.empty()) return false;
    return binding != nullptr && binding->valueScale == "dB";
}
float topDb(const ofxOceanodeTimelineLane& lane) {
    return 20.0f * std::log10(std::max(1e-6f, lane.valueMax));
}
float normalizedToDb(const ofxOceanodeTimelineLane& lane, float normalized) {
    if(normalized <= 0.0f) return -std::numeric_limits<float>::infinity();
    return kDbFloor + std::min(1.0f, normalized) * (topDb(lane) - kDbFloor);
}
float dbToNormalized(const ofxOceanodeTimelineLane& lane, float db) {
    if(!std::isfinite(db) || db <= kDbFloor) return 0.0f;
    const float span = std::max(1e-6f, topDb(lane) - kDbFloor);
    return ofClamp((db - kDbFloor) / span, 0.0f, 1.0f);
}
float normalizedToGain(const ofxOceanodeTimelineLane& lane, float normalized) {
    const float floorGain = std::max(0.0f, std::min(lane.valueMin, lane.valueMax));
    if(normalized <= 0.0f) return floorGain;
    const float gain = std::pow(10.0f, normalizedToDb(lane, normalized) / 20.0f);
    return ofClamp(gain, floorGain, lane.valueMax);
}
float gainToNormalized(const ofxOceanodeTimelineLane& lane, float gain) {
    if(gain <= 0.0f) return 0.0f;
    return dbToNormalized(lane, 20.0f * std::log10(gain));
}
}

int ofxOceanodeTimelineManager::retargetParameterPaths(const std::map<std::string, std::string>& oldToNew) {
    int changed = 0;
    for(auto& track : tracks) {
        for(auto& binding : track.bindings) {
            const auto it = oldToNew.find(binding.parameterPath);
            if(it == oldToNew.end() || it->second.empty() || it->second == it->first) continue;
            binding.parameterPath = it->second;
            binding.missingTarget = false;
            changed++;
        }
    }
    if(changed > 0) {
        invalidateEvaluationIndexes();
        invalidateSchedule();
    }
    return changed;
}

bool ofxOceanodeTimelineManager::removeBinding(const std::string& trackId, const std::string& bindingId) {
    invalidateSchedule(); // correct queued events while the binding still exists
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](const auto& binding) { return binding.id == bindingId; });
    if(it == track->bindings.end()) return false;
    const std::string parameterPath = it->parameterPath;
    stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + bindingId);
    stateCaptureLastAppliedValues.erase(parameterPath);
    for(auto& clip : track->clips) {
        // An LFO clip points at its output binding from the clip itself, not
        // through a lane -- without this it kept a dangling id and silently
        // stopped driving anything.
        if(clip.lfoOutputBindingId == bindingId) clip.lfoOutputBindingId.clear();
        for(auto& lane : clip.lanes) {
            lane.bindingIds.erase(std::remove(lane.bindingIds.begin(), lane.bindingIds.end(), bindingId), lane.bindingIds.end());
            if(lane.pianoPitchBindingId == bindingId) lane.pianoPitchBindingId.clear();
            if(lane.pianoGateBindingId == bindingId) lane.pianoGateBindingId.clear();
            if(lane.pianoVelocityBindingId == bindingId) lane.pianoVelocityBindingId.clear();
            if(lane.drumDurationBindingId == bindingId) lane.drumDurationBindingId.clear();
            if(lane.drumFilterBindingId == bindingId) lane.drumFilterBindingId.clear();
            if(lane.drumStutterBindingId == bindingId) lane.drumStutterBindingId.clear();
        }
    }
    track->bindings.erase(it);
    normalizeNoteGroups(*track);
    invalidateEvaluationIndexes();
    refreshTimelineFlag(parameterPath);
    return true;
}

bool ofxOceanodeTimelineManager::setBindingMode(const std::string& trackId,
                                                const std::string& bindingId,
                                                ofxOceanodeTimelineAutomationMode mode) {
    auto* binding = getBinding(trackId, bindingId);
    if(binding == nullptr) return false;
    binding->mode = mode;
    return true;
}

bool ofxOceanodeTimelineManager::setBindingClamp(const std::string& trackId,
                                                 const std::string& bindingId,
                                                 bool clampToParameterRange) {
    auto* binding = getBinding(trackId, bindingId);
    if(binding == nullptr) return false;
    binding->clampToParameterRange = clampToParameterRange;
    return true;
}

int ofxOceanodeTimelineManager::captureStateAsCurvePoints(const std::string& trackId,
                                                          const std::string& bindingId,
                                                          double timelineBeat) {
    auto* track = getTrack(trackId);
    if(track == nullptr || track->isWaveTrack || container == nullptr) return 0;

    struct CaptureTarget {
        std::string bindingId;
        std::string parameterPath;
        std::string valueType;
        float value = 0.0f;
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;
        bool boundedRange = true;
    };
    std::vector<CaptureTarget> targets;
    targets.reserve(bindingId.empty() ? track->bindings.size() : 1);

    for(const auto& binding : track->bindings) {
        if(!bindingId.empty() && binding.id != bindingId) continue;
        auto* parameter = container->findTimelineParameter(binding.parameterPath);
        if(parameter == nullptr) continue;

        CaptureTarget target;
        target.bindingId = binding.id;
        target.parameterPath = binding.parameterPath;
        target.valueType = binding.valueType;
        if(binding.valueType == typeid(float).name()) {
            auto& value = parameter->cast<float>().getParameter();
            target.value = value.get();
            target.rangeMin = value.getMin();
            target.rangeMax = value.getMax();
            target.boundedRange = std::isfinite(target.rangeMin) && std::isfinite(target.rangeMax) &&
                target.rangeMax > target.rangeMin &&
                target.rangeMin > std::numeric_limits<float>::lowest() * 0.5f &&
                target.rangeMax < std::numeric_limits<float>::max() * 0.5f;
        } else if(binding.valueType == typeid(int).name()) {
            auto& value = parameter->cast<int>().getParameter();
            target.value = static_cast<float>(value.get());
            target.rangeMin = static_cast<float>(value.getMin());
            target.rangeMax = static_cast<float>(value.getMax());
            target.boundedRange = value.getMin() != std::numeric_limits<int>::lowest() &&
                value.getMax() != std::numeric_limits<int>::max() && value.getMax() > value.getMin();
        } else if(binding.valueType == typeid(bool).name()) {
            target.value = parameter->cast<bool>().getParameter().get() ? 1.0f : 0.0f;
            target.rangeMin = 0.0f;
            target.rangeMax = 1.0f;
        } else if(binding.valueType == typeid(std::vector<float>).name()) {
            auto& value = parameter->cast<std::vector<float>>().getParameter();
            const auto current = value.get();
            const auto minimum = value.getMin();
            const auto maximum = value.getMax();
            target.value = current.empty() ? 0.0f : current.front();
            target.rangeMin = minimum.empty() ? std::min(0.0f, target.value) : minimum.front();
            target.rangeMax = maximum.empty() ? std::max(1.0f, target.value) : maximum.front();
            target.boundedRange = !minimum.empty() && !maximum.empty() &&
                std::isfinite(target.rangeMin) && std::isfinite(target.rangeMax) &&
                target.rangeMax > target.rangeMin &&
                target.rangeMin > std::numeric_limits<float>::lowest() * 0.5f &&
                target.rangeMax < std::numeric_limits<float>::max() * 0.5f;
        } else if(binding.valueType == typeid(std::vector<int>).name()) {
            auto& value = parameter->cast<std::vector<int>>().getParameter();
            const auto current = value.get();
            const auto minimum = value.getMin();
            const auto maximum = value.getMax();
            target.value = current.empty() ? 0.0f : static_cast<float>(current.front());
            target.rangeMin = minimum.empty() ? std::min(0.0f, target.value) : static_cast<float>(minimum.front());
            target.rangeMax = maximum.empty() ? std::max(1.0f, target.value) : static_cast<float>(maximum.front());
            target.boundedRange = !minimum.empty() && !maximum.empty() &&
                minimum.front() != std::numeric_limits<int>::lowest() &&
                maximum.front() != std::numeric_limits<int>::max() && maximum.front() > minimum.front();
        } else if(binding.valueType == typeid(std::vector<bool>).name()) {
            auto& value = parameter->cast<std::vector<bool>>().getParameter();
            const auto current = value.get();
            target.value = !current.empty() && current.front() ? 1.0f : 0.0f;
            target.rangeMin = 0.0f;
            target.rangeMax = 1.0f;
        } else {
            continue; // Text values cannot be interpolated as a curve.
        }

        if(!target.boundedRange) {
            target.rangeMin = std::min(0.0f, target.value);
            target.rangeMax = std::max(1.0f, target.value);
            if(target.rangeMax - target.rangeMin < 1e-6f) target.rangeMax = target.rangeMin + 1.0f;
        }
        targets.push_back(std::move(target));
    }
    if(targets.empty()) return 0;

    timelineBeat = std::max(0.0, timelineBeat);
    constexpr double captureTail = 1.0 / 24.0;
    std::string fallbackCaptureClipId;
    for(const auto& clip : track->clips) {
        if(!clip.isStateCapture) continue;
        fallbackCaptureClipId = clip.id;
        break;
    }
    if(fallbackCaptureClipId.empty()) {
        fallbackCaptureClipId = createClip(trackId, "Captured State", timelineBeat, captureTail);
        auto* created = getClip(trackId, fallbackCaptureClipId);
        if(created == nullptr) return 0;
        created->isStateCapture = true;
        created->repeatContent = false;
    }

    // A shared capture clip remains shared until the user explicitly
    // separates it. Once separated, locate each parameter's own destination
    // by its binding so later track-level captures preserve that independence.
    std::vector<std::string> targetClipIds;
    targetClipIds.reserve(targets.size());
    for(const auto& target : targets) {
        std::string exactClipId;
        if(const auto* currentTrack = getTrack(trackId)) {
            for(const auto& clip : currentTrack->clips) {
                if(!clip.isStateCapture) continue;
                const bool ownsBinding = std::any_of(clip.lanes.begin(), clip.lanes.end(), [&](const auto& lane) {
                    return lane.type == ofxOceanodeTimelineLaneType::Curve &&
                        std::find(lane.bindingIds.begin(), lane.bindingIds.end(), target.bindingId) != lane.bindingIds.end();
                });
                if(ownsBinding) {
                    exactClipId = clip.id;
                    break;
                }
            }
        }
        targetClipIds.push_back(exactClipId.empty() ? fallbackCaptureClipId : exactClipId);
    }

    // Bake any user stretch into absolute point positions before growing each
    // distinct destination clip. This is deliberately per clip: separated
    // parameter clips may have been moved or stretched independently.
    std::unordered_map<std::string, double> captureSourceBeats;
    for(const auto& captureClipId : targetClipIds) {
        if(captureSourceBeats.count(captureClipId) > 0) continue;
        auto* captureClip = getClip(trackId, captureClipId);
        if(captureClip == nullptr) continue;
        std::vector<std::vector<double>> absolutePointBeats(captureClip->lanes.size());
        for(size_t laneIndex = 0; laneIndex < captureClip->lanes.size(); ++laneIndex) {
            const auto& lane = captureClip->lanes[laneIndex];
            absolutePointBeats[laneIndex].reserve(lane.curvePoints.size());
            for(const auto& point : lane.curvePoints)
                absolutePointBeats[laneIndex].push_back(
                    ofxOceanodeTimelineClipTime::sourceToTimelineBeat(*captureClip, point.beat));
        }
        const double oldEnd = captureClip->startBeat + captureClip->durationBeats;
        const double newStart = std::min(captureClip->startBeat, timelineBeat);
        const double newEnd = std::max(oldEnd, timelineBeat + captureTail);
        for(size_t laneIndex = 0; laneIndex < captureClip->lanes.size(); ++laneIndex) {
            auto& lane = captureClip->lanes[laneIndex];
            for(size_t pointIndex = 0; pointIndex < lane.curvePoints.size(); ++pointIndex)
                lane.curvePoints[pointIndex].beat = absolutePointBeats[laneIndex][pointIndex] - newStart;
        }
        captureClip->startBeat = newStart;
        captureClip->durationBeats = std::max(captureTail, newEnd - newStart);
        captureClip->contentDurationBeats = captureClip->durationBeats;
        captureClip->contentStretch = 1.0;
        captureClip->repeatContent = false;
        captureSourceBeats[captureClipId] = timelineBeat - newStart;
    }

    auto remapCurveRange = [](ofxOceanodeTimelineLane& lane, float newMin, float newMax) {
        if(newMax - newMin < 1e-9f) newMax = newMin + 1.0f;
        const float oldMin = lane.valueMin;
        const float oldSpan = lane.valueMax - lane.valueMin;
        const float newSpan = newMax - newMin;
        for(auto& point : lane.curvePoints) {
            const float actual = std::abs(oldSpan) < 1e-9f ? oldMin : oldMin + point.value * oldSpan;
            point.value = ofClamp((actual - newMin) / newSpan, 0.0f, 1.0f);
        }
        lane.valueMin = newMin;
        lane.valueMax = newMax;
    };
    auto setCurvePoint = [](ofxOceanodeTimelineLane& lane, double beat, float value) {
        const auto existing = std::lower_bound(lane.curvePoints.begin(), lane.curvePoints.end(), beat,
            [](const auto& point, double candidate) { return point.beat < candidate; });
        if(existing != lane.curvePoints.end() && std::abs(existing->beat - beat) <= 1e-6) {
            existing->value = value;
            return;
        }
        const size_t oldCount = lane.curvePoints.size();
        auto oldTensions = lane.curveTensions;
        oldTensions.resize(oldCount > 0 ? oldCount - 1 : 0);
        const size_t insertionIndex = static_cast<size_t>(existing - lane.curvePoints.begin());
        lane.curvePoints.insert(existing, {beat, value});
        std::vector<ofxOceanodeTimelineCurveTension> tensions(
            lane.curvePoints.size() > 0 ? lane.curvePoints.size() - 1 : 0);
        for(size_t segment = 0; segment < tensions.size(); ++segment) {
            if((insertionIndex > 0 && segment == insertionIndex - 1) || segment == insertionIndex) continue;
            const size_t oldSegment = segment < insertionIndex ? segment : segment - 1;
            if(oldSegment < oldTensions.size()) tensions[segment] = oldTensions[oldSegment];
        }
        lane.curveTensions = std::move(tensions);
    };

    int captured = 0;
    for(size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        const auto& target = targets[targetIndex];
        const auto& captureClipId = targetClipIds[targetIndex];
        auto* captureClip = getClip(trackId, captureClipId); // createLane may move lane storage.
        const auto sourceBeatIt = captureSourceBeats.find(captureClipId);
        if(captureClip == nullptr || sourceBeatIt == captureSourceBeats.end()) continue;
        const double sourceBeat = sourceBeatIt->second;
        ofxOceanodeTimelineLane* lane = nullptr;
        for(auto& candidate : captureClip->lanes) {
            if(candidate.type == ofxOceanodeTimelineLaneType::Curve &&
               std::find(candidate.bindingIds.begin(), candidate.bindingIds.end(), target.bindingId) != candidate.bindingIds.end()) {
                lane = &candidate;
                break;
            }
        }
        if(lane == nullptr) {
            const auto laneId = createLane(trackId, captureClipId, target.parameterPath,
                                           ofxOceanodeTimelineLaneType::Curve);
            if(laneId.empty() || !addBindingToLane(trackId, captureClipId, laneId, target.bindingId)) continue;
            lane = getLane(trackId, captureClipId, laneId);
            if(lane == nullptr) continue;
            lane->curvePoints.clear(); // remove createLane's generic diagonal.
            lane->curveTensions.clear();
            lane->valueMin = target.rangeMin;
            lane->valueMax = target.rangeMax;
            lane->curveInterpolation = target.valueType == typeid(bool).name() ||
                target.valueType == typeid(std::vector<bool>).name() ? "Step" : "Linear";
        } else if(target.boundedRange) {
            if(std::abs(lane->valueMin - target.rangeMin) > 1e-6f ||
               std::abs(lane->valueMax - target.rangeMax) > 1e-6f)
                remapCurveRange(*lane, target.rangeMin, target.rangeMax);
        } else if(target.value < lane->valueMin || target.value > lane->valueMax) {
            remapCurveRange(*lane, std::min(lane->valueMin, target.value),
                            std::max(lane->valueMax, target.value));
        }

        const float span = lane->valueMax - lane->valueMin;
        const bool decibels = ofxOceanodeTimelineScale::isDecibels(*lane, getBinding(trackId, target.bindingId));
        const float normalized = decibels ? ofxOceanodeTimelineScale::gainToNormalized(*lane, target.value)
            : std::abs(span) < 1e-9f ? 0.0f
            : ofClamp((target.value - lane->valueMin) / span, 0.0f, 1.0f);
        setCurvePoint(*lane, sourceBeat, normalized);

        const std::string overrideKey = trackId + "\x1f" + target.bindingId;
        if(stateCaptureManualOverrideKeys.erase(overrideKey) > 0) {
            if(auto* binding = getBinding(trackId, target.bindingId)) {
                binding->hasLiveOverride = false;
                binding->liveOverrideValue.clear();
            }
        }
        if(auto* parameter = container->findTimelineParameter(target.parameterPath))
            stateCaptureLastAppliedValues[target.parameterPath] = parameterValueForAutomation(*parameter);
        ++captured;
    }

    if(auto* sortedTrack = getTrack(trackId)) {
        std::sort(sortedTrack->clips.begin(), sortedTrack->clips.end(), [](const auto& a, const auto& b) {
            if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
            return a.id < b.id;
        });
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return captured;
}

std::string ofxOceanodeTimelineManager::createClip(const std::string& trackId,
                                                   const std::string& requestedName,
                                                   double startBeat,
                                                   double durationBeats) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return std::string();

    ofxOceanodeTimelineClip clip;
    clip.id = makeUniqueClipId();
    clip.name = makeUniqueClipName(*track, requestedName);
    clip.startBeat = std::max(0.0, startBeat);
    clip.durationBeats = std::max(1.0 / 24.0, durationBeats);
    clip.contentDurationBeats = clip.durationBeats;
    ++nextClipNumber;
    track->clips.push_back(std::move(clip));
    invalidateEvaluationIndexes();
    return track->clips.back().id;
}

std::string ofxOceanodeTimelineManager::createLfoClip(const std::string& trackId,
                                                      const std::string& outputBindingId,
                                                      const std::string& requestedName,
                                                      double startBeat,
                                                      double durationBeats) {
    const auto clipId = createClip(trackId, requestedName, startBeat, durationBeats);
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return std::string();

    clip->isLfo = true;
    clip->lfoOutputBindingId = outputBindingId;
    if(const auto* binding = getBinding(trackId, outputBindingId)) {
        if(container != nullptr) {
            if(auto* parameter = container->findTimelineParameter(binding->parameterPath)) {
                if(binding->valueType == typeid(float).name()) {
                    clip->lfoOutputMin = parameter->cast<float>().getParameter().getMin();
                    clip->lfoOutputMax = parameter->cast<float>().getParameter().getMax();
                } else if(binding->valueType == typeid(int).name()) {
                    clip->lfoOutputMin = static_cast<float>(parameter->cast<int>().getParameter().getMin());
                    clip->lfoOutputMax = static_cast<float>(parameter->cast<int>().getParameter().getMax());
                }
            }
        }
    }

    ensureLfoLanes(trackId, clipId);
    return clipId;
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

void ofxOceanodeTimelineManager::ensureLfoLanes(const std::string& trackId, const std::string& clipId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr || !clip->isLfo) return;
    for(const auto& definition : kLfoParameters) {
        const bool present = std::any_of(clip->lanes.begin(), clip->lanes.end(),
            [&](const auto& lane) { return lane.lfoParameter == definition.id; });
        if(present) continue;
        const auto laneId = createLane(trackId, clipId, definition.name, ofxOceanodeTimelineLaneType::Curve);
        clip = getClip(trackId, clipId); // createLane may reallocate
        if(auto* lane = getLane(trackId, clipId, laneId)) {
            lane->lfoParameter = definition.id;
            lane->valueMin = definition.minimum;
            lane->valueMax = definition.maximum;
            if(lane->lfoParameter == "frequency") lane->lfoSnap = true;
            const float normalized = lfoUnmapValue(definition.minimum, definition.maximum,
                                                   lfoLaneIsLog(*lane), definition.defaultValue);
            lane->curvePoints = {{0.0, normalized}, {clip->contentDurationBeats, normalized}};
            lane->curveTensions.assign(1, ofxOceanodeTimelineCurveTension{});
        }
    }
}

namespace {
// ---- Clip splitting helpers (all positions in a clip's source beats) ----

// The part of a curve between from and to, moved to placeAt. Boundary
// points carry the curve's value there; segments kept whole keep their
// tension, and a cut segment gets the tension that best reproduces its
// part of the original shape.
void appendCurveWindow(const ofxOceanodeTimelineLane& lane, double from, double to, double placeAt,
                       std::vector<ofxOceanodeTimelineCurvePoint>& outPoints,
                       std::vector<ofxOceanodeTimelineCurveTension>& outTensions) {
    auto points = lane.curvePoints;
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    const float fallback = points.empty() ? 0.0f : points.front().value;
    auto valueAt = [&](double beat) {
        return valueAtBeat(points, lane.curveTensions, lane.curveInterpolation, beat, fallback);
    };
    const auto mode = curveInterpolationMode(lane.curveInterpolation);
    auto fitTension = [&](double x0, double x1) {
        ofxOceanodeTimelineCurveTension best{};
        if(mode == CurveInterpolationMode::Linear || mode == CurveInterpolationMode::Step) return best;
        const float v0 = valueAt(x0), v1 = valueAt(x1);
        if(std::abs(v1 - v0) < 1e-6f) return best;
        constexpr int kSamples = 16;
        float target[kSamples];
        for(int i = 0; i < kSamples; ++i) target[i] = valueAt(x0 + (x1 - x0) * (i + 1) / (kSamples + 1.0));
        double bestError = 1e30;
        for(int a = 0; a < 50; ++a) {
            const float inflection = 0.01f + a * (0.98f / 49.0f);
            for(int b = 0; b < 48; ++b) {
                const float steepness = 0.1f * std::pow(100.0f, b / 47.0f); // 0.1 .. 10
                const ofxOceanodeTimelineCurveTension candidate{inflection, steepness};
                double error = 0.0;
                for(int i = 0; i < kSamples && error < bestError; ++i) {
                    const float u = static_cast<float>((i + 1) / (kSamples + 1.0));
                    const float d = ofLerp(v0, v1, curveSegmentShape(u, mode, candidate)) - target[i];
                    error += d * d;
                }
                if(error < bestError) { bestError = error; best = candidate; }
            }
        }
        // Refine around the best grid point.
        const ofxOceanodeTimelineCurveTension coarse = best;
        for(int a = -20; a <= 20; ++a) {
            const float inflection = ofClamp(coarse.inflection + a * 0.001f, 0.01f, 0.99f);
            for(int b = -20; b <= 20; ++b) {
                const float steepness = ofClamp(coarse.steepness * std::pow(1.005f, static_cast<float>(b)), 0.1f, 10.0f);
                const ofxOceanodeTimelineCurveTension candidate{inflection, steepness};
                double error = 0.0;
                for(int i = 0; i < kSamples && error < bestError; ++i) {
                    const float u = static_cast<float>((i + 1) / (kSamples + 1.0));
                    const float d = ofLerp(v0, v1, curveSegmentShape(u, mode, candidate)) - target[i];
                    error += d * d;
                }
                if(error < bestError) { bestError = error; best = candidate; }
            }
        }
        return best;
    };
    // Emitted points with their original beat and index (-1: a boundary point).
    struct WindowPoint { double originalBeat; float value; int index; };
    std::vector<WindowPoint> window;
    auto pointIndexAt = [&](double beat) {
        for(size_t i = 0; i < points.size(); ++i)
            if(std::abs(points[i].beat - beat) <= kEpsilon) return static_cast<int>(i);
        return -1;
    };
    window.push_back({from, valueAt(from), pointIndexAt(from)});
    for(size_t i = 0; i < points.size(); ++i) {
        if(points[i].beat > from + kEpsilon && points[i].beat < to - kEpsilon)
            window.push_back({points[i].beat, points[i].value, static_cast<int>(i)});
    }
    window.push_back({to, valueAt(to), pointIndexAt(to)});
    // Joining a second window: the previous window's last point stays and a
    // new point at the same beat starts this one (a jump, as at a cycle wrap).
    if(!outPoints.empty()) outTensions.push_back({});
    for(size_t i = 0; i < window.size(); ++i) {
        if(i > 0) {
            const int a = window[i - 1].index, b = window[i].index;
            const bool wholeSegment = a >= 0 && b == a + 1 && static_cast<size_t>(a) < lane.curveTensions.size();
            outTensions.push_back(wholeSegment ? lane.curveTensions[a]
                                               : fitTension(window[i - 1].originalBeat, window[i].originalBeat));
        }
        outPoints.push_back({placeAt + window[i].originalBeat - from, window[i].value});
    }
}

// Events ([start, start + length)) that overlap [from, to), clipped to it and moved to placeAt.
template<typename Event>
void appendEventWindow(const std::vector<Event>& events, double from, double to, double placeAt,
                       std::vector<Event>& out) {
    for(const auto& event : events) {
        const double start = std::max(event.startBeat, from);
        const double end = std::min(event.startBeat + event.durationBeats, to);
        if(end - start <= 1e-6) continue;
        Event piece = event;
        piece.startBeat = placeAt + start - from;
        piece.durationBeats = end - start;
        out.push_back(piece);
    }
}
}

bool ofxOceanodeTimelineManager::computeClipSplit(const ofxOceanodeTimelineClip& original, double timelineBeat,
                                                  ofxOceanodeTimelineClip& left, ofxOceanodeTimelineClip& right) const {
    const double minimum = 1.0 / 96.0;
    const double clipEnd = original.startBeat + original.durationBeats;
    if(timelineBeat <= original.startBeat + minimum || timelineBeat >= clipEnd - minimum) return false;

    const double content = ofxOceanodeTimelineClipTime::sourceDuration(original);
    const bool repeating = original.repeatContent;
    // Source beat at the cut. A repeating clip keeps its content and the
    // right part starts that far into it; a stretched one-shot is cut there.
    const double cut = ofClamp(ofxOceanodeTimelineClipTime::timelineToSourceBeat(original, timelineBeat), 0.0, content);

    left = original;
    right = original;
    left.durationBeats = timelineBeat - original.startBeat;
    right.startBeat = timelineBeat;
    right.durationBeats = clipEnd - timelineBeat;
    if(!repeating) {
        left.contentDurationBeats = std::max(1.0 / 24.0, cut);
        right.contentDurationBeats = std::max(1.0 / 24.0, content - cut);
    }

    for(size_t laneIndex = 0; laneIndex < original.lanes.size(); ++laneIndex) {
        const auto& lane = original.lanes[laneIndex];
        auto& leftLane = left.lanes[laneIndex];
        auto& rightLane = right.lanes[laneIndex];
        // Right part, as source ranges of the original placed one after the
        // other: [cut, content) then, when repeating, [0, cut).
        struct Piece { double from, to, placeAt; };
        std::vector<Piece> rightPieces = {{cut, content, 0.0}};
        if(repeating && cut > kEpsilon) rightPieces.push_back({0.0, cut, content - cut});

        const bool curveData = !lane.curvePoints.empty() &&
            (lane.type == ofxOceanodeTimelineLaneType::Curve || ofxOceanodeTimelineIsHeldValueLane(lane.type) || original.isLfo);
        if(curveData && lane.curveInterpolation != "Value") {
            rightLane.curvePoints.clear();
            rightLane.curveTensions.clear();
            for(const auto& piece : rightPieces)
                appendCurveWindow(lane, piece.from, piece.to, piece.placeAt, rightLane.curvePoints, rightLane.curveTensions);
            if(!repeating) {
                leftLane.curvePoints.clear();
                leftLane.curveTensions.clear();
                appendCurveWindow(lane, 0.0, cut, 0.0, leftLane.curvePoints, leftLane.curveTensions);
            }
        }
        auto splitEvents = [&](const auto& events, auto& leftEvents, auto& rightEvents) {
            rightEvents.clear();
            for(const auto& piece : rightPieces) appendEventWindow(events, piece.from, piece.to, piece.placeAt, rightEvents);
            if(!repeating) {
                leftEvents.clear();
                appendEventWindow(events, 0.0, cut, 0.0, leftEvents);
            }
        };
        splitEvents(lane.pianoNotes, leftLane.pianoNotes, rightLane.pianoNotes);
        for(size_t row = 0; row < lane.multiValueRows.size(); ++row)
            splitEvents(lane.multiValueRows[row], leftLane.multiValueRows[row], rightLane.multiValueRows[row]);
        for(size_t row = 0; row < lane.multiGateRows.size(); ++row)
            splitEvents(lane.multiGateRows[row], leftLane.multiGateRows[row], rightLane.multiGateRows[row]);

        // Step patterns repeat every pattern length inside the content: the
        // right part's pattern starts that far into it (steps crossing the
        // start are split in two).
        const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
        const double patternLength = std::max(cellLength, lane.stepCount * cellLength);
        const double patternShift = positiveModulo(cut, patternLength);
        if(!lane.step.steps.empty() && patternShift > kEpsilon) {
            rightLane.step.steps.clear();
            for(const auto& step : lane.step.steps) {
                const double duration = step.durationBeats > kEpsilon ? step.durationBeats : cellLength;
                const double start = step.startBeat;
                const double end = start + duration;
                auto addStep = [&](double s, double d, bool whole) {
                    if(d <= 1e-6) return;
                    auto piece = step;
                    piece.startBeat = s;
                    if(!whole) piece.durationBeats = d; // a whole step keeps "one cell" (0) as is
                    rightLane.step.steps.push_back(piece);
                };
                if(start >= patternShift - kEpsilon) addStep(start - patternShift, duration, true);
                else if(end > patternShift + kEpsilon) {
                    addStep(0.0, end - patternShift, false);
                    addStep(start - patternShift + patternLength, patternShift - start, false);
                } else addStep(start - patternShift + patternLength, duration, true);
            }
            rightLane.step.sortSteps();
        }
        if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid && cut > kEpsilon) {
            // Each row continues from where its pattern was at the cut.
            for(auto& row : rightLane.drumRows) {
                const double shift = positiveModulo(cut, row.length);
                for(auto& step : row.steps)
                    if(!ofxOceanodeTimelineDrumGrid::stepHidden(row, step)) step.beat = positiveModulo(step.beat - shift, row.length);
                ofxOceanodeTimelineDrumGrid::normalizeRow(row);
            }
        }
        if(lane.type == ofxOceanodeTimelineLaneType::MultiSlider && !lane.multiSliderValues.empty()) {
            const int count = static_cast<int>(lane.multiSliderValues.size());
            const int shiftCells = static_cast<int>(std::llround(patternShift / cellLength)) % std::max(1, count);
            std::rotate(rightLane.multiSliderValues.begin(), rightLane.multiSliderValues.begin() + shiftCells,
                        rightLane.multiSliderValues.end());
        }
    }

    // An LFO restarts its phase at the start of a clip: carry the phase it
    // had reached at the cut into the right part's phase offset.
    if(original.isLfo) {
        const double cycles = ofxOceanodeTimelineLfo::cyclesAt(original, cut);
        const float phase = static_cast<float>(cycles - std::floor(cycles));
        for(auto& lane : right.lanes) {
            if(lane.lfoParameter != "phaseOffset") continue;
            auto wrap = [&](float value) { float v = value + phase; return v - std::floor(v); };
            if(lane.curveInterpolation == "Value") lane.lfoValue = wrap(lane.lfoValue);
            else for(auto& point : lane.curvePoints) point.value = wrap(point.value);
        }
    }

    return true;
}

std::string ofxOceanodeTimelineManager::splitClip(const std::string& trackId, const std::string& clipId, double timelineBeat) {
    auto* track = getTrack(trackId);
    const auto* sourceClip = getClip(trackId, clipId);
    if(track == nullptr || sourceClip == nullptr || track->isWaveTrack) return std::string();
    const ofxOceanodeTimelineClip original = *sourceClip;
    ofxOceanodeTimelineClip left, right;
    if(!computeClipSplit(original, timelineBeat, left, right)) return std::string();
    // Keep the original/left half as the track's capture destination. The
    // right half is an ordinary editable curve clip after the split.
    right.isStateCapture = false;

    // Splitting dissolves a clip group: the halves become independent.
    if(getGroupForClip(trackId, clipId) != nullptr) ungroupClip(trackId, clipId);

    const std::string rightId = createClip(trackId, original.name, right.startBeat, right.durationBeats);
    if(rightId.empty()) return std::string();
    auto* rightClip = getClip(trackId, rightId);
    const std::string rightName = rightClip->name;
    auto rightLanes = std::move(right.lanes);
    right.lanes.clear();
    right.id = rightId;
    right.name = rightName;
    *rightClip = right;
    for(auto& lane : rightLanes) {
        const std::string laneId = makeUniqueLaneId();
        ++nextLaneNumber;
        lane.id = laneId;
        if(auto* clip = getClip(trackId, rightId)) clip->lanes.push_back(lane);
    }
    if(auto* leftClip = getClip(trackId, clipId)) *leftClip = left;

    if(auto* sortedTrack = getTrack(trackId)) {
        std::sort(sortedTrack->clips.begin(), sortedTrack->clips.end(), [](const auto& a, const auto& b) {
            if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
            return a.id < b.id;
        });
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return rightId;
}

std::string ofxOceanodeTimelineManager::duplicateClip(const std::string& srcTrackId, const std::string& clipId,
                                                      const std::string& dstTrackId, double newStartBeat,
                                                      const std::string& srcLaneId,
                                                      const std::string& dstBindingId) {
    const auto* srcTrack = getTrack(srcTrackId);
    const auto* dstTrack = getTrack(dstTrackId);
    const auto* sourceClip = getClip(srcTrackId, clipId);
    if(srcTrack == nullptr || dstTrack == nullptr || sourceClip == nullptr) return std::string();
    if(srcTrack->isWaveTrack != dstTrack->isWaveTrack) return std::string();
    // A value copy: createClip below may reallocate the source track's clips.
    ofxOceanodeTimelineClip copy = *sourceClip;
    // A duplicate is ordinary automation; otherwise the next capture would
    // have two ambiguous destination clips on the same track.
    copy.isStateCapture = false;
    const bool sameTrack = srcTrackId == dstTrackId;

    // The destination track's binding for the parameter a source binding drives.
    auto mapBinding = [&](const std::string& bindingId) -> std::string {
        if(bindingId.empty() || sameTrack) return bindingId;
        const auto* binding = getBinding(srcTrackId, bindingId);
        if(binding == nullptr) return std::string();
        for(const auto& candidate : dstTrack->bindings)
            if(candidate.parameterPath == binding->parameterPath) return candidate.id;
        return std::string();
    };
    auto mapIds = [&](std::vector<std::string>& ids) {
        std::vector<std::string> mapped;
        for(const auto& id : ids) {
            const std::string target = mapBinding(id);
            if(!target.empty() && std::find(mapped.begin(), mapped.end(), target) == mapped.end()) mapped.push_back(target);
        }
        ids = std::move(mapped);
    };

    if(!dstBindingId.empty()) {
        if(dstTrack->isWaveTrack || getBinding(dstTrackId, dstBindingId) == nullptr) return std::string();
        if(copy.isLfo) {
            // The lanes are the oscillator's own controls; only the output moves.
            copy.lfoOutputBindingId = dstBindingId;
            for(auto& lane : copy.lanes) mapIds(lane.bindingIds);
        } else {
            const auto laneIt = std::find_if(copy.lanes.begin(), copy.lanes.end(),
                                             [&](const auto& lane) { return lane.id == srcLaneId; });
            if(laneIt == copy.lanes.end()) return std::string();
            ofxOceanodeTimelineLane lane = *laneIt;
            if(ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)) {
                const auto* group = findNoteGroupForBinding(*dstTrack, dstBindingId);
                if(group == nullptr) return std::string();
                lane.pianoPitchBindingId = group->pitchBindingId;
                lane.pianoGateBindingId = group->gateBindingId;
                lane.pianoVelocityBindingId = group->velocityBindingId;
                lane.drumDurationBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group->durationBindingId : std::string();
                lane.drumFilterBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group->filterBindingId : std::string();
                lane.drumStutterBindingId = lane.type == ofxOceanodeTimelineLaneType::DrumGrid ? group->stutterBindingId : std::string();
                lane.bindingIds = group->members();
            } else {
                lane.bindingIds = {dstBindingId};
            }
            copy.lanes = {lane};
        }
    } else if(!sameTrack && !dstTrack->isWaveTrack) {
        if(copy.isLfo) {
            copy.lfoOutputBindingId = mapBinding(copy.lfoOutputBindingId);
            if(copy.lfoOutputBindingId.empty()) return std::string();
            for(auto& lane : copy.lanes) mapIds(lane.bindingIds);
        } else {
            std::vector<ofxOceanodeTimelineLane> kept;
            for(auto lane : copy.lanes) {
                mapIds(lane.bindingIds);
                if(ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)) {
                    lane.pianoPitchBindingId = mapBinding(lane.pianoPitchBindingId);
                    lane.pianoGateBindingId = mapBinding(lane.pianoGateBindingId);
                    lane.drumDurationBindingId = mapBinding(lane.drumDurationBindingId);
                    lane.drumFilterBindingId = mapBinding(lane.drumFilterBindingId);
                    lane.drumStutterBindingId = mapBinding(lane.drumStutterBindingId);
                    lane.pianoVelocityBindingId = mapBinding(lane.pianoVelocityBindingId);
                }
                if(!lane.bindingIds.empty()) kept.push_back(std::move(lane));
            }
            if(kept.empty()) return std::string();
            copy.lanes = std::move(kept);
        }
    }

    const std::string newId = createClip(dstTrackId, copy.name, newStartBeat, copy.durationBeats);
    auto* newClip = getClip(dstTrackId, newId);
    if(newClip == nullptr) return std::string();
    copy.id = newId;
    copy.name = newClip->name;
    copy.startBeat = std::max(0.0, newStartBeat);
    // Lanes go in one at a time: makeUniqueLaneId() only avoids ids already
    // in the timeline, so ids handed out before insertion could repeat.
    auto lanes = std::move(copy.lanes);
    copy.lanes.clear();
    *newClip = std::move(copy);
    for(auto& lane : lanes) {
        lane.id = makeUniqueLaneId();
        ++nextLaneNumber;
        if(auto* clip = getClip(dstTrackId, newId)) clip->lanes.push_back(std::move(lane));
    }
    invalidateSchedule();
    return newId;
}

int ofxOceanodeTimelineManager::separateStateCaptureClip(const std::string& trackId,
                                                         const std::string& clipId) {
    const auto* source = getClip(trackId, clipId);
    const auto* track = getTrack(trackId);
    if(source == nullptr || track == nullptr || track->isWaveTrack || source->isLfo ||
       !source->isStateCapture || source->lanes.size() < 2) return 0;

    // Copy the descriptors before duplicateClip() grows the track's clip
    // vector and invalidates every pointer/reference into it.
    const double startBeat = source->startBeat;
    const auto lanes = source->lanes;
    for(const auto& lane : lanes)
        if(lane.bindingIds.empty()) return 0;

    std::vector<std::string> createdClipIds;
    createdClipIds.reserve(lanes.size() - 1);
    for(size_t laneIndex = 1; laneIndex < lanes.size(); ++laneIndex) {
        const auto newClipId = duplicateClip(trackId, clipId, trackId, startBeat,
                                             lanes[laneIndex].id,
                                             lanes[laneIndex].bindingIds.front());
        if(newClipId.empty()) {
            for(const auto& createdId : createdClipIds) removeClip(trackId, createdId);
            return 0;
        }
        if(getClip(trackId, newClipId) != nullptr) {
            createdClipIds.push_back(newClipId);
        } else {
            removeClip(trackId, newClipId);
            for(const auto& createdId : createdClipIds) removeClip(trackId, createdId);
            return 0;
        }
    }

    for(const auto& createdId : createdClipIds)
        if(auto* created = getClip(trackId, createdId)) created->isStateCapture = true;
    if(getGroupForClip(trackId, clipId) != nullptr) ungroupClip(trackId, clipId);
    if(auto* original = getClip(trackId, clipId)) {
        original->lanes = {lanes.front()};
        original->isStateCapture = true;
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return static_cast<int>(createdClipIds.size() + 1);
}

bool ofxOceanodeTimelineManager::trimClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat) {
    auto* track = getTrack(trackId);
    auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || track->isWaveTrack) return false;
    ofxOceanodeTimelineClip base = *clip;
    newStartBeat = std::max(0.0, newStartBeat);
    const double end = base.startBeat + base.durationBeats;
    if(newStartBeat >= end - 1.0 / 96.0) return false;
    if(newStartBeat < base.startBeat) {
        // Growing to the left: only repeating content can (it is the same
        // content one cycle earlier). Extend by whole cycles, then cut.
        if(!base.repeatContent) return false;
        const double cycle = ofxOceanodeTimelineClipTime::cycleDuration(base);
        const double cycles = std::ceil((base.startBeat - newStartBeat) / std::max(kEpsilon, cycle));
        base.startBeat -= cycles * cycle;
        base.durationBeats += cycles * cycle;
    }
    ofxOceanodeTimelineClip trimmed = base;
    if(std::abs(newStartBeat - base.startBeat) > 1e-9) {
        ofxOceanodeTimelineClip left;
        if(!computeClipSplit(base, newStartBeat, left, trimmed)) return false;
    }
    trimmed.startBeat = newStartBeat;
    trimmed.durationBeats = end - newStartBeat;
    // Same clip (id, name, lane ids): only its start and content moved.
    *clip = trimmed;
    std::sort(track->clips.begin(), track->clips.end(), [](const auto& a, const auto& b) {
        if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
        return a.id < b.id;
    });
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::extendClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat) {
    auto* track = getTrack(trackId);
    auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || track->isWaveTrack) return false;
    newStartBeat = std::max(0.0, newStartBeat);
    if(newStartBeat >= clip->startBeat - 1e-9 || clip->isLfo) return trimClipStart(trackId, clipId, newStartBeat);

    const double stretch = ofxOceanodeTimelineClipTime::stretch(*clip);
    auto isGridLane = [](const ofxOceanodeTimelineLane& lane) {
        return lane.type == ofxOceanodeTimelineLaneType::Step || lane.type == ofxOceanodeTimelineLaneType::MultiSlider ||
               lane.type == ofxOceanodeTimelineLaneType::DrumGrid;
    };
    // Step grids (Step, Multi Slider) can only grow by whole cells, so the
    // added source time is a whole number of the first grid lane's cells --
    // never more than fits before beat 0.
    const double maxSourceDelta = clip->startBeat / stretch;
    double sourceDelta = (clip->startBeat - newStartBeat) / stretch;
    for(const auto& lane : clip->lanes) {
        if(!isGridLane(lane)) continue;
        const double cell = std::max(1.0 / 24.0, lane.beatsPerStep);
        sourceDelta = std::min(std::round(sourceDelta / cell), std::floor(maxSourceDelta / cell + 1e-9)) * cell;
        break;
    }
    sourceDelta = std::min(sourceDelta, maxSourceDelta);
    if(sourceDelta <= 1e-9) return false;

    for(auto& lane : clip->lanes) {
        for(auto& point : lane.curvePoints) point.beat += sourceDelta;
        for(auto& note : lane.pianoNotes) note.startBeat += sourceDelta;
        for(auto& row : lane.multiValueRows) for(auto& region : row) region.startBeat += sourceDelta;
        for(auto& row : lane.multiGateRows) for(auto& region : row) region.startBeat += sourceDelta;
        if(!isGridLane(lane)) continue;
        // A grid lane with another cell size shifts by as many of its own
        // cells as fit (its pattern may then sit slightly off).
        const double cell = std::max(1.0 / 24.0, lane.beatsPerStep);
        const int addedCells = static_cast<int>(std::llround(sourceDelta / cell));
        if(addedCells <= 0) continue;
        if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid) {
            // Rows keep repeating from the new start: shift each pattern so
            // the hits already placed stay where they were.
            for(auto& row : lane.drumRows) {
                for(auto& step : row.steps)
                    if(!ofxOceanodeTimelineDrumGrid::stepHidden(row, step)) step.beat = positiveModulo(step.beat + addedCells * cell, row.length);
                ofxOceanodeTimelineDrumGrid::normalizeRow(row);
            }
            continue;
        }
        for(auto& step : lane.step.steps) step.startBeat += addedCells * cell;
        lane.step.sortSteps();
        if(lane.type == ofxOceanodeTimelineLaneType::MultiSlider)
            lane.multiSliderValues.insert(lane.multiSliderValues.begin(), static_cast<size_t>(addedCells), 0.0f);
        lane.stepCount += addedCells;
    }

    clip->contentDurationBeats = ofxOceanodeTimelineClipTime::sourceDuration(*clip) + sourceDelta;
    const double end = clip->startBeat + clip->durationBeats;
    clip->startBeat = std::max(0.0, clip->startBeat - sourceDelta * stretch);
    clip->durationBeats = end - clip->startBeat;
    std::sort(track->clips.begin(), track->clips.end(), [](const auto& a, const auto& b) {
        if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
        return a.id < b.id;
    });
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::renameClip(const std::string& trackId, const std::string& clipId,
                                            const std::string& requestedName) {
    auto* track = getTrack(trackId);
    auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || requestedName.empty()) return false;
    if(clip->name == requestedName) return true;
    clip->name = makeUniqueClipName(*track, requestedName);
    return true;
}

bool ofxOceanodeTimelineManager::removeClip(const std::string& trackId, const std::string& clipId) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    if(const auto* clip = getClip(trackId, clipId)) {
        releaseWaveClipIfNeeded(trackId, clip->id);
        if(clip->isStateCapture) {
            for(const auto& lane : clip->lanes) {
                for(const auto& bindingId : lane.bindingIds) {
                    const std::string key = trackId + "\x1f" + bindingId;
                    if(stateCaptureManualOverrideKeys.erase(key) > 0) {
                        if(auto* binding = getBinding(trackId, bindingId)) {
                            binding->hasLiveOverride = false;
                            binding->liveOverrideValue.clear();
                        }
                    }
                    if(const auto* binding = getBinding(trackId, bindingId))
                        stateCaptureLastAppliedValues.erase(binding->parameterPath);
                }
            }
        }
    }
    const auto oldSize = track->clips.size();
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(), [&](const auto& clip) {
        return clip.id == clipId;
    }), track->clips.end());
    const bool removed = track->clips.size() != oldSize;
    if(removed) {
        removeClipFromGroups(trackId, clipId);
        invalidateEvaluationIndexes();
    }
    return removed;
}

void ofxOceanodeTimelineManager::removeClipFromGroups(const std::string& trackId, const std::string& clipId) {
    for(auto& group : clipGroups) {
        group.members.erase(std::remove_if(group.members.begin(), group.members.end(), [&](const auto& member) {
            return member.first == trackId && member.second == clipId;
        }), group.members.end());
    }
    // A "group" of zero or one member is meaningless -- drop it rather than
    // leave an inert record around that the UI's group outline/Ungroup
    // button would otherwise have to special-case.
    clipGroups.erase(std::remove_if(clipGroups.begin(), clipGroups.end(), [](const auto& group) {
        return group.members.size() < 2;
    }), clipGroups.end());
}

std::string ofxOceanodeTimelineManager::groupClips(const std::vector<std::pair<std::string, std::string>>& members) {
    std::vector<std::pair<std::string, std::string>> merged;
    auto addUnique = [&](const std::pair<std::string, std::string>& member) {
        if(std::find(merged.begin(), merged.end(), member) == merged.end()) merged.push_back(member);
    };
    // Re-grouping a mix of already-grouped and ungrouped clips folds
    // everything into one group instead of creating overlapping ones --
    // every group any of the requested clips already belongs to gets
    // dissolved and its members pulled into the new group.
    std::set<std::string> groupIdsToDissolve;
    for(const auto& member : members) {
        if(getClip(member.first, member.second) == nullptr) continue; // stale reference -- skip rather than fail the whole call
        addUnique(member);
        if(const auto* existingGroup = getGroupForClip(member.first, member.second)) {
            groupIdsToDissolve.insert(existingGroup->id);
        }
    }
    for(const auto& group : clipGroups) {
        if(groupIdsToDissolve.count(group.id) == 0) continue;
        for(const auto& member : group.members) {
            // Groups are persisted as references. Do not carry a stale
            // reference into a newly merged group if a clip was deleted by
            // another path before the group metadata was refreshed.
            if(getClip(member.first, member.second) != nullptr) addUnique(member);
        }
    }
    // Do not dissolve an existing group when the requested selection did not
    // produce at least two live clips. In particular, a stale selection must
    // never make a valid group lose its only visible member.
    if(merged.size() < 2) {
        if(groupIdsToDissolve.size() == 1) return *groupIdsToDissolve.begin();
        return std::string();
    }
    clipGroups.erase(std::remove_if(clipGroups.begin(), clipGroups.end(), [&](const auto& group) {
        return groupIdsToDissolve.count(group.id) > 0;
    }), clipGroups.end());

    ofxOceanodeTimelineClipGroup newGroup;
    newGroup.id = makeUniqueGroupId();
    newGroup.members = std::move(merged);
    ++nextGroupNumber;
    clipGroups.push_back(std::move(newGroup));
    return clipGroups.back().id;
}

bool ofxOceanodeTimelineManager::ungroupClip(const std::string& trackId, const std::string& clipId) {
    const auto it = std::find_if(clipGroups.begin(), clipGroups.end(), [&](const auto& group) {
        return std::any_of(group.members.begin(), group.members.end(), [&](const auto& member) {
            return member.first == trackId && member.second == clipId;
        });
    });
    if(it == clipGroups.end()) return false;
    clipGroups.erase(it);
    return true;
}

const ofxOceanodeTimelineClipGroup* ofxOceanodeTimelineManager::getGroupForClip(const std::string& trackId, const std::string& clipId) const {
    for(const auto& group : clipGroups) {
        for(const auto& member : group.members) {
            if(member.first == trackId && member.second == clipId) return &group;
        }
    }
    return nullptr;
}

ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(const std::string& trackId, const std::string& clipId) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(const std::string& trackId, const std::string& clipId) const {
    const auto* track = getTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

std::string ofxOceanodeTimelineManager::createLane(const std::string& trackId, const std::string& clipId,
                                                   const std::string& requestedName,
                                                   ofxOceanodeTimelineLaneType type) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return std::string();
    if(type == ofxOceanodeTimelineLaneType::Wave) return std::string();
    ofxOceanodeTimelineLane lane;
    lane.id = makeUniqueLaneId();
    lane.name = requestedName.empty() ? "Lane" : requestedName;
    lane.type = type;
    lane.probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));
    lane.beatsPerStep = 0.25;
    lane.stepCount = std::max(1, static_cast<int>(std::llround(clip->contentDurationBeats / lane.beatsPerStep)));
    if(ofxOceanodeTimelineIsHeldValueLane(type)) {
        lane.curveInterpolation = "Step";
        lane.curvePoints = {{0.0, 0.0f}};
    } else if(type == ofxOceanodeTimelineLaneType::Curve) {
        lane.curvePoints = {{0.0, 0.0f}, {clip->contentDurationBeats, 1.0f}};
        lane.curveTensions.push_back({0.5f, 1.0f});
    } else if(type == ofxOceanodeTimelineLaneType::MultiValue) {
        lane.multiValueRows.assign(std::max(1, lane.multiRowCount), {});
    } else if(type == ofxOceanodeTimelineLaneType::MultiGate) {
        lane.multiGateRows.assign(std::max(1, lane.multiRowCount), {});
    } else if(type == ofxOceanodeTimelineLaneType::MultiSlider) {
        lane.multiSliderValues.assign(lane.stepCount, 0.0f);
    } else if(type == ofxOceanodeTimelineLaneType::DrumGrid) {
        lane.drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(4, {}, std::min(16.0, clip->contentDurationBeats));
    }
    ++nextLaneNumber;
    clip->lanes.push_back(std::move(lane));
    return clip->lanes.back().id;
}

std::string ofxOceanodeTimelineManager::duplicateLane(const std::string& trackId, const std::string& clipId,
                                                       const std::string& laneId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr || clip->isLfo) return {};
    const auto source = std::find_if(clip->lanes.begin(), clip->lanes.end(),
                                     [&](const auto& lane) { return lane.id == laneId; });
    if(source == clip->lanes.end() || source->type == ofxOceanodeTimelineLaneType::Wave ||
       source->isWaveVolume || !source->lfoParameter.empty()) return {};
    ofxOceanodeTimelineLane copy = *source;
    copy.id = makeUniqueLaneId();
    // Its own rolls: a copy on the source's seed would pass and fail with it.
    copy.probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));
    ++nextLaneNumber;
    const auto newId = copy.id;
    clip->lanes.insert(std::next(source), std::move(copy));
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return newId;
}

bool ofxOceanodeTimelineManager::removeLane(const std::string& trackId, const std::string& clipId,
                                            const std::string& laneId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return false;
    if(const auto* lane = getLane(trackId, clipId, laneId)) {
        if(lane->type == ofxOceanodeTimelineLaneType::Wave) releaseWaveClipIfNeeded(trackId, clipId);
        if(clip->isStateCapture) {
            for(const auto& bindingId : lane->bindingIds) {
                const std::string key = trackId + "\x1f" + bindingId;
                if(stateCaptureManualOverrideKeys.erase(key) == 0) continue;
                if(auto* binding = getBinding(trackId, bindingId)) {
                    binding->hasLiveOverride = false;
                    binding->liveOverrideValue.clear();
                }
            }
        }
    }
    const auto oldSize = clip->lanes.size();
    clip->lanes.erase(std::remove_if(clip->lanes.begin(), clip->lanes.end(), [&](const auto& lane) {
        return lane.id == laneId;
    }), clip->lanes.end());
    if(clip->lanes.size() == oldSize) return false;
    invalidateSchedule();
    return true;
}

void ofxOceanodeTimelineManager::releaseWaveClipIfNeeded(const std::string& trackId, const std::string& clipId) {
    if(waveAudioProvider != nullptr) waveAudioProvider->releaseWaveClip(trackId, clipId);
}

bool ofxOceanodeTimelineManager::setLaneMultiRowCount(const std::string& trackId, const std::string& clipId,
                                                      const std::string& laneId, int rowCount) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr) return false;
    rowCount = ofClamp(rowCount, 1, 16);
    lane->multiRowCount = rowCount;
    // Grow/shrink in place so existing row data survives a resize -- only
    // the vector actually used by this lane's current type; the other one
    // is left alone (and picks up the new count next time the lane is
    // switched to that type, via setClipLaneType's "only if still empty"
    // initialization above).
    if(lane->type == ofxOceanodeTimelineLaneType::MultiValue) lane->multiValueRows.resize(rowCount);
    else if(lane->type == ofxOceanodeTimelineLaneType::MultiGate) lane->multiGateRows.resize(rowCount);
    return true;
}

bool ofxOceanodeTimelineManager::reloadWaveform(const std::string& trackId, const std::string& clipId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return false;
    const bool result = readWaveformCache(clip->waveFilePath, clip->waveNumChannels,
                                          clip->waveFileDurationMs, clip->waveformPeaks);
    // waveFileDurationMs is tempo-independent; waveFileDurationBeats is not.
    // Re-derive the beat-domain length at the tempo in force now and carry
    // this clip's own source range across with it, so a slice keeps pointing
    // at the same audio instead of being clamped (by normalizeWaveClipMapping
    // below) against a length measured at whatever tempo was set when the
    // file was first imported.
    if(container != nullptr && clip->waveFileDurationMs > 0.0) {
        const double bpm = std::max(1.0f, container->getTransportState().bpm);
        const double refreshedBeats = clip->waveFileDurationMs * bpm / 60000.0;
        if(clip->waveFileDurationBeats > kEpsilon) {
            const double ratio = refreshedBeats / clip->waveFileDurationBeats;
            if(std::abs(ratio - 1.0) > 1e-9) {
                clip->waveSourceStartBeat *= ratio;
                clip->contentDurationBeats *= ratio;
            }
        }
        clip->waveFileDurationBeats = refreshedBeats;
        normalizeWaveClipMapping(*clip);
    }
    return result;
}

bool ofxOceanodeTimelineManager::addBindingToLane(const std::string& trackId, const std::string& clipId,
                                                  const std::string& laneId, const std::string& bindingId) {
    auto* track = getTrack(trackId);
    auto* lane = getLane(trackId, clipId, laneId);
    if(track == nullptr || lane == nullptr || getBinding(trackId, bindingId) == nullptr) return false;
    if(std::find(lane->bindingIds.begin(), lane->bindingIds.end(), bindingId) != lane->bindingIds.end()) return true;
    lane->bindingIds.push_back(bindingId);
    if(lane->type == ofxOceanodeTimelineLaneType::DrumGrid) {
        // A kit is played by its gate first; pitch is optional.
        const bool alreadyRoled = lane->pianoPitchBindingId == bindingId || lane->pianoGateBindingId == bindingId ||
                                  lane->drumDurationBindingId == bindingId || lane->drumFilterBindingId == bindingId ||
                                  lane->drumStutterBindingId == bindingId;
        if(!alreadyRoled) {
            if(lane->pianoGateBindingId.empty()) lane->pianoGateBindingId = bindingId;
            else if(lane->pianoPitchBindingId.empty()) lane->pianoPitchBindingId = bindingId;
        }
    } else if(lane->type == ofxOceanodeTimelineLaneType::PianoRoll) {
        // Only auto-assign the next open role slot when this binding hasn't
        // already been explicitly placed into one. The per-role UI combo
        // (Pitch/Gate/Velocity) sets its own role field itself BEFORE
        // calling this function, purely to make sure the binding also ends
        // up in the generic bindingIds list -- without this guard, e.g.
        // assigning "Gate" while Pitch was still empty would silently ALSO
        // assign that same binding to Pitch (the next empty slot found
        // below), corrupting the automation with a stray extra
        // contribution (the note's raw pitch number) any time a note
        // played. Only a fresh lane with no role set at all should get the
        // Pitch-then-Gate-then-Velocity auto-fill.
        const bool alreadyRoled = lane->pianoPitchBindingId == bindingId ||
            lane->pianoGateBindingId == bindingId || lane->pianoVelocityBindingId == bindingId;
        if(!alreadyRoled) {
            if(lane->pianoPitchBindingId.empty()) lane->pianoPitchBindingId = bindingId;
            else if(lane->pianoGateBindingId.empty()) lane->pianoGateBindingId = bindingId;
            else if(lane->pianoVelocityBindingId.empty()) lane->pianoVelocityBindingId = bindingId;
        }
    }
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::removeBindingFromLane(const std::string& trackId, const std::string& clipId,
                                                       const std::string& laneId, const std::string& bindingId) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr) return false;
    const auto oldSize = lane->bindingIds.size();
    lane->bindingIds.erase(std::remove(lane->bindingIds.begin(), lane->bindingIds.end(), bindingId), lane->bindingIds.end());
    if(lane->pianoPitchBindingId == bindingId) lane->pianoPitchBindingId.clear();
    if(lane->pianoGateBindingId == bindingId) lane->pianoGateBindingId.clear();
    if(lane->pianoVelocityBindingId == bindingId) lane->pianoVelocityBindingId.clear();
    if(lane->drumDurationBindingId == bindingId) lane->drumDurationBindingId.clear();
    if(lane->drumFilterBindingId == bindingId) lane->drumFilterBindingId.clear();
    if(lane->drumStutterBindingId == bindingId) lane->drumStutterBindingId.clear();
    const bool removed = lane->bindingIds.size() != oldSize;
    const auto* clip = getClip(trackId, clipId);
    if(removed && clip != nullptr && clip->isStateCapture) {
        const std::string key = trackId + "\x1f" + bindingId;
        if(stateCaptureManualOverrideKeys.erase(key) > 0) {
            if(auto* binding = getBinding(trackId, bindingId)) {
                binding->hasLiveOverride = false;
                binding->liveOverrideValue.clear();
            }
        }
    }
    if(removed) invalidateSchedule();
    return removed;
}

ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(const std::string& trackId, const std::string& clipId,
                                                             const std::string& laneId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(const std::string& trackId, const std::string& clipId,
                                                                    const std::string& laneId) const {
    const auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](const auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

bool ofxOceanodeTimelineManager::setClipLaneType(const std::string& trackId, const std::string& clipId,
                                                 const std::string& laneId, ofxOceanodeTimelineLaneType type) {
    auto* lane = getLane(trackId, clipId, laneId);
    auto* clip = getClip(trackId, clipId);
    if(lane == nullptr) return false;
    // Wave is a track type, not a lane type -- createLane refuses it and
    // loading folds the old Wave-lane format onto the clip, so nothing may
    // reintroduce one here either.
    if(type == ofxOceanodeTimelineLaneType::Wave) return false;
    const auto previousType = lane->type;
    if(clip != nullptr && clip->isStateCapture &&
       previousType == ofxOceanodeTimelineLaneType::Curve && type != previousType) {
        for(const auto& bindingId : lane->bindingIds) {
            const std::string key = trackId + "\x1f" + bindingId;
            if(stateCaptureManualOverrideKeys.erase(key) == 0) continue;
            if(auto* binding = getBinding(trackId, bindingId)) {
                binding->hasLiveOverride = false;
                binding->liveOverrideValue.clear();
            }
        }
    }
    if(previousType == ofxOceanodeTimelineLaneType::Curve && ofxOceanodeTimelineIsHeldValueLane(type)) {
        for(auto& point : lane->curvePoints)
            point.value = lane->valueMin + point.value * (lane->valueMax - lane->valueMin);
    } else if(ofxOceanodeTimelineIsHeldValueLane(previousType) && type == ofxOceanodeTimelineLaneType::Curve) {
        const float span = lane->valueMax - lane->valueMin;
        for(auto& point : lane->curvePoints)
            point.value = std::abs(span) < 1e-9f ? 0.0f : (point.value - lane->valueMin) / span;
    }
    lane->type = type;
    if(ofxOceanodeTimelineIsHeldValueLane(type)) {
        lane->curveInterpolation = "Step";
        if(lane->curvePoints.empty()) lane->curvePoints = {{0.0, 0.0f}};
    }
    // a dropdown holds option indices
    if(type == ofxOceanodeTimelineLaneType::Dropdown) {
        for(auto& point : lane->curvePoints) point.value = std::max(0.0f, std::round(point.value));
    }
    if(previousType == ofxOceanodeTimelineLaneType::Wave && type != ofxOceanodeTimelineLaneType::Wave)
        releaseWaveClipIfNeeded(trackId, clipId);
    if(type == ofxOceanodeTimelineLaneType::PianoRoll) {
        lane->valueMin = 0.0f;
        lane->valueMax = 1.0f;
        if(lane->pianoPitchBindingId.empty() && !lane->bindingIds.empty()) lane->pianoPitchBindingId = lane->bindingIds[0];
        if(lane->pianoGateBindingId.empty() && lane->bindingIds.size() > 1) lane->pianoGateBindingId = lane->bindingIds[1];
        if(lane->pianoVelocityBindingId.empty() && lane->bindingIds.size() > 2) lane->pianoVelocityBindingId = lane->bindingIds[2];
    } else if(type == ofxOceanodeTimelineLaneType::Curve) {
        lane->curveInterpolation = "Linear";
        if(lane->curvePoints.empty()) {
            const double contentDuration = clip == nullptr ? 4.0 : clip->contentDurationBeats;
            lane->curvePoints = {{0.0, 0.0f}, {contentDuration, 1.0f}};
            lane->curveTensions = {{0.5f, 1.0f}};
        }
    } else if(type == ofxOceanodeTimelineLaneType::MultiValue) {
        if(lane->multiValueRows.empty()) lane->multiValueRows.assign(std::max(1, lane->multiRowCount), {});
    } else if(type == ofxOceanodeTimelineLaneType::MultiGate) {
        if(lane->multiGateRows.empty()) lane->multiGateRows.assign(std::max(1, lane->multiRowCount), {});
    } else if(type == ofxOceanodeTimelineLaneType::MultiSlider) {
        if(lane->multiSliderValues.empty()) lane->multiSliderValues.assign(std::max(1, lane->stepCount), 0.0f);
    } else if(type == ofxOceanodeTimelineLaneType::DrumGrid) {
        // A piano lane keeps its roles; a plain lane's first target is the gate.
        if(previousType != ofxOceanodeTimelineLaneType::PianoRoll) {
            if(lane->pianoGateBindingId.empty() && !lane->bindingIds.empty()) lane->pianoGateBindingId = lane->bindingIds[0];
            if(lane->pianoPitchBindingId.empty() && lane->bindingIds.size() > 1 &&
               lane->bindingIds[1] != lane->pianoGateBindingId) lane->pianoPitchBindingId = lane->bindingIds[1];
        }
        if(lane->drumRows.empty()) {
            int voices = 0;
            std::vector<std::string> names;
            const auto* track = getTrack(trackId);
            if(track == nullptr || !drumGridInstrument(*track, *lane, voices, names)) voices = 4;
            lane->drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(std::max(1, voices), names,
                std::min(16.0, clip == nullptr ? 4.0 : clip->contentDurationBeats));
            lane->drumStepFollow = true;
            lane->drumSwingBeats = 0.25;
        }
        invalidateEvaluationIndexes();
    }
    if(type != previousType) invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setClipTiming(const std::string& trackId, const std::string& clipId,
                                               double startBeat, double durationBeats) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return false;
    const double previousStart = clip->startBeat, previousDuration = clip->durationBeats;
    clip->startBeat = std::max(0.0, startBeat);
    clip->durationBeats = std::max(1.0 / 24.0, durationBeats);
    // Resizing a Wave clip stretches its complete source over the new length.
    const auto* track = getTrack(trackId);
    if(track != nullptr && track->isWaveTrack) normalizeWaveClipMapping(*clip);
    // A clip dragged while playing moves notes already queued ahead.
    if(clip->startBeat != previousStart || clip->durationBeats != previousDuration) invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::isTrackSoloed(const ofxOceanodeTimelineTrack& track) const {
    if(playback.drivenSolo.empty()) return track.solo;
    const auto it = playback.drivenSolo.find(PlayKey{0, track.id});
    return it == playback.drivenSolo.end() ? track.solo : it->second;
}

bool ofxOceanodeTimelineManager::isTrackAudible(const ofxOceanodeTimelineTrack& track) const {
    return !track.muted && (isTrackSoloed(track) || std::none_of(tracks.begin(), tracks.end(),
                                                    [this](const auto& other) { return isTrackSoloed(other); }));
}

bool ofxOceanodeTimelineManager::setTrackMuted(const std::string& trackId, bool muted) {
    auto* track = getTrack(trackId);
    if(track == nullptr || track->muted == muted) return false;
    track->muted = muted;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setTrackLatencyOffset(const std::string& trackId, float milliseconds) {
    auto* track = getTrack(trackId);
    const float clamped = ofClamp(milliseconds, -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs);
    if(track == nullptr || track->latencyOffsetMs == clamped) return false;
    track->latencyOffsetMs = clamped;
    // Events already handed over carry the old offset: schedule them again.
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setTrackSolo(const std::string& trackId, bool solo) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return false;
    // Setting it by hand replaces what a Toggle lane drove, until a lane
    // drives it again.
    const bool wasDriven = playback.drivenSolo.erase(PlayKey{0, trackId}) > 0;
    if(track->solo == solo && !wasDriven) return false;
    track->solo = solo;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneDrivesTrackSolo(const std::string& trackId, const std::string& clipId,
                                                      const std::string& laneId, bool drives) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || lane->drivesTrackSolo == drives) return false;
    lane->drivesTrackSolo = drives;
    if(drives && container != nullptr) applyLaneTrackSolos(container->getTransportState().beatPosition);
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneTargetProbability(const std::string& trackId, const std::string& clipId,
                                                        const std::string& laneId, const std::string& parameterPath,
                                                        float probability) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr) return false;
    probability = ofClamp(probability, 0.0f, 1.0f);
    const auto it = lane->targetProbability.find(parameterPath);
    const float current = it == lane->targetProbability.end() ? 1.0f : it->second;
    if(current == probability) return false;
    if(probability >= 1.0f) lane->targetProbability.erase(it);
    else lane->targetProbability[parameterPath] = probability;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::applyLaneTrackSolos(double beat) {
    bool changed = false;
    for(auto& track : tracks) {
        if(track.isWaveTrack) continue;
        bool driven = false, on = false;
        for(const auto& clip : track.clips) {
            if(clip.isLfo || clip.muted || !clipCoversBeat(clip, beat)) continue;
            for(const auto& lane : clip.lanes) {
                if(!lane.drivesTrackSolo || lane.type != ofxOceanodeTimelineLaneType::MultiGate ||
                   !laneAudible(clip, lane)) continue;
                driven = true;
                // A clip its chance roll left out rests: the solo is off.
                if(isClipChanceSilenced(clip) || lane.multiGateRows.empty()) continue;
                const double sourceBeat = ofxOceanodeTimelineClipTime::timelineToSourceBeat(clip, beat);
                on = on || evaluateMultiGateRow(lane.multiGateRows.front(), sourceBeat, 0.0,
                    clip.repeatContent ? ofxOceanodeTimelineClipTime::sourceDuration(clip) : 0.0);
            }
        }
        if(!driven) continue;
        // Playback state, not the track's saved solo.
        if(isTrackSoloed(track) != on) changed = true;
        playback.drivenSolo[PlayKey{0, track.id}] = on;
    }
    return changed;
}

bool ofxOceanodeTimelineManager::setLaneMuted(const std::string& trackId, const std::string& clipId,
                                            const std::string& laneId, bool muted) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || lane->muted == muted) return false;
    lane->muted = muted;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneSolo(const std::string& trackId, const std::string& clipId,
                                           const std::string& laneId, bool solo) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || lane->solo == solo) return false;
    lane->solo = solo;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setClipChance(const std::string& trackId, const std::string& clipId, float chance) {
    auto* clip = getClip(trackId, clipId);
    chance = ofClamp(chance, 0.0f, 1.0f);
    if(clip == nullptr || clip->chance == chance) return false;
    clip->chance = chance;
    // The roll reads the clip's chance, so it changes now; follow the
    // transport's pass as the next update would.
    if(container != nullptr) playContext = playContextFor(container->getTransportState());
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setClipMuted(const std::string& trackId, const std::string& clipId, bool muted) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr || clip->muted == muted) return false;
    clip->muted = muted;
    refreshPlaybackEnablement();
    return true;
}

void ofxOceanodeTimelineManager::refreshPlaybackEnablement() {
    invalidateEvaluationIndexes();
    if(container != nullptr) {
        const auto transport = container->getTransportState();
        collectActiveValues(playContext, transport.beatPosition, transport.isPlaying, false, activeAutomationValues);
        updatePianoGateOwnership(transport.isPlaying);
    }
    // Correct queued events against the updated playback state.
    invalidateSchedule();
    applyAutomation();
}

bool ofxOceanodeTimelineManager::resizeClipEnd(const std::string& trackId, const std::string& clipId,
                                               double duration, ofxOceanodeTimelineEndEdit operation) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr || !std::isfinite(duration)) return false;
    const double source = ofxOceanodeTimelineClipTime::sourceDuration(*clip);
    const double ratio = ofxOceanodeTimelineClipTime::effectiveStretch(*clip);
    duration = std::max(1.0 / 24.0, duration);
    setClipTiming(trackId, clipId, clip->startBeat, duration);
    const auto* track = getTrack(trackId);
    if(track != nullptr && track->isWaveTrack) {
        // Audio always stretches its full slice, independent of the global tool.
    } else if(operation == ofxOceanodeTimelineEndEdit::Repeat) {
        clip->contentStretch = ratio;
        setClipContentDuration(trackId, clipId, source, true);
    } else if(operation == ofxOceanodeTimelineEndEdit::Trim) {
        setClipContentDuration(trackId, clipId, duration / ratio, false);
    } else {
        setClipContentDuration(trackId, clipId, source, false);
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setClipContentDuration(const std::string& trackId, const std::string& clipId,
                                                         double contentDurationBeats, bool repeatContent) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return false;
    clip->contentDurationBeats = std::max(1.0 / 24.0, contentDurationBeats);
    clip->repeatContent = repeatContent;
    const auto* track = getTrack(trackId);
    if(track != nullptr && track->isWaveTrack) {
        // Never allow a generic repeat/crop edit to turn a Wave slice back
        // into a repeating pattern clip.
        normalizeWaveClipMapping(*clip);
        return true;
    }
    // A non-repeating clip always maps its complete source duration over its
    // visible duration. Mirror that effective ratio here so switching it to
    // repetition later preserves the exact stretch the user was seeing.
    if(!repeatContent) {
        clip->contentStretch = std::max(1.0 / 1024.0,
            clip->durationBeats / clip->contentDurationBeats);
    }
    return true;
}

bool ofxOceanodeTimelineManager::consolidateClipContent(const std::string& trackId,
                                                        const std::string& clipId) {
    auto* clip = getClip(trackId, clipId);
    if(clip == nullptr) return false;

    const double oldSourceDuration = ofxOceanodeTimelineClipTime::sourceDuration(*clip);
    const bool croppedRepeat = clip->repeatContent &&
        clip->durationBeats < ofxOceanodeTimelineClipTime::cycleDuration(*clip) - kEpsilon;
    const double visibleSourceDuration = croppedRepeat
        ? clip->durationBeats / ofxOceanodeTimelineClipTime::stretch(*clip)
        : oldSourceDuration;
    const double contentEnd = std::max(1.0 / 24.0, visibleSourceDuration);
    bool changed = false;

    for(auto& lane : clip->lanes) {
        if(lane.type == ofxOceanodeTimelineLaneType::Step) {
            const auto oldSize = lane.step.steps.size();
            lane.step.steps.erase(std::remove_if(lane.step.steps.begin(), lane.step.steps.end(),
                [&](const auto& step) { return step.startBeat >= contentEnd - kEpsilon; }),
                lane.step.steps.end());
            changed |= lane.step.steps.size() != oldSize;
            for(auto& step : lane.step.steps) {
                if(step.durationBeats > kEpsilon && step.startBeat + step.durationBeats > contentEnd) {
                    step.durationBeats = std::max(0.0, contentEnd - step.startBeat);
                    changed = true;
                }
            }
            const double stepLength = std::max(1.0 / 24.0, lane.beatsPerStep);
            const int consolidatedSteps = std::max(1, static_cast<int>(std::ceil(contentEnd / stepLength)));
            if(lane.stepCount > consolidatedSteps) {
                lane.stepCount = consolidatedSteps;
                changed = true;
            }
        } else if(lane.type == ofxOceanodeTimelineLaneType::PianoRoll) {
            const auto oldSize = lane.pianoNotes.size();
            lane.pianoNotes.erase(std::remove_if(lane.pianoNotes.begin(), lane.pianoNotes.end(),
                [&](const auto& note) { return note.startBeat >= contentEnd - kEpsilon; }),
                lane.pianoNotes.end());
            changed |= lane.pianoNotes.size() != oldSize;
            for(auto& note : lane.pianoNotes) {
                const double maximumDuration = std::max(kEpsilon, contentEnd - note.startBeat);
                if(note.durationBeats > maximumDuration) {
                    note.durationBeats = maximumDuration;
                    changed = true;
                }
            }
        } else if(lane.type == ofxOceanodeTimelineLaneType::MultiValue ||
                  lane.type == ofxOceanodeTimelineLaneType::MultiGate) {
            // Blocks live in source beats like steps and notes do, so they
            // get the same treatment: drop the ones that start past the new
            // end, crop the one straddling it.
            auto trimRows = [&](auto& rows) {
                for(auto& row : rows) {
                    const auto oldSize = row.size();
                    row.erase(std::remove_if(row.begin(), row.end(),
                        [&](const auto& region) { return region.startBeat >= contentEnd - kEpsilon; }),
                        row.end());
                    changed |= row.size() != oldSize;
                    for(auto& region : row) {
                        const double maximumDuration = std::max(kEpsilon, contentEnd - region.startBeat);
                        if(region.durationBeats > maximumDuration) {
                            region.durationBeats = maximumDuration;
                            changed = true;
                        }
                    }
                }
            };
            if(lane.type == ofxOceanodeTimelineLaneType::MultiValue) trimRows(lane.multiValueRows);
            else trimRows(lane.multiGateRows);
        } else if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid) {
            // Patterns repeat inside the content: nothing lies past its end.
        } else if(lane.type == ofxOceanodeTimelineLaneType::MultiSlider) {
            // A dense grid has no per-cell start to test: shortening it is
            // shortening the grid, exactly as the Step branch does.
            const double cellLength = std::max(1.0 / 24.0, lane.beatsPerStep);
            const int consolidatedSteps = std::max(1, static_cast<int>(std::ceil(contentEnd / cellLength)));
            if(lane.stepCount > consolidatedSteps) {
                lane.stepCount = consolidatedSteps;
                changed = true;
            }
            if(static_cast<int>(lane.multiSliderValues.size()) != lane.stepCount) {
                lane.multiSliderValues.resize(static_cast<size_t>(std::max(1, lane.stepCount)), 0.0f);
                changed = true;
            }
        } else {
            std::sort(lane.curvePoints.begin(), lane.curvePoints.end(),
                [](const auto& a, const auto& b) { return a.beat < b.beat; });
            std::string boundaryValue;
            const bool hasBoundaryValue = evaluateCurve(lane, contentEnd, boundaryValue);
            const auto firstHidden = std::upper_bound(lane.curvePoints.begin(), lane.curvePoints.end(), contentEnd + kEpsilon,
                [](double beat, const auto& point) { return beat < point.beat; });
            const bool removedPoints = firstHidden != lane.curvePoints.end();
            if(removedPoints) {
                lane.curvePoints.erase(firstHidden, lane.curvePoints.end());
                changed = true;
            }
            if(removedPoints && hasBoundaryValue &&
               (lane.curvePoints.empty() || lane.curvePoints.back().beat < contentEnd - kEpsilon)) {
                if(lane.curvePoints.empty() && contentEnd > kEpsilon)
                    lane.curvePoints.push_back({0.0, ofToFloat(boundaryValue)});
                lane.curvePoints.push_back({contentEnd, ofToFloat(boundaryValue)});
            }
            lane.curveTensions.resize(lane.curvePoints.empty() ? 0 : lane.curvePoints.size() - 1);
        }
    }

    if(croppedRepeat || std::abs(clip->contentDurationBeats - contentEnd) > kEpsilon) {
        clip->contentDurationBeats = contentEnd;
        if(croppedRepeat) clip->repeatContent = false;
        clip->contentStretch = std::max(1.0 / 1024.0,
            clip->durationBeats / clip->contentDurationBeats);
        changed = true;
    }
    return changed;
}

bool ofxOceanodeTimelineManager::setClipNumber(const std::string& trackId, const std::string& clipId,
                                              const std::string& laneId, double beat, float value) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || !ofxOceanodeTimelineIsHeldValueLane(lane->type) ||
       !std::isfinite(beat) || !std::isfinite(value) || beat < 0.0) return false;
    auto it = std::lower_bound(lane->curvePoints.begin(), lane->curvePoints.end(), beat,
        [](const auto& point, double b) { return point.beat < b; });
    if(it != lane->curvePoints.end() && std::abs(it->beat - beat) < kEpsilon) it->value = value;
    else lane->curvePoints.insert(it, {beat, value});
    lane->curveTensions.resize(lane->curvePoints.empty() ? 0 : lane->curvePoints.size() - 1);
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setClipStep(const std::string& trackId, const std::string& clipId,
                                             const std::string& laneId, double startBeat,
                                             const std::string& value, double durationBeats) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || lane->type != ofxOceanodeTimelineLaneType::Step) return false;
    lane->step.setStep(startBeat, value, durationBeats);
    return true;
}

bool ofxOceanodeTimelineManager::removeClipStep(const std::string& trackId, const std::string& clipId,
                                                const std::string& laneId, double startBeat) {
    auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr || lane->type != ofxOceanodeTimelineLaneType::Step) return false;
    return lane->step.removeStep(startBeat);
}

ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(const std::string& trackId, const std::string& bindingId) {
    auto* track = getTrack(trackId);
    if(track == nullptr) return nullptr;
    if(evaluationIndexesValid) {
        const auto trackIndex = trackEvaluationIndexes.find(track);
        if(trackIndex != trackEvaluationIndexes.end()) {
            const auto cached = trackIndex->second.bindingById.find(bindingId);
            if(cached != trackIndex->second.bindingById.end())
                return cached->second;
        }
    }
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](auto& binding) { return binding.id == bindingId; });
    return it == track->bindings.end() ? nullptr : &*it;
}

ofxOceanodeNodeModel* ofxOceanodeTimelineManager::pianoPitchNode(const std::string& trackId, const ofxOceanodeTimelineLane& lane) const {
    if(lane.pianoPitchBindingId.empty()) return nullptr;
    const auto* binding = getBinding(trackId, lane.pianoPitchBindingId);
    auto* parameter = binding == nullptr ? nullptr : findParameterCached(binding->parameterPath);
    return parameter == nullptr ? nullptr : parameter->getNodeModel();
}

const ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(const std::string& trackId, const std::string& bindingId) const {
    const auto* track = getTrack(trackId);
    if(track == nullptr) return nullptr;
    if(evaluationIndexesValid) {
        const auto trackIndex = trackEvaluationIndexes.find(track);
        if(trackIndex != trackEvaluationIndexes.end()) {
            const auto cached = trackIndex->second.bindingById.find(bindingId);
            if(cached != trackIndex->second.bindingById.end()) return cached->second;
        }
    }
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](const auto& binding) { return binding.id == bindingId; });
    return it == track->bindings.end() ? nullptr : &*it;
}

void ofxOceanodeTimelineManager::setBpmAutomationEnabled(bool enabled) {
    bpmAutomationEnabled = enabled;
    if(enabled && bpmAutomationPoints.empty()) {
        const float currentBpm = container == nullptr ? 120.0f : container->getTransportState().bpm;
        bpmAutomationPoints.push_back({0.0, ofClamp(currentBpm, bpmMinimum, bpmMaximum)});
    }
}

void ofxOceanodeTimelineManager::setBpmRange(float minimum, float maximum) {
    bpmMinimum = std::max(1.0f, std::min(minimum, maximum));
    bpmMaximum = std::max(bpmMinimum + 1.0f, std::max(minimum, maximum));
    for(auto& point : bpmAutomationPoints) point.value = ofClamp(point.value, bpmMinimum, bpmMaximum);
}

float ofxOceanodeTimelineManager::evaluateBpm(double beat, float fallbackBpm) const {
    if(!bpmAutomationEnabled || bpmAutomationPoints.empty()) return std::max(1.0f, fallbackBpm);
    const auto& points = bpmAutomationPoints;
    beat = std::max(0.0, beat);
    if(beat <= points.front().beat) return ofClamp(points.front().value, bpmMinimum, bpmMaximum);
    if(beat >= points.back().beat) return ofClamp(points.back().value, bpmMinimum, bpmMaximum);
    const auto mode = curveInterpolationMode(bpmInterpolation);
    for(size_t i = 1; i < points.size(); ++i) {
        if(beat > points[i].beat) continue;
        const double span = std::max(kEpsilon, points[i].beat - points[i - 1].beat);
        const float t = static_cast<float>(ofClamp((beat - points[i - 1].beat) / span, 0.0, 1.0));
        const auto tension = i - 1 < bpmCurveTensions.size() ? bpmCurveTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
        return ofClamp(ofLerp(points[i - 1].value, points[i].value, curveSegmentShape(t, mode, tension)), bpmMinimum, bpmMaximum);
    }
    return std::max(1.0f, fallbackBpm);
}

void ofxOceanodeTimelineManager::setBpmInterpolation(const std::string& interpolation) {
    bpmInterpolation = interpolation;
    // Topology/shape changes should not retain hidden shaping state from a
    // different mode, mirroring resetCurveTensions() for a lane's curve.
    bpmCurveTensions.assign(bpmAutomationPoints.empty() ? 0 : bpmAutomationPoints.size() - 1,
                            ofxOceanodeTimelineCurveTension{});
}

double ofxOceanodeTimelineManager::tempoSegmentSeconds(size_t i, double fraction) const {
    // Elapsed time under a segment is the integral of 60/bpm(x) over its
    // length. That has a closed form only when bpm(x) is linear in x; Step
    // holds at the start value the whole way and jumps at the very boundary
    // (an instant, so it contributes nothing extra); Log/Exp and Sigmoid have
    // no closed form for this, so they're integrated numerically with a fixed
    // Simpson's rule -- the shapes are smooth and bounded, so this is far more
    // precise than the pixel/ruler math this feeds needs.
    const auto& points = bpmAutomationPoints;
    const double segmentStart = std::max(0.0, points[i - 1].beat);
    const double segmentDuration = std::max(segmentStart, points[i].beat) - segmentStart;
    fraction = ofClamp(fraction, 0.0, 1.0);
    if(segmentDuration <= kEpsilon || fraction <= 0.0) return 0.0;
    const double startBpm = std::max(1.0, static_cast<double>(ofClamp(points[i - 1].value, bpmMinimum, bpmMaximum)));
    const double endBpm = std::max(1.0, static_cast<double>(ofClamp(points[i].value, bpmMinimum, bpmMaximum)));
    const auto mode = curveInterpolationMode(bpmInterpolation);
    if(mode == CurveInterpolationMode::Step) return segmentDuration * fraction * 60.0 / startBpm;
    if(mode == CurveInterpolationMode::Linear) {
        const double duration = segmentDuration * fraction;
        const double partialEndBpm = startBpm + (endBpm - startBpm) * fraction;
        const double slope = (partialEndBpm - startBpm) / duration;
        return std::abs(slope) <= kEpsilon ? duration * 60.0 / startBpm
                                           : 60.0 * std::log(partialEndBpm / startBpm) / slope;
    }
    const auto tension = i - 1 < bpmCurveTensions.size() ? bpmCurveTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
    constexpr int steps = 64;
    const double h = fraction / steps;
    auto integrand = [&](double x) {
        const float shaped = curveSegmentShape(static_cast<float>(x), mode, tension);
        return 60.0 / std::max(1.0, startBpm + (endBpm - startBpm) * static_cast<double>(shaped));
    };
    double sum = integrand(0.0) + integrand(fraction);
    for(int k = 1; k < steps; ++k) sum += integrand(k * h) * (k % 2 == 0 ? 2.0 : 4.0);
    return segmentDuration * (h / 3.0) * sum;
}

bool ofxOceanodeTimelineManager::refreshTempoCache() const {
    if(!bpmAutomationEnabled || bpmAutomationPoints.empty()) return false;
    // Seconds at every point, cached: conversions run dozens of times per
    // frame and curved segments are integrated numerically. The editor writes
    // the points in place, so the map is fingerprinted (a few words per point)
    // rather than tracked.
    const auto& points = bpmAutomationPoints;
    ChangeHash hash;
    hash.add(points.size());
    for(const auto& point : points) { hash.add(point.beat); hash.add(point.value); }
    hash.add(bpmCurveTensions.size());
    for(const auto& tension : bpmCurveTensions) { hash.add(tension.inflection); hash.add(tension.steepness); }
    hash.add(bpmMinimum); hash.add(bpmMaximum); hash.add(bpmInterpolation);
    if(tempoSecondsCacheValid && tempoSecondsCacheSignature == hash.result() && tempoSecondsAtPoint.size() == points.size())
        return true;
    const double firstBeat = std::max(0.0, points.front().beat);
    tempoSecondsAtPoint.assign(points.size(), 0.0);
    tempoSecondsAtPoint[0] = firstBeat * 60.0 / static_cast<double>(ofClamp(points.front().value, bpmMinimum, bpmMaximum));
    for(size_t i = 1; i < points.size(); ++i)
        tempoSecondsAtPoint[i] = tempoSecondsAtPoint[i - 1] + tempoSegmentSeconds(i, 1.0);
    tempoSecondsCacheSignature = hash.result();
    tempoSecondsCacheValid = true;
    return true;
}

double ofxOceanodeTimelineManager::beatToSeconds(double beat, float fallbackBpm) const {
    beat = std::max(0.0, beat);
    if(!refreshTempoCache()) return beat * 60.0 / std::max(1.0f, fallbackBpm);
    const auto& points = bpmAutomationPoints;
    const double firstBeat = std::max(0.0, points.front().beat);
    if(beat <= firstBeat)
        return beat * 60.0 / static_cast<double>(ofClamp(points.front().value, bpmMinimum, bpmMaximum));
    const double lastBeat = std::max(0.0, points.back().beat);
    if(beat >= lastBeat)
        return tempoSecondsAtPoint.back() + (beat - lastBeat) * 60.0 /
               static_cast<double>(ofClamp(points.back().value, bpmMinimum, bpmMaximum));
    // First point past the beat (points are kept sorted by beat).
    const auto upper = std::upper_bound(points.begin(), points.end(), beat,
                                        [](double b, const auto& point) { return b < point.beat; });
    const size_t i = std::max<size_t>(1, static_cast<size_t>(upper - points.begin()));
    const double segmentStart = std::max(0.0, points[i - 1].beat);
    const double segmentDuration = std::max(segmentStart, points[i].beat) - segmentStart;
    if(segmentDuration <= kEpsilon || beat <= segmentStart) return tempoSecondsAtPoint[i - 1];
    return tempoSecondsAtPoint[i - 1] + tempoSegmentSeconds(i, (beat - segmentStart) / segmentDuration);
}

double ofxOceanodeTimelineManager::secondsToBeat(double seconds, float fallbackBpm) const {
    seconds = std::max(0.0, seconds);
    if(!refreshTempoCache()) return seconds * std::max(1.0f, fallbackBpm) / 60.0;
    const auto& points = bpmAutomationPoints;
    const double firstBpm = static_cast<double>(ofClamp(points.front().value, bpmMinimum, bpmMaximum));
    if(seconds <= tempoSecondsAtPoint.front()) return seconds * firstBpm / 60.0;
    if(seconds >= tempoSecondsAtPoint.back())
        return std::max(0.0, points.back().beat) + (seconds - tempoSecondsAtPoint.back()) *
               static_cast<double>(ofClamp(points.back().value, bpmMinimum, bpmMaximum)) / 60.0;
    // The segment whose cached span holds the time.
    const size_t i = std::max<size_t>(1, static_cast<size_t>(
        std::upper_bound(tempoSecondsAtPoint.begin(), tempoSecondsAtPoint.end(), seconds) - tempoSecondsAtPoint.begin()));
    const double segmentStart = std::max(0.0, points[i - 1].beat);
    const double segmentDuration = std::max(segmentStart, points[i].beat) - segmentStart;
    const double into = seconds - tempoSecondsAtPoint[i - 1];
    if(segmentDuration <= kEpsilon) return segmentStart;
    const double startBpm = std::max(1.0, static_cast<double>(ofClamp(points[i - 1].value, bpmMinimum, bpmMaximum)));
    const double endBpm = std::max(1.0, static_cast<double>(ofClamp(points[i].value, bpmMinimum, bpmMaximum)));
    const auto mode = curveInterpolationMode(bpmInterpolation);
    if(mode == CurveInterpolationMode::Step)
        return segmentStart + std::min(segmentDuration, into * startBpm / 60.0);
    if(mode == CurveInterpolationMode::Linear) {
        // bpm(x) = b0 + k x  =>  t(x) = 60 ln(bpm(x) / b0) / k  =>  x = b0 (e^(k t / 60) - 1) / k
        const double slope = (endBpm - startBpm) / segmentDuration;
        const double x = std::abs(slope) <= kEpsilon ? into * startBpm / 60.0
                                                     : startBpm * std::expm1(slope * into / 60.0) / slope;
        return segmentStart + ofClamp(x, 0.0, segmentDuration);
    }
    // Curved: Newton on the segment fraction (d seconds / d fraction is just
    // duration * 60 / bpm there), kept inside a shrinking bracket.
    const auto tension = i - 1 < bpmCurveTensions.size() ? bpmCurveTensions[i - 1] : ofxOceanodeTimelineCurveTension{};
    const double total = tempoSecondsAtPoint[i] - tempoSecondsAtPoint[i - 1];
    double low = 0.0, high = 1.0;
    double x = total > 0.0 ? ofClamp(into / total, 0.0, 1.0) : 0.0;
    for(int iteration = 0; iteration < 24; ++iteration) {
        const double error = tempoSegmentSeconds(i, x) - into;
        if(std::abs(error) <= 1e-10) break;
        if(error < 0.0) low = x; else high = x;
        const float shaped = curveSegmentShape(static_cast<float>(x), mode, tension);
        const double bpm = std::max(1.0, startBpm + (endBpm - startBpm) * static_cast<double>(shaped));
        double next = x - error / (segmentDuration * 60.0 / bpm);
        if(!(next > low && next < high)) next = (low + high) * 0.5;
        if(std::abs(next - x) <= 1e-12) { x = next; break; }
        x = next;
    }
    return segmentStart + x * segmentDuration;
}

namespace {
void sortMarkers(std::vector<ofxOceanodeTimelineMarker>& markers) {
    std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
}
}

std::string ofxOceanodeTimelineManager::addMarker(double beat, const std::string& name) {
    ofxOceanodeTimelineMarker marker;
    uint64_t number = markers.size() + 1;
    do {
        marker.id = makeId("timeline_marker", number++);
    } while(std::any_of(markers.begin(), markers.end(), [&](const auto& m) { return m.id == marker.id; }));
    marker.beat = std::max(0.0, beat);
    marker.name = name.empty() ? "Marker " + ofToString(markers.size() + 1) : name;
    markers.push_back(marker);
    sortMarkers(markers);
    return marker.id;
}

bool ofxOceanodeTimelineManager::removeMarker(const std::string& markerId) {
    const auto oldSize = markers.size();
    markers.erase(std::remove_if(markers.begin(), markers.end(), [&](const auto& m) { return m.id == markerId; }), markers.end());
    return markers.size() != oldSize;
}

bool ofxOceanodeTimelineManager::moveMarker(const std::string& markerId, double beat) {
    for(auto& marker : markers) {
        if(marker.id != markerId) continue;
        marker.beat = std::max(0.0, beat);
        sortMarkers(markers);
        return true;
    }
    return false;
}

bool ofxOceanodeTimelineManager::renameMarker(const std::string& markerId, const std::string& name) {
    if(name.empty()) return false;
    for(auto& marker : markers) if(marker.id == markerId) { marker.name = name; return true; }
    return false;
}

const ofxOceanodeTimelineMarker* ofxOceanodeTimelineManager::getMarker(const std::string& markerId) const {
    for(const auto& marker : markers) if(marker.id == markerId) return &marker;
    return nullptr;
}

void ofxOceanodeTimelineManager::syncLoopToTransport() {
    if(container == nullptr) return;
    const auto transport = container->getTransport();
    if(transport == nullptr) return;
    // An external clock owns position and looping. Preserve the timeline's
    // local setting so it can resume when sync is released, but do not apply
    // it while the external source is in control.
    transport->setLoop(!transport->hasExternalClock() && loopEnabled,
                       loopStartBeat, loopEndBeat);
}

void ofxOceanodeTimelineManager::setLoopEnabled(bool enabled) {
    if(loopEnabled == enabled) return;
    loopEnabled = enabled;
    hasEvaluatedTransportBeat = false;
    invalidateSchedule();
    syncLoopToTransport();
}

void ofxOceanodeTimelineManager::setLoopRange(double startBeat, double endBeat) {
    const double minimumLength = 1.0 / 24.0;
    loopStartBeat = std::max(0.0, std::min(startBeat, endBeat - minimumLength));
    loopEndBeat = std::max(loopStartBeat + minimumLength, endBeat);
    hasEvaluatedTransportBeat = false;
    // Events past the old boundary belong to a loop that no longer exists.
    invalidateSchedule();
    syncLoopToTransport();
}

void ofxOceanodeTimelineManager::update() {
    beginParameterCache();
    evaluateAutomation();
    applyAutomation();
    endParameterCache();
}

void ofxOceanodeTimelineManager::beginParameterCache() {
    parameterCache.clear();
    parameterCacheActive = true;
}

void ofxOceanodeTimelineManager::endParameterCache() {
    parameterCacheActive = false;
    parameterCache.clear();
}

ofxOceanodeAbstractParameter* ofxOceanodeTimelineManager::findParameterCached(const std::string& path) const {
    if(container == nullptr) return nullptr;
    if(!parameterCacheActive) return container->findTimelineParameter(path);
    const auto it = parameterCache.find(path);
    if(it != parameterCache.end()) return it->second;
    auto* parameter = container->findTimelineParameter(path);
    parameterCache.emplace(path, parameter);
    return parameter;
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
    const uint64_t mutationEpoch = context.epoch();
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
    for(const auto& track : tracks) {
        if(!isTrackAudible(track)) continue;
        contributionTrack = &track;
        for(const auto& clip : track.clips) {
            contributionClip = &clip;
            contributionLanes.clear();
            double sourceBeat = 0.0;
            if(!clipSourceBeat(clip, context, beatPosition, sourceBeat)) {
                // A clip its chance roll left out plays as an empty pass: the
                // lanes that rest at 0 between their events (gates, toggles,
                // steps, value regions) write that rest, instead of holding
                // whatever the previous pass ended on through this one.
                // Curves, numbers, dropdowns and sliders have no rest and hold,
                // as they do between clips; piano gates already rest at 0.
                if(!ofxOceanodeTimelineClipChanceSilenced(clip, context) || clip.muted || clip.isLfo || !clipCoversBeat(clip, beatPosition)) continue;
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
                                                ofxOceanodeTimelineClipTime::cycleIndex(clip, beatPosition),
                                                ofxOceanodeTimelineClipTime::sourceDuration(clip), mutationEpoch)
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
                            const int64_t cycle = ofxOceanodeTimelineClipTime::cycleIndex(clip, beatPosition);
                            for(int row = 0; row < rowCount; ++row) {
                                if(std::isnan(blockStarts[row])) continue;
                                if(!toggleTargetPasses(lane, binding->parameterPath, chance->second,
                                                       blockStarts[row], row, cycle, mutationEpoch)) reached[row] = "0";
                            }
                            contribute(binding, joinAutomationValues(reached));
                        }
                    }
                } else if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid) {
                    // Drum Grid: row r drives component row.slot of the
                    // instrument's Gate (its velocity while a hit is open)
                    // and Pitch (60 + the step's offset, held after the gate
                    // closes so a tail keeps its tuning).
                    const int64_t repeat = ofxOceanodeTimelineClipTime::cycleIndex(clip, beatPosition);
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
                                                                                  gap, mutationEpoch, gate, pitch, &duration, &filter, &stutter) &&
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
                    const int64_t pianoRepeat = ofxOceanodeTimelineClipTime::cycleIndex(clip, beatPosition);
                    const double pianoCycle = static_cast<double>(pianoRepeat);
                    // With chance notes, only those sounding in this repeat
                    // hold a voice: a failed note neither cuts the one before
                    // it nor moves the slot's pitch. Mutating notes play as
                    // this repeat changed them.
                    const auto* playedNotes = &lane.pianoNotes;
                    if(voicing->chance) {
                        const auto& played = repeatPlayback(context, track, clip, lane, shared, pianoRepeat);
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
                                   mutationEpoch))
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

    // Lanes inside one clip use the binding's blend mode (for example, a
    // Multiply binding shared by a Curve and a piano Gate evaluates as
    // curve * gate). Where clips on one row overlap, the last one wins: the
    // clip that starts latest (then the later one in the track) is the one
    // heard. Finally, expose one contribution per binding for
    // cross-track/cross-binding blending.
    for(const auto& track : tracks) {
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

// Release a piano gate once when playback or its final unmuted clip goes away.
// Keeping zeros in the ordinary stopped evaluation would fight live keyboards.
void ofxOceanodeTimelineManager::updatePianoGateOwnership(bool isPlaying) {
    std::unordered_set<std::string> next;
    if(isPlaying) for(const auto* binding : zeroWhenInactiveBindingsCache)
        if(!binding->bypass) next.insert(binding->parameterPath);
    for(const auto& entry : bindingsByPathCache)
        if(playingPianoGatePaths.count(entry.first) > 0 &&
           std::any_of(entry.second.begin(), entry.second.end(), [](const auto* binding) {
               return !binding->bypass && binding->hasLiveOverride;
           })) next.insert(entry.first);
    releasedPianoGatePaths.clear();
    for(const auto& path : playingPianoGatePaths) {
        if(next.count(path) > 0) continue;
        releasedPianoGatePaths.push_back(path);
        if(activeAutomationValues.count(path) == 0)
            activeAutomationValues[path].emplace_back(ofxOceanodeTimelineAutomationMode::Replace, "0");
    }
    playingPianoGatePaths = std::move(next);
}

namespace {
// Gives group groupId of track a kit role's binding (a copy of `binding`, or
// the track's own binding of that parameter) and its Drum Grid lanes the
// role, when the group never had one. Returns true when it changed.
bool adoptGroupKitRole(ofxOceanodeTimelineTrack& track, const std::string& groupId, const KitRole& role,
                       const ofxOceanodeTimelineParameterBinding& binding) {
    auto group = std::find_if(track.noteGroups.begin(), track.noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(group == track.noteGroups.end() || (*group).*role.decided || !((*group).*role.group).empty()) return false;
    std::string id;
    for(const auto& existing : track.bindings) if(existing.parameterPath == binding.parameterPath) id = existing.id;
    if(id.empty()) {
        if(std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& b) { return b.id == binding.id; })) return false;
        track.bindings.push_back(binding);
        id = binding.id;
    }
    // A binding belongs to one group only.
    for(const auto& other : track.noteGroups) if(other.id != groupId && other.contains(id)) return false;
    if(group->contains(id)) return false;
    const auto oldMembers = group->members();
    (*group).*role.group = id;
    (*group).*role.decided = true;
    for(auto& clip : track.clips) {
        for(auto& lane : clip.lanes) {
            if(lane.type != ofxOceanodeTimelineLaneType::DrumGrid || !(lane.*role.lane).empty()) continue;
            const bool uses = std::any_of(lane.bindingIds.begin(), lane.bindingIds.end(), [&](const auto& laneBinding) {
                return std::find(oldMembers.begin(), oldMembers.end(), laneBinding) != oldMembers.end();
            });
            if(!uses) continue;
            lane.*role.lane = id;
            if(std::find(lane.bindingIds.begin(), lane.bindingIds.end(), id) == lane.bindingIds.end()) lane.bindingIds.push_back(id);
        }
    }
    return true;
}
}

void ofxOceanodeTimelineManager::adoptInstrumentDurations() {
    if(container == nullptr || replayingTimelineEdit) return;
    struct Adoption { std::string trackId, groupId; const KitRole* role; ofxOceanodeTimelineParameterBinding binding; };
    std::vector<Adoption> adopted;
    for(size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if(tracks[trackIndex].isWaveTrack) continue;
        for(size_t groupIndex = 0; groupIndex < tracks[trackIndex].noteGroups.size(); ++groupIndex) {
            for(const KitRole* role : {&kDurationRole, &kFilterRole, &kStutterRole}) {
                auto& track = tracks[trackIndex];
                const auto group = track.noteGroups[groupIndex];
                if(group.*role->decided || !(group.*role->group).empty()) continue;
                // The instrument behind the group's gate (or pitch).
                ofxOceanodeAbstractParameter* input = nullptr;
                for(const auto* id : {&group.gateBindingId, &group.pitchBindingId}) {
                    const auto* roleBinding = bindingInTrack(track, *id);
                    auto* parameter = roleBinding == nullptr ? nullptr : container->findTimelineParameter(roleBinding->parameterPath);
                    auto* model = parameter == nullptr ? nullptr : parameter->getNodeModel();
                    ofxOceanodeTimelineInstrument instrument;
                    if(model == nullptr || !model->getTimelineInstrument(instrument)) continue;
                    if(instrument.gate != parameter && instrument.pitch != parameter) continue;
                    input = role == &kDurationRole ? instrument.duration
                          : role == &kFilterRole ? instrument.filter : instrument.stutter;
                    break;
                }
                if(input == nullptr) continue;
                const std::string path = container->getTimelineParameterPath(*input);
                std::string bindingId;
                for(const auto& binding : track.bindings) if(binding.parameterPath == path) bindingId = binding.id;
                if(bindingId.empty()) bindingId = addBinding(track.id, *input);
                auto& liveTrack = tracks[trackIndex];
                const auto* binding = bindingInTrack(liveTrack, bindingId);
                if(binding == nullptr) continue;
                const auto copy = *binding;
                if(adoptGroupKitRole(liveTrack, group.id, *role, copy)) {
                    normalizeNoteGroups(liveTrack);
                    adopted.push_back({liveTrack.id, group.id, role, copy});
                }
            }
        }
    }
    if(adopted.empty()) return;
    // Not an edit of its own: undo/redo snapshots (and an open gesture's
    // starting point) get the same role, so history neither records it nor
    // sees an unrecorded change and gives up.
    auto patch = [&](TimelineState& state) {
        bool changed = false;
        for(const auto& adoption : adopted)
            for(auto& track : state.tracks)
                if(track.id == adoption.trackId && adoptGroupKitRole(track, adoption.groupId, *adoption.role, adoption.binding)) {
                    normalizeNoteGroups(track);
                    changed = true;
                }
        if(!changed) return;
        std::swap(tracks, state.tracks);
        const auto tracksJson = toJson()["tracks"];
        std::swap(tracks, state.tracks);
        state.data["tracks"] = tracksJson;
    };
    for(auto* stack : {&timelineUndo, &timelineRedo})
        for(auto& edit : *stack) { patch(edit.before); patch(edit.after); }
    if(pendingTimelineEdit) patch(pendingTimelineEdit->before);
    invalidateEvaluationIndexes();
    invalidateSchedule();
}

void ofxOceanodeTimelineManager::evaluateAutomation() {
    if(container == nullptr) return;
    adoptInstrumentDurations();
    if(metronome) metronome->setMeter(timeSignatureNumerator, timeSignatureDenominator);
    // Track/binding/clip storage may have changed through the editor since
    // the previous update. Rebuild only then, and share the indexes across the
    // frame evaluation, scheduler probes and both applyAutomation passes.
    ensureEvaluationIndexes();
    loopWrappedThisFrame = false;
    auto transport = container->getTransportState();
    // Following an external clock (MIDI clock): the master owns position, tempo and looping.
    const auto ownerTransportPtr = container->getTransport();
    const bool externalClock = ownerTransportPtr != nullptr && ownerTransportPtr->hasExternalClock();

    // The transport owns the loop (it wraps exactly at the loop end, keeping
    // the overshoot). Hand it the range; an external clock owns looping itself.
    // There is a single timeline (the root's), so it alone sets the loop.
    if(auto ownerTransport = container->getTransport()) {
        ownerTransport->setLoop(!externalClock && loopEnabled, loopStartBeat, loopEndBeat);
        transport = container->getTransportState();
    }

    // A stopped state-capture curve must still be editable from the node GUI.
    // The GUI writes after update(), so compare the value found at the start of
    // the next update with the last value automation itself applied. A mismatch
    // becomes a live override and is committed/cleared by the next capture.
    if(transport.isPlaying) {
        if(!stateCaptureManualOverrideKeys.empty()) {
            for(auto& track : tracks) {
                for(auto& binding : track.bindings) {
                    const std::string key = track.id + "\x1f" + binding.id;
                    if(stateCaptureManualOverrideKeys.count(key) == 0) continue;
                    binding.hasLiveOverride = false;
                    binding.liveOverrideValue.clear();
                }
            }
            stateCaptureManualOverrideKeys.clear();
        }
    } else {
        std::unordered_set<std::string> checkedBindings;
        for(auto& track : tracks) {
            if(!isTrackAudible(track)) continue;
            for(const auto& clip : track.clips) {
                if(clip.muted || !clip.isStateCapture ||
                   transport.beatPosition < clip.startBeat - kEpsilon ||
                   transport.beatPosition >= clip.startBeat + clip.durationBeats - kEpsilon) continue;
                for(const auto& lane : clip.lanes) {
                    if(!laneAudible(clip, lane) || lane.type != ofxOceanodeTimelineLaneType::Curve) continue;
                    for(const auto& bindingId : lane.bindingIds) {
                        const std::string key = track.id + "\x1f" + bindingId;
                        if(!checkedBindings.insert(key).second) continue;
                        auto* binding = getBinding(track.id, bindingId);
                        if(binding == nullptr) continue;
                        // A piano-key preview (or another explicit caller of
                        // setLiveOverride) owns its override lifetime. Do not
                        // adopt and later clear it as a state-capture edit.
                        if(binding->hasLiveOverride &&
                           stateCaptureManualOverrideKeys.count(key) == 0) continue;
                        auto* parameter = findParameterCached(binding->parameterPath);
                        const auto previous = stateCaptureLastAppliedValues.find(binding->parameterPath);
                        if(parameter == nullptr || previous == stateCaptureLastAppliedValues.end()) continue;
                        const std::string current = parameterValueForAutomation(*parameter);
                        if(current == previous->second) continue;
                        binding->hasLiveOverride = true;
                        binding->liveOverrideValue = current;
                        stateCaptureManualOverrideKeys.insert(key);
                    }
                }
            }
        }
    }
    if(hasSeenLoopCount && transport.loopCount != lastSeenLoopCount) loopWrappedThisFrame = true;
    lastSeenLoopCount = transport.loopCount;
    hasSeenLoopCount = true;
    scheduleUsesTempoMap = !externalClock;
    {
        // Retrigger gap: at least 10 ms and 1.2 frames of playback, so the
        // per-frame path always samples the gate-off between repeated notes.
        const double frameSeconds = std::max(1.0 / 240.0, static_cast<double>(ofGetLastFrameTime()));
        const double beatsPerSecond = std::max(1.0f, transport.bpm) / 60.0;
        const double target = ofClamp(std::max(0.010, 1.2 * frameSeconds) * beatsPerSecond, 1.0 / 192.0, 0.25);
        // Once playback has queued events, keep their gate boundaries stable.
        // Even a large GUI frame-time fluctuation must not invalidate them:
        // correction sends would close a future note 1 ms after it opens, then
        // reschedule that same note on the next frame. Repeated fluctuations
        // turn one note into a burst of on/off events and late corrections.
        // The frame-driven path can still adapt, as can the next scheduling
        // run after stopping or an explicit content/transport edit.
        const bool schedulingInFlight = schedulingEnabled && transport.isPlaying &&
                                        !scheduledPaths.empty();
        // Scheduled lanes only need two or three control blocks of gate-off.
        const double scheduledTarget = std::max(1.0 / 1920.0, 0.003 * beatsPerSecond);
        if(!schedulingInFlight && (std::abs(target - retriggerGapBeats) > 0.25 * retriggerGapBeats ||
           std::abs(scheduledTarget - scheduledRetriggerGapBeats) > 0.25 * scheduledRetriggerGapBeats)) {
            retriggerGapBeats = target;
            scheduledRetriggerGapBeats = scheduledTarget;
            invalidateSchedule();
        }
        // A stall raises the scheduling window at once; it shrinks back over
        // a few seconds of short frames.
        const double rawFrameSeconds = std::min(0.5, static_cast<double>(ofGetLastFrameTime()));
        frameIntervalPeakSeconds = std::max(rawFrameSeconds,
            frameIntervalPeakSeconds * std::pow(0.5, std::max(0.0, rawFrameSeconds) / 2.0));
    }
    // Decide which paths are scheduled before evaluating this frame: their
    // lanes retrigger with the short gap, in the frame value and in the
    // events handed ahead alike.
    schedulablePathsThisFrame.clear();
    if(schedulingEnabled && transport.isPlaying && transport.driverMode != TransportDriverMode::FrameStep)
        schedulablePathsThisFrame = selectSchedulablePaths();
    {
        std::unordered_set<std::string> fast(schedulablePathsThisFrame.begin(), schedulablePathsThisFrame.end());
        if(fast != fastRetriggerPaths) {
            fastRetriggerPaths = std::move(fast);
            // Events already queued used the other gap.
            if(!scheduledPaths.empty()) invalidateSchedule();
        }
    }
    lastEvaluatedTransportBeat = transport.beatPosition;
    hasEvaluatedTransportBeat = true;

    if(bpmAutomationEnabled && !externalClock) { // an external clock sets the tempo
        const float automatedBpm = evaluateBpm(transport.beatPosition, transport.bpm);
        if(std::abs(automatedBpm - transport.bpm) > 0.001f) {
            container->setBpm(automatedBpm);
            transport.bpm = automatedBpm;
        }
    }

    playContext = playContextFor(transport);
    // Toggle lanes bound to their track's solo decide, before anything reads
    // which tracks are audible this frame.
    if(applyLaneTrackSolos(transport.beatPosition)) {
        invalidateEvaluationIndexes();
        ensureEvaluationIndexes();
        invalidateSchedule();
    }
    // Drifted notes start again from the written ones on the next play.
    if(!transport.isPlaying && !playback.pianoDrift.empty()) playback.clearRolls();
    collectActiveValues(playContext, transport.beatPosition, transport.isPlaying, true, activeAutomationValues);
    updatePianoGateOwnership(transport.isPlaying);

    // Hand the discrete events of the coming window to any backend that can
    // execute them at an exact instant. Everything below still runs exactly
    // as before for this frame's own position.
    runScheduler(transport, loopWrappedThisFrame);

    // Wave tracks never touch the automation binding map. They are reported
    // through the optional audio backend as clip sources instead.
    if(waveAudioProvider != nullptr) {
        for(const auto& track : tracks) {
            if(!track.isWaveTrack) continue;
            for(const auto& clip : track.clips) {
                // Wave clips always map their complete source linearly over
                // the visible clip, independent of repeatContent and of the
                // cached contentStretch. Position and rate are derived from
                // the same ratio so the synth's audio-rate read head reaches
                // the slice end exactly when the playhead reaches the clip end.
                const double visibleDuration = std::max(1.0 / 24.0, clip.durationBeats);
                const double sourceDuration = std::max(1.0 / 24.0, clip.contentDurationBeats);
                const double sourcePerTimelineBeat = sourceDuration / visibleDuration;
                const double localBeat = transport.beatPosition - clip.startBeat;
                const bool active = isTrackAudible(track) && !clip.muted && !isClipChanceSilenced(clip) && localBeat >= -kEpsilon && localBeat < visibleDuration - kEpsilon;
                const double waveSourceBeat = active
                    ? ofClamp(localBeat * sourcePerTimelineBeat, 0.0, sourceDuration) : 0.0;
                const float volume = ofClamp(evaluateWaveClipVolume(track.id, clip.id,
                                                                     transport.beatPosition), 0.0f, 4.0f);
                // wavePlaybackRate is a user multiplier; the stretch ratio is
                // applied here once and nowhere else.
                const float clipPlaybackRate = ofClamp(
                    static_cast<float>(clip.wavePlaybackRate * sourcePerTimelineBeat),
                    0.01f, 16.0f);
                waveAudioProvider->updateWaveClip(track.id, clip.id, clip.waveFilePath, clip.waveGain,
                                                  volume, clip.waveNumChannels, waveSourceBeat,
                                                  clip.contentDurationBeats,
                                                  clip.waveSourceStartBeat,
                                                  std::max(1.0 / 24.0,
                                                           clip.waveFileDurationBeats > 0.0
                                                               ? clip.waveFileDurationBeats
                                                               : clip.waveFileDurationMs * transport.bpm / 60000.0),
                                                  clipPlaybackRate, clip.waveReverse, transport.bpm,
                                                  transport.isPlaying, active,
                                                  loopWrappedThisFrame);
            }
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

const std::vector<std::string>& ofxOceanodeTimelineManager::orderedAutomationPaths() const {
    return automationPathOrderCache;
}

uint64_t ofxOceanodeTimelineManager::pianoInputLeadUs(const std::string& path) const {
    const auto it = pianoInputPriorityCache.find(path);
    if(it == pianoInputPriorityCache.end()) return 0;
    // Prepare pitch, then velocity, then let Gate fire on the musical grid.
    // Distinct timetags also protect listeners that resend all note inputs.
    return it->second == 0 ? 2000 : it->second == 1 ? 1000 : 0;
}

void ofxOceanodeTimelineManager::applyAutomation() {
    if(container == nullptr) return;
    if(!evaluationIndexesValid) rebuildEvaluationIndexes();
    const auto& activeValues = activeAutomationValues;
    for(const auto& path : orderedAutomationPaths()) {
        auto& bindings = bindingsByPathCache.at(path);
        auto* parameter = findParameterCached(path);
        ofxOceanodeTimelineParameterBinding* defaultBinding = nullptr;
        for(auto* binding : bindings) {
            binding->missingTarget = parameter == nullptr;
            if(!binding->bypass && defaultBinding == nullptr) defaultBinding = binding;
        }
        if(parameter == nullptr) continue;
        parameter->setTimelined(defaultBinding != nullptr);
        if(defaultBinding == nullptr) continue;

        std::string value;
        if(!computeParameterValue(bindings, activeValues, parameter, value)) {
            stateCaptureLastAppliedValues.erase(path);
            continue;
        }
        if(!value.empty()) {
            // A scheduled path's backend already received this value with its
            // exact instant. The parameter, its GUI and the node graph are
            // still updated here, on the frame the playhead reaches it, but the
            // backend must not send it a second time -- an untimed duplicate
            // would arrive late and, for a trigger parameter, fire twice.
            // (Nothing is set, nor notified, when the value is already there.)
            ofxOceanodeScheduling::ScopedBackendSuppression suppression(
                scheduledBackendPaths.count(path) > 0);
            applyAutomationValue(*parameter, value, defaultBinding->valueType);
        }
        // Only state-capture curves compare against it (GUI edits while stopped).
        if(stateCapturePathsCache.count(path) > 0)
            stateCaptureLastAppliedValues[path] = parameterValueForAutomation(*parameter);
    }
    // A gate released this frame whose binding is no longer active -- bypassed,
    // or gone with an undo or a load -- has nothing above to close it. Close it
    // once, at its own width, and leave it alone afterwards.
    for(const auto& path : releasedPianoGatePaths) {
        const auto bindingsIt = bindingsByPathCache.find(path);
        if(bindingsIt != bindingsByPathCache.end() &&
           std::any_of(bindingsIt->second.begin(), bindingsIt->second.end(),
                       [](const auto* binding) { return !binding->bypass; })) continue;
        auto* parameter = findParameterCached(path);
        if(parameter == nullptr) continue;
        const std::string type = parameter->valueType();
        applyAutomationValue(*parameter, broadcastScalarNumericVectorValue(*parameter, "0", type), type);
    }
}

// ---------------------------------------------------------------------------
// Timestamped scheduling
// ---------------------------------------------------------------------------

void ofxOceanodeTimelineManager::setSchedulingEnabled(bool enabled) {
    if(schedulingEnabled == enabled) return;
    schedulingEnabled = enabled;
    invalidateSchedule();
}

void ofxOceanodeTimelineManager::setSchedulingLookaheadMs(double milliseconds) {
    const double clamped = std::max(0.0, std::min(1000.0, milliseconds));
    if(std::abs(clamped - schedulingLookaheadMs) < 1e-6) return;
    schedulingLookaheadMs = clamped;
    invalidateSchedule();
}

void ofxOceanodeTimelineManager::invalidateSchedule() {
    // Corrected by the next runScheduler(), not here: an editor or a seek
    // calls this between frames, when the position the correction has to
    // agree with is not known yet.
    if(!scheduledPaths.empty()) scheduleInvalidationPending = true;
    scheduledBackendPaths.clear();
    hasScheduleCursor = false;
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
    hash.add(tracks.data()); hash.add(tracks.size());
    for(const auto& track : tracks) {
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
            hash.add(clip.contentDurationBeats); hash.add(clip.contentStretch); hash.add(clip.repeatContent);
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

double ofxOceanodeTimelineManager::beatAtOffset(const ofxOceanodeTransportState& transport,
                                                double offsetSeconds) const {
    if(offsetSeconds <= 0.0) return transport.beatPosition;
    auto secondsAt = [&](double beat) {
        if(!scheduleUsesTempoMap) return beat * 60.0 / std::max(1.0f, transport.bpm);
        return beatToSeconds(beat, transport.bpm);
    };
    auto advance = [&](double fromBeat, double seconds) {
        if(!scheduleUsesTempoMap) return fromBeat + seconds * std::max(1.0f, transport.bpm) / 60.0;
        return beatAfterSeconds(fromBeat, seconds, transport.bpm);
    };
    const double beat = advance(transport.beatPosition, offsetSeconds);
    const bool loopActive = transport.loopActive() &&
        transport.beatPosition < transport.loopEndBeat - kEpsilon;
    if(!loopActive || beat < transport.loopEndBeat) return beat;
    const double loopSeconds = secondsAt(transport.loopEndBeat) - secondsAt(transport.loopStartBeat);
    double remaining = offsetSeconds - (secondsAt(transport.loopEndBeat) - secondsAt(transport.beatPosition));
    if(loopSeconds > 0.0) remaining = std::fmod(std::max(0.0, remaining), loopSeconds);
    return std::min(advance(transport.loopStartBeat, remaining), transport.loopEndBeat);
}

bool ofxOceanodeTimelineManager::scheduledValueAt(const ofxOceanodeTransportState& transport,
                                                  uint64_t nowUs, uint64_t dueUs,
                                                  const std::string& path,
                                                  ofxOceanodeAbstractParameter* parameter,
                                                  std::string& outValue) const {
    const auto bindingsIt = bindingsByPathCache.find(path);
    if(parameter == nullptr || bindingsIt == bindingsByPathCache.end()) return false;
    // A stopped or frame-stepped transport does not move with the clock: what
    // it holds now is what it will hold then.
    if(transport.isPlaying && transport.driverMode != TransportDriverMode::FrameStep) {
        ActiveValueMap values;
        const std::set<std::string> only{path};
        const double offsetSeconds = dueUs > nowUs ? static_cast<double>(dueUs - nowUs) / 1000000.0 : 0.0;
        const double beat = beatAtOffset(transport, offsetSeconds);
        // Behind the playhead means the probe wrapped into the next loop pass.
        const bool nextPass = transport.loopActive() && beat < transport.beatPosition;
        collectActiveValues(playContextFor(transport, nextPass ? 1 : 0), beat + 1e-7, true, false, values, nullptr, &only);
        if(computeParameterValue(bindingsIt->second, values, parameter, outValue)) return true;
    }
    return computeParameterValue(bindingsIt->second, activeAutomationValues, parameter, outValue);
}

// Events already handed to a backend cannot be recalled (scsynth's /clearSched
// is global and would also drop whatever an unrelated part of the patch
// scheduled). Instead every path that has something queued is given the value
// it will hold at an instant after everything queued for it: the stale events
// still execute, and this lands on top of them. It must be the value due at
// that instant, not the one the playhead holds now -- events newly scheduled
// before it (after a seek or an edit) would otherwise be overwritten by it.
void ofxOceanodeTimelineManager::sendScheduleCorrections(const ofxOceanodeTransportState& transport,
                                                         uint64_t nowUs,
                                                         const std::set<std::string>* onlyPaths) {
    if(container == nullptr) {
        scheduledPaths.clear();
        return;
    }
    if(!evaluationIndexesValid) rebuildEvaluationIndexes();
    for(auto& entry : scheduledPaths) {
        if(onlyPaths != nullptr && onlyPaths->count(entry.first) == 0) continue;
        auto& state = entry.second;
        if(state.pending.empty() && state.lastScheduledDueUs <= nowUs) continue;
        auto* parameter = findParameterCached(entry.first);
        if(parameter == nullptr) continue;
        const uint64_t dueUs = std::max(nowUs, state.lastScheduledDueUs + 1000);
        std::string value;
        if(!scheduledValueAt(transport, nowUs, dueUs, entry.first, parameter, value)) {
            // No longer automated: land on what is playing now, after the
            // queued events, so none of them is left standing.
            while(!state.pending.empty() && state.pending.front().first <= nowUs) {
                state.valueInEffect = state.pending.front().second;
                state.pending.pop_front();
            }
            value = state.valueInEffect;
            if(value.empty()) continue;
        }
        ofxOceanodeScheduledParameterEvent event;
        event.value = value;
        event.dueSteadyTimeUs = dueUs;
        event.generation = transport.generation;
        event.isCorrection = true;
        ofxOceanodeScheduling::dispatch(parameter, event);
    }
    if(onlyPaths == nullptr) scheduledPaths.clear();
    else for(const auto& path : *onlyPaths) scheduledPaths.erase(path);
}

// The beat reached `seconds` after startBeat, following the tempo map.
double ofxOceanodeTimelineManager::beatAfterSeconds(double startBeat, double seconds,
                                                    float fallbackBpm) const {
    if(seconds <= 0.0) return startBeat;
    if(!bpmAutomationEnabled || bpmAutomationPoints.empty())
        return startBeat + seconds * std::max(1.0f, fallbackBpm) / 60.0;
    return secondsToBeat(beatToSeconds(startBeat, fallbackBpm) + seconds, fallbackBpm);
}

void ofxOceanodeTimelineManager::collectChangeBeats(const ofxOceanodeTimelinePlayContext& context,
                                                    double fromBeat, double toBeat,
                                                    bool inclusiveStart,
                                                    std::vector<double>& outBeats) const {
    constexpr size_t kMaximumChangeBeats = 4096;
    if(toBeat < fromBeat) return;
    const uint64_t mutationEpoch = context.epoch();
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
    for(const auto& track : tracks) {
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
            addTimelineBeat(clipStart);
            addTimelineBeat(clipEnd);
            if(clipEnd < fromBeat - kEpsilon || clipStart > toBeat + kEpsilon) continue;

            const double windowStart = std::max(fromBeat, clipStart);
            const double windowEnd = std::min(toBeat, clipEnd);
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

                auto addSourceBeat = [&](double sourceBeat) {
                    if(sourceBeat < sourceFrom - kEpsilon || sourceBeat > sourceTo + kEpsilon) return;
                    addTimelineBeat(ofxOceanodeTimelineClipTime::sourceToTimelineBeat(clip, sourceBeat, cycle));
                };
                // The start of a repetition restarts the pattern (and re-rolls
                // per-cycle probabilities), so it is always a candidate.
                addSourceBeat(0.0);

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
                                                                     sourceTo, cycle, sourceLength, gap, mutationEpoch, hits);
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
                                const auto& played = repeatPlayback(context, track, clip, lane, shared, cycle);
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

    std::sort(outBeats.begin(), outBeats.end());
    outBeats.erase(std::unique(outBeats.begin(), outBeats.end(),
                               [](double a, double b) { return std::abs(a - b) <= kEpsilon; }),
                   outBeats.end());
}

double ofxOceanodeTimelineManager::effectiveSchedulingLookaheadMs() const {
    if(!schedulingLookaheadAdaptive) return std::max(0.0, schedulingLookaheadMs);
    const double adaptive = std::max(50.0, 3000.0 * frameIntervalPeakSeconds + 10.0);
    return std::max(0.0, std::min(schedulingLookaheadMs, adaptive));
}

std::vector<std::string> ofxOceanodeTimelineManager::selectSchedulablePaths() const {
    std::vector<std::string> schedulablePaths;
    for(const auto& path : orderedAutomationPaths()) {
        // Continuous lanes (curves, LFOs, live overrides) keep the frame path.
        if(continuouslyEvaluatedPathsCache.count(path) > 0) continue;
        auto* parameter = findParameterCached(path);
        // Only a backend that can execute a value at an instant gets events.
        if(parameter == nullptr || !ofxOceanodeScheduling::hasParameterTarget(parameter)) continue;
        schedulablePaths.push_back(path);
    }
    // A piano note is one event across Pitch, Velocity and Gate. When one of
    // its routed roles is held on the frame path although its backend could
    // schedule it (a curve, an LFO or a live override shares it), a scheduled
    // Gate would open with the previous note's pitch: the whole lane keeps the
    // frame path. (A role whose consumer cannot schedule at all is untimed
    // either way and holds nothing back.) Lanes can share a role path, so
    // repeat until no group is split.
    const auto scheduled = [&](const std::string& path) {
        return std::find(schedulablePaths.begin(), schedulablePaths.end(), path) != schedulablePaths.end();
    };
    const auto heldBack = [&](const std::string& path) {
        if(scheduled(path)) return false;
        auto* parameter = findParameterCached(path);
        return parameter != nullptr && ofxOceanodeScheduling::hasParameterTarget(parameter);
    };
    for(bool split = !pianoRoleGroupsCache.empty(); split;) {
        split = false;
        for(const auto& group : pianoRoleGroupsCache) {
            if(std::none_of(group.begin(), group.end(), heldBack) ||
               std::none_of(group.begin(), group.end(), scheduled)) continue;
            schedulablePaths.erase(std::remove_if(schedulablePaths.begin(), schedulablePaths.end(),
                [&](const std::string& path) {
                    return std::find(group.begin(), group.end(), path) != group.end();
                }), schedulablePaths.end());
            split = true;
        }
    }
    return schedulablePaths;
}

// The frame path needs a gate-off of at least 1.2 frames between back-to-back
// notes to see it at all. A lane whose targets are all scheduled has its gate
// boundaries executed at their exact instant, so a few milliseconds (a couple
// of control blocks) is enough for the instrument to see a new edge, and the
// note opens that much closer to the grid.
double ofxOceanodeTimelineManager::laneRetriggerGapBeats(const ofxOceanodeTimelineTrack& track,
                                                         const ofxOceanodeTimelineLane& lane) const {
    if(fastRetriggerPaths.empty()) return retriggerGapBeats;
    const auto trackIt = trackEvaluationIndexes.find(&track);
    if(trackIt == trackEvaluationIndexes.end()) return retriggerGapBeats;
    const auto scheduled = [&](const std::string& bindingId) {
        const auto it = trackIt->second.bindingById.find(bindingId);
        return it != trackIt->second.bindingById.end() &&
               fastRetriggerPaths.count(it->second->parameterPath) > 0;
    };
    const bool allScheduled = ofxOceanodeTimelineLaneUsesNoteRoles(lane.type)
        ? scheduled(lane.pianoGateBindingId)
        : !lane.bindingIds.empty() && std::all_of(lane.bindingIds.begin(), lane.bindingIds.end(), scheduled);
    return allScheduled ? scheduledRetriggerGapBeats : retriggerGapBeats;
}

void ofxOceanodeTimelineManager::runScheduler(const ofxOceanodeTransportState& transport,
                                              bool loopWrappedThisFrame) {
    if(container == nullptr) return;
    // The instant the reported beat position belongs to. Everything the
    // scheduler computes is an offset from it, which is why a late or long
    // frame shifts nothing: only the window's far edge moves.
    const uint64_t nowUs = transport.steadyTimeUs != 0
        ? transport.steadyTimeUs : ofxOceanodeScheduling::steadyNowUs();
    // Due times are wall-clock projections of the playhead. A frame-stepped
    // transport (frame mode, offline/NRT renders) advances a fixed step per
    // frame whatever the clock does, so the backend would play the notes on
    // the clock while the playhead lags or leads them -- and an NRT capture
    // would record them a lookahead early. It keeps the frame path.
    const bool schedulingActive = schedulingEnabled && transport.isPlaying &&
        transport.driverMode != TransportDriverMode::FrameStep;

    const bool generationChanged = hasSeenTransportGeneration &&
        lastSeenTransportGeneration != transport.generation;
    lastSeenTransportGeneration = transport.generation;
    hasSeenTransportGeneration = true;
    // A loop wrap is continuous playback (the transport does not bump the
    // generation for it); only seeks, stops and edits invalidate what was
    // scheduled.
    const bool invalidated = generationChanged || scheduleInvalidationPending;
    scheduleInvalidationPending = false;

    if(!schedulingActive || invalidated) {
        if(!scheduledPaths.empty()) sendScheduleCorrections(transport, nowUs);
        scheduledPaths.clear();
        scheduledBackendPaths.clear();
        hasScheduleCursor = false;
        if(!schedulingActive) return;
    }

    // ---- which paths can be scheduled at all -----------------------------
    // Selected by evaluateAutomation() before this frame's evaluation, which
    // already used it for the scheduled lanes' retrigger gap.
    struct PathTarget {
        ofxOceanodeAbstractParameter* parameter = nullptr;
        const std::vector<ofxOceanodeTimelineParameterBinding*>* bindings = nullptr;
    };
    const std::vector<std::string>& schedulablePaths = schedulablePathsThisFrame;
    std::unordered_map<std::string, PathTarget> targets;
    targets.reserve(schedulablePaths.size());
    for(const auto& path : schedulablePaths)
        targets[path] = {findParameterCached(path), &bindingsByPathCache.at(path)};
    if(schedulablePaths.empty()) {
        // Same rule as every other invalidation path: events already handed
        // to a backend cannot be recalled, so correct them before forgetting
        // they exist. sendScheduleCorrections clears scheduledPaths itself.
        if(!scheduledPaths.empty()) sendScheduleCorrections(transport, nowUs);
        scheduledPaths.clear();
        scheduledBackendPaths.clear();
        return;
    }

    // ---- promote what became due, and repair anything unforeseen ---------
    std::set<std::string> activePaths;
    // Suppression starts one frame later than scheduling: on the frame a path
    // joins the scheduler the backend has not been given anything yet, so that
    // frame's ordinary send is what puts the current value in place.
    std::set<std::string> suppressedPaths;
    // The playhead and the backend's clock agree only to within a frame or
    // so (frame jitter, an external clock's reports, a tempo ramp sampled per
    // frame). A difference that small is timing, not content to repair.
    const uint64_t toleranceUs = static_cast<uint64_t>(1000000.0 *
        std::max(0.020, 2.0 * std::min(0.25, static_cast<double>(ofGetLastFrameTime()))));
    for(const auto& path : schedulablePaths) {
        auto& target = targets[path];
        std::string frameValue;
        if(!computeParameterValue(*target.bindings, activeAutomationValues, target.parameter, frameValue))
            frameValue = parameterValueForAutomation(*target.parameter); // between clips: the held value
        auto& state = scheduledPaths[path];
        const bool wasInitialized = state.initialized;
        while(!state.pending.empty() && state.pending.front().first <= nowUs) {
            if(state.pending.front().second != state.valueInEffect) {
                state.previousValueInEffect = state.valueInEffect;
                state.valueInEffectSinceUs = state.pending.front().first;
            }
            state.valueInEffect = state.pending.front().second;
            state.pending.pop_front();
        }
        const auto explainedByTiming = [&] {
            // The playhead is about to reach a queued value...
            for(const auto& queued : state.pending) {
                if(queued.first > nowUs + toleranceUs) break;
                if(queued.second == frameValue) return true;
            }
            // ...or has not quite reached the one the backend just applied.
            return state.valueInEffectSinceUs + toleranceUs >= nowUs &&
                   state.previousValueInEffect == frameValue;
        };
        if(!state.initialized) {
            state.initialized = true;
            state.valueInEffect = frameValue;
            state.previousValueInEffect.clear();
            state.valueInEffectSinceUs = 0;
            state.repairQuietUntilUs = 0;
            state.lastScheduledValue = frameValue;
            state.lastScheduledDueUs = nowUs;
        } else if(frameValue != state.valueInEffect && nowUs >= state.repairQuietUntilUs &&
                  !explainedByTiming()) {
            // The value the playhead actually has is not the one the backend
            // was told to play: the content was edited inside the window, or
            // something outside the timeline moved. Correct it after
            // everything already queued for this path, with the value due
            // then. The queued events still execute; until the correction
            // lands they are not compared again.
            const uint64_t dueUs = std::max(nowUs, state.lastScheduledDueUs + 1000);
            std::string value;
            if(!scheduledValueAt(transport, nowUs, dueUs, path, target.parameter, value)) value = frameValue;
            ofxOceanodeScheduledParameterEvent event;
            event.value = value;
            event.dueSteadyTimeUs = dueUs;
            event.generation = transport.generation;
            event.isCorrection = true;
            ofxOceanodeScheduling::dispatch(target.parameter, event);
            state.pending.emplace_back(dueUs, value);
            state.lastScheduledValue = value;
            state.lastScheduledDueUs = dueUs;
            state.repairQuietUntilUs = dueUs;
        }
        activePaths.insert(path);
        if(wasInitialized) suppressedPaths.insert(path);
    }
    {
        // Paths that stopped being scheduled (their binding went away, a curve
        // or a live override took them over): correct what is still queued.
        std::set<std::string> dropped;
        for(const auto& entry : scheduledPaths)
            if(activePaths.count(entry.first) == 0) dropped.insert(entry.first);
        if(!dropped.empty()) sendScheduleCorrections(transport, nowUs, &dropped);
    }
    scheduledBackendPaths = suppressedPaths;
    if(activePaths.empty()) return;

    // ---- track latency correction ----------------------------------------
    // Each track's offset moves when its events reach the backend, nothing
    // else: the playhead, the values in effect and the repair checks stay on
    // musical time. An early track needs its events that much sooner, so the
    // window reaches further ahead by the earliest offset.
    std::unordered_map<std::string, int64_t> latencyOffsetUs;
    int64_t earliestOffsetUs = 0;
    for(const auto& track : tracks) {
        if(track.latencyOffsetMs == 0.0f) continue;
        const int64_t offsetUs = static_cast<int64_t>(std::llround(
            ofClamp(track.latencyOffsetMs, -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs) * 1000.0));
        for(const auto& binding : track.bindings) {
            if(activePaths.count(binding.parameterPath) == 0) continue;
            latencyOffsetUs.emplace(binding.parameterPath, offsetUs);
            earliestOffsetUs = std::min(earliestOffsetUs, offsetUs);
        }
    }

    // ---- walk the window -------------------------------------------------
    // The window can run past the loop end: events of the next pass are
    // scheduled at the exact instant the transport will wrap, so the loop's
    // first beat is on time too.
    const double lookaheadSeconds = effectiveSchedulingLookaheadMs() / 1000.0 - earliestOffsetUs / 1000000.0;
    const bool loopActive = transport.loopActive() &&
        transport.beatPosition < transport.loopEndBeat - kEpsilon;
    const uint64_t pass = transport.loopCount;
    if(!hasScheduleCursor || scheduleCursorPass < pass) {
        // First window, or the transport wrapped before anything past the
        // loop end was scheduled: start from where it is now.
        scheduleCursorBeat = transport.beatPosition;
        scheduleCursorPass = pass;
        hasScheduleCursor = true;
    }
    auto secondsAt = [&](double beat) {
        if(!scheduleUsesTempoMap) return beat * 60.0 / std::max(1.0f, transport.bpm);
        return beatToSeconds(beat, transport.bpm);
    };
    const double secondsNow = secondsAt(transport.beatPosition);
    const double horizonBeat = scheduleUsesTempoMap
        ? beatAfterSeconds(transport.beatPosition, lookaheadSeconds, transport.bpm)
        : transport.beatPosition + lookaheadSeconds * std::max(1.0f, transport.bpm) / 60.0;

    // (beat, seconds from now) pairs to schedule, in time order.
    std::vector<std::pair<double, double>> changes;
    if(scheduleCursorPass == pass) {
        if(scheduleCursorBeat < transport.beatPosition) scheduleCursorBeat = transport.beatPosition;
        const double thisPassEnd = loopActive ? std::min(horizonBeat, transport.loopEndBeat) : horizonBeat;
        if(thisPassEnd > scheduleCursorBeat) {
            std::vector<double> beats;
            collectChangeBeats(playContextFor(transport), scheduleCursorBeat, thisPassEnd, false, beats);
            for(const double beat : beats) {
                // The loop end itself belongs to the next pass (its start).
                if(loopActive && beat >= transport.loopEndBeat - kEpsilon) continue;
                changes.push_back({beat, secondsAt(beat) - secondsNow});
            }
            scheduleCursorBeat = thisPassEnd;
        }
    }
    // Changes from here on belong to the next loop pass (clip chance re-rolls).
    const size_t firstNextPassChange = changes.size();
    if(loopActive && horizonBeat > transport.loopEndBeat) {
        // The next pass: from the loop start (inclusive) for as long as the
        // window reaches past the end.
        const double untilWrap = secondsAt(transport.loopEndBeat) - secondsNow;
        const double nextPassEnd = std::min(transport.loopEndBeat,
            transport.loopStartBeat + (horizonBeat - transport.loopEndBeat));
        const bool freshPass = scheduleCursorPass == pass;
        const double nextPassFrom = freshPass ? transport.loopStartBeat : scheduleCursorBeat;
        if(nextPassEnd > nextPassFrom || freshPass) {
            std::vector<double> beats;
            // The next pass is a new clip start: its own chance and mutations.
            collectChangeBeats(playContextFor(transport, 1), nextPassFrom, nextPassEnd, freshPass, beats);
            const double startSeconds = secondsAt(transport.loopStartBeat);
            for(const double beat : beats)
                changes.push_back({beat, untilWrap + secondsAt(beat) - startSeconds});
            scheduleCursorBeat = std::max(nextPassFrom, nextPassEnd);
            scheduleCursorPass = pass + 1;
        }
    }
    if(changes.empty()) return;

    ActiveValueMap futureValues;
    futureValues.reserve(bindingsByPathCache.size());
    for(size_t changeIndex = 0; changeIndex < changes.size(); ++changeIndex) {
        const auto& change = changes[changeIndex];
        const double beat = change.first;
        const double offsetSeconds = change.second;
        const uint64_t dueUs = nowUs + static_cast<uint64_t>(std::max(0.0, offsetSeconds) * 1000000.0);
        const auto context = playContextFor(transport, changeIndex >= firstNextPassChange ? 1 : 0);
        // Evaluate just past the boundary: every lane's own test is
        // "start <= beat < end", so the probe must land inside the new
        // step/note/region, not exactly on its edge.
        collectActiveValues(context, beat + 1e-7, true, false, futureValues, nullptr, &activePaths);
        for(const auto& path : schedulablePaths) {
            if(activePaths.count(path) == 0) continue;
            auto& target = targets[path];
            std::string value;
            if(!computeParameterValue(*target.bindings, futureValues, target.parameter, value)) continue;
            auto& state = scheduledPaths[path];
            if(value == state.lastScheduledValue) continue;
            ofxOceanodeScheduledParameterEvent event;
            event.value = value;
            // Never let one path's events swap order, whatever the rounding of
            // the tempo integral did.
            const uint64_t leadUs = pianoInputLeadUs(path);
            const auto offsetIt = latencyOffsetUs.find(path);
            const int64_t offsetUs = offsetIt == latencyOffsetUs.end() ? 0 : offsetIt->second;
            const int64_t shiftedDueUs = static_cast<int64_t>(dueUs) - static_cast<int64_t>(leadUs) + offsetUs;
            const uint64_t preparedDueUs = static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(nowUs), shiftedDueUs));
            event.dueSteadyTimeUs = std::max(preparedDueUs, state.lastScheduledDueUs + 1);
            event.generation = transport.generation;
            if(!ofxOceanodeScheduling::dispatch(target.parameter, event)) {
                // The backend could not take it (no synth yet): drop this path
                // back to the frame path for now.
                scheduledBackendPaths.erase(path);
                state.initialized = false;
                state.pending.clear();
                continue;
            }
            state.lastScheduledValue = value;
            state.lastScheduledDueUs = event.dueSteadyTimeUs;
            // Promotion/correction compares against the GUI playhead. Pitch
            // is already prepared in the backend during the lead interval,
            // but remains logically unchanged until its musical boundary.
            const int64_t musicalDueUs = static_cast<int64_t>(event.dueSteadyTimeUs) - offsetUs;
            state.pending.emplace_back(std::max<int64_t>(static_cast<int64_t>(dueUs), musicalDueUs), value);
        }
    }
}

void ofxOceanodeTimelineManager::setLiveOverride(const std::string& trackId, const std::string& bindingId, const std::string& value) {
    if(auto* binding = getBinding(trackId, bindingId)) {
        stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + bindingId);
        binding->hasLiveOverride = true;
        binding->liveOverrideValue = value;
    }
}

void ofxOceanodeTimelineManager::setPianoKeyPreview(const std::string& trackId, const std::string& clipId,
                                                    const std::string& laneId, int pitch) {
    const auto* track = getTrack(trackId);
    const auto* clip = getClip(trackId, clipId);
    const auto* lane = getLane(trackId, clipId, laneId);
    if(track == nullptr || clip == nullptr || lane == nullptr) return;
    ofxOceanodeTimelinePianoVoicing voicing;
    computeLaneVoicing(*track, *clip, *lane, voicing);
    const int slot = voicing.kit ? pitch - kKitBaseNote : 0;
    if(slot < 0 || (voicing.kit && slot >= voicing.outputWidth)) return;
    auto width = [&](const ofxOceanodeTimelineParameterBinding& binding) {
        size_t size = static_cast<size_t>(std::max({voicing.outputWidth, slot + 1, 2}));
        if(auto* parameter = findParameterCached(binding.parameterPath)) {
            if(binding.valueType == typeid(std::vector<float>).name())
                size = std::max(size, parameter->cast<std::vector<float>>().getParameter().get().size());
            else if(binding.valueType == typeid(std::vector<int>).name())
                size = std::max(size, parameter->cast<std::vector<int>>().getParameter().get().size());
        }
        return size;
    };
    auto isVector = [](const std::string& type) {
        return type == typeid(std::vector<float>).name() || type == typeid(std::vector<int>).name() ||
               type == typeid(std::vector<bool>).name();
    };
    if(const auto* gate = getBinding(trackId, lane->pianoGateBindingId)) {
        const bool level = voicing.instrument && lane->pianoVelocityBindingId.empty() &&
            (gate->valueType == typeid(float).name() || gate->valueType == typeid(std::vector<float>).name());
        const std::string on = level ? automationText::formatFloat(lane->pianoDefaultVelocity) : std::string("1");
        std::string value = on;
        if(isVector(gate->valueType)) {
            std::vector<std::string> slots(width(*gate), "0");
            slots[slot] = on;
            value = joinAutomationValues(slots);
        }
        setLiveOverride(trackId, gate->id, value);
        playingPianoGatePaths.insert(gate->parameterPath);
    }
    if(const auto* pitchBinding = getBinding(trackId, lane->pianoPitchBindingId); pitchBinding != nullptr && !voicing.kit)
        setLiveOverride(trackId, pitchBinding->id, automationText::formatInt(pitch));   // a scalar: broadcast to every voice
}

void ofxOceanodeTimelineManager::clearPianoKeyPreview(const std::string& trackId, const std::string& clipId,
                                                      const std::string& laneId) {
    const auto* lane = getLane(trackId, clipId, laneId);
    if(lane == nullptr) return;
    if(!lane->pianoGateBindingId.empty()) clearLiveOverride(trackId, lane->pianoGateBindingId);
    if(!lane->pianoPitchBindingId.empty()) clearLiveOverride(trackId, lane->pianoPitchBindingId);
}

void ofxOceanodeTimelineManager::clearLiveOverride(const std::string& trackId, const std::string& bindingId) {
    if(auto* binding = getBinding(trackId, bindingId)) {
        stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + bindingId);
        binding->hasLiveOverride = false;
        binding->liveOverrideValue.clear();
    }
}

void ofxOceanodeTimelineManager::clearTimelineFlag(const ofxOceanodeTimelineTrack& track) {
    if(container == nullptr) return;
    for(const auto& binding : track.bindings) {
        if(auto* parameter = container->findTimelineParameter(binding.parameterPath)) parameter->setTimelined(false);
    }
}

void ofxOceanodeTimelineManager::refreshTimelineFlag(const std::string& parameterPath) {
    if(container == nullptr) return;
    const bool isActive = std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) {
        return std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& binding) {
            return binding.parameterPath == parameterPath && !binding.bypass;
        });
    });
    if(auto* parameter = container->findTimelineParameter(parameterPath))
        parameter->setTimelined(isActive);
}

// Timeline history follows the canvas's gesture transactions and history
// limits, but owns only timeline data. Replaying it must not reload the patch
// or rewind live parameter values and the transport.
ofJson ofxOceanodeTimelineManager::timelineEditData() const {
    auto state = toJson();
    state.erase("view");
    return state;
}

ofxOceanodeTimelineManager::TimelineState ofxOceanodeTimelineManager::captureTimelineEdit() const {
    TimelineState state;
    state.data = timelineEditData();
    state.tracks = tracks;
    state.groups = clipGroups;
    state.markers = markers;
    state.tempoPoints = bpmAutomationPoints;
    state.tempoTensions = bpmCurveTensions;
    state.nextTrack = nextTrackNumber;
    state.nextBinding = nextBindingNumber;
    state.nextClip = nextClipNumber;
    state.nextLane = nextLaneNumber;
    state.nextGroup = nextGroupNumber;
    // Include the typed copy and audio caches in the history budget.
    state.bytes = state.data.dump().size() * 2;
    for(auto& track : state.tracks) {
        for(auto& binding : track.bindings) {
            binding.hasLiveOverride = false;
            binding.liveOverrideValue.clear();
        }
        for(const auto& clip : track.clips)
            state.bytes += clip.waveformPeaks.size() * sizeof(float);
    }
    return state;
}

void ofxOceanodeTimelineManager::restoreTimelineState(const TimelineState& state) {
    // Use in-memory data instead of the preset importer: that importer migrates
    // and normalizes content and reopens audio files. History must restore exact
    // editor data, including waveform caches for files no longer on disk.
    const auto oldView = viewState;
    clear();
    tracks = state.tracks;
    clipGroups = state.groups;
    markers = state.markers;
    bpmAutomationPoints = state.tempoPoints;
    bpmCurveTensions = state.tempoTensions;
    nextTrackNumber = state.nextTrack;
    nextBindingNumber = state.nextBinding;
    nextClipNumber = state.nextClip;
    nextLaneNumber = state.nextLane;
    nextGroupNumber = state.nextGroup;
    const auto& data = state.data;
    timebase = data["timebase"] == "Time" ? ofxOceanodeTimelineTimebase::Time : ofxOceanodeTimelineTimebase::Beats;
    playOnPresetLoad = data["playOnPresetLoad"];
    const auto& tempo = data["tempo"];
    bpmAutomationEnabled = tempo["enabled"];
    bpmLaneCollapsed = tempo["collapsed"];
    bpmLaneVisible = tempo["visible"];
    bpmMinimum = tempo["minimum"];
    bpmMaximum = tempo["maximum"];
    bpmInterpolation = tempo["interpolation"].get<std::string>();
    loopEnabled = data["loop"]["enabled"];
    loopStartBeat = data["loop"]["startBeat"];
    loopEndBeat = data["loop"]["endBeat"];
    schedulingEnabled = data["scheduling"]["enabled"];
    schedulingLookaheadMs = data["scheduling"]["lookaheadMs"];
    schedulingLookaheadAdaptive = data["scheduling"].value("adaptive", true);
    setTimeSignature(data["timeSignature"]["numerator"], data["timeSignature"]["denominator"]);
    masterGridBeats = data.value("masterGridBeats", 0.25);
    viewState = oldView;
    syncLoopToTransport();
    for(const auto& track : tracks)
        for(const auto& binding : track.bindings) refreshTimelineFlag(binding.parameterPath);
}

void ofxOceanodeTimelineManager::beginTimelineEdit() {
    if(replayingTimelineEdit || pendingTimelineEdit) return;
    pendingTimelineEdit = std::make_unique<TimelineEdit>();
    pendingTimelineEdit->before = captureTimelineEdit();
    if(container != nullptr && container->getTransport() != nullptr &&
       !container->getTransport()->hasExternalClock() && !bpmAutomationEnabled) {
        pendingTimelineEdit->manualTempo = true;
        pendingTimelineEdit->beforeBpm = container->getTransportState().bpm;
    }
}

void ofxOceanodeTimelineManager::endTimelineEdit() {
    if(!pendingTimelineEdit) return;
    auto edit = std::move(pendingTimelineEdit);
    edit->after = captureTimelineEdit();
    edit->manualTempo = edit->manualTempo && !bpmAutomationEnabled &&
        container != nullptr && !container->getTransport()->hasExternalClock();
    if(edit->manualTempo) {
        edit->afterBpm = container->getTransportState().bpm;
        edit->manualTempo = std::abs(edit->beforeBpm - edit->afterBpm) > 0.0001f;
    }
    if(edit->before.data == edit->after.data && !edit->manualTempo) return;
    edit->bytes = edit->before.bytes + edit->after.bytes;
    timelineRedo.clear();
    timelineUndo.push_back(std::move(*edit));
    trimTimelineHistory();
}

void ofxOceanodeTimelineManager::trimTimelineHistory() {
    constexpr size_t maxEntries = 100;
    constexpr size_t maxBytes = 64 * 1024 * 1024;
    size_t bytes = 0;
    for(const auto* stack : {&timelineUndo, &timelineRedo})
        for(const auto& edit : *stack) bytes += edit.bytes;
    while(timelineUndo.size() + timelineRedo.size() > maxEntries || bytes > maxBytes) {
        auto& stack = timelineUndo.size() > 1 || timelineRedo.empty() ? timelineUndo : timelineRedo;
        if(stack.empty()) break;
        bytes -= stack.front().bytes;
        stack.pop_front();
    }
}

void ofxOceanodeTimelineManager::clearTimelineHistory() {
    pendingTimelineEdit.reset();
    timelineUndo.clear();
    timelineRedo.clear();
}

bool ofxOceanodeTimelineManager::stepTimelineHistory(bool backwards) {
    endTimelineEdit();
    auto& source = backwards ? timelineUndo : timelineRedo;
    auto& destination = backwards ? timelineRedo : timelineUndo;
    if(source.empty()) return false;
    const auto& edit = source.back();
    const bool tempoMismatch = edit.manualTempo && container != nullptr &&
        (container->getTransport() == nullptr || container->getTransport()->hasExternalClock() ||
         std::abs(container->getTransportState().bpm - (backwards ? edit.afterBpm : edit.beforeBpm)) > 0.0001f);
    if(tempoMismatch || timelineEditData() != (backwards ? edit.after.data : edit.before.data)) {
        // An unrecorded edit (e.g. parameter removal from the canvas) must
        // never be overwritten by restoring an older timeline wholesale.
        clearTimelineHistory();
        return false;
    }
    const auto& target = backwards ? edit.before : edit.after;
    replayingTimelineEdit = true;
    try {
        // Set manual tempo before restoring clip positions: the container's
        // Time timebase retiming must not rescale an already restored snapshot.
        if(edit.manualTempo && container != nullptr &&
           !container->getTransport()->hasExternalClock())
            container->setBpm(backwards ? edit.beforeBpm : edit.afterBpm, false);
        restoreTimelineState(target);
        replayingTimelineEdit = false;
    } catch(...) {
        replayingTimelineEdit = false;
        clearTimelineHistory();
        throw;
    }
    destination.push_back(std::move(source.back()));
    source.pop_back();
    return true;
}

bool ofxOceanodeTimelineManager::undoTimelineEdit() { return stepTimelineHistory(true); }
bool ofxOceanodeTimelineManager::redoTimelineEdit() { return stepTimelineHistory(false); }

void ofxOceanodeTimelineManager::clear() {
    if(!replayingTimelineEdit) clearTimelineHistory();
    // Correct anything already queued on a backend while the bindings that
    // know the right values still exist (afterwards a queued gate-on would
    // play with nothing to turn it off).
    invalidateSchedule();
    for(const auto& track : tracks) clearTimelineFlag(track);
    endParameterCache();
    tracks.clear();
    invalidateEvaluationIndexes();
    clipGroups.clear();
    markers.clear();
    viewState = {};
    playOnPresetLoad = false;
    timebase = ofxOceanodeTimelineTimebase::Beats;
    nextTrackNumber = 1;
    nextBindingNumber = 1;
    nextClipNumber = 1;
    nextLaneNumber = 1;
    nextGroupNumber = 1;
    pendingParameterLanes.clear();
    pendingTrackRenameId.clear();
    pendingTrackRenameIsNew = false;
    pendingNoteGroupSetupParameter = nullptr;
    activeAutomationValues.clear();
    // playingPianoGatePaths survives: a gate held open by the content being
    // replaced (an undo, a preset load) is released on the next update.
    stateCaptureLastAppliedValues.clear();
    stateCaptureManualOverrideKeys.clear();
    invalidateSchedule();
    bpmAutomationEnabled = false;
    bpmLaneCollapsed = true;
    bpmLaneVisible = false;
    bpmMinimum = 20.0f;
    bpmMaximum = 300.0f;
    bpmAutomationPoints.clear();
    bpmCurveTensions.clear();
    bpmInterpolation = "Linear";
    loopEnabled = false;
    loopStartBeat = 0.0;
    loopEndBeat = 4.0;
    syncLoopToTransport();
    hasEvaluatedTransportBeat = false;
    lastEvaluatedTransportBeat = 0.0;
    timeSignatureNumerator = 4;
    timeSignatureDenominator = 4;
    masterGridBeats = 0.25;
}

void ofxOceanodeTimelineManager::setMasterGridBeats(double beats) {
    masterGridBeats = std::isfinite(beats) ? ofClamp(beats, 0.0, 16.0) : 0.25;
}

void ofxOceanodeTimelineManager::retimeForBpmChange(float oldBpm, float newBpm) {
    if(timebase != ofxOceanodeTimelineTimebase::Time ||
       oldBpm <= 0.0f || newBpm <= 0.0f) return;
    const double scale = static_cast<double>(newBpm) / static_cast<double>(oldBpm);
    if(!std::isfinite(scale) || scale <= 0.0 || std::abs(scale - 1.0) <= kEpsilon) return;

    // These values live directly in global timeline-beat space.
    for(auto& marker : markers) marker.beat *= scale;
    for(auto& point : bpmAutomationPoints) point.beat *= scale;
    loopStartBeat *= scale;
    loopEndBeat = std::max(loopStartBeat + 1.0 / 24.0, loopEndBeat * scale);

    for(auto& track : tracks) {
        for(auto& point : track.waveVolumePoints) point.beat *= scale;
        for(auto& clip : track.clips) {
            clip.startBeat *= scale;
            clip.durationBeats = std::max(1.0 / 24.0, clip.durationBeats * scale);
            // Local lane data remains untouched. Scaling the local-to-global
            // mapping is enough to preserve the absolute time of every step,
            // note, curve point, LFO cycle and audio sample inside the clip.
            clip.contentStretch = std::max(1.0 / 1024.0, clip.contentStretch * scale);
            if(track.isWaveTrack) normalizeWaveClipMapping(clip);
        }
    }

    retriggerGapBeats = ofClamp(retriggerGapBeats * scale, 1.0 / 192.0, 0.25);
    tempoSecondsCacheValid = false;
    hasEvaluatedTransportBeat = false;
    invalidateEvaluationIndexes();
    invalidateSchedule();
    syncLoopToTransport();
}

void ofxOceanodeTimelineManager::setTimeSignature(int numerator, int denominator) {
    const double previousBeatsPerBar = getBeatsPerBar();
    timeSignatureNumerator = std::max(1, numerator);
    timeSignatureDenominator = std::max(1, denominator);
    if(!loopEnabled && std::abs(loopStartBeat) <= kEpsilon &&
       std::abs(loopEndBeat - previousBeatsPerBar) <= kEpsilon) {
        loopEndBeat = getBeatsPerBar();
    }
}

std::string ofxOceanodeTimelineManager::modeToString(ofxOceanodeTimelineAutomationMode mode) {
    switch(mode) {
        case ofxOceanodeTimelineAutomationMode::Add: return "Add";
        case ofxOceanodeTimelineAutomationMode::Multiply: return "Multiply";
        case ofxOceanodeTimelineAutomationMode::Min: return "Min";
        case ofxOceanodeTimelineAutomationMode::Max: return "Max";
        case ofxOceanodeTimelineAutomationMode::Replace:
        default: return "Replace";
    }
}

ofxOceanodeTimelineAutomationMode ofxOceanodeTimelineManager::modeFromString(const std::string& mode) {
    if(mode == "Add") return ofxOceanodeTimelineAutomationMode::Add;
    if(mode == "Multiply") return ofxOceanodeTimelineAutomationMode::Multiply;
    if(mode == "Min") return ofxOceanodeTimelineAutomationMode::Min;
    if(mode == "Max") return ofxOceanodeTimelineAutomationMode::Max;
    return ofxOceanodeTimelineAutomationMode::Replace;
}

std::string ofxOceanodeTimelineManager::laneTypeToString(ofxOceanodeTimelineLaneType laneType) {
    switch(laneType) {
        case ofxOceanodeTimelineLaneType::PianoRoll: return "PianoRoll";
        case ofxOceanodeTimelineLaneType::DrumGrid: return "DrumGrid";
        case ofxOceanodeTimelineLaneType::Number: return "Number";
        case ofxOceanodeTimelineLaneType::Dropdown: return "Dropdown";
        case ofxOceanodeTimelineLaneType::Curve: return "Curve";
        case ofxOceanodeTimelineLaneType::MultiValue: return "MultiValue";
        case ofxOceanodeTimelineLaneType::MultiSlider: return "MultiSlider";
        case ofxOceanodeTimelineLaneType::MultiGate: return "MultiGate";
        case ofxOceanodeTimelineLaneType::Wave: return "Wave";
        case ofxOceanodeTimelineLaneType::Step:
        default: return "Step";
    }
}

ofxOceanodeTimelineLaneType ofxOceanodeTimelineManager::laneTypeFromString(const std::string& laneType) {
    if(laneType == "PianoRoll") return ofxOceanodeTimelineLaneType::PianoRoll;
    if(laneType == "DrumGrid") return ofxOceanodeTimelineLaneType::DrumGrid;
    if(laneType == "Number") return ofxOceanodeTimelineLaneType::Number;
    if(laneType == "Dropdown") return ofxOceanodeTimelineLaneType::Dropdown;
    if(laneType == "Curve") return ofxOceanodeTimelineLaneType::Curve;
    if(laneType == "MultiValue") return ofxOceanodeTimelineLaneType::MultiValue;
    if(laneType == "MultiSlider") return ofxOceanodeTimelineLaneType::MultiSlider;
    if(laneType == "MultiGate") return ofxOceanodeTimelineLaneType::MultiGate;
    if(laneType == "Wave") return ofxOceanodeTimelineLaneType::Wave;
    return ofxOceanodeTimelineLaneType::Step;
}

ofJson ofxOceanodeTimelineManager::toJson() const {
    ofJson json;
    json["version"] = kPresetVersion;
    json["timebase"] = timebase == ofxOceanodeTimelineTimebase::Time ? "Time" : "Beats";
    json["view"] = {
        {"pixelsPerSecond", viewState.pixelsPerSecond},
        {"labelWidth", viewState.labelWidth},
        {"scrollX", viewState.scrollX}
    };
    json["tempo"] = {
        {"enabled", bpmAutomationEnabled},
        {"collapsed", bpmLaneCollapsed},
        {"visible", bpmLaneVisible},
        {"minimum", bpmMinimum},
        {"maximum", bpmMaximum},
        {"interpolation", bpmInterpolation},
        {"points", ofJson::array()},
        {"tensions", ofJson::array()}
    };
    for(const auto& point : bpmAutomationPoints)
        json["tempo"]["points"].push_back({{"beat", point.beat}, {"bpm", point.value}});
    for(const auto& tension : bpmCurveTensions)
        json["tempo"]["tensions"].push_back({{"inflection", tension.inflection}, {"steepness", tension.steepness}});
    json["playOnPresetLoad"] = playOnPresetLoad;
    json["loop"] = {
        {"enabled", loopEnabled},
        {"startBeat", loopStartBeat},
        {"endBeat", loopEndBeat}
    };
    json["markers"] = ofJson::array();
    for(const auto& marker : markers)
        json["markers"].push_back({{"id", marker.id}, {"name", marker.name}, {"beat", marker.beat}});
    json["timeSignature"] = {
        {"numerator", timeSignatureNumerator},
        {"denominator", timeSignatureDenominator}
    };
    json["masterGridBeats"] = masterGridBeats;
    json["scheduling"] = {
        {"enabled", schedulingEnabled},
        {"lookaheadMs", schedulingLookaheadMs},
        {"adaptive", schedulingLookaheadAdaptive}
    };
    json["tracks"] = ofJson::array();
    for(const auto& track : tracks) {
        ofJson trackJson;
        trackJson["id"] = track.id;
        trackJson["name"] = track.name;
        trackJson["collapsed"] = track.collapsed;
        trackJson["muted"] = track.muted;
        trackJson["solo"] = track.solo;
        trackJson["isWaveTrack"] = track.isWaveTrack;
        trackJson["waveVolume"] = track.waveVolume;
        trackJson["waveVolumeAutomationEnabled"] = track.waveVolumeAutomationEnabled;
        if(track.latencyOffsetMs != 0.0f) trackJson["latencyOffsetMs"] = track.latencyOffsetMs;
        trackJson["waveVolumeInterpolation"] = track.waveVolumeInterpolation;
        trackJson["waveVolumePoints"] = ofJson::array();
        for(const auto& point : track.waveVolumePoints)
            trackJson["waveVolumePoints"].push_back({{"beat", point.beat}, {"value", point.value}});
        trackJson["waveVolumeTensions"] = ofJson::array();
        for(const auto& tension : track.waveVolumeTensions)
            trackJson["waveVolumeTensions"].push_back({{"inflection", tension.inflection}, {"steepness", tension.steepness}});
        trackJson["color"] = {
            {"r", track.color.r}, {"g", track.color.g},
            {"b", track.color.b}, {"a", track.color.a}
        };
        trackJson["bindings"] = ofJson::array();
        for(const auto& binding : track.bindings) {
            trackJson["bindings"].push_back({
                {"id", binding.id},
                {"parameterPath", binding.parameterPath},
                {"valueType", binding.valueType},
                {"defaultValue", binding.defaultValue},
                {"mode", modeToString(binding.mode)},
                {"laneType", laneTypeToString(binding.laneType)},
                {"bypass", binding.bypass},
                {"clampToRange", binding.clampToParameterRange},
                {"rowHeight", binding.rowHeight},
                {"valueScale", binding.valueScale}
            });
        }
        trackJson["noteGroups"] = ofJson::array();
        for(const auto& group : track.noteGroups) {
            trackJson["noteGroups"].push_back({
                {"id", group.id},
                {"name", group.name},
                {"pitch", group.pitchBindingId},
                {"gate", group.gateBindingId},
                {"velocity", group.velocityBindingId},
                {"duration", group.durationBindingId},
                {"durationDecided", group.durationDecided},
                {"filter", group.filterBindingId},
                {"filterDecided", group.filterDecided},
                {"stutter", group.stutterBindingId},
                {"stutterDecided", group.stutterDecided},
                {"expanded", group.expanded},
                {"rowHeight", group.rowHeight},
                {"voices", group.voices}
            });
        }
        trackJson["clips"] = ofJson::array();
        for(const auto& clip : track.clips) {
            ofJson clipJson;
            clipJson["id"] = clip.id;
            clipJson["name"] = clip.name;
            clipJson["startBeat"] = clip.startBeat;
            clipJson["durationBeats"] = clip.durationBeats;
            clipJson["contentDurationBeats"] = clip.contentDurationBeats;
            clipJson["contentStretch"] = clip.contentStretch;
            clipJson["repeatContent"] = clip.repeatContent;
            clipJson["muted"] = clip.muted;
            clipJson["chance"] = clip.chance;
            clipJson["isLfo"] = clip.isLfo;
            clipJson["isStateCapture"] = clip.isStateCapture;
            clipJson["lfoOutputBindingId"] = clip.lfoOutputBindingId;
            clipJson["lfoOutputMin"] = clip.lfoOutputMin;
            clipJson["lfoOutputMax"] = clip.lfoOutputMax;
            clipJson["waveFilePath"] = clip.waveFilePath;
            clipJson["waveGain"] = clip.waveGain;
            clipJson["wavePlaybackRate"] = clip.wavePlaybackRate;
            clipJson["waveSourceStartBeat"] = clip.waveSourceStartBeat;
            clipJson["waveFileDurationBeats"] = clip.waveFileDurationBeats;
            clipJson["waveReverse"] = clip.waveReverse;
            clipJson["lanes"] = ofJson::array();
            for(const auto& lane : clip.lanes) {
                ofJson laneJson;
                laneJson["id"] = lane.id;
                laneJson["name"] = lane.name;
                laneJson["muted"] = lane.muted;
                laneJson["solo"] = lane.solo;
                if(lane.drivesTrackSolo) laneJson["drivesTrackSolo"] = true;
                if(!lane.targetProbability.empty()) {
                    laneJson["targetProbability"] = ofJson::object();
                    for(const auto& [path, probability] : lane.targetProbability)
                        laneJson["targetProbability"][path] = probability;
                }
                laneJson["laneType"] = laneTypeToString(lane.type);
                laneJson["bindingIds"] = lane.bindingIds;
                laneJson["stepCount"] = lane.stepCount;
                laneJson["beatsPerStep"] = lane.beatsPerStep;
                laneJson["valueMin"] = lane.valueMin;
                laneJson["valueMax"] = lane.valueMax;
                laneJson["probabilityEnabled"] = lane.probabilityEnabled;
                laneJson["probabilitySeed"] = lane.probabilitySeed;
                laneJson["behavior"] = lane.behavior;
                laneJson["pianoLowPitch"] = lane.pianoLowPitch;
                laneJson["pianoHighPitch"] = lane.pianoHighPitch;
                laneJson["pianoSnapToGrid"] = lane.pianoSnapToGrid;
                laneJson["pianoLiveEditSound"] = lane.pianoLiveEditSound;
                laneJson["pianoPitchBindingId"] = lane.pianoPitchBindingId;
                laneJson["pianoGateBindingId"] = lane.pianoGateBindingId;
                laneJson["pianoVelocityBindingId"] = lane.pianoVelocityBindingId;
                laneJson["pianoDefaultVelocity"] = lane.pianoDefaultVelocity;
                if(!lane.drumDurationBindingId.empty()) laneJson["drumDurationBindingId"] = lane.drumDurationBindingId;
                if(!lane.drumFilterBindingId.empty()) laneJson["drumFilterBindingId"] = lane.drumFilterBindingId;
                if(!lane.drumStutterBindingId.empty()) laneJson["drumStutterBindingId"] = lane.drumStutterBindingId;
                laneJson["pianoMonophonic"] = lane.pianoMonophonic;
                laneJson["mutation"] = mutationRulesToJson(lane.mutation);
                laneJson["mutationMonophonic"] = lane.mutationMonophonic;
                laneJson["mutationDrift"] = lane.mutationDrift;
                if(lane.mutationStrength < 1.0f) laneJson["mutationStrength"] = lane.mutationStrength;
                laneJson["pianoScale"] = {{"enabled", lane.pianoScaleEnabled}, {"mask", lane.pianoScaleMask},
                                          {"snap", lane.pianoScaleSnap}};
                laneJson["randomDeviation"] = {{"octave", lane.randomOctave}, {"octaveRange", lane.randomOctaveRange},
                                               {"octaveDirection", lane.randomOctaveDirection},
                                               {"pitch", lane.randomPitch}, {"pitchRange", lane.randomPitchRange},
                                               {"pitchDirection", lane.randomPitchDirection}};
                laneJson["mutationGroups"] = ofJson::array();
                for(const auto& group : lane.mutationGroups) {
                    auto groupJson = mutationRulesToJson(group.rules);
                    groupJson["overrides"] = group.overrides;
                    laneJson["mutationGroups"].push_back(groupJson);
                }
                laneJson["curveClamp"] = lane.curveClamp;
                laneJson["curveInterpolation"] = lane.curveInterpolation;
                laneJson["curveScale"] = lane.curveScale;
                laneJson["step"] = lane.step.toJson();
                laneJson["curvePoints"] = ofJson::array();
                for(const auto& point : lane.curvePoints) laneJson["curvePoints"].push_back({{"beat", point.beat}, {"value", point.value}});
                laneJson["curveTensions"] = ofJson::array();
                for(const auto& tension : lane.curveTensions) laneJson["curveTensions"].push_back({
                    {"inflection", tension.inflection}, {"steepness", tension.steepness}
                });
                laneJson["pianoNotes"] = ofJson::array();
                for(const auto& note : lane.pianoNotes) laneJson["pianoNotes"].push_back({
                    {"startBeat", note.startBeat}, {"durationBeats", note.durationBeats},
                    {"pitch", note.pitch}, {"velocity", note.velocity},
                    {"probability", note.probability}, {"mutation", note.mutation},
                    {"mutationGroup", note.mutationGroup}
                });
                laneJson["multiRowCount"] = lane.multiRowCount;
                laneJson["multiValueRows"] = ofJson::array();
                for(const auto& row : lane.multiValueRows) {
                    ofJson rowJson = ofJson::array();
                    for(const auto& region : row) rowJson.push_back({
                        {"startBeat", region.startBeat}, {"durationBeats", region.durationBeats},
                        {"value", region.value}
                    });
                    laneJson["multiValueRows"].push_back(std::move(rowJson));
                }
                laneJson["multiGateRows"] = ofJson::array();
                for(const auto& row : lane.multiGateRows) {
                    ofJson rowJson = ofJson::array();
                    for(const auto& region : row) rowJson.push_back({
                        {"startBeat", region.startBeat}, {"durationBeats", region.durationBeats},
                        {"velocity", region.velocity}
                    });
                    laneJson["multiGateRows"].push_back(std::move(rowJson));
                }
                laneJson["multiValueInteger"] = lane.multiValueInteger;
                laneJson["multiSliderValues"] = lane.multiSliderValues;
                laneJson["valueQuantizeSteps"] = lane.valueQuantizeSteps;
                laneJson["valueSnap"] = lane.valueSnap;
                laneJson["lfoParameter"] = lane.lfoParameter;
                laneJson["lfoValue"] = lane.lfoValue;
                laneJson["lfoSnap"] = lane.lfoSnap;
                laneJson["lfoSnapMode"] = lane.lfoSnapMode;
                // Only Drum Grid lanes carry it, so other lanes save as before.
                if(lane.type == ofxOceanodeTimelineLaneType::DrumGrid || !lane.drumRows.empty())
                    laneJson["drumGrid"] = drumGridToJson(lane);
                // The Wave lane and the per-clip volume lane are load-only
                // (see ofxOceanodeTimelineLane): audio and its envelope are
                // written at clip and track level instead.
                clipJson["lanes"].push_back(std::move(laneJson));
            }
            trackJson["clips"].push_back(std::move(clipJson));
        }

        json["tracks"].push_back(std::move(trackJson));
    }
    json["clipGroups"] = ofJson::array();
    for(const auto& group : clipGroups) {
        ofJson groupJson;
        groupJson["id"] = group.id;
        groupJson["members"] = ofJson::array();
        for(const auto& member : group.members)
            groupJson["members"].push_back({{"trackId", member.first}, {"clipId", member.second}});
        json["clipGroups"].push_back(std::move(groupJson));
    }
    return json;
}

namespace {
// A non-finite number is written to JSON as null, and reading a null where a
// number is expected throws -- which skipped the whole track holding it. Drop
// null members, so every field falls back to its default, and read null array
// entries as 0.
ofJson withoutNulls(const ofJson& json) {
    if(json.is_object()) {
        ofJson result = ofJson::object();
        for(auto it = json.begin(); it != json.end(); ++it)
            if(!it.value().is_null()) result[it.key()] = withoutNulls(it.value());
        return result;
    }
    if(json.is_array()) {
        ofJson result = ofJson::array();
        for(const auto& item : json) result.push_back(item.is_null() ? ofJson(0) : withoutNulls(item));
        return result;
    }
    return json;
}
}

void ofxOceanodeTimelineManager::fromJson(const ofJson& input) {
    // Track ids repeat between files: nothing played before may carry over.
    playback = PlaybackState();
    const ofJson json = withoutNulls(input);
    const int fileVersion = json.value("version", kPresetVersion);
    if(fileVersion > kPresetVersion) {
        ofLogWarning("ofxOceanodeTimeline")
            << "Timeline was written by a newer version (" << fileVersion << " > " << kPresetVersion
            << "); anything this build does not know about will be dropped on the next save.";
    }
    clear();
    if(!json.is_object()) return;
    timebase = json.value("timebase", std::string("Beats")) == "Time"
        ? ofxOceanodeTimelineTimebase::Time
        : ofxOceanodeTimelineTimebase::Beats;
    if(json.contains("view") && json["view"].is_object()) {
        const auto& view = json["view"];
        viewState.pixelsPerSecond = ofClamp(
            view.value("pixelsPerSecond", kDefaultTimelinePixelsPerSecond),
            kMinTimelinePixelsPerSecond, kMaxTimelinePixelsPerSecond);
        viewState.scrollX = std::max(0.0f, view.value("scrollX", 0.0f));
        viewState.labelWidth = ofClamp(view.value("labelWidth", 280.0f), 200.0f, 520.0f);
    }
    playOnPresetLoad = json.value("playOnPresetLoad", false);
    if(!json.contains("tracks") || !json["tracks"].is_array()) return;

    std::set<std::string> loadedTrackIds;
    std::set<std::string> loadedBindingIds;
    std::set<std::string> loadedClipIds;
    std::set<std::string> loadedLaneIds;
    std::set<std::string> loadedGroupIds;
    // clipGroups (parsed after the tracks loop below) references clips by
    // the track/clip id *as saved in the file*. uniqueLoadedId can remap an
    // id that collides with one already loaded (or that was empty), so the
    // original-id -> final-id mapping is recorded here as each track/clip
    // loads, and consulted when resolving a group's members afterwards.
    std::map<std::string, std::string> loadedTrackIdRemap;
    std::map<std::string, std::string> loadedClipIdRemap;
    auto uniqueLoadedId = [&](std::string candidate, const char* prefix,
                              uint64_t& counter, std::set<std::string>& usedIds) {
        if(!candidate.empty() && usedIds.insert(candidate).second) return candidate;
        do {
            candidate = makeId(prefix, counter++);
        } while(!usedIds.insert(candidate).second);
        return candidate;
    };

    if(json.contains("tempo") && json["tempo"].is_object()) {
        const auto& tempo = json["tempo"];
        bpmAutomationEnabled = tempo.value("enabled", false);
        bpmLaneCollapsed = tempo.value("collapsed", true);
        bpmLaneVisible = tempo.value("visible", false);
        setBpmRange(tempo.value("minimum", 20.0f), tempo.value("maximum", 300.0f));
        bpmInterpolation = tempo.value("interpolation", std::string("Linear"));
        if(bpmInterpolation != "Step" && bpmInterpolation != "Linear" &&
           bpmInterpolation != "Log / Exp" && bpmInterpolation != "Sigmoid") {
            bpmInterpolation = "Linear";
        }
        if(tempo.contains("points") && tempo["points"].is_array()) {
            for(const auto& point : tempo["points"]) {
                if(!point.is_object()) continue;
                bpmAutomationPoints.push_back({
                    std::max(0.0, point.value("beat", 0.0)),
                    ofClamp(point.value("bpm", 120.0f), bpmMinimum, bpmMaximum)
                });
            }
        }
        std::sort(bpmAutomationPoints.begin(), bpmAutomationPoints.end(), [](const auto& a, const auto& b) {
            return a.beat < b.beat;
        });
        if(tempo.contains("tensions") && tempo["tensions"].is_array()) {
            for(const auto& tensionJson : tempo["tensions"]) {
                if(!tensionJson.is_object()) continue;
                bpmCurveTensions.push_back({
                    ofClamp(tensionJson.value("inflection", 0.5f), 0.01f, 0.99f),
                    ofClamp(tensionJson.value("steepness", 1.0f), 0.1f, 10.0f)
                });
            }
        }
        bpmCurveTensions.resize(bpmAutomationPoints.empty() ? 0 : bpmAutomationPoints.size() - 1);
    }
    if(json.contains("loop") && json["loop"].is_object()) {
        const auto& loop = json["loop"];
        loopEnabled = loop.value("enabled", false);
        setLoopRange(loop.value("startBeat", 0.0), loop.value("endBeat", 4.0));
    }
    if(json.contains("markers") && json["markers"].is_array()) {
        for(const auto& markerJson : json["markers"]) {
            if(!markerJson.is_object()) continue;
            try {
                const std::string id = addMarker(markerJson.value("beat", 0.0), markerJson.value("name", std::string()));
                const std::string savedId = markerJson.value("id", std::string());
                if(!savedId.empty() && getMarker(savedId) == nullptr)
                    for(auto& marker : markers) if(marker.id == id) marker.id = savedId;
            } catch(const std::exception& e) {
                ofLogError("ofxOceanodeTimeline") << "Skipped a marker that could not be read: " << e.what();
            }
        }
    }
    if(json.contains("timeSignature") && json["timeSignature"].is_object()) {
        const auto& signature = json["timeSignature"];
        setTimeSignature(signature.value("numerator", 4), signature.value("denominator", 4));
    }
    {
        const auto it = json.find("masterGridBeats");
        masterGridBeats = it != json.end() && it->is_number() ? ofClamp(it->get<double>(), 0.0, 16.0) : 0.25;
    }
    if(json.contains("scheduling") && json["scheduling"].is_object()) {
        const auto& scheduling = json["scheduling"];
        setSchedulingEnabled(scheduling.value("enabled", true));
        setSchedulingLookaheadMs(scheduling.value("lookaheadMs", 120.0));
        setSchedulingLookaheadAdaptive(scheduling.value("adaptive", true));
    }
    // Every clip, lane and loop below is about to be replaced.
    invalidateSchedule();

    for(const auto& trackJson : json["tracks"]) {
        if(!trackJson.is_object()) continue;
        // One unreadable track (a field of the wrong type throws) is skipped
        // instead of losing the whole timeline.
        try {
        ofxOceanodeTimelineTrack track;
        // Saved binding id -> loaded binding id (ids can be renamed on a
        // collision, and a duplicate parameter maps onto the row kept).
        std::map<std::string, std::string> bindingRemap;
        auto mapBinding = [&](const std::string& savedId) {
            const auto it = bindingRemap.find(savedId);
            return it == bindingRemap.end() ? savedId : it->second;
        };
        const std::string originalTrackId = trackJson.value("id", std::string());
        track.id = uniqueLoadedId(originalTrackId, "timeline_track", nextTrackNumber, loadedTrackIds);
        if(!originalTrackId.empty()) loadedTrackIdRemap[originalTrackId] = track.id;
        track.name = makeUniqueTrackName(trackJson.value("name", std::string("Timeline Track")));
        track.collapsed = trackJson.value("collapsed", false);
        track.muted = trackJson.value("muted", false);
        track.solo = trackJson.value("solo", false);
        track.isWaveTrack = trackJson.value("isWaveTrack", false);
        track.waveVolume = ofClamp(trackJson.value("waveVolume", 1.0f), 0.0f, 4.0f);
        // Track automation is always present for Wave Tracks. The old flag is
        // still read for compatibility, but it must not make legacy presets
        // lose the default automation lane.
        track.waveVolumeAutomationEnabled = track.isWaveTrack ||
            trackJson.value("waveVolumeAutomationEnabled", false);
        track.latencyOffsetMs = ofClamp(trackJson.value("latencyOffsetMs", 0.0f), -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs);
        track.waveVolumeInterpolation = trackJson.value("waveVolumeInterpolation", std::string("Linear"));
        if(trackJson.contains("waveVolumePoints") && trackJson["waveVolumePoints"].is_array()) {
            for(const auto& pointJson : trackJson["waveVolumePoints"]) {
                if(!pointJson.is_object()) continue;
                track.waveVolumePoints.push_back({
                    std::max(0.0, pointJson.value("beat", 0.0)),
                    ofClamp(pointJson.value("value", 1.0f), 0.0f, 4.0f)
                });
            }
        }
        std::sort(track.waveVolumePoints.begin(), track.waveVolumePoints.end(),
                  [](const auto& a, const auto& b) { return a.beat < b.beat; });
        if(trackJson.contains("waveVolumeTensions") && trackJson["waveVolumeTensions"].is_array()) {
            for(const auto& tensionJson : trackJson["waveVolumeTensions"]) {
                if(!tensionJson.is_object()) continue;
                track.waveVolumeTensions.push_back({
                    ofClamp(tensionJson.value("inflection", 0.5f), 0.01f, 0.99f),
                    ofClamp(tensionJson.value("steepness", 1.0f), 0.1f, 10.0f)
                });
            }
        }
        track.waveVolumeTensions.resize(track.waveVolumePoints.size() > 1 ? track.waveVolumePoints.size() - 1 : 0);
        if(trackJson.contains("color") && trackJson["color"].is_object()) {
            const auto& colorJson = trackJson["color"];
            track.color = ofColor(
                static_cast<unsigned char>(ofClamp(colorJson.value("r", 65), 0, 255)),
                static_cast<unsigned char>(ofClamp(colorJson.value("g", 165), 0, 255)),
                static_cast<unsigned char>(ofClamp(colorJson.value("b", 245), 0, 255)),
                static_cast<unsigned char>(ofClamp(colorJson.value("a", 255), 0, 255))
            );
        }

        if(trackJson.contains("bindings") && trackJson["bindings"].is_array()) {
            for(const auto& bindingJson : trackJson["bindings"]) {
                if(!bindingJson.is_object()) continue;
                ofxOceanodeTimelineParameterBinding binding;
                const std::string savedBindingId = bindingJson.value("id", std::string());
                binding.parameterPath = bindingJson.value("parameterPath", std::string());
                if(binding.parameterPath.empty()) continue;
                const auto duplicate = std::find_if(track.bindings.begin(), track.bindings.end(), [&](const auto& existing) {
                    return existing.parameterPath == binding.parameterPath;
                });
                if(duplicate != track.bindings.end()) {
                    if(!savedBindingId.empty()) bindingRemap[savedBindingId] = duplicate->id;
                    continue;
                }
                binding.id = uniqueLoadedId(savedBindingId, "timeline_binding", nextBindingNumber, loadedBindingIds);
                if(!savedBindingId.empty()) bindingRemap[savedBindingId] = binding.id;
                binding.valueType = bindingJson.value("valueType", std::string());
                binding.defaultValue = bindingJson.value("defaultValue", std::string());
                binding.mode = modeFromString(bindingJson.value("mode", std::string("Replace")));
                binding.laneType = laneTypeFromString(bindingJson.value("laneType", std::string("Step")));
                binding.valueScale = bindingJson.value("valueScale", std::string());
                binding.bypass = bindingJson.value("bypass", false);
                binding.clampToParameterRange = bindingJson.value("clampToRange", true);
                binding.rowHeight = ofClamp(bindingJson.value("rowHeight", 28.0f), 20.0f, 300.0f);
                track.bindings.push_back(std::move(binding));
            }
        }

        if(trackJson.contains("clips") && trackJson["clips"].is_array()) {
            for(const auto& clipJson : trackJson["clips"]) {
                if(!clipJson.is_object()) continue;
                ofxOceanodeTimelineClip clip;
                const std::string originalClipId = clipJson.value("id", std::string());
                clip.id = uniqueLoadedId(originalClipId, "timeline_clip", nextClipNumber, loadedClipIds);
                if(!originalClipId.empty()) loadedClipIdRemap[originalClipId] = clip.id;
                clip.name = makeUniqueClipName(track, clipJson.value("name", std::string("Clip")));
                clip.startBeat = std::max(0.0, clipJson.value("startBeat", 0.0));
                clip.durationBeats = std::max(1.0 / 24.0, clipJson.value("durationBeats", 4.0));
                clip.contentDurationBeats = std::max(1.0 / 24.0, clipJson.value("contentDurationBeats", clip.durationBeats));
                clip.repeatContent = clipJson.value("repeatContent", true);
                clip.muted = clipJson.value("muted", false);
                clip.chance = ofClamp(clipJson.value("chance", 1.0f), 0.0f, 1.0f);
                clip.contentStretch = std::max(1.0 / 1024.0,
                    clipJson.value("contentStretch", clip.repeatContent
                        ? 1.0 : clip.durationBeats / clip.contentDurationBeats));
                clip.isLfo = clipJson.value("isLfo", false);
                clip.isStateCapture = clipJson.value("isStateCapture", false);
                clip.lfoOutputBindingId = mapBinding(clipJson.value("lfoOutputBindingId", std::string()));
                if(std::none_of(track.bindings.begin(), track.bindings.end(),
                                [&](const auto& b) { return b.id == clip.lfoOutputBindingId; }))
                    clip.lfoOutputBindingId.clear();
                clip.lfoOutputMin = clipJson.value("lfoOutputMin", 0.0f);
                clip.lfoOutputMax = clipJson.value("lfoOutputMax", 1.0f);
                clip.waveFilePath = clipJson.value("waveFilePath", std::string());
                clip.waveGain = ofClamp(clipJson.value("waveGain", 1.0f), 0.0f, 4.0f);
                clip.wavePlaybackRate = ofClamp(clipJson.value("wavePlaybackRate", 1.0f), 0.1f, 4.0f);
                clip.waveSourceStartBeat = std::max(0.0, clipJson.value("waveSourceStartBeat", 0.0));
                clip.waveFileDurationBeats = std::max(0.0, clipJson.value("waveFileDurationBeats", 0.0));
                clip.waveReverse = clipJson.value("waveReverse", false);
                if(clipJson.contains("lanes") && clipJson["lanes"].is_array()) {
                    for(const auto& laneJson : clipJson["lanes"]) {
                        if(!laneJson.is_object()) continue;
                        ofxOceanodeTimelineLane lane;
                        lane.id = uniqueLoadedId(laneJson.value("id", std::string()),
                                                 "timeline_lane", nextLaneNumber, loadedLaneIds);
                        lane.name = laneJson.value("name", std::string("Lane"));
                        lane.muted = laneJson.value("muted", false);
                        lane.solo = laneJson.value("solo", false);
                        lane.drivesTrackSolo = laneJson.value("drivesTrackSolo", false);
                        if(laneJson.contains("targetProbability") && laneJson["targetProbability"].is_object()) {
                            for(const auto& [path, probability] : laneJson["targetProbability"].items()) {
                                if(!probability.is_number()) continue;
                                const float chance = ofClamp(probability.get<float>(), 0.0f, 1.0f);
                                if(chance < 1.0f) lane.targetProbability[path] = chance;
                            }
                        }
                        lane.type = laneTypeFromString(laneJson.value("laneType", std::string("Step")));
                        lane.stepCount = std::max(1, laneJson.value("stepCount", 16));
                        lane.beatsPerStep = std::max(1.0 / 24.0, laneJson.value("beatsPerStep", 0.25));
                        lane.valueMin = laneJson.value("valueMin", 0.0f);
                        lane.valueMax = laneJson.value("valueMax", 1.0f);
                        lane.probabilityEnabled = laneJson.value("probabilityEnabled", true);
                        lane.probabilitySeed = laneJson.value("probabilitySeed", 0);
                        lane.behavior = laneJson.value("behavior", std::string("Probability"));
                        if(lane.behavior != "Probability" && lane.behavior != "Always" && lane.behavior != "Mute")
                            lane.behavior = "Probability";
                        lane.pianoLowPitch = ofClamp(laneJson.value("pianoLowPitch", 36), 0, 127);
                        lane.pianoHighPitch = ofClamp(laneJson.value("pianoHighPitch", 84), lane.pianoLowPitch, 127);
                        lane.pianoSnapToGrid = laneJson.value("pianoSnapToGrid", true);
                        lane.pianoLiveEditSound = laneJson.value("pianoLiveEditSound", false);
                        lane.pianoPitchBindingId = mapBinding(laneJson.value("pianoPitchBindingId", std::string()));
                        lane.pianoGateBindingId = mapBinding(laneJson.value("pianoGateBindingId", std::string()));
                        lane.pianoVelocityBindingId = mapBinding(laneJson.value("pianoVelocityBindingId", std::string()));
                        lane.drumDurationBindingId = mapBinding(laneJson.value("drumDurationBindingId", std::string()));
                        lane.drumFilterBindingId = mapBinding(laneJson.value("drumFilterBindingId", std::string()));
                        lane.drumStutterBindingId = mapBinding(laneJson.value("drumStutterBindingId", std::string()));
                        lane.pianoDefaultVelocity = ofClamp(laneJson.value("pianoDefaultVelocity", 0.8f), 0.0f, 1.0f);
                        lane.pianoMonophonic = laneJson.value("pianoMonophonic", false);
                        if(laneJson.contains("mutation") && laneJson["mutation"].is_object())
                            lane.mutation = mutationRulesFromJson(laneJson["mutation"]);
                        lane.mutationMonophonic = laneJson.value("mutationMonophonic", false);
                        {
                            const auto it = laneJson.find("mutationStrength");
                            lane.mutationStrength = it != laneJson.end() && it->is_number() ? ofClamp(it->get<float>(), 0.0f, 1.0f) : 1.0f;
                        }
                        // Drift was a checkbox before it was an amount.
                        if(laneJson.contains("mutationDrift")) {
                            const auto& drift = laneJson["mutationDrift"];
                            lane.mutationDrift = drift.is_boolean() ? (drift.get<bool>() ? 1.0f : 0.0f)
                                : drift.is_number() ? ofClamp(drift.get<float>(), 0.0f, 1.0f) : 0.0f;
                        }
                        if(laneJson.contains("pianoScale") && laneJson["pianoScale"].is_object()) {
                            const auto& scale = laneJson["pianoScale"];
                            lane.pianoScaleEnabled = scale.value("enabled", false);
                            lane.pianoScaleMask = scale.value("mask", 0xAB5) & 0xFFF;
                            lane.pianoScaleSnap = scale.value("snap", false);
                        }
                        // Older presets have none: no deviation.
                        if(laneJson.contains("randomDeviation") && laneJson["randomDeviation"].is_object()) {
                            const auto& random = laneJson["randomDeviation"];
                            lane.randomOctave = ofClamp(random.value("octave", 0.0f), 0.0f, 1.0f);
                            lane.randomOctaveRange = std::clamp(random.value("octaveRange", 1), 1, 4);
                            lane.randomOctaveDirection = std::clamp(random.value("octaveDirection", 0), 0, 2);
                            lane.randomPitch = ofClamp(random.value("pitch", 0.0f), 0.0f, 1.0f);
                            lane.randomPitchRange = std::clamp(random.value("pitchRange", 2), 1, 24);
                            lane.randomPitchDirection = std::clamp(random.value("pitchDirection", 0), 0, 2);
                        }
                        if(laneJson.contains("mutationGroups") && laneJson["mutationGroups"].is_array()) {
                            for(const auto& groupJson : laneJson["mutationGroups"]) {
                                if(!groupJson.is_object() || lane.mutationGroups.size() + 1 >= kMaxMutationGroups) continue;
                                lane.mutationGroups.push_back({mutationRulesFromJson(groupJson),
                                                               groupJson.value("overrides", 0) & 0x7FFF});
                            }
                        }

                        lane.curveClamp = laneJson.value("curveClamp", true);
                        lane.isWaveVolume = laneJson.value("isWaveVolume", false);
                        lane.lfoParameter = laneJson.value("lfoParameter", std::string());
                        lane.lfoValue = ofClamp(laneJson.value("lfoValue", 0.5f), 0.0f, 1.0f);
                        lane.lfoSnap = laneJson.value("lfoSnap", false);
                        lane.lfoSnapMode = std::max(0, std::min(2, laneJson.value("lfoSnapMode", 0)));
                        lane.curveInterpolation = laneJson.value("curveInterpolation", std::string("Linear"));
                        lane.curveScale = laneJson.value("curveScale", std::string());
                        const bool lfoValueMode = !lane.lfoParameter.empty() && lane.curveInterpolation == "Value";
                        if(!lfoValueMode && lane.curveInterpolation != "Step" && lane.curveInterpolation != "Linear" &&
                           lane.curveInterpolation != "Log / Exp" && lane.curveInterpolation != "Sigmoid") {
                            lane.curveInterpolation = "Linear";
                        }
                        if(laneJson.contains("bindingIds") && laneJson["bindingIds"].is_array()) {
                            for(const auto& bindingId : laneJson["bindingIds"]) {
                                if(!bindingId.is_string()) continue;
                                const std::string id = mapBinding(bindingId.get<std::string>());
                                const bool belongsToTrack = std::any_of(track.bindings.begin(), track.bindings.end(),
                                    [&](const auto& binding) { return binding.id == id; });
                                if(belongsToTrack && std::find(lane.bindingIds.begin(), lane.bindingIds.end(), id) == lane.bindingIds.end())
                                    lane.bindingIds.push_back(id);
                            }
                        }
                        auto validateRole = [&](std::string& roleId) {
                            const bool belongsToTrack = std::any_of(track.bindings.begin(), track.bindings.end(),
                                [&](const auto& binding) { return binding.id == roleId; });
                            if(!belongsToTrack) roleId.clear();
                            else if(std::find(lane.bindingIds.begin(), lane.bindingIds.end(), roleId) == lane.bindingIds.end())
                                lane.bindingIds.push_back(roleId);
                        };
                        validateRole(lane.pianoPitchBindingId);
                        validateRole(lane.pianoGateBindingId);
                        validateRole(lane.pianoVelocityBindingId);
                        validateRole(lane.drumDurationBindingId);
                        if(lane.drumDurationBindingId == lane.pianoPitchBindingId || lane.drumDurationBindingId == lane.pianoGateBindingId ||
                           lane.drumDurationBindingId == lane.pianoVelocityBindingId)
                            lane.drumDurationBindingId.clear();
                        validateRole(lane.drumFilterBindingId);
                        if(lane.drumFilterBindingId == lane.pianoPitchBindingId || lane.drumFilterBindingId == lane.pianoGateBindingId ||
                           lane.drumFilterBindingId == lane.pianoVelocityBindingId || lane.drumFilterBindingId == lane.drumDurationBindingId)
                            lane.drumFilterBindingId.clear();
                        validateRole(lane.drumStutterBindingId);
                        if(lane.drumStutterBindingId == lane.pianoPitchBindingId || lane.drumStutterBindingId == lane.pianoGateBindingId ||
                           lane.drumStutterBindingId == lane.pianoVelocityBindingId || lane.drumStutterBindingId == lane.drumDurationBindingId ||
                           lane.drumStutterBindingId == lane.drumFilterBindingId)
                            lane.drumStutterBindingId.clear();
                        if(lane.pianoGateBindingId == lane.pianoPitchBindingId)
                            lane.pianoGateBindingId.clear();
                        if(lane.pianoVelocityBindingId == lane.pianoPitchBindingId ||
                           lane.pianoVelocityBindingId == lane.pianoGateBindingId)
                            lane.pianoVelocityBindingId.clear();
                        if(laneJson.contains("step") && laneJson["step"].is_object()) lane.step.fromJson(laneJson["step"]);
                        if(laneJson.contains("curvePoints") && laneJson["curvePoints"].is_array()) {
                            for(const auto& pointJson : laneJson["curvePoints"]) {
                                if(!pointJson.is_object()) continue;
                                lane.curvePoints.push_back({
                                    pointJson.value("beat", 0.0),
                                    pointJson.value("value", 0.0f)
                                });
                            }
                        }
                        std::sort(lane.curvePoints.begin(), lane.curvePoints.end(), [](const auto& a, const auto& b) {
                            return a.beat < b.beat;
                        });
                        if(laneJson.contains("curveTensions") && laneJson["curveTensions"].is_array()) {
                            for(const auto& tensionJson : laneJson["curveTensions"]) {
                                if(!tensionJson.is_object()) continue;
                                lane.curveTensions.push_back({
                                    ofClamp(tensionJson.value("inflection", 0.5f), 0.01f, 0.99f),
                                    ofClamp(tensionJson.value("steepness", 1.0f), 0.1f, 10.0f)
                                });
                            }
                        }
                        lane.curveTensions.resize(lane.curvePoints.empty() ? 0 : lane.curvePoints.size() - 1);
                        if(ofxOceanodeTimelineIsHeldValueLane(lane.type)) lane.curveInterpolation = "Step";
                        if(laneJson.contains("pianoNotes") && laneJson["pianoNotes"].is_array()) {
                            for(const auto& noteJson : laneJson["pianoNotes"]) {
                                if(!noteJson.is_object()) continue;
                                lane.pianoNotes.push_back({
                                    std::max(0.0, noteJson.value("startBeat", 0.0)),
                                    std::max(1.0 / 24.0, noteJson.value("durationBeats", 0.25)),
                                    static_cast<int>(ofClamp(noteJson.value("pitch", 60), 0, 127)),
                                    ofClamp(noteJson.value("velocity", 1.0f), 0.0f, 1.0f),
                                    ofClamp(noteJson.value("probability", 1.0f), 0.0f, 1.0f),
                                    ofClamp(noteJson.value("mutation", 0.0f), 0.0f, 1.0f),
                                    std::clamp(noteJson.value("mutationGroup", 0), 0, kMaxMutationGroups - 1)
                                });
                            }
                            // A note of a group that didn't load follows A.
                            for(auto& note : lane.pianoNotes)
                                if(note.mutationGroup > static_cast<int>(lane.mutationGroups.size())) note.mutationGroup = 0;
                        }
                        lane.multiRowCount = ofClamp(laneJson.value("multiRowCount", 4), 1, 16);
                        if(laneJson.contains("multiValueRows") && laneJson["multiValueRows"].is_array()) {
                            for(const auto& rowJson : laneJson["multiValueRows"]) {
                                std::vector<ofxOceanodeTimelineValueRegion> row;
                                if(rowJson.is_array()) {
                                    for(const auto& regionJson : rowJson) {
                                        if(!regionJson.is_object()) continue;
                                        row.push_back({
                                            std::max(0.0, regionJson.value("startBeat", 0.0)),
                                            std::max(1.0 / 24.0, regionJson.value("durationBeats", 1.0)),
                                            regionJson.value("value", 0.0f)
                                        });
                                    }
                                }
                                lane.multiValueRows.push_back(std::move(row));
                            }
                        }
                        if(laneJson.contains("multiGateRows") && laneJson["multiGateRows"].is_array()) {
                            for(const auto& rowJson : laneJson["multiGateRows"]) {
                                std::vector<ofxOceanodeTimelineGateRegion> row;
                                if(rowJson.is_array()) {
                                    for(const auto& regionJson : rowJson) {
                                        if(!regionJson.is_object()) continue;
                                        row.push_back({
                                            std::max(0.0, regionJson.value("startBeat", 0.0)),
                                            std::max(1.0 / 24.0, regionJson.value("durationBeats", 1.0)),
                                            ofClamp(regionJson.value("velocity", 1.0f), 0.0f, 1.0f)
                                        });
                                    }
                                }
                                lane.multiGateRows.push_back(std::move(row));
                            }
                        }
                        if(laneJson.contains("drumGrid") && laneJson["drumGrid"].is_object())
                            drumGridFromJson(laneJson["drumGrid"], lane);
                        lane.multiValueInteger = laneJson.value("multiValueInteger", false);
                        if(laneJson.contains("multiSliderValues") && laneJson["multiSliderValues"].is_array()) {
                            // Real values in the lane's own valueMin/valueMax
                            // units, not normalized 0..1 -- unlike curve
                            // points, so no 0..1 clamp here (an earlier
                            // version of this loader incorrectly clamped to
                            // 0..1, corrupting any lane whose range didn't
                            // happen to already be 0..1).
                            for(const auto& v : laneJson["multiSliderValues"])
                                lane.multiSliderValues.push_back(v.get<float>());
                        }
                        lane.valueQuantizeSteps = std::max(0, laneJson.value("valueQuantizeSteps", 0));
                        // Before the toggle existed, a step count of 2+ meant snapping was on.
                        lane.valueSnap = laneJson.value("valueSnap", lane.valueQuantizeSteps >= 2);
                        lane.waveFilePath = laneJson.value("waveFilePath", std::string());
                        lane.waveGain = ofClamp(laneJson.value("waveGain", 1.0f), 0.0f, 4.0f);
                        if(lane.isWaveVolume) {
                            // Migrate the first legacy per-clip volume lane
                            // into the new track-wide automation domain.
                            track.isWaveTrack = true;
                            if(track.waveVolumePoints.empty()) {
                                track.waveVolumeAutomationEnabled = true;
                                for(const auto& point : lane.curvePoints) {
                                    const float value = ofClamp(lane.valueMin +
                                        point.value * (lane.valueMax - lane.valueMin), 0.0f, 4.0f);
                                    track.waveVolumePoints.push_back({
                                        ofxOceanodeTimelineClipTime::sourceToTimelineBeat(clip, point.beat, 0),
                                        value
                                    });
                                }
                            }
                            continue;
                        }
                        if(lane.type == ofxOceanodeTimelineLaneType::Wave) {
                            // Migrate the old representation in which each
                            // audio clip contained a Wave lane.
                            track.isWaveTrack = true;
                            if(clip.waveFilePath.empty()) clip.waveFilePath = lane.waveFilePath;
                            clip.waveGain = lane.waveGain;
                            continue;
                        }
                        clip.lanes.push_back(std::move(lane));
                    }
                }
                track.clips.push_back(std::move(clip));
            }
        }

        if(track.waveVolumePoints.size() > 1) {
            std::sort(track.waveVolumePoints.begin(), track.waveVolumePoints.end(),
                      [](const auto& a, const auto& b) { return a.beat < b.beat; });
            track.waveVolumeTensions.resize(track.waveVolumePoints.size() - 1);
        }
        // Wave clips are independent audio slices, not repeating pattern
        // clips. Older presets could inherit the generic timeline's repeat
        // flag after an edge drag, which cropped a slice when its visible
        // duration was shorter than its source content. Preserve the visible
        // duration but map the complete source over it instead.
        // The stored contentStretch is also recomputed for clips that were
        // already non-repeating: a stale value (e.g. 1.0 saved next to a
        // shorter duration) made the audio rate disagree with the position.
        if(track.isWaveTrack) {
            for(auto& clip : track.clips) normalizeWaveClipMapping(clip);
        }
        // A shared capture has one destination; after "Separate parameter
        // clips" there is intentionally one destination per parameter.
        // Wave/LFO clips can never participate in either form.
        for(auto& clip : track.clips) {
            if(track.isWaveTrack || clip.isLfo) clip.isStateCapture = false;
        }
        if(track.isWaveTrack && track.waveVolumePoints.empty()) {
            track.waveVolumePoints = {{0.0, track.waveVolume}, {4.0, track.waveVolume}};
            track.waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
        }

        if(trackJson.contains("noteGroups") && trackJson["noteGroups"].is_array()) {
            for(const auto& groupJson : trackJson["noteGroups"]) {
                if(!groupJson.is_object()) continue;
                ofxOceanodeTimelineNoteGroup group;
                group.id = groupJson.value("id", std::string());
                if(group.id.empty() || std::any_of(track.noteGroups.begin(), track.noteGroups.end(),
                                                   [&](const auto& g) { return g.id == group.id; })) continue;
                group.name = groupJson.value("name", std::string("Notes"));
                group.pitchBindingId = mapBinding(groupJson.value("pitch", std::string()));
                group.gateBindingId = mapBinding(groupJson.value("gate", std::string()));
                group.velocityBindingId = mapBinding(groupJson.value("velocity", std::string()));
                {
                    const std::string savedDuration = groupJson.value("duration", std::string());
                    group.durationBindingId = mapBinding(savedDuration);
                    group.durationDecided = groupJson.value("durationDecided", !savedDuration.empty());
                    const std::string savedFilter = groupJson.value("filter", std::string());
                    group.filterBindingId = mapBinding(savedFilter);
                    group.filterDecided = groupJson.value("filterDecided", !savedFilter.empty());
                    const std::string savedStutter = groupJson.value("stutter", std::string());
                    group.stutterBindingId = mapBinding(savedStutter);
                    group.stutterDecided = groupJson.value("stutterDecided", !savedStutter.empty());
                }
                group.expanded = groupJson.value("expanded", false);
                group.rowHeight = ofClamp(groupJson.value("rowHeight", 48.0f), 20.0f, 300.0f);
                group.voices = ofClamp(groupJson.value("voices", 0), 0, 128);
                // A binding belongs to one group only.
                for(auto* role : {&group.pitchBindingId, &group.gateBindingId, &group.velocityBindingId, &group.durationBindingId,
                                  &group.filterBindingId, &group.stutterBindingId})
                    if(findNoteGroupForBinding(track, *role) != nullptr) role->clear();
                track.noteGroups.push_back(group);
            }
            normalizeNoteGroups(track);
        }

        tracks.push_back(std::move(track));
        } catch(const std::exception& e) {
            ofLogError("ofxOceanodeTimeline") << "Skipped a timeline track that could not be read: " << e.what();
        }
    }

    // Continue numbering after the highest id in use, so an id is never
    // handed out again while something may still remember it.
    auto nextAfter = [](const std::string& id, uint64_t current) {
        const auto underscore = id.find_last_of('_');
        if(underscore == std::string::npos || underscore + 1 >= id.size()) return current;
        const std::string digits = id.substr(underscore + 1);
        if(!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return current;
        if(digits.size() > 18) return current;
        return std::max<uint64_t>(current, std::stoull(digits) + 1);
    };
    nextTrackNumber = tracks.size() + 1;
    nextBindingNumber = 1;
    nextClipNumber = 1;
    nextLaneNumber = 1;
    for(const auto& track : tracks) {
        nextTrackNumber = nextAfter(track.id, nextTrackNumber);
        for(const auto& binding : track.bindings) nextBindingNumber = nextAfter(binding.id, nextBindingNumber);
        for(const auto& clip : track.clips) {
            nextClipNumber = nextAfter(clip.id, nextClipNumber);
            for(const auto& lane : clip.lanes) nextLaneNumber = nextAfter(lane.id, nextLaneNumber);
        }
    }

    // Rebuild waveform caches after loading, once ids and clip storage are
    // final. Legacy Wave lanes were migrated to their containing clip above.
    for(auto& track : tracks) {
        for(auto& clip : track.clips) {
            if(track.isWaveTrack && !clip.waveFilePath.empty()) reloadWaveform(track.id, clip.id);
        }
    }

    if(json.contains("clipGroups") && json["clipGroups"].is_array()) {
        for(const auto& groupJson : json["clipGroups"]) {
            if(!groupJson.is_object()) continue;
            ofxOceanodeTimelineClipGroup group;
            group.id = uniqueLoadedId(groupJson.value("id", std::string()),
                                      "timeline_group", nextGroupNumber, loadedGroupIds);
            if(groupJson.contains("members") && groupJson["members"].is_array()) {
                for(const auto& memberJson : groupJson["members"]) {
                    if(!memberJson.is_object()) continue;
                    const std::string rawTrackId = memberJson.value("trackId", std::string());
                    const std::string rawClipId = memberJson.value("clipId", std::string());
                    const auto trackRemapIt = loadedTrackIdRemap.find(rawTrackId);
                    const auto clipRemapIt = loadedClipIdRemap.find(rawClipId);
                    const std::string resolvedTrackId = trackRemapIt != loadedTrackIdRemap.end() ? trackRemapIt->second : rawTrackId;
                    const std::string resolvedClipId = clipRemapIt != loadedClipIdRemap.end() ? clipRemapIt->second : rawClipId;
                    // Drop a member whose clip didn't survive the load above
                    // (a hand-edited file, or a clip that no longer exists)
                    // rather than keep a dangling reference around.
                    if(getClip(resolvedTrackId, resolvedClipId) != nullptr)
                        group.members.emplace_back(resolvedTrackId, resolvedClipId);
                }
            }
            if(group.members.size() >= 2) clipGroups.push_back(std::move(group));
        }
    }
    nextGroupNumber = clipGroups.size() + 1;
    for(const auto& group : clipGroups) nextGroupNumber = nextAfter(group.id, nextGroupNumber);

    // Give loaded LFO clips any control lane they are missing (collect ids first:
    // adding lanes must not happen while iterating the clip containers).
    std::vector<std::pair<std::string, std::string>> lfoClipIds;
    for(const auto& track : tracks) {
        for(const auto& clip : track.clips) {
            if(clip.isLfo) lfoClipIds.emplace_back(track.id, clip.id);
        }
    }
    for(const auto& ids : lfoClipIds) ensureLfoLanes(ids.first, ids.second);
}

namespace {
void clipFileError(std::string* error, const std::string& message) {
    if(error) *error = message;
}
bool validClipDocument(const ofJson& document) {
    if(!document.is_object() || document.value("format", std::string()) != "ofxOceanodeTimelineClip" ||
       document.value("version", 0) != 1 || !document.contains("track") || !document["track"].is_object()) return false;
    const auto& track = document["track"];
    if(!track.contains("clips") || !track["clips"].is_array() || track["clips"].size() != 1 ||
       !track["clips"][0].is_object() || !track.contains("bindings") || !track["bindings"].is_array()) return false;
    std::set<std::string> ids;
    for(const auto& binding : track["bindings"]) {
        if(!binding.is_object()) return false;
        const std::string id = binding.value("id", std::string());
        if(id.empty() || !ids.insert(id).second) return false;
    }
    return true;
}
}

ofJson ofxOceanodeTimelineManager::exportClip(const std::string& trackId, const std::string& clipId) const {
    const auto* track = getTrack(trackId);
    const auto* clip = getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr) return ofJson();
    ofxOceanodeTimelineTrack portable;
    portable.id = "clip_track";
    portable.name = track->name;
    portable.color = track->color;
    portable.isWaveTrack = track->isWaveTrack;
    portable.clips.push_back(*clip);
    portable.clips.front().startBeat = 0.0;
    // A reusable capture must not become the destination track's live capture buffer.
    portable.clips.front().isStateCapture = false;
    std::set<std::string> used{clip->lfoOutputBindingId};
    for(const auto& lane : clip->lanes) {
        used.insert(lane.bindingIds.begin(), lane.bindingIds.end());
        used.insert(lane.pianoPitchBindingId);
        used.insert(lane.pianoGateBindingId);
        used.insert(lane.pianoVelocityBindingId);
        used.insert(lane.drumDurationBindingId);
        used.insert(lane.drumFilterBindingId);
        used.insert(lane.drumStutterBindingId);
    }
    for(const auto& binding : track->bindings)
        if(used.count(binding.id)) portable.bindings.push_back(binding);
    for(auto group : track->noteGroups) {
        if(!used.count(group.pitchBindingId)) group.pitchBindingId.clear();
        if(!used.count(group.gateBindingId)) group.gateBindingId.clear();
        if(!used.count(group.velocityBindingId)) group.velocityBindingId.clear();
        if(!used.count(group.durationBindingId)) group.durationBindingId.clear();
        if(!used.count(group.filterBindingId)) group.filterBindingId.clear();
        if(!used.count(group.stutterBindingId)) group.stutterBindingId.clear();
        if(!group.members().empty()) portable.noteGroups.push_back(std::move(group));
    }
    // Use the preset codec so every lane type and future lane field stays in sync.
    ofxOceanodeTimelineManager isolated;
    isolated.tracks.push_back(std::move(portable));
    return {{"format", "ofxOceanodeTimelineClip"}, {"version", 1},
            {"timelineVersion", kPresetVersion}, {"track", isolated.toJson()["tracks"][0]}};
}

bool ofxOceanodeTimelineManager::saveClipFile(const std::string& trackId, const std::string& clipId,
    const std::string& path, std::string* error) const {
    clipFileError(error, "");
    std::filesystem::path copiedMedia;
    try {
        auto document = exportClip(trackId, clipId);
        if(document.is_null() || path.empty()) {
            clipFileError(error, "The clip or save path is no longer available."); return false;
        }
        const auto destination = std::filesystem::absolute(path);
        auto& audio = document["track"]["clips"][0]["waveFilePath"];
        if(audio.is_string() && !audio.get<std::string>().empty()) {
            const auto source = std::filesystem::path(ofToDataPath(audio.get<std::string>(), true));
            const auto folder = destination.parent_path() / (destination.filename().string() + ".media");
            std::filesystem::create_directories(folder);
            copiedMedia = folder / (ofToString(ofGetSystemTimeMicros()) + "-" + source.filename().string());
            std::filesystem::copy_file(source, copiedMedia);
            audio = std::filesystem::relative(copiedMedia, destination.parent_path()).generic_string();
        }
        if(!oceanodePresetSave::saveJson(destination.string(), document)) {
            if(!copiedMedia.empty()) std::filesystem::remove(copiedMedia);
            clipFileError(error, "Could not write the clip file."); return false;
        }
        return true;
    } catch(const std::exception& e) {
        if(!copiedMedia.empty()) { std::error_code ignored; std::filesystem::remove(copiedMedia, ignored); }
        clipFileError(error, e.what()); return false;
    }
}

ofJson ofxOceanodeTimelineManager::readClipFile(const std::string& path, std::string* error) {
    clipFileError(error, "");
    try {
        auto document = ofLoadJson(path);
        if(!validClipDocument(document) || document.value("timelineVersion", kPresetVersion) > kPresetVersion) {
            clipFileError(error, "This is not a supported Oceanode clip file."); return ofJson();
        }
        auto encoded = document["track"];
        const auto audioPath = encoded["clips"][0].value("waveFilePath", std::string());
        encoded["clips"][0]["waveFilePath"] = "";
        ofxOceanodeTimelineManager isolated;
        isolated.fromJson({{"version", kPresetVersion}, {"tracks", ofJson::array({encoded})}});
        if(isolated.tracks.size() != 1 || isolated.tracks.front().clips.size() != 1) {
            clipFileError(error, "The clip contents could not be read."); return ofJson();
        }
        document = isolated.exportClip(isolated.tracks.front().id, isolated.tracks.front().clips.front().id);
        if(!audioPath.empty()) {
            std::filesystem::path media(audioPath);
            if(media.is_relative()) media = std::filesystem::absolute(path).parent_path() / media;
            document["track"]["clips"][0]["waveFilePath"] = media.lexically_normal().string();
        }
        return document;
    } catch(const std::exception& e) {
        clipFileError(error, e.what()); return ofJson();
    }
}

std::pair<std::string, std::string> ofxOceanodeTimelineManager::importClip(const ofJson& document,
    const std::string& trackId, double startBeat, const std::map<std::string, std::string>& targetPaths,
    std::string* error) {
    clipFileError(error, "");
    try {
        if(!validClipDocument(document) || document.value("timelineVersion", kPresetVersion) > kPresetVersion ||
           !std::isfinite(startBeat)) {
            clipFileError(error, "This is not a supported Oceanode clip file."); return {};
        }
        // Decode before touching the destination. No project targets or transport
        // are attached to this manager, and waveform IO is deferred until insertion.
        auto encodedTrack = document["track"];
        const auto audioPath = encodedTrack["clips"][0].value("waveFilePath", std::string());
        encodedTrack["clips"][0]["waveFilePath"] = "";
        ofxOceanodeTimelineManager isolated;
        isolated.fromJson({{"version", kPresetVersion}, {"tracks", ofJson::array({encodedTrack})}});
        if(isolated.tracks.size() != 1 || isolated.tracks.front().clips.size() != 1) {
            clipFileError(error, "The clip contents could not be read."); return {};
        }
        auto source = std::move(isolated.tracks.front());
        auto copy = std::move(source.clips.front());
        copy.waveFilePath = audioPath;
        copy.isStateCapture = false;
        auto* destination = trackId.empty() ? nullptr : getTrack(trackId);
        if(!trackId.empty() && (destination == nullptr || destination->isWaveTrack != source.isWaveTrack)) {
            clipFileError(error, "Choose an audio track for audio clips, or an automation track for other clips."); return {};
        }
        // Explicit mappings only: never connect an imported clip merely because
        // a different project happens to reuse the original parameter names.
        for(const auto& binding : source.bindings) {
            const auto mapping = targetPaths.find(binding.id);
            if(mapping == targetPaths.end() || mapping->second.empty()) continue;
            auto* parameter = container == nullptr ? nullptr : container->findTimelineParameter(mapping->second);
            if(parameter == nullptr || !isStepLaneCompatible(*parameter) ||
               (parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly)) {
                clipFileError(error, "A chosen target is unavailable or cannot be automated."); return {};
            }
        }
        // A piano role must retain its own destination; reject ambiguous remaps.
        for(const auto& lane : copy.lanes) {
            std::set<std::string> roles;
            for(const auto* role : {&lane.pianoPitchBindingId, &lane.pianoGateBindingId, &lane.pianoVelocityBindingId,
                                    &lane.drumDurationBindingId, &lane.drumFilterBindingId, &lane.drumStutterBindingId}) {
                const auto mapping = targetPaths.find(*role);
                if(mapping != targetPaths.end() && !mapping->second.empty() && !roles.insert(mapping->second).second) {
                    clipFileError(error, "Choose different parameters for pitch, gate and velocity."); return {};
                }
            }
        }
        const std::string destinationId = destination ? trackId : (source.isWaveTrack
            ? createWaveTrack(source.name) : createTrack(source.name));
        destination = getTrack(destinationId);
        if(trackId.empty()) destination->color = source.color;
        std::map<std::string, std::string> remap;
        std::set<std::string> newBindingIds;
        for(const auto& binding : source.bindings) {
            const auto mapping = targetPaths.find(binding.id);
            if(mapping == targetPaths.end() || mapping->second.empty()) continue;
            std::string id;
            for(const auto& existing : destination->bindings)
                if(existing.parameterPath == mapping->second) id = existing.id;
            if(id.empty()) {
                auto* parameter = container->findTimelineParameter(mapping->second);
                id = addBinding(destinationId, *parameter, binding.mode);
                newBindingIds.insert(id);
                if(auto* added = getBinding(destinationId, id)) {
                    added->clampToParameterRange = binding.clampToParameterRange;
                    added->bypass = binding.bypass;
                }
            }
            remap[binding.id] = id;
        }
        auto mapped = [&](const std::string& id) {
            const auto it = remap.find(id); return it == remap.end() ? std::string() : it->second;
        };
        copy.lfoOutputBindingId = mapped(copy.lfoOutputBindingId);
        for(auto& lane : copy.lanes) {
            std::vector<std::string> ids;
            for(const auto& old : lane.bindingIds) {
                const auto id = mapped(old);
                if(!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
            lane.bindingIds = std::move(ids);
            lane.pianoPitchBindingId = mapped(lane.pianoPitchBindingId);
            lane.pianoGateBindingId = mapped(lane.pianoGateBindingId);
            lane.pianoVelocityBindingId = mapped(lane.pianoVelocityBindingId);
            lane.drumDurationBindingId = mapped(lane.drumDurationBindingId);
            lane.drumFilterBindingId = mapped(lane.drumFilterBindingId);
            lane.drumStutterBindingId = mapped(lane.drumStutterBindingId);
        }
        // Preserve existing note groups and their lanes. New groups are added
        // directly so importing cannot retarget piano lanes already on the track.
        for(const auto& group : source.noteGroups) {
            const auto pitch = mapped(group.pitchBindingId), gate = mapped(group.gateBindingId), velocity = mapped(group.velocityBindingId);
            const auto duration = mapped(group.durationBindingId), filter = mapped(group.filterBindingId);
            const auto stutter = mapped(group.stutterBindingId);
            if(pitch.empty() && gate.empty() && velocity.empty()) continue;
            bool occupied = false;
            for(const auto& id : {pitch, gate, velocity, duration, filter, stutter})
                if(!id.empty() && (!newBindingIds.count(id) || findNoteGroupForBinding(*destination, id))) occupied = true;
            if(!occupied) {
                auto added = group;
                size_t number = 1;
                do { added.id = "timeline_notes_" + ofToString(number++); }
                while(getNoteGroup(destinationId, added.id) != nullptr);
                added.pitchBindingId = pitch;
                added.gateBindingId = gate;
                added.velocityBindingId = velocity;
                added.durationBindingId = duration;
                added.filterBindingId = filter;
                added.stutterBindingId = stutter;
                destination->noteGroups.push_back(std::move(added));
            }
        }
        const auto id = createClip(destinationId, copy.name, std::max(0.0, startBeat), copy.durationBeats);
        copy.id = id;
        copy.name = getClip(destinationId, id)->name;
        copy.startBeat = std::max(0.0, startBeat);
        auto lanes = std::move(copy.lanes);
        copy.lanes.clear();
        *getClip(destinationId, id) = std::move(copy);
        for(auto& lane : lanes) {
            lane.id = makeUniqueLaneId(); ++nextLaneNumber;
            getClip(destinationId, id)->lanes.push_back(std::move(lane));
        }
        if(source.isWaveTrack && !audioPath.empty()) reloadWaveform(destinationId, id);
        invalidateEvaluationIndexes();
        invalidateSchedule();
        return {destinationId, id};
    } catch(const std::exception& e) {
        clipFileError(error, e.what()); return {};
    }
}

bool ofxOceanodeTimelineManager::savePreset(const std::string& presetFolderPath) const {
    const std::string path = presetFolderPath + "/timeline.json";
    if(!failedLoadPath.empty() && failedLoadPath == path && ofFile::doesFileExist(path)) {
        // This file could not be read when it was loaded: keep a copy before
        // overwriting it, so nothing is lost for good.
        ofFile::copyFromTo(path, presetFolderPath + "/timeline.unreadable.json", false, true);
        failedLoadPath.clear();
    }
    return oceanodePresetSave::saveJson(path, toJson());
}

bool ofxOceanodeTimelineManager::loadPreset(const std::string& presetFolderPath) {
    const std::string path = presetFolderPath + "/timeline.json";
    if(!ofFile::doesFileExist(path)) {
        clear();
        return false;
    }
    failedLoadPath.clear();
    try {
        const ofJson json = ofLoadJson(path);
        if(!json.is_object()) {
            // ofLoadJson reports a parse error and hands back nothing.
            ofLogError("ofxOceanodeTimeline") << "Could not read " << path
                << " (a copy is kept as timeline.unreadable.json on the next save)";
            clear();
            failedLoadPath = path;
            return false;
        }
        fromJson(json);
        return true;
    } catch(const std::exception& e) {
        ofLogError("ofxOceanodeTimeline") << "Could not load timeline: " << e.what()
            << " (a copy is kept as timeline.unreadable.json on the next save)";
        clear();
        failedLoadPath = path;
        return false;
    }
}
