#ifndef ofxOceanodeTimelineController_h
#define ofxOceanodeTimelineController_h

#include "ofxOceanodeBaseController.h"
#include "Editors/ofxOceanodeTimelineBpmEditor.h"
#include "Editors/ofxOceanodeTimelineEditorView.h"
#include "Editors/ofxOceanodeTimelineWaveVolumeEditor.h"
#include "Editors/ofxOceanodeTimelineLaneEditors.h"
#include "Editors/ofxOceanodeTimelineLfoEditor.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ofxOceanodeContainer;
class ofxOceanodeAbstractParameter;
class ofxOceanodeTimelineManager;
struct ofxOceanodeTimelineTrack;
struct ofxOceanodeTimelineClip;
struct ofxOceanodeTimelineParameterBinding;

class ofxOceanodeTimelineController : public ofxOceanodeBaseController {
public:
    explicit ofxOceanodeTimelineController(std::shared_ptr<ofxOceanodeContainer> container);

    void draw() override;
    // Handles a track name/creation request queued by anything outside this
    // controller (currently: a node parameter's right-click "Add to Timeline
    // Track -> New Timeline Track"). Call this every frame from
    // ofxOceanodeControls::draw() UNCONDITIONALLY -- not gated behind this
    // controller's own window being visible/focused the way draw() is.
    // Requesting a new track can happen while the Timeline window is closed
    // or just isn't the active docked tab; if consuming the request were
    // left inside draw() (as it used to be), it would silently wait until
    // the user happened to bring that window/tab forward, and would then
    // pop up anchored to wherever that window/tab's viewport is instead of
    // the main canvas the user was actually looking at.
    void drawPendingTrackPopup();
    void drawBeforeWindows() override { drawPendingTrackPopup(); }

private:
    void drawRuler(ofxOceanodeTimelineManager& timeline, float labelWidth, float timelineWidth,
                   double endBeat, double beatPosition, float bpm);
    void drawLaneEditor(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
                        float contentWidth, double endBeat, double beatPosition);
    void drawWaveClipProperties(ofxOceanodeTimelineManager& timeline,
                                const ofxOceanodeTimelineTrack& track,
                                ofxOceanodeTimelineClip& clip);
    void drawWaveTrackEditor(ofxOceanodeTimelineManager& timeline,
                             const ofxOceanodeTimelineTrack& track,
                             float contentWidth, double endBeat,
                             double beatPosition);
    void drawBlendModeOptions(ofxOceanodeTimelineManager& timeline, const std::string& trackId,
                             const ofxOceanodeTimelineParameterBinding& binding);
    void requestClipRename(const std::string& trackId, const ofxOceanodeTimelineClip& clip);
    void drawClipRenamePopup(ofxOceanodeTimelineManager& timeline);
    void drawRenamePopup(ofxOceanodeTimelineManager& timeline);
    void drawClipPopup(ofxOceanodeTimelineManager& timeline);
    bool createWaveClipFromFile(ofxOceanodeTimelineManager& timeline,
                                const std::string& trackId,
                                const std::string& filePath,
                                double startBeat,
                                double durationBeats);
    bool replaceWaveClipFromFile(ofxOceanodeTimelineManager& timeline,
                                 const std::string& trackId,
                                 const std::string& clipId,
                                 const std::string& filePath);
    // Shortens a Wave clip from the left (cuts there and drops the left part).
    bool trimWaveClipStart(ofxOceanodeTimelineManager& timeline, const std::string& trackId,
                           const std::string& clipId, double newStartBeat);
    bool splitWaveClipAtPlayhead(ofxOceanodeTimelineManager& timeline,
                                 const std::string& trackId,
                                 const std::string& clipId,
                                 double playheadBeat);
    double getContentEndBeat(const ofxOceanodeTimelineManager& timeline) const;
    double snapBeat(double beat) const;
    double editIncrement() const;
    double displayGridBeats() const;
    float beatToPixels(const ofxOceanodeTimelineManager& timeline, double beat, float fallbackBpm) const;
    double pixelsToBeat(const ofxOceanodeTimelineManager& timeline, float pixels,
                        float fallbackBpm, double endBeatHint) const;

