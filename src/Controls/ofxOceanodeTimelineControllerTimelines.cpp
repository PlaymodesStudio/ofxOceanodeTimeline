// The editor's timelines: which one it shows (any canvas's, any of its
// timelines), the player bar of one that plays on its own, the timelines
// menu, and the further Timeline windows the main controller hosts.

#include "ofxOceanodeTimelineController.h"

#include "Managers/ofxOceanodeContainer.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeColors.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

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
// its name on a badge that opens the navigator (every timeline as a tree),
// its actions, then its player when it is not the main one. The main timeline's transport, tempo and loop stay in the
// toolbar below.
namespace {
enum class ScopeKind { Main, Macro, Further };
ScopeKind scopeKindOf(const TimelineRef& ref) {
    if(isMain(ref)) return ScopeKind::Main;
    return ref.timeline.empty() ? ScopeKind::Macro : ScopeKind::Further;
}
ImU32 scopeColor(ScopeKind kind) {
    switch(kind) {
        case ScopeKind::Main: return IM_COL32(0, 0, 0, 255);
        case ScopeKind::Macro: return IM_COL32(160, 100, 210, 255);
        case ScopeKind::Further: return IM_COL32(50, 165, 150, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}
// A timeline's colour: its own when it has one, else its kind's.
ImU32 timelineAccent(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    ofColor color;
    return timeline.customTimelineColor(ref, color) ? IM_COL32(color.r, color.g, color.b, 255) : scopeColor(scopeKindOf(ref));
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
    const ImU32 accent = timelineAccent(timeline, shown);
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
    // The badge: the shown timeline's name on its colour. Click: the
    // navigator, every timeline as a tree, this one marked.
    {
        const std::string name = timeline.timelineName(shown);
        const ImVec2 textSize = ImGui::CalcTextSize(name.c_str());
        const float arrow = ImGui::GetFontSize() * 0.5f;
        const ImVec2 size(textSize.x + arrow + 20.0f, rowHeight);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if(ImGui::InvisibleButton("##timelinesNavigatorButton", size)) ImGui::OpenPopup("##timelinesNavigator");
        const bool hot = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##timelinesNavigator");
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), accent, 3.0f);
        if(hot) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(255, 255, 255, 30), 3.0f);
        // Dark text, light on a dark colour picked for it.
        const ImVec4 rgb = ImGui::ColorConvertU32ToFloat4(accent);
        const bool darkAccent = rgb.x * 0.299f + rgb.y * 0.587f + rgb.z * 0.114f < 0.45f;
        const ImU32 ink = darkAccent ? IM_COL32(235, 238, 245, 255) : IM_COL32(15, 18, 24, 255);
        dl->AddText(ImVec2(pos.x + 7.0f, pos.y + (size.y - textSize.y) * 0.5f), ink, name.c_str());
        ImGui::RenderArrow(dl, ImVec2(pos.x + size.x - arrow - 7.0f, pos.y + (size.y - arrow) * 0.5f), ink, ImGuiDir_Down, 0.5f);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nClick: every timeline, and where this one is.", scopeExplanation(kind, free));
        if(ImGui::BeginPopup("##timelinesNavigator")) {
            drawTimelinesTree(timeline, shown);
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine(0.0f, 4.0f);
    drawTimelineActions(timeline);
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
    centerNextModal();
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

// ---- New Timeline: its name, colour and owner, asked for every new one ----

void ofxOceanodeTimelineController::openNewTimelineDialog(ofxOceanodeTimelineManager& timeline,
                                                          const ofxOceanodeTimelineManager::NewTimelineRequest& request) {
    newTimelineRequest = request;
    if(request.duplicate) {
        newTimelineRequest.duplicateOf = ofxOceanodeTimelineManager::normalizedRef(request.duplicateOf);
        newTimelineOwners = {newTimelineRequest.duplicateOf.canvas}; // a copy stays on its canvas
    } else {
        newTimelineOwners = request.canvases.empty() ? timeline.allCanvases() : request.canvases;
    }
    for(auto& canvas : newTimelineOwners) if(rootCanvas(canvas)) canvas.clear();
    newTimelineOwnerLabels.clear();
    for(const auto& canvas : newTimelineOwners) {
        if(canvas.empty()) { newTimelineOwnerLabels.push_back("Main canvas"); continue; }
        // The macro's path, with its title when it has one of its own.
        auto* macro = timeline.canvasContainer(canvas);
        const std::string title = macro != nullptr ? timeline.getMacroSequenceLabel(*macro) : std::string();
        const auto last = canvas.rfind(" / ");
        const std::string tail = last == std::string::npos ? canvas : canvas.substr(last + 3);
        newTimelineOwnerLabels.push_back(title.empty() || title == tail ? canvas : canvas + "  (" + title + ")");
    }
    const std::string wanted = rootCanvas(request.canvas) ? std::string() : request.canvas;
    const auto at = std::find(newTimelineOwners.begin(), newTimelineOwners.end(), wanted);
    newTimelineOwner = at == newTimelineOwners.end() ? 0 : static_cast<int>(at - newTimelineOwners.begin());
    newTimelineNameEdited = false;
    newTimelineColorEdited = false;
    suggestNewTimelineDefaults(timeline);
    requestNewTimelinePopup = true;
}

void ofxOceanodeTimelineController::suggestNewTimelineDefaults(ofxOceanodeTimelineManager& timeline) {
    if(newTimelineOwners.empty()) return;
    const std::string& canvas = newTimelineOwners[newTimelineOwner];
    std::string name;
    ImU32 color = scopeColor(ScopeKind::Further);
    if(newTimelineRequest.duplicate) {
        const auto& source = newTimelineRequest.duplicateOf;
        name = timeline.timelineName(source) + " copy";
        ofColor sourceColor;
        if(timeline.customTimelineColor(source, sourceColor)) color = IM_COL32(sourceColor.r, sourceColor.g, sourceColor.b, 255);
    } else if(!timeline.timelineExists({canvas, std::string()})) {
        // A macro with no timeline yet: this one becomes its own.
        auto* macro = timeline.canvasContainer(canvas);
        name = macro != nullptr ? timeline.getMacroSequenceLabel(*macro) : canvas;
        color = scopeColor(ScopeKind::Macro);
    } else {
        // What createTimeline would call it (findTimeline: nothing made yet).
        const auto* base = timeline.findTimeline({canvas, std::string()});
        uint64_t number = base != nullptr ? std::max<uint64_t>(base->nextTimelineNumber, 2) : 2;
        if(base != nullptr)
            while(std::any_of(base->timelines.begin(), base->timelines.end(), [&](const auto& other) {
                return other->timelineId == "tl_" + std::to_string(number); })) ++number;
        name = "Timeline " + std::to_string(number);
    }
    if(!newTimelineNameEdited) std::snprintf(newTimelineName, sizeof(newTimelineName), "%s", name.c_str());
    if(!newTimelineColorEdited) {
        const ImVec4 rgb = ImGui::ColorConvertU32ToFloat4(color);
        newTimelineColor[0] = rgb.x;
        newTimelineColor[1] = rgb.y;
        newTimelineColor[2] = rgb.z;
    }
}

void ofxOceanodeTimelineController::drawNewTimelinePopup(ofxOceanodeTimelineManager& timeline) {
    ofxOceanodeTimelineManager::NewTimelineRequest request;
    if(timeline.consumeNewTimelineRequest(request)) openNewTimelineDialog(timeline, request);
    if(requestNewTimelinePopup) {
        ImGui::OpenPopup("New Timeline##newTimeline");
        requestNewTimelinePopup = false;
    }
    centerNextModal();
    if(!ImGui::BeginPopupModal("New Timeline##newTimeline", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(newTimelineOwners.empty()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const bool duplicate = newTimelineRequest.duplicate;
    if(duplicate) ImGui::TextDisabled("A copy of %s", timeline.timelineName(newTimelineRequest.duplicateOf).c_str());
    else if(newTimelineRequest.parameter != nullptr)
        ImGui::TextDisabled("With %s on its first track", newTimelineRequest.parameter->getName().c_str());
    const float fieldWidth = 320.0f;
    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(fieldWidth);
    const bool entered = ImGui::InputText("Name##newTimelineName", newTimelineName, sizeof(newTimelineName),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if(ImGui::IsItemEdited()) newTimelineNameEdited = true;
    if(ImGui::ColorEdit3("Color##newTimelineColor", newTimelineColor,
                         ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
        newTimelineColorEdited = true;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Its window's scope bar is tinted with it (change it later from the timelines menu).");
    if(newTimelineColorEdited) {
        ImGui::SameLine();
        if(ImGui::SmallButton("Default##newTimelineColorDefault")) {
            newTimelineColorEdited = false;
            suggestNewTimelineDefaults(timeline);
        }
    }
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::BeginDisabled(newTimelineOwners.size() < 2);
    if(ImGui::BeginCombo("Owner##newTimelineOwner", newTimelineOwnerLabels[newTimelineOwner].c_str(), ImGuiComboFlags_HeightLarge)) {
        for(int i = 0; i < static_cast<int>(newTimelineOwners.size()); ++i) {
            if(ImGui::Selectable((newTimelineOwnerLabels[i] + "##owner" + std::to_string(i)).c_str(), i == newTimelineOwner) &&
               i != newTimelineOwner) {
                newTimelineOwner = i;
                suggestNewTimelineDefaults(timeline);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(duplicate ? "A copy stays on the canvas of the timeline it copies."
                                    : "The canvas it belongs to: the main canvas or a macro's.\n"
                                      "A timeline drives what is inside its canvas.");
    const std::string& canvas = newTimelineOwners[newTimelineOwner];
    if(!duplicate && !timeline.timelineExists({canvas, std::string()}))
        ImGui::TextDisabled("It becomes this macro's own timeline (it has none yet).");
    const bool named = newTimelineName[0] != '\0';
    const bool ownerThere = timeline.canvasContainer(canvas) != nullptr;
    if(!ownerThere) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.4f, 1.0f), "That canvas is gone.");
    ImGui::Spacing();
    ImGui::BeginDisabled(!named || !ownerThere);
    const bool create = ImGui::Button(duplicate ? "Duplicate" : "Create") || (entered && named && ownerThere);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if(create && named && ownerThere) {
        TimelineRef made;
        if(duplicate) {
            const auto id = timeline.duplicateTimeline(newTimelineRequest.duplicateOf, newTimelineName);
            if(!id.empty()) made = {newTimelineRequest.duplicateOf.canvas, id};
        } else {
            made = timeline.addTimeline(canvas, newTimelineName);
        }
        if(!made.canvas.empty() || !made.timeline.empty()) { // {}: not made
            if(newTimelineColorEdited) {
                if(const auto* play = timeline.playSettings(made)) {
                    auto settings = *play;
                    settings.customColor = true;
                    settings.color = ofColor(static_cast<unsigned char>(ofClamp(newTimelineColor[0] * 255.0f, 0.0f, 255.0f)),
                                             static_cast<unsigned char>(ofClamp(newTimelineColor[1] * 255.0f, 0.0f, 255.0f)),
                                             static_cast<unsigned char>(ofClamp(newTimelineColor[2] * 255.0f, 0.0f, 255.0f)));
                    timeline.setPlaySettings(made, settings);
                }
            }
            if(auto* parameter = newTimelineRequest.parameter; parameter != nullptr && !duplicate)
                if(auto* sequence = timeline.timelineFor(made)) {
                    const auto track = timeline.createTrack(*sequence, parameter->getName());
                    timeline.addBinding(*sequence, track, *parameter);
                }
            if(newTimelineRequest.window != 0) timeline.setTimelineWindowRef(newTimelineRequest.window, made);
            else timeline.requestTimelineScope(made);
        }
        newTimelineRequest = {};
        ImGui::CloseCurrentPopup();
    } else if(cancel) {
        newTimelineRequest = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- The timelines tree: every canvas's timelines, nested as the canvases are ----
namespace {
// How a timeline plays: the root transport, its own (independent), where
// clips of another place it, or not at all.
enum class PlayMode { Main, Free, Clips, Idle };
const char* playModeLabel(PlayMode mode) {
    switch(mode) {
        case PlayMode::Main: return "Main";
        case PlayMode::Free: return "Free";
        case PlayMode::Clips: return "Clips";
        case PlayMode::Idle: return "Idle";
    }
    return "";
}
const char* playModeExplanation(PlayMode mode) {
    switch(mode) {
        case PlayMode::Main: return "Main: the root transport, tempo and loop.";
        case PlayMode::Free: return "Free: independent, on its own transport and tempo.";
        case PlayMode::Clips: return "Clips: plays where clips of another timeline place it, at that timeline's tempo\n"
                                     "(its own BPM only for clips set to keep it).";
        case PlayMode::Idle: return "Idle: not independent and placed nowhere, so it does not play.";
    }
    return "";
}
std::string parentPath(const std::string& canvas) {
    const auto at = canvas.rfind(" / ");
    return at == std::string::npos ? std::string() : canvas.substr(0, at);
}
} // namespace

void ofxOceanodeTimelineController::drawTimelinesTree(ofxOceanodeTimelineManager& timeline, const TimelineRef& shown) {
    // Every timeline under its owner canvas ("" the root), each canvas under
    // the one around it: the macros between them too, so the nesting is
    // the canvases' own.
    const auto refs = timeline.allTimelines();
    std::map<std::string, std::vector<TimelineRef>> timelinesOf; // in allTimelines' order: the default first
    std::map<std::string, std::vector<std::string>> childrenOf;
    std::set<std::string> known{std::string()};
    for(const auto& ref : refs) {
        const std::string canvas = rootCanvas(ref.canvas) ? std::string() : ref.canvas;
        timelinesOf[canvas].push_back({canvas, ref.timeline});
        for(std::string at = canvas; !at.empty() && known.insert(at).second; at = parentPath(at))
            childrenOf[parentPath(at)].push_back(at);
    }
    for(auto& entry : childrenOf) std::sort(entry.second.begin(), entry.second.end());
    // The timelines a timeline track of another places as clips.
    std::set<TimelineRef> placed;
    for(const auto& ref : refs) {
        const auto* sequence = timeline.findTimeline(ref);
        if(sequence == nullptr) continue;
        const std::string canvas = rootCanvas(ref.canvas) ? std::string() : ref.canvas;
        for(const auto& track : sequence->tracks) {
            if(!track.isMacroTrack()) continue;
            const std::string target = track.macroCanvas == "." ? canvas
                : canvas.empty() ? track.macroCanvas : canvas + " / " + track.macroCanvas;
            placed.insert({target, track.macroTimeline});
        }
    }
    const auto rootState = container->getTransportState();
    auto rootTransport = container->getTransport();

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
    // The Nodes list's stripes rather than the theme's table colours.
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, OceanodeColors::ZebraEven);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, OceanodeColors::ZebraOdd);
    // The header row in the windows' title colour.
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 3.0f));
    if(!ImGui::BeginTable("##timelinesTree", 5, tableFlags)) { ImGui::PopStyleVar(); ImGui::PopStyleColor(3); return; }
    ImGui::TableSetupColumn("Timeline");
    ImGui::TableSetupColumn("Mode");
    ImGui::TableSetupColumn("BPM");
    ImGui::TableSetupColumn("Meter");
    ImGui::TableSetupColumn("Sync");
    ImGui::TableHeadersRow();

    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4 playing(0.47f, 0.92f, 0.63f, 1.0f);
    bool picked = false;
    // One timeline's row: a leaf, picked with a click.
    auto timelineRow = [&](const TimelineRef& ref) {
        const bool main = isMain(ref);
        const bool free = !main && timeline.isFree(ref);
        const auto* play = main ? nullptr : timeline.playSettings(ref);
        auto transport = timeline.transportFor(ref);
        const bool isPlaying = transport != nullptr && transport->getState().isPlaying;
        const PlayMode mode = main ? PlayMode::Main : free ? PlayMode::Free
            : placed.count(ofxOceanodeTimelineManager::normalizedRef(ref)) > 0 ? PlayMode::Clips : PlayMode::Idle;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DrawLinesToNodes |
                                   ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if(sameRef(ref, shown)) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(("##" + ofxOceanodeTimelineManager::timeSourceId(ref)).c_str(), flags);
        if(ImGui::IsItemClicked()) {
            enterTimelineScope(timeline, ref);
            picked = true;
        }
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s%s", playModeExplanation(mode),
                              free && play != nullptr && play->playOnLoad ? "\nStarts on load, while its macro is active." : "");
        // Its colour, as its scope bar's, then its name.
        ImGui::SameLine(0.0f, 0.0f);
        {
            const ImU32 accent = timelineAccent(timeline, ref);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float height = ImGui::GetTextLineHeight();
            ImGui::Dummy(ImVec2(4.0f, height));
            ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 4.0f, pos.y + height), accent);
        }
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextUnformatted(timeline.timelineName(ref).c_str());

        ImGui::TableSetColumnIndex(1);
        if(isPlaying) ImGui::TextColored(playing, "%s", playModeLabel(mode));
        else if(mode == PlayMode::Idle) ImGui::TextDisabled("%s", playModeLabel(mode));
        else ImGui::TextUnformatted(playModeLabel(mode));

        // Tempo: the transport's now when it has one, else its own (only
        // clips keeping its tempo use it: dimmed).
        ImGui::TableSetColumnIndex(2);
        float bpm = rootState.bpm;
        if(transport != nullptr) bpm = transport->getState().bpm;
        else if(play != nullptr) {
            if(play->bpmAutomationEnabled && !play->bpmPoints.empty()) bpm = play->bpmPoints.front().value;
            else if(play->followClockTempo) bpm = rootState.bpm * play->tempoRatio;
            else bpm = play->bpm;
        }
        ImGui::TextColored(transport != nullptr ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : dim, "%.1f", bpm);

        ImGui::TableSetColumnIndex(3);
        if(main) ImGui::Text("%d/%d", timeline.getTimeSignatureNumerator(), timeline.getTimeSignatureDenominator());
        else if(play != nullptr) ImGui::Text("%d/%d", play->timeSignatureNumerator, play->timeSignatureDenominator);

        // Where its tempo comes from.
        ImGui::TableSetColumnIndex(4);
        if(main) {
            const char* sync = "Internal";
            if(rootTransport != nullptr && rootTransport->hasExternalClock()) {
                sync = "External";
#ifdef OFXOCEANODE_USE_MIDI
                if(container->isMidiClockSyncEnabled()) sync = "MIDI Clock";
#endif
            } else if(timeline.isBpmAutomationEnabled()) {
                sync = "Lane";
            }
            ImGui::TextUnformatted(sync);
        } else if(play != nullptr) {
            const char* sync = play->bpmAutomationEnabled ? "Lane" : play->followClockTempo ? "x Clock" : "Internal";
            if(free) ImGui::TextUnformatted(sync);
            else ImGui::TextDisabled("Host (%s)", sync);
        }
    };
    // A canvas: a group row (the main canvas, or the macro's title), its
    // timelines, then the canvases inside it.
    std::function<void(const std::string&)> canvasRows = [&](const std::string& canvas) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string label = "Main canvas";
        if(!canvas.empty()) {
            auto* macro = timeline.canvasContainer(canvas);
            const std::string parent = parentPath(canvas);
            label = macro != nullptr ? timeline.getMacroSequenceLabel(*macro)
                : parent.empty() ? canvas : canvas.substr(parent.size() + 3);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        const bool open = ImGui::TreeNodeEx(("##canvas " + canvas).c_str(), ImGuiTreeNodeFlags_SpanAllColumns |
                                            ImGuiTreeNodeFlags_DrawLinesToNodes | ImGuiTreeNodeFlags_DefaultOpen);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(canvas.empty() ? "The main canvas: its timelines, then the macros in it."
                                             : "A macro: its timelines, then the macros in it.");
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        if(!open) return;
        for(const auto& ref : timelinesOf[canvas]) timelineRow(ref);
        for(const auto& child : childrenOf[canvas]) canvasRows(child);
        ImGui::TreePop();
    };
    canvasRows(std::string());
    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if(picked) ImGui::CloseCurrentPopup();
}

// The shown timeline's actions, behind a "..." button.
void ofxOceanodeTimelineController::drawTimelineActions(ofxOceanodeTimelineManager& timeline) {
    const auto shown = scopeRef();
    if(ImGui::Button("...##timelineActions")) ImGui::OpenPopup("##timelineActionsMenu");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New, duplicate, rename, color, delete, place, open in a window");
    if(ImGui::BeginPopup("##timelineActionsMenu")) {
        const std::string canvas = rootCanvas(shown.canvas) ? std::string() : shown.canvas;
        // Both through the New Timeline dialog (name, colour, owner), shown in this window.
        if(ImGui::MenuItem("New timeline...")) {
            ofxOceanodeTimelineManager::NewTimelineRequest request;
            request.canvas = canvas;
            request.window = windowId;
            timeline.requestNewTimeline(request);
        }
        if(ImGui::MenuItem("Duplicate this timeline...")) {
            ofxOceanodeTimelineManager::NewTimelineRequest request;
            request.canvas = canvas;
            request.duplicateOf = shown;
            request.duplicate = true;
            request.window = windowId;
            timeline.requestNewTimeline(request);
        }
        if(ImGui::MenuItem("Rename...", nullptr, false, !isMain(shown))) {
            timelineRenameRef = shown;
            std::snprintf(timelineNameBuffer, sizeof(timelineNameBuffer), "%s", timeline.timelineName(shown).c_str());
            requestTimelineRename = true;
        }
        // The scope bar's colour for it (the main timeline's too).
        if(ImGui::BeginMenu("Color")) {
            const ImVec4 current = ImGui::ColorConvertU32ToFloat4(timelineAccent(timeline, shown));
            float rgb[3] = {current.x, current.y, current.z};
            if(ImGui::ColorPicker3("##timelineColor", rgb, ImGuiColorEditFlags_NoSidePreview))
                timeline.setTimelineColor(shown, ofColor(static_cast<unsigned char>(ofClamp(rgb[0] * 255.0f, 0.0f, 255.0f)),
                                                         static_cast<unsigned char>(ofClamp(rgb[1] * 255.0f, 0.0f, 255.0f)),
                                                         static_cast<unsigned char>(ofClamp(rgb[2] * 255.0f, 0.0f, 255.0f))));
            ofColor custom;
            if(ImGui::MenuItem("Default color", nullptr, false, timeline.customTimelineColor(shown, custom)))
                timeline.resetTimelineColor(shown);
            ImGui::EndMenu();
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

// The shown timeline's player, compact: whether it is independent (then the
// toolbar drives it), else its own tempo and a settings popup for the rest
// (loop range, meter, quantize), used once it is.
void ofxOceanodeTimelineController::drawPlayerBar(ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    const auto* current = timeline.playSettings(ref);
    if(current == nullptr) return;
    auto settings = *current;
    bool changed = false;
    bool free = timeline.isFree(ref);
    if(ImGui::Checkbox("Independent##free", &free)) timeline.setFree(ref, free);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("On: its own transport. The toolbar below plays, stops and seeks it, at its own\n"
                          "tempo, alongside the main timeline; nodes can follow it as a Time Source.\n"
                          "Off: it plays only where clips of another timeline place it.");
    if(free) {
        // Its transport, tempo, meter and loop are the toolbar's while it is shown.
        if(timeline.transportFor(ref) == nullptr) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("starting...");
        }
        return;
    }
    // Its own tempo, for clips set to keep it: a fixed BPM, a ratio of the
    // clock's, or its tempo lane.
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
        ImGui::SetTooltip("Its own tempo, used by clips set to Keep its own tempo (placed, it otherwise\n"
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
