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

// ---- The scope bar: which timeline this window shows ----
// One row at the top of the window, tinted by the kind of timeline shown:
// a kind badge, the path to it (Main > Macro 1 > ...), the timelines menu
// and its actions, then the shown timeline's player when it is not the
// main one. The main timeline's transport, tempo and loop stay in the
// toolbar below.
namespace {
enum class ScopeKind { Main, Macro, Further };
ScopeKind scopeKindOf(const TimelineRef& ref) {
    if(isMain(ref)) return ScopeKind::Main;
    return ref.timeline.empty() ? ScopeKind::Macro : ScopeKind::Further;
}
ImU32 scopeColor(ScopeKind kind) {
    switch(kind) {
        case ScopeKind::Main: return IM_COL32(80, 130, 190, 255);
        case ScopeKind::Macro: return IM_COL32(160, 100, 210, 255);
        case ScopeKind::Further: return IM_COL32(50, 165, 150, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}
const char* scopeBadge(ScopeKind kind) {
    switch(kind) {
        case ScopeKind::Main: return "MAIN";
        case ScopeKind::Macro: return "MACRO";
        case ScopeKind::Further: return "TIMELINE";
    }
    return "";
}
const char* scopeExplanation(ScopeKind kind, bool free) {
    switch(kind) {
        case ScopeKind::Main: return "The main timeline: the root transport, tempo and loop in the toolbar.";
        case ScopeKind::Macro: return free
            ? "This macro's own timeline, saved with the macro.\nIndependent: it plays on its own transport (and wherever macro clips place it)."
            : "This macro's own timeline, saved with the macro.\nIt plays where macro clips place it, at the host's tempo.";
        case ScopeKind::Further: return free
            ? "A further timeline of this canvas.\nIndependent: it plays on its own transport (and wherever clips place it)."
            : "A further timeline of this canvas.\nIt plays where clips of another timeline place it, at the host's tempo.";
    }
    return "";
}
ImU32 withAlpha(ImU32 color, int alpha) { return (color & 0x00FFFFFF) | (static_cast<ImU32>(alpha) << 24); }
} // namespace

void ofxOceanodeTimelineController::drawScopeBar(ofxOceanodeTimelineManager& timeline) {
    const auto shown = scopeRef();
    const ScopeKind kind = scopeKindOf(shown);
    const bool free = !isMain(shown) && timeline.isFree(shown);
    const ImU32 accent = scopeColor(kind);
    // The band is filled on channel 0 once the row's height is known.
    auto* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::PushID("scopeBar");
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    const float pad = 4.0f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad);
    const float top = ImGui::GetCursorScreenPos().y - pad;
    const float rowHeight = ImGui::GetFrameHeight();
    // Kind badge.
    {
        const char* badge = scopeBadge(kind);
        const ImVec2 textSize = ImGui::CalcTextSize(badge);
        const ImVec2 size(textSize.x + 14.0f, rowHeight);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), accent, 3.0f);
        dl->AddText(ImVec2(pos.x + 7.0f, pos.y + (size.y - textSize.y) * 0.5f), IM_COL32(15, 18, 24, 255), badge);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scopeExplanation(kind, free));
    }
    // The path to it: every timeline it is inside, each a way back (which
    // ends the row: the scopes just changed).
    ImGui::SameLine(0.0f, 8.0f);
    bool left = false;
    if(!scopes.empty()) {
        if(ImGui::SmallButton("Main##scopeMain")) { leaveScopesTo(0); left = true; }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the main timeline");
        for(size_t depth = 0; !left && depth + 1 < scopes.size(); ++depth) {
            ImGui::SameLine(0.0f, 2.0f); ImGui::TextDisabled(">"); ImGui::SameLine(0.0f, 2.0f);
            if(ImGui::SmallButton((scopes[depth].label + "##scope" + ofToString(depth)).c_str())) {
                leaveScopesTo(depth + 1);
                left = true;
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Back to this timeline");
        }
        if(!left) { ImGui::SameLine(0.0f, 2.0f); ImGui::TextDisabled(">"); ImGui::SameLine(0.0f, 4.0f); }
    }
    if(!left) {
        drawTimelinesMenu(timeline);
        if(!isMain(shown)) {
            ImGui::SameLine(0.0f, 10.0f);
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                dl->AddLine(ImVec2(pos.x, pos.y + 2.0f), ImVec2(pos.x, pos.y + rowHeight - 2.0f), IM_COL32(255, 255, 255, 38), 1.0f);
                ImGui::Dummy(ImVec2(1.0f, rowHeight));
            }
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::PushID("playerBar");
            drawPlayerBar(timeline, shown);
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    {
        const float bottom = ImGui::GetItemRectMax().y + pad;
        const float bandLeft = ImGui::GetWindowPos().x;
        const float bandRight = bandLeft + ImGui::GetWindowWidth();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(ImVec2(bandLeft, top), ImVec2(bandRight, bottom), IM_COL32(22, 23, 27, 255));
        dl->AddRectFilled(ImVec2(bandLeft, top), ImVec2(bandRight, bottom), withAlpha(accent, kind == ScopeKind::Main ? 18 : 34));
        dl->AddRectFilled(ImVec2(bandLeft, top), ImVec2(bandLeft + 4.0f, bottom), accent);
        dl->AddLine(ImVec2(bandLeft, bottom), ImVec2(bandRight, bottom), withAlpha(accent, 120), 1.0f);
        dl->ChannelsMerge();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad);
    }
    drawTimelineRenamePopup(timeline);
}

