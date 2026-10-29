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

ofxOceanodeTimelineCurveTension fitSegmentShape(const std::vector<ofxOceanodeTimelineCurvePoint>& points,
                                                const std::vector<ofxOceanodeTimelineCurveTension>& tensions,
                                                const std::string& interpolation, double x0, double x1) {
    const auto mode = curveInterpolationMode(interpolation);
    const float fallback = points.empty() ? 0.0f : points.front().value;
    auto valueAt = [&](double beat) { return valueAtBeat(points, tensions, interpolation, beat, fallback); };
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
}
} // namespace ofxOceanodeTimelineCurve
namespace {

std::string resolveTimelineAudioPath(const std::string& input) {
    if(input.empty()) return {};
    if(ofFile::doesFileExist(input)) return input;
    const std::string dataPath = ofToDataPath(input, true);
    if(ofFile::doesFileExist(dataPath)) return dataPath;
    const std::string samplePath = ofToDataPath("Supercollider/Samples/" + input, true);
    if(ofFile::doesFileExist(samplePath)) return samplePath;
    return input;
}

std::string sumAutomationValues(const std::vector<std::string>& values,
                                const std::string& valueType) {
    std::vector<std::pair<ofxOceanodeTimelineAutomationMode, std::string>> additions;
    additions.reserve(values.size());
    for(const auto& value : values)
        additions.emplace_back(ofxOceanodeTimelineAutomationMode::Add, value);
    return combineAutomationValues(additions, valueType);
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

constexpr float kFrequencyGridFactors[3] = {1.0f, 2.0f / 3.0f, 1.5f}; // straight, triplet, dotted
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
} // namespace
void ofxOceanodeTimelineMergeOverlappingGates(std::vector<ofxOceanodeTimelineGateRegion>& row) {
    std::stable_sort(row.begin(), row.end(), [](const auto& a, const auto& b) { return a.startBeat < b.startBeat; });
    std::vector<ofxOceanodeTimelineGateRegion> merged;
    for(const auto& region : row) {
        if(!merged.empty() && region.startBeat < merged.back().end() - kEpsilon)
            merged.back().durationBeats = std::max(merged.back().end(), region.end()) - merged.back().startBeat;
        else merged.push_back(region);
    }
    row = std::move(merged);
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

double contentOffset(const ofxOceanodeTimelineClip& clip) {
    if(!(clip.contentOffsetBeats > 0.0) || !std::isfinite(clip.contentOffsetBeats)) return 0.0;
    const double offset = positiveModulo(clip.contentOffsetBeats, sourceDuration(clip));
    return offset < sourceDuration(clip) - 1e-9 ? offset : 0.0;
}

double sourceToTimelineBeat(const ofxOceanodeTimelineClip& clip,
                            double sourceBeat, int64_t cycle) {
    const double content = sourceDuration(clip);
    // Where in the cycle that content beat plays: content before the offset
    // plays after the content's end.
    if(const double offset = contentOffset(clip); offset > 0.0) {
        sourceBeat -= offset;
        if(sourceBeat < -kEpsilon) sourceBeat += content;
    }
    if(clip.repeatContent)
        return clip.startBeat + (static_cast<double>(cycle) * content + sourceBeat) * stretch(clip);
    return clip.startBeat + sourceBeat * clip.durationBeats / content;
}

double timelineToSourceBeat(const ofxOceanodeTimelineClip& clip,
                            double timelineBeat) {
    const double localBeat = std::max(0.0, timelineBeat - clip.startBeat);
    const double played = clip.repeatContent
        ? positiveModulo(localBeat, cycleDuration(clip)) / stretch(clip)
        : localBeat * sourceDuration(clip) / std::max(1.0 / 24.0, clip.durationBeats);
    const double offset = contentOffset(clip);
    if(offset <= 0.0) return played;
    const double rotated = played + offset;
    return rotated >= sourceDuration(clip) ? rotated - sourceDuration(clip) : rotated;
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
} // namespace

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

ofxOceanodeTimelineManager::ofxOceanodeTimelineManager(ofxOceanodeContainer* owner) : container(owner),
    clipChanceSalt(static_cast<uint64_t>(ofRandom(1.0f, 16777216.0f)) * 0x9e3779b97f4a7c15ULL) {
    playContext.salt = clipChanceSalt;
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

void ofxOceanodeTimelineManager::rebuildParameterTrackColorCache() const {
    parameterTrackColorCache.clear();
    size_t bindingCount = 0;
    for(const auto& track : mainSequence.tracks) bindingCount += track.bindings.size();
    parameterTrackColorCache.reserve(bindingCount);
    // Preserve the previous first-track-wins behaviour for parameters that
    // are published to more than one track.
    for(const auto& track : mainSequence.tracks) {
        for(const auto& binding : track.bindings)
            parameterTrackColorCache.emplace(binding.parameterPath, track.color);
    }
    parameterTrackColorCacheFrame = static_cast<uint64_t>(ofGetFrameNum());
}

void ofxOceanodeTimelineManager::setContainer(ofxOceanodeContainer* owner) {
    if(container == owner) return;
    for(const auto& track : mainSequence.tracks) clearTimelineFlag(track);
    container = owner;
    invalidateEvaluationIndexes();
    std::set<std::string> parameterPaths;
    for(const auto& track : mainSequence.tracks)
        for(const auto& binding : track.bindings)
            parameterPaths.insert(binding.parameterPath);
    for(const auto& path : parameterPaths) refreshTimelineFlag(path);
}

ofxOceanodeAbstractParameter* ofxOceanodeTimelineResolver::find(const std::string& path) const {
    return base == nullptr ? nullptr : base->findTimelineParameter(path);
}

std::string ofxOceanodeTimelineResolver::pathOf(ofxOceanodeAbstractParameter& parameter) const {
    return base == nullptr ? std::string() : base->getTimelineParameterPath(parameter, *base);
}

std::string ofxOceanodeTimelineManager::createTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName) {
    ofxOceanodeTimelineTrack track;
    track.id = sequence.makeUniqueTrackId();
    track.name = sequence.makeUniqueTrackName(requestedName);
    static const ofColor defaultColors[] = {
        ofColor(65, 165, 245, 255),
        ofColor(75, 190, 125, 255),
        ofColor(235, 165, 65, 255),
        ofColor(190, 95, 220, 255),
        ofColor(235, 90, 95, 255)
    };
    track.color = defaultColors[sequence.tracks.size() % (sizeof(defaultColors) / sizeof(defaultColors[0]))];
    ++sequence.nextTrackNumber;
    sequence.tracks.push_back(std::move(track));
    invalidateEvaluationIndexes();
    return sequence.tracks.back().id;
}

std::string ofxOceanodeTimelineManager::insertTrackCopy(ofxOceanodeTimelineSequence& sequence, ofxOceanodeTimelineTrack copy,
    const std::string& name, size_t index) {
    const auto id = createTrack(sequence, name);
    copy.name = getTrack(sequence, id)->name;
    copy.id = id;
    *getTrack(sequence, id) = std::move(copy);
    auto& duplicate = *getTrack(sequence, id);
    std::map<std::string, std::string> bindings;
    for(auto& binding : duplicate.bindings) {
        const auto old = binding.id;
        binding.id = sequence.makeUniqueBindingId(); ++sequence.nextBindingNumber;
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
        clip.id = sequence.makeUniqueClipId(); ++sequence.nextClipNumber;
        remap(clip.lfoOutputBindingId);
        for(auto& lane : clip.lanes) {
            lane.id = sequence.makeUniqueLaneId(); ++sequence.nextLaneNumber;
            for(auto& binding : lane.bindingIds) remap(binding);
            lane.bindingIds.erase(std::remove(lane.bindingIds.begin(), lane.bindingIds.end(), std::string()), lane.bindingIds.end());
            remap(lane.pianoPitchBindingId); remap(lane.pianoGateBindingId); remap(lane.pianoVelocityBindingId);
            remap(lane.drumDurationBindingId); remap(lane.drumFilterBindingId); remap(lane.drumStutterBindingId);
        }
    }
    std::rotate(sequence.tracks.begin() + index, sequence.tracks.end() - 1, sequence.tracks.end());
    sequence.normalizeFolders(); // a copy placed next to a folder goes after its contents
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return id;
}

std::string ofxOceanodeTimelineManager::duplicateTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId) {
    const std::string sourceId = trackId; // caller may pass the track's own id by reference
    const auto* source = getTrack(sequence, sourceId);
    if(source == nullptr) return {};
    const auto groups = sequence.clipGroups;
    std::vector<std::string> originals;
    for(const auto& clip : source->clips) originals.push_back(clip.id);
    const size_t index = static_cast<size_t>(source - sequence.tracks.data()) + 1;
    const auto id = insertTrackCopy(sequence, *source, source->name + " copy", index);
    std::map<std::string, std::string> clips;
    const auto& copied = getTrack(sequence, id)->clips;
    for(size_t i = 0; i < originals.size(); ++i) clips[originals[i]] = copied[i].id;
    // Independent groups only; never link copies back to another track.
    for(const auto& group : groups) {
        std::vector<std::pair<std::string, std::string>> members;
        for(const auto& member : group.members) {
            const auto found = clips.find(member.second);
            if(member.first == sourceId && found != clips.end()) members.emplace_back(id, found->second);
        }
        if(members.size() > 1) groupClips(sequence, members);
    }
    if(getTrack(sequence, id)->isFolder) {
        // A folder's copy holds copies of everything inside the original.
        std::vector<std::string> children;
        for(const auto& track : sequence.tracks) if(track.parentId == sourceId) children.push_back(track.id);
        for(const auto& child : children) {
            const auto childCopy = duplicateTrack(sequence, child);
            if(!childCopy.empty()) moveTrack(sequence, childCopy, id, std::numeric_limits<size_t>::max());
        }
    }
    return id;
}

std::vector<std::pair<std::string, std::string>> ofxOceanodeTimelineManager::breakClipLanesIntoTracks(ofxOceanodeTimelineSequence& sequence, 
    const std::string& trackId, const std::string& clipId) {
    const std::string sourceId = trackId, sourceClipId = clipId;
    const auto* source = getTrack(sequence, sourceId);
    const auto* sourceClip = getClip(sequence, sourceId, sourceClipId);
    if(source == nullptr || sourceClip == nullptr || source->isWaveTrack ||
       sourceClip->isLfo || sourceClip->lanes.size() < 2) return {};
    size_t index = static_cast<size_t>(source - sequence.tracks.data()) + 1;
    // Copy track settings without copying all the other clips for every lane.
    ofxOceanodeTimelineTrack settings;
    settings.name = source->name;
    settings.color = source->color;
    settings.collapsed = source->collapsed;
    settings.bindings = source->bindings;
    settings.noteGroups = source->noteGroups;
    settings.parentId = source->parentId; // the new tracks stay in the source's folder
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
        const auto id = insertTrackCopy(sequence, std::move(copy), settings.name + " / " + lane.name, index++);
        result.emplace_back(id, getTrack(sequence, id)->clips.front().id);
    }
    removeClip(sequence, sourceId, sourceClipId);
    return result;
}

std::string ofxOceanodeTimelineManager::createWaveTrack(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName) {
    // Macro timelines hold automation only (audio voices are keyed by track and clip id).
    if(!isMainSequence(sequence)) return std::string();
    const auto trackId = createTrack(sequence, requestedName);
    if(auto* track = getTrack(sequence, trackId)) {
        track->isWaveTrack = true;
        track->waveTrackHeight = 96.0f;
        track->waveVolumeAutomationEnabled = true;
        track->waveVolumePoints = {{0.0, track->waveVolume}, {4.0, track->waveVolume}};
        track->waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
    }
    return trackId;
}

