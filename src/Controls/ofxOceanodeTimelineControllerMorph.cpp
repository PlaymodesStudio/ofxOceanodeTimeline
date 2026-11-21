// The editor's side of a Macro Morphing Track: the windows of its engine's
// routers, of one source and of one transition (the MacroMixer node's own
// panels, MacroMixerGui), the folder dialog of "Add Morphing Source", and
// the overlaps of its clips drawn over them.

#include "ofxOceanodeTimelineController.h"
#include "Editors/ofxOceanodeTimelineLaneEditorUi.h"

#include "Managers/ofxOceanodeContainer.h"
#include "MacroMixerGui.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <string>

using namespace ofxOceanodeTimelineLaneEditorUi;

// The panels' drafts and queued edits. The blocked-node dialog is the
// engine's: the manager registers each engine and applies its answers.
struct ofxOceanodeTimelineController::MorphPanels {
    MacroMixerGui::State state;
};

namespace {
// A clip's transition keeps its policies as JSON, as the track's playback
// reads them: {key: {"policy": string, "options": json}}, by type (morph
// policies) or by router id (mix policy overrides); null when there is none.
MacroMixerMorphPolicies policiesFromJson(const ofJson& json) {
    MacroMixerMorphPolicies policies;
    if(!json.is_object()) return policies;
    for(auto it = json.begin(); it != json.end(); ++it) {
        const ofJson& entry = it.value();
        if(!entry.is_object() || !entry.contains("policy") || !entry["policy"].is_string()) continue;
        MacroMixerMorphPolicy policy;
        policy.policy = entry["policy"].get<std::string>();
        if(policy.policy.empty()) continue;
        if(entry.contains("options")) policy.options = entry["options"];
        policies[it.key()] = std::move(policy);
    }
    return policies;
}
ofJson policiesToJson(const MacroMixerMorphPolicies& policies) {
    if(policies.empty()) return ofJson();
    ofJson json = ofJson::object();
    for(const auto& [key, policy] : policies) {
        ofJson entry = ofJson::object();
        entry["policy"] = policy.policy;
        entry["options"] = policy.options;
        json[key] = std::move(entry);
    }
    return json;
}
const ImVec4 kMorphWarning(1.0f, 0.65f, 0.24f, 1.0f);
constexpr float kMorphWrapWidth = 420.0f;
} // namespace

ofxOceanodeTimelineController::MorphPanels& ofxOceanodeTimelineController::morphPanelState() {
    if(!morphPanels) {
        morphPanels = std::make_shared<MorphPanels>();
    }
    return *morphPanels;
}

void ofxOceanodeTimelineController::reportMorphError(const std::string& message) {
    morphErrorMessage = message.empty() ? std::string("The Macro Morphing Track refused it.") : message;
    requestMorphErrorPopup = true;
}

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineController::morphRefHere(
        const ofxOceanodeTimelineManager& timeline) const {
    ofxOceanodeTimelineManager::TimelineRef inner;
    if(rowScope != nullptr && rowScope->sequence != nullptr && timeline.refOf(*rowScope->sequence, inner))
        return ofxOceanodeTimelineManager::normalizedRef(inner);
    return scopeRef();
}

// The windows are of one timeline's track: shown for another, the drafts of
// the last one's routers (kept by router id) go.
void ofxOceanodeTimelineController::showMorphWindowsHere(const ofxOceanodeTimelineManager& timeline) {
    const auto here = morphRefHere(timeline);
    if(!(morphWindowRef == here) && morphPanels) morphPanels->state.routerDrafts.clear();
    morphWindowRef = here;
}

void ofxOceanodeTimelineController::openMorphRouters(const ofxOceanodeTimelineManager& timeline) {
    showMorphWindowsHere(timeline);
    morphRoutersOpen = true;
    morphRoutersFocus = true;
}

void ofxOceanodeTimelineController::openMorphSource(const ofxOceanodeTimelineManager& timeline, const std::string& clipId) {
    showMorphWindowsHere(timeline);
    morphSourceClipId = clipId;
    morphSourceOpen = true;
    morphSourceFocus = true;
}

