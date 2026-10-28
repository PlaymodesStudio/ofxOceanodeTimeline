#include "ofxOceanodeTimelineDrumGridEditor.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ofxOceanodeTimelineLaneEditorUi;
using namespace ofxOceanodeTimelineEditorUi;
using namespace ofxOceanodeTimelineClipTime;
namespace DrumGrid = ofxOceanodeTimelineDrumGrid;

namespace {
using StripMode = ofxOceanodeTimelineDrumGridEditor::StripMode;
using Step = ofxOceanodeTimelineDrumStep;
using Row = ofxOceanodeTimelineDrumRow;
constexpr int kModes = ofxOceanodeTimelineDrumGridEditor::kStripModeCount;
// Short forms only where the full words do not fit the zone.
constexpr const char* kStripLabels[kModes] = {"Vel", "Pitch", "Prob", "Ratch", "Gate", "Dur", "Filt"};
constexpr const char* kStripNames[kModes] = {"Velocity", "Pitch", "Probability", "Ratchet", "Gate", "Duration", "Filter"};
constexpr const char* kStripHints[kModes] = {
    "How hard the hit plays (the gate level).",
    "Semitones from the piece's own tuning (60 = as tuned).",
    "Chance the hit plays, rolled every pattern repeat.",
    "Evenly spaced sub-hits inside the hit's length, each its own gate.",
    "How much of its length (of each ratchet sub-hit) the gate stays open.",
    "Multiplies the hit's decays (x1 = as set; stretch a closed hat open).",
    "Per-hit filter: down = low-pass (LP), up = high-pass (HP), centre = open."};
// Filter bars: low-pass below the centre, high-pass above, in two colours.
constexpr ImU32 kLowPassColor = IM_COL32(95, 170, 255, 235);
constexpr ImU32 kHighPassColor = IM_COL32(255, 150, 70, 235);
std::string filterText(float filter) {
    char text[16];
    if(std::abs(filter) < 0.005f) return "open";
    std::snprintf(text, sizeof(text), "%s %.0f %%", filter < 0.0f ? "LP" : "HP", std::abs(filter) * 100.0f);
    return text;
}

// Follows the UI font size (the app's UI scale), 1 at ImGui's default 13 px.
float uiScale() { return std::max(0.75f, ImGui::GetFontSize() / 13.0f); }

// Duration bar: log scale over the whole x0.1 .. x24 range (x1 a little
// below the middle, drawn as the bars' baseline).
constexpr float kDurationMax = DrumGrid::kMaxDuration;
const float kLogDurationMin = std::log2(0.1f), kLogDurationSpan = std::log2(kDurationMax) - std::log2(0.1f);
float durationNormalized(float d) {
    return ofClamp((std::log2(ofClamp(d, 0.1f, kDurationMax)) - kLogDurationMin) / kLogDurationSpan, 0.0f, 1.0f);
}
float durationAt(float t) { return ofClamp(std::pow(2.0f, kLogDurationMin + ofClamp(t, 0.0f, 1.0f) * kLogDurationSpan), 0.1f, kDurationMax); }
float snapDuration(float d) {
    static const float marks[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 24.0f};
    float best = marks[0];
    for(float m : marks) if(std::abs(std::log2(d / m)) < std::abs(std::log2(d / best))) best = m;
    return best;
}

float stripNormalized(const Step& step, StripMode mode) {
    switch(mode) {
        case StripMode::Velocity: return step.velocity;
        case StripMode::Pitch: return (step.pitch + 24) / 48.0f;
        case StripMode::Probability: return step.probability;
        case StripMode::Ratchet: return step.ratchet / static_cast<float>(DrumGrid::kMaxRatchet);
        case StripMode::Gate: return step.gate;
        case StripMode::Duration: return durationNormalized(step.duration);
        case StripMode::Filter: return (step.filter + 1.0f) * 0.5f;
    }
    return 0.0f;
}
float stripValue(const Step& step, StripMode mode) {
    switch(mode) {
        case StripMode::Velocity: return step.velocity;
        case StripMode::Pitch: return static_cast<float>(step.pitch);
        case StripMode::Probability: return step.probability;
        case StripMode::Ratchet: return static_cast<float>(step.ratchet);
        case StripMode::Gate: return step.gate;
        case StripMode::Duration: return step.duration;
        case StripMode::Filter: return step.filter;
    }
    return 0.0f;
}
void setStripValue(Step& step, StripMode mode, float value) {
    switch(mode) {
        case StripMode::Velocity: step.velocity = ofClamp(value, 0.0f, 1.0f); break;
        case StripMode::Pitch: step.pitch = std::max(-24, std::min(24, static_cast<int>(std::lround(value)))); break;
        case StripMode::Probability: step.probability = ofClamp(value, 0.0f, 1.0f); break;
        case StripMode::Ratchet: step.ratchet = std::max(1, std::min(DrumGrid::kMaxRatchet, static_cast<int>(std::lround(value)))); break;
        case StripMode::Gate: step.gate = ofClamp(value, 0.05f, 1.0f); break;
        case StripMode::Duration: step.duration = ofClamp(value, 0.1f, kDurationMax); break;
        case StripMode::Filter: step.filter = ofClamp(value, -1.0f, 1.0f); break;
    }
}
// Absolute painting: the value at height t (0 bottom .. 1 top). free: no
// snapping (Duration with Alt).
float stripValueAt(StripMode mode, float t, bool free = false) {
    t = ofClamp(t, 0.0f, 1.0f);
    switch(mode) {
        case StripMode::Velocity: return t;
        case StripMode::Pitch: return std::round(-24.0f + t * 48.0f);     // semitone snap
        case StripMode::Probability: return std::round(t * 20.0f) / 20.0f; // 5% steps
        case StripMode::Ratchet: return 1.0f + std::floor(std::min(0.999f, t) * DrumGrid::kMaxRatchet);
        case StripMode::Gate: return std::max(0.05f, std::round(t * 20.0f) / 20.0f);
        case StripMode::Duration: return free ? durationAt(t) : snapDuration(durationAt(t));
        case StripMode::Filter: {
            // Snaps to open near the centre, else to 1 % (Alt: free).
            const float v = (t - 0.5f) * 2.0f;
            if(free) return v;
            return std::abs(v) < 0.06f ? 0.0f : std::round(v * 100.0f) / 100.0f;
        }
    }
    return t;
}
// Alt: relative, fine drag -- units per pixel.
float stripFineRate(StripMode mode) {
    switch(mode) {
        case StripMode::Pitch: return 1.0f / 8.0f;
        case StripMode::Ratchet: return 1.0f / 12.0f;
        default: return 0.003f;
    }
}
float stripDefault(StripMode mode, const Row& row) {
    switch(mode) {
        case StripMode::Velocity: return 0.8f;
        case StripMode::Probability: return 1.0f;
        case StripMode::Ratchet: return 1.0f;
        case StripMode::Gate: return row.gate;
        case StripMode::Duration: return 1.0f;
        case StripMode::Filter: return 0.0f;
        default: return 0.0f;
    }
}
bool fromBaseline(StripMode mode) { return mode == StripMode::Pitch || mode == StripMode::Duration || mode == StripMode::Filter; }
// Duration and Filter: Alt paints unsnapped (the others: Alt drags finely).
bool altFreePaint(StripMode mode) { return mode == StripMode::Duration || mode == StripMode::Filter; }

std::string stripText(const Step& step, StripMode mode) {
    char text[16];
    switch(mode) {
        case StripMode::Velocity: std::snprintf(text, sizeof(text), "%.0f", step.velocity * 100.0f); break;
        case StripMode::Pitch: std::snprintf(text, sizeof(text), "%+d", step.pitch); break;
        case StripMode::Probability: std::snprintf(text, sizeof(text), "%.0f%%", step.probability * 100.0f); break;
        case StripMode::Ratchet: std::snprintf(text, sizeof(text), "x%d", step.ratchet); break;
        case StripMode::Gate: std::snprintf(text, sizeof(text), "%.0f%%", step.gate * 100.0f); break;
        case StripMode::Duration: std::snprintf(text, sizeof(text), "x%.2f", step.duration); break;
        case StripMode::Filter: return filterText(step.filter);
    }
    return text;
}

// "Grid: Master (32nd)" / "Grid: 16th (fixed)": what new hits snap to.
std::string gridLabel(const ofxOceanodeTimelineLane& lane, double master) {
    if(lane.drumStepFollow)
        return std::string("Grid: Master (") + kDivisionOptions[divisionIndexForBeats(master > 0.0 ? master : 0.25)].label + ")";
    return std::string("Grid: ") + kDivisionOptions[divisionIndexForBeats(lane.beatsPerStep)].label + " (fixed)";
}
// Master or a fixed division; returns true when changed.
bool gridChoices(ofxOceanodeTimelineLane& lane, double master) {
    bool changed = false;
    const std::string follow = std::string("Master (") +
        kDivisionOptions[divisionIndexForBeats(master > 0.0 ? master : 0.25)].label + ")";
    if(ImGui::Selectable(follow.c_str(), lane.drumStepFollow)) { lane.drumStepFollow = true; changed = true; }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Follow the timeline's grid (toolbar)");
    for(int i = 1; i < kDivisionOptionCount; ++i) {
        const bool selected = !lane.drumStepFollow && std::abs(kDivisionOptions[i].beats - lane.beatsPerStep) < 1e-9;
        if(ImGui::Selectable(kDivisionOptions[i].label, selected)) {
            lane.drumStepFollow = false;
            lane.beatsPerStep = kDivisionOptions[i].beats;
            changed = true;
        }
    }
    return changed;
}

bool onGrid(double beat, double grid) { return std::abs(beat / grid - std::round(beat / grid)) * grid < 1e-6; }

// A row's length for its header: steps of the grid when it divides evenly.
std::string lengthLabel(const Row& row, double grid) {
    const double steps = row.length / grid;
    if(std::abs(steps - std::round(steps)) < 1e-6) return ofToString(static_cast<int>(std::lround(steps)));
    char text[24];
    std::snprintf(text, sizeof(text), "%gb", std::round(row.length * 1000.0) / 1000.0);
    return text;
}

Step newStep(const Row& row, double beat, double grid) {
    Step step;
    step.beat = beat;
    step.length = grid;
    step.gate = row.gate;
    return step;
}

// Hidden hits (past the row's length) are never touched by pattern edits.
void clearVisible(Row& row) {
    row.steps.erase(std::remove_if(row.steps.begin(), row.steps.end(),
                                   [&](const Step& s) { return !DrumGrid::stepHidden(row, s); }), row.steps.end());
}

// Every hit on the grid's cells i * every (from 0), nothing else (hidden hits stay).
void fillEvery(Row& row, double grid, int every) {
    clearVisible(row);
    for(double beat = 0.0; beat < row.length - 1e-9; beat += every * grid) DrumGrid::addStep(row, newStep(row, beat, grid));
}

// Moves every audible hit by `by` beats, wrapping inside the row's pattern.
void shiftRow(Row& row, double by) {
    for(auto& step : row.steps) {
        if(DrumGrid::stepHidden(row, step)) continue;
        double beat = std::fmod(step.beat + by, row.length);
        if(beat < 0.0) beat += row.length;
        step.beat = beat;
    }
    DrumGrid::normalizeRow(row);
}

// Drum Grid and Piano Roll lanes route through the same role bindings.
void drawRoleCombo(ofxOceanodeTimelineLaneEditorContext& context, const char* label, std::string& roleId,
                   ofxOceanodeTimelineTargetRole role, char* search, size_t searchSize) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto& lane = context.lane;
    const auto* currentBinding = roleId.empty() ? nullptr : timeline.getBinding(track.id, roleId);
    const std::string preview = currentBinding == nullptr ? "None" : compactParameterName(currentBinding->parameterPath);
    propertyLabel(label);
    if(ImGui::BeginCombo((std::string("##drum") + label).c_str(), preview.c_str())) {
        if(ImGui::Selectable("None", currentBinding == nullptr)) {
            const std::string previous = roleId;
            roleId.clear();
            const bool stillUsed = lane.pianoPitchBindingId == previous || lane.pianoGateBindingId == previous ||
                                   lane.pianoVelocityBindingId == previous || lane.drumDurationBindingId == previous ||
                                   lane.drumFilterBindingId == previous;
            if(!previous.empty() && !stillUsed) timeline.removeBindingFromLane(track.id, context.clip.id, lane.id, previous);
        }
        ImGui::InputText("Search parameters", search, searchSize);
        std::vector<std::string> paths = context.targetPaths;
        if(paths.empty()) for(const auto& binding : track.bindings) paths.push_back(binding.parameterPath);
        for(const auto& path : paths) {
            if(search[0] && ofToLower(path).find(ofToLower(std::string(search))) == std::string::npos) continue;
            const bool selected = currentBinding && currentBinding->parameterPath == path;
            if(ImGui::Selectable(path.c_str(), selected)) {
                context.requestedTargetPath = path;
                context.requestedTargetRole = role;
            }
        }
        ImGui::EndCombo();
    }
}