std::string ofxOceanodeTimelineManager::createWaveVolumeLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                             const std::string& clipId) {
    const auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    if(track == nullptr || clip == nullptr || !track->isWaveTrack) return {};
    // Compatibility shim for older callers. Volume belongs to the whole
    // Wave Track now, so never create a lane inside this clip.
    auto* editableTrack = getTrack(sequence, trackId);
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

std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(ofxOceanodeTimelineSequence& sequence, const std::string& trackId) {
    static std::vector<ofxOceanodeTimelineCurvePoint> empty;
    auto* track = getTrack(sequence, trackId);
    return track != nullptr ? track->waveVolumePoints : empty;
}

const std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId) const {
    static const std::vector<ofxOceanodeTimelineCurvePoint> empty;
    const auto* track = getTrack(sequence, trackId);
    return track != nullptr ? track->waveVolumePoints : empty;
}

bool ofxOceanodeTimelineManager::addWaveTrackVolumePoint(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat, float value) {
    auto* track = getTrack(sequence, trackId);
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

bool ofxOceanodeTimelineManager::removeWaveTrackVolumePoint(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, double beat, double epsilon) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || !track->isWaveTrack) return false;
    const auto oldSize = track->waveVolumePoints.size();
    track->waveVolumePoints.erase(std::remove_if(track->waveVolumePoints.begin(), track->waveVolumePoints.end(),
        [&](const auto& point) { return std::abs(point.beat - beat) <= epsilon; }), track->waveVolumePoints.end());
    track->waveVolumeTensions.resize(track->waveVolumePoints.size() > 1 ? track->waveVolumePoints.size() - 1 : 0);
    return oldSize != track->waveVolumePoints.size();
}

bool ofxOceanodeTimelineManager::renameTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& requestedName) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || requestedName.empty()) return false;
    if(track->name == requestedName) return true;
    track->name = sequence.makeUniqueTrackName(requestedName);
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

bool ofxOceanodeTimelineManager::removeTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId) {
    invalidateSchedule(); // correct queued events while the track still exists
    auto it = std::find_if(sequence.tracks.begin(), sequence.tracks.end(), [&](const auto& track) { return track.id == trackId; });
    if(it == sequence.tracks.end()) return false;
    std::set<std::string> affectedPaths;
    for(const auto& binding : it->bindings) {
        affectedPaths.insert(binding.parameterPath);
        if(isMainSequence(sequence)) stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + binding.id);
        if(isMainSequence(sequence)) stateCaptureLastAppliedValues.erase(binding.parameterPath);
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
        removeClipFromGroups(sequence, trackId, clip.id);
        if(isMainSequence(sequence)) releaseWaveClipIfNeeded(trackId, clip.id);
    }
    sequence.tracks.erase(it);
    sequence.normalizeFolders(); // a removed folder's tracks move to the top level
    invalidateEvaluationIndexes();
    for(const auto& path : affectedPaths) refreshTimelineFlag(path);
    return true;
}

ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId) {
    // The id cache indexes the main timeline only; ids repeat between sequences.
    if(evaluationIndexesValid && isMainSequence(sequence)) {
        const auto cached = trackByIdCache.find(trackId);
        if(cached != trackByIdCache.end()) return cached->second;
    }
    return sequence.findTrack(trackId);
}

const ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId) const {
    // The id cache indexes the main timeline only; ids repeat between sequences.
    if(evaluationIndexesValid && isMainSequence(sequence)) {
        const auto cached = trackByIdCache.find(trackId);
        if(cached != trackByIdCache.end()) return cached->second;
    }
    return sequence.findTrack(trackId);
}

std::string ofxOceanodeTimelineManager::addBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                   ofxOceanodeAbstractParameter& parameter,
                                                   ofxOceanodeTimelineAutomationMode mode) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || track->isWaveTrack || track->isFolder || track->isMacroTrack() || container == nullptr ||
       !isStepLaneCompatible(parameter)) return std::string();

    // Relative to the sequence's container; nothing outside it.
    const std::string path = resolverFor(sequence).pathOf(parameter);
    if(path.empty()) return std::string();
    // A parameter may be published to several tracks. Each track owns an
    // independent binding (and blend mode), while duplicate bindings inside
    // one track would only create ambiguous rows and are therefore rejected.
    if(std::any_of(track->bindings.begin(), track->bindings.end(), [&](const auto& existing) {
        return existing.parameterPath == path;
    })) return std::string();

    ofxOceanodeTimelineParameterBinding binding;
    binding.id = sequence.makeUniqueBindingId();
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
    ++sequence.nextBindingNumber;
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

bool ofxOceanodeTimelineManager::bindClipTarget(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
    const std::string& laneId, const std::string& path, ofxOceanodeTimelineTargetRole role) {
    auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    auto* parameter = container == nullptr ? nullptr : container->findTimelineParameter(path);
    if(track == nullptr || track->isWaveTrack || clip == nullptr || parameter == nullptr ||
       !isStepLaneCompatible(*parameter) || (parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly)) return false;
    using Role = ofxOceanodeTimelineTargetRole;
    if(role == Role::LfoOutput) { if(!clip->isLfo) return false; }
    else if(lane == nullptr || clip->isLfo || (role != Role::Lane && !ofxOceanodeTimelineLaneUsesNoteRoles(lane->type))) return false;
    std::string bindingId;
    for(const auto& binding : track->bindings) if(binding.parameterPath == path) bindingId = binding.id;
    if(bindingId.empty()) bindingId = addBinding(sequence, trackId, *parameter);
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
        addBindingToLane(sequence, trackId, clipId, laneId, bindingId);
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
        addBindingToLane(sequence, trackId, clipId, laneId, bindingId);
        if(!previous.empty() && previous != bindingId && previous != lane->pianoPitchBindingId &&
           previous != lane->pianoGateBindingId && previous != lane->pianoVelocityBindingId &&
           previous != lane->drumDurationBindingId && previous != lane->drumFilterBindingId &&
           previous != lane->drumStutterBindingId)
            removeBindingFromLane(sequence, trackId, clipId, laneId, previous);
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

bool ofxOceanodeTimelineManager::moveBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId, int newIndex) {
    auto* track = getTrack(sequence, trackId);
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


bool ofxOceanodeTimelineManager::setBindingOrder(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::vector<std::string>& orderedBindingIds) {
    auto* track = getTrack(sequence, trackId);
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

ofxOceanodeTimelineNoteGroup* ofxOceanodeTimelineManager::getNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId) {
    auto* track = getTrack(sequence, trackId);
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

std::string ofxOceanodeTimelineManager::createNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& name,
                                                        const std::string& pitchBindingId, const std::string& gateBindingId,
                                                        const std::string& velocityBindingId) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || track->isWaveTrack) return std::string();
    ofxOceanodeTimelineNoteGroup group;
    uint64_t number = 1;
    do {
        group.id = ofxOceanodeTimelineSequence::makeId("timeline_notes", number++);
    } while(std::any_of(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == group.id; }));
    group.name = name.empty() ? std::string("Notes") : name;
    track->noteGroups.push_back(group);
    const std::string id = group.id;
    if(!setNoteGroupRoles(sequence, trackId, id, pitchBindingId, gateBindingId, velocityBindingId)) return std::string();
    return getNoteGroup(sequence, trackId, id) != nullptr ? id : std::string();
}

bool ofxOceanodeTimelineManager::setNoteGroupRoles(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                                                   const std::string& pitchBindingId, const std::string& gateBindingId,
                                                   const std::string& velocityBindingId) {
    auto* track = getTrack(sequence, trackId);
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
} // namespace

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

