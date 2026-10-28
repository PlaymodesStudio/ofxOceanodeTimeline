#pragma once
#include "ofxOceanodeTimelineLaneEditor.h"
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ofxOceanodeTimelinePianoRollEditor : public ofxOceanodeTimelineLaneEditor {
public:
    void drawPreview(const ofxOceanodeTimelineClipPreview& preview) const override;
    void drawTargets(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawProperties(ofxOceanodeTimelineLaneEditorContext& context) override;
    bool hasMutation() const override { return true; }
    void drawMutation(ofxOceanodeTimelineLaneEditorContext& context) override;
    bool hasRandomDeviation() const override { return true; }
    void drawRandomDeviation(ofxOceanodeTimelineLaneEditorContext& context) override;
    void drawCanvas(ofxOceanodeTimelineLaneEditorContext& context,
                    const ofxOceanodeTimelineLaneCanvas& canvas) override;
    void clearSelection();
    void updateMonitoring(ofxOceanodeTimelineManager& timeline, const std::string& activeLaneId);
    bool hasNoteSelection(const std::string& laneId) const {
        return pianoSelectionLaneId == laneId && !pianoSelectedNoteIndices.empty();
    }
    void forgetTrack(const std::string& trackId);

private:
    char targetSearch[128] = {};
    bool pianoInsertionDrag = false;
    bool editPreviewActive = false;
    std::string editPreviewTrackId, editPreviewClipId, editPreviewLaneId;
    std::string editPreviewGateId, editPreviewPitchId;
    int editPreviewPitch = -1;
    // Piano-roll keyboard-strip "audition" state: while a key is held down,
    // the lane's Gate/Pitch bindings get a live override (see
    // ofxOceanodeTimelineManager::setLiveOverride) so the bound parameters
    // sound immediately, independent of the playhead or any clip data.
    bool pianoKeyboardPreviewActive = false;
    std::string pianoKeyboardPreviewTrackId;
    std::string pianoKeyboardPreviewGateBindingId;
    std::string pianoKeyboardPreviewClipId, pianoKeyboardPreviewLaneId;
    std::string pianoKeyboardPreviewPitchBindingId;
    int pianoKeyboardPreviewPitch = -1;
    enum class PianoDragMode { None, Move, Resize, Velocity, Probability, Mutation, Stretch };
    PianoDragMode pianoDragMode = PianoDragMode::None;
    int pianoDragNoteIndex = -1;
    double pianoDragBeatOffset = 0.0;
    // Multi-note selection: which notes (by index into the lane's
    // pianoNotes) are selected, plus the state needed to move the whole
    // group together by a common delta instead of snapping each note
    // independently to the mouse.
    std::string pianoSelectionLaneId;
    std::set<int> pianoSelectedNoteIndices;
    struct PianoDragSnapshotEntry { int index; double startBeat; int pitch; double durationBeats = 0.0; };
    std::vector<PianoDragSnapshotEntry> pianoDragSnapshot;
    double pianoDragAnchorStartBeat = 0.0;
    int pianoDragAnchorPitch = 0;
    std::vector<std::pair<int, float>> pianoValueSnapshot;
    float pianoValueAnchor = 0.0f, pianoValueStartMouseY = 0.0f;
    ImVec2 pianoValuePaintPreviousMouse = ImVec2(0, 0);
    // Shift+drag: the selection's timing scaled from its first start (origin)
    // by how far the grabbed beat moved, every start and end on the grid.
    double pianoStretchOrigin = 0.0;
    double pianoStretchGrab = 0.0;
    // The VEL / PROB / MUT strips' heights per lane (session state, like lane
    // heights), resized by the handles above them: 0 none, 1 VEL, 2 PROB, 3 MUT.
    std::map<std::string, std::array<float, 3>> pianoStripHeights;
    int pianoStripResize = 0;
    float pianoStripResizeStartY = 0.0f, pianoStripResizeStartHeight = 0.0f;
    bool pianoMarqueeActive = false;
    float pianoMarqueeStartX = 0.0f;
    float pianoMarqueeStartY = 0.0f;
    // Mutate tab: the group being edited (per lane), and the frame the tab
    // was last drawn, so the roll can light that group's notes up.
    std::string mutationEditLaneId;
    int mutationEditGroup = 0;
    int mutationTabFrame = -1;
    enum class PianoNumericField { None, Velocity, Probability, Mutation };
    PianoNumericField pianoNumericField = PianoNumericField::None;
    int pianoNumericNoteIndex = -1;
    float pianoNumericValue = 0.0f;
};