// A division combo; returns true when the user picked one.
bool divisionCombo(const char* id, const char* preview, double& beats) {
    bool changed = false;
    if(ImGui::BeginCombo(id, preview)) {
        for(int i = 1; i < kDivisionOptionCount; ++i) {
            if(ImGui::Selectable(kDivisionOptions[i].label, std::abs(kDivisionOptions[i].beats - beats) < 1e-9)) {
                beats = kDivisionOptions[i].beats;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
}

// Name, M / S and the length field (- [n] +) fit at normal font sizes.
float ofxOceanodeTimelineDrumGridEditor::headerWidth() {
    return std::round(118.0f * uiScale());
}

float ofxOceanodeTimelineDrumGridEditor::lengthFieldWidth() {
    return std::round(30.0f * uiScale());
}

int ofxOceanodeTimelineDrumGridEditor::focusedRow(const ofxOceanodeTimelineLane& lane) const {
    const auto it = focusedRows.find(lane.id);
    const int row = it == focusedRows.end() ? 0 : it->second;
    return lane.drumRows.empty() ? -1 : std::max(0, std::min(row, static_cast<int>(lane.drumRows.size()) - 1));
}

void ofxOceanodeTimelineDrumGridEditor::drawTargets(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    const auto& track = context.track;
    auto& lane = context.lane;
    const ofxOceanodeTimelineNoteGroup* group = nullptr;
    for(const auto& id : lane.bindingIds)
        if(group == nullptr) group = ofxOceanodeTimelineManager::findNoteGroupForBinding(track, id);
    if(group != nullptr) {
        // Grouped lanes follow their instrument's routing.
        if(ImGui::Button(("Routing: " + group->name + "...").c_str())) context.noteGroupToEdit = group->id;
        const auto* gate = timeline.getBinding(track.id, group->gateBindingId);
        const auto* pitch = timeline.getBinding(track.id, group->pitchBindingId);
        ImGui::TextWrapped("Gate -> %s", gate == nullptr ? "No target" : gate->parameterPath.c_str());
        ImGui::TextWrapped("Pitch -> %s", pitch == nullptr ? "No target" : pitch->parameterPath.c_str());
        const auto* duration = timeline.getBinding(track.id, group->durationBindingId);
        ImGui::TextWrapped("Duration -> %s", duration == nullptr ? "No target" : duration->parameterPath.c_str());
        const auto* filter = timeline.getBinding(track.id, group->filterBindingId);
        ImGui::TextWrapped("Filter -> %s", filter == nullptr ? "No target" : filter->parameterPath.c_str());
        ImGui::TextDisabled("Row n plays component (slot) n");
    } else {
        drawRoleCombo(context, "Gate", lane.pianoGateBindingId, ofxOceanodeTimelineTargetRole::Gate,
                      targetSearch, sizeof(targetSearch));
        drawRoleCombo(context, "Pitch", lane.pianoPitchBindingId, ofxOceanodeTimelineTargetRole::Pitch,
                      targetSearch, sizeof(targetSearch));
        drawRoleCombo(context, "Duration", lane.drumDurationBindingId, ofxOceanodeTimelineTargetRole::Duration,
                      targetSearch, sizeof(targetSearch));
        drawRoleCombo(context, "Filter", lane.drumFilterBindingId, ofxOceanodeTimelineTargetRole::Filter,
                      targetSearch, sizeof(targetSearch));
        ImGui::TextDisabled("Gate: velocity while a hit is open.\nPitch: 60 + the hit's offset.\nDuration: the hit's decay multiplier.");
    }
}

void ofxOceanodeTimelineDrumGridEditor::drawProperties(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& timeline = context.timeline;
    auto& lane = context.lane;
    // The grid: where new hits snap and which columns are drawn. Hits already
    // placed keep their positions whatever it becomes.
    {
        const double master = timeline.getMasterGridBeats();
        const char* masterLabel = kDivisionOptions[divisionIndexForBeats(master > 0.0 ? master : 0.25)].label;
        const std::string follow = std::string("Master (") + masterLabel + ")";
        const std::string preview = lane.drumStepFollow ? follow
            : kDivisionOptions[divisionIndexForBeats(lane.beatsPerStep)].label;
        propertyLabel("Grid");
        if(ImGui::BeginCombo("##drumGrid", preview.c_str())) {
            gridChoices(lane, master);
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Where new hits snap. Hits already placed keep their exact positions\n"
                              "when the grid changes; hits off the grid show an outline.");
    }
    float swing = lane.drumSwing * 100.0f;
    propertyLabel("Swing");
    if(ImGui::SliderFloat("##drumSwing", &swing, 0.0f, 75.0f, "%.0f%%")) lane.drumSwing = ofClamp(swing / 100.0f, 0.0f, 0.75f);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Delays hits on every second swing step by this much of a swing step");
    propertyLabel("Swing on");
    divisionCombo("##drumSwingGrid", kDivisionOptions[divisionIndexForBeats(lane.drumSwingBeats)].label, lane.drumSwingBeats);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The step swing applies to: hits on its odd positions are delayed.\nHits off this grid are not swung.");
    ImGui::Checkbox("Probability##drumProbability", &lane.probabilityEnabled);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: every hit plays, whatever its probability");
    propertyLabel("Seed", smallButtonReserve("New##drumSeed"));
    ImGui::DragInt("##drumSeed", &lane.probabilitySeed, 1.0f, 0, 99999);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Which hits a probability below 100%% lets through.\nRolled again every pattern repeat and every start.");
    ImGui::SameLine();
    if(ImGui::SmallButton("New##drumSeed")) lane.probabilitySeed = static_cast<int>(ofRandom(1.0f, 100000.0f));

    ImGui::Separator();
    ImGui::Text("Rows: %d", static_cast<int>(lane.drumRows.size()));
    ImGui::SameLine();
    if(ImGui::SmallButton("+##drumAddRow") && lane.drumRows.size() < static_cast<size_t>(DrumGrid::kMaxRows)) {
        int slot = 0;
        for(const auto& row : lane.drumRows) slot = std::max(slot, row.slot + 1);
        auto rows = DrumGrid::defaultRows(slot + 1, {}, lane.drumRows.empty() ? 4.0 : lane.drumRows.back().length);
        lane.drumRows.push_back(rows.back());
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a row on the next slot");
    ImGui::SameLine();
    if(ImGui::SmallButton("-##drumRemoveRow") && lane.drumRows.size() > 1) lane.drumRows.pop_back();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the last row");
    int voices = 0;
    std::vector<std::string> names;
    const bool instrument = timeline.drumGridInstrument(context.track, lane, voices, names);
    ImGui::BeginDisabled(!instrument);
    if(ImGui::SmallButton("Rows from instrument")) {
        // One row per slot, named by the kit; hits of a slot already here are kept.
        auto rows = DrumGrid::defaultRows(std::max(1, voices), names, lane.drumRows.empty() ? 4.0 : lane.drumRows.front().length);
        for(auto& row : rows) {
            for(const auto& old : lane.drumRows) {
                if(old.slot != row.slot) continue;
                const auto name = row.name;
                row = old;
                row.name = name;
                break;
            }
        }
        lane.drumRows = std::move(rows);
    }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(instrument ? "One row per kit slot, named by the instrument" : "The Gate target is not an instrument");
    ImGui::TextDisabled("Row settings and the strip's parameter: Row tab");
}

void ofxOceanodeTimelineDrumGridEditor::drawRowSettings(ofxOceanodeTimelineLane& lane, int rowIndex, double grid) {
    auto& row = lane.drumRows[static_cast<size_t>(rowIndex)];
    char name[128];
    std::snprintf(name, sizeof(name), "%s", row.name.c_str());
    propertyLabel("Name");
    if(ImGui::InputText("##drumRowName", name, sizeof(name))) row.name = name;
    propertyLabel("Slot");
    if(ImGui::DragInt("##drumRowSlot", &row.slot, 0.1f, 0, 127)) row.slot = std::max(0, row.slot);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Gate/Pitch/Duration component this row plays (0 = first kit piece)");
    // Length: in steps of the current grid, and in beats.
    int steps = static_cast<int>(std::lround(row.length / grid));
    propertyLabel("Steps");
    if(ImGui::DragInt("##drumRowSteps", &steps, 0.2f, 1, static_cast<int>(DrumGrid::kMaxLength / grid))) {
        row.length = std::max(1, steps) * grid;
        DrumGrid::normalizeRow(row);
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Pattern length in steps of the current grid (rows of different lengths: polymeter)");
    float beats = static_cast<float>(row.length);
    propertyLabel("Beats");
    if(ImGui::DragFloat("##drumRowBeats", &beats, 0.01f, static_cast<float>(DrumGrid::kMinLength),
                        static_cast<float>(DrumGrid::kMaxLength), "%.3g")) {
        row.length = beats;
        DrumGrid::normalizeRow(row);
    }
    float gate = row.gate * 100.0f;
    propertyLabel("Gate");
    if(ImGui::SliderFloat("##drumRowGate", &gate, 5.0f, 100.0f, "%.0f%%")) row.gate = ofClamp(gate / 100.0f, 0.05f, 1.0f);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Gate of new hits placed in this row (each hit keeps its own; the Gate strip edits them)");
    if(ImGui::SmallButton("All rows this length")) {
        for(auto& other : lane.drumRows) { other.length = row.length; DrumGrid::normalizeRow(other); }
    }
}

void ofxOceanodeTimelineDrumGridEditor::drawExtraTab(ofxOceanodeTimelineLaneEditorContext& context) {
    auto& lane = context.lane;
    const int focused = focusedRow(lane);
    if(focused < 0) { ImGui::TextDisabled("No rows"); return; }
    // Which row the strip and these settings edit.
    {
        auto label = [&](int r) {
            const auto& row = lane.drumRows[static_cast<size_t>(r)];
            return (row.name.empty() ? "Row " + ofToString(r + 1) : row.name) + "##drumRowPick" + ofToString(r);
        };
        propertyLabel("Row");
        if(ImGui::BeginCombo("##drumFocusedRow", label(focused).c_str())) {
            for(int r = 0; r < static_cast<int>(lane.drumRows.size()); ++r)
                if(ImGui::Selectable(label(r).c_str(), r == focused)) focusedRows[lane.id] = r;
            ImGui::EndCombo();
        }
    }
    // The strip's parameter, one full-width choice per line.
    ImGui::SeparatorText("Strip");
    StripMode mode = stripModes.count(lane.id) ? stripModes[lane.id] : StripMode::Velocity;
    for(int t = 0; t < kModes; ++t) {
        if(ImGui::RadioButton((std::string(kStripNames[t]) + "##drumStripMode").c_str(), static_cast<int>(mode) == t))
            stripModes[lane.id] = static_cast<StripMode>(t);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kStripHints[t]);
    }
    ImGui::SeparatorText("Row");
    drawRowSettings(lane, focusedRow(lane), context.timeline.drumStepBeats(lane));
}

void ofxOceanodeTimelineDrumGridEditor::drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                                                   const ofxOceanodeTimelineLaneCanvas& canvas) {
    auto& lane = context.lane;
    auto& clip = context.clip;
    const auto& track = context.track;
    const auto& view = context.view;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = uiScale();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float textHeight = ImGui::GetTextLineHeight();

    for(auto& row : lane.drumRows) DrumGrid::normalizeRow(row);
    const int rowCount = static_cast<int>(lane.drumRows.size());
    const int focused = focusedRow(lane);
    StripMode mode = stripModes.count(lane.id) ? stripModes[lane.id] : StripMode::Velocity;
    const double grid = context.timeline.drumStepBeats(lane);

    // ---- geometry ---------------------------------------------------------
    const float zoneLeft = canvas.zoneLeft;
    const float headerLeft = zoneLeft - headerWidth() + 2.0f;
    const float headerRight = zoneLeft - 3.0f;
    const float top = canvas.min.y + 4.0f;
    const float bottom = canvas.max.y - kLaneResizeHandleHeight - 2.0f;
    const float total = std::max(1.0f, bottom - top);
    // The strip: a row of parameter tabs (full words), then the bars.
    const float tabHeight = textHeight + 6.0f;
    const float stripHeight = total >= 110.0f * scale + tabHeight
        ? ofClamp(total * 0.3f, 36.0f * scale + tabHeight, 100.0f * scale + tabHeight) : 0.0f;
    const float stripBottom = bottom;
    const float stripTop = stripBottom - stripHeight;
    const float barsTop = stripTop + tabHeight + 2.0f;
    // Tabs pinned at the zone's left edge: full words when they fit.
    struct TabRect { float x1, x2; };
    std::vector<TabRect> tabRects;
    bool shortTabs = false;
    {
        const float pad = 8.0f * scale, gapX = 3.0f;
        auto layout = [&](const char* const* labels) {
            tabRects.clear();
            float x = zoneLeft + 2.0f;
            for(int t = 0; t < kModes; ++t) {
                const float w = ImGui::CalcTextSize(labels[t]).x + 2.0f * pad;
                tabRects.push_back({x, x + w});
                x += w + gapX;
            }
            return x;
        };
        if(layout(kStripNames) > canvas.max.x - 4.0f) { shortTabs = true; layout(kStripLabels); }
    }
    // The grid chip after the tabs: the lane's grid, click to change it.
    const std::string gridText = gridLabel(lane, context.timeline.getMasterGridBeats());
    const float chipX1 = tabRects.back().x2 + 12.0f * scale;
    const float chipX2 = chipX1 + ImGui::CalcTextSize(gridText.c_str()).x + 16.0f * scale;
    const bool chipVisible = chipX2 <= canvas.max.x - 2.0f;
    const float gridBottom = stripHeight > 0.0f ? stripTop - 6.0f * scale : bottom;
    const float rowHeight = rowCount > 0 ? std::max(1.0f, (gridBottom - top) / rowCount) : 0.0f;
    const double content = sourceDuration(clip);
    const int clipCycles = clip.repeatContent
        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip) - 1e-9))) : 1;
    const float left = canvas.left, right = canvas.right;
    const double beatsPerBar = std::max(1e-6, context.timeline.getBeatsPerBar());
    auto xOf = [&](double sourceBeat, int cycle) {
        return canvas.timelineOrigin.x + view.beatToPixels(sourceToTimelineBeat(clip, sourceBeat, cycle));
    };
    // Visible clip repeats only.
    int firstCycle = 0, lastCycle = clipCycles - 1;
    if(clip.repeatContent) {
        const double leftBeat = view.pixelsToBeat(left - canvas.timelineOrigin.x);
        const double rightBeat = view.pixelsToBeat(right - canvas.timelineOrigin.x);
        firstCycle = std::max(0, static_cast<int>(std::floor((leftBeat - clip.startBeat) / cycleDuration(clip))) - 1);
        lastCycle = std::min(clipCycles - 1, static_cast<int>(std::floor((rightBeat - clip.startBeat) / cycleDuration(clip))) + 1);
    }
    // Playhead, in source beats of a clip repeat.
    int playCycle = -1;
    double playBeat = -1.0;
    if(canvas.beatPosition >= clip.startBeat && canvas.beatPosition < clip.startBeat + clip.durationBeats) {
        playCycle = static_cast<int>(cycleIndex(clip, canvas.beatPosition));
        playBeat = timelineToSourceBeat(clip, canvas.beatPosition);
    }
    // Where a hit is drawn (pattern p of its row, clip repeat c), and its width.
    auto hitStartBeat = [&](const Row& row, const Step& step, int64_t pattern) {
        return pattern * row.length + step.beat + DrumGrid::swingOffset(lane, step);
    };

    // ---- drawing ------------------------------------------------------------
    dl->PushClipRect(ImVec2(headerLeft, canvas.min.y), ImVec2(canvas.max.x, canvas.max.y), true);
    auto trackAlpha = [&](int alpha) { return IM_COL32(track.color.r, track.color.g, track.color.b, alpha); };
    // Each row in its pad's colour (the instrument's rowColors), else the track's.
    std::vector<ofColor> instrumentColors;
    {
        int voices = 0;
        std::vector<std::string> names;
        context.timeline.drumGridInstrument(track, lane, voices, names, nullptr, &instrumentColors);
    }
    std::vector<ofColor> rowColors;
    for(const auto& row : lane.drumRows) rowColors.push_back(DrumGrid::rowColor(instrumentColors, row, track.color));
    auto rowAlpha = [&](int r, int alpha) {
        const auto& c = rowColors[static_cast<size_t>(r)];
        return IM_COL32(c.r, c.g, c.b, alpha);
    };
    dl->AddRectFilled(ImVec2(left, top - 1.0f), ImVec2(right, bottom), trackAlpha(28), 2.0f);

    // Row headers: colour swatch, name, M / S, and the length as a field with
    // - / + (in steps of the grid, or beats). Taller rows put the length on a
    // second line; short ones put it first.
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * scale, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
        const float buttonWidth = ImGui::CalcTextSize("-").x + 6.0f * scale;
        const float fieldWidth = lengthFieldWidth();
        for(int r = 0; r < rowCount; ++r) {
            auto& row = lane.drumRows[static_cast<size_t>(r)];
            const float y1 = top + r * rowHeight, y2 = y1 + rowHeight;
            const bool isFocused = r == focused;
            const bool audible = DrumGrid::rowAudible(lane, row);
            dl->AddRectFilled(ImVec2(headerLeft, y1 + 0.5f), ImVec2(headerRight, y2 - 0.5f),
                              isFocused ? IM_COL32(70, 70, 80, 255) : IM_COL32(44, 44, 50, 255), 2.0f);
            dl->AddRectFilled(ImVec2(headerLeft, y1 + 0.5f), ImVec2(headerLeft + (isFocused ? 5.0f : 3.0f) * scale, y2 - 0.5f),
                              rowAlpha(r, 255));
            const float box = std::min(rowHeight - 2.0f, 14.0f * scale);
            if(box < 6.0f) continue;
            const bool twoLines = rowHeight >= 2.0f * textHeight + 6.0f;
            const float lineY = twoLines ? y1 + 2.0f : y1 + (rowHeight - textHeight) * 0.5f;
            const float by = twoLines ? lineY + (textHeight - box) * 0.5f : y1 + (rowHeight - box) * 0.5f;
            const float sx = headerRight - box, mx = sx - box - 2.0f;
            dl->AddRectFilled(ImVec2(mx, by), ImVec2(mx + box, by + box), row.mute ? IM_COL32(205, 120, 40, 255) : IM_COL32(64, 64, 70, 255), 2.0f);
            dl->AddRectFilled(ImVec2(sx, by), ImVec2(sx + box, by + box), row.solo ? IM_COL32(220, 200, 60, 255) : IM_COL32(64, 64, 70, 255), 2.0f);
            if(box >= textHeight - 2.0f) {
                dl->AddText(ImVec2(mx + (box - ImGui::CalcTextSize("M").x) * 0.5f, by + (box - textHeight) * 0.5f), IM_COL32(235, 235, 235, 255), "M");
                dl->AddText(ImVec2(sx + (box - ImGui::CalcTextSize("S").x) * 0.5f, by + (box - textHeight) * 0.5f), IM_COL32(235, 235, 235, 255), "S");
            }
            if(rowHeight < textHeight) continue;
            const float controlsX = headerLeft + 7.0f * scale;
            const float controlsY = twoLines ? lineY + textHeight + 2.0f : lineY;
            const float nameX = twoLines ? controlsX : controlsX + 2.0f * buttonWidth + fieldWidth + 6.0f;
            const std::string name = fitText(row.name.empty() ? "Row " + ofToString(r + 1) : row.name, mx - 4.0f - nameX);
            const ofColor& c = rowColors[static_cast<size_t>(r)];
            const ImU32 nameColor = audible ? IM_COL32((c.r + 2 * 235) / 3, (c.g + 2 * 235) / 3, (c.b + 2 * 235) / 3, 255)
                                            : IM_COL32(130, 130, 130, 255);
            dl->AddText(ImVec2(nameX, lineY), nameColor, name.c_str());
            // - [length] +
            ImGui::PushID(("drumLength" + lane.id).c_str());
            ImGui::PushID(r);
            ImGui::SetCursorScreenPos(ImVec2(controlsX, controlsY));
            if(ImGui::SmallButton("-")) {
                const double steps = row.length / grid;
                row.length = std::max(grid, (std::ceil(steps - 1e-6) - 1.0) * grid);
                DrumGrid::normalizeRow(row);
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("One %s step shorter (hits past the end are kept, silent)",
                                                         kDivisionOptions[divisionIndexForBeats(grid)].label);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fieldWidth);
            const double steps = row.length / grid;
            if(std::abs(steps - std::round(steps)) < 1e-6) {
                int count = static_cast<int>(std::lround(steps));
                if(ImGui::DragInt("##length", &count, 0.1f, 1, static_cast<int>(DrumGrid::kMaxLength / grid), "%d")) {
                    row.length = std::max(1, count) * grid;
                    DrumGrid::normalizeRow(row);
                }
            } else {
                float beats = static_cast<float>(row.length);
                if(ImGui::DragFloat("##length", &beats, 0.01f, static_cast<float>(DrumGrid::kMinLength),
                                    static_cast<float>(DrumGrid::kMaxLength), "%.3gb")) {
                    row.length = beats;
                    DrumGrid::normalizeRow(row);
                }
            }
            if(ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("Length: %s steps of %s = %g beats. Drag, or double-click to type.\nHits past the end are kept (silent).",
                                  lengthLabel(row, grid).c_str(), kDivisionOptions[divisionIndexForBeats(grid)].label, row.length);
            ImGui::SameLine();
            if(ImGui::SmallButton("+")) {
                const double count = row.length / grid;
                row.length = std::min(DrumGrid::kMaxLength, (std::floor(count + 1e-6) + 1.0) * grid);
                DrumGrid::normalizeRow(row);
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("One %s step longer", kDivisionOptions[divisionIndexForBeats(grid)].label);
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::PopStyleVar(2);
    }

    // Rows: grid cells of each pattern, then the hits at their positions. The
    // first pattern is solid; its repeats are ghosted, with a bright end line
    // at the row's length (drag it) and fainter lines at further repeats.
    constexpr int kMaxCellsDrawn = 20000;
    int cellsDrawn = 0;
    for(int r = 0; r < rowCount; ++r) {
        const auto& row = lane.drumRows[static_cast<size_t>(r)];
        const float y1 = top + r * rowHeight + 1.0f, y2 = top + (r + 1) * rowHeight - 1.0f;
        if(y2 <= y1) continue;
        const bool audible = DrumGrid::rowAudible(lane, row);
        const int64_t patterns = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(content / row.length - 1e-9)));
        const int cellsPerPattern = std::max(1, static_cast<int>(std::ceil(row.length / grid - 1e-9)));
        for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            for(int64_t p = 0; p < patterns; ++p) {
                const double patternStart = p * row.length;
                if(patternStart >= content) break;
                const float px1 = xOf(patternStart, cycle), px2 = xOf(std::min(content, patternStart + row.length), cycle);
                if(px2 < left || px1 > right) continue;
                const bool firstPass = p == 0;
                for(int k = 0; k < cellsPerPattern && cellsDrawn < kMaxCellsDrawn; ++k) {
                    const double s1 = patternStart + k * grid;
                    const double s2 = std::min({content, patternStart + row.length, s1 + grid});
                    if(s1 >= content || s2 <= s1) break;
                    const float x1 = xOf(s1, cycle), x2 = xOf(s2, cycle);
                    if(x2 < left || x1 > right || x2 - x1 < 1.5f) continue;
                    ++cellsDrawn;
                    const float cx1 = std::max(x1, left) + 1.0f, cx2 = std::min(x2, right) - 1.0f;
                    dl->AddRectFilled(ImVec2(cx1, y1), ImVec2(cx2, y2), firstPass ? IM_COL32(30, 30, 34, 200) : IM_COL32(18, 18, 20, 210), 1.5f);
                    // Column line: bars strongest, then beats, then steps.
                    const double timelineBeat = sourceToTimelineBeat(clip, s1, cycle);
                    const bool bar = std::abs(timelineBeat / beatsPerBar - std::round(timelineBeat / beatsPerBar)) < 1e-6;
                    const bool beat = std::abs(timelineBeat - std::round(timelineBeat)) < 1e-6;
                    if(x1 >= left && (bar || beat))
                        dl->AddLine(ImVec2(x1, y1), ImVec2(x1, y2), bar ? kBar : IM_COL32(92, 92, 92, 145), bar ? 1.5f : 1.0f);
                    if(cycle == playCycle && playBeat >= s1 && playBeat < s2)
                        dl->AddRectFilled(ImVec2(cx1, y1), ImVec2(cx2, y2), IM_COL32(255, 255, 255, 26));
                }
                // Hits at their true positions; off the current grid: outlined.
                for(const auto& step : row.steps) {
                    if(DrumGrid::stepHidden(row, step)) continue;
                    const double s1 = hitStartBeat(row, step, p);
                    if(s1 >= content || s1 + step.length <= 0.0) continue;
                    const double s2 = std::min(content, s1 + step.length);
                    float x1 = xOf(std::max(0.0, s1), cycle), x2 = xOf(s2, cycle);
                    if(x2 < left || x1 > right) continue;
                    x2 = std::max(x2 - 1.0f, x1 + 3.0f);
                    const float probability = ofClamp(step.probability, 0.0f, 1.0f);
                    int alpha = firstPass ? 240 : 85;
                    if(!audible) alpha /= 3;
                    alpha = static_cast<int>(alpha * (0.35f + 0.65f * probability));
                    const float fillTop = y2 - std::max(0.2f, step.velocity) * (y2 - y1);
                    dl->AddRectFilled(ImVec2(x1 + 1.0f, fillTop), ImVec2(x2, y2), rowAlpha(r, alpha), 1.5f);
                    const bool sounding = cycle == playCycle && playBeat >= s1 && playBeat < s2;
                    if(sounding && audible) dl->AddRect(ImVec2(x1 + 1.0f, y1), ImVec2(x2, y2), IM_COL32(255, 255, 255, 200), 1.5f, 0, 1.5f);
                    if(!onGrid(step.beat, grid))
                        dl->AddRect(ImVec2(x1 + 1.0f, y1), ImVec2(x2, y2), IM_COL32(255, 200, 90, firstPass ? 230 : 110), 1.5f, 0, 1.0f);
                    if(probability < 1.0f)
                        for(float x = x1 + 2.0f; x < x2 - 1.0f; x += 3.0f)
                            dl->AddRectFilled(ImVec2(x, y1 + 1.0f), ImVec2(x + 1.0f, y1 + 2.0f), IM_COL32(245, 245, 245, firstPass ? 200 : 90));
                    if(step.ratchet > 1 && x2 - x1 >= step.ratchet * 2.0f)
                        for(int k = 1; k < step.ratchet; ++k) {
                            const float x = x1 + (x2 - x1) * k / step.ratchet;
                            dl->AddLine(ImVec2(x, y1 + 1.0f), ImVec2(x, y1 + std::min(6.0f * scale, (y2 - y1) * 0.4f)), IM_COL32(245, 245, 245, firstPass ? 220 : 90), 1.0f);
                        }
                    if(firstPass && step.pitch != 0 && x2 - x1 >= 16.0f * scale && y2 - y1 >= textHeight) {
                        char text[8];
                        std::snprintf(text, sizeof(text), "%+d", step.pitch);
                        dl->AddText(ImVec2(x1 + 2.0f, y1 + (y2 - y1 - textHeight) * 0.5f), IM_COL32(20, 20, 20, 240), text);
                    }
                }
                // Further repeats: a fainter line where each starts.
                if(p >= 2) dl->AddLine(ImVec2(px1, y1), ImVec2(px1, y2), rowAlpha(r, 120), 1.5f * scale);
            }
            // Hidden hits (at or past the length): faint outlines where they
            // would play if the row were long enough.
            for(const auto& step : row.steps) {
                if(!DrumGrid::stepHidden(row, step) || step.beat >= content) continue;
                const float x1 = xOf(step.beat, cycle);
                const float x2 = std::max(x1 + 3.0f, xOf(std::min(content, step.beat + step.length), cycle) - 1.0f);
                if(x2 < left || x1 > right) continue;
                dl->AddRect(ImVec2(x1 + 1.0f, y1 + 2.0f), ImVec2(x2, y2 - 2.0f), rowAlpha(r, 110), 1.5f, 0, 1.0f);
            }
            // The end of the row's cycle: a thick line in its colour with a handle.
            if(row.length <= content + 1e-9) {
                const float x = xOf(row.length, cycle);
                if(x >= left - 4.0f && x <= right + 4.0f) {
                    const bool active = drag == Drag::Length && dragLaneId == lane.id && dragRow == r;
                    dl->AddLine(ImVec2(x, y1), ImVec2(x, y2), rowAlpha(r, 255), 3.0f * scale);
                    const float handleHeight = std::min(y2 - y1, 14.0f * scale);
                    const float hy = (y1 + y2 - handleHeight) * 0.5f;
                    dl->AddRectFilled(ImVec2(x - 3.5f * scale, hy), ImVec2(x + 3.5f * scale, hy + handleHeight),
                                      active ? IM_COL32(255, 255, 255, 255) : rowAlpha(r, 255), 2.0f);
                    dl->AddRect(ImVec2(x - 3.5f * scale, hy), ImVec2(x + 3.5f * scale, hy + handleHeight), IM_COL32(15, 15, 18, 220), 2.0f);
                }
            }
        }
    }

    // Parameter strip for the focused row: one bar per hit.
    if(stripHeight > 0.0f && focused >= 0) {
        const auto& row = lane.drumRows[static_cast<size_t>(focused)];
        // Header column: whose parameter, which one, and the hint.
        dl->AddRectFilled(ImVec2(headerLeft, stripTop), ImVec2(headerRight, stripBottom), IM_COL32(44, 44, 50, 255), 2.0f);
        {
            float y = stripTop + 3.0f;
            const float width = headerRight - headerLeft - 8.0f;
            bool first = true;
            for(const std::string& line : {fitText(row.name.empty() ? "Row " + ofToString(focused + 1) : row.name, width),
                                           fitText(kStripNames[static_cast<int>(mode)], width),
                                           fitText(altFreePaint(mode) ? "Alt: free" : "Alt: fine", width)}) {
                if(y + textHeight > stripBottom) break;
                dl->AddText(ImVec2(headerLeft + 4.0f, y), first ? IM_COL32(230, 230, 230, 255) : IM_COL32(160, 160, 170, 255), line.c_str());
                y += textHeight + 1.0f;
                first = false;
            }
        }
        for(int t = 0; t < kModes; ++t) {
            const auto& rect = tabRects[static_cast<size_t>(t)];
            const bool on = static_cast<int>(mode) == t;
            const bool hovered = hoverRect(ImVec2(rect.x1, stripTop), ImVec2(rect.x2, stripTop + tabHeight));
            dl->AddRectFilled(ImVec2(rect.x1, stripTop), ImVec2(rect.x2, stripTop + tabHeight),
                              on ? rowAlpha(focused, 230) : hovered ? IM_COL32(88, 88, 96, 255) : IM_COL32(58, 58, 64, 255), 3.0f);
            const char* label = shortTabs ? kStripLabels[t] : kStripNames[t];
            const float w = ImGui::CalcTextSize(label).x;
            dl->AddText(ImVec2((rect.x1 + rect.x2 - w) * 0.5f, stripTop + 3.0f),
                        on ? IM_COL32(20, 20, 20, 255) : IM_COL32(225, 225, 225, 255), label);
        }
        if(chipVisible) {
            const bool hovered = hoverRect(ImVec2(chipX1, stripTop), ImVec2(chipX2, stripTop + tabHeight));
            dl->AddRect(ImVec2(chipX1, stripTop), ImVec2(chipX2, stripTop + tabHeight),
                        hovered ? IM_COL32(220, 220, 230, 255) : IM_COL32(130, 130, 140, 220), 3.0f);
            dl->AddText(ImVec2(chipX1 + 8.0f * scale, stripTop + 3.0f), IM_COL32(225, 225, 225, 255), gridText.c_str());
        }
        dl->AddRectFilled(ImVec2(left, barsTop), ImVec2(right, stripBottom), IM_COL32(20, 20, 23, 220), 2.0f);
        const float baseY = mode == StripMode::Duration ? stripBottom - durationNormalized(1.0f) * (stripBottom - barsTop)
                          : mode == StripMode::Pitch || mode == StripMode::Filter ? (barsTop + stripBottom) * 0.5f : stripBottom;
        if(mode == StripMode::Filter && stripBottom - barsTop >= 2.0f * textHeight) {
            dl->AddText(ImVec2(left + 3.0f, barsTop + 1.0f), kHighPassColor, "HP");
            dl->AddText(ImVec2(left + 3.0f, stripBottom - textHeight - 1.0f), kLowPassColor, "LP");
        }
        if(mode == StripMode::Duration) {
            for(float mark : {0.25f, 0.5f, 2.0f, 4.0f, 8.0f, 16.0f}) {
                const float y = stripBottom - durationNormalized(mark) * (stripBottom - barsTop);
                dl->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(70, 70, 78, 140));
            }
        }
        if(fromBaseline(mode)) dl->AddLine(ImVec2(left, baseY), ImVec2(right, baseY), IM_COL32(110, 110, 120, 160));
        const int64_t patterns = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(content / row.length - 1e-9)));
        const float gridPixels = std::abs(xOf(grid, 0) - xOf(0.0, 0));
        for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            for(int64_t p = 0; p < patterns; ++p) {
                const bool firstPass = p == 0;
                for(const auto& step : row.steps) {
                    if(DrumGrid::stepHidden(row, step)) continue;
                    const double s1 = hitStartBeat(row, step, p);
                    if(s1 >= content || s1 + step.length <= 0.0) continue;
                    const float x1 = xOf(std::max(0.0, s1), cycle);
                    const float x2 = std::max(x1 + 3.0f, std::min(xOf(std::min(content, s1 + step.length), cycle), x1 + gridPixels) - 1.5f);
                    if(x2 < left || x1 > right) continue;
                    const float n = ofClamp(stripNormalized(step, mode), 0.0f, 1.0f);
                    const float y = stripBottom - n * (stripBottom - barsTop);
                    const int alpha = firstPass ? 225 : 90;
                    ImU32 barColor = rowAlpha(focused, alpha);
                    if(mode == StripMode::Filter && std::abs(step.filter) >= 0.005f)
                        barColor = (step.filter < 0.0f ? kLowPassColor : kHighPassColor) & (firstPass ? 0xFFFFFFFFu : 0x7FFFFFFFu);
                    dl->AddRectFilled(ImVec2(x1 + 1.0f, std::min(y, baseY)), ImVec2(x2, std::max(y, baseY) + 1.0f), barColor, 1.0f);
                    if(mode == StripMode::Ratchet && step.ratchet > 1 && x2 - x1 >= 4.0f)
                        for(int k = 1; k < step.ratchet; ++k) {
                            const float ky = stripBottom - (stripBottom - barsTop) * k / DrumGrid::kMaxRatchet;
                            dl->AddLine(ImVec2(x1 + 1.0f, ky), ImVec2(x2, ky), IM_COL32(20, 20, 23, 200));
                        }
                    if(firstPass && x2 - x1 >= 26.0f * scale && stripBottom - barsTop >= textHeight * 1.5f)
                        dl->AddText(ImVec2(x1 + 2.0f, barsTop + 1.0f), IM_COL32(230, 230, 230, 230), stripText(step, mode).c_str());
                }
            }
        }
    }
    dl->PopClipRect();

    // ---- interaction --------------------------------------------------------
    const bool anyPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if(anyPopup) popupFramesClosed = 0;
    else if(popupFramesClosed < 2) ++popupFramesClosed;
    const bool inputAllowed = !anyPopup && popupFramesClosed >= 2 && rowCount > 0;
    const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool overHeader = windowHovered && hoverRect(ImVec2(headerLeft, top), ImVec2(headerRight, gridBottom));
    const bool overGrid = (canvas.hovered || drag != Drag::None) && hoverRect(ImVec2(left, top), ImVec2(right, gridBottom));
    int tabUnderMouse = -1;
    if(stripHeight > 0.0f && windowHovered)
        for(int t = 0; t < kModes; ++t)
            if(hoverRect(ImVec2(tabRects[static_cast<size_t>(t)].x1, stripTop), ImVec2(tabRects[static_cast<size_t>(t)].x2, stripTop + tabHeight)))
                tabUnderMouse = t;
    const bool overStrip = stripHeight > 0.0f && tabUnderMouse < 0 && (canvas.hovered || drag != Drag::None) &&
                           hoverRect(ImVec2(left, barsTop), ImVec2(right, stripBottom));
    auto rowAtY = [&](float y) {
        if(rowHeight <= 0.0f) return -1;
        const int r = static_cast<int>(std::floor((y - top) / rowHeight));
        return r >= 0 && r < rowCount ? r : -1;
    };
    // Source beat under an x (of the clip repeat there), false outside the content.
    auto sourceAtX = [&](float x, double& sourceBeat) {
        const double timelineBeat = view.pixelsToBeat(x - canvas.timelineOrigin.x);
        if(timelineBeat < clip.startBeat || timelineBeat >= clip.startBeat + clip.durationBeats) return false;
        sourceBeat = timelineToSourceBeat(clip, timelineBeat);
        return sourceBeat >= 0.0 && sourceBeat < content;
    };
    // Pattern-relative position in a row.
    auto patternPos = [&](const Row& row, double sourceBeat) {
        return sourceBeat - std::floor(sourceBeat / row.length) * row.length;
    };
    // The hit drawn under a source beat in a row (by its drawn span, at least
    // 4 px wide), -1 for none.
    const double minSpan = std::abs(view.pixelsToBeat(4.0f) - view.pixelsToBeat(0.0f)) / std::max(1e-6, effectiveStretch(clip));
    auto hitAt = [&](const Row& row, double sourceBeat) {
        const int64_t p = static_cast<int64_t>(std::floor(sourceBeat / row.length));
        int best = -1;
        double bestDistance = 0.0;
        for(int64_t q = p - 1; q <= p + 1; ++q) {
            for(size_t i = 0; i < row.steps.size(); ++i) {
                const auto& step = row.steps[i];
                const double s1 = std::max(0.0, hitStartBeat(row, step, q));
                const double s2 = s1 + std::max(step.length, minSpan);
                if(sourceBeat < s1 || sourceBeat >= s2) continue;
                const double distance = sourceBeat - s1;
                if(best < 0 || distance < bestDistance) { best = static_cast<int>(i); bestDistance = distance; }
            }
        }
        return best;
    };
    // The hit a press in a row's cell grabs: the one under the pointer, else
    // one placed inside that cell (so a cell holding an off-grid hit toggles
    // that hit instead of stacking a second one on the grid).
    auto grabAt = [&](const Row& row, double sourceBeat) {
        const int under = hitAt(row, sourceBeat);
        if(under >= 0) return under;
        const double pos = patternPos(row, sourceBeat);
        const double cell = std::floor(pos / grid) * grid;
        return DrumGrid::stepInRange(row, cell, cell + grid, pos);
    };
    auto focusLane = [&]() { context.focusedLaneId = lane.id; };

    if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if(drag == Drag::Pending && dragLaneId == lane.id && dragRow >= 0 && dragRow < rowCount) {
            // A click: remove the grabbed hit, or add one on the pressed cell.
            auto& row = lane.drumRows[static_cast<size_t>(dragRow)];
            if(dragStep >= 0 && dragStep < static_cast<int>(row.steps.size())) row.steps.erase(row.steps.begin() + dragStep);
            else if(dragStep < 0 && dragCellBeat < row.length - 1e-9) DrumGrid::addStep(row, newStep(row, dragCellBeat, grid));
        }
        if(dragLaneId == lane.id) drag = Drag::None;
    }

    if(inputAllowed) {
        // Row headers: M / S, focus, menu (the length widgets handle themselves).
        if(overHeader && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            const int r = rowAtY(mouse.y);
            if(r >= 0) {
                auto& row = lane.drumRows[static_cast<size_t>(r)];
                const float y1 = top + r * rowHeight;
                const float box = std::min(rowHeight - 2.0f, 14.0f * scale);
                const bool twoLines = rowHeight >= 2.0f * textHeight + 6.0f;
                const float by = twoLines ? y1 + 2.0f + (textHeight - box) * 0.5f : y1 + (rowHeight - box) * 0.5f;
                const float sx = headerRight - box, mx = sx - box - 2.0f;
                const bool overMute = box >= 6.0f && hoverRect(ImVec2(mx, by), ImVec2(mx + box, by + box));
                const bool overSolo = box >= 6.0f && hoverRect(ImVec2(sx, by), ImVec2(sx + box, by + box));
                if(overMute) ImGui::SetTooltip("Mute %s", row.name.c_str());
                else if(overSolo) ImGui::SetTooltip("Solo %s", row.name.c_str());
                else ImGui::SetTooltip("%s  (slot %d, %s steps = %g beats)\nClick: edit its parameters below. Right-click: row menu",
                                       row.name.c_str(), row.slot, lengthLabel(row, grid).c_str(), row.length);
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    focusLane();
                    if(overMute) row.mute = !row.mute;
                    else if(overSolo) row.solo = !row.solo;
                    else focusedRows[lane.id] = r;
                }
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    focusLane();
                    focusedRows[lane.id] = r;
                    popupLaneId = lane.id;
                    popupRow = r;
                    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", row.name.c_str());
                    ImGui::OpenPopup(("##drumRowMenu" + lane.id).c_str());
                }
            }
        }
        if(chipVisible && stripHeight > 0.0f && windowHovered &&
           hoverRect(ImVec2(chipX1, stripTop), ImVec2(chipX2, stripTop + tabHeight))) {
            ImGui::SetTooltip("Where new hits snap and which columns show.\nHits already placed never move. Click to change.");
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                focusLane();
                popupLaneId = lane.id;
                ImGui::OpenPopup(("##drumGridMenu" + lane.id).c_str());
            }
        }
        if(tabUnderMouse >= 0) {
            ImGui::SetTooltip("%s: %s\nDrag across the hits to paint. %s Right-click a bar: default.",
                              kStripNames[tabUnderMouse], kStripHints[tabUnderMouse],
                              tabUnderMouse == static_cast<int>(StripMode::Duration) ? "Snaps to x0.125 .. x24; Alt: free."
                              : tabUnderMouse == static_cast<int>(StripMode::Filter) ? "Snaps to open near the centre; Alt: free. Double-click a bar: open."
                                                                                       : "Alt: fine.");
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                stripModes[lane.id] = mode = static_cast<StripMode>(tabUnderMouse);
                focusLane();
            }
        }

        if(context.focused) {
            const auto& io = ImGui::GetIO();
            // A row's end-of-cycle handle: drag to set its length (grid steps;
            // Alt: free), double-click for the clip's length.
            auto handleAt = [&](int& rowOut, int& cycleOut) {
                const int r = rowAtY(mouse.y);
                if(r < 0) return false;
                const auto& row = lane.drumRows[static_cast<size_t>(r)];
                if(row.length > content + 1e-9) return false;
                for(int cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                    if(std::abs(mouse.x - xOf(row.length, cycle)) <= 5.0f * scale) { rowOut = r; cycleOut = cycle; return true; }
                }
                return false;
            };
            int handleRow = -1, handleCycle = 0;
            const bool overHandle = (canvas.hovered || windowHovered) && drag == Drag::None &&
                                    mouse.y >= top && mouse.y < gridBottom && handleAt(handleRow, handleCycle);
            if(overHandle) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    auto& row = lane.drumRows[static_cast<size_t>(handleRow)];
                    row.length = std::min(DrumGrid::kMaxLength, content);
                    DrumGrid::normalizeRow(row);
                } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    drag = Drag::Length;
                    dragLaneId = lane.id;
                    dragRow = handleRow;
                    dragLengthCycle = handleCycle;
                    focusedRows[lane.id] = handleRow;
                } else ImGui::SetTooltip("End of the row's cycle: drag to change its length (Alt: free).\n"
                                         "Double-click: as long as the clip. Hits past it are kept, silent.");
            }
            if(drag == Drag::Length && dragLaneId == lane.id && dragRow >= 0 && dragRow < rowCount) {
                auto& row = lane.drumRows[static_cast<size_t>(dragRow)];
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // Source beats along the grabbed clip repeat (its pattern starts at 0).
                    const float x0 = xOf(0.0, dragLengthCycle), x1 = xOf(1.0, dragLengthCycle);
                    double beats = (mouse.x - x0) / std::max(1e-3f, x1 - x0);
                    beats = io.KeyAlt ? std::max(DrumGrid::kMinLength, beats) : std::max(grid, std::round(beats / grid) * grid);
                    row.length = std::min(DrumGrid::kMaxLength, beats);
                    DrumGrid::normalizeRow(row);
                    const double count = row.length / grid;
                    char text[48];
                    if(std::abs(count - std::round(count)) < 1e-6)
                        std::snprintf(text, sizeof(text), "%d steps", static_cast<int>(std::lround(count)));
                    else std::snprintf(text, sizeof(text), "%.3g b", row.length);
                    ImGui::SetTooltip("%s", text);
                }
            }
            // Grid: click toggles; drag from an empty cell paints hits, from a
            // hit moves it (horizontally, by grid steps; Alt: freely) or sets its
            // velocity (vertically, or Shift); Cmd/Ctrl-drag erases.
            if(overGrid && !overHandle && drag != Drag::Length && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const int r = rowAtY(mouse.y);
                double sourceBeat = 0.0;
                if(r >= 0 && sourceAtX(mouse.x, sourceBeat)) {
                    auto& row = lane.drumRows[static_cast<size_t>(r)];
                    focusedRows[lane.id] = r;
                    dragLaneId = lane.id;
                    dragRow = r;
                    dragStartPos = patternPos(row, sourceBeat);
                    dragCellBeat = std::floor(dragStartPos / grid) * grid;
                    dragStep = grabAt(row, sourceBeat);
                    dragStartY = mouse.y;
                    previousMouseX = mouse.x;
                    previousMouseY = mouse.y;
                    if(io.KeyCtrl || io.KeySuper) {
                        drag = Drag::Erase;
                        if(dragStep >= 0) row.steps.erase(row.steps.begin() + dragStep);
                        dragStep = -1;
                    } else if(dragStep >= 0) {
                        const auto& step = row.steps[static_cast<size_t>(dragStep)];
                        dragStartValue = step.velocity;
                        dragStartBeat = step.beat;
                        drag = io.KeyShift ? Drag::Velocity : Drag::Pending;
                    } else drag = Drag::Pending;
                }
            }
            if(dragLaneId == lane.id && ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragRow >= 0 && dragRow < rowCount &&
               (drag == Drag::Pending || drag == Drag::Paint || drag == Drag::Erase || drag == Drag::Velocity || drag == Drag::Move)) {
                auto& row = lane.drumRows[static_cast<size_t>(dragRow)];
                double sourceBeat = 0.0;
                const bool inside = sourceAtX(mouse.x, sourceBeat);
                const double pos = inside ? patternPos(row, sourceBeat) : dragStartPos;
                if(drag == Drag::Pending) {
                    const bool movedCell = inside && std::floor(pos / grid) * grid != dragCellBeat;
                    const int r = rowAtY(mouse.y);
                    if(dragStep >= 0 && std::abs(mouse.y - dragStartY) > 4.0f && !movedCell && r == dragRow) drag = Drag::Velocity;
                    else if(dragStep >= 0 && (movedCell || std::abs(mouse.x - previousMouseX) > 6.0f)) drag = Drag::Move;
                    else if(dragStep < 0 && (movedCell || (r >= 0 && r != dragRow))) {
                        drag = Drag::Paint;
                        if(DrumGrid::stepInRange(row, dragCellBeat, dragCellBeat + grid, dragCellBeat) < 0)
                            DrumGrid::addStep(row, newStep(row, dragCellBeat, grid));
                    }
                }
                if(drag == Drag::Velocity && dragStep >= 0 && dragStep < static_cast<int>(row.steps.size())) {
                    auto& step = row.steps[static_cast<size_t>(dragStep)];
                    step.velocity = ofClamp(dragStartValue + (dragStartY - mouse.y) / (120.0f * scale), 0.0f, 1.0f);
                    ImGui::SetTooltip("Velocity %.0f", step.velocity * 100.0f);
                } else if(drag == Drag::Move && dragStep >= 0 && dragStep < static_cast<int>(row.steps.size())) {
                    // By whole grid steps (keeping an off-grid hit's offset); Alt: freely.
                    double delta = pos - dragStartPos;
                    if(!io.KeyAlt) delta = std::round(delta / grid) * grid;
                    double beat = std::fmod(dragStartBeat + delta, row.length);
                    if(beat < 0.0) beat += row.length;
                    auto moved = row.steps[static_cast<size_t>(dragStep)];
                    moved.beat = beat;
                    row.steps.erase(row.steps.begin() + dragStep);
                    dragStep = DrumGrid::addStep(row, moved);
                    char text[48];
                    std::snprintf(text, sizeof(text), "Beat %.4g%s", beat, onGrid(beat, grid) ? "" : " (off grid)");
                    ImGui::SetTooltip("%s", text);
                } else if(drag == Drag::Paint || drag == Drag::Erase) {
                    // Every cell the stroke crossed since the last frame.
                    const float distance = std::max(std::abs(mouse.x - previousMouseX), std::abs(mouse.y - previousMouseY));
                    const int samples = std::max(1, static_cast<int>(distance / 2.0f));
                    for(int k = 1; k <= samples; ++k) {
                        const float t = static_cast<float>(k) / samples;
                        const int r = rowAtY(previousMouseY + (mouse.y - previousMouseY) * t);
                        double sample = 0.0;
                        if(r < 0 || !sourceAtX(previousMouseX + (mouse.x - previousMouseX) * t, sample)) continue;
                        auto& target = lane.drumRows[static_cast<size_t>(r)];
                        const double cell = std::floor(patternPos(target, sample) / grid) * grid;
                        if(cell >= target.length - 1e-9) continue;
                        const int existing = DrumGrid::stepInRange(target, cell, cell + grid, cell);
                        if(drag == Drag::Paint && existing < 0) DrumGrid::addStep(target, newStep(target, cell, grid));
                        if(drag == Drag::Erase && existing >= 0) target.steps.erase(target.steps.begin() + existing);
                    }
                }
                if(drag != Drag::Move && drag != Drag::Pending) { previousMouseX = mouse.x; previousMouseY = mouse.y; }
            }
            if(overGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                const int r = rowAtY(mouse.y);
                double sourceBeat = 0.0;
                if(r >= 0 && sourceAtX(mouse.x, sourceBeat)) {
                    const int step = grabAt(lane.drumRows[static_cast<size_t>(r)], sourceBeat);
                    focusedRows[lane.id] = r;
                    if(step >= 0) {
                        popupLaneId = lane.id;
                        popupRow = r;
                        popupStep = step;
                        ImGui::OpenPopup(("##drumStepMenu" + lane.id).c_str());
                    }
                }
            }
            if(overGrid && drag == Drag::None) {
                const int r = rowAtY(mouse.y);
                double sourceBeat = 0.0;
                if(r >= 0 && sourceAtX(mouse.x, sourceBeat)) {
                    const auto& row = lane.drumRows[static_cast<size_t>(r)];
                    const int index = grabAt(row, sourceBeat);
                    if(index >= 0) {
                        const auto& step = row.steps[static_cast<size_t>(index)];
                        ImGui::SetTooltip("%s at beat %.4g%s\nvel %.0f  pitch %+d  prob %.0f%%  ratchet x%d  gate %.0f%%  duration x%.2f  filter %s\n"
                                          "Click: remove. Drag: move / velocity. Right-click: edit",
                                          row.name.c_str(), step.beat, onGrid(step.beat, grid) ? "" : " (off the grid)",
                                          step.velocity * 100.0f, step.pitch, step.probability * 100.0f, step.ratchet,
                                          step.gate * 100.0f, step.duration, filterText(step.filter).c_str());
                    }
                }
            }

            // Strip: paint the focused row's bars (each hit's bar at its x).
            if(focused >= 0 && stripHeight > 0.0f) {
                auto& row = lane.drumRows[static_cast<size_t>(focused)];
                const bool altFree = io.KeyAlt;
                auto valueAtY = [&](float y) { return stripValueAt(mode, (stripBottom - y) / std::max(1.0f, stripBottom - barsTop), altFree); };
                // The hit whose bar is under x: under the pointer, else in that cell.
                auto barAt = [&](float x) {
                    double sourceBeat = 0.0;
                    return sourceAtX(x, sourceBeat) ? grabAt(row, sourceBeat) : -1;
                };
                if(overStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const int index = barAt(mouse.x);
                    if(index >= 0) {
                        dragLaneId = lane.id;
                        dragRow = focused;
                        dragStep = index;
                        dragStartY = mouse.y;
                        previousMouseX = mouse.x;
                        previousMouseY = mouse.y;
                        dragStartValue = stripValue(row.steps[static_cast<size_t>(index)], mode);
                        drag = io.KeyAlt && !altFreePaint(mode) ? Drag::StripFine : Drag::Strip;
                        if(drag == Drag::Strip) setStripValue(row.steps[static_cast<size_t>(index)], mode, valueAtY(mouse.y));
                    }
                }
                if(dragLaneId == lane.id && dragRow == focused && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                   (drag == Drag::Strip || drag == Drag::StripFine)) {
                    if(drag == Drag::StripFine && dragStep >= 0 && dragStep < static_cast<int>(row.steps.size())) {
                        auto& step = row.steps[static_cast<size_t>(dragStep)];
                        setStripValue(step, mode, dragStartValue + (dragStartY - mouse.y) * stripFineRate(mode));
                        ImGui::SetTooltip("%s %s", kStripNames[static_cast<int>(mode)], stripText(step, mode).c_str());
                    } else if(drag == Drag::Strip) {
                        // Every bar between the last mouse position and this one.
                        const float distance = std::abs(mouse.x - previousMouseX);
                        const int samples = std::max(1, static_cast<int>(distance / 2.0f));
                        for(int k = 1; k <= samples; ++k) {
                            const float t = static_cast<float>(k) / samples;
                            const int index = barAt(previousMouseX + (mouse.x - previousMouseX) * t);
                            if(index >= 0) setStripValue(row.steps[static_cast<size_t>(index)], mode,
                                                         valueAtY(previousMouseY + (mouse.y - previousMouseY) * t));
                        }
                        const int index = barAt(mouse.x);
                        if(index >= 0) ImGui::SetTooltip("%s %s", kStripNames[static_cast<int>(mode)],
                                                         stripText(row.steps[static_cast<size_t>(index)], mode).c_str());
                    }
                    previousMouseX = mouse.x;
                    previousMouseY = mouse.y;
                }
                if(overStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    const int index = barAt(mouse.x);
                    if(index >= 0) setStripValue(row.steps[static_cast<size_t>(index)], mode, stripDefault(mode, row));
                }
                // Filter: double-click a bar opens it.
                if(overStrip && mode == StripMode::Filter && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    const int index = barAt(mouse.x);
                    if(index >= 0) row.steps[static_cast<size_t>(index)].filter = 0.0f;
                }
                if(overStrip && drag == Drag::None) {
                    const int index = barAt(mouse.x);
                    if(index >= 0) ImGui::SetTooltip("%s %s", kStripNames[static_cast<int>(mode)],
                                                     stripText(row.steps[static_cast<size_t>(index)], mode).c_str());
                }
            }
        }
    }
    if(popupLaneId == lane.id) {
        if(ImGui::BeginPopup(("##drumGridMenu" + lane.id).c_str())) {
            ImGui::TextDisabled("Grid of this lane");
            if(gridChoices(lane, context.timeline.getMasterGridBeats())) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        drawRowMenu(lane, grid);
        drawStepMenu(lane, grid);
    }
}