bool ofxOceanodeTimelineManager::setNoteGroupKitRole(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                                                     const std::string& bindingId, int which) {
    const auto& role = which == 0 ? kDurationRole : which == 1 ? kFilterRole : kStutterRole;
    auto* track = getTrack(sequence, trackId);
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

bool ofxOceanodeTimelineManager::removeNoteGroup(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId, bool removeBindings) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return false;
    auto it = std::find_if(track->noteGroups.begin(), track->noteGroups.end(), [&](const auto& g) { return g.id == groupId; });
    if(it == track->noteGroups.end()) return false;
    const auto members = it->members();
    track->noteGroups.erase(it);
    if(removeBindings) {
        for(const auto& id : members) removeBinding(sequence, trackId, id);
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

std::string ofxOceanodeTimelineManager::createNoteGroupClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& groupId,
                                                            const std::string& requestedName, double startBeat,
                                                            double durationBeats, std::string* laneIdOut,
                                                            ofxOceanodeTimelineLaneType laneType) {
    const auto* groupPtr = getNoteGroup(sequence, trackId, groupId);
    if(groupPtr == nullptr) return std::string();
    const auto group = *groupPtr;
    const std::string clipId = createClip(sequence, trackId, requestedName.empty() ? group.name : requestedName, startBeat, durationBeats);
    if(clipId.empty()) return clipId;
    if(laneType == ofxOceanodeTimelineLaneType::DrumGrid) {
        const std::string laneId = createLane(sequence, trackId, clipId, group.name, ofxOceanodeTimelineLaneType::DrumGrid);
        if(laneId.empty()) return clipId;
        if(auto* lane = getLane(sequence, trackId, clipId, laneId)) {
            lane->pianoPitchBindingId = group.pitchBindingId;
            lane->pianoGateBindingId = group.gateBindingId;
            lane->pianoVelocityBindingId = group.velocityBindingId;
            lane->drumDurationBindingId = group.durationBindingId;
            lane->drumFilterBindingId = group.filterBindingId;
            lane->drumStutterBindingId = group.stutterBindingId;
            lane->bindingIds = group.members();
            int voices = 0;
            std::vector<std::string> names;
            const auto* track = getTrack(sequence, trackId);
            if(track == nullptr || !drumGridInstrument(*track, *lane, voices, names) || voices <= 0) voices = 4;
            if(group.voices > 0) voices = group.voices;
            const auto* clip = getClip(sequence, trackId, clipId);
            // Rows as long as the clip's content (at most 16 beats).
            const double length = std::min(16.0, clip == nullptr ? 4.0 : clip->contentDurationBeats);
            lane->drumRows = ofxOceanodeTimelineDrumGrid::defaultRows(voices, names, length);
        }
        invalidateEvaluationIndexes();
        if(laneIdOut != nullptr) *laneIdOut = laneId;
        return clipId;
    }
    const std::string laneId = createLane(sequence, trackId, clipId, group.name, ofxOceanodeTimelineLaneType::PianoRoll);
    if(laneId.empty()) return clipId;
    if(auto* lane = getLane(sequence, trackId, clipId, laneId)) {
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
        const auto* track = getTrack(sequence, trackId);
        int voices = 0;
        bool kit = false;
        if(track != nullptr) pianoLaneVoicesAndKit(*track, *lane, voices, kit);
        if(kit) {
            lane->pianoLowPitch = 36;
            lane->pianoHighPitch = 36 + std::max(12, voices) - 1;
        } else if(const auto* pitchBinding = getBinding(sequence, trackId, group.pitchBindingId)) {
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
} // namespace ofxOceanodeTimelineScale

int ofxOceanodeTimelineManager::retargetParameterPaths(ofxOceanodeTimelineSequence& sequence, const std::map<std::string, std::string>& oldToNew) {
    int changed = 0;
    for(auto& track : sequence.tracks) {
        for(auto& binding : track.bindings) {
            const auto it = oldToNew.find(binding.parameterPath);
            if(it == oldToNew.end() || it->second.empty() || it->second == it->first) continue;
            binding.parameterPath = it->second;
            binding.missingTarget = false;
            changed++;
        }
    }
    // Macro tracks follow a moved macro: its canvas id changed as its
    // parameters' paths did (same node and parameter after "::").
    std::map<std::string, std::string> canvases;
    for(const auto& entry : oldToNew) {
        const auto oldCut = entry.first.find("::"), newCut = entry.second.find("::");
        if(oldCut == std::string::npos || newCut == std::string::npos) continue;
        if(entry.first.substr(oldCut) != entry.second.substr(newCut)) continue;
        canvases.emplace(entry.first.substr(0, oldCut), entry.second.substr(0, newCut));
    }
    for(auto& track : sequence.tracks) {
        if(!track.isMacroTrack()) continue;
        for(const auto& canvas : canvases) {
            if(track.macroCanvas == canvas.first) track.macroCanvas = canvas.second;
            else if(track.macroCanvas.rfind(canvas.first + " / ", 0) == 0)
                track.macroCanvas = canvas.second + track.macroCanvas.substr(canvas.first.size());
            else continue;
            ++changed;
            break;
        }
    }
    if(changed > 0) {
        invalidateEvaluationIndexes();
        invalidateSchedule();
    }
    return changed;
}

bool ofxOceanodeTimelineManager::removeBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId) {
    invalidateSchedule(); // correct queued events while the binding still exists
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return false;
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](const auto& binding) { return binding.id == bindingId; });
    if(it == track->bindings.end()) return false;
    const std::string parameterPath = it->parameterPath;
    if(isMainSequence(sequence)) stateCaptureManualOverrideKeys.erase(trackId + "\x1f" + bindingId);
    if(isMainSequence(sequence)) stateCaptureLastAppliedValues.erase(parameterPath);
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

bool ofxOceanodeTimelineManager::setBindingMode(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                const std::string& bindingId,
                                                ofxOceanodeTimelineAutomationMode mode) {
    auto* binding = getBinding(sequence, trackId, bindingId);
    if(binding == nullptr) return false;
    binding->mode = mode;
    return true;
}

bool ofxOceanodeTimelineManager::setBindingClamp(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                 const std::string& bindingId,
                                                 bool clampToParameterRange) {
    auto* binding = getBinding(sequence, trackId, bindingId);
    if(binding == nullptr) return false;
    binding->clampToParameterRange = clampToParameterRange;
    return true;
}

int ofxOceanodeTimelineManager::captureStateAsCurvePoints(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                          const std::string& bindingId,
                                                          double timelineBeat) {
    auto* track = getTrack(sequence, trackId);
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
        fallbackCaptureClipId = createClip(sequence, trackId, "Captured State", timelineBeat, captureTail);
        auto* created = getClip(sequence, trackId, fallbackCaptureClipId);
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
        if(const auto* currentTrack = getTrack(sequence, trackId)) {
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
        auto* captureClip = getClip(sequence, trackId, captureClipId);
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
        auto* captureClip = getClip(sequence, trackId, captureClipId); // createLane may move lane storage.
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
            const auto laneId = createLane(sequence, trackId, captureClipId, target.parameterPath,
                                           ofxOceanodeTimelineLaneType::Curve);
            if(laneId.empty() || !addBindingToLane(sequence, trackId, captureClipId, laneId, target.bindingId)) continue;
            lane = getLane(sequence, trackId, captureClipId, laneId);
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
        const bool decibels = ofxOceanodeTimelineScale::isDecibels(*lane, getBinding(sequence, trackId, target.bindingId));
        const float normalized = decibels ? ofxOceanodeTimelineScale::gainToNormalized(*lane, target.value)
            : std::abs(span) < 1e-9f ? 0.0f
            : ofClamp((target.value - lane->valueMin) / span, 0.0f, 1.0f);
        setCurvePoint(*lane, sourceBeat, normalized);

        const std::string overrideKey = trackId + "\x1f" + target.bindingId;
        if(isMainSequence(sequence) && stateCaptureManualOverrideKeys.erase(overrideKey) > 0) {
            if(auto* binding = getBinding(sequence, trackId, target.bindingId)) {
                binding->hasLiveOverride = false;
                binding->liveOverrideValue.clear();
            }
        }
        if(auto* parameter = isMainSequence(sequence) ? container->findTimelineParameter(target.parameterPath) : nullptr)
            stateCaptureLastAppliedValues[target.parameterPath] = parameterValueForAutomation(*parameter);
        ++captured;
    }

    if(auto* sortedTrack = getTrack(sequence, trackId)) {
        std::sort(sortedTrack->clips.begin(), sortedTrack->clips.end(), [](const auto& a, const auto& b) {
            if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
            return a.id < b.id;
        });
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return captured;
}

std::string ofxOceanodeTimelineManager::createClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                   const std::string& requestedName,
                                                   double startBeat,
                                                   double durationBeats) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || track->isFolder) return std::string();

    ofxOceanodeTimelineClip clip;
    clip.id = sequence.makeUniqueClipId();
    clip.name = ofxOceanodeTimelineSequence::makeUniqueClipName(*track, requestedName);
    clip.startBeat = std::max(0.0, startBeat);
    clip.durationBeats = std::max(1.0 / 24.0, durationBeats);
    clip.contentDurationBeats = clip.durationBeats;
    ++sequence.nextClipNumber;
    track->clips.push_back(std::move(clip));
    invalidateEvaluationIndexes();
    return track->clips.back().id;
}

