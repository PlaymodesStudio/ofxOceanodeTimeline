#include "ofxOceanodeTimelinePianoRollEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include "Nodes/ofxOceanodeGlobalPitch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

namespace {
// Mutation groups A..D: shades of violet, mid, light, deep and pink-violet.
ImU32 mutationGroupColor(int group, int alpha = 255) {
    static const int colors[kMaxMutationGroups][3] = {{185, 120, 245}, {225, 185, 255}, {130, 75, 210}, {215, 105, 225}};
    const auto& c = colors[std::clamp(group, 0, kMaxMutationGroups - 1)];
    return IM_COL32(c[0], c[1], c[2], alpha);
}
const char* mutationGroupName(int group) {
    static const char* names[] = {"A", "B", "C", "D"};
    return names[std::clamp(group, 0, kMaxMutationGroups - 1)];
}
bool inPianoScale(const ofxOceanodeTimelineLane& lane, int pitch) {
    return !lane.pianoScaleEnabled || ((lane.pianoScaleMask >> (((pitch % 12) + 12) % 12)) & 1);
}
// With scale snap on: the nearest scale key (the lower one on a tie).
int snapToPianoScale(const ofxOceanodeTimelineLane& lane, int pitch, int low, int high) {
    if(!lane.pianoScaleEnabled || !lane.pianoScaleSnap || (lane.pianoScaleMask & 0xFFF) == 0) return pitch;
    for(int distance = 0; distance <= 11; ++distance) {
        if(pitch - distance >= low && inPianoScale(lane, pitch - distance)) return pitch - distance;
        if(pitch + distance <= high && inPianoScale(lane, pitch + distance)) return pitch + distance;
    }
    return pitch;
}
// Scale presets, rooted on C (shared by the lane scale and the mutation keys).
const std::pair<const char*, int> kScalePresets[] = {
    {"Chromatic", 0xFFF}, {"Major", 0xAB5}, {"Minor", 0x5AD}, {"Harmonic minor", 0x9AD},
    {"Dorian", 0x6AD}, {"Major pentatonic", 0x295}, {"Minor pentatonic", 0x4A9}, {"Whole tone", 0x555}};
const char* scalePresetName(int mask) {
    for(const auto& preset : kScalePresets) if(preset.second == mask) return preset.first;
    return "Custom";
}
int rotateScale(int mask, int steps) {
    mask &= 0xFFF;
    return ((mask << steps) | (mask >> (12 - steps))) & 0xFFF;
}
// One coloured chip per group; returns true when clicked.
bool mutationGroupChip(int group, bool selected) {
    const ImU32 color = mutationGroupColor(group, selected ? 255 : 110);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mutationGroupColor(group, selected ? 255 : 190));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mutationGroupColor(group));
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? IM_COL32(20, 20, 24, 255) : IM_COL32(235, 235, 240, 255));
    const bool clicked = ImGui::SmallButton((std::string(" ") + mutationGroupName(group) + " ##mutationGroup" + ofToString(group)).c_str());
    ImGui::PopStyleColor(4);
    return clicked;
}
}

void ofxOceanodeTimelinePianoRollEditor::drawTargets(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;

    auto drawPianoRole = [&](const char* label, std::string& roleId) {
        const auto* currentBinding = roleId.empty() ? nullptr : timeline.getBinding(track.id, roleId);
        const std::string preview = currentBinding == nullptr ? "None" : compactParameterName(currentBinding->parameterPath);
        propertyLabel(label);
        if(ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str())) {
            if(ImGui::Selectable("None", currentBinding == nullptr)) {
                const std::string previousRoleId = roleId;
                roleId.clear();
                const bool stillUsed = lane->pianoPitchBindingId == previousRoleId ||
                    lane->pianoGateBindingId == previousRoleId ||
                    lane->pianoVelocityBindingId == previousRoleId;
                if(!previousRoleId.empty() && !stillUsed)
                    timeline.removeBindingFromLane(track.id, clip->id, lane->id, previousRoleId);
            }
            ImGui::InputText("Search parameters", targetSearch, sizeof(targetSearch));
            std::vector<std::string> paths = context.targetPaths;
            if(paths.empty()) for(const auto& binding : track.bindings) paths.push_back(binding.parameterPath);
            for(const auto& path : paths) {
                if(targetSearch[0] && ofToLower(path).find(ofToLower(std::string(targetSearch))) == std::string::npos) continue;
                const bool selected = currentBinding && currentBinding->parameterPath == path;
                if(ImGui::Selectable(path.c_str(), selected)) {
                    context.requestedTargetPath = path;
                    context.requestedTargetRole = &roleId == &lane->pianoPitchBindingId ? ofxOceanodeTimelineTargetRole::Pitch
                        : &roleId == &lane->pianoGateBindingId ? ofxOceanodeTimelineTargetRole::Gate : ofxOceanodeTimelineTargetRole::Velocity;
                }
            }
            ImGui::EndCombo();
        }
    };
    const ofxOceanodeTimelineNoteGroup* laneGroup = nullptr;
    for(const auto& id : lane->bindingIds)
        if(laneGroup == nullptr) laneGroup = ofxOceanodeTimelineManager::findNoteGroupForBinding(track, id);
    if(laneGroup != nullptr) {
        // Grouped piano rolls follow their group's targets.
        const std::string buttonLabel = "Routing: " + laneGroup->name + "...";
        if(ImGui::Button(buttonLabel.c_str()))
            context.noteGroupToEdit = laneGroup->id;
        {
            const char* labels[] = {"Pitch", "Gate", "Velocity"};
            const std::string* roles[] = {&laneGroup->pitchBindingId, &laneGroup->gateBindingId, &laneGroup->velocityBindingId};
            for(int role = 0; role < 3; ++role) {
                const auto* target = timeline.getBinding(track.id, *roles[role]);
                ImGui::TextWrapped("%s -> %s", labels[role], target == nullptr ? "No target" : target->parameterPath.c_str());
            }
        }
        if(ImGui::IsItemHovered()) {
            std::string tip;
            const char* roleNames[3] = {"Pitch", "Gate", "Velocity"};
            const std::string* roles[3] = {&laneGroup->pitchBindingId, &laneGroup->gateBindingId, &laneGroup->velocityBindingId};
            for(int role = 0; role < 3; ++role) {
                const auto* member = roles[role]->empty() ? nullptr : timeline.getBinding(track.id, *roles[role]);
                tip += std::string(role > 0 ? "\n" : "") + roleNames[role] + ": " +
                    (member == nullptr ? std::string("none") : member->parameterPath);
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    } else {
        drawPianoRole("Pitch", lane->pianoPitchBindingId);
        drawPianoRole("Gate", lane->pianoGateBindingId);
        drawPianoRole("Velocity", lane->pianoVelocityBindingId);
    }
}

void ofxOceanodeTimelinePianoRollEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto* lane = &context.lane;
    drawDivision(*lane);
    propertyLabel("Low");
    ImGui::DragInt("##Low", &lane->pianoLowPitch, 0.25f, 0, 127);
    propertyLabel("High");
    ImGui::DragInt("##High", &lane->pianoHighPitch, 0.25f, 0, 127);
    lane->pianoHighPitch = std::max(lane->pianoLowPitch, lane->pianoHighPitch);
    propertyLabel("Vel");
    ImGui::SliderFloat("##Vel", &lane->pianoDefaultVelocity, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Live edit sound", &lane->pianoLiveEditSound);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Monitor pitch and gate while inserting or moving notes.");
    ImGui::Checkbox("Snap", &lane->pianoSnapToGrid);
    ImGui::SameLine(); ImGui::Checkbox("Mono", &lane->pianoMonophonic);
    // Scale overlay: shades the keys outside the scale; snap keeps drawn and
    // moved notes on scale keys.
    ImGui::Checkbox("Scale##pianoScale", &lane->pianoScaleEnabled);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Shade the keys outside the scale");
    ImGui::SameLine();
    ImGui::BeginDisabled(!lane->pianoScaleEnabled);
    ImGui::Checkbox("Snap to scale##pianoScaleSnap", &lane->pianoScaleSnap);
    propertyLabel("Keys", 2.0f * smallButtonReserve("<"));
    if(ImGui::BeginCombo("##pianoScaleMask", scalePresetName(lane->pianoScaleMask))) {
        for(const auto& preset : kScalePresets)
            if(ImGui::Selectable(preset.first, preset.second == lane->pianoScaleMask)) lane->pianoScaleMask = preset.second;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if(ImGui::SmallButton("<##pianoScaleDown")) lane->pianoScaleMask = rotateScale(lane->pianoScaleMask, 11);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Transpose down a semitone");
    ImGui::SameLine();
    if(ImGui::SmallButton(">##pianoScaleUp")) lane->pianoScaleMask = rotateScale(lane->pianoScaleMask, 1);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Transpose up a semitone");
    pitchClassKeyboard("##pianoScaleKeys", lane->pianoScaleMask, IM_COL32(120, 175, 235, 255), 28.0f);
    ImGui::EndDisabled();
    propertyLabel("Seed", smallButtonReserve("New##pianoSeed"));
    ImGui::DragInt("##Seed", &lane->probabilitySeed, 1.0f, 0, 99999);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Which notes a probability below 100%% lets through.\nChange it for another pattern.");
    ImGui::SameLine();
    if(ImGui::SmallButton("New##pianoSeed")) lane->probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));

}