    ofxOceanodeTimelineEditorView editorView(const ofxOceanodeTimelineManager& timeline,
                                            float fallbackBpm, double endBeat) const;
    ofxOceanodeTimelineClipEditorState editorState;
    ofxOceanodeTimelineLfoEditor lfoEditor;
    ofxOceanodeTimelineLaneEditors laneEditors;
    ofxOceanodeTimelineBpmEditor bpmEditor;
    ofxOceanodeTimelineWaveVolumeEditor waveVolumeEditor;
    std::shared_ptr<ofxOceanodeContainer> container;
    int visibleBars = 8;
    double rulerSnapBeats = 0.25;
    bool snapEnabled = true; // magnet toggle: clip, marker, loop and playhead moves snap to the grid
    // Derived from the selected snap and current zoom. The selected value is
    // kept separately so zooming back in restores the user's finer grid.
    double effectiveRulerSnapBeats = 0.25;
    std::string pendingTrackId;
    std::string pendingClipId;
    std::string pendingLaneId;
    std::string waveTrackResizeId;
    float waveTrackResizeStartMouseY = 0.0f;
    float waveTrackResizeStartHeight = 0.0f;
    char pendingTrackName[128] = {};
    char pendingClipName[128] = {};
    char pendingClipRenameName[128] = {};
    std::string clipRenameTrackId;
    std::string clipRenameClipId;
    std::string pendingClipBindingId;
    int pendingClipLaneType = 1; // Curve
    double pendingStartBeat = 0.0;
    double pendingDurationBeats = 1.0;
    // Beat used by a Wave Track's context-menu "Add new wave clip" action.
    // Header clicks use the playhead; empty-row clicks use the clicked beat.
    double pendingWaveClipBeat = 0.0;

    // Duplicate: Cmd+drag on a clip's body drops a copy on whichever row the
    // mouse is over (same row, another parameter row, another track).
    enum class ClipDragMode { None, Move, Resize, Stretch, Repeat, TrimStart, Duplicate };
    ClipDragMode clipDragMode = ClipDragMode::None;
    std::string draggingTrackId;
    std::string draggingClipId;
    double dragOffsetBeats = 0.0;
    double dragInitialContentDuration = 4.0;
    double dragInitialContentStretch = 1.0;
    float dragTimelineOriginX = 0.0f;
    // A press only becomes a drag once the mouse has moved: a plain click
    // (or the double-click that opens the editor) must not change the clip.
    float clipDragStartMouseX = 0.0f;
    bool clipDragCommitted = false;
    float clipDragStartMouseY = 0.0f;
    // Duplicate drag: the row and lane it started on, and every clip row
    // drawn this frame (rebuilt each frame) to find the one under the mouse.
    std::string dragSourceBindingId;
    std::string dragSourceLaneId;
    struct ClipDropRow {
        std::string trackId;
        std::string bindingId; // empty: a single-row (collapsed / Wave) track
        float top = 0.0f, bottom = 0.0f;
    };
    std::vector<ClipDropRow> clipDropRows;
    // Left-edge trim: previewed while dragging, applied on release.
    double trimPreviewBeat = 0.0;
    bool trimExtends = false; // Shift on the left edge: extendClipStart instead of trimClipStart
    float dragRowTop = 0.0f, dragRowBottom = 0.0f;
    // Screen rows of the clip editor drawn this frame (a click there is not a
    // click on empty timeline space).
    float editorScreenTop = 0.0f, editorScreenBottom = 0.0f;
    bool editorScreenValid = false;
    bool lastClickInEditor = false; // Delete then edits the editor's content, not the clip
    // Ephemeral multi-clip selection built with Shift+click on a clip (see
    // handleClip in the .cpp). This is the input to the "Group" button/menu
    // item and to a plain drag that keeps multiple selected clips moving
    // together even before they're formally grouped -- it is not persisted
    // and is a separate concept from a *group* (ofxOceanodeTimelineClipGroup),
    // which persists once created. Keyed by (trackId, clipId).
    std::set<std::pair<std::string, std::string>> selectedClips;
    // Set once per frame (across every track, not just the one currently
    // being drawn) whenever a clip consumes a mouse click -- lets the
    // "clicked empty timeline space" handler know a click already had a
    // more specific target, without it having to know which track that was.
    bool anyClipInteractionClaimedThisFrame = false;
    // Snapshot of every clip that should move/stretch together with the one
    // being dragged (its persistent group's members, or the current
    // Shift+click selection if the dragged clip is part of one bigger than
    // itself), taken once when the drag starts so the whole set scales from
    // one shared reference each frame instead of drifting from repeatedly
    // re-deriving deltas off values already mutated this frame. Empty means
    // the current drag (if any) is an ordinary single-clip drag.
    struct GroupDragSnapshotEntry {
        std::string trackId, clipId;
        double startBeat, durationBeats, contentDurationBeats, contentStretch;
    };
    std::vector<GroupDragSnapshotEntry> groupDragSnapshot;
    double groupDragAnchorBeat = 0.0;
    bool requestRenamePopup = false;
    bool pendingNewTrackDialog = false;
    bool requestClipPopup = false;
    bool requestClipRenamePopup = false;
    // Track removal is deferred until the track iteration has finished;
    // erasing from timeline.getTracks() inside its context menu would
    // invalidate the current track and every clip/lane reference below it.
    bool requestTrackDeletion = false;
    std::string trackDeletionId;
    bool requestClipDeletion = false;
    std::string clipDeletionTrackId;
    std::string clipDeletionClipId;
    bool requestSeparateStateCaptureClip = false;
    std::string separateStateCaptureTrackId;
    std::string separateStateCaptureClipId;
    bool requestWaveSplit = false;
    std::vector<std::string> midiPortChoices; // MIDI clock sync popup