std::string ofxOceanodeTimelineManager::createLfoClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                      const std::string& outputBindingId,
                                                      const std::string& requestedName,
                                                      double startBeat,
                                                      double durationBeats) {
    const auto clipId = createClip(sequence, trackId, requestedName, startBeat, durationBeats);
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return std::string();

    clip->isLfo = true;
    clip->lfoOutputBindingId = outputBindingId;
    if(const auto* binding = getBinding(sequence, trackId, outputBindingId)) {
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

    ensureLfoLanes(sequence, trackId, clipId);
    return clipId;
}

void ofxOceanodeTimelineManager::ensureLfoLanes(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr || !clip->isLfo) return;
    for(const auto& definition : kLfoParameters) {
        const bool present = std::any_of(clip->lanes.begin(), clip->lanes.end(),
            [&](const auto& lane) { return lane.lfoParameter == definition.id; });
        if(present) continue;
        const auto laneId = createLane(sequence, trackId, clipId, definition.name, ofxOceanodeTimelineLaneType::Curve);
        clip = getClip(sequence, trackId, clipId); // createLane may reallocate
        if(auto* lane = getLane(sequence, trackId, clipId, laneId)) {
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
        return ofxOceanodeTimelineCurve::fitSegmentShape(points, lane.curveTensions, lane.curveInterpolation, x0, x1);
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

// ---- Clip merging helpers --------------------------------------------------
// A clip's lane laid out as the clip plays it, on a span that starts `placeAt`
// beats into the merged clip and lasts the clip's own length: the content
// offset is baked in by the caller, repeats are unrolled and the stretch is
// applied, so one merged beat is one timeline beat. Pattern lanes (Step,
// Multi Slider, Drum Grid) become a single pattern as long as the merged clip
// (`total` beats), holding events only over this clip's span.
struct ClipLayout {
    double placeAt = 0.0; // where the clip starts in the merged clip
    double length = 0.0;  // its timeline length
    double content = 1.0; // source beats per repeat
    double stretch = 1.0; // timeline beats per source beat
    // Each repeat k as source range [0, to) placed at k * content (unrolled source beats).
    template<class F> void forEachRepeat(F&& f) const {
        for(int64_t k = 0; static_cast<double>(k) * content * stretch < length - kEpsilon; ++k)
            f(static_cast<double>(k) * content, std::min(content, (length - static_cast<double>(k) * content * stretch) / stretch));
    }
    double toMerged(double unrolledSourceBeat) const { return placeAt + unrolledSourceBeat * stretch; }
    // Pattern occurrences of position q (pattern `period` long) inside the
    // played span, as unrolled source beats.
    template<class F> void forEachOccurrence(double period, double q, F&& f) const {
        forEachRepeat([&](double base, double to) {
            for(double patternStart = 0.0; patternStart < to - kEpsilon; patternStart += period)
                if(patternStart + q < to - kEpsilon) f(base + patternStart + q, base + to);
        });
    }
};

void layOutCurve(const ofxOceanodeTimelineLane& lane, const ClipLayout& layout,
                 std::vector<ofxOceanodeTimelineCurvePoint>& points, std::vector<ofxOceanodeTimelineCurveTension>& tensions) {
    points.clear(); tensions.clear();
    layout.forEachRepeat([&](double base, double to) { appendCurveWindow(lane, 0.0, to, base, points, tensions); });
    for(auto& point : points) point.beat = layout.toMerged(point.beat);
}

template<class Event>
std::vector<Event> layOutEvents(const std::vector<Event>& events, const ClipLayout& layout) {
    std::vector<Event> out;
    layout.forEachRepeat([&](double base, double to) { appendEventWindow(events, 0.0, to, base, out); });
    for(auto& event : out) {
        event.startBeat = layout.toMerged(event.startBeat);
        event.durationBeats *= layout.stretch;
    }
    return out;
}

// Value of a placed curve at a merged beat (the points' own interpolation).
float placedCurveValue(const ofxOceanodeTimelineLane& lane, const std::vector<ofxOceanodeTimelineCurvePoint>& points,
                       const std::vector<ofxOceanodeTimelineCurveTension>& tensions, double beat) {
    return valueAtBeat(points, tensions, lane.curveInterpolation, beat, points.empty() ? 0.0f : points.front().value);
}
} // namespace

bool ofxOceanodeTimelineManager::computeClipSplit(const ofxOceanodeTimelineClip& placed, double timelineBeat,
                                                  ofxOceanodeTimelineClip& left, ofxOceanodeTimelineClip& right) const {
    // Cut content as it plays: a rotated clip is laid out from its offset first.
    ofxOceanodeTimelineClip original = placed;
    bakeContentOffset(original);
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

    cutClipContent(original, cut, repeating, left, right);
    return true;
}

void ofxOceanodeTimelineManager::cutClipContent(const ofxOceanodeTimelineClip& original, double cut, bool repeating,
                                                ofxOceanodeTimelineClip& left, ofxOceanodeTimelineClip& right) const {
    const double content = ofxOceanodeTimelineClipTime::sourceDuration(original);
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
}

void ofxOceanodeTimelineManager::bakeContentOffset(ofxOceanodeTimelineClip& clip) const {
    const double offset = ofxOceanodeTimelineClipTime::contentOffset(clip);
    clip.contentOffsetBeats = 0.0;
    if(offset <= 0.0) return;
    // The right part of a repeating cut at the offset is the content rotated
    // to start there.
    ofxOceanodeTimelineClip left = clip, rotated = clip;
    cutClipContent(clip, offset, true, left, rotated);
    clip.lanes = std::move(rotated.lanes);
}

std::string ofxOceanodeTimelineManager::mergeClips(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                   const std::string& firstClipId, const std::string& secondClipId) {
    const auto* track = getTrack(sequence, trackId);
    if(track == nullptr || track->isWaveTrack || track->isMacroTrack() || firstClipId == secondClipId) return std::string();
    const auto* first = getClip(sequence, trackId, firstClipId);
    const auto* second = getClip(sequence, trackId, secondClipId);
    if(first == nullptr || second == nullptr) return std::string();
    for(const auto* clip : {first, second})
        if(clip->isLfo || clip->isStateCapture) return std::string();
    // The earlier clip leads: it keeps its id and settings, and the later
    // one's content wins where they overlap (as overlapping clips play).
    auto order = [&](const std::string& id) {
        return std::find_if(track->clips.begin(), track->clips.end(), [&](const auto& c) { return c.id == id; }) - track->clips.begin();
    };
    ofxOceanodeTimelineClip a = *first, b = *second;
    if(b.startBeat < a.startBeat - kEpsilon ||
       (std::abs(b.startBeat - a.startBeat) <= kEpsilon && order(b.id) < order(a.id))) std::swap(a, b);
    bakeContentOffset(a);
    bakeContentOffset(b);
    const double start = a.startBeat;
    const double total = std::max(a.startBeat + a.durationBeats, b.startBeat + b.durationBeats) - start;
    auto layoutOf = [&](const ofxOceanodeTimelineClip& clip) {
        ClipLayout layout;
        layout.placeAt = clip.startBeat - start;
        layout.length = clip.durationBeats;
        layout.content = ofxOceanodeTimelineClipTime::sourceDuration(clip);
        layout.stretch = ofxOceanodeTimelineClipTime::effectiveStretch(clip);
        return layout;
    };
    const ClipLayout firstLayout = layoutOf(a), secondLayout = layoutOf(b);
    const double cut = secondLayout.placeAt; // the second clip's data starts here
    auto isCurve = [](const ofxOceanodeTimelineLane& lane) {
        return lane.type == ofxOceanodeTimelineLaneType::Curve || ofxOceanodeTimelineIsHeldValueLane(lane.type);
    };

    // "The same parameters with the same kind of lane": one lane in the result.
    auto keyOf = [&](const ofxOceanodeTimelineLane& lane, const ClipLayout& layout) {
        auto ids = lane.bindingIds;
        std::sort(ids.begin(), ids.end());
        std::string key = ofToString(static_cast<int>(lane.type)) + "|" + lane.curveInterpolation + "|" +
            ofToString(lane.valueMin, 6) + "|" + ofToString(lane.valueMax, 6) + "|";
        for(const auto& id : ids) key += id + ",";
        key += "|" + lane.pianoPitchBindingId + "," + lane.pianoGateBindingId + "," + lane.pianoVelocityBindingId + "," +
               lane.drumDurationBindingId + "," + lane.drumFilterBindingId + "," + lane.drumStutterBindingId;
        // Pattern lanes share one grid: the same cell once stretched.
        if(lane.type == ofxOceanodeTimelineLaneType::Step || lane.type == ofxOceanodeTimelineLaneType::MultiSlider)
            key += "|" + ofToString(std::max(1.0 / 24.0, lane.beatsPerStep) * layout.stretch, 6);
        return key;
    };

    bool refused = false;
    auto layOut = [&](const ofxOceanodeTimelineLane& source, const ClipLayout& layout) {
        ofxOceanodeTimelineLane lane = source;
        if(isCurve(source)) {
            if(!source.curvePoints.empty()) layOutCurve(source, layout, lane.curvePoints, lane.curveTensions);
        } else if(source.type == ofxOceanodeTimelineLaneType::PianoRoll) {
            lane.pianoNotes = layOutEvents(source.pianoNotes, layout);
        } else if(source.type == ofxOceanodeTimelineLaneType::MultiGate) {
            for(size_t row = 0; row < source.multiGateRows.size(); ++row) lane.multiGateRows[row] = layOutEvents(source.multiGateRows[row], layout);
        } else if(source.type == ofxOceanodeTimelineLaneType::MultiValue) {
            for(size_t row = 0; row < source.multiValueRows.size(); ++row) lane.multiValueRows[row] = layOutEvents(source.multiValueRows[row], layout);
        } else if(source.type == ofxOceanodeTimelineLaneType::Step) {
            const double cell = std::max(1.0 / 24.0, source.beatsPerStep);
            const double period = std::max(cell, source.stepCount * cell);
            lane.step.steps.clear();
            for(const auto& step : source.step.steps) {
                if(step.startBeat >= period - kEpsilon) continue;
                layout.forEachOccurrence(period, step.startBeat, [&](double at, double limit) {
                    auto piece = step;
                    const double length = std::min(step.durationBeats > kEpsilon ? step.durationBeats : cell, limit - at);
                    piece.startBeat = layout.toMerged(at);
                    piece.durationBeats = length * layout.stretch;
                    lane.step.steps.push_back(piece);
                });
            }
            lane.beatsPerStep = cell * layout.stretch;
            lane.stepCount = std::max(1, static_cast<int>(std::ceil(total / lane.beatsPerStep - 1e-9)));
            lane.step.sortSteps();
        } else if(source.type == ofxOceanodeTimelineLaneType::MultiSlider) {
            const double cell = std::max(1.0 / 24.0, source.beatsPerStep);
            const int count = std::max(1, source.stepCount);
            lane.beatsPerStep = cell * layout.stretch;
            lane.stepCount = std::max(1, static_cast<int>(std::ceil(total / lane.beatsPerStep - 1e-9)));
            // Cells outside this clip's span are filled in after joining (NaN until then).
            lane.multiSliderValues.assign(static_cast<size_t>(lane.stepCount), std::numeric_limits<float>::quiet_NaN());
            for(int m = 0; m < lane.stepCount; ++m) {
                const double local = (m + 0.5) * lane.beatsPerStep - layout.placeAt;
                if(local < 0.0 || local >= layout.length) continue;
                const double sourceBeat = positiveModulo(local / layout.stretch, layout.content);
                const int index = std::min(count - 1, static_cast<int>(std::floor(positiveModulo(sourceBeat, cell * count) / cell)));
                if(index < static_cast<int>(source.multiSliderValues.size())) lane.multiSliderValues[static_cast<size_t>(m)] = source.multiSliderValues[static_cast<size_t>(index)];
            }
        } else if(source.type == ofxOceanodeTimelineLaneType::DrumGrid) {
            // Each row becomes one pattern as long as the merged clip.
            if(total > ofxOceanodeTimelineDrumGrid::kMaxLength + kEpsilon) refused = true;
            for(size_t r = 0; r < source.drumRows.size(); ++r) {
                const auto& row = source.drumRows[r];
                auto& placedRow = lane.drumRows[r];
                placedRow.steps.clear();
                placedRow.length = std::min(total, ofxOceanodeTimelineDrumGrid::kMaxLength);
                const double period = ofClamp(row.length, ofxOceanodeTimelineDrumGrid::kMinLength, ofxOceanodeTimelineDrumGrid::kMaxLength);
                for(const auto& step : row.steps) {
                    if(ofxOceanodeTimelineDrumGrid::stepHidden(row, step)) continue;
                    layout.forEachOccurrence(period, step.beat, [&](double at, double) {
                        auto piece = step;
                        piece.beat = layout.toMerged(at);
                        piece.length = step.length * layout.stretch;
                        placedRow.steps.push_back(piece);
                    });
                }
                ofxOceanodeTimelineDrumGrid::normalizeRow(placedRow);
            }
            lane.drumSwingBeats = source.drumSwingBeats * layout.stretch;
        }
        return lane;
    };

    // Joining the second clip's lane into the first's: the first gives way at `cut`.
    auto cropEvents = [&](auto& events) {
        events.erase(std::remove_if(events.begin(), events.end(), [&](const auto& e) { return e.startBeat >= cut - kEpsilon; }), events.end());
        for(auto& e : events) e.durationBeats = std::min(e.durationBeats, cut - e.startBeat);
    };
    auto join = [&](ofxOceanodeTimelineLane& into, const ofxOceanodeTimelineLane& from) {
        if(isCurve(into)) {
            if(from.curvePoints.empty()) return;
            if(into.curvePoints.empty()) { into.curvePoints = from.curvePoints; into.curveTensions = from.curveTensions; return; }
            // The first clip's value holds until the second starts (or is cut there).
            const float held = placedCurveValue(into, into.curvePoints, into.curveTensions, cut);
            while(!into.curvePoints.empty() && into.curvePoints.back().beat > cut + kEpsilon) into.curvePoints.pop_back();
            into.curveTensions.resize(into.curvePoints.empty() ? 0 : into.curvePoints.size() - 1);
            if(into.curvePoints.empty() || into.curvePoints.back().beat < cut - kEpsilon) {
                if(!into.curvePoints.empty()) into.curveTensions.push_back({});
                into.curvePoints.push_back({cut, held});
            }
            into.curveTensions.push_back({}); // a step from the held value to the second clip's first point
            into.curvePoints.insert(into.curvePoints.end(), from.curvePoints.begin(), from.curvePoints.end());
            into.curveTensions.insert(into.curveTensions.end(), from.curveTensions.begin(), from.curveTensions.end());
            into.curveTensions.resize(into.curvePoints.size() - 1);
        } else if(into.type == ofxOceanodeTimelineLaneType::PianoRoll) {
            cropEvents(into.pianoNotes);
            into.pianoNotes.insert(into.pianoNotes.end(), from.pianoNotes.begin(), from.pianoNotes.end());
        } else if(into.type == ofxOceanodeTimelineLaneType::MultiGate || into.type == ofxOceanodeTimelineLaneType::MultiValue) {
            into.multiRowCount = std::max(into.multiRowCount, from.multiRowCount);
            into.multiGateRows.resize(std::max(into.multiGateRows.size(), from.multiGateRows.size()));
            into.multiValueRows.resize(std::max(into.multiValueRows.size(), from.multiValueRows.size()));
            for(size_t row = 0; row < into.multiGateRows.size(); ++row) {
                cropEvents(into.multiGateRows[row]);
                if(row < from.multiGateRows.size())
                    into.multiGateRows[row].insert(into.multiGateRows[row].end(), from.multiGateRows[row].begin(), from.multiGateRows[row].end());
            }
            for(size_t row = 0; row < into.multiValueRows.size(); ++row) {
                cropEvents(into.multiValueRows[row]);
                if(row < from.multiValueRows.size())
                    into.multiValueRows[row].insert(into.multiValueRows[row].end(), from.multiValueRows[row].begin(), from.multiValueRows[row].end());
            }
        } else if(into.type == ofxOceanodeTimelineLaneType::Step) {
            cropEvents(into.step.steps);
            into.step.steps.insert(into.step.steps.end(), from.step.steps.begin(), from.step.steps.end());
            into.step.sortSteps();
        } else if(into.type == ofxOceanodeTimelineLaneType::MultiSlider) {
            for(size_t m = 0; m < into.multiSliderValues.size() && m < from.multiSliderValues.size(); ++m)
                if(!std::isnan(from.multiSliderValues[m])) into.multiSliderValues[m] = from.multiSliderValues[m];
        } else if(into.type == ofxOceanodeTimelineLaneType::DrumGrid) {
            for(const auto& fromRow : from.drumRows) {
                auto row = std::find_if(into.drumRows.begin(), into.drumRows.end(), [&](const auto& r) { return r.slot == fromRow.slot; });
                if(row == into.drumRows.end()) { into.drumRows.push_back(fromRow); continue; }
                row->steps.erase(std::remove_if(row->steps.begin(), row->steps.end(), [&](const auto& s) { return s.beat >= cut - kEpsilon; }), row->steps.end());
                row->steps.insert(row->steps.end(), fromRow.steps.begin(), fromRow.steps.end());
                ofxOceanodeTimelineDrumGrid::normalizeRow(*row);
            }
        }
    };

    ofxOceanodeTimelineClip merged = a;
    merged.startBeat = start;
    merged.durationBeats = total;
    merged.contentDurationBeats = total;
    merged.contentStretch = 1.0;
    merged.repeatContent = false;
    merged.contentOffsetBeats = 0.0;
    merged.lanes.clear();
    std::vector<std::string> firstKeys;
    std::vector<bool> joined;
    for(const auto& lane : a.lanes) {
        merged.lanes.push_back(layOut(lane, firstLayout));
        firstKeys.push_back(keyOf(lane, firstLayout));
        joined.push_back(false);
    }
    for(const auto& lane : b.lanes) {
        auto placed = layOut(lane, secondLayout);
        const auto key = keyOf(lane, secondLayout);
        size_t match = firstKeys.size();
        for(size_t i = 0; i < firstKeys.size(); ++i)
            if(!joined[i] && firstKeys[i] == key) { match = i; break; }
        if(match == firstKeys.size()) { merged.lanes.push_back(std::move(placed)); continue; } // keeps its own lane id
        join(merged.lanes[match], placed);
        joined[match] = true;
    }
    if(refused) return std::string();
    // Slider cells no clip covers hold the nearest covered value.
    for(auto& lane : merged.lanes) {
        if(lane.type != ofxOceanodeTimelineLaneType::MultiSlider) continue;
        auto& values = lane.multiSliderValues;
        float last = std::numeric_limits<float>::quiet_NaN();
        for(auto& value : values) { if(std::isnan(value)) value = last; else last = value; }
        float next = std::numeric_limits<float>::quiet_NaN();
        for(auto it = values.rbegin(); it != values.rend(); ++it) { if(std::isnan(*it)) *it = next; else next = *it; }
        for(auto& value : values) if(std::isnan(value)) value = 0.0f;
    }

    const std::string mergedId = a.id, removedId = b.id;
    *getClip(sequence, trackId, mergedId) = std::move(merged);
    removeClip(sequence, trackId, removedId);
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return mergedId;
}

std::string ofxOceanodeTimelineManager::splitClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double timelineBeat) {
    auto* track = getTrack(sequence, trackId);
    const auto* sourceClip = getClip(sequence, trackId, clipId);
    if(track == nullptr || sourceClip == nullptr || track->isWaveTrack) return std::string();
    const ofxOceanodeTimelineClip original = *sourceClip;
    ofxOceanodeTimelineClip left, right;
    if(!computeClipSplit(original, timelineBeat, left, right)) return std::string();
    // Keep the original/left half as the track's capture destination. The
    // right half is an ordinary editable curve clip after the split.
    right.isStateCapture = false;

    // Splitting dissolves a clip group: the halves become independent.
    if(getGroupForClip(sequence, trackId, clipId) != nullptr) ungroupClip(sequence, trackId, clipId);

    const std::string rightId = createClip(sequence, trackId, original.name, right.startBeat, right.durationBeats);
    if(rightId.empty()) return std::string();
    auto* rightClip = getClip(sequence, trackId, rightId);
    const std::string rightName = rightClip->name;
    auto rightLanes = std::move(right.lanes);
    right.lanes.clear();
    right.id = rightId;
    right.name = rightName;
    *rightClip = right;
    for(auto& lane : rightLanes) {
        const std::string laneId = sequence.makeUniqueLaneId();
        ++sequence.nextLaneNumber;
        lane.id = laneId;
        if(auto* clip = getClip(sequence, trackId, rightId)) clip->lanes.push_back(lane);
    }
    if(auto* leftClip = getClip(sequence, trackId, clipId)) *leftClip = left;

    if(auto* sortedTrack = getTrack(sequence, trackId)) {
        std::sort(sortedTrack->clips.begin(), sortedTrack->clips.end(), [](const auto& a, const auto& b) {
            if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
            return a.id < b.id;
        });
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return rightId;
}

std::string ofxOceanodeTimelineManager::duplicateClip(ofxOceanodeTimelineSequence& sequence, const std::string& srcTrackId, const std::string& clipId,
                                                      const std::string& dstTrackId, double newStartBeat,
                                                      const std::string& srcLaneId,
                                                      const std::string& dstBindingId) {
    const auto* srcTrack = getTrack(sequence, srcTrackId);
    const auto* dstTrack = getTrack(sequence, dstTrackId);
    const auto* sourceClip = getClip(sequence, srcTrackId, clipId);
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
        const auto* binding = getBinding(sequence, srcTrackId, bindingId);
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
        if(dstTrack->isWaveTrack || getBinding(sequence, dstTrackId, dstBindingId) == nullptr) return std::string();
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

    const std::string newId = createClip(sequence, dstTrackId, copy.name, newStartBeat, copy.durationBeats);
    auto* newClip = getClip(sequence, dstTrackId, newId);
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
        lane.id = sequence.makeUniqueLaneId();
        ++sequence.nextLaneNumber;
        if(auto* clip = getClip(sequence, dstTrackId, newId)) clip->lanes.push_back(std::move(lane));
    }
    invalidateSchedule();
    return newId;
}

int ofxOceanodeTimelineManager::separateStateCaptureClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                         const std::string& clipId) {
    const auto* source = getClip(sequence, trackId, clipId);
    const auto* track = getTrack(sequence, trackId);
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
        const auto newClipId = duplicateClip(sequence, trackId, clipId, trackId, startBeat,
                                             lanes[laneIndex].id,
                                             lanes[laneIndex].bindingIds.front());
        if(newClipId.empty()) {
            for(const auto& createdId : createdClipIds) removeClip(sequence, trackId, createdId);
            return 0;
        }
        if(getClip(sequence, trackId, newClipId) != nullptr) {
            createdClipIds.push_back(newClipId);
        } else {
            removeClip(sequence, trackId, newClipId);
            for(const auto& createdId : createdClipIds) removeClip(sequence, trackId, createdId);
            return 0;
        }
    }

    for(const auto& createdId : createdClipIds)
        if(auto* created = getClip(sequence, trackId, createdId)) created->isStateCapture = true;
    if(getGroupForClip(sequence, trackId, clipId) != nullptr) ungroupClip(sequence, trackId, clipId);
    if(auto* original = getClip(sequence, trackId, clipId)) {
        original->lanes = {lanes.front()};
        original->isStateCapture = true;
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return static_cast<int>(createdClipIds.size() + 1);
}