void ofxOceanodeTimelineController::openMorphTransition(const ofxOceanodeTimelineManager& timeline, const std::string& clipId) {
    showMorphWindowsHere(timeline);
    morphTransitionClipId = clipId;
    morphTransitionOpen = true;
    morphTransitionFocus = true;
}

bool ofxOceanodeTimelineController::morphOverlapAt(const ofxOceanodeTimelineTrack& track, double beat,
                                                   ofxOceanodeTimelineMorphOverlap& found) {
    for(const auto& overlap : ofxOceanodeTimelineManager::morphOverlaps(track)) {
        if(beat < overlap.fromBeat || beat > overlap.toBeat) continue;
        found = overlap;
        return true;
    }
    return false;
}

bool ofxOceanodeTimelineController::morphOverlapInto(const ofxOceanodeTimelineTrack& track, const std::string& clipId,
                                                     ofxOceanodeTimelineMorphOverlap& found) {
    for(const auto& overlap : ofxOceanodeTimelineManager::morphOverlaps(track)) {
        if(overlap.laterClipId != clipId) continue;
        found = overlap;
        return true;
    }
    return false;
}

// "Add Morphing Source": the folder dialog blocks, so it runs here, after
// the track loop and its popups.
void ofxOceanodeTimelineController::serviceMorphRequests(ofxOceanodeTimelineManager& timeline) {
    if(!morphFolderRequest) return;
    morphFolderRequest = false;
    // The timeline whose track asked: the shown one, or an unfolded row's.
    const bool shown = morphFolderRef == scopeRef() && shownSequence != nullptr;
    auto* asked = shown ? shownSequence : timeline.timelineFor(morphFolderRef);
    if(asked == nullptr) return;
    auto& sequence = *asked;
    const auto* track = timeline.morphTrackOf(static_cast<const ofxOceanodeTimelineSequence&>(sequence));
    if(track == nullptr) return;
    const std::string trackId = track->id;
    auto result = ofSystemLoadDialog("Choose Macro Folder", true);
    if(!result.bSuccess) return;
    std::string error;
    const std::string clipId = timeline.addMorphSource(sequence, result.getPath(), error);
    if(clipId.empty()) {
        // (No error: a node blocks the source, and its dialog is asking.)
        if(!error.empty()) reportMorphError(error);
        return;
    }
    if(!shown) return;
    selectedTracks.clear();
    selectedClips = {{trackId, clipId}};
}

