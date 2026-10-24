#pragma once
#include <set>
#include <string>
#include <unordered_map>

// Shared shell state: focus, folding and row layout. Per-type interaction
// state belongs to the editors themselves. No model pointers survive a frame.
struct ofxOceanodeTimelineClipEditorState {
    std::string editorTrackId;
    std::string editorClipId;
    std::string editorLaneId;
    // Independent expand/collapse state for each lane in a multi-lane
    // clip's editor (keyed by lane id, globally unique). Absence from
    // this set means expanded -- most lanes are expanded most of the
    // time, so "collapsed" is the state worth tracking explicitly.
    std::set<std::string> collapsedLaneIds;
    // Per-lane editor height override (keyed by lane id), set by dragging
    // the resize handle at the bottom of a lane's editor row. Absence means
    // "use the type's default height" -- UI-only, not persisted with the
    // preset.
    std::unordered_map<std::string, float> laneEditorHeights;
    // The resize grip is handled from its screen rectangle instead of as an
    // overlapping ImGui item. This keeps the editor canvas/child window from
    // stealing the active id after the drag has started.
    std::string resizingLaneId;
    float laneResizeStartMouseY = 0.0f;
    float laneResizeStartHeight = 0.0f;
    // Clip editors folded down to their header row (session state, per clip id).
    std::set<std::string> foldedClipEditors;
    bool clipEditorOpen = false;
};
