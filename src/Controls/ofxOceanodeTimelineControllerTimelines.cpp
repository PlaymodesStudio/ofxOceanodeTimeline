// The editor's timelines: which one it shows (any canvas's, any of its
// timelines), the player bar of one that plays on its own, the timelines
// menu, and the further Timeline windows the main controller hosts.

#include "ofxOceanodeTimelineController.h"

#include "Managers/ofxOceanodeContainer.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;

namespace {
bool rootCanvas(const std::string& canvas) { return canvas.empty() || canvas == "Canvas" || canvas == "0"; }
bool isMain(const TimelineRef& ref) { return rootCanvas(ref.canvas) && ref.timeline.empty(); }
bool sameRef(TimelineRef a, TimelineRef b) {
    if(rootCanvas(a.canvas)) a.canvas.clear();
    if(rootCanvas(b.canvas)) b.canvas.clear();
    return a == b;
}
const char* kQuantizeLabels[] = {"Now", "Next beat", "Next bar"};
} // namespace

ofxOceanodeTimelineController::ofxOceanodeTimelineController(std::shared_ptr<ofxOceanodeContainer> container, int window)
: ofxOceanodeBaseController("Timeline " + std::to_string(window)), container(std::move(container)), windowId(window) {}

void ofxOceanodeTimelineController::enterTimelineScope(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    if(isMain(ref)) { leaveScopesTo(0); return; }
    auto* sequence = timeline.timelineFor(ref);
    auto* canvas = timeline.canvasContainer(ref.canvas);
    if(sequence == nullptr || canvas == nullptr) return;
    // Already shown.
    if(!scopes.empty() && sameRef(scopes.back().ref, ref)) return;
    // A canvas's further timeline replaces the stack (a macro's: on top of
    // the macro's own timeline).
    if(!scopes.empty() && !scopes.back().ref.timeline.empty()) leaveScopesTo(scopes.size() - 1);
    if(rootCanvas(ref.canvas)) leaveScopesTo(0);
    else enterMacroScope(timeline, *canvas);
    if(ref.timeline.empty()) return;
    scopes.push_back({sequence, canvas, timeline.timelineName(ref), ref});
    resetEditorForScope();
}