void ofxOceanodeTimelineController::drawMorphOverlaps(const ofxOceanodeTimelineManager& timeline,
                                                      const ofxOceanodeTimelineTrack& track, ImDrawList* dl, const ImVec2& laneMin,
                                                      const ImVec2& max, const std::function<float(double)>& beatOffset) {
    if(!track.isMorphTrack) return;
    const bool windowHere = morphTransitionOpen && morphWindowRef == morphRefHere(timeline);
    const float top = laneMin.y + 3.0f, bottom = max.y - 3.0f;
    if(bottom <= top) return;
    // The track's colour, lifted: brighter than the two clips under it.
    auto lifted = [&](float lift, int alpha) {
        auto channel = [&](unsigned char c) { return static_cast<int>(c + (255 - c) * lift); };
        return IM_COL32(channel(track.color.r), channel(track.color.g), channel(track.color.b), alpha);
    };
    for(const auto& overlap : ofxOceanodeTimelineManager::morphOverlaps(track)) {
        const float x1 = laneMin.x + beatOffset(overlap.fromBeat);
        const float x2 = laneMin.x + beatOffset(overlap.toBeat);
        if(x2 <= x1 || x2 < dl->GetClipRectMin().x || x1 > dl->GetClipRectMax().x) continue;
        const ofxOceanodeTimelineClip* later = nullptr;
        for(const auto& clip : track.clips) if(clip.id == overlap.laterClipId) later = &clip;
        const bool morph = later != nullptr && later->morphIn.morph;
        const bool edited = windowHere && morphTransitionClipId == overlap.laterClipId;
        const bool hovered = clipDragMode == ClipDragMode::None && !ImGui::IsAnyItemActive() &&
                             hoverRect(ImVec2(x1, top), ImVec2(x2, bottom));
        const ImU32 fill = lifted(0.35f, edited ? 150 : hovered ? 125 : 95);
        const ImU32 line = lifted(0.75f, edited || hovered ? 255 : 200);
        dl->AddRectFilled(ImVec2(x1, top), ImVec2(x2, bottom), fill);
        dl->PushClipRect(ImVec2(x1, top), ImVec2(x2, bottom), true);
        dl->AddLine(ImVec2(x1, top), ImVec2(x2, bottom), line, 1.0f);
        dl->AddLine(ImVec2(x1, bottom), ImVec2(x2, top), line, 1.0f);
        dl->PopClipRect();
        dl->AddRect(ImVec2(x1, top), ImVec2(x2, bottom), line, 0.0f, 0, edited ? 2.0f : 1.0f);
        const char* label = morph ? "Morph" : "Mix";
        const ImVec2 labelSize = ImGui::CalcTextSize(label);
        if(x2 - x1 >= labelSize.x + 8.0f && bottom - top >= labelSize.y + 4.0f) {
            // On a plate of its own, so the diagonals do not cross the word.
            const ImVec2 at((x1 + x2 - labelSize.x) * 0.5f, (top + bottom - labelSize.y) * 0.5f);
            const ImU32 plate = blendOver(fill, mutedTrackColor(track.color, 0.17f));
            dl->AddRectFilled(ImVec2(at.x - 3.0f, at.y - 1.0f), ImVec2(at.x + labelSize.x + 3.0f, at.y + labelSize.y + 1.0f), plate);
            dl->AddText(at, readableTextColor(plate), label);
        }
        if(hovered)
            ImGui::SetTooltip("%s transition, %.2f beats.\nDouble-click: its settings. Move or resize the clips to change it.",
                              label, overlap.toBeat - overlap.fromBeat);
    }
}

