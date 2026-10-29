// Scheduler: hands discrete changes ahead of the playhead to timestamping
// backends, and corrects them after seeks and edits.

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
    // Macro timelines first: on a path both drive, the macro timeline's
    // offset decides. Inside one, the macro tracks' around it add up.
    for(const auto* sequence : evaluatedSequences()) for(const auto& track : sequence->tracks) {
        // A track's own offset plus its folders' (see trackLatencyWithFolders).
        float latencyMs = trackLatencyWithFolders(track);
        for(const auto& played : playedMacros)
            if(played.sequence.get() == sequence)
                latencyMs = ofClamp(latencyMs + played.outerLatencyMs, -kMaxTrackLatencyOffsetMs, kMaxTrackLatencyOffsetMs);
        if(latencyMs == 0.0f) continue;
        const int64_t offsetUs = static_cast<int64_t>(std::llround(
            latencyMs * 1000.0));
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