void ofxOceanodeTimelineDrumGridEditor::drawRowMenu(ofxOceanodeTimelineLane& lane, double grid) {
    if(!ImGui::BeginPopup(("##drumRowMenu" + lane.id).c_str())) return;
    if(popupRow < 0 || popupRow >= static_cast<int>(lane.drumRows.size())) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& row = lane.drumRows[static_cast<size_t>(popupRow)];
    ImGui::SetNextItemWidth(160.0f);
    if(ImGui::InputText("Name##drumRowRename", renameBuffer, sizeof(renameBuffer))) row.name = renameBuffer;
    int steps = static_cast<int>(std::lround(row.length / grid));
    ImGui::SetNextItemWidth(160.0f);
    if(ImGui::DragInt("Steps##drumRowMenu", &steps, 0.2f, 1, static_cast<int>(DrumGrid::kMaxLength / grid))) {
        row.length = std::max(1, steps) * grid;
        DrumGrid::normalizeRow(row);
    }
    float beats = static_cast<float>(row.length);
    ImGui::SetNextItemWidth(160.0f);
    if(ImGui::DragFloat("Beats##drumRowMenu", &beats, 0.01f, static_cast<float>(DrumGrid::kMinLength),
                        static_cast<float>(DrumGrid::kMaxLength), "%.3g")) {
        row.length = beats;
        DrumGrid::normalizeRow(row);
    }
    float gate = row.gate * 100.0f;
    ImGui::SetNextItemWidth(160.0f);
    if(ImGui::SliderFloat("Gate (new hits)##drumRowMenu", &gate, 5.0f, 100.0f, "%.0f%%")) row.gate = ofClamp(gate / 100.0f, 0.05f, 1.0f);
    if(ImGui::SmallButton("Set every hit to this gate")) for(auto& step : row.steps) step.gate = row.gate;
    ImGui::SetNextItemWidth(160.0f);
    if(ImGui::DragInt("Slot##drumRowMenu", &row.slot, 0.1f, 0, 127)) row.slot = std::max(0, row.slot);
    contextMenuSection("Pattern (grid steps)");
    if(ImGui::MenuItem("Clear")) clearVisible(row);
    for(int every : {2, 3, 4, 8}) {
        if(ImGui::MenuItem(("Fill every " + ofToString(every)).c_str())) fillEvery(row, grid, every);
    }
    if(ImGui::MenuItem("Shift left")) shiftRow(row, -grid);
    if(ImGui::MenuItem("Shift right")) shiftRow(row, grid);
    if(ImGui::MenuItem("Quantize to grid")) {
        for(auto& step : row.steps)
            if(!DrumGrid::stepHidden(row, step)) step.beat = std::round(step.beat / grid) * grid;
        shiftRow(row, 0.0);
        // Two hits on one step: keep the first.
        row.steps.erase(std::unique(row.steps.begin(), row.steps.end(),
                                    [](const auto& a, const auto& b) { return std::abs(a.beat - b.beat) < 1e-9; }),
                        row.steps.end());
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Move every hit to the nearest step of the current grid");
    if(ImGui::MenuItem("Randomize steps")) {
        // About one grid step in three gets a hit; hits already on a step keep their settings.
        std::vector<Step> kept;
        for(const auto& step : row.steps) if(DrumGrid::stepHidden(row, step)) kept.push_back(step);
        for(double beat = 0.0; beat < row.length - 1e-9; beat += grid) {
            if(ofRandom(1.0f) >= 0.35f) continue;
            const int existing = DrumGrid::stepInRange(row, beat, beat + grid, beat);
            kept.push_back(existing >= 0 ? row.steps[static_cast<size_t>(existing)] : newStep(row, beat, grid));
        }
        row.steps = std::move(kept);
        DrumGrid::normalizeRow(row);
    }
    if(ImGui::MenuItem("Randomize velocities"))
        for(auto& step : row.steps) step.velocity = ofRandom(0.4f, 1.0f);
    contextMenuSection("Row");
    if(ImGui::MenuItem("Copy row")) { clipboard = row; hasClipboard = true; }
    if(ImGui::MenuItem("Paste row", nullptr, false, hasClipboard)) {
        const int slot = row.slot;
        const std::string name = row.name;
        const bool mute = row.mute, solo = row.solo;
        row = clipboard;
        row.slot = slot; row.name = name; row.mute = mute; row.solo = solo;
        DrumGrid::normalizeRow(row);
    }
    bool remove = false, add = false;
    if(ImGui::MenuItem("Add row below", nullptr, false,
                       lane.drumRows.size() < static_cast<size_t>(DrumGrid::kMaxRows))) add = true;
    if(ImGui::MenuItem("Remove row", nullptr, false, lane.drumRows.size() > 1)) remove = true;
    if(add) {
        int slot = 0;
        for(const auto& other : lane.drumRows) slot = std::max(slot, other.slot + 1);
        auto added = DrumGrid::defaultRows(slot + 1, {}, row.length).back();
        lane.drumRows.insert(lane.drumRows.begin() + popupRow + 1, std::move(added));
        ImGui::CloseCurrentPopup();
    } else if(remove) {
        lane.drumRows.erase(lane.drumRows.begin() + popupRow);
        popupRow = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ofxOceanodeTimelineDrumGridEditor::drawStepMenu(ofxOceanodeTimelineLane& lane, double grid) {
    if(!ImGui::BeginPopup(("##drumStepMenu" + lane.id).c_str())) return;
    if(popupRow < 0 || popupRow >= static_cast<int>(lane.drumRows.size()) || popupStep < 0 ||
       popupStep >= static_cast<int>(lane.drumRows[static_cast<size_t>(popupRow)].steps.size())) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    auto& row = lane.drumRows[static_cast<size_t>(popupRow)];
    auto& step = row.steps[static_cast<size_t>(popupStep)];
    ImGui::Text("%s, hit at beat %.4g%s", row.name.c_str(), step.beat, onGrid(step.beat, grid) ? "" : " (off the grid)");
    // Drag to change; double-click (or Ctrl+click) to type a value.
    float beat = static_cast<float>(step.beat);
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Beat##drumStep", &beat, static_cast<float>(grid) / 8.0f, 0.0f, static_cast<float>(row.length), "%.4g")) {
        auto moved = step;
        moved.beat = ofClamp(static_cast<double>(beat), 0.0, row.length - 1e-6);
        row.steps.erase(row.steps.begin() + popupStep);
        popupStep = DrumGrid::addStep(row, moved);
        ImGui::EndPopup();
        return;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Position from the start of the row's pattern, in beats");
    float length = static_cast<float>(step.length);
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Length (beats)##drumStep", &length, static_cast<float>(grid) / 8.0f, 1.0f / 96.0f, 16.0f, "%.4g"))
        step.length = ofClamp(static_cast<double>(length), 1.0 / 96.0, 16.0);
    float gate = step.gate * 100.0f;
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Gate##drumStep", &gate, 0.5f, 5.0f, 100.0f, "%.0f%%")) step.gate = ofClamp(gate / 100.0f, 0.05f, 1.0f);
    float velocity = step.velocity * 100.0f;
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Velocity##drumStep", &velocity, 0.5f, 0.0f, 100.0f, "%.0f")) step.velocity = ofClamp(velocity / 100.0f, 0.0f, 1.0f);
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragInt("Pitch (st)##drumStep", &step.pitch, 0.1f, -24, 24, "%+d")) step.pitch = std::max(-24, std::min(24, step.pitch));
    float probability = step.probability * 100.0f;
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Probability##drumStep", &probability, 0.5f, 0.0f, 100.0f, "%.0f%%")) step.probability = ofClamp(probability / 100.0f, 0.0f, 1.0f);
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragInt("Ratchet##drumStep", &step.ratchet, 0.05f, 1, DrumGrid::kMaxRatchet, "x%d"))
        step.ratchet = std::max(1, std::min(DrumGrid::kMaxRatchet, step.ratchet));
    ImGui::SetNextItemWidth(140.0f);
    if(ImGui::DragFloat("Duration##drumStep", &step.duration, 0.01f, 0.1f, kDurationMax, "x%.2f", ImGuiSliderFlags_Logarithmic))
        step.duration = ofClamp(step.duration, 0.1f, kDurationMax);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplies the hit's decays (x1 = as set)");
    {
        float filter = step.filter * 100.0f;
        ImGui::SetNextItemWidth(140.0f);
        if(ImGui::DragFloat("Filter##drumStep", &filter, 0.5f, -100.0f, 100.0f, "%+.0f"))
            step.filter = ofClamp(filter / 100.0f, -1.0f, 1.0f);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n-100 low-pass fully closed .. 0 open .. +100 high-pass fully closed",
                                                     filterText(step.filter).c_str());
    }
    if(ImGui::Button("Snap to grid##drumStep")) {
        auto moved = step;
        moved.beat = std::fmod(std::round(step.beat / grid) * grid, row.length);
        row.steps.erase(row.steps.begin() + popupStep);
        popupStep = DrumGrid::addStep(row, moved);
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    if(ImGui::Button("Delete##drumStep")) {
        row.steps.erase(row.steps.begin() + popupStep);
        popupStep = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ofxOceanodeTimelineDrumGridEditor::drawPreview(const ofxOceanodeTimelineClipPreview& preview) const {
    const auto& clip = preview.clip;
    const auto* lane = preview.lane;
    if(lane == nullptr || lane->drumRows.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& track = preview.track;
    const auto& min = preview.min;
    const auto& max = preview.max;
    const int rows = static_cast<int>(lane->drumRows.size());
    const double content = sourceDuration(clip);
    const int clipCycles = clip.repeatContent
        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip) - 1e-9))) : 1;
    const float rowStep = std::max(1.0f, (max.y - min.y - 6.0f) / rows);
    const float radius = ofClamp(rowStep * 0.32f, 0.8f, 2.5f);
    int drawn = 0;
    dl->PushClipRect(ImVec2(preview.left + 1.0f, min.y + 2.0f), ImVec2(preview.right - 1.0f, max.y - 2.0f), true);
    for(int cycle = std::max(0, preview.firstVisibleCycle); cycle < clipCycles && cycle <= preview.lastVisibleCycle; ++cycle) {
        for(int r = 0; r < rows; ++r) {
            const auto& row = lane->drumRows[static_cast<size_t>(r)];
            const float y = min.y + 3.0f + (r + 0.5f) * rowStep;
            const ImU32 color = DrumGrid::rowAudible(*lane, row) ? IM_COL32(245, 245, 245, 210)
                                                                   : IM_COL32(track.color.r, track.color.g, track.color.b, 90);
            const double length = std::max(DrumGrid::kMinLength, row.length);
            for(double base = 0.0; base < content && drawn < 20000; base += length) {
                for(const auto& step : row.steps) {
                    const double beat = base + step.beat + DrumGrid::swingOffset(*lane, step);
                    if(beat < 0.0 || beat >= content) continue;
                    const float x = min.x + preview.view.beatToPixels(sourceToTimelineBeat(clip, beat, cycle));
                    if(x < preview.left || x > preview.right) continue;
                    dl->AddCircleFilled(ImVec2(x + radius, y), radius, color);
                    ++drawn;
                }
            }
        }
    }
    dl->PopClipRect();
}