    // ---- Markers ----
    std::string markerDragId;          // pressed marker (a click jumps, a drag moves it)
    float markerDragStartX = 0.0f;
    bool markerDragMoved = false;
    std::string markerMenuId;          // marker under a right-click ("" = empty ruler)
    double markerMenuBeat = 0.0;
    std::string markerRenameId;
    char markerRenameBuffer[128] = "";
    bool requestMarkerRename = false;
    // Jumps (marker clicks) can wait for the next bar while playing.
    bool quantizeJumps = false;
    bool pendingJump = false;
    double pendingJumpTarget = 0.0;
    double pendingJumpAt = 0.0;
    uint64_t pendingJumpLoopCount = 0;
    void jumpToBeat(double beat);
    void drawMarkerRenamePopup(ofxOceanodeTimelineManager& timeline);

    // ---- View ----
    bool followPlayhead = false;       // scroll to keep the playhead in view while playing
    bool requestZoomToFit = false;     // fit every clip and marker in the visible width
    std::vector<double> tapTimes;      // tap tempo (seconds)

    // ---- Clip box selection ----
    struct ClipScreenRect { std::string trackId, clipId; float x1, y1, x2, y2; };
    std::vector<ClipScreenRect> clipScreenRects; // clips drawn this frame
    bool marqueePending = false;       // pressed on empty track space
    bool marqueeActive = false;        // dragged far enough: selecting
    float marqueeStartX = 0.0f, marqueeStartY = 0.0f;
    bool marqueeAdditive = false;
    double clipMenuBeat = 0.0;         // beat under the right-click that opened a clip menu
    double clipSplitBeat = 0.0;
    bool requestClipSplit = false;
    std::string clipSplitTrackId;
    std::string clipSplitClipId;
    std::string waveSplitTrackId;
    std::string waveSplitClipId;
    // Picking an audio file opens a blocking OS dialog. Asking for one from
    // inside an ImGui popup or child means the dialog runs with that popup
    // still on the stack and the frame half-built, so the request is only
    // recorded here and serviced once the track loop and its popups are done
    // (see the handler next to the wave-split one in draw()).
    enum class WaveFileRequest { None, NewTrack, NewClip, ReplaceClip };
    WaveFileRequest waveFileRequest = WaveFileRequest::None;
    std::string waveFileRequestTrackId;
    std::string waveFileRequestClipId;
    double waveFileRequestBeat = 0.0;
    std::vector<std::pair<std::string, std::string>> keyboardClipDeletionRequests;
    // "Remove from Timeline" (on a binding's row, or on a collapsed track's
    // header menu) unbinds a parameter entirely. Deferred for the same
    // reason as clip deletion above: both popups that can request this are
    // opened mid-iteration over track.bindings, and code later in that same
    // iteration still reads the binding being removed.
    bool requestRemoveBinding = false;
    std::string removeBindingTrackId;
    std::string removeBindingId;
    // A clip can hold more than one lane (e.g. a curve and a step pattern
    // combined into one clip); these back the clip editor's "Add lane" /
    // "Remove this lane" controls. The actual mutation is deferred to a
    // fixed point in draw(), like clip deletion above, since createLane/
    // removeLane can reallocate the clip's lane vector and the editor keeps
    // pointers into it for the rest of the frame it was requested on.
    bool requestAddLane = false;
    int pendingAddLaneType = 0;
    bool requestRemoveLane = false;
    std::string pendingRemoveLaneId;
    // Dragging a parameter row's label to reorder it within its track.
    std::string bindingDragTrackId;
    std::string bindingDragId;
    float bindingDragStartY = 0.0f;
    bool bindingDragActive = false;
    // Dragging a parameter row's bottom edge to change its height.
    std::string rowResizeTrackId;
    std::string rowResizeBindingId;
    float rowResizeStartY = 0.0f;
    float rowResizeStartHeight = 0.0f;
    // The clip editor opens right below this parameter row (the row it was opened from).
    std::string editorAnchorBindingId;
    // Binding of the row whose clips are being handled (empty for single-row tracks).
    std::string currentRowBindingId;