void ofxOceanodeTimelinePianoRollEditor::drawMutation(ofxOceanodeTimelineLaneEditorContext& context) {
    using Rules = ofxOceanodeTimelineMutationRules;
    auto* lane = &context.lane;
    if(mutationEditLaneId != lane->id) { mutationEditLaneId = lane->id; mutationEditGroup = 0; }
    mutationEditGroup = std::clamp(mutationEditGroup, 0, static_cast<int>(lane->mutationGroups.size()));
    mutationTabFrame = ImGui::GetFrameCount();

    // Group chips: click to edit a group, right-click B..D to delete it.
    auto notesIn = [&](int group, bool mutatingOnly) {
        return static_cast<int>(std::count_if(lane->pianoNotes.begin(), lane->pianoNotes.end(), [&](const auto& note) {
            return note.mutationGroup == group && (!mutatingOnly || note.mutation > 0.0f);
        }));
    };
    for(int group = 0; group <= static_cast<int>(lane->mutationGroups.size()); ++group) {
        if(group > 0) ImGui::SameLine(0.0f, 3.0f);
        if(mutationGroupChip(group, group == mutationEditGroup)) mutationEditGroup = group;
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %d note%s%s", mutationGroupName(group), notesIn(group, false),
                              notesIn(group, false) == 1 ? "" : "s", group > 0 ? "\nRight-click: delete" : "");
        if(group > 0 && ImGui::BeginPopupContextItem()) {
            if(ImGui::MenuItem((std::string("Delete group ") + mutationGroupName(group)).c_str())) {
                ofxOceanodeTimelineRemoveMutationGroup(*lane, group);
                if(mutationEditGroup >= group) mutationEditGroup = std::max(0, mutationEditGroup - 1);
            }
            ImGui::EndPopup();
        }
    }
    if(static_cast<int>(lane->mutationGroups.size()) + 1 < kMaxMutationGroups) {
        ImGui::SameLine(0.0f, 3.0f);
        if(ImGui::SmallButton("+##addMutationGroup")) {
            lane->mutationGroups.push_back({lane->mutation, 0});
            mutationEditGroup = static_cast<int>(lane->mutationGroups.size());
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("New group");
    }
    ImGui::Checkbox("Mono##mutationMono", &lane->mutationMonophonic);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Monophonic: shifted or lengthened notes never overlap others");
    ImGui::SameLine();
    ImGui::Checkbox("Drift##mutationDrift", &lane->mutationDrift);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each repeat mutates the previous one. Resets on stop.");

    const int group = mutationEditGroup;
    auto* entry = group == 0 ? nullptr : &lane->mutationGroups[group - 1];
    const Rules rules = ofxOceanodeTimelineMutationRulesFor(*lane, group);

    // B..D show A's value (dimmed) until changed; a changed value can be
    // set back to A's from its right-click menu.
    auto following = [&](int field) { return entry != nullptr && !(entry->overrides & field); };
    auto beginField = [&](int field) {
        if(following(field)) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    };
    auto endField = [&](int field, bool wasFollowing) {
        if(wasFollowing) {
            ImGui::PopStyleVar();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Same as A");
        } else if(entry != nullptr && ImGui::BeginPopupContextItem()) {
            if(ImGui::MenuItem("Same as A")) entry->overrides &= ~field;
            ImGui::EndPopup();
        }
    };
    auto commit = [&](int field, auto member, auto value) {
        if(entry == nullptr) lane->mutation.*member = value;
        else { entry->rules.*member = value; entry->overrides |= field; }
    };

    auto strength = [&](const char* title, const char* id, int field, float Rules::* member) {
        ImGui::SeparatorText(title);
        propertyLabel("Strength");
        const bool wasFollowing = following(field);
        beginField(field);
        float percent = rules.*member * 100.0f;
        if(ImGui::SliderFloat(id, &percent, 0.0f, 100.0f, percent <= 0.0f ? "Off" : "%.0f%%"))
            commit(field, member, ofClamp(std::round(percent) / 100.0f, 0.0f, 1.0f));
        endField(field, wasFollowing);
        ImGui::BeginDisabled(rules.*member <= 0.0f);
    };
    auto intField = [&](const char* label, const char* id, int field, int Rules::* member, float speed, int low, int high, const char* format) {
        propertyLabel(label);
        const bool wasFollowing = following(field);
        beginField(field);
        int value = rules.*member;
        if(ImGui::DragInt(id, &value, speed, low, high, format)) commit(field, member, std::clamp(value, low, high));
        endField(field, wasFollowing);
    };

    strength("Pitch", "##mutationPitchStrength", Rules::PitchStrength, &Rules::pitchStrength);
    {
        // Right-click: which way the pitch may move (+/-, up only, down only).
        static const char* directions[] = {"+/-", "+", "-"};
        static const char* directionNames[] = {"+/-  Up or down", "+  Up only", "-  Down only"};
        constexpr int fields = Rules::PitchRange | Rules::PitchDirection;
        propertyLabel("Range");
        const bool wasFollowing = entry != nullptr && !(entry->overrides & fields);
        if(wasFollowing) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        int value = rules.pitchRange;
        const std::string format = std::string(directions[std::clamp(rules.pitchDirection, 0, 2)]) + " %d st";
        if(ImGui::DragInt("##mutationPitchRange", &value, 0.2f, 1, 48, format.c_str()))
            commit(Rules::PitchRange, &Rules::pitchRange, std::clamp(value, 1, 48));
        if(wasFollowing) ImGui::PopStyleVar();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip(wasFollowing ? "Same as A\nRight-click: direction" : "Right-click: direction");
        if(ImGui::BeginPopupContextItem("##mutationPitchDirection")) {
            for(int direction = 0; direction < 3; ++direction)
                if(ImGui::MenuItem(directionNames[direction], nullptr, rules.pitchDirection == direction))
                    commit(Rules::PitchDirection, &Rules::pitchDirection, direction);
            if(entry != nullptr && (entry->overrides & fields)) {
                ImGui::Separator();
                if(ImGui::MenuItem("Same as A")) entry->overrides &= ~fields;
            }
            ImGui::EndPopup();
        }
    }
    {
        // Scale presets are rooted on C; the arrows transpose the selection.
        const char* current = scalePresetName(rules.scaleMask);
        const bool wasFollowing = following(Rules::ScaleMask);
        propertyLabel("Keys", 2.0f * smallButtonReserve("<"));
        beginField(Rules::ScaleMask);
        if(ImGui::BeginCombo("##mutationScale", current)) {
            for(const auto& preset : kScalePresets)
                if(ImGui::Selectable(preset.first, preset.second == rules.scaleMask))
                    commit(Rules::ScaleMask, &Rules::scaleMask, preset.second);
            ImGui::Separator();
            if(lane->pianoScaleEnabled && ImGui::Selectable("Lane scale"))
                commit(Rules::ScaleMask, &Rules::scaleMask, lane->pianoScaleMask & 0xFFF);
            if(ImGui::Selectable("Keys these notes use")) {
                int mask = 0;
                for(const auto& note : lane->pianoNotes)
                    if(note.mutationGroup == group) mask |= 1 << (note.pitch % 12);
                if(mask != 0) commit(Rules::ScaleMask, &Rules::scaleMask, mask);
            }
            ImGui::EndCombo();
        }
        endField(Rules::ScaleMask, wasFollowing);
        auto rotate = [&](int steps) { commit(Rules::ScaleMask, &Rules::scaleMask, rotateScale(rules.scaleMask, steps)); };
        ImGui::SameLine();
        if(ImGui::SmallButton("<##mutationDown")) rotate(11);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Transpose the allowed keys down a semitone");
        ImGui::SameLine();
        if(ImGui::SmallButton(">##mutationUp")) rotate(1);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Transpose the allowed keys up a semitone");
        int mask = rules.scaleMask;
        if(pitchClassKeyboard("##mutationKeys", mask, mutationGroupColor(group, wasFollowing ? 120 : 255)))
            commit(Rules::ScaleMask, &Rules::scaleMask, mask);
    }
    ImGui::EndDisabled();

    strength("Offset", "##mutationOffsetStrength", Rules::OffsetStrength, &Rules::offsetStrength);
    intField("Steps", "##mutationOffsetSteps", Rules::OffsetSteps, &Rules::offsetSteps, 0.1f, 1, 64, "+/- %d");
    ImGui::EndDisabled();

    strength("Length", "##mutationLengthStrength", Rules::LengthStrength, &Rules::lengthStrength);
    {
        propertyLabel("Up to");
        const bool wasFollowing = following(Rules::LengthMax);
        beginField(Rules::LengthMax);
        float value = rules.lengthMax;
        if(ImGui::DragFloat("##mutationLengthMax", &value, 0.02f, 1.0f, 16.0f, "x %.2f"))
            commit(Rules::LengthMax, &Rules::lengthMax, ofClamp(value, 1.0f, 16.0f));
        if(!wasFollowing && ImGui::IsItemHovered()) ImGui::SetTooltip("x 2: half to double");
        endField(Rules::LengthMax, wasFollowing);
    }
    ImGui::EndDisabled();

    strength("Velocity", "##mutationVelocityStrength", Rules::VelocityStrength, &Rules::velocityStrength);
    {
        propertyLabel("Range");
        const bool wasFollowing = following(Rules::VelocityRange);
        beginField(Rules::VelocityRange);
        float value = rules.velocityRange;
        if(ImGui::DragFloat("##mutationVelocityRange", &value, 0.005f, 0.01f, 1.0f, "+/- %.2f"))
            commit(Rules::VelocityRange, &Rules::velocityRange, ofClamp(value, 0.01f, 1.0f));
        endField(Rules::VelocityRange, wasFollowing);
    }
    ImGui::EndDisabled();
}

void ofxOceanodeTimelinePianoRollEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    auto& editorLaneId = context.focusedLaneId;
    const bool isFocused = context.focused;
    updateMonitoring(timeline, context.focusedLaneId);
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float zoneLeft = canvas.zoneLeft, left = canvas.left, right = canvas.right;
    const bool editorCanvasHovered = canvas.hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The note selection is keyed by index into this lane's pianoNotes,
    // so switching to a different lane must drop it -- otherwise stale
    // indices could highlight (or, worse, delete) unrelated notes.
    if(isFocused && pianoSelectionLaneId != lane->id) {
        pianoSelectionLaneId = lane->id;
        pianoSelectedNoteIndices.clear();
        pianoDragSnapshot.clear();
        pianoValueSnapshot.clear();
        pianoDragMode = PianoDragMode::None;
        pianoInsertionDrag = false;
        pianoDragNoteIndex = -1;
        pianoMarqueeActive = false;
    }
    const float rollTop = editorMin.y + 8.0f;
    // VEL / PROB / MUT strip heights: dragged from the handles above them
    // (the roll keeps at least 40 px)
    auto& strips = pianoStripHeights.try_emplace(lane->id, std::array<float, 3>{25.0f, 26.0f, 22.0f}).first->second;
    {
        const float room = std::max(36.0f, (editorMax.y - 19.0f) - rollTop - 40.0f);
        strips[0] = ofClamp(strips[0], 12.0f, std::max(12.0f, room - 24.0f));
        strips[1] = ofClamp(strips[1], 12.0f, std::max(12.0f, room - strips[0] - 12.0f));
        strips[2] = ofClamp(strips[2], 12.0f, std::max(12.0f, room - strips[0] - strips[1]));
    }
    const float mutationBottom = editorMax.y - 6.0f;
    const float mutationTop = mutationBottom - strips[2];
    const float probabilityBottom = mutationTop - 4.0f;
    const float probabilityTop = probabilityBottom - strips[1];
    const float velocityBottom = probabilityTop - 4.0f;
    const float velocityTop = velocityBottom - strips[0];
    const float rollBottom = velocityTop - 5.0f;
    const float stripsBottom = mutationBottom;
    // A note's value in one strip: 0 VEL, 1 PROB, 2 MUT.
    auto noteValue = [](ofxOceanodeTimelinePianoNote& note, int field) -> float& {
        return field == 0 ? note.velocity : field == 1 ? note.probability : note.mutation;
    };
    const float keyboardRight = zoneLeft - 1.0f;
    const float keyboardLeft = keyboardRight - kPianoKeyboardWidth;
    const float pianoScrollbarRight = keyboardLeft - 4.0f;
    const float pianoScrollbarLeft = pianoScrollbarRight - kPianoScrollbarWidth;

    const int lowPitch = ofClamp(lane->pianoLowPitch, 0, 127);
    const int highPitch = ofClamp(lane->pianoHighPitch, lowPitch, 127);
    // Global Pitch: notes are drawn where the lane's instrument will play them
    // (its scale and transpose, as that node's ticks say). Display only: the
    // clip stores and sends its notes unchanged and the instrument applies
    // Global Pitch once. A moved note leaves a ghost at its stored pitch, and
    // editing acts on the stored notes; clicking either one picks the note.
    ofxOceanodeNodeModel* soundingNode = timeline.pianoPitchNode(track.id, *lane);
    auto sounding = [soundingNode](int pitch) -> int {
        if(soundingNode == nullptr) return pitch;
        return std::clamp((int)std::round(ofxOceanodeGlobalPitch::apply(soundingNode, (float)pitch, 0, false)), 0, 127);
    };
    auto atPitch = [&sounding](const ofxOceanodeTimelinePianoNote& note, int pitch) {
        return note.pitch == pitch || sounding(note.pitch) == pitch;
    };
    dl->AddRectFilled(ImVec2(keyboardLeft, rollTop), ImVec2(keyboardRight, rollBottom),
                      IM_COL32(205, 205, 200, 255));
    const float pitchHeight = (rollBottom - rollTop) / static_cast<float>(highPitch - lowPitch + 1);
    // Black keys are drawn on the side away from the note grid (like the
    // shank of a real key, which doesn't reach all the way to the front
    // edge). Each row is clipped to its own bounds so a black key can
    // never visually bleed into the semitone above or below it.
    const float blackKeyRight = keyboardLeft + kPianoKeyboardWidth * 0.70f;
    for(int keyboardPitch = lowPitch; keyboardPitch <= highPitch; ++keyboardPitch) {
        const float keyBottom = rollBottom - (keyboardPitch - lowPitch) * pitchHeight;
        const float keyTop = keyBottom - pitchHeight;
        const int noteClass = keyboardPitch % 12;
        const bool blackKey = noteClass == 1 || noteClass == 3 || noteClass == 6 || noteClass == 8 || noteClass == 10;
        // Clicking (and, while held, dragging across) a key previews it:
        // the lane's Gate/Pitch bindings get a live override for as long
        // as the mouse is down, so the bound parameters sound the note
        // immediately -- like pressing a real keyboard -- without
        // writing anything into the clip.
        const bool keyPressedHere = pianoKeyboardPreviewActive && pianoKeyboardPreviewTrackId == track.id &&
            pianoKeyboardPreviewPitch == keyboardPitch;
        const bool keyHovered = hoverRect(ImVec2(keyboardLeft, keyTop), ImVec2(keyboardRight, keyBottom));
        if(keyHovered) ImGui::SetTooltip("Left-click to audition. Right-click to select all notes at this pitch.\nShift+right-click adds this pitch to the selection.");
        if(keyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            editorLaneId = lane->id;
            if(pianoSelectionLaneId != lane->id || !ImGui::GetIO().KeyShift)
                pianoSelectedNoteIndices.clear();
            pianoSelectionLaneId = lane->id;
            for(size_t index = 0; index < lane->pianoNotes.size(); ++index)
                if(atPitch(lane->pianoNotes[index], keyboardPitch))
                    pianoSelectedNoteIndices.insert(static_cast<int>(index));
            pianoDragMode = PianoDragMode::None;
            pianoDragNoteIndex = -1;
            pianoDragSnapshot.clear();
            pianoMarqueeActive = false;
        }
        if(keyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Match the canvas's own convention (see editorCanvasHovered
            // above): clicking anywhere interactive on an unfocused
            // lane focuses it, rather than silently doing nothing.
            if(!isFocused) editorLaneId = lane->id;
            pianoKeyboardPreviewActive = true;
            pianoKeyboardPreviewTrackId = track.id;
            pianoKeyboardPreviewGateBindingId = lane->pianoGateBindingId;
            pianoKeyboardPreviewPitchBindingId = lane->pianoPitchBindingId;
            pianoKeyboardPreviewPitch = keyboardPitch;
            pianoKeyboardPreviewClipId = clip->id;
            pianoKeyboardPreviewLaneId = lane->id;
            timeline.setPianoKeyPreview(track.id, clip->id, lane->id, keyboardPitch);
        } else if(pianoKeyboardPreviewActive && pianoKeyboardPreviewTrackId == track.id && keyHovered &&
                  ImGui::IsMouseDown(ImGuiMouseButton_Left) && pianoKeyboardPreviewPitch != keyboardPitch) {
            pianoKeyboardPreviewPitch = keyboardPitch;
            timeline.setPianoKeyPreview(track.id, pianoKeyboardPreviewClipId, pianoKeyboardPreviewLaneId, keyboardPitch);
        }
        dl->PushClipRect(ImVec2(keyboardLeft, keyTop), ImVec2(keyboardRight, keyBottom), true);
        if(blackKey) {
            dl->AddRectFilled(ImVec2(keyboardLeft, keyTop),
                              ImVec2(blackKeyRight, keyBottom), keyPressedHere ? IM_COL32(90, 150, 230, 255) : IM_COL32(30, 31, 34, 255));
        } else {
            if(keyPressedHere) dl->AddRectFilled(ImVec2(keyboardLeft, keyTop), ImVec2(keyboardRight, keyBottom), IM_COL32(140, 185, 240, 255));
            dl->AddLine(ImVec2(keyboardLeft, keyTop), ImVec2(keyboardRight, keyTop),
                        IM_COL32(75, 75, 75, 185));
        }
        if(lane->pianoScaleEnabled) {
            if(inPianoScale(*lane, keyboardPitch))
                dl->AddRectFilled(ImVec2(keyboardRight - 4.0f, keyTop + 1.0f), ImVec2(keyboardRight, keyBottom - 1.0f), IM_COL32(120, 175, 235, 255));
            else dl->AddRectFilled(ImVec2(keyboardLeft, keyTop), ImVec2(keyboardRight, keyBottom), IM_COL32(0, 0, 0, 60));
        }
        if(noteClass == 0 && pitchHeight >= 13.0f) {
            char noteLabel[8];
            std::snprintf(noteLabel, sizeof(noteLabel), "C%d", keyboardPitch / 12 - 1);
            const float textHeight = ImGui::GetFontSize();
            const float textY = keyTop + (pitchHeight - textHeight) * 0.5f;
            dl->AddText(ImVec2(keyboardLeft + 2.0f, textY), IM_COL32(45, 45, 45, 230), noteLabel);
        }
        dl->PopClipRect();
    }
    dl->AddRect(ImVec2(keyboardLeft, rollTop), ImVec2(keyboardRight, rollBottom),
                IM_COL32(track.color.r, track.color.g, track.color.b, 210));
    if(pianoKeyboardPreviewActive && pianoKeyboardPreviewTrackId == track.id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if(!pianoKeyboardPreviewGateBindingId.empty()) timeline.clearLiveOverride(track.id, pianoKeyboardPreviewGateBindingId);
        if(!pianoKeyboardPreviewPitchBindingId.empty()) timeline.clearLiveOverride(track.id, pianoKeyboardPreviewPitchBindingId);
        pianoKeyboardPreviewActive = false;
        pianoKeyboardPreviewTrackId.clear();
        pianoKeyboardPreviewGateBindingId.clear();
        pianoKeyboardPreviewPitchBindingId.clear();
        pianoKeyboardPreviewPitch = -1;
    }

    // Vertical scrollbar: a thumb sized to the fraction of the full
    // 0-127 pitch range currently visible, positioned so pitch 127 is at
    // the top and pitch 0 at the bottom (matching the keyboard above).
    // Dragging pans without changing zoom; clicking the track jumps the
    // window there -- the same conventions as the horizontal timeline
    // scrollbar drawn by hand below the whole viewport. Shown on every
    // piano lane, focused or not: it only changes which keys are visible.
    {
        const int scrollRangeSize = highPitch - lowPitch + 1;
        const int maxLowPitch = std::max(0, 128 - scrollRangeSize);
        // The zoom buttons below occupy a strip at the top and bottom of
        // the scrollbar's own column; the draggable track fills what's
        // left between them.
        const float pianoTrackTop = std::min(rollBottom, rollTop + kPianoZoomButtonHeight + 2.0f);
        const float pianoTrackBottom = std::max(pianoTrackTop, rollBottom - kPianoZoomButtonHeight - 2.0f);
        const float scrollTrackHeight = std::max(1.0f, pianoTrackBottom - pianoTrackTop);
        const float pianoThumbHeight = std::min(scrollTrackHeight,
            std::max(16.0f, scrollTrackHeight * static_cast<float>(scrollRangeSize) / 128.0f));
        const float pianoThumbTravel = std::max(0.0f, scrollTrackHeight - pianoThumbHeight);
        const float pianoLowFraction = maxLowPitch > 0 ? static_cast<float>(lowPitch) / static_cast<float>(maxLowPitch) : 0.0f;
        const float pianoThumbTop = pianoTrackTop + pianoThumbTravel * (1.0f - pianoLowFraction);
        const ImVec2 pianoScrollThumbMin(pianoScrollbarLeft, pianoThumbTop);
        const ImVec2 pianoScrollThumbMax(pianoScrollbarRight, pianoThumbTop + pianoThumbHeight);

        dl->AddRectFilled(ImVec2(pianoScrollbarLeft, pianoTrackTop), ImVec2(pianoScrollbarRight, pianoTrackBottom),
                          IM_COL32(20, 20, 20, 255));

        ImGui::SetCursorScreenPos(ImVec2(pianoScrollbarLeft, pianoTrackTop));
        ImGui::InvisibleButton(("##pianoVScrollbar" + lane->id).c_str(),
                               ImVec2(std::max(1.0f, pianoScrollbarRight - pianoScrollbarLeft), scrollTrackHeight));
        const bool pianoThumbHovered = ImGui::IsItemHovered();
        const bool pianoThumbActive = ImGui::IsItemActive();
        if(maxLowPitch > 0) {
            if(ImGui::IsItemClicked() && !hoverRect(pianoScrollThumbMin, pianoScrollThumbMax)) {
                const float targetThumbTop = ofClamp(ImGui::GetIO().MousePos.y - pianoThumbHeight * 0.5f,
                                                     pianoTrackTop, pianoTrackTop + pianoThumbTravel);
                const float targetFraction = 1.0f - (targetThumbTop - pianoTrackTop) / std::max(1.0f, pianoThumbTravel);
                lane->pianoLowPitch = ofClamp(static_cast<int>(std::lround(targetFraction * maxLowPitch)), 0, maxLowPitch);
                lane->pianoHighPitch = lane->pianoLowPitch + scrollRangeSize - 1;
            }
            if(pianoThumbActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const float deltaFraction = -ImGui::GetIO().MouseDelta.y / std::max(1.0f, pianoThumbTravel);
                const int newLow = ofClamp(lane->pianoLowPitch + static_cast<int>(std::lround(deltaFraction * maxLowPitch)),
                                           0, maxLowPitch);
                lane->pianoLowPitch = newLow;
                lane->pianoHighPitch = newLow + scrollRangeSize - 1;
            }
        }
        dl->AddRectFilled(pianoScrollThumbMin, pianoScrollThumbMax,
                          pianoThumbActive ? IM_COL32(150, 150, 160, 255)
                                            : pianoThumbHovered ? IM_COL32(120, 120, 130, 255) : IM_COL32(90, 90, 100, 255), 3.0f);

        // "+"/"-" buttons flanking the scrollbar zoom the visible pitch
        // range (i.e. change note height) around its current center.
        // This replaces an earlier Ctrl+wheel zoom gesture: the piano
        // roll canvas is drawn inside the same ImGui window as the rest
        // of the timeline, so any wheel motion over it also triggered
        // the timeline's own horizontal zoom regardless of modifier
        // keys, and the two clashed instead of resizing the notes.
        const float pianoZoomButtonWidth = pianoScrollbarRight - pianoScrollbarLeft;
        const int pianoZoomCenterPitch = (lowPitch + highPitch) / 2;
        const auto applyPianoZoom = [&](float factor) {
            const int newRangeSize = std::clamp(static_cast<int>(std::lround(scrollRangeSize * factor)), 1, 128);
            int newHigh = pianoZoomCenterPitch + newRangeSize / 2;
            newHigh = ofClamp(newHigh, newRangeSize - 1, 127);
            lane->pianoLowPitch = newHigh - newRangeSize + 1;
            lane->pianoHighPitch = newHigh;
        };
        // Drawn by hand (InvisibleButton + manual fill/text) rather
        // than ImGui::Button, matching every other absolutely-positioned
        // control on this canvas -- a themed Button here came out
        // invisible: it's added to the same window draw list as the
        // hand-drawn scrollbar, and at this 10x14 size ImGui's own
        // frame padding could make it decide there was nothing worth
        // submitting.
        const auto drawPianoZoomButton = [&](const std::string& idSuffix, const ImVec2& buttonMin, const char* glyph, float factor) {
            const ImVec2 buttonMax(buttonMin.x + pianoZoomButtonWidth, buttonMin.y + kPianoZoomButtonHeight);
            ImGui::SetCursorScreenPos(buttonMin);
            ImGui::InvisibleButton(("##pianoZoom" + idSuffix + lane->id).c_str(), ImVec2(pianoZoomButtonWidth, kPianoZoomButtonHeight));
            const bool zoomHovered = ImGui::IsItemHovered();
            const bool zoomActive = ImGui::IsItemActive();
            dl->AddRectFilled(buttonMin, buttonMax,
                              zoomActive ? IM_COL32(150, 150, 160, 255) : zoomHovered ? IM_COL32(110, 110, 120, 255) : IM_COL32(60, 60, 66, 255), 2.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(glyph);
            dl->AddText(ImVec2(buttonMin.x + (pianoZoomButtonWidth - textSize.x) * 0.5f,
                               buttonMin.y + (kPianoZoomButtonHeight - textSize.y) * 0.5f),
                       IM_COL32(230, 230, 235, 255), glyph);
            if(ImGui::IsItemClicked()) applyPianoZoom(factor);
        };
        drawPianoZoomButton("In", ImVec2(pianoScrollbarLeft, rollTop), "+", 1.0f / 1.2f);
        drawPianoZoomButton("Out", ImVec2(pianoScrollbarLeft, rollBottom - kPianoZoomButtonHeight), "-", 1.2f);
    }

    dl->AddRectFilled(ImVec2(left, velocityTop), ImVec2(right, velocityBottom), IM_COL32(28, 28, 31, 255));
    dl->AddRectFilled(ImVec2(left, probabilityTop), ImVec2(right, probabilityBottom), IM_COL32(24, 24, 27, 255));
    dl->AddRectFilled(ImVec2(left, mutationTop), ImVec2(right, mutationBottom), IM_COL32(27, 24, 30, 255));
    dl->AddText(ImVec2(left + 4.0f, velocityTop + 5.0f), IM_COL32(145, 145, 150, 210), "VEL");
    dl->AddText(ImVec2(left + 4.0f, probabilityTop + 5.0f), IM_COL32(145, 145, 150, 210), "PROB");
    dl->AddText(ImVec2(left + 4.0f, mutationTop + 4.0f), IM_COL32(150, 140, 165, 210), "MUT");
    for(int pitch = lowPitch; pitch <= highPitch; ++pitch) {
        const float y = rollBottom - (pitch - lowPitch + 1) * pitchHeight;
        const int noteClass = pitch % 12;
        const bool blackKey = noteClass == 1 || noteClass == 3 || noteClass == 6 || noteClass == 8 || noteClass == 10;
        if(blackKey) dl->AddRectFilled(ImVec2(left, y), ImVec2(right, y + pitchHeight), IM_COL32(255, 255, 255, 8));
        // Scale overlay: keys outside the scale are shaded, scale keys tinted.
        if(lane->pianoScaleEnabled)
            dl->AddRectFilled(ImVec2(left, y), ImVec2(right, y + pitchHeight),
                              inPianoScale(*lane, pitch) ? IM_COL32(120, 175, 235, 14) : IM_COL32(0, 0, 0, 70));
        // `y` is the top edge of this pitch's row, i.e. the boundary with
        // the row above it. The octave separator belongs below C (the
        // B/C boundary), which is the top edge of B's own row (noteClass
        // 11) -- not the top edge of C's row (that would be the C/C#
        // boundary, one semitone too high).
        dl->AddLine(ImVec2(left, y), ImVec2(right, y), noteClass == 11 ? IM_COL32(125, 125, 125, 165) : IM_COL32(65, 65, 65, 100));
    }
    const double grid = std::max(1.0 / kPPQ, lane->beatsPerStep);
    const double content = sourceDuration(*clip);
    const int cycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleDuration(*clip)))) : 1;
    // Absolute timeline divisions, independent of clip offset, stretch or repeats.
    const double displayGrid = view.displayGridBeats();
    const double visibleFrom = beatAtOffset(std::max(0.0f, left - timelineMin.x));
    const double visibleTo = std::min(view.endBeat, beatAtOffset(std::max(0.0f, right - timelineMin.x)) + displayGrid);
    // Only the repeats reaching the visible part of the canvas are drawn and
    // hit-tested: a short pattern repeated over a long clip has thousands.
    int firstCycle = 0, lastCycle = cycles - 1;
    if(clip->repeatContent && cycles > 1) {
        const double cycleLength = std::max(1e-6, cycleDuration(*clip));
        firstCycle = ofClamp(static_cast<int>(std::floor((visibleFrom - clip->startBeat) / cycleLength)), 0, cycles - 1);
        lastCycle = ofClamp(static_cast<int>(std::floor((visibleTo - clip->startBeat) / cycleLength)), firstCycle, cycles - 1);
    }
    const int firstLine = std::max(0, static_cast<int>(std::floor(visibleFrom / displayGrid)));
    const int lastLine = static_cast<int>(std::ceil(visibleTo / displayGrid));
    for(int i = firstLine; i <= lastLine; ++i) {
        const double beat = i * displayGrid;
        const float x = timelineMin.x + beatOffset(beat);
        if(x < left || x > right) continue;
        const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
        const bool quarter = std::fmod(beat, 1.0) < 0.001;
        if(bar || quarter || beatOffset(beat + displayGrid) - beatOffset(beat) >= 4.0f)
            dl->AddLine(ImVec2(x, rollTop), ImVec2(x, stripsBottom),
                        bar ? kBar : quarter ? IM_COL32(92, 92, 92, 145) : kGrid, bar ? 1.5f : 1.0f);
    }
    // Keep source-cycle boundaries visible without restarting the musical grid.
    for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        if(clip->repeatContent && cycle > 0) {
            const float x = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, 0.0, cycle));
            if(x < left || x > right) continue;
            dl->AddLine(ImVec2(x, rollTop), ImVec2(x, stripsBottom), IM_COL32(track.color.r, track.color.g, track.color.b, 225), 2.0f);
        }
    }

    // While the Mutate tab shows a group, its notes light up (others dim).
    const bool groupFocus = mutationEditLaneId == lane->id && mutationTabFrame >= ImGui::GetFrameCount() - 1;
    dl->PushClipRect(ImVec2(left, rollTop), ImVec2(right, stripsBottom), true);
    for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        for(size_t noteIndex = 0; noteIndex < lane->pianoNotes.size(); ++noteIndex) {
            const auto& note = lane->pianoNotes[noteIndex];
            // Notes outside the shown keys are not drawn, but their VEL / PROB /
            // MUT handles always are.
            const int soundingPitch = sounding(note.pitch);
            const bool shown = soundingPitch >= lowPitch && soundingPitch <= highPitch;
            const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat, cycle));
            const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat + note.durationBeats, cycle));
            const float y2 = rollBottom - (soundingPitch - lowPitch) * pitchHeight;
            const float y1 = y2 - pitchHeight + 1.0f;
            const bool selected = isFocused && pianoSelectedNoteIndices.count(static_cast<int>(noteIndex)) > 0;
            if(soundingPitch != note.pitch && note.pitch >= lowPitch && note.pitch <= highPitch) {
                // the ghost: where the note is stored
                const float gy2 = rollBottom - (note.pitch - lowPitch) * pitchHeight;
                const float gy1 = gy2 - pitchHeight + 1.0f;
                dl->AddRect(ImVec2(x1 + 1.0f, gy1), ImVec2(x2 - 1.0f, gy2),
                            selected ? IM_COL32(250, 210, 90, 150) : IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 110 : 60), 2.0f, 0, 1.0f);
            }
            if(shown) {
                dl->AddRectFilled(ImVec2(x1 + 1.0f, y1), ImVec2(x2 - 1.0f, y2),
                                  selected ? IM_COL32(250, 210, 90, 245) : IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 225 : 135), 2.0f);
                dl->AddLine(ImVec2(x2 - 3.0f, y1 + 1.0f), ImVec2(x2 - 3.0f, y2 - 1.0f), IM_COL32(255, 255, 255, 180), 1.0f);
            }
            const float handleX = x1 + 3.0f;
            const float velocityY = velocityBottom - ofClamp(note.velocity, 0.0f, 1.0f) * (velocityBottom - velocityTop - 3.0f);
            const float probabilityY = probabilityBottom - ofClamp(note.probability, 0.0f, 1.0f) * (probabilityBottom - probabilityTop - 3.0f);
            dl->AddLine(ImVec2(handleX, velocityBottom - 2.0f), ImVec2(handleX, velocityY), IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 235 : 125), 3.0f);
            dl->AddCircleFilled(ImVec2(handleX, velocityY), 3.5f, IM_COL32(240, 240, 245, cycle == 0 ? 245 : 135));
            dl->AddLine(ImVec2(handleX, probabilityBottom - 2.0f), ImVec2(handleX, probabilityY), IM_COL32(245, 185, 80, cycle == 0 ? 235 : 125), 3.0f);
            dl->AddCircleFilled(ImVec2(handleX, probabilityY), 3.5f, IM_COL32(250, 225, 150, cycle == 0 ? 245 : 135));
            const float mutationY = mutationBottom - ofClamp(note.mutation, 0.0f, 1.0f) * (mutationBottom - mutationTop - 3.0f);
            const int group = note.mutationGroup;
            dl->AddLine(ImVec2(handleX, mutationBottom - 2.0f), ImVec2(handleX, mutationY), mutationGroupColor(group, cycle == 0 ? 235 : 125), 3.0f);
            dl->AddCircleFilled(ImVec2(handleX, mutationY), 3.5f, mutationGroupColor(group, cycle == 0 ? 255 : 135));
            // A note that may mutate wears an outline in its group's colour.
            const bool lit = groupFocus && group == mutationEditGroup;
            if(shown && (note.mutation > 0.0f || lit))
                dl->AddRect(ImVec2(x1 + 1.0f, y1), ImVec2(x2 - 1.0f, y2),
                            mutationGroupColor(group, lit ? 255 : groupFocus ? 80 : cycle == 0 ? 220 : 120), 2.0f, 0, lit ? 2.5f : 1.5f);
        }
    }
    // While playing, the repeat under the playhead shows where its mutated
    // notes actually play (outlines; the written notes stay editable).
    std::vector<ofxOceanodeTimelinePianoNote> playedNotes;
    const double playhead = canvas.beatPosition;
    if(context.container.getTransportState().isPlaying &&
       playhead >= clip->startBeat && playhead < clip->startBeat + clip->durationBeats) {
        const int64_t repeat = cycleIndex(*clip, playhead);
        if(timeline.pianoRepeatNotes(track.id, clip->id, lane->id, repeat, playedNotes)) {
            for(size_t n = 0; n < playedNotes.size() && n < lane->pianoNotes.size(); ++n) {
                const auto& played = playedNotes[n];
                const auto& written = lane->pianoNotes[n];
                if(played.pitch == written.pitch && played.startBeat == written.startBeat &&
                   played.durationBeats == written.durationBeats) continue;
                const int playedPitch = sounding(played.pitch);
                if(playedPitch < lowPitch || playedPitch > highPitch) continue;
                const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, played.startBeat, repeat));
                const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, played.startBeat + played.durationBeats, repeat));
                const float y2 = rollBottom - (playedPitch - lowPitch) * pitchHeight;
                const float y1 = y2 - pitchHeight + 1.0f;
                dl->AddRectFilled(ImVec2(x1 + 1.0f, y1), ImVec2(x2 - 1.0f, y2), mutationGroupColor(written.mutationGroup, 70), 2.0f);
                dl->AddRect(ImVec2(x1 + 1.0f, y1), ImVec2(x2 - 1.0f, y2), mutationGroupColor(written.mutationGroup, 235), 2.0f, 0, 1.5f);
            }
        }
    }
    dl->PopClipRect();

    if(isFocused) {
    const bool hovered = hoverRect(ImVec2(left, rollTop), ImVec2(right, rollBottom));
    const bool velocityHovered = hoverRect(ImVec2(left, velocityTop), ImVec2(right, velocityBottom));
    const bool probabilityHovered = hoverRect(ImVec2(left, probabilityTop), ImVec2(right, probabilityBottom));
    const bool mutationHovered = hoverRect(ImVec2(left, mutationTop), ImVec2(right, mutationBottom));
    const bool valueStripHovered = velocityHovered || probabilityHovered || mutationHovered;
    const int hoveredField = velocityHovered ? 0 : probabilityHovered ? 1 : 2;
    // The handles in the gaps above VEL, PROB and MUT: drag to resize the strip
    {
        const float gapTop[3] = {rollBottom, velocityBottom, probabilityBottom};
        const float gapBottom[3] = {velocityTop, probabilityTop, mutationTop};
        int handle = 0;
        for(int strip = 0; strip < 3; ++strip)
            if(hoverRect(ImVec2(left, gapTop[strip] + 0.5f), ImVec2(right, gapBottom[strip] - 0.5f))) handle = strip + 1;
        if(pianoStripResize == 0 && handle != 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                pianoStripResize = handle;
                pianoStripResizeStartY = ImGui::GetIO().MousePos.y;
                pianoStripResizeStartHeight = strips[handle - 1];
            }
        }
        if(pianoStripResize != 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float h = pianoStripResizeStartHeight - (ImGui::GetIO().MousePos.y - pianoStripResizeStartY);
                strips[pianoStripResize - 1] = std::max(12.0f, h);   // clamped next frame
            } else pianoStripResize = 0;
        }
        const ImU32 handleColor = IM_COL32(150, 150, 158, 150);
        const int shown = pianoStripResize != 0 ? pianoStripResize : handle;
        if(shown != 0) {
            const float y = (gapTop[shown - 1] + gapBottom[shown - 1]) * 0.5f;
            dl->AddLine(ImVec2(left, y), ImVec2(right, y), handleColor, 2.0f);
        }
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
    double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
    const int pitch = ofClamp(highPitch - static_cast<int>((mouse.y - rollTop) / pitchHeight), lowPitch, highPitch);
    const double snappedSource = lane->pianoSnapToGrid ? std::round(sourceBeat / grid) * grid : sourceBeat;
    auto hitPianoValueHandle = [&]() {
        int result = -1;
        float nearestDistance = 8.0f;
        for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            for(int i = static_cast<int>(lane->pianoNotes.size()) - 1; i >= 0; --i) {
                const float handleX = timelineMin.x + beatOffset(
                    sourceToTimelineBeat(*clip, lane->pianoNotes[i].startBeat, cycle)) + 3.0f;
                const float distance = std::abs(mouse.x - handleX);
                if(distance <= nearestDistance) {
                    nearestDistance = distance;
                    result = i;
                }
            }
        }
        return result;
    };

    if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hitIndex = -1;
        for(int i = static_cast<int>(lane->pianoNotes.size()) - 1; i >= 0; --i) {
            const auto& note = lane->pianoNotes[i];
            if(atPitch(note, pitch) && sourceBeat >= note.startBeat && sourceBeat <= note.startBeat + note.durationBeats) {
                hitIndex = i;
                break;
            }
        }
        if(hitIndex >= 0) {
            // Right-clicking a note that's part of a multi-selection
            // deletes the whole group; right-clicking any other note
            // (selected alone, or not selected at all) deletes just it.
            std::set<int> toDelete;
            if(pianoSelectedNoteIndices.count(hitIndex) > 0 && pianoSelectedNoteIndices.size() > 1) {
                toDelete = pianoSelectedNoteIndices;
            } else {
                toDelete.insert(hitIndex);
            }
            for(auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
                if(*it >= 0 && *it < static_cast<int>(lane->pianoNotes.size()))
                    lane->pianoNotes.erase(lane->pianoNotes.begin() + *it);
            }
            pianoSelectedNoteIndices.clear();
        }
    }
    if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pianoDragNoteIndex = -1;
        pianoInsertionDrag = false;
        int hitIndex = -1;
        for(int i = static_cast<int>(lane->pianoNotes.size()) - 1; i >= 0; --i) {
            const auto& note = lane->pianoNotes[i];
            if(atPitch(note, pitch) && sourceBeat >= note.startBeat && sourceBeat <= note.startBeat + note.durationBeats) {
                hitIndex = i;
                break;
            }
        }
        const bool ctrlDown = ImGui::GetIO().KeyCtrl;
        if(hitIndex >= 0) {
            if(ctrlDown) {
                // Ctrl+click only toggles membership -- it never starts a
                // drag, so a whole selection can be built up click by
                // click before moving any of it.
                if(pianoSelectedNoteIndices.count(hitIndex) > 0) pianoSelectedNoteIndices.erase(hitIndex);
                else pianoSelectedNoteIndices.insert(hitIndex);
            } else {
                if(pianoSelectedNoteIndices.count(hitIndex) == 0) {
                    // Clicking a note outside the current selection
                    // replaces it, matching the old single-note behaviour.
                    pianoSelectedNoteIndices.clear();
                    pianoSelectedNoteIndices.insert(hitIndex);
                }
                // Otherwise the click landed on a note that's already
                // part of the selection -- keep the whole group selected
                // and drag it together.
                const auto& note = lane->pianoNotes[hitIndex];
                pianoDragNoteIndex = hitIndex;
                const double edgeThreshold = std::max(grid * 0.2,
                    std::abs(beatAtOffset(mouse.x - timelineMin.x + 6.0f) - timelineBeat));
                pianoDragMode = std::abs(sourceBeat - note.startBeat - note.durationBeats) <= edgeThreshold
                    ? PianoDragMode::Resize : PianoDragMode::Move;
                pianoDragBeatOffset = sourceBeat - note.startBeat;
                if(ImGui::GetIO().KeyShift) {
                    // Shift+drag: stretch the selection (every note when the
                    // grabbed one is not selected) from its first start
                    if(pianoSelectedNoteIndices.size() <= 1 && pianoSelectedNoteIndices.count(hitIndex) > 0
                       && !ImGui::GetIO().KeyAlt) {
                        pianoSelectedNoteIndices.clear();
                        for(size_t i = 0; i < lane->pianoNotes.size(); ++i) pianoSelectedNoteIndices.insert(static_cast<int>(i));
                    }
                    pianoDragMode = PianoDragMode::Stretch;
                    pianoDragSnapshot.clear();
                    double origin = std::numeric_limits<double>::max();
                    for(int index : pianoSelectedNoteIndices) {
                        if(index < 0 || index >= static_cast<int>(lane->pianoNotes.size())) continue;
                        const auto& n = lane->pianoNotes[index];
                        pianoDragSnapshot.push_back({index, n.startBeat, n.pitch, n.durationBeats});
                        origin = std::min(origin, n.startBeat);
                    }
                    pianoStretchOrigin = origin == std::numeric_limits<double>::max() ? 0.0 : origin;
                    pianoStretchGrab = std::max(sourceBeat, pianoStretchOrigin + grid * 0.25);
                } else if(pianoDragMode == PianoDragMode::Move) {
                    pianoDragSnapshot.clear();
                    for(int index : pianoSelectedNoteIndices) {
                        if(index >= 0 && index < static_cast<int>(lane->pianoNotes.size()))
                            pianoDragSnapshot.push_back({index, lane->pianoNotes[index].startBeat, lane->pianoNotes[index].pitch});
                    }
                    pianoDragAnchorStartBeat = note.startBeat;
                    // the row clicked: the note or its Global Pitch ghost, the drag moves it from there
                    pianoDragAnchorPitch = pitch;
                    if(ImGui::GetIO().KeyAlt) {
                        // Alt+drag duplicates the whole selection in
                        // place and drags the copies instead, leaving
                        // the originals untouched where they were.
                        // "note" and hitIndex must not be dereferenced
                        // into lane->pianoNotes again after this point --
                        // push_back below can reallocate the vector.
                        std::unordered_map<int, int> originalToDuplicate;
                        for(int index : pianoSelectedNoteIndices) {
                            if(index < 0 || index >= static_cast<int>(lane->pianoNotes.size())) continue;
                            const auto copy = lane->pianoNotes[index];
                            lane->pianoNotes.push_back(copy);
                            originalToDuplicate[index] = static_cast<int>(lane->pianoNotes.size()) - 1;
                        }
                        pianoSelectedNoteIndices.clear();
                        for(const auto& [originalIndex, duplicateIndex] : originalToDuplicate)
                            pianoSelectedNoteIndices.insert(duplicateIndex);
                        const auto foundDuplicate = originalToDuplicate.find(hitIndex);
                        if(foundDuplicate != originalToDuplicate.end()) pianoDragNoteIndex = foundDuplicate->second;
                        pianoDragSnapshot.clear();
                        for(int index : pianoSelectedNoteIndices) {
                            if(index >= 0 && index < static_cast<int>(lane->pianoNotes.size()))
                                pianoDragSnapshot.push_back({index, lane->pianoNotes[index].startBeat, lane->pianoNotes[index].pitch});
                        }
                    }
                }
            }
        } else if(ctrlDown) {
            // Ctrl+drag on empty space marquee-selects instead of
            // drawing a new note. Without Shift the previous selection
            // is replaced once the drag resolves on release.
            pianoMarqueeActive = true;
            pianoMarqueeStartX = mouse.x;
            pianoMarqueeStartY = mouse.y;
            if(!ImGui::GetIO().KeyShift) pianoSelectedNoteIndices.clear();
        } else {
            pianoSelectedNoteIndices.clear();
            const double duration = grid;
            if(lane->pianoMonophonic) {
                lane->pianoNotes.erase(std::remove_if(lane->pianoNotes.begin(), lane->pianoNotes.end(), [&](const auto& note) {
                    return note.startBeat < snappedSource + duration && note.startBeat + note.durationBeats > snappedSource;
                }), lane->pianoNotes.end());
            }
            lane->pianoNotes.push_back({std::max(0.0, snappedSource), duration, snapToPianoScale(*lane, pitch, lowPitch, highPitch),
                                        lane->pianoDefaultVelocity, 1.0f});
            pianoDragNoteIndex = static_cast<int>(lane->pianoNotes.size()) - 1;
            pianoSelectedNoteIndices.insert(pianoDragNoteIndex);
            pianoDragMode = PianoDragMode::Resize;
            pianoInsertionDrag = true;
        }
    }
    if(pianoMarqueeActive) {
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 marqueeMin(std::min(pianoMarqueeStartX, mouse.x), std::min(pianoMarqueeStartY, mouse.y));
            const ImVec2 marqueeMax(std::max(pianoMarqueeStartX, mouse.x), std::max(pianoMarqueeStartY, mouse.y));
            dl->PushClipRect(ImVec2(left, rollTop), ImVec2(right, rollBottom), true);
            dl->AddRectFilled(marqueeMin, marqueeMax, IM_COL32(250, 210, 90, 40));
            dl->AddRect(marqueeMin, marqueeMax, IM_COL32(250, 210, 90, 200));
            dl->PopClipRect();
        } else {
            const float marqueeMinX = std::min(pianoMarqueeStartX, mouse.x);
            const float marqueeMaxX = std::max(pianoMarqueeStartX, mouse.x);
            const float marqueeMinY = std::min(pianoMarqueeStartY, mouse.y);
            const float marqueeMaxY = std::max(pianoMarqueeStartY, mouse.y);
            for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                for(size_t noteIndex = 0; noteIndex < lane->pianoNotes.size(); ++noteIndex) {
                    const auto& note = lane->pianoNotes[noteIndex];
                    const int soundingPitch = sounding(note.pitch);   // the marquee picks what it shows
                    if(soundingPitch < lowPitch || soundingPitch > highPitch) continue;
                    const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat, cycle));
                    const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat + note.durationBeats, cycle));
                    const float y2 = rollBottom - (soundingPitch - lowPitch) * pitchHeight;
                    const float y1 = y2 - pitchHeight + 1.0f;
                    if(x2 >= marqueeMinX && x1 <= marqueeMaxX && y2 >= marqueeMinY && y1 <= marqueeMaxY) {
                        pianoSelectedNoteIndices.insert(static_cast<int>(noteIndex));
                    }
                }
            }
            pianoMarqueeActive = false;
        }
    }
    // Cmd+A (ImGui's logical Ctrl on macOS): every note of this lane
    if((hovered || valueStripHovered) && !ImGui::IsAnyItemActive() &&
       !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        pianoSelectedNoteIndices.clear();
        for(size_t i = 0; i < lane->pianoNotes.size(); ++i) pianoSelectedNoteIndices.insert(static_cast<int>(i));
    }
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !pianoSelectedNoteIndices.empty() && !ImGui::IsAnyItemActive() &&
       !ImGui::GetIO().WantTextInput &&
       (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        std::vector<int> sortedSelection(pianoSelectedNoteIndices.begin(), pianoSelectedNoteIndices.end());
        std::sort(sortedSelection.rbegin(), sortedSelection.rend());
        for(int index : sortedSelection) {
            if(index >= 0 && index < static_cast<int>(lane->pianoNotes.size()))
                lane->pianoNotes.erase(lane->pianoNotes.begin() + index);
        }
        pianoSelectedNoteIndices.clear();
    }
    if(valueStripHovered) {
        static const char* fieldNames[] = {"velocity", "probability", "mutation"};
        ImGui::SetTooltip("%sDrag a selected handle to scale the selection proportionally. Drag across other handles to paint %s. Right-click for a numeric value.",
                          hoveredField == 2 ? "Mutation: chance the note plays changed by the lane's Mutate rules, rolled every repeat.\n" : "",
                          fieldNames[hoveredField]);
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pianoNumericNoteIndex = hitPianoValueHandle();
            if(pianoNumericNoteIndex >= 0) {
                pianoNumericField = hoveredField == 0 ? PianoNumericField::Velocity
                    : hoveredField == 1 ? PianoNumericField::Probability : PianoNumericField::Mutation;
                pianoNumericValue = noteValue(lane->pianoNotes[pianoNumericNoteIndex], hoveredField);
                ImGui::OpenPopup("Piano note value");
            }
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pianoDragNoteIndex = hitPianoValueHandle();
            pianoDragMode = hoveredField == 0 ? PianoDragMode::Velocity
                : hoveredField == 1 ? PianoDragMode::Probability : PianoDragMode::Mutation;
            pianoValuePaintPreviousMouse = mouse;
            pianoValueSnapshot.clear();
            if(pianoDragNoteIndex >= 0 && pianoSelectedNoteIndices.count(pianoDragNoteIndex) &&
               pianoSelectedNoteIndices.size() > 1) {
                pianoValueAnchor = noteValue(lane->pianoNotes[pianoDragNoteIndex], hoveredField);
                pianoValueStartMouseY = mouse.y;
                for(int index : pianoSelectedNoteIndices) {
                    if(index >= 0 && index < static_cast<int>(lane->pianoNotes.size()))
                        pianoValueSnapshot.emplace_back(index, noteValue(lane->pianoNotes[index], hoveredField));
                }
            }
            if(pianoDragNoteIndex >= 0 && pianoSelectedNoteIndices.count(pianoDragNoteIndex) == 0) {
                pianoSelectedNoteIndices.clear();
                pianoSelectedNoteIndices.insert(pianoDragNoteIndex);
            }
        }
    }
    if(pianoDragNoteIndex >= 0 && pianoDragNoteIndex < static_cast<int>(lane->pianoNotes.size()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto& note = lane->pianoNotes[pianoDragNoteIndex];
        if(pianoDragMode == PianoDragMode::Resize) {
            const double end = lane->pianoSnapToGrid ? std::round(sourceBeat / grid) * grid : sourceBeat;
            note.durationBeats = std::max(grid, end - note.startBeat);
        } else if(pianoDragMode == PianoDragMode::Move) {
            // Move every selected note by the same delta from where it
            // started the drag, rather than snapping each one straight
            // to the mouse -- that's what keeps a multi-note selection's
            // shape intact while dragging it.
            const double newAnchorStart = sourceBeat - pianoDragBeatOffset;
            const double snappedAnchorStart = std::max(0.0, lane->pianoSnapToGrid
                ? std::round(newAnchorStart / grid) * grid : newAnchorStart);
            const double deltaBeat = snappedAnchorStart - pianoDragAnchorStartBeat;
            const int deltaPitch = pitch - pianoDragAnchorPitch;
            for(const auto& entry : pianoDragSnapshot) {
                if(entry.index < 0 || entry.index >= static_cast<int>(lane->pianoNotes.size())) continue;
                auto& dragged = lane->pianoNotes[entry.index];
                dragged.startBeat = std::max(0.0, entry.startBeat + deltaBeat);
                dragged.pitch = snapToPianoScale(*lane, ofClamp(entry.pitch + deltaPitch, lowPitch, highPitch), lowPitch, highPitch);
            }
        } else if(pianoDragMode == PianoDragMode::Stretch) {
            // Every start and end scaled from the origin, then on the grid
            // (always: Shift+drag quantizes), at least one grid step long.
            const double factor = std::max(0.0, (sourceBeat - pianoStretchOrigin) /
                                                std::max(1e-6, pianoStretchGrab - pianoStretchOrigin));
            auto onGrid = [&](double beat) { return std::round(beat / grid) * grid; };
            for(const auto& entry : pianoDragSnapshot) {
                if(entry.index < 0 || entry.index >= static_cast<int>(lane->pianoNotes.size())) continue;
                auto& stretched = lane->pianoNotes[entry.index];
                const double start = std::max(0.0, onGrid(pianoStretchOrigin + (entry.startBeat - pianoStretchOrigin) * factor));
                double end = onGrid(pianoStretchOrigin + (entry.startBeat + entry.durationBeats - pianoStretchOrigin) * factor);
                if(end < start + grid) end = start + grid;
                stretched.startBeat = start;
                stretched.durationBeats = end - start;
            }
            ImGui::SetTooltip("Stretch x %.2f (on the grid)", factor);
        }
    }
    if(lane->pianoLiveEditSound && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
       (pianoDragMode == PianoDragMode::Move || pianoInsertionDrag) &&
       pianoDragNoteIndex >= 0 && pianoDragNoteIndex < static_cast<int>(lane->pianoNotes.size())) {
        const int monitorPitch = lane->pianoNotes[pianoDragNoteIndex].pitch;
        if(!editPreviewActive || editPreviewPitch != monitorPitch) {
            timeline.setPianoKeyPreview(track.id, clip->id, lane->id, monitorPitch);
            editPreviewActive = true;
            editPreviewTrackId = track.id; editPreviewClipId = clip->id; editPreviewLaneId = lane->id;
            editPreviewGateId = lane->pianoGateBindingId; editPreviewPitchId = lane->pianoPitchBindingId;
            editPreviewPitch = monitorPitch;
        }
    }
    const int paintField = pianoDragMode == PianoDragMode::Velocity ? 0
        : pianoDragMode == PianoDragMode::Probability ? 1 : pianoDragMode == PianoDragMode::Mutation ? 2 : -1;
    if(paintField >= 0 &&
       (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
        const float top = paintField == 0 ? velocityTop : paintField == 1 ? probabilityTop : mutationTop;
        const float bottom = paintField == 0 ? velocityBottom : paintField == 1 ? probabilityBottom : mutationBottom;
        if(!pianoValueSnapshot.empty()) {
            const float delta = (pianoValueStartMouseY - mouse.y) / std::max(1.0f, bottom - top - 3.0f);
            const float anchor = ofClamp(pianoValueAnchor + delta, 0.0f, 1.0f);
            for(const auto& entry : pianoValueSnapshot) {
                if(entry.first < 0 || entry.first >= static_cast<int>(lane->pianoNotes.size())) continue;
                auto& note = lane->pianoNotes[entry.first];
                // A zero anchor has no ratio; use a shared offset to raise it from zero.
                const float value = ofClamp(pianoValueAnchor > 0.0f
                    ? entry.second * anchor / pianoValueAnchor : entry.second + anchor, 0.0f, 1.0f);
                noteValue(note, paintField) = value;
            }
        } else {
            const float dx = mouse.x - pianoValuePaintPreviousMouse.x;
            const float x1 = std::min(mouse.x, pianoValuePaintPreviousMouse.x);
            const float x2 = std::max(mouse.x, pianoValuePaintPreviousMouse.x);
            // Interpolate the entire stroke segment, including handles skipped by
            // a fast mouse movement and all notes of a chord at the same beat.
            const float radius = std::abs(dx) < 1.0f ? 8.0f : 0.0f;
            for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                for(auto& note : lane->pianoNotes) {
                    const float x = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat, cycle)) + 3.0f;
                    if(x < left || x > right || x < x1 - radius || x > x2 + radius) continue;
                    const float t = std::abs(dx) < 1.0f ? 1.0f : ofClamp((x - pianoValuePaintPreviousMouse.x) / dx, 0.0f, 1.0f);
                    const float y = pianoValuePaintPreviousMouse.y + t * (mouse.y - pianoValuePaintPreviousMouse.y);
                    const float value = ofClamp((bottom - y) / std::max(1.0f, bottom - top - 3.0f), 0.0f, 1.0f);
                    noteValue(note, paintField) = value;
                }
            }
        }
        pianoValuePaintPreviousMouse = mouse;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        pianoDragMode = PianoDragMode::None;
        pianoInsertionDrag = false;
        pianoDragNoteIndex = -1;
        pianoDragSnapshot.clear();
        pianoValueSnapshot.clear();
    }
    if(ImGui::BeginPopup("Piano note value")) {
        if(pianoNumericNoteIndex >= 0 &&
           pianoNumericNoteIndex < static_cast<int>(lane->pianoNotes.size()) &&
           pianoNumericField != PianoNumericField::None) {
            ImGui::TextUnformatted("Piano note");
            contextMenuSection("Value");
            ImGui::SetNextItemWidth(110.0f);
            const int field = pianoNumericField == PianoNumericField::Velocity ? 0
                : pianoNumericField == PianoNumericField::Probability ? 1 : 2;
            const char* labels[] = {"Velocity", "Probability", "Mutation"};
            if(ImGui::InputFloat(labels[field], &pianoNumericValue, 0.01f, 0.1f, "%.3f")) {
                pianoNumericValue = ofClamp(pianoNumericValue, 0.0f, 1.0f);
                noteValue(lane->pianoNotes[pianoNumericNoteIndex], field) = pianoNumericValue;
            }
            if(field == 2) {
                // The note's mutation group; a selected note moves the whole selection.
                const bool inSelection = pianoSelectedNoteIndices.count(pianoNumericNoteIndex) > 0 &&
                                         pianoSelectedNoteIndices.size() > 1;
                contextMenuSection(inSelection ? "Group (selected notes)" : "Group");
                auto assign = [&](int group) {
                    if(inSelection) {
                        for(int index : pianoSelectedNoteIndices)
                            if(index >= 0 && index < static_cast<int>(lane->pianoNotes.size()))
                                lane->pianoNotes[index].mutationGroup = group;
                    } else lane->pianoNotes[pianoNumericNoteIndex].mutationGroup = group;
                };
                const int current = lane->pianoNotes[pianoNumericNoteIndex].mutationGroup;
                for(int group = 0; group <= static_cast<int>(lane->mutationGroups.size()); ++group) {
                    if(group > 0) ImGui::SameLine(0.0f, 3.0f);
                    if(mutationGroupChip(group, group == current)) assign(group);
                }
                if(static_cast<int>(lane->mutationGroups.size()) + 1 < kMaxMutationGroups) {
                    ImGui::SameLine(0.0f, 3.0f);
                    if(ImGui::SmallButton("+##newMutationGroup")) {
                        lane->mutationGroups.push_back({lane->mutation, 0});
                        assign(static_cast<int>(lane->mutationGroups.size()));
                    }
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New group (follows A until changed in the Mutate tab)");
                }
            }
        }
        ImGui::EndPopup();
    }
    } // isFocused (piano roll interaction)

}