bool ofxOceanodeTimelineManager::trimClipStart(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double newStartBeat) {
    auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
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

bool ofxOceanodeTimelineManager::extendClipStart(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, double newStartBeat) {
    auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    if(track == nullptr || clip == nullptr || track->isWaveTrack) return false;
    newStartBeat = std::max(0.0, newStartBeat);
    if(newStartBeat >= clip->startBeat - 1e-9 || clip->isLfo) return trimClipStart(sequence, trackId, clipId, newStartBeat);
    bakeContentOffset(*clip);

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

bool ofxOceanodeTimelineManager::renameClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                            const std::string& requestedName) {
    auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    if(track == nullptr || clip == nullptr || requestedName.empty()) return false;
    if(clip->name == requestedName) return true;
    clip->name = ofxOceanodeTimelineSequence::makeUniqueClipName(*track, requestedName);
    return true;
}

bool ofxOceanodeTimelineManager::removeClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return false;
    if(const auto* clip = getClip(sequence, trackId, clipId)) {
        if(isMainSequence(sequence)) releaseWaveClipIfNeeded(trackId, clip->id);
        if(clip->isStateCapture) {
            for(const auto& lane : clip->lanes) {
                for(const auto& bindingId : lane.bindingIds) {
                    const std::string key = trackId + "\x1f" + bindingId;
                    if(isMainSequence(sequence) && stateCaptureManualOverrideKeys.erase(key) > 0) {
                        if(auto* binding = getBinding(sequence, trackId, bindingId)) {
                            binding->hasLiveOverride = false;
                            binding->liveOverrideValue.clear();
                        }
                    }
                    if(const auto* binding = isMainSequence(sequence) ? getBinding(sequence, trackId, bindingId) : nullptr)
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
        removeClipFromGroups(sequence, trackId, clipId);
        invalidateEvaluationIndexes();
    }
    return removed;
}

void ofxOceanodeTimelineManager::removeClipFromGroups(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    for(auto& group : sequence.clipGroups) {
        group.members.erase(std::remove_if(group.members.begin(), group.members.end(), [&](const auto& member) {
            return member.first == trackId && member.second == clipId;
        }), group.members.end());
    }
    // A "group" of zero or one member is meaningless -- drop it rather than
    // leave an inert record around that the UI's group outline/Ungroup
    // button would otherwise have to special-case.
    sequence.clipGroups.erase(std::remove_if(sequence.clipGroups.begin(), sequence.clipGroups.end(), [](const auto& group) {
        return group.members.size() < 2;
    }), sequence.clipGroups.end());
}

std::string ofxOceanodeTimelineManager::groupClips(ofxOceanodeTimelineSequence& sequence, const std::vector<std::pair<std::string, std::string>>& members) {
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
        if(getClip(sequence, member.first, member.second) == nullptr) continue; // stale reference -- skip rather than fail the whole call
        addUnique(member);
        if(const auto* existingGroup = getGroupForClip(sequence, member.first, member.second)) {
            groupIdsToDissolve.insert(existingGroup->id);
        }
    }
    for(const auto& group : sequence.clipGroups) {
        if(groupIdsToDissolve.count(group.id) == 0) continue;
        for(const auto& member : group.members) {
            // Groups are persisted as references. Do not carry a stale
            // reference into a newly merged group if a clip was deleted by
            // another path before the group metadata was refreshed.
            if(getClip(sequence, member.first, member.second) != nullptr) addUnique(member);
        }
    }
    // Do not dissolve an existing group when the requested selection did not
    // produce at least two live clips. In particular, a stale selection must
    // never make a valid group lose its only visible member.
    if(merged.size() < 2) {
        if(groupIdsToDissolve.size() == 1) return *groupIdsToDissolve.begin();
        return std::string();
    }
    sequence.clipGroups.erase(std::remove_if(sequence.clipGroups.begin(), sequence.clipGroups.end(), [&](const auto& group) {
        return groupIdsToDissolve.count(group.id) > 0;
    }), sequence.clipGroups.end());

    ofxOceanodeTimelineClipGroup newGroup;
    newGroup.id = sequence.makeUniqueGroupId();
    newGroup.members = std::move(merged);
    ++sequence.nextGroupNumber;
    sequence.clipGroups.push_back(std::move(newGroup));
    return sequence.clipGroups.back().id;
}

bool ofxOceanodeTimelineManager::ungroupClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    const auto it = std::find_if(sequence.clipGroups.begin(), sequence.clipGroups.end(), [&](const auto& group) {
        return std::any_of(group.members.begin(), group.members.end(), [&](const auto& member) {
            return member.first == trackId && member.second == clipId;
        });
    });
    if(it == sequence.clipGroups.end()) return false;
    sequence.clipGroups.erase(it);
    return true;
}

const ofxOceanodeTimelineClipGroup* ofxOceanodeTimelineManager::getGroupForClip(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) const {
    for(const auto& group : sequence.clipGroups) {
        for(const auto& member : group.members) {
            if(member.first == trackId && member.second == clipId) return &group;
        }
    }
    return nullptr;
}

ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) const {
    const auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

std::string ofxOceanodeTimelineManager::createLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                   const std::string& requestedName,
                                                   ofxOceanodeTimelineLaneType type) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return std::string();
    if(type == ofxOceanodeTimelineLaneType::Wave) return std::string();
    if(const auto* owner = getTrack(sequence, trackId); owner != nullptr && owner->isMacroTrack()) return std::string();
    ofxOceanodeTimelineLane lane;
    lane.id = sequence.makeUniqueLaneId();
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
    ++sequence.nextLaneNumber;
    clip->lanes.push_back(std::move(lane));
    return clip->lanes.back().id;
}

std::string ofxOceanodeTimelineManager::duplicateLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                       const std::string& laneId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr || clip->isLfo) return {};
    const auto source = std::find_if(clip->lanes.begin(), clip->lanes.end(),
                                     [&](const auto& lane) { return lane.id == laneId; });
    if(source == clip->lanes.end() || source->type == ofxOceanodeTimelineLaneType::Wave ||
       source->isWaveVolume || !source->lfoParameter.empty()) return {};
    ofxOceanodeTimelineLane copy = *source;
    copy.id = sequence.makeUniqueLaneId();
    // Its own rolls: a copy on the source's seed would pass and fail with it.
    copy.probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));
    ++sequence.nextLaneNumber;
    const auto newId = copy.id;
    clip->lanes.insert(std::next(source), std::move(copy));
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return newId;
}