    // "Piano Roll" dialog: picks the pitch/gate/velocity parameters of a
    // note group (a new one, or retargeting an existing one).
    struct NoteRoleCandidate {
        std::string path;
        std::string label;       // "Node / Parameter"
        bool sameNode = false;   // on the node the dialog was opened from
        ofxOceanodeAbstractParameter* parameter = nullptr;
    };
    std::vector<NoteRoleCandidate> noteSetupCandidates;
    std::string noteSetupRolePaths[3];        // pitch, gate, velocity
    char noteSetupName[128] = "";
    char noteSetupFilter[64] = "";
    std::string noteSetupTrackId;             // empty: a new track
    bool noteSetupTrackFixed = false;         // opened from a track row
    std::string noteSetupEditGroupId;         // retargeting this group
    double noteSetupClipStart = 0.0;
    double noteSetupClipDuration = 4.0;
    // Instruments in the patch (ofxOceanodeNodeModel::getTimelineInstrument):
    // picking one fills the three roles and its voice count.
    struct NoteInstrumentCandidate {
        std::string label;       // the node's name
        std::string rolePaths[3];
        int voices = 0;
        bool kit = false;
    };
    std::vector<NoteInstrumentCandidate> noteSetupInstruments;
    int noteSetupInstrument = -1;             // index into noteSetupInstruments; -1: parameters by hand
    int noteSetupVoices = 0;                  // the group's override; 0: automatic
    bool requestNoteSetupPopup = false;
    // source: the parameter it was opened from (its node's parameters are
    // offered first and used to guess the roles). trackId empty: choose.
    void openNoteGroupSetup(ofxOceanodeTimelineManager& timeline, ofxOceanodeAbstractParameter* source,
                            const std::string& trackId, const std::string& editGroupId,
                            double clipStart = -1.0, double clipDuration = -1.0);
    void drawNoteGroupSetupPopup(ofxOceanodeTimelineManager& timeline);
    enum class LoopDragMode { None, Scrub, Start, End, Move };
    LoopDragMode loopDragMode = LoopDragMode::None;
    double loopDragAnchorBeat = 0.0;
    double loopDragStartBeat = 0.0;
    double loopDragEndBeat = 4.0;

};

#endif
