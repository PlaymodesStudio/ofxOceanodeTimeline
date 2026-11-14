#include "ofxOceanodeTimelineHeldValueEditor.h"
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

void ofxOceanodeTimelineHeldValueEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    auto& sequence = context.sequence;
    auto* container = &context.container;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    const bool isFocused = context.focused;
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float left = canvas.left, right = canvas.right;
    const bool editorCanvasHovered = canvas.hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Number: free values. Dropdown: option indices of the bound
    // parameter, shown by name and picked from its list.
    const bool dropdown = lane->type == ofxOceanodeTimelineLaneType::Dropdown;
    const auto options = dropdown ? laneDropdownOptions(timeline, sequence, *container, track.id, *lane) : std::vector<std::string>();
    if(dropdown) followDropdownOptions(timeline, *lane, options);
    const char* valuePopup = dropdown ? "Dropdown option" : "Number value";
    const double content = sourceDuration(*clip);
    const double cycleLength = cycleDuration(*clip);
    const int cycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleLength))) : 1;
    const float top = editorMin.y + 8.0f, bottom = editorMax.y - 8.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double mouseBeat = beatAtOffset(mouse.x - timelineMin.x);
    const double sourceBeat = timelineToSourceBeat(*clip, mouseBeat);
    const int hoveredCycle = clip->repeatContent ? std::max(0, static_cast<int>(std::floor(
        std::max(0.0, mouseBeat - clip->startBeat) / cycleLength))) : 0;
    const bool hovered = editorCanvasHovered && !ImGui::IsPopupOpen(valuePopup) &&
        hoverRect(ImVec2(left, top), ImVec2(right, bottom));
    // Match the clip's title/half-bright plot palette, at half saturation.
    ofColor dropdownColor = track.color;
    dropdownColor.setSaturation(dropdownColor.getSaturation() * 0.5f);
    const ImU32 dropdownColors[] = {
        IM_COL32(dropdownColor.r, dropdownColor.g, dropdownColor.b, 102),
        IM_COL32(dropdownColor.r / 2, dropdownColor.g / 2, dropdownColor.b / 2, 102)
    };
    int hit = -1;
    float hitX = 0.0f;
    for(int cycle = 0; cycle < cycles; ++cycle) {
        for(size_t i = 0; i < lane->curvePoints.size(); ++i) {
            const auto& point = lane->curvePoints[i];
            const float x = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, point.beat, cycle));
            const float endX = timelineMin.x + beatOffset(sourceToTimelineBeat(*clip,
                i + 1 < lane->curvePoints.size() ? lane->curvePoints[i + 1].beat : content, cycle));
            ImU32 color = IM_COL32(track.color.r, track.color.g, track.color.b, cycle == 0 ? 65 : 30);
            // Alternate by region, independent of the option; odd-sized
            // patterns continue alternating across repeat boundaries.
            if(dropdown) {
                color = dropdownColors[(static_cast<size_t>(cycle) * lane->curvePoints.size() + i) % 2];
            }
            dl->AddRectFilled(ImVec2(x, top), ImVec2(endX, bottom), color);
            dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(235, 235, 235, 210), 2.0f);
            dl->PushClipRect(ImVec2(std::max(left, x + 3.0f), top), ImVec2(std::min(right, endX), bottom), true);
            const std::string label = dropdown ? dropdownOptionLabel(options, point.value) : ofToString(point.value, 6);
            dl->AddText(ImVec2(x + 6.0f, top + 6.0f), IM_COL32(245, 245, 245, 235), label.c_str());
            dl->PopClipRect();
            if(cycle == hoveredCycle && std::abs(mouse.x - x) <= 8.0f) {
                hit = static_cast<int>(i);
                hitX = x;
            }
        }
    }
    if(dropdown && hovered && hit >= 0)
        dl->AddLine(ImVec2(hitX, top), ImVec2(hitX, bottom),
                    ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
    if(isFocused) {
        if(hovered) ImGui::SetTooltip(dropdown
            ? "Click to select an option from here on. Drag a marker to move it.\nDouble-click or right-click a marker to change it. Shift-click to delete."
            : "Click to add a number. Drag a marker to move it.\nDouble-click or right-click a marker to edit. Shift-click to delete.");
        if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            curveDragPointIndex = hit;
            if(hit >= 0 && ImGui::GetIO().KeyShift) {
                eraseCurvePointWithTensions(*lane, static_cast<size_t>(hit));
                timeline.invalidateSchedule();
                curveDragPointIndex = -1;
            } else if(hit < 0 && !ImGui::GetIO().KeyShift) {
                const double beat = std::max(0.0, std::min(content, view.snapBeat(sourceBeat)));
                const float value = ofxOceanodeTimelineCurve::valueAtBeat(lane->curvePoints, {}, "Step", beat, 0.0f);
                timeline.setClipNumber(sequence, track.id, clip->id, lane->id, beat, value);
                for(size_t i = 0; i < lane->curvePoints.size(); ++i)
                    if(std::abs(lane->curvePoints[i].beat - beat) < 1e-6) curveValuePointIndex = static_cast<int>(i);
                curveNumericValue = value;
                ImGui::OpenPopup(valuePopup);
            }
        }
        if(hovered && hit >= 0 && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            curveValuePointIndex = hit;
            curveNumericValue = lane->curvePoints[hit].value;
            curveDragPointIndex = -1;
            ImGui::OpenPopup(valuePopup);
        }
        if(curveDragPointIndex >= 0 && curveDragPointIndex < static_cast<int>(lane->curvePoints.size()) &&
           ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsPopupOpen(valuePopup)) {
            const int i = curveDragPointIndex;
            const double minimum = i > 0 ? lane->curvePoints[i - 1].beat + 1.0 / kPPQ : 0.0;
            const double maximum = i + 1 < static_cast<int>(lane->curvePoints.size()) ? lane->curvePoints[i + 1].beat - 1.0 / kPPQ : content;
            lane->curvePoints[i].beat = std::max(minimum, std::min(std::max(minimum, maximum), view.snapBeat(sourceBeat)));
            timeline.invalidateSchedule();
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) curveDragPointIndex = -1;
        if(ImGui::BeginPopup(valuePopup)) {
            if(curveValuePointIndex >= 0 && curveValuePointIndex < static_cast<int>(lane->curvePoints.size())) {
                auto& point = lane->curvePoints[curveValuePointIndex];
                if(dropdown && !options.empty()) {
                    ImGui::TextUnformatted("Option");
                    const int current = static_cast<int>(std::lround(point.value));
                    for(int option = 0; option < static_cast<int>(options.size()); ++option) {
                        ImGui::PushID(option);
                        const auto label = dropdownOptionLabel(options, static_cast<float>(option));
                        if(ImGui::Selectable(label.c_str(), option == current)) {
                            point.value = static_cast<float>(option);
                            timeline.invalidateSchedule();
                            ImGui::CloseCurrentPopup();
                        }
                        if(option == current && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                } else if(dropdown) {
                    // the bound parameter is missing or has no options
                    ImGui::TextDisabled("No dropdown options bound");
                    int index = static_cast<int>(std::lround(point.value));
                    ImGui::SetNextItemWidth(140.0f);
                    if(ImGui::InputInt("Index", &index)) {
                        point.value = static_cast<float>(std::max(0, index));
                        timeline.invalidateSchedule();
                    }
                } else {
                    ImGui::TextUnformatted("Number");
                    ImGui::SetNextItemWidth(140.0f);
                    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    if(ImGui::InputFloat("Value", &curveNumericValue, 0.0f, 0.0f, "%.9g") && std::isfinite(curveNumericValue)) {
                        point.value = curveNumericValue;
                        timeline.invalidateSchedule();
                    }
                }
                float beat = static_cast<float>(point.beat);
                ImGui::SetNextItemWidth(140.0f);
                if(ImGui::InputFloat("Beat", &beat, 0.0f, 0.0f, "%.6g") && std::isfinite(beat)) {
                    const double minimum = curveValuePointIndex > 0 ? lane->curvePoints[curveValuePointIndex - 1].beat + 1.0 / kPPQ : 0.0;
                    const double maximum = curveValuePointIndex + 1 < static_cast<int>(lane->curvePoints.size()) ? lane->curvePoints[curveValuePointIndex + 1].beat - 1.0 / kPPQ : content;
                    point.beat = std::max(minimum, std::min(std::max(minimum, maximum), static_cast<double>(beat)));
                    timeline.invalidateSchedule();
                }
                if(ImGui::MenuItem(dropdown ? "Delete option change" : "Delete number")) {
                    eraseCurvePointWithTensions(*lane, static_cast<size_t>(curveValuePointIndex));
                    curveValuePointIndex = -1;
                    timeline.invalidateSchedule();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

}

void ofxOceanodeTimelineHeldValueEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    auto& timeline = preview.timeline;
    auto& sequence = preview.sequence;
    auto* container = &preview.container;
    const auto& track = preview.track;
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    const int firstVisibleCycle = preview.firstVisibleCycle, lastVisibleCycle = preview.lastVisibleCycle;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool dropdown = lane->type == ofxOceanodeTimelineLaneType::Dropdown;
    const auto options = dropdown ? laneDropdownOptions(timeline, sequence, *container, track.id, *lane) : std::vector<std::string>();
    // (The previewed lane is the timeline's own: only this view of it is const.)
    if(dropdown) followDropdownOptions(timeline, const_cast<ofxOceanodeTimelineLane&>(*lane), options);
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    const int cycles = clip.repeatContent ? static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip))) : 1;
    for(int cycle = firstVisibleCycle; cycle < cycles && cycle <= lastVisibleCycle; ++cycle) {
        for(const auto& point : lane->curvePoints) {
            const float x = min.x + beatOffset(sourceToTimelineBeat(clip, point.beat, cycle));
            dl->AddLine(ImVec2(x, min.y + 4.0f), ImVec2(x, max.y - 4.0f), previewInk(preview, 160));
            const std::string label = dropdown ? dropdownOptionLabel(options, point.value) : ofToString(point.value, 4);
            dl->AddText(ImVec2(x + 4.0f, min.y + 4.0f), previewInk(preview, 220), label.c_str());
        }
    }
    dl->PopClipRect();
    return;

}

// ======================= Colour lane =======================

namespace {
ImU32 colorInk(const ofFloatColor& color, int alpha = 255) {
    auto byte = [](float channel) { return static_cast<int>(ofClamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(byte(color.r), byte(color.g), byte(color.b), alpha);
}

// The lane's colour over [fromBeat, toBeat] of its content, painted between
// two screen x: in slices, as a blend follows its sigmoid, not a straight line.
void paintColorSpan(ImDrawList* dl, const ofxOceanodeTimelineLane& lane, double fromBeat, double toBeat,
                    float x1, float x2, float top, float bottom, float clipLeft, float clipRight, int alpha = 255) {
    if(x2 <= x1 || x2 < clipLeft || x1 > clipRight) return;
    const float visible1 = std::max(x1, clipLeft), visible2 = std::min(x2, clipRight);
    const int slices = std::max(1, std::min(96, static_cast<int>((visible2 - visible1) / 5.0f) + 1));
    auto beatAt = [&](float x) { return fromBeat + (toBeat - fromBeat) * static_cast<double>((x - x1) / (x2 - x1)); };
    ofFloatColor previous;
    ofxOceanodeTimelineColorAt(lane, beatAt(visible1), previous);
    for(int slice = 0; slice < slices; ++slice) {
        const float xa = visible1 + (visible2 - visible1) * slice / slices;
        const float xb = visible1 + (visible2 - visible1) * (slice + 1) / slices;
        ofFloatColor next = previous;
        // (Just short of the span's end: at it, the next key's colour begins.)
        ofxOceanodeTimelineColorAt(lane, slice + 1 == slices ? beatAt(xb) - 1e-9 : beatAt(xb), next);
        const ImU32 a = colorInk(previous, alpha), b = colorInk(next, alpha);
        dl->AddRectFilledMultiColor(ImVec2(xa, top), ImVec2(xb, bottom), a, b, b, a);
        previous = next;
    }
}

// The content beats where the lane's colour spans start and end: the lead-in
// before the first key, each key to the next, and the last key to the end.
template<typename Visit>
void forEachColorSpan(const ofxOceanodeTimelineLane& lane, double content, Visit&& visit) {
    const auto& points = lane.curvePoints;
    if(points.empty()) return;
    if(points.front().beat > 0.0) visit(0.0, std::min(content, points.front().beat));
    for(size_t i = 0; i < points.size(); ++i) {
        const double from = points[i].beat, to = i + 1 < points.size() ? points[i + 1].beat : content;
        if(from >= content) break;
        if(to > from) visit(from, std::min(content, to));
    }
}

// A key no other key shares its colour with (a cut clip's first key repeats
// the one before it): editing it must not change another.
size_t ownColorIndex(ofxOceanodeTimelineLane& lane, size_t pointIndex) {
    auto& point = lane.curvePoints[pointIndex];
    long index = std::lround(point.value);
    bool shared = index < 0 || index >= static_cast<long>(lane.colors.size());
    for(size_t i = 0; i < lane.curvePoints.size() && !shared; ++i)
        shared = i != pointIndex && std::lround(lane.curvePoints[i].value) == index;
    if(shared) {
        lane.colors.push_back(ofxOceanodeTimelineColorOfPoint(lane, point.value));
        index = static_cast<long>(lane.colors.size()) - 1;
        point.value = static_cast<float>(index);
    }
    return static_cast<size_t>(index);
}

// A new key at a beat, between its neighbours: both halves of the segment it
// cuts keep that segment's sigmoid.
int insertColorKey(ofxOceanodeTimelineLane& lane, double beat, float value) {
    auto& points = lane.curvePoints;
    auto& tensions = lane.curveTensions;
    tensions.resize(points.empty() ? 0 : points.size() - 1);
    const auto at = std::lower_bound(points.begin(), points.end(), beat,
        [](const auto& point, double b) { return point.beat < b; });
    const size_t index = static_cast<size_t>(at - points.begin());
    points.insert(at, {beat, value});
    if(points.size() >= 2) {
        const size_t cut = index == 0 ? 0 : index - 1; // the segment it lands in (the first, ahead of every key)
        const ofxOceanodeTimelineCurveTension shape = index > 0 && cut < tensions.size() ? tensions[cut] : ofxOceanodeTimelineCurveTension{};
        tensions.insert(tensions.begin() + static_cast<std::ptrdiff_t>(std::min(cut, tensions.size())), shape);
    }
    tensions.resize(points.size() - 1);
    return static_cast<int>(index);
}
}

void ofxOceanodeTimelineColorEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
    const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto* clip = &context.clip;
    auto* lane = &context.lane;
    const auto& view = context.view;
    const bool isFocused = context.focused;
    auto beatOffset = [&](double beat) { return view.beatToPixels(beat); };
    auto beatAtOffset = [&](float pixels) { return view.pixelsToBeat(pixels); };
    const auto& editorMin = canvas.min;
    const auto& editorMax = canvas.max;
    const auto& timelineMin = canvas.timelineOrigin;
    const float left = canvas.left, right = canvas.right;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const char* keyPopup = "Colour key";
    const double content = sourceDuration(*clip);
    const double cycleLength = cycleDuration(*clip);
    const int cycles = clip->repeatContent ? std::max(1, static_cast<int>(std::ceil(clip->durationBeats / cycleLength))) : 1;
    // Two bands: the colours above, their blend curves below.
    const float top = editorMin.y + 8.0f, bottom = editorMax.y - 8.0f;
    const float middle = top + (bottom - top) * 0.5f;
    const float colorBottom = middle - 3.0f, blendTop = middle + 3.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double mouseBeat = beatAtOffset(mouse.x - timelineMin.x);
    const double sourceBeat = timelineToSourceBeat(*clip, mouseBeat);
    const int hoveredCycle = clip->repeatContent ? std::max(0, static_cast<int>(std::floor(
        std::max(0.0, mouseBeat - clip->startBeat) / cycleLength))) : 0;
    const bool hovered = canvas.hovered && !ImGui::IsPopupOpen(keyPopup) &&
        hoverRect(ImVec2(left, top), ImVec2(right, bottom));
    const bool overColors = hovered && mouse.y < middle, overBlends = hovered && mouse.y >= middle;
    auto xOf = [&](double source, int cycle) { return timelineMin.x + beatOffset(sourceToTimelineBeat(*clip, source, cycle)); };

    // The colour band: what the lane plays.
    for(int cycle = 0; cycle < cycles; ++cycle)
        forEachColorSpan(*lane, content, [&](double from, double to) {
            paintColorSpan(dl, *lane, from, to, xOf(from, cycle), xOf(to, cycle), top, colorBottom, left, right, cycle == 0 ? 255 : 150);
        });
    // The blend band: each segment's sigmoid, 0 at its key, 1 at the next.
    dl->AddRectFilled(ImVec2(left, blendTop), ImVec2(right, bottom), IM_COL32(18, 18, 18, 150));
    const ImU32 blendInk = IM_COL32(track.color.r, track.color.g, track.color.b, 235);
    int blendHit = -1;
    float blendHitWidth = 1.0f;
    lane->curveTensions.resize(lane->curvePoints.empty() ? 0 : lane->curvePoints.size() - 1);
    for(int cycle = 0; cycle < cycles; ++cycle)
        for(size_t i = 0; i + 1 < lane->curvePoints.size(); ++i) {
            const float xa = xOf(lane->curvePoints[i].beat, cycle), xb = xOf(lane->curvePoints[i + 1].beat, cycle);
            if(xb <= xa || xb < left || xa > right) continue;
            const auto& tension = lane->curveTensions[i];
            const int samples = std::max(2, std::min(64, static_cast<int>((xb - xa) / 4.0f) + 2));
            ImVec2 previous(xa, bottom);
            for(int sample = 1; sample <= samples; ++sample) {
                const float linear = static_cast<float>(sample) / samples;
                const ImVec2 next(xa + (xb - xa) * linear,
                                  bottom - (bottom - blendTop) * sigmoidFlex(linear, tension.inflection, tension.steepness));
                dl->AddLine(previous, next, cycle == 0 ? blendInk : IM_COL32(track.color.r, track.color.g, track.color.b, 110), 1.5f);
                previous = next;
            }
            dl->AddLine(ImVec2(xa, blendTop), ImVec2(xa, bottom), IM_COL32(235, 235, 235, 60), 1.0f);
            // Its handle sits where the curve bends.
            const float bend = ofClamp(tension.inflection, 0.01f, 0.99f);
            const ImVec2 handle(xa + (xb - xa) * bend, bottom - (bottom - blendTop) * sigmoidFlex(bend, tension.inflection, tension.steepness));
            if(cycle == 0 && xb - xa >= 16.0f) dl->AddCircleFilled(handle, 3.5f, IM_COL32(245, 245, 245, 220));
            if(cycle == hoveredCycle && overBlends && mouse.x >= xa && mouse.x < xb) {
                blendHit = static_cast<int>(i);
                blendHitWidth = xb - xa;
            }
        }
    // The keys: a swatch each, over the band.
    int hit = -1;
    for(int cycle = 0; cycle < cycles; ++cycle)
        for(size_t i = 0; i < lane->curvePoints.size(); ++i) {
            const float x = xOf(lane->curvePoints[i].beat, cycle);
            if(x < left - 8.0f || x > right + 8.0f) continue;
            // To the right of its beat, except where that would leave the
            // content: a key at (or just before) the end sits to its left,
            // so it can still be picked up, edited and moved.
            const float contentEnd = xOf(content, cycle);
            const float swatchLeft = std::max(xOf(0.0, cycle), std::min(x - 1.0f, contentEnd - 12.0f));
            const ImVec2 swatchMin(swatchLeft, top), swatchMax(swatchLeft + 12.0f, colorBottom);
            dl->AddRectFilled(swatchMin, swatchMax, colorInk(ofxOceanodeTimelineColorOfPoint(*lane, lane->curvePoints[i].value)));
            const bool over = cycle == hoveredCycle && overColors && mouse.x >= swatchMin.x - 4.0f && mouse.x <= swatchMax.x + 2.0f;
            dl->AddRect(swatchMin, swatchMax, over ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive) : IM_COL32(245, 245, 245, 235), 0.0f, 0, over ? 2.0f : 1.0f);
            dl->AddRect(ImVec2(swatchMin.x - 1.0f, swatchMin.y - 1.0f), ImVec2(swatchMax.x + 1.0f, swatchMax.y + 1.0f), IM_COL32(0, 0, 0, 200));
            if(over) hit = static_cast<int>(i);
        }

    if(!isFocused) return;
    if(overColors) ImGui::SetTooltip("Click to add a colour key. Drag a key to move it.\nDouble-click or right-click a key to pick its colour. Shift-click to delete.");
    else if(blendHit >= 0) {
        const auto& tension = lane->curveTensions[static_cast<size_t>(blendHit)];
        ImGui::SetTooltip("Blend from key %d to %d (bend at %d%%, steepness %.2f)\nDrag sideways: where it bends. Up / down: how sharply.\nDouble-click: a straight blend.",
                          blendHit + 1, blendHit + 2, static_cast<int>(std::round(tension.inflection * 100.0f)), tension.steepness);
    }
    if(overColors && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        keyDragIndex = hit;
        if(hit >= 0 && ImGui::GetIO().KeyShift) {
            // (The last key stays: a Colour lane always has a colour.)
            if(lane->curvePoints.size() > 1) {
                eraseCurvePointWithTensions(*lane, static_cast<size_t>(hit));
                timeline.invalidateSchedule();
            }
            keyDragIndex = -1;
        } else if(hit < 0 && !ImGui::GetIO().KeyShift) {
            // A new key holds the colour playing there, so adding it changes nothing until it is edited.
            const double beat = std::max(0.0, std::min(content, view.snapBeat(sourceBeat)));
            ofFloatColor here(1.0f, 1.0f, 1.0f, 1.0f);
            ofxOceanodeTimelineColorAt(*lane, beat, here);
            const bool taken = std::any_of(lane->curvePoints.begin(), lane->curvePoints.end(),
                [&](const auto& point) { return std::abs(point.beat - beat) < 1e-6; });
            if(!taken) {
                lane->colors.push_back(here);
                keyEditIndex = insertColorKey(*lane, beat, static_cast<float>(lane->colors.size() - 1));
                timeline.invalidateSchedule();
                ImGui::OpenPopup(keyPopup);
            }
            keyDragIndex = -1;
        }
    }
    if(overColors && hit >= 0 && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        keyEditIndex = hit;
        keyDragIndex = -1;
        ImGui::OpenPopup(keyPopup);
    }
    if(keyDragIndex >= 0 && keyDragIndex < static_cast<int>(lane->curvePoints.size()) &&
       ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsPopupOpen(keyPopup)) {
        const int i = keyDragIndex;
        const double minimum = i > 0 ? lane->curvePoints[i - 1].beat + 1.0 / kPPQ : 0.0;
        const double maximum = i + 1 < static_cast<int>(lane->curvePoints.size()) ? lane->curvePoints[i + 1].beat - 1.0 / kPPQ : content;
        lane->curvePoints[i].beat = std::max(minimum, std::min(std::max(minimum, maximum), view.snapBeat(sourceBeat)));
        timeline.invalidateSchedule();
    }
    // A blend's shape, as a Project Track's easing: sideways moves the bend, up steepens it.
    if(blendHit >= 0 && keyDragIndex < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            lane->curveTensions[static_cast<size_t>(blendHit)] = ofxOceanodeTimelineCurveTension{};
            timeline.invalidateSchedule();
            blendDragIndex = -1;
        } else {
            blendDragIndex = blendHit;
            blendDragStart = mouse;
            blendDragWidth = blendHitWidth;
            blendDragInitial = lane->curveTensions[static_cast<size_t>(blendHit)];
        }
    }
    if(blendDragIndex >= 0 && blendDragIndex < static_cast<int>(lane->curveTensions.size()) &&
       ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        auto& tension = lane->curveTensions[static_cast<size_t>(blendDragIndex)];
        tension.inflection = ofClamp(blendDragInitial.inflection + (mouse.x - blendDragStart.x) / std::max(1.0f, blendDragWidth), 0.01f, 0.99f);
        tension.steepness = ofClamp(blendDragInitial.steepness * std::exp(-(mouse.y - blendDragStart.y) / 40.0f * 0.5f), 0.1f, 10.0f);
        timeline.invalidateSchedule();
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { keyDragIndex = -1; blendDragIndex = -1; }

    if(ImGui::BeginPopup(keyPopup)) {
        if(keyEditIndex >= 0 && keyEditIndex < static_cast<int>(lane->curvePoints.size())) {
            const size_t colorIndex = ownColorIndex(*lane, static_cast<size_t>(keyEditIndex));
            auto& color = lane->colors[colorIndex];
            float channels[4] = {color.r, color.g, color.b, color.a};
            if(ImGui::ColorPicker4("##colourKey", channels, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float)) {
                color = ofFloatColor(channels[0], channels[1], channels[2], channels[3]);
                timeline.invalidateSchedule();
            }
            auto& point = lane->curvePoints[static_cast<size_t>(keyEditIndex)];
            float beat = static_cast<float>(point.beat);
            ImGui::SetNextItemWidth(140.0f);
            if(ImGui::InputFloat("Beat", &beat, 0.0f, 0.0f, "%.6g") && std::isfinite(beat)) {
                const double minimum = keyEditIndex > 0 ? lane->curvePoints[static_cast<size_t>(keyEditIndex) - 1].beat + 1.0 / kPPQ : 0.0;
                const double maximum = keyEditIndex + 1 < static_cast<int>(lane->curvePoints.size())
                    ? lane->curvePoints[static_cast<size_t>(keyEditIndex) + 1].beat - 1.0 / kPPQ : content;
                point.beat = std::max(minimum, std::min(std::max(minimum, maximum), static_cast<double>(beat)));
                timeline.invalidateSchedule();
            }
            if(lane->curvePoints.size() > 1 && ImGui::MenuItem("Delete colour key")) {
                eraseCurvePointWithTensions(*lane, static_cast<size_t>(keyEditIndex));
                keyEditIndex = -1;
                timeline.invalidateSchedule();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void ofxOceanodeTimelineColorEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    if(lane == nullptr || lane->curvePoints.empty()) return;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const float left = preview.left, right = preview.right;
    auto beatOffset = [&](double beat) { return preview.view.beatToPixels(beat); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double content = sourceDuration(clip);
    const int cycles = clip.repeatContent ? static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip))) : 1;
    dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
    for(int cycle = preview.firstVisibleCycle; cycle < cycles && cycle <= preview.lastVisibleCycle; ++cycle) {
        auto xOf = [&](double source) { return min.x + beatOffset(sourceToTimelineBeat(clip, source, cycle)); };
        forEachColorSpan(*lane, content, [&](double from, double to) {
            paintColorSpan(dl, *lane, from, to, xOf(from), xOf(to), min.y + 4.0f, max.y - 4.0f, left, right, 230);
        });
        for(const auto& point : lane->curvePoints) {
            const float x = xOf(point.beat);
            dl->AddLine(ImVec2(x, min.y + 4.0f), ImVec2(x, max.y - 4.0f), previewInk(preview, 200));
        }
    }
    dl->PopClipRect();
}