void ofxOceanodeTimelineController::drawTimelinesBar(ofxOceanodeTimelineManager& timeline) {
    ImGui::PushID("timelinesBar");
    drawTimelinesMenu(timeline);
    ImGui::PopID();
    // Rename, from the menu.
    if(requestTimelineRename) {
        ImGui::OpenPopup("Rename timeline##timelineRename");
        requestTimelineRename = false;
    }
    if(ImGui::BeginPopupModal("Rename timeline##timelineRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Name##timelineRenameInput", timelineNameBuffer, sizeof(timelineNameBuffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if(enter || ImGui::Button("Rename")) {
            timeline.renameTimeline(timelineRenameRef, timelineNameBuffer);
            for(auto& scope : scopes) if(sameRef(scope.ref, timelineRenameRef)) scope.label = timelineNameBuffer;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void ofxOceanodeTimelineController::drawTimelinesMenu(ofxOceanodeTimelineManager& timeline) {
    const auto shown = scopeRef();
    const std::string current = timeline.timelineName(shown) + (timeline.isFree(shown) && !isMain(shown) ? "  (plays on its own)" : "");
    ImGui::SetNextItemWidth(std::min(260.0f, std::max(140.0f, ImGui::CalcTextSize(current.c_str()).x + 30.0f)));
    if(ImGui::BeginCombo("##timelinesMenu", current.c_str(), ImGuiComboFlags_HeightLarge)) {
        // Every timeline, grouped by canvas: pick one to show it here.
        std::string group = "\x01";
        for(const auto& ref : timeline.allTimelines()) {
            const std::string canvas = rootCanvas(ref.canvas) ? std::string("Main canvas") : ref.canvas;
            if(canvas != group) {
                group = canvas;
                ImGui::Separator();
                ImGui::TextDisabled("%s", group.c_str());
            }
            std::string label = "  " + timeline.timelineName(ref);
            if(timeline.isFree(ref) && !isMain(ref)) {
                auto transport = timeline.transportFor(ref);
                label += transport != nullptr && transport->getState().isPlaying ? "   [playing]" : "   [on its own]";
            }
            label += "##" + ofxOceanodeTimelineManager::timeSourceId(ref);
            if(ImGui::Selectable(label.c_str(), sameRef(ref, shown))) enterTimelineScope(timeline, ref);
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("The timeline shown here. Every canvas can own several timelines;\n"
                          "any of them can play on its own or be placed in another as clips.");
    ImGui::SameLine(0.0f, 2.0f);
    if(ImGui::Button("...##timelineActions")) ImGui::OpenPopup("##timelineActionsMenu");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New, duplicate, rename, delete, place, open in a window");
    if(ImGui::BeginPopup("##timelineActionsMenu")) {
        const std::string canvas = rootCanvas(shown.canvas) ? std::string() : shown.canvas;
        if(ImGui::MenuItem("New timeline on this canvas")) {
            const auto id = timeline.createTimeline(canvas, std::string());
            if(!id.empty()) enterTimelineScope(timeline, {canvas, id});
        }
        if(ImGui::MenuItem("Duplicate this timeline")) {
            const auto id = timeline.duplicateTimeline(shown);
            if(!id.empty()) enterTimelineScope(timeline, {canvas, id});
        }
        if(ImGui::MenuItem("Rename...", nullptr, false, !isMain(shown))) {
            timelineRenameRef = shown;
            std::snprintf(timelineNameBuffer, sizeof(timelineNameBuffer), "%s", timeline.timelineName(shown).c_str());
            requestTimelineRename = true;
        }
        if(ImGui::MenuItem("Delete this timeline", nullptr, false, !shown.timeline.empty())) {
            const auto removed = shown;
            leaveScopesTo(scopes.empty() ? 0 : scopes.size() - 1);
            timeline.removeTimeline(removed);
        }
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && shown.timeline.empty())
            ImGui::SetTooltip("A canvas's default timeline goes with its canvas.");
        ImGui::Separator();
        // Timelines of this canvas or of canvases inside it, never one that plays this one.
        if(ImGui::BeginMenu("Place a timeline here as clips")) {
            bool any = false;
            for(const auto& ref : timeline.allTimelines()) {
                const bool inside = rootCanvas(canvas) || sameRef({ref.canvas, std::string()}, {canvas, std::string()}) ||
                                    ref.canvas.rfind(canvas + " / ", 0) == 0;
                if(!inside || sameRef(ref, shown) || timeline.timelineReaches(ref, shown)) continue;
                any = true;
                const std::string label = (rootCanvas(ref.canvas) ? std::string() : ref.canvas + " > ") +
                                          timeline.timelineName(ref) + "##place" + ofxOceanodeTimelineManager::timeSourceId(ref);
                if(ImGui::MenuItem(label.c_str())) {
                    auto& host = scope(timeline);
                    beginHistoryGesture(timeline);
                    const auto trackId = timeline.createTimelineTrack(host, ref);
                    if(!trackId.empty()) {
                        auto* placed = timeline.timelineFor(ref);
                        const double length = placed == nullptr ? timeline.getBeatsPerBar() : timeline.macroTimelineLength(*placed);
                        const auto clipId = timeline.createClip(host, trackId, timeline.timelineName(ref), 0.0, length);
                        if(auto* clip = timeline.getClip(host, trackId, clipId)) {
                            clip->contentDurationBeats = length;
                            clip->contentStretch = 1.0;
                            clip->repeatContent = true;
                        }
                    }
                    finishHistoryGesture(timeline);
                }
            }
            if(!any) ImGui::TextDisabled("No other timeline can be placed here.");
            ImGui::EndMenu();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("A track whose clips play that timeline: place, repeat, stretch and offset them like any clip.");
        if(ImGui::MenuItem("Open in a new window")) timeline.openTimelineWindow(shown);
        ImGui::EndPopup();
    }
}

void ofxOceanodeTimelineController::drawPlayerBar(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    const auto* current = timeline.playSettings(ref);
    if(current == nullptr) return;
    auto settings = *current;
    bool changed = false;
    bool free = timeline.isFree(ref);
    if(ImGui::Checkbox("Plays on its own##free", &free)) timeline.setFree(ref, free);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("On: this timeline has its own transport -- play, stop and seek it by itself,\n"
                          "at its own tempo, alongside the main timeline (nodes can follow it as a Time Source).\n"
                          "Off: it plays only where clips of another timeline place it.");
    if(!free) {
        ImGui::SameLine();
        ImGui::TextDisabled("plays where clips place it");
        return;
    }
    auto transport = timeline.transportFor(ref);
    if(transport == nullptr) { ImGui::SameLine(); ImGui::TextDisabled("starting..."); return; }
    const auto state = transport->getState();
    double pendingClock = 0.0;
    bool pendingStarts = false;
    const bool pending = transport->nextPendingAction(pendingClock, pendingStarts);
    ImGui::SameLine(0.0f, 8.0f);
    if(ImGui::Button("|<##toStart")) timeline.seekTimeline(ref, 0.0);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Back to its start");
    ImGui::SameLine(0.0f, 2.0f);
    if(ImGui::Button("Stop##stop")) timeline.stopTimeline(ref, true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Stop and go back to its start (on the quantize below)");
    ImGui::SameLine(0.0f, 2.0f);
    const bool playing = state.isPlaying;
    if(playing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.3f, 1.0f));
    if(ImGui::Button(playing ? "Pause##play" : (pending && pendingStarts ? "Armed##play" : "Play##play"))) {
        if(playing) timeline.stopTimeline(ref, false);
        else if(pending) transport->cancelPendingActions();
        else timeline.playTimeline(ref);
    }
    if(playing) ImGui::PopStyleColor();
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(pending ? "Waiting for its quantized instant (click to cancel)" : "Play or pause it (Space while this timeline is shown)");
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(84.0f);
    if(ImGui::Combo("##quantize", &settings.quantize, kQuantizeLabels, 3)) changed = true;
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("When Play and Stop take effect: at once, or on the next beat or bar\n"
                          "of the main timeline while it plays (else of the clock), so it starts in phase.");
    ImGui::SameLine(0.0f, 8.0f);
    {
        const double beatsPerBar = std::max(1e-6, settings.timeSignatureNumerator * (4.0 / std::max(1, settings.timeSignatureDenominator)));
        const double beat = std::max(0.0, state.beatPosition);
        const int bar = static_cast<int>(std::floor(beat / beatsPerBar)) + 1;
        const double inBar = beat - (bar - 1) * beatsPerBar;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(playing ? ImVec4(0.47f, 0.92f, 0.63f, 1.0f) : ImVec4(0.9f, 0.9f, 0.93f, 1.0f),
                           "%3d. %d   %.1f BPM", bar, static_cast<int>(std::floor(inBar)) + 1, state.bpm);
    }
    ImGui::SameLine(0.0f, 8.0f);
    // Tempo: a fixed BPM, a ratio of the clock's, or its tempo lane.
    const char* tempoModes[] = {"BPM", "x Clock", "Lane"};
    int tempoMode = settings.bpmAutomationEnabled ? 2 : settings.followClockTempo ? 1 : 0;
    ImGui::SetNextItemWidth(70.0f);
    if(ImGui::Combo("##tempoMode", &tempoMode, tempoModes, 3)) {
        settings.bpmAutomationEnabled = tempoMode == 2;
        settings.followClockTempo = tempoMode == 1;
        if(tempoMode == 2 && settings.bpmPoints.empty()) settings.bpmPoints.push_back({0.0, settings.bpm});
        changed = true;
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("BPM: its own fixed tempo.\nx Clock: the clock's tempo times a ratio (follows it).\n"
                          "Lane: its own tempo automation (edit its points).");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(78.0f);
    if(tempoMode == 0) {
        if(ImGui::DragFloat("##bpm", &settings.bpm, 0.1f, 1.0f, 999.0f, "%.1f BPM")) changed = true;
    } else if(tempoMode == 1) {
        if(ImGui::DragFloat("##ratio", &settings.tempoRatio, 0.005f, 0.01f, 64.0f, "x %.3f")) changed = true;
    } else {
        if(ImGui::Button("Points...##tempoPoints")) ImGui::OpenPopup("##tempoPointsPopup");
        drawTempoPointsPopup(timeline, ref);
        settings = *timeline.playSettings(ref); // the popup may have changed them
    }
    ImGui::SameLine(0.0f, 8.0f);
    if(ImGui::Checkbox("Loop##loop", &settings.loopEnabled)) changed = true;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!settings.loopEnabled);
    ImGui::SetNextItemWidth(48.0f);
    if(ImGui::InputDouble("##loopStart", &settings.loopStartBeat, 0.0, 0.0, "%.3g")) changed = true;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(48.0f);
    if(ImGui::InputDouble("##loopEnd", &settings.loopEndBeat, 0.0, 0.0, "%.3g")) changed = true;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Loop start and end (beats)");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(28.0f);
    if(ImGui::DragInt("##meterNum", &settings.timeSignatureNumerator, 0.1f, 1, 64)) changed = true;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(34.0f);
    const std::string denominator = std::to_string(settings.timeSignatureDenominator);
    if(ImGui::BeginCombo("##meterDen", denominator.c_str(), ImGuiComboFlags_NoArrowButton)) {
        for(int option : {1, 2, 4, 8, 16, 32})
            if(ImGui::Selectable(std::to_string(option).c_str(), option == settings.timeSignatureDenominator)) {
                settings.timeSignatureDenominator = option;
                changed = true;
            }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Its time signature");
    if(changed) timeline.setPlaySettings(ref, settings);
}

void ofxOceanodeTimelineController::drawTempoPointsPopup(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    if(!ImGui::BeginPopup("##tempoPointsPopup")) return;
    const auto* current = timeline.playSettings(ref);
    if(current == nullptr) { ImGui::EndPopup(); return; }
    auto settings = *current;
    bool changed = false;
    const char* interpolations[] = {"Step", "Linear", "Log / Exp", "Sigmoid"};
    int interpolation = 1;
    for(int i = 0; i < 4; ++i) if(settings.bpmInterpolation == interpolations[i]) interpolation = i;
    ImGui::SetNextItemWidth(110.0f);
    if(ImGui::Combo("Interpolation", &interpolation, interpolations, 4)) {
        settings.bpmInterpolation = interpolations[interpolation];
        changed = true;
    }
    ImGui::TextDisabled("Beat            BPM");
    int remove = -1;
    for(size_t i = 0; i < settings.bpmPoints.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        auto& point = settings.bpmPoints[i];
        ImGui::SetNextItemWidth(90.0f);
        if(ImGui::InputDouble("##beat", &point.beat, 0.0, 0.0, "%.3g")) changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if(ImGui::DragFloat("##value", &point.value, 0.1f, settings.bpmMinimum, settings.bpmMaximum, "%.1f")) changed = true;
        ImGui::SameLine();
        if(ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if(remove >= 0) {
        settings.bpmPoints.erase(settings.bpmPoints.begin() + remove);
        changed = true;
    }
    if(ImGui::Button("Add point")) {
        const double beat = settings.bpmPoints.empty() ? 0.0 : settings.bpmPoints.back().beat +
            settings.timeSignatureNumerator * (4.0 / std::max(1, settings.timeSignatureDenominator));
        const float value = settings.bpmPoints.empty() ? settings.bpm : settings.bpmPoints.back().value;
        settings.bpmPoints.push_back({beat, value});
        changed = true;
    }
    if(changed) timeline.setPlaySettings(ref, settings);
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::drawTimelineWindows() {
    if(secondary() || container == nullptr) return;
    auto& timeline = container->getTimelineManager();
    const auto windows = timeline.getTimelineWindows(); // closing one edits the list
    for(auto it = windowEditors.begin(); it != windowEditors.end();) {
        const bool listed = std::any_of(windows.begin(), windows.end(), [&](const auto& window) { return window.id == it->first; });
        it = listed ? std::next(it) : windowEditors.erase(it);
    }
    // Docked with the main Timeline window the first time.
    ImGuiID dock = 0;
    if(auto* main = ImGui::FindWindowByName(getControllerName().c_str())) dock = main->DockId;
    for(const auto& window : windows) {
        auto& editor = windowEditors[window.id];
        if(editor == nullptr) editor = std::make_unique<ofxOceanodeTimelineController>(container, window.id);
        const std::string title = "Timeline: " + timeline.timelineName(window.ref) + "###timelineWindow" + std::to_string(window.id);
        bool open = true;
        if(dock != 0) ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900.0f, 420.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoFocusOnAppearing)) {
            // Its own zoom and scroll: what the editors read while it draws.
            std::swap(timeline.getViewState(), editor->ownViewState);
            editor->draw();
            std::swap(timeline.getViewState(), editor->ownViewState);
        }
        ImGui::End();
        if(!open) timeline.closeTimelineWindow(window.id);
    }
}

void ofxOceanodeTimelineController::seekShownTimeline(ofxOceanodeTimelineManager& timeline, double beat) {
    const auto shown = scopeRef();
    if(inMacroScope() && timeline.isFree(shown)) { timeline.seekTimeline(shown, beat); return; }
    if(auto transport = container->getTransport()) transport->seekToBeat(beat);
}

void ofxOceanodeTimelineController::requestPlacedScope(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track) {
    auto* placed = timeline.placedTimeline(scope(timeline), track);
    if(placed == nullptr || !timeline.refOf(*placed, pendingScope)) return;
    pendingScopeRequested = true;
}