bool ofxOceanodeTimelineManager::removeLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                            const std::string& laneId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return false;
    if(const auto* lane = getLane(sequence, trackId, clipId, laneId)) {
        if(isMainSequence(sequence) && lane->type == ofxOceanodeTimelineLaneType::Wave) releaseWaveClipIfNeeded(trackId, clipId);
        if(clip->isStateCapture) {
            for(const auto& bindingId : lane->bindingIds) {
                const std::string key = trackId + "\x1f" + bindingId;
                if(!isMainSequence(sequence) || stateCaptureManualOverrideKeys.erase(key) == 0) continue;
                if(auto* binding = getBinding(sequence, trackId, bindingId)) {
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

bool ofxOceanodeTimelineManager::setLaneMultiRowCount(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                      const std::string& laneId, int rowCount) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
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

bool ofxOceanodeTimelineManager::reloadWaveform(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId) {
    auto* clip = getClip(sequence, trackId, clipId);
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

bool ofxOceanodeTimelineManager::addBindingToLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                  const std::string& laneId, const std::string& bindingId) {
    auto* track = getTrack(sequence, trackId);
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(track == nullptr || lane == nullptr || getBinding(sequence, trackId, bindingId) == nullptr) return false;
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

bool ofxOceanodeTimelineManager::removeBindingFromLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                       const std::string& laneId, const std::string& bindingId) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
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
    const auto* clip = getClip(sequence, trackId, clipId);
    if(removed && clip != nullptr && clip->isStateCapture) {
        const std::string key = trackId + "\x1f" + bindingId;
        if(isMainSequence(sequence) && stateCaptureManualOverrideKeys.erase(key) > 0) {
            if(auto* binding = getBinding(sequence, trackId, bindingId)) {
                binding->hasLiveOverride = false;
                binding->liveOverrideValue.clear();
            }
        }
    }
    if(removed) invalidateSchedule();
    return removed;
}

ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                             const std::string& laneId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                                    const std::string& laneId) const {
    const auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](const auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

bool ofxOceanodeTimelineManager::setClipLaneType(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                 const std::string& laneId, ofxOceanodeTimelineLaneType type) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    auto* clip = getClip(sequence, trackId, clipId);
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
            if(!isMainSequence(sequence) || stateCaptureManualOverrideKeys.erase(key) == 0) continue;
            if(auto* binding = getBinding(sequence, trackId, bindingId)) {
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
    if(isMainSequence(sequence) && previousType == ofxOceanodeTimelineLaneType::Wave && type != ofxOceanodeTimelineLaneType::Wave)
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
            const auto* track = getTrack(sequence, trackId);
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

bool ofxOceanodeTimelineManager::setClipTiming(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                               double startBeat, double durationBeats) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return false;
    const double previousStart = clip->startBeat, previousDuration = clip->durationBeats;
    clip->startBeat = std::max(0.0, startBeat);
    clip->durationBeats = std::max(1.0 / 24.0, durationBeats);
    // Resizing a Wave clip stretches its complete source over the new length.
    const auto* track = getTrack(sequence, trackId);
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
    if(isTrackMutedWithFolders(track)) return false;
    // Without any solo (the usual case) skip the walk up each track's folders.
    const bool anySolo = std::any_of(mainSequence.tracks.begin(), mainSequence.tracks.end(),
                                     [this](const auto& other) { return isTrackSoloed(other); });
    return !anySolo || isTrackSoloedWithFolders(track);
}

bool ofxOceanodeTimelineManager::isTrackMutedWithFolders(const ofxOceanodeTimelineTrack& track) const {
    size_t steps = 0;
    for(const auto* at = &track; at != nullptr && steps <= mainSequence.tracks.size(); at = mainSequence.parentOf(*at), ++steps)
        if(at->muted) return true;
    return false;
}

bool ofxOceanodeTimelineManager::isTrackSoloedWithFolders(const ofxOceanodeTimelineTrack& track) const {
    size_t steps = 0;
    for(const auto* at = &track; at != nullptr && steps <= mainSequence.tracks.size(); at = mainSequence.parentOf(*at), ++steps)
        if(isTrackSoloed(*at)) return true;
    return false;
}

float ofxOceanodeTimelineManager::trackLatencyWithFolders(const ofxOceanodeTimelineTrack& track) const {
    float total = 0.0f;
    size_t steps = 0;
    for(const auto* at = &track; at != nullptr && steps <= mainSequence.tracks.size(); at = mainSequence.parentOf(*at), ++steps)
        total += at->latencyOffsetMs;
    return ofClamp(total, -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs);
}

std::string ofxOceanodeTimelineManager::createFolder(ofxOceanodeTimelineSequence& sequence, const std::string& requestedName,
                                                     const std::string& parentId) {
    const auto id = createTrack(sequence, requestedName.empty() ? "Folder" : requestedName);
    auto* folder = getTrack(sequence, id);
    folder->isFolder = true;
    const auto* parent = parentId.empty() ? nullptr : sequence.findTrack(parentId);
    folder->parentId = parent != nullptr && parent->isFolder ? parentId : std::string();
    sequence.normalizeFolders();
    invalidateEvaluationIndexes();
    return id;
}

bool ofxOceanodeTimelineManager::moveTrack(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                           const std::string& parentId, size_t index) {
    const std::string movedId = trackId, targetParent = parentId; // callers may pass ids that move
    auto* track = sequence.findTrack(movedId);
    if(track == nullptr) return false;
    if(!targetParent.empty()) {
        const auto* parent = sequence.findTrack(targetParent);
        if(parent == nullptr || !parent->isFolder || sequence.isWithin(targetParent, movedId)) return false;
    }
    sequence.normalizeFolders();
    // Take the track out with everything inside it.
    const size_t from = static_cast<size_t>(sequence.findTrack(movedId) - sequence.tracks.data());
    const size_t to = sequence.subtreeEnd(from);
    std::vector<ofxOceanodeTimelineTrack> block(std::make_move_iterator(sequence.tracks.begin() + from),
                                                std::make_move_iterator(sequence.tracks.begin() + to));
    sequence.tracks.erase(sequence.tracks.begin() + from, sequence.tracks.begin() + to);
    block.front().parentId = targetParent;
    // Insert before the index-th track of the new folder, or after its last one.
    std::vector<size_t> siblings;
    for(size_t i = 0; i < sequence.tracks.size(); ++i)
        if(sequence.tracks[i].parentId == targetParent) siblings.push_back(i);
    size_t insertAt;
    if(index < siblings.size()) insertAt = siblings[index];
    else if(!siblings.empty()) insertAt = sequence.subtreeEnd(siblings.back());
    else if(!targetParent.empty())
        insertAt = static_cast<size_t>(sequence.findTrack(targetParent) - sequence.tracks.data()) + 1;
    else insertAt = sequence.tracks.size();
    sequence.tracks.insert(sequence.tracks.begin() + insertAt,
                           std::make_move_iterator(block.begin()), std::make_move_iterator(block.end()));
    sequence.normalizeFolders();
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::removeFolder(ofxOceanodeTimelineSequence& sequence, const std::string& folderId, bool keepTracks) {
    const std::string removedId = folderId;
    const auto* folder = sequence.findTrack(removedId);
    if(folder == nullptr || !folder->isFolder) return false;
    const std::string outerParent = folder->parentId;
    if(keepTracks) {
        for(auto& track : sequence.tracks) if(track.parentId == removedId) track.parentId = outerParent;
    } else {
        // Innermost first, so every removal finds its track.
        std::vector<std::string> inside;
        for(const auto& track : sequence.tracks)
            if(track.id != removedId && sequence.isWithin(track.id, removedId)) inside.push_back(track.id);
        std::sort(inside.begin(), inside.end(), [&](const auto& a, const auto& b) {
            return sequence.depthOf(*sequence.findTrack(a)) > sequence.depthOf(*sequence.findTrack(b));
        });
        for(const auto& id : inside) removeTrack(sequence, id);
    }
    removeTrack(sequence, removedId);
    sequence.normalizeFolders();
    invalidateEvaluationIndexes();
    return true;
}

bool ofxOceanodeTimelineManager::setTrackMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, bool muted) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr || track->muted == muted) return false;
    track->muted = muted;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setTrackLatencyOffset(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, float milliseconds) {
    auto* track = getTrack(sequence, trackId);
    const float clamped = ofClamp(milliseconds, -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs);
    if(track == nullptr || track->latencyOffsetMs == clamped) return false;
    track->latencyOffsetMs = clamped;
    // Events already handed over carry the old offset: schedule them again.
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setTrackSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, bool solo) {
    auto* track = getTrack(sequence, trackId);
    if(track == nullptr) return false;
    // Setting it by hand replaces what a Toggle lane drove, until a lane
    // drives it again.
    const bool wasDriven = isMainSequence(sequence) && playback.drivenSolo.erase(PlayKey{0, trackId}) > 0;
    if(track->solo == solo && !wasDriven) return false;
    track->solo = solo;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneDrivesTrackSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                      const std::string& laneId, bool drives) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(lane == nullptr || lane->drivesTrackSolo == drives) return false;
    lane->drivesTrackSolo = drives;
    if(drives && container != nullptr) applyLaneTrackSolos(container->getTransportState().beatPosition);
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneTargetProbability(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                        const std::string& laneId, const std::string& parameterPath,
                                                        float probability) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
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
    // Mutes and solos set by hand are the user listening: a lane can't solo
    // a track they muted, or one their solo of another track leaves out.
    const bool handSolo = std::any_of(mainSequence.tracks.begin(), mainSequence.tracks.end(),
                                      [](const auto& track) { return track.solo; });
    for(auto& track : mainSequence.tracks) {
        if(track.isWaveTrack) continue;
        // Folders count: a solo set on a folder is the user's for all inside.
        const auto soloedByHand = [&](const ofxOceanodeTimelineTrack& at) {
            size_t steps = 0;
            for(const auto* t = &at; t != nullptr && steps <= mainSequence.tracks.size(); t = mainSequence.parentOf(*t), ++steps)
                if(t->solo) return true;
            return false;
        };
        if(isTrackMutedWithFolders(track) || (handSolo && !soloedByHand(track))) {
            const bool wasSoloed = isTrackSoloed(track);
            playback.drivenSolo.erase(PlayKey{0, track.id});
            if(isTrackSoloed(track) != wasSoloed) changed = true;
            continue;
        }
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

bool ofxOceanodeTimelineManager::setLaneMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                            const std::string& laneId, bool muted) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(lane == nullptr || lane->muted == muted) return false;
    lane->muted = muted;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setLaneSolo(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                           const std::string& laneId, bool solo) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(lane == nullptr || lane->solo == solo) return false;
    lane->solo = solo;
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setClipChance(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, float chance) {
    auto* clip = getClip(sequence, trackId, clipId);
    chance = ofClamp(chance, 0.0f, 1.0f);
    if(clip == nullptr || clip->chance == chance) return false;
    clip->chance = chance;
    // The roll reads the clip's chance, so it changes now; follow the
    // transport's pass as the next update would.
    if(container != nullptr) playContext = playContextFor(container->getTransportState());
    refreshPlaybackEnablement();
    return true;
}

bool ofxOceanodeTimelineManager::setClipMuted(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId, bool muted) {
    auto* clip = getClip(sequence, trackId, clipId);
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

bool ofxOceanodeTimelineManager::resizeClipEnd(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                               double duration, ofxOceanodeTimelineEndEdit operation) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr || !std::isfinite(duration)) return false;
    const double source = ofxOceanodeTimelineClipTime::sourceDuration(*clip);
    const double ratio = ofxOceanodeTimelineClipTime::effectiveStretch(*clip);
    duration = std::max(1.0 / 24.0, duration);
    setClipTiming(sequence, trackId, clipId, clip->startBeat, duration);
    const auto* track = getTrack(sequence, trackId);
    if(track != nullptr && track->isWaveTrack) {
        // Audio always stretches its full slice, independent of the global tool.
    } else if(operation == ofxOceanodeTimelineEndEdit::Repeat) {
        clip->contentStretch = ratio;
        setClipContentDuration(sequence, trackId, clipId, source, true);
    } else if(operation == ofxOceanodeTimelineEndEdit::Trim) {
        setClipContentDuration(sequence, trackId, clipId, duration / ratio, false);
    } else {
        setClipContentDuration(sequence, trackId, clipId, source, false);
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::setClipContentDuration(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                         double contentDurationBeats, bool repeatContent) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return false;
    // A new content length re-lays the content: start it where it plays from.
    if(std::abs(std::max(1.0 / 24.0, contentDurationBeats) - clip->contentDurationBeats) > 1e-9) bakeContentOffset(*clip);
    clip->contentDurationBeats = std::max(1.0 / 24.0, contentDurationBeats);
    clip->repeatContent = repeatContent;
    const auto* track = getTrack(sequence, trackId);
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

bool ofxOceanodeTimelineManager::setClipContentOffset(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                      const std::string& clipId, double offsetBeats) {
    const auto* track = getTrack(sequence, trackId);
    auto* clip = getClip(sequence, trackId, clipId);
    if(track == nullptr || clip == nullptr || track->isWaveTrack || clip->isLfo || !std::isfinite(offsetBeats)) return false;
    double offset = positiveModulo(offsetBeats, ofxOceanodeTimelineClipTime::sourceDuration(*clip));
    if(offset >= ofxOceanodeTimelineClipTime::sourceDuration(*clip) - 1e-9) offset = 0.0;
    if(std::abs(offset - clip->contentOffsetBeats) <= 1e-9) return false;
    clip->contentOffsetBeats = offset;
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return true;
}

bool ofxOceanodeTimelineManager::consolidateClipContent(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                        const std::string& clipId) {
    auto* clip = getClip(sequence, trackId, clipId);
    if(clip == nullptr) return false;
    bakeContentOffset(*clip);

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

bool ofxOceanodeTimelineManager::setClipNumber(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                              const std::string& laneId, double beat, float value) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
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

bool ofxOceanodeTimelineManager::setClipStep(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                             const std::string& laneId, double startBeat,
                                             const std::string& value, double durationBeats) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(lane == nullptr || lane->type != ofxOceanodeTimelineLaneType::Step) return false;
    lane->step.setStep(startBeat, value, durationBeats);
    return true;
}

bool ofxOceanodeTimelineManager::removeClipStep(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& clipId,
                                                const std::string& laneId, double startBeat) {
    auto* lane = getLane(sequence, trackId, clipId, laneId);
    if(lane == nullptr || lane->type != ofxOceanodeTimelineLaneType::Step) return false;
    return lane->step.removeStep(startBeat);
}

ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId) {
    auto* track = getTrack(sequence, trackId);
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

const ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(const ofxOceanodeTimelineSequence& sequence, const std::string& trackId, const std::string& bindingId) const {
    const auto* track = getTrack(sequence, trackId);
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
} // namespace

std::string ofxOceanodeTimelineManager::addMarker(double beat, const std::string& name) {
    ofxOceanodeTimelineMarker marker;
    uint64_t number = markers.size() + 1;
    do {
        marker.id = ofxOceanodeTimelineSequence::makeId("timeline_marker", number++);
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
} // namespace

void ofxOceanodeTimelineManager::adoptInstrumentDurations() {
    if(container == nullptr || replayingTimelineEdit) return;
    struct Adoption { std::string trackId, groupId; const KitRole* role; ofxOceanodeTimelineParameterBinding binding; };
    std::vector<Adoption> adopted;
    for(size_t trackIndex = 0; trackIndex < mainSequence.tracks.size(); ++trackIndex) {
        if(mainSequence.tracks[trackIndex].isWaveTrack) continue;
        for(size_t groupIndex = 0; groupIndex < mainSequence.tracks[trackIndex].noteGroups.size(); ++groupIndex) {
            for(const KitRole* role : {&kDurationRole, &kFilterRole, &kStutterRole}) {
                auto& track = mainSequence.tracks[trackIndex];
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
                auto& liveTrack = mainSequence.tracks[trackIndex];
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
        std::swap(mainSequence.tracks, state.tracks);
        const auto tracksJson = toJson()["tracks"];
        std::swap(mainSequence.tracks, state.tracks);
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
            for(auto& track : mainSequence.tracks) {
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
        for(auto& track : mainSequence.tracks) {
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
    releaseSilencedRests(transport);

    // Hand the discrete events of the coming window to any backend that can
    // execute them at an exact instant. Everything below still runs exactly
    // as before for this frame's own position.
    runScheduler(transport, loopWrappedThisFrame);

    // Wave tracks never touch the automation binding map. They are reported
    // through the optional audio backend as clip sources instead.
    if(waveAudioProvider != nullptr) {
        for(const auto& placement : playedSequences(transport.beatPosition, transport.beatPosition)) {
            // Voices are keyed by track and clip id: copies of a sequence would
            // share them, so only the main timeline plays Wave clips for now.
            if(placement.instanceKey != 0) continue;
            const double beat = placement.map.toSequence(transport.beatPosition);
            for(const auto& track : placement.sequence->tracks) {
                if(!track.isWaveTrack) continue;
                for(const auto& clip : track.clips) {
                    // Wave clips always map their complete source linearly over
                    // the visible clip, independent of repeatContent and of the
                    // cached contentStretch. Position and rate are derived from
                    // the same ratio so the synth's audio-rate read head reaches
                    // the slice end exactly when the playhead reaches the clip end.
                    const double visibleDuration = std::max(1.0 / 24.0, clip.durationBeats);
                    const double sourceDuration = std::max(1.0 / 24.0, clip.contentDurationBeats);
                    const double sourcePerBeat = sourceDuration / visibleDuration;
                    // Read faster where the sequence plays faster (its map's ratio).
                    const double sourcePerTimelineBeat = sourcePerBeat / placement.map.ratio;
                    const double localBeat = beat - clip.startBeat;
                    const bool active = isTrackAudible(track) && !clip.muted && !isClipChanceSilenced(clip) && localBeat >= -kEpsilon && localBeat < visibleDuration - kEpsilon;
                    const double waveSourceBeat = active
                        ? ofClamp(localBeat * sourcePerBeat, 0.0, sourceDuration) : 0.0;
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

void ofxOceanodeTimelineManager::clear() {
    if(!replayingTimelineEdit) clearTimelineHistory();
    // Correct anything already queued on a backend while the bindings that
    // know the right values still exist (afterwards a queued gate-on would
    // play with nothing to turn it off).
    invalidateSchedule();
    for(const auto& track : mainSequence.tracks) clearTimelineFlag(track);
    endParameterCache();
    mainSequence.tracks.clear();
    invalidateEvaluationIndexes();
    mainSequence.clipGroups.clear();
    markers.clear();
    viewState = {};
    playOnPresetLoad = false;
    timebase = ofxOceanodeTimelineTimebase::Beats;
    mainSequence.nextTrackNumber = 1;
    mainSequence.nextBindingNumber = 1;
    mainSequence.nextClipNumber = 1;
    mainSequence.nextLaneNumber = 1;
    mainSequence.nextGroupNumber = 1;
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

    for(auto& track : mainSequence.tracks) {
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


// ---------------------------------------------------------------------------
// The same editing operations on the main timeline
// ---------------------------------------------------------------------------

std::string ofxOceanodeTimelineManager::createTrack(const std::string& requestedName) {
    return createTrack(mainSequence, requestedName);
}

std::string ofxOceanodeTimelineManager::insertTrackCopy(ofxOceanodeTimelineTrack copy, const std::string& name, size_t index) {
    return insertTrackCopy(mainSequence, copy, name, index);
}

std::string ofxOceanodeTimelineManager::duplicateTrack(const std::string& trackId) {
    return duplicateTrack(mainSequence, trackId);
}

std::vector<std::pair<std::string, std::string>> ofxOceanodeTimelineManager::breakClipLanesIntoTracks(const std::string& trackId, const std::string& clipId) {
    return breakClipLanesIntoTracks(mainSequence, trackId, clipId);
}

std::string ofxOceanodeTimelineManager::createWaveTrack(const std::string& requestedName) {
    return createWaveTrack(mainSequence, requestedName);
}

std::string ofxOceanodeTimelineManager::createWaveVolumeLane(const std::string& trackId, const std::string& clipId) {
    return createWaveVolumeLane(mainSequence, trackId, clipId);
}

std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(const std::string& trackId) {
    return getWaveTrackVolumePoints(mainSequence, trackId);
}

const std::vector<ofxOceanodeTimelineCurvePoint>& ofxOceanodeTimelineManager::getWaveTrackVolumePoints(const std::string& trackId) const {
    return getWaveTrackVolumePoints(mainSequence, trackId);
}

bool ofxOceanodeTimelineManager::addWaveTrackVolumePoint(const std::string& trackId, double beat, float value) {
    return addWaveTrackVolumePoint(mainSequence, trackId, beat, value);
}

bool ofxOceanodeTimelineManager::removeWaveTrackVolumePoint(const std::string& trackId, double beat, double epsilon) {
    return removeWaveTrackVolumePoint(mainSequence, trackId, beat, epsilon);
}

bool ofxOceanodeTimelineManager::renameTrack(const std::string& trackId, const std::string& requestedName) {
    return renameTrack(mainSequence, trackId, requestedName);
}

bool ofxOceanodeTimelineManager::removeTrack(const std::string& trackId) {
    return removeTrack(mainSequence, trackId);
}

ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(const std::string& trackId) {
    return getTrack(mainSequence, trackId);
}

const ofxOceanodeTimelineTrack* ofxOceanodeTimelineManager::getTrack(const std::string& trackId) const {
    return getTrack(mainSequence, trackId);
}

std::string ofxOceanodeTimelineManager::addBinding(const std::string& trackId, ofxOceanodeAbstractParameter& parameter, ofxOceanodeTimelineAutomationMode mode) {
    return addBinding(mainSequence, trackId, parameter, mode);
}

bool ofxOceanodeTimelineManager::bindClipTarget(const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::string& path, ofxOceanodeTimelineTargetRole role) {
    return bindClipTarget(mainSequence, trackId, clipId, laneId, path, role);
}

bool ofxOceanodeTimelineManager::moveBinding(const std::string& trackId, const std::string& bindingId, int newIndex) {
    return moveBinding(mainSequence, trackId, bindingId, newIndex);
}

bool ofxOceanodeTimelineManager::setBindingOrder(const std::string& trackId, const std::vector<std::string>& orderedBindingIds) {
    return setBindingOrder(mainSequence, trackId, orderedBindingIds);
}

ofxOceanodeTimelineNoteGroup* ofxOceanodeTimelineManager::getNoteGroup(const std::string& trackId, const std::string& groupId) {
    return getNoteGroup(mainSequence, trackId, groupId);
}

std::string ofxOceanodeTimelineManager::createNoteGroup(const std::string& trackId, const std::string& name, const std::string& pitchBindingId, const std::string& gateBindingId, const std::string& velocityBindingId) {
    return createNoteGroup(mainSequence, trackId, name, pitchBindingId, gateBindingId, velocityBindingId);
}

bool ofxOceanodeTimelineManager::setNoteGroupRoles(const std::string& trackId, const std::string& groupId, const std::string& pitchBindingId, const std::string& gateBindingId, const std::string& velocityBindingId) {
    return setNoteGroupRoles(mainSequence, trackId, groupId, pitchBindingId, gateBindingId, velocityBindingId);
}

bool ofxOceanodeTimelineManager::setNoteGroupKitRole(const std::string& trackId, const std::string& groupId, const std::string& bindingId, int which) {
    return setNoteGroupKitRole(mainSequence, trackId, groupId, bindingId, which);
}

bool ofxOceanodeTimelineManager::removeNoteGroup(const std::string& trackId, const std::string& groupId, bool removeBindings) {
    return removeNoteGroup(mainSequence, trackId, groupId, removeBindings);
}

std::string ofxOceanodeTimelineManager::createNoteGroupClip(const std::string& trackId, const std::string& groupId, const std::string& requestedName, double startBeat, double durationBeats, std::string* laneIdOut, ofxOceanodeTimelineLaneType laneType) {
    return createNoteGroupClip(mainSequence, trackId, groupId, requestedName, startBeat, durationBeats, laneIdOut, laneType);
}

int ofxOceanodeTimelineManager::retargetParameterPaths(const std::map<std::string, std::string>& oldToNew) {
    return retargetParameterPaths(mainSequence, oldToNew);
}

bool ofxOceanodeTimelineManager::removeBinding(const std::string& trackId, const std::string& bindingId) {
    return removeBinding(mainSequence, trackId, bindingId);
}

bool ofxOceanodeTimelineManager::setBindingMode(const std::string& trackId, const std::string& bindingId, ofxOceanodeTimelineAutomationMode mode) {
    return setBindingMode(mainSequence, trackId, bindingId, mode);
}

bool ofxOceanodeTimelineManager::setBindingClamp(const std::string& trackId, const std::string& bindingId, bool clampToParameterRange) {
    return setBindingClamp(mainSequence, trackId, bindingId, clampToParameterRange);
}

int ofxOceanodeTimelineManager::captureStateAsCurvePoints(const std::string& trackId, const std::string& bindingId, double timelineBeat) {
    return captureStateAsCurvePoints(mainSequence, trackId, bindingId, timelineBeat);
}

std::string ofxOceanodeTimelineManager::createClip(const std::string& trackId, const std::string& requestedName, double startBeat, double durationBeats) {
    return createClip(mainSequence, trackId, requestedName, startBeat, durationBeats);
}

std::string ofxOceanodeTimelineManager::createLfoClip(const std::string& trackId, const std::string& outputBindingId, const std::string& requestedName, double startBeat, double durationBeats) {
    return createLfoClip(mainSequence, trackId, outputBindingId, requestedName, startBeat, durationBeats);
}

void ofxOceanodeTimelineManager::ensureLfoLanes(const std::string& trackId, const std::string& clipId) {
    return ensureLfoLanes(mainSequence, trackId, clipId);
}

std::string ofxOceanodeTimelineManager::splitClip(const std::string& trackId, const std::string& clipId, double timelineBeat) {
    return splitClip(mainSequence, trackId, clipId, timelineBeat);
}

std::string ofxOceanodeTimelineManager::duplicateClip(const std::string& srcTrackId, const std::string& clipId, const std::string& dstTrackId, double newStartBeat, const std::string& srcLaneId, const std::string& dstBindingId) {
    return duplicateClip(mainSequence, srcTrackId, clipId, dstTrackId, newStartBeat, srcLaneId, dstBindingId);
}

int ofxOceanodeTimelineManager::separateStateCaptureClip(const std::string& trackId, const std::string& clipId) {
    return separateStateCaptureClip(mainSequence, trackId, clipId);
}

bool ofxOceanodeTimelineManager::trimClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat) {
    return trimClipStart(mainSequence, trackId, clipId, newStartBeat);
}

bool ofxOceanodeTimelineManager::extendClipStart(const std::string& trackId, const std::string& clipId, double newStartBeat) {
    return extendClipStart(mainSequence, trackId, clipId, newStartBeat);
}

bool ofxOceanodeTimelineManager::renameClip(const std::string& trackId, const std::string& clipId, const std::string& requestedName) {
    return renameClip(mainSequence, trackId, clipId, requestedName);
}

bool ofxOceanodeTimelineManager::removeClip(const std::string& trackId, const std::string& clipId) {
    return removeClip(mainSequence, trackId, clipId);
}

void ofxOceanodeTimelineManager::removeClipFromGroups(const std::string& trackId, const std::string& clipId) {
    return removeClipFromGroups(mainSequence, trackId, clipId);
}

std::string ofxOceanodeTimelineManager::groupClips(const std::vector<std::pair<std::string, std::string>>& members) {
    return groupClips(mainSequence, members);
}

bool ofxOceanodeTimelineManager::ungroupClip(const std::string& trackId, const std::string& clipId) {
    return ungroupClip(mainSequence, trackId, clipId);
}

const ofxOceanodeTimelineClipGroup* ofxOceanodeTimelineManager::getGroupForClip(const std::string& trackId, const std::string& clipId) const {
    return getGroupForClip(mainSequence, trackId, clipId);
}

ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(const std::string& trackId, const std::string& clipId) {
    return getClip(mainSequence, trackId, clipId);
}

const ofxOceanodeTimelineClip* ofxOceanodeTimelineManager::getClip(const std::string& trackId, const std::string& clipId) const {
    return getClip(mainSequence, trackId, clipId);
}

std::string ofxOceanodeTimelineManager::createLane(const std::string& trackId, const std::string& clipId, const std::string& requestedName, ofxOceanodeTimelineLaneType type) {
    return createLane(mainSequence, trackId, clipId, requestedName, type);
}

std::string ofxOceanodeTimelineManager::duplicateLane(const std::string& trackId, const std::string& clipId, const std::string& laneId) {
    return duplicateLane(mainSequence, trackId, clipId, laneId);
}

bool ofxOceanodeTimelineManager::removeLane(const std::string& trackId, const std::string& clipId, const std::string& laneId) {
    return removeLane(mainSequence, trackId, clipId, laneId);
}

bool ofxOceanodeTimelineManager::setLaneMultiRowCount(const std::string& trackId, const std::string& clipId, const std::string& laneId, int rowCount) {
    return setLaneMultiRowCount(mainSequence, trackId, clipId, laneId, rowCount);
}

bool ofxOceanodeTimelineManager::reloadWaveform(const std::string& trackId, const std::string& clipId) {
    return reloadWaveform(mainSequence, trackId, clipId);
}

bool ofxOceanodeTimelineManager::addBindingToLane(const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::string& bindingId) {
    return addBindingToLane(mainSequence, trackId, clipId, laneId, bindingId);
}

bool ofxOceanodeTimelineManager::removeBindingFromLane(const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::string& bindingId) {
    return removeBindingFromLane(mainSequence, trackId, clipId, laneId, bindingId);
}

ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(const std::string& trackId, const std::string& clipId, const std::string& laneId) {
    return getLane(mainSequence, trackId, clipId, laneId);
}

const ofxOceanodeTimelineLane* ofxOceanodeTimelineManager::getLane(const std::string& trackId, const std::string& clipId, const std::string& laneId) const {
    return getLane(mainSequence, trackId, clipId, laneId);
}

bool ofxOceanodeTimelineManager::setClipLaneType(const std::string& trackId, const std::string& clipId, const std::string& laneId, ofxOceanodeTimelineLaneType type) {
    return setClipLaneType(mainSequence, trackId, clipId, laneId, type);
}

bool ofxOceanodeTimelineManager::setClipTiming(const std::string& trackId, const std::string& clipId, double startBeat, double durationBeats) {
    return setClipTiming(mainSequence, trackId, clipId, startBeat, durationBeats);
}

bool ofxOceanodeTimelineManager::setTrackMuted(const std::string& trackId, bool muted) {
    return setTrackMuted(mainSequence, trackId, muted);
}

bool ofxOceanodeTimelineManager::setTrackLatencyOffset(const std::string& trackId, float milliseconds) {
    return setTrackLatencyOffset(mainSequence, trackId, milliseconds);
}

bool ofxOceanodeTimelineManager::setTrackSolo(const std::string& trackId, bool solo) {
    return setTrackSolo(mainSequence, trackId, solo);
}

bool ofxOceanodeTimelineManager::setLaneDrivesTrackSolo(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool drives) {
    return setLaneDrivesTrackSolo(mainSequence, trackId, clipId, laneId, drives);
}

bool ofxOceanodeTimelineManager::setLaneTargetProbability(const std::string& trackId, const std::string& clipId, const std::string& laneId, const std::string& parameterPath, float probability) {
    return setLaneTargetProbability(mainSequence, trackId, clipId, laneId, parameterPath, probability);
}

bool ofxOceanodeTimelineManager::setLaneMuted(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool muted) {
    return setLaneMuted(mainSequence, trackId, clipId, laneId, muted);
}

bool ofxOceanodeTimelineManager::setLaneSolo(const std::string& trackId, const std::string& clipId, const std::string& laneId, bool solo) {
    return setLaneSolo(mainSequence, trackId, clipId, laneId, solo);
}

bool ofxOceanodeTimelineManager::setClipChance(const std::string& trackId, const std::string& clipId, float chance) {
    return setClipChance(mainSequence, trackId, clipId, chance);
}

bool ofxOceanodeTimelineManager::setClipMuted(const std::string& trackId, const std::string& clipId, bool muted) {
    return setClipMuted(mainSequence, trackId, clipId, muted);
}

bool ofxOceanodeTimelineManager::resizeClipEnd(const std::string& trackId, const std::string& clipId, double duration, ofxOceanodeTimelineEndEdit operation) {
    return resizeClipEnd(mainSequence, trackId, clipId, duration, operation);
}

bool ofxOceanodeTimelineManager::setClipContentDuration(const std::string& trackId, const std::string& clipId, double contentDurationBeats, bool repeatContent) {
    return setClipContentDuration(mainSequence, trackId, clipId, contentDurationBeats, repeatContent);
}

bool ofxOceanodeTimelineManager::consolidateClipContent(const std::string& trackId, const std::string& clipId) {
    return consolidateClipContent(mainSequence, trackId, clipId);
}

bool ofxOceanodeTimelineManager::setClipNumber(const std::string& trackId, const std::string& clipId, const std::string& laneId, double beat, float value) {
    return setClipNumber(mainSequence, trackId, clipId, laneId, beat, value);
}

bool ofxOceanodeTimelineManager::setClipStep(const std::string& trackId, const std::string& clipId, const std::string& laneId, double startBeat, const std::string& value, double durationBeats) {
    return setClipStep(mainSequence, trackId, clipId, laneId, startBeat, value, durationBeats);
}

bool ofxOceanodeTimelineManager::removeClipStep(const std::string& trackId, const std::string& clipId, const std::string& laneId, double startBeat) {
    return removeClipStep(mainSequence, trackId, clipId, laneId, startBeat);
}

ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(const std::string& trackId, const std::string& bindingId) {
    return getBinding(mainSequence, trackId, bindingId);
}

const ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineManager::getBinding(const std::string& trackId, const std::string& bindingId) const {
    return getBinding(mainSequence, trackId, bindingId);
}

bool ofxOceanodeTimelineManager::setClipContentOffset(const std::string& trackId, const std::string& clipId, double offsetBeats) {
    return setClipContentOffset(mainSequence, trackId, clipId, offsetBeats);
}

std::string ofxOceanodeTimelineManager::createFolder(const std::string& requestedName, const std::string& parentId) {
    return createFolder(mainSequence, requestedName, parentId);
}

bool ofxOceanodeTimelineManager::moveTrack(const std::string& trackId, const std::string& parentId, size_t index) {
    return moveTrack(mainSequence, trackId, parentId, index);
}

bool ofxOceanodeTimelineManager::removeFolder(const std::string& folderId, bool keepTracks) {
    return removeFolder(mainSequence, folderId, keepTracks);
}

std::string ofxOceanodeTimelineManager::mergeClips(const std::string& trackId, const std::string& firstClipId,
                                                   const std::string& secondClipId) {
    return mergeClips(mainSequence, trackId, firstClipId, secondClipId);
}
