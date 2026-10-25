#include "ofxOceanodeTimelinePianoRollEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineCurve;
using namespace ofxOceanodeTimelineClipTime;

void ofxOceanodeTimelinePianoRollEditor::drawTargets(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;

    auto drawPianoRole = [&](const char* label, std::string& roleId) {
        const auto* currentBinding = roleId.empty() ? nullptr : timeline.getBinding(track.id, roleId);
        const std::string preview = currentBinding == nullptr ? "None" : compactParameterName(currentBinding->parameterPath);
        ImGui::SetNextItemWidth(labeledWidgetWidth(112.0f, label));
        if(ImGui::BeginCombo(label, preview.c_str())) {
            if(ImGui::Selectable("None", currentBinding == nullptr)) {
                const std::string previousRoleId = roleId;
                roleId.clear();
                const bool stillUsed = lane->pianoPitchBindingId == previousRoleId ||
                    lane->pianoGateBindingId == previousRoleId ||
                    lane->pianoVelocityBindingId == previousRoleId;
                if(!previousRoleId.empty() && !stillUsed)
                    timeline.removeBindingFromLane(track.id, clip->id, lane->id, previousRoleId);
            }
            for(const auto& candidate : track.bindings) {
                const bool selected = candidate.id == roleId;
                const std::string compactName = compactParameterName(candidate.parameterPath);
                if(ImGui::Selectable(compactName.c_str(), selected)) {
                    const std::string previousRoleId = roleId;
                    if(&roleId != &lane->pianoPitchBindingId && lane->pianoPitchBindingId == candidate.id) lane->pianoPitchBindingId.clear();
                    if(&roleId != &lane->pianoGateBindingId && lane->pianoGateBindingId == candidate.id) lane->pianoGateBindingId.clear();
                    if(&roleId != &lane->pianoVelocityBindingId && lane->pianoVelocityBindingId == candidate.id) lane->pianoVelocityBindingId.clear();
                    roleId = candidate.id;
                    timeline.addBindingToLane(track.id, clip->id, lane->id, candidate.id);
                    const bool previousStillUsed = lane->pianoPitchBindingId == previousRoleId ||
                        lane->pianoGateBindingId == previousRoleId ||
                        lane->pianoVelocityBindingId == previousRoleId;
                    if(!previousRoleId.empty() && previousRoleId != candidate.id && !previousStillUsed)
                        timeline.removeBindingFromLane(track.id, clip->id, lane->id, previousRoleId);
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", candidate.parameterPath.c_str());
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
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "Low"));
    ImGui::DragInt("Low", &lane->pianoLowPitch, 0.25f, 0, 127);
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "High"));
    ImGui::DragInt("High", &lane->pianoHighPitch, 0.25f, 0, 127);
    lane->pianoHighPitch = std::max(lane->pianoLowPitch, lane->pianoHighPitch);
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "Vel"));
    ImGui::SliderFloat("Vel", &lane->pianoDefaultVelocity, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Snap", &lane->pianoSnapToGrid);
    ImGui::SameLine(); ImGui::Checkbox("Mono", &lane->pianoMonophonic);
    ImGui::SetNextItemWidth(labeledWidgetWidth(94.0f, "Seed"));
    ImGui::DragInt("Seed", &lane->probabilitySeed, 1.0f, 0, 99999);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Which notes a probability below 100%% lets through.\nChange it for another pattern.");
    ImGui::SameLine();
    if(ImGui::SmallButton("New##pianoSeed")) lane->probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));

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
        pianoMarqueeActive = false;
    }
    const float rollTop = editorMin.y + 8.0f;
    // VEL / PROB strip heights: dragged from the handles above them (the
    // roll keeps at least 40 px)
    auto& strips = pianoStripHeights.try_emplace(lane->id, 25.0f, 26.0f).first->second;
    {
        const float room = std::max(24.0f, (editorMax.y - 15.0f) - rollTop - 40.0f);
        strips.first = ofClamp(strips.first, 12.0f, std::max(12.0f, room - 12.0f));
        strips.second = ofClamp(strips.second, 12.0f, std::max(12.0f, room - strips.first));
    }
    const float probabilityBottom = editorMax.y - 6.0f;
    const float probabilityTop = probabilityBottom - strips.second;
    const float velocityBottom = probabilityTop - 4.0f;
    const float velocityTop = velocityBottom - strips.first;
    const float rollBottom = velocityTop - 5.0f;
    const float keyboardRight = zoneLeft - 1.0f;
    const float keyboardLeft = keyboardRight - kPianoKeyboardWidth;
    const float pianoScrollbarRight = keyboardLeft - 4.0f;
    const float pianoScrollbarLeft = pianoScrollbarRight - kPianoScrollbarWidth;

    const int lowPitch = ofClamp(lane->pianoLowPitch, 0, 127);
    const int highPitch = ofClamp(lane->pianoHighPitch, lowPitch, 127);
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
    // scrollbar drawn by hand below the whole viewport.
    if(isFocused) {
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
    dl->AddText(ImVec2(left + 4.0f, velocityTop + 5.0f), IM_COL32(145, 145, 150, 210), "VEL");
    dl->AddText(ImVec2(left + 4.0f, probabilityTop + 5.0f), IM_COL32(145, 145, 150, 210), "PROB");
    for(int pitch = lowPitch; pitch <= highPitch; ++pitch) {
        const float y = rollBottom - (pitch - lowPitch + 1) * pitchHeight;
        const int noteClass = pitch % 12;
        const bool blackKey = noteClass == 1 || noteClass == 3 || noteClass == 6 || noteClass == 8 || noteClass == 10;
        if(blackKey) dl->AddRectFilled(ImVec2(left, y), ImVec2(right, y + pitchHeight), IM_COL32(255, 255, 255, 8));
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
    const int firstLine = std::max(0, static_cast<int>(std::floor(visibleFrom / displayGrid)));
    const int lastLine = static_cast<int>(std::ceil(visibleTo / displayGrid));
    for(int i = firstLine; i <= lastLine; ++i) {
        const double beat = i * displayGrid;
        const float x = timelineMin.x + beatOffset(beat);
        if(x < left || x > right) continue;
        const bool bar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
        const bool quarter = std::fmod(beat, 1.0) < 0.001;
        if(bar || quarter || beatOffset(beat + displayGrid) - beatOffset(beat) >= 4.0f)
            dl->AddLine(ImVec2(x, rollTop), ImVec2(x, probabilityBottom),
                        bar ? kBar : quarter ? IM_COL32(92, 92, 92, 145) : kGrid, bar ? 1.5f : 1.0f);
    }
    // Keep source-cycle boundaries visible without restarting the musical grid.
    for(int cycle = 0; cycle < cycles; ++cycle) {
        if(clip->repeatContent && cycle > 0) {
            const float x = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, 0.0, cycle));
            if(x < left || x > right) continue;
            dl->AddLine(ImVec2(x, rollTop), ImVec2(x, probabilityBottom), IM_COL32(track.color.r, track.color.g, track.color.b, 225), 2.0f);
        }
    }

    dl->PushClipRect(ImVec2(left, rollTop), ImVec2(right, probabilityBottom), true);
    for(int cycle = 0; cycle < cycles; ++cycle) {
        for(size_t noteIndex = 0; noteIndex < lane->pianoNotes.size(); ++noteIndex) {
            const auto& note = lane->pianoNotes[noteIndex];
            if(note.pitch < lowPitch || note.pitch > highPitch) continue;
            const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat, cycle));
            const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat + note.durationBeats, cycle));
            const float y2 = rollBottom - (note.pitch - lowPitch) * pitchHeight;
            const float y1 = y2 - pitchHeight + 1.0f;
            const bool selected = isFocused && pianoSelectedNoteIndices.count(static_cast<int>(noteIndex)) > 0;
            dl->AddRectFilled(ImVec2(x1 + 1.0f, y1), ImVec2(x2 - 1.0f, y2),
                              selected ? IM_COL32(250, 210, 90, 245) : IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 225 : 135), 2.0f);
            dl->AddLine(ImVec2(x2 - 3.0f, y1 + 1.0f), ImVec2(x2 - 3.0f, y2 - 1.0f), IM_COL32(255, 255, 255, 180), 1.0f);
            const float handleX = x1 + 3.0f;
            const float velocityY = velocityBottom - ofClamp(note.velocity, 0.0f, 1.0f) * (velocityBottom - velocityTop - 3.0f);
            const float probabilityY = probabilityBottom - ofClamp(note.probability, 0.0f, 1.0f) * (probabilityBottom - probabilityTop - 3.0f);
            dl->AddLine(ImVec2(handleX, velocityBottom - 2.0f), ImVec2(handleX, velocityY), IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 235 : 125), 3.0f);
            dl->AddCircleFilled(ImVec2(handleX, velocityY), 3.5f, IM_COL32(240, 240, 245, cycle == 0 ? 245 : 135));
            dl->AddLine(ImVec2(handleX, probabilityBottom - 2.0f), ImVec2(handleX, probabilityY), IM_COL32(245, 185, 80, cycle == 0 ? 235 : 125), 3.0f);
            dl->AddCircleFilled(ImVec2(handleX, probabilityY), 3.5f, IM_COL32(250, 225, 150, cycle == 0 ? 245 : 135));
        }
    }
    dl->PopClipRect();

    if(isFocused) {
    const bool hovered = hoverRect(ImVec2(left, rollTop), ImVec2(right, rollBottom));
    const bool velocityHovered = hoverRect(ImVec2(left, velocityTop), ImVec2(right, velocityBottom));
    const bool probabilityHovered = hoverRect(ImVec2(left, probabilityTop), ImVec2(right, probabilityBottom));
    // The handles in the gaps above VEL and PROB: drag to resize the strip
    {
        const bool velHandle = hoverRect(ImVec2(left, rollBottom + 0.5f), ImVec2(right, velocityTop - 0.5f));
        const bool probHandle = hoverRect(ImVec2(left, velocityBottom + 0.5f), ImVec2(right, probabilityTop - 0.5f));
        if(pianoStripResize == 0 && (velHandle || probHandle)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                pianoStripResize = velHandle ? 1 : 2;
                pianoStripResizeStartY = ImGui::GetIO().MousePos.y;
                pianoStripResizeStartHeight = velHandle ? strips.first : strips.second;
            }
        }
        if(pianoStripResize != 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float h = pianoStripResizeStartHeight - (ImGui::GetIO().MousePos.y - pianoStripResizeStartY);
                (pianoStripResize == 1 ? strips.first : strips.second) = std::max(12.0f, h);   // clamped next frame
            } else pianoStripResize = 0;
        }
        const ImU32 handleColor = IM_COL32(150, 150, 158, 150);
        if(velHandle || pianoStripResize == 1)
            dl->AddLine(ImVec2(left, rollBottom + 2.5f), ImVec2(right, rollBottom + 2.5f), handleColor, 2.0f);
        if(probHandle || pianoStripResize == 2)
            dl->AddLine(ImVec2(left, velocityBottom + 2.0f), ImVec2(right, velocityBottom + 2.0f), handleColor, 2.0f);
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double timelineBeat = beatAtOffset(mouse.x - timelineMin.x);
    double sourceBeat = timelineToSourceBeat(*clip, timelineBeat);
    const int pitch = ofClamp(highPitch - static_cast<int>((mouse.y - rollTop) / pitchHeight), lowPitch, highPitch);
    const double snappedSource = lane->pianoSnapToGrid ? std::round(sourceBeat / grid) * grid : sourceBeat;
    auto hitPianoValueHandle = [&]() {
        int result = -1;
        float nearestDistance = 8.0f;
        for(int cycle = 0; cycle < cycles; ++cycle) {
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
            if(note.pitch == pitch && sourceBeat >= note.startBeat && sourceBeat <= note.startBeat + note.durationBeats) {
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
        int hitIndex = -1;
        for(int i = static_cast<int>(lane->pianoNotes.size()) - 1; i >= 0; --i) {
            const auto& note = lane->pianoNotes[i];
            if(note.pitch == pitch && sourceBeat >= note.startBeat && sourceBeat <= note.startBeat + note.durationBeats) {
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
                    pianoDragAnchorPitch = note.pitch;
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
            lane->pianoNotes.push_back({std::max(0.0, snappedSource), duration, pitch, lane->pianoDefaultVelocity, 1.0f});
            pianoDragNoteIndex = static_cast<int>(lane->pianoNotes.size()) - 1;
            pianoSelectedNoteIndices.insert(pianoDragNoteIndex);
            pianoDragMode = PianoDragMode::Resize;
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
            for(int cycle = 0; cycle < cycles; ++cycle) {
                for(size_t noteIndex = 0; noteIndex < lane->pianoNotes.size(); ++noteIndex) {
                    const auto& note = lane->pianoNotes[noteIndex];
                    if(note.pitch < lowPitch || note.pitch > highPitch) continue;
                    const float x1 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat, cycle));
                    const float x2 = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, note.startBeat + note.durationBeats, cycle));
                    const float y2 = rollBottom - (note.pitch - lowPitch) * pitchHeight;
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
    if((hovered || velocityHovered || probabilityHovered) && !ImGui::IsAnyItemActive() &&
       !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        pianoSelectedNoteIndices.clear();
        for(size_t i = 0; i < lane->pianoNotes.size(); ++i) pianoSelectedNoteIndices.insert(static_cast<int>(i));
    }
    if(hovered && !pianoSelectedNoteIndices.empty() && !ImGui::IsAnyItemActive() &&
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
    if(velocityHovered || probabilityHovered) {
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pianoNumericNoteIndex = hitPianoValueHandle();
            if(pianoNumericNoteIndex >= 0) {
                pianoNumericField = velocityHovered
                    ? PianoNumericField::Velocity : PianoNumericField::Probability;
                const auto& note = lane->pianoNotes[pianoNumericNoteIndex];
                pianoNumericValue = pianoNumericField == PianoNumericField::Velocity
                    ? note.velocity : note.probability;
                ImGui::OpenPopup("Piano note value");
            }
        } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pianoDragNoteIndex = hitPianoValueHandle();
            if(pianoDragNoteIndex >= 0) {
                pianoDragMode = velocityHovered ? PianoDragMode::Velocity : PianoDragMode::Probability;
                if(pianoSelectedNoteIndices.count(pianoDragNoteIndex) == 0) {
                    pianoSelectedNoteIndices.clear();
                    pianoSelectedNoteIndices.insert(pianoDragNoteIndex);
                }
                const auto& anchorNote = lane->pianoNotes[pianoDragNoteIndex];
                pianoValueDragAnchorValue = pianoDragMode == PianoDragMode::Velocity
                    ? anchorNote.velocity : anchorNote.probability;
                pianoValueDragSnapshot.clear();
                for(int index : pianoSelectedNoteIndices) {
                    if(index < 0 || index >= static_cast<int>(lane->pianoNotes.size())) continue;
                    const auto& selectedNote = lane->pianoNotes[index];
                    pianoValueDragSnapshot.push_back({index,
                        pianoDragMode == PianoDragMode::Velocity
                            ? selectedNote.velocity : selectedNote.probability});
                }
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
                dragged.pitch = ofClamp(entry.pitch + deltaPitch, lowPitch, highPitch);
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
        } else if(pianoDragMode == PianoDragMode::Velocity) {
            const float targetValue = ofClamp((velocityBottom - mouse.y) /
                std::max(1.0f, velocityBottom - velocityTop - 3.0f), 0.0f, 1.0f);
            const float delta = targetValue - pianoValueDragAnchorValue;
            for(const auto& entry : pianoValueDragSnapshot) {
                if(entry.index < 0 || entry.index >= static_cast<int>(lane->pianoNotes.size())) continue;
                lane->pianoNotes[entry.index].velocity = ofClamp(entry.value + delta, 0.0f, 1.0f);
            }
        } else if(pianoDragMode == PianoDragMode::Probability) {
            const float targetValue = ofClamp((probabilityBottom - mouse.y) /
                std::max(1.0f, probabilityBottom - probabilityTop - 3.0f), 0.0f, 1.0f);
            const float delta = targetValue - pianoValueDragAnchorValue;
            for(const auto& entry : pianoValueDragSnapshot) {
                if(entry.index < 0 || entry.index >= static_cast<int>(lane->pianoNotes.size())) continue;
                lane->pianoNotes[entry.index].probability = ofClamp(entry.value + delta, 0.0f, 1.0f);
            }
        }
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        pianoDragMode = PianoDragMode::None;
        pianoDragNoteIndex = -1;
        pianoDragSnapshot.clear();
        pianoValueDragSnapshot.clear();
    }
    if(ImGui::BeginPopup("Piano note value")) {
        if(pianoNumericNoteIndex >= 0 &&
           pianoNumericNoteIndex < static_cast<int>(lane->pianoNotes.size()) &&
           pianoNumericField != PianoNumericField::None) {
            ImGui::TextUnformatted("Piano note");
            contextMenuSection("Value");
            ImGui::SetNextItemWidth(110.0f);
            const char* label = pianoNumericField == PianoNumericField::Velocity
                ? "Velocity" : "Probability";
            if(ImGui::InputFloat(label, &pianoNumericValue, 0.01f, 0.1f, "%.3f")) {
                pianoNumericValue = ofClamp(pianoNumericValue, 0.0f, 1.0f);
                auto& note = lane->pianoNotes[pianoNumericNoteIndex];
                if(pianoNumericField == PianoNumericField::Velocity) note.velocity = pianoNumericValue;
                else note.probability = pianoNumericValue;
            }
        }
        ImGui::EndPopup();
    }
    } // isFocused (piano roll interaction)

}

void ofxOceanodeTimelinePianoRollEditor::clearSelection() {
    pianoSelectedNoteIndices.clear();
    pianoDragSnapshot.clear();
    pianoValueDragSnapshot.clear();
    pianoDragMode = PianoDragMode::None;
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