void ofxOceanodeTimelineController::drawMorphWindows(ofxOceanodeTimelineManager& timeline) {
    // Something refused where no editor was at hand (a macro's header menu).
    if(const std::string elsewhere = timeline.takeMorphError(); !elsewhere.empty()) reportMorphError(elsewhere);
    if(requestMorphErrorPopup) {
        ImGui::OpenPopup("Macro Morphing Track"); requestMorphErrorPopup = false;
    }
    centerNextModal();
    // Resizable: fitted to its text when it opens, the text wrapping to it after.
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 80.0f), ImVec2(FLT_MAX, FLT_MAX));
    if(ImGui::BeginPopupModal("Macro Morphing Track", nullptr, ImGuiWindowFlags_None)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(morphErrorMessage.c_str());
        ImGui::PopTextWrapPos();
        if(ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if(!(morphRoutersOpen || morphSourceOpen || morphTransitionOpen)) return;
    // Of the timeline they were opened on: the shown one, or the one of an
    // unfolded row (they stay while that timeline and its track are there).
    auto* opened = morphWindowRef == scopeRef() && shownSequence != nullptr ? shownSequence : timeline.timelineFor(morphWindowRef);
    const ofxOceanodeTimelineTrack* track = opened != nullptr
        ? timeline.morphTrackOf(static_cast<const ofxOceanodeTimelineSequence&>(*opened)) : nullptr;
    if(track == nullptr) {
        // The timeline or its track is gone: its windows close, with the
        // drafts and the edits still queued for it.
        morphRoutersOpen = morphSourceOpen = morphTransitionOpen = false;
        if(morphPanels) {
            morphPanels->state.actions.clear();
            morphPanels->state.routerDrafts.clear();
        }
        return;
    }
    auto& sequence = *opened;
    auto& panels = morphPanelState();
    MacroMixerRuntime* engine = timeline.morphEngine(sequence);
    const std::string trackId = track->id;
    const std::string status = timeline.morphEngineStatus(sequence);
    // Resizable, each fitted to its content the first time it opens (its
    // size is remembered after): a long list scrolls inside the window.
    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    auto placeNext = [](bool& focus) {
        if(!focus) return;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        ImGui::SetNextWindowPos(ImVec2(mouse.x + 16.0f, mouse.y + 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        focus = false;
    };
    auto wrapped = [](const ImVec4* color, const std::string& text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kMorphWrapWidth);
        if(color != nullptr) ImGui::TextColored(*color, "%s", text.c_str());
        else ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    };

    // ---- Routers: what the track's engine publishes on Timeline nodes ----
    if(morphRoutersOpen) {
        placeNext(morphRoutersFocus);
        const std::string title = "Macro Morphing Routers - " + timeline.timelineName(morphWindowRef) +
                                  "###timelineMorphRouters" + ofToString(windowId);
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 120.0f), ImVec2(FLT_MAX, FLT_MAX));
        if(ImGui::Begin(title.c_str(), &morphRoutersOpen, windowFlags)) {
            if(!status.empty()) wrapped(&kMorphWarning, status);
            if(!timeline.morphHasPublisher(sequence))
                wrapped(nullptr, "No Timeline node points at this timeline: the routers appear as the ports of a "
                                 "Timeline node that does. Until then the track plays and its outputs go nowhere.");
            if(engine == nullptr) ImGui::TextDisabled("The track's engine is not available.");
            else {
                // The canvas the timeline is on: its registry says which router types exist.
                ofxOceanodeContainer* host = timeline.canvasContainer(morphWindowRef.canvas);
                if(host == nullptr) host = container.get();
                MacroMixerGui::drawRouters(*engine, panels.state, host);
                ImGui::Spacing();
                MacroMixerGui::drawDefaultPolicies(*engine, panels.state);
            }
        }
        ImGui::End();
    }

    // ---- Source: the one a clip plays ----
    if(morphSourceOpen) {
        const auto* clip = timeline.getClip(static_cast<const ofxOceanodeTimelineSequence&>(sequence), trackId, morphSourceClipId);
        if(clip == nullptr) morphSourceOpen = false;
        else {
            placeNext(morphSourceFocus);
            const std::string sourceName = timeline.morphSourceName(sequence, clip->morphSource);
            const std::string title = "Morphing Source - " + (sourceName.empty() ? clip->name : sourceName) +
                                      "###timelineMorphSource" + ofToString(windowId);
            ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 120.0f), ImVec2(FLT_MAX, FLT_MAX));
            if(ImGui::Begin(title.c_str(), &morphSourceOpen, windowFlags)) {
                if(!status.empty()) wrapped(&kMorphWarning, status);
                const auto revision = engine != nullptr ? engine->macro(clip->morphSource) : MacroMixerRuntime::Revision();
                if(engine == nullptr) ImGui::TextDisabled("The track's engine is not available.");
                else if(!revision) wrapped(&kMorphWarning, "The track's engine no longer has this clip's source.");
                else {
                    ImGui::PushID(revision->id().c_str());
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(revision->name().c_str());
                    ImGui::SameLine();
                    if(ImGui::SmallButton("Refresh"))
                        panels.state.actions.push_back({MacroMixerGui::ActionKind::Refresh, revision->source()});
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("Read the source folder again.\nEdits to its timelines not saved with Save Source are lost.");
                    // Its timelines as edited here (the track unfolded), against its folder's.
                    const bool dirty = timeline.morphSourceDirty(sequence, clip->morphSource);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!dirty);
                    if(ImGui::SmallButton("Save Source")) {
                        std::string saveError;
                        if(!timeline.saveMorphSource(sequence, clip->morphSource, saveError)) reportMorphError(saveError);
                    }
                    ImGui::EndDisabled();
                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(dirty ? "Write this source's timelines, as edited here, into its folder."
                                                : "This source's timelines are as its folder holds them.");
                    if(dirty) { ImGui::SameLine(); ImGui::TextColored(kMorphWarning, "unsaved timeline edits"); }
                    ImGui::Separator();
                    MacroMixerGui::SourceContext context;
                    context.color = ofFloatColor(track->color);
                    context.morph = clip->morphIn.morph;
                    MacroMixerGui::drawSource(*engine, panels.state, context, *revision);
                    ImGui::PopID();
                }
            }
            ImGui::End();
        }
    }

    // ---- Transition: into a clip, from the clip it overlaps ----
    if(morphTransitionOpen) {
        const auto* clip = timeline.getClip(static_cast<const ofxOceanodeTimelineSequence&>(sequence), trackId, morphTransitionClipId);
        if(clip == nullptr) morphTransitionOpen = false;
        else {
            placeNext(morphTransitionFocus);
            ofxOceanodeTimelineMorphOverlap overlap;
            const bool overlapping = morphOverlapInto(*track, clip->id, overlap);
            const auto* earlier = overlapping
                ? timeline.getClip(static_cast<const ofxOceanodeTimelineSequence&>(sequence), trackId, overlap.earlierClipId) : nullptr;
            const std::string title = "Transition - " + (earlier != nullptr ? earlier->name + " > " : std::string()) + clip->name +
                                      "###timelineMorphTransition" + ofToString(windowId);
            ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 120.0f), ImVec2(FLT_MAX, FLT_MAX));
            if(ImGui::Begin(title.c_str(), &morphTransitionOpen, windowFlags)) {
                if(overlapping) ImGui::TextDisabled("Over the overlap: %.2f beats", overlap.toBeat - overlap.fromBeat);
                else wrapped(&kMorphWarning, "This clip does not overlap the clip before it: it starts with a hard switch. "
                                              "These settings are kept for when it does.");
                ofxOceanodeTimelineMorphTransition transition = clip->morphIn;
                bool changed = false;
                if(ImGui::RadioButton("Mix", !transition.morph)) { changed = changed || transition.morph; transition.morph = false; }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Both sources play: their router outputs mix by each router's policy.");
                ImGui::SameLine();
                if(ImGui::RadioButton("Morph", transition.morph)) { changed = changed || !transition.morph; transition.morph = true; }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("The incoming source's controls glide from the outgoing source's values.");
                ImGui::SetNextItemWidth(180.0f);
                if(ImGui::SliderFloat("Bi Pow", &transition.biPow, -1.0f, 1.0f, "%.2f")) {
                    transition.biPow = ofClamp(transition.biPow, -1.0f, 1.0f);
                    changed = true;
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("The shape of the progress through the overlap (0: linear).");
                if(transition.morph) {
                    ImGui::SetNextItemWidth(180.0f);
                    if(ImGui::SliderFloat("Log Blend", &transition.logBlend, 0.0f, 1.0f, "%.2f")) {
                        transition.logBlend = ofClamp(transition.logBlend, 0.0f, 1.0f);
                        changed = true;
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("How far controls with a Log morph hint glide geometrically (1) rather than linearly (0).");
                }
                ImGui::Separator();
                if(engine == nullptr) ImGui::TextDisabled("The track's engine is not available.");
                else {
                    // This transition's own policies, over those into the clip's source.
                    MacroMixerMorphPolicies policies = policiesFromJson(transition.morphPolicies);
                    MacroMixerGui::RouterPolicies routers = policiesFromJson(transition.routerOverrides);
                    ImGui::PushID("transitionPolicies");
                    if(MacroMixerGui::drawTransitionPolicies(engine->contract(), engine->morphPoliciesInto(clip->morphSource),
                                                             policies, routers)) {
                        transition.morphPolicies = policiesToJson(policies);
                        transition.routerOverrides = policiesToJson(routers);
                        changed = true;
                    }
                    ImGui::PopID();
                }
                // (clip is not read after this: the model may replace it.)
                if(changed) timeline.setMorphTransition(sequence, trackId, morphTransitionClipId, transition);
            }
            ImGui::End();
        }
    }

    // The panels' edits (and the blocked-node dialog's answers), outside
    // their drawing: to the engine, whose state the track then stores.
    while(!panels.state.actions.empty()) {
        const MacroMixerGui::Action action = std::move(panels.state.actions.front());
        panels.state.actions.pop_front();
        std::string error;
        if(!timeline.applyMorphAction(sequence, action, panels.state, error) && !error.empty()) reportMorphError(error);
    }
}
