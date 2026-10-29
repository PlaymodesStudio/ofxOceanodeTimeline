// Persistence: timeline.json, clip files and presets.

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
} // namespace

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
    for(const auto& track : mainSequence.tracks) {
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
            if(clip.contentOffsetBeats != 0.0) clipJson["contentOffsetBeats"] = clip.contentOffsetBeats;
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
    for(const auto& group : mainSequence.clipGroups) {
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
} // namespace

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
            candidate = ofxOceanodeTimelineSequence::makeId(prefix, counter++);
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
        track.id = uniqueLoadedId(originalTrackId, "timeline_track", mainSequence.nextTrackNumber, loadedTrackIds);
        if(!originalTrackId.empty()) loadedTrackIdRemap[originalTrackId] = track.id;
        track.name = mainSequence.makeUniqueTrackName(trackJson.value("name", std::string("Timeline Track")));
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
                binding.id = uniqueLoadedId(savedBindingId, "timeline_binding", mainSequence.nextBindingNumber, loadedBindingIds);
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
                clip.id = uniqueLoadedId(originalClipId, "timeline_clip", mainSequence.nextClipNumber, loadedClipIds);
                if(!originalClipId.empty()) loadedClipIdRemap[originalClipId] = clip.id;
                clip.name = ofxOceanodeTimelineSequence::makeUniqueClipName(track, clipJson.value("name", std::string("Clip")));
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
                // Older files have no offset: their content starts at its start.
                clip.contentOffsetBeats = clip.isLfo || track.isWaveTrack ? 0.0 : clipJson.value("contentOffsetBeats", 0.0);
                clip.contentOffsetBeats = ofxOceanodeTimelineClipTime::contentOffset(clip);
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
                                                 "timeline_lane", mainSequence.nextLaneNumber, loadedLaneIds);
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

        mainSequence.tracks.push_back(std::move(track));
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
    mainSequence.nextTrackNumber = mainSequence.tracks.size() + 1;
    mainSequence.nextBindingNumber = 1;
    mainSequence.nextClipNumber = 1;
    mainSequence.nextLaneNumber = 1;
    for(const auto& track : mainSequence.tracks) {
        mainSequence.nextTrackNumber = nextAfter(track.id, mainSequence.nextTrackNumber);
        for(const auto& binding : track.bindings) mainSequence.nextBindingNumber = nextAfter(binding.id, mainSequence.nextBindingNumber);
        for(const auto& clip : track.clips) {
            mainSequence.nextClipNumber = nextAfter(clip.id, mainSequence.nextClipNumber);
            for(const auto& lane : clip.lanes) mainSequence.nextLaneNumber = nextAfter(lane.id, mainSequence.nextLaneNumber);
        }
    }

    // Rebuild waveform caches after loading, once ids and clip storage are
    // final. Legacy Wave lanes were migrated to their containing clip above.
    for(auto& track : mainSequence.tracks) {
        for(auto& clip : track.clips) {
            if(track.isWaveTrack && !clip.waveFilePath.empty()) reloadWaveform(track.id, clip.id);
        }
    }

    if(json.contains("clipGroups") && json["clipGroups"].is_array()) {
        for(const auto& groupJson : json["clipGroups"]) {
            if(!groupJson.is_object()) continue;
            ofxOceanodeTimelineClipGroup group;
            group.id = uniqueLoadedId(groupJson.value("id", std::string()),
                                      "timeline_group", mainSequence.nextGroupNumber, loadedGroupIds);
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
            if(group.members.size() >= 2) mainSequence.clipGroups.push_back(std::move(group));
        }
    }
    mainSequence.nextGroupNumber = mainSequence.clipGroups.size() + 1;
    for(const auto& group : mainSequence.clipGroups) mainSequence.nextGroupNumber = nextAfter(group.id, mainSequence.nextGroupNumber);

    // Give loaded LFO clips any control lane they are missing (collect ids first:
    // adding lanes must not happen while iterating the clip containers).
    std::vector<std::pair<std::string, std::string>> lfoClipIds;
    for(const auto& track : mainSequence.tracks) {
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
} // namespace

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
    isolated.mainSequence.tracks.push_back(std::move(portable));
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
        if(isolated.mainSequence.tracks.size() != 1 || isolated.mainSequence.tracks.front().clips.size() != 1) {
            clipFileError(error, "The clip contents could not be read."); return ofJson();
        }
        document = isolated.exportClip(isolated.mainSequence.tracks.front().id, isolated.mainSequence.tracks.front().clips.front().id);
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
        if(isolated.mainSequence.tracks.size() != 1 || isolated.mainSequence.tracks.front().clips.size() != 1) {
            clipFileError(error, "The clip contents could not be read."); return {};
        }
        auto source = std::move(isolated.mainSequence.tracks.front());
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
            lane.id = mainSequence.makeUniqueLaneId(); ++mainSequence.nextLaneNumber;
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