void ofxOceanodeTimelinePianoRollEditor::updateMonitoring(ofxOceanodeTimelineManager& timeline,
                                                         const std::string& activeLaneId) {
    if(!editPreviewActive) return;
    const auto* lane = timeline.getLane(editPreviewTrackId, editPreviewClipId, editPreviewLaneId);
    if(ImGui::IsMouseDown(ImGuiMouseButton_Left) && lane != nullptr && lane->pianoLiveEditSound &&
       activeLaneId == editPreviewLaneId && pianoDragMode != PianoDragMode::None) return;
    timeline.clearLiveOverride(editPreviewTrackId, editPreviewGateId);
    timeline.clearLiveOverride(editPreviewTrackId, editPreviewPitchId);
    editPreviewActive = false;
    editPreviewPitch = -1;
}

void ofxOceanodeTimelinePianoRollEditor::clearSelection() {
    pianoSelectedNoteIndices.clear();
    pianoDragSnapshot.clear();
    pianoValueSnapshot.clear();
    pianoDragMode = PianoDragMode::None;
    pianoInsertionDrag = false;
    pianoDragNoteIndex = -1;
    pianoMarqueeActive = false;
}

void ofxOceanodeTimelinePianoRollEditor::forgetTrack(const std::string& trackId) {
    if(pianoKeyboardPreviewTrackId != trackId) return;
    pianoKeyboardPreviewActive = false;
    pianoKeyboardPreviewTrackId.clear();
    pianoKeyboardPreviewGateBindingId.clear();
    pianoKeyboardPreviewPitchBindingId.clear();
    pianoKeyboardPreviewPitch = -1;
}

void ofxOceanodeTimelinePianoRollEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const int cycles = clip.repeatContent ? static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip))) : 1;
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int cycle = firstVisibleCycle; cycle < cycles && cycle <= lastVisibleCycle; ++cycle) {
        for(const auto& note : lane->pianoNotes) {
            const float nx1 = min.x + beatOffset(sourceToTimelineBeat(clip, note.startBeat, cycle));
            const float nx2 = min.x + beatOffset(sourceToTimelineBeat(clip, note.startBeat + note.durationBeats, cycle));
            const float ny = max.y - 5.0f - (note.pitch % 12) / 12.0f * (max.y - min.y - 10.0f);
            dl->AddRectFilled(ImVec2(nx1, ny - 3.0f), ImVec2(nx2, ny + 2.0f), IM_COL32(245, 245, 245, 180), 1.0f);
        }
    }
    dl->PopClipRect();
    return;

}