void ofxOceanodeTimelineController::drawTimelineRenamePopup(ofxOceanodeTimelineManager& timeline) {
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
    const std::string current = timeline.timelineName(shown);
    ImGui::SetNextItemWidth(std::min(260.0f, std::max(120.0f, ImGui::CalcTextSize(current.c_str()).x + 30.0f)));
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
                label += transport != nullptr && transport->getState().isPlaying ? "   [playing]" : "   [independent]";
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
                    if(!trackId.empty()) timeline.createMacroClip(host, trackId, 0.0);
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

// The shown timeline's player, compact: whether it is independent, its
// transport while it is, its own tempo, and a settings popup for the rest
// (loop range, meter, quantize).
void ofxOceanodeTimelineController::drawPlayerBar(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    const auto* current = timeline.playSettings(ref);
    if(current == nullptr) return;
    auto settings = *current;
    bool changed = false;
    bool free = timeline.isFree(ref);
    if(ImGui::Checkbox("Independent##free", &free)) timeline.setFree(ref, free);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("On: its own transport. Play, stop and seek it here, at its own tempo,\n"
                          "alongside the main timeline; nodes can follow it as a Time Source.\n"
                          "Off: it plays only where clips of another timeline place it.");
    auto transport = free ? timeline.transportFor(ref) : nullptr;
    if(free && transport == nullptr) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("starting...");
        return;
    }
    if(transport != nullptr) {
        const auto state = transport->getState();
        double pendingClock = 0.0;
        bool pendingStarts = false;
        const bool pending = transport->nextPendingAction(pendingClock, pendingStarts);
        ImGui::SameLine(0.0f, 8.0f);
        if(ImGui::Button("|<##toStart")) timeline.seekTimeline(ref, 0.0);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Back to its start");
        ImGui::SameLine(0.0f, 2.0f);
        if(ImGui::Button("Stop##stop")) timeline.stopTimeline(ref, true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Stop and go back to its start (quantized)");
        ImGui::SameLine(0.0f, 2.0f);
        const bool playing = state.isPlaying;
        const bool stopping = playing && pending && !pendingStarts;
        if(playing) ImGui::PushStyleColor(ImGuiCol_Button, stopping ? ImVec4(0.55f, 0.45f, 0.2f, 1.0f) : ImVec4(0.2f, 0.55f, 0.3f, 1.0f));
        if(ImGui::Button(stopping ? "Stopping##play" : playing ? "Pause##play" : (pending && pendingStarts ? "Armed##play" : "Play##play"))) {
            if(pending) transport->cancelPendingActions(); // a pending start or stop: cancelled
            else if(playing) timeline.stopTimeline(ref, false);
            else timeline.playTimeline(ref);
        }
        if(playing) ImGui::PopStyleColor();
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip(pending ? "Waiting for its quantized instant (click to cancel)" : "Play or pause it (Space while this timeline is shown)");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::SetNextItemWidth(84.0f);
        if(ImGui::Combo("##quantize", &settings.quantize, kQuantizeLabels, 3)) changed = true;
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("When Play and Stop take effect: at once, or on the next beat or bar\n"
                              "of the main timeline while it plays (else of the clock).");
        ImGui::SameLine(0.0f, 8.0f);
        {
            const double beatsPerBar = std::max(1e-6, settings.timeSignatureNumerator * (4.0 / std::max(1, settings.timeSignatureDenominator)));
            const double beat = std::max(0.0, state.beatPosition);
            const int bar = static_cast<int>(std::floor(beat / beatsPerBar)) + 1;
            const double inBar = beat - (bar - 1) * beatsPerBar;
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(playing ? ImVec4(0.47f, 0.92f, 0.63f, 1.0f) : ImVec4(0.9f, 0.9f, 0.93f, 1.0f),
                               "%d.%d  %.1f BPM", bar, static_cast<int>(std::floor(inBar)) + 1, state.bpm);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Its position (bar . beat) and tempo\nBeat %.3f", beat);
        }
    }
    // Its own tempo: a fixed BPM, a ratio of the clock's, or its tempo lane.
    ImGui::SameLine(0.0f, 10.0f);
    const char* tempoModes[] = {"BPM", "x Clock", "Lane"};
    int tempoMode = settings.bpmAutomationEnabled ? 2 : settings.followClockTempo ? 1 : 0;
    ImGui::SetNextItemWidth(66.0f);
    if(ImGui::Combo("##tempoMode", &tempoMode, tempoModes, 3)) {
        settings.bpmAutomationEnabled = tempoMode == 2;
        settings.followClockTempo = tempoMode == 1;
        if(tempoMode == 2 && settings.bpmPoints.empty()) settings.bpmPoints.push_back({0.0, settings.bpm});
        changed = true;
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", free ? "Its tempo.\nBPM: fixed. x Clock: the clock's tempo times a ratio. Lane: its own tempo points."
                                     : "Its own tempo, used by clips set to Keep its own tempo (placed, it otherwise\n"
                                       "follows the host). BPM: fixed. x Clock: the clock's tempo times a ratio. Lane: tempo points.");
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
    if(free) {
        ImGui::SameLine(0.0f, 8.0f);
        if(ImGui::Checkbox("Loop##loop", &settings.loopEnabled)) changed = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Loop %.3g to %.3g (set the range in the settings)", settings.loopStartBeat, settings.loopEndBeat);
    }
    ImGui::SameLine(0.0f, 4.0f);
    if(ImGui::Button("...##playerSettings")) ImGui::OpenPopup("##playerSettingsPopup");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Loop range and time signature");
    if(changed) timeline.setPlaySettings(ref, settings);
    drawPlayerSettingsPopup(timeline, ref);
}

void ofxOceanodeTimelineController::drawPlayerSettingsPopup(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    if(!ImGui::BeginPopup("##playerSettingsPopup")) return;
    const auto* current = timeline.playSettings(ref);
    if(current == nullptr) { ImGui::EndPopup(); return; }
    auto settings = *current;
    bool changed = false;
    ImGui::TextDisabled("%s", timeline.timelineName(ref).c_str());
    ImGui::Separator();
    if(ImGui::Checkbox("Loop", &settings.loopEnabled)) changed = true;
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::BeginDisabled(!settings.loopEnabled);
    ImGui::SetNextItemWidth(60.0f);
    if(ImGui::InputDouble("##loopStart", &settings.loopStartBeat, 0.0, 0.0, "%.3g")) changed = true;
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("-");
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetNextItemWidth(60.0f);
    if(ImGui::InputDouble("##loopEnd", &settings.loopEndBeat, 0.0, 0.0, "%.3g")) changed = true;
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextDisabled("beats");
    ImGui::EndDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Meter");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(32.0f);
    if(ImGui::DragInt("##meterNum", &settings.timeSignatureNumerator, 0.1f, 1, 64)) changed = true;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(40.0f);
    const std::string denominator = std::to_string(settings.timeSignatureDenominator);
    if(ImGui::BeginCombo("##meterDen", denominator.c_str(), ImGuiComboFlags_NoArrowButton)) {
        for(int option : {1, 2, 4, 8, 16, 32})
            if(ImGui::Selectable(std::to_string(option).c_str(), option == settings.timeSignatureDenominator)) {
                settings.timeSignatureDenominator = option;
                changed = true;
            }
        ImGui::EndCombo();
    }
    if(!timeline.isFree(ref)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Quantize");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetNextItemWidth(90.0f);
        if(ImGui::Combo("##quantize", &settings.quantize, kQuantizeLabels, 3)) changed = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("When its Play and Stop take effect once it is independent.");
    }
    if(changed) timeline.setPlaySettings(ref, settings);
    ImGui::EndPopup();
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

bool ofxOceanodeTimelineController::shownLoop(const ofxOceanodeTimelineManager& timeline, double& startBeat, double& endBeat) const {
    if(!inMacroScope()) {
        startBeat = timeline.getLoopStartBeat();
        endBeat = timeline.getLoopEndBeat();
        return timeline.isLoopEnabled();
    }
    const auto& play = scopes.back().sequence->play;
    startBeat = play.loopStartBeat;
    endBeat = play.loopEndBeat;
    return play.loopEnabled && timeline.isFree(scopeRef());
}

// Sets the range and turns the loop on.
void ofxOceanodeTimelineController::setShownLoop(ofxOceanodeTimelineManager& timeline, double startBeat, double endBeat) {
    if(!inMacroScope()) {
        if(!timeline.isLoopEnabled()) timeline.setLoopEnabled(true);
        timeline.setLoopRange(startBeat, endBeat);
        return;
    }
    const auto* current = timeline.playSettings(scopeRef());
    if(current == nullptr) return;
    auto settings = *current;
    settings.loopEnabled = true;
    settings.loopStartBeat = startBeat;
    settings.loopEndBeat = endBeat;
    timeline.setPlaySettings(scopeRef(), settings);
}

void ofxOceanodeTimelineController::requestPlacedScope(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track) {
    auto* placed = timeline.placedTimeline(scope(timeline), track);
    if(placed == nullptr || !timeline.refOf(*placed, pendingScope)) return;
    pendingScopeRequested = true;
}
