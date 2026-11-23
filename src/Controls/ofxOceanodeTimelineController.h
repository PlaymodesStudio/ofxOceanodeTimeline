#ifndef ofxOceanodeTimelineController_h
#define ofxOceanodeTimelineController_h

#include "ofxOceanodeBaseController.h"
#include "Editors/ofxOceanodeTimelineBpmEditor.h"
#include "Editors/ofxOceanodeTimelineEditorView.h"
#include "Editors/ofxOceanodeTimelineWaveVolumeEditor.h"
#include "Editors/ofxOceanodeTimelineLaneEditors.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include "imgui.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <array>
#include <memory>
#include <optional>
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
    // A further Timeline window (windowId > 0): an editor of its own, hosted
    // and drawn by the main Timeline controller, showing the timeline the
    // manager's window list gives it.
    ofxOceanodeTimelineController(std::shared_ptr<ofxOceanodeContainer> container, int windowId);

    // The editor of one window: every timeline lives in a window of its own
    // (see drawTimelineWindows). The main controller (windowId 0) draws no
    // window of its own: it opens, draws and focuses the timeline windows,
    // routes requests to them, and shows the dialogs. (A test harness may
    // still draw it as an editor: it then handles requests itself.)
    void draw() override;
    bool drawsOwnWindows() const override { return true; }
    // Handles a request queued by anything outside this controller (a node
    // parameter's right-click "Add to Timeline", a track rename, a piano
    // roll setup), on the editor that shows the timeline it names. Called
    // every frame from the main controller's drawBeforeWindows, on the
    // editor it picks (requestTaker) -- never gated behind a window being
    // visible or focused: the request would otherwise wait until its tab
    // came forward, and its dialog would anchor to that window's viewport
    // instead of the main canvas the user was looking at.
    void drawPendingTrackPopup();
    void drawBeforeWindows() override;
    // The New Timeline dialog, from the persistent DockSpace.
    void drawPopups() override;
    // The Window menu's Timelines list: every timeline, ticked while open.
    void drawWindowMenuItems() override;
    // Shows a macro's canvas in this window (its first timeline, or its
    // empty state) from the next frame: never mid-draw.
    void requestMacroScope(ofxOceanodeContainer& macroContainer) { pendingMacroScopeContainer = &macroContainer; }

private:
    void drawSynthNavigation(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
                             const ofxOceanodeTimelineClip* clip = nullptr);
    // Arrange items of the track and folder menus: order among siblings,
    // filing into a folder, moving out of one (applied after the track loop).
    void drawTrackArrangeMenu(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track);
    std::string pendingSynthWindowFocus;
    int pendingSynthWindowFocusFrames = 0;
    void filesDropped(ofDragInfo& info);
    void queueAudioDrop(const std::vector<std::string>& files, float x, float y);
    void bindNativeFileDrops();
    void updateAudioDropPreview(const std::vector<std::string>& files, float x, float y);
    void updateNativeAudioDropPreview();
    void drawAudioDropPreview(ofxOceanodeTimelineManager& timeline);
    struct AudioDropPreviewFile {
        std::string path;
        double durationSeconds = 0.0;
    };
    std::vector<std::string> audioDropPreviewPaths;
    std::vector<AudioDropPreviewFile> audioDropPreviewFiles;
    float audioDropPreviewX = 0.0f, audioDropPreviewY = 0.0f;
    bool audioDropPreviewActive = false;
    std::shared_ptr<void> nativeFileDropBinding;
    void* nativeFileDropWindow = nullptr;
    void importDroppedAudio(ofxOceanodeTimelineManager& timeline);
    ofEventListener fileDropListener;
    ofRectangle fileDropBounds;
    bool fileDropMainViewport = true;
    float fileDropOriginX = 0.0f;
    double fileDropEndBeat = 0.0;
    float fileDropBpm = 120.0f;
    struct AudioFileDrop {
        std::vector<std::string> files;
        std::string trackId;
        double beat = 0.0;
    };
    std::vector<AudioFileDrop> pendingAudioDrops;

    const std::vector<std::string>& patchTargetPaths() const;
    // (One list per canvas: the timeline shown and those unfolded in it
    // belong to different ones, and their lanes are drawn in the same frame.)
    struct PatchTargetPaths {
        std::vector<std::string> paths;
        // A macro's timeline: where each canvas's parameters start in paths
        // (its own first, then each canvas above it), with a heading.
        std::vector<std::pair<size_t, std::string>> groups;
        uint64_t time = 0;
        uint64_t fingerprint = 0;
        bool valid = false;
    };
    const PatchTargetPaths& patchTargets() const;
    mutable std::map<const ofxOceanodeContainer*, PatchTargetPaths> patchTargetPathsCache;
    struct TargetAssignment {
        std::string trackId, clipId, laneId, path;
        ofxOceanodeTimelineTargetRole role = ofxOceanodeTimelineTargetRole::Lane;
        ofxOceanodeTimelineSequence* sequence = nullptr; // null: the scope's
    };
    TargetAssignment pendingTargetAssignment;
    char laneTargetSearch[128] = {};
    void beginHistoryGesture(ofxOceanodeTimelineManager& timeline);
    void finishHistoryGesture(ofxOceanodeTimelineManager& timeline);
    void restoreHistory(ofxOceanodeTimelineManager& timeline, bool redo);
    int lastTimelineDrawFrame = -1;
    void drawRuler(ofxOceanodeTimelineManager& timeline, float labelWidth, float timelineWidth,
                   double endBeat, double beatPosition, float bpm);
    void drawLaneEditor(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
                        float contentWidth, double endBeat, double beatPosition);
    void drawClipLaneEditor(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
                            ofxOceanodeTimelineClip& clip, float contentWidth, double endBeat, double beatPosition);
    void expandClipLanes(const ofxOceanodeTimelineTrack& track, const std::string& clipId);
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
    // What the New Clip dialogs create on a track of the shown timeline,
    // driving one binding: a clip with one lane of the chosen type (an LFO
    // clip for the LFO entry), a clip of the binding's note group, or --
    // for a Piano Roll / Drum Grid not in a group yet -- nothing yet: the
    // note setup dialog opens to pick its parameters.
    struct NewClip { enum Kind { Clip, NoteGroupClip, NoteSetup } kind = Clip; std::string clipId, laneId; };
    NewClip createNewClip(ofxOceanodeTimelineManager& timeline, const std::string& trackId, const std::string& bindingId,
                          int typeIndex, const std::string& name, double startBeat, double durationBeats);
    // A lane of the chosen type in a clip of the shown timeline, driving the
    // binding (a value lane spans its parameter's range). Returns its id.
    std::string addBoundLane(ofxOceanodeTimelineManager& timeline, const std::string& trackId, const std::string& clipId,
                             const ofxOceanodeTimelineParameterBinding& binding, int typeIndex, const std::string& name);
    // A parameter's right-click "Add to Timeline": a new clip driving it on a
    // track of a timeline, or on a new track, set up in one dialog.
    void openParameterClipDialog(ofxOceanodeTimelineManager& timeline,
                                 const ofxOceanodeTimelineManager::ParameterClipRequest& request);
    void drawParameterClipPopup(ofxOceanodeTimelineManager& timeline);
    void drawParameterLanePopup(ofxOceanodeTimelineManager& timeline, ofxOceanodeAbstractParameter& parameter,
                                const ofxOceanodeTimelineTrack& track);
    void drawParameterTargetPopup(ofxOceanodeTimelineManager& timeline, ofxOceanodeAbstractParameter& parameter,
                                  const ofxOceanodeTimelineTrack& track);
    ofxOceanodeTimelineManager::ParameterClipRequest parameterClip;
    char parameterClipTrackName[128] = {};
    float parameterClipTrackColor[4] = {};
    char parameterClipName[128] = {};
    int parameterClipType = 1; // Curve
    std::string parameterLaneClipId; // New Lane / New Target: the clip it goes in
    std::string parameterTargetLaneId; // New Target: the lane that drives it too
    double parameterClipStart = 0.0;
    double parameterClipDuration = 4.0;
    bool requestParameterClipPopup = false;
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
    ofxOceanodeTimelineLaneEditors laneEditors;
    ofxOceanodeTimelineBpmEditor bpmEditor;
    ofxOceanodeTimelineWaveVolumeEditor waveVolumeEditor;
    std::shared_ptr<ofxOceanodeContainer> container;
    double rulerSnapBeats = 0.25;
    bool snapEnabled = true; // magnet toggle: clip, marker, loop and playhead moves snap to the grid
    // Derived from the selected snap and current zoom. The selected value is
    // kept separately so zooming back in restores the user's finer grid.
    double effectiveRulerSnapBeats = 0.25;
    std::string pendingTrackId;
    std::string pendingClipId;
    std::string pendingLaneId;
    // Dragging a track's bottom edge: that track's own height (a Wave
    // track's waveTrackHeight, any other's trackHeight).
    std::string trackResizeId;
    float trackResizeStartMouseY = 0.0f;
    float trackResizeStartHeight = 0.0f;
    char pendingTrackName[128] = {};
    char pendingClipName[128] = {};
    char pendingClipRenameName[128] = {};
    std::string clipRenameTrackId;
    std::string clipRenameClipId;
    ofxOceanodeTimelineSequence* clipRenameSequence = nullptr; // the clip's sequence
    std::string pendingClipBindingId;
    std::string pendingAddLaneTarget;
    int pendingAddLaneChoice = 1;
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
    // The Right edge tool (0 Repeat, 1 Trim, 2 Stretch, as endEditTool) a
    // drag uses with these keys held: the selected one with neither. Shift
    // holds Trim and Cmd Stretch, except that the key of the selected tool
    // holds Repeat instead, so all three are always in reach.
    int rightEdgeToolWith(bool shift, bool command) const;
    // What dragging a clip's right edge does now (rightEdgeToolWith the keys
    // held); Wave clips stretch.
    ClipDragMode rightEdgeDragMode(bool waveTrack) const;
    // Shows the cursor of an edge drag (trim, stretch or repeat) at the mouse.
    void showEdgeCursor(ClipDragMode mode) const;
    std::string draggingTrackId;
    std::string draggingClipId;
    // Chance knob at a clip's top-right: a vertical drag sets the clip chance.
    std::string chanceDragTrackId;
    std::string chanceDragClipId;
    float chanceDragStartMouseY = 0.0f;
    float chanceDragStartValue = 1.0f;
    // A wave clip's gain knob being dragged (its travel, 0..1, at the start).
    std::string gainDragTrackId;
    std::string gainDragClipId;
    float gainDragStartMouseY = 0.0f;
    float gainDragStartTravel = 0.5f;
    // A wave clip's fade handle being dragged (out: the fade at its end), and
    // the screen x of beat zero in its row.
    std::string fadeDragTrackId;
    std::string fadeDragClipId;
    bool fadeDragOut = false;
    float fadeDragOriginX = 0.0f;
    // The bend of a fade's curve being dragged: sideways moves it along the
    // fade (over its width), up and down sets how sharp it is.
    bool fadeTensionDrag = false;
    ImVec2 fadeTensionDragStart;
    float fadeTensionDragWidth = 1.0f, fadeTensionDragHeight = 1.0f;
    ofxOceanodeTimelineCurveTension fadeTensionDragInitial;
    float labelWidthDragStart = 0.0f; // left-column resize handle in the ruler
    double dragOffsetBeats = 0.0;
    double dragInitialContentDuration = 4.0;
    double dragInitialContentStretch = 1.0;
    // The clip as it was when an edge drag began: a Stretch drag back past
    // its start restores it each frame and reverses it from there.
    ofxOceanodeTimelineClip dragInitialClip;
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
    std::set<std::string> selectedTracks; // editor selection, independent of clip groups
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
        bool repeatContent;
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
    std::pair<std::string, std::string> breakClipLanesRequest;
    std::string duplicateTrackId; // deferred until iteration has finished
    // Copy and paste of tracks and clips, between any timelines (the
    // clipboard is the manager's: every Timeline window shares it). A copy
    // is made at once; a paste waits until the rows are drawn, like the
    // duplicate above.
    struct TrackPasteRequest {
        bool pending = false;
        bool clips = false;    // copied clips (else tracks)
        std::string trackId;   // tracks: after it; clips: onto it ("" new tracks)
        double beat = 0.0;     // clips: where the first one starts
    };
    TrackPasteRequest trackPaste;
    std::string pasteNotice; // what a paste left out, shown in a popup
    bool requestPasteNoticePopup = false;
    // This track, or every selected track when it is one of them.
    void copyTracksAt(ofxOceanodeTimelineManager& timeline, const std::string& trackId);
    // This clip, or every selected clip when it is one of them.
    void copyClipsAt(ofxOceanodeTimelineManager& timeline, const std::string& trackId, const std::string& clipId);
    // "Paste track" / "Paste 3 clips here" (the menus' items), empty when
    // nothing of that kind is copied.
    std::string pasteLabel(const ofxOceanodeTimelineManager& timeline, bool clips, bool here) const;
    // Cmd+C, Cmd+V and the toolbar's Paste: from and to the selection.
    void copySelection(ofxOceanodeTimelineManager& timeline);
    void pasteAtSelection(ofxOceanodeTimelineManager& timeline, double playheadBeat);
    void serviceTrackPaste(ofxOceanodeTimelineManager& timeline);
    void drawPasteNoticePopup();
    std::string saveMacroTrackId; // the macro track whose (library) Macro is saved, after the rows
    // Tracks of unfolded macro timelines that are selected, by sequence and
    // id (ids repeat between timelines). Apart from selectedTracks, which
    // holds tracks of the timeline shown.
    std::set<std::pair<const ofxOceanodeTimelineSequence*, std::string>> innerSelectedTracks;
    // What an unfolded track's menu asks for, applied after the rows are drawn.
    struct InnerTrackRequest {
        enum class Kind { Duplicate, Remove, RemoveBinding } kind = Kind::Duplicate;
        ofxOceanodeTimelineSequence* sequence = nullptr;
        std::string trackId, bindingId;
    };
    std::vector<InnerTrackRequest> innerTrackRequests;
    // Renaming one of those tracks, in its own dialog.
    ofxOceanodeTimelineSequence* innerRenameSequence = nullptr;
    std::string innerRenameTrackId;
    char innerRenameName[128] = {};
    bool requestInnerRename = false;
    // Folders and track order, also applied after the track iteration.
    struct TrackMoveRequest { std::string trackId, parentId; size_t index = 0; };
    std::vector<TrackMoveRequest> trackMoveRequests;
    bool requestNewFolder = false;
    std::string newFolderParentId;
    std::string newFolderAroundTrackId; // put this track in the new folder
    bool requestFolderDeletion = false;
    std::string folderDeletionId;
    bool folderDeletionKeepsTracks = true;
    // Dragging a track header to reorder it or drop it into a folder: the
    // track, where the press started, and the header rows drawn this frame.
    std::string trackHeaderDragId;
    ImVec2 trackHeaderDragStart;
    bool trackHeaderDragging = false;
    struct TrackHeaderRow { std::string trackId; bool isFolder = false; int depth = 0; float minY = 0, maxY = 0; };
    std::vector<TrackHeaderRow> trackHeaderRows;
    bool requestTrackDeletion = false;
    std::string trackDeletionId;
    // ---- Editor scope: which timeline the editor shows and edits ----
    // The timeline shown (shownRef, {canvas, ""} while the canvas shown has
    // none) and the macro canvases from the root down to it, outermost
    // first (the breadcrumb: `scopes`, empty on the main canvas). Showing
    // another clears selections and the clip editor, whose ids belong to a
    // timeline.
    struct EditorScope {
        ofxOceanodeTimelineSequence* sequence = nullptr; // the shown one, on the last entry
        ofxOceanodeContainer* container = nullptr; // the macro's (pickers resolve inside it)
        std::string label;
        ofxOceanodeTimelineManager::TimelineRef ref; // the shown timeline on the last entry; {canvas, ""} above it
    };
    std::vector<EditorScope> scopes;
    ofxOceanodeTimelineManager::TimelineRef shownRef;
    ofxOceanodeTimelineSequence* shownSequence = nullptr; // re-resolved each frame (dropDeadScopes)
    // The timeline edited: the shown one (a scratch, never shown, while the
    // canvas shown has none: the editor draws its empty state then).
    ofxOceanodeTimelineSequence& scope(ofxOceanodeTimelineManager& timeline) {
        if(rowScope != nullptr) return *rowScope->sequence;
        static ofxOceanodeTimelineSequence none;
        return shownSequence != nullptr ? *shownSequence : none;
    }
    const ofxOceanodeTimelineSequence& scope(const ofxOceanodeTimelineManager& timeline) const {
        if(rowScope != nullptr) return *rowScope->sequence;
        static const ofxOceanodeTimelineSequence none;
        return shownSequence != nullptr ? *shownSequence : none;
    }
    // A macro's canvas is shown (its timeline, or its empty state).
    bool inMacroScope() const { return !scopes.empty(); }
    // The canvas shown ("" the main canvas).
    std::string shownCanvas() const { return shownRef.canvas; }
    // A canvas: its first timeline, else its empty state.
    void showCanvas(ofxOceanodeTimelineManager& timeline, const std::string& canvas);
    void enterMacroScope(ofxOceanodeTimelineManager& timeline, ofxOceanodeContainer& macroContainer);
    // Any timeline of any canvas.
    void enterTimelineScope(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref);
    void setShown(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref,
                  ofxOceanodeTimelineSequence* sequence);
    // A canvas to show on the next draw (leaving a breadcrumb step needs the manager).
    std::string pendingCanvasToShow;
    bool pendingCanvasShowRequested = false;
    // The timeline shown ({canvas, ""}: none on that canvas).
    ofxOceanodeTimelineManager::TimelineRef scopeRef() const { return shownRef; }
    // ---- Timelines: the list, the shown one's player, further windows ----
    // The scope bar at the top of the window: which timeline is shown (its
    // name on a badge that opens the navigator, the actions menu) and, for
    // one other than the main, its player (whether it is independent, else
    // its own tempo; an independent one's transport is the toolbar's).
    float beginScopeBar(ofxOceanodeTimelineManager& timeline);
    void endScopeBar(ofxOceanodeTimelineManager& timeline, float top);
    void drawTimelineActions(ofxOceanodeTimelineManager& timeline);
    // The navigator: every timeline under its canvas, the canvases nested as
    // they are, with how each plays, its tempo, meter and sync.
    void drawTimelinesTree(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& shown);
    void drawTimelineRenamePopup(ofxOceanodeTimelineManager& timeline);
    void drawPlayerBar(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref);
    void drawPlayerSettings(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref);
    void drawTempoPointsPopup(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref);
    // The main controller draws the timeline windows (before the core's windows).
    void drawTimelineWindows();
    // The title of a window (its ImGui id after ###, stable across renames).
    std::string timelineWindowTitle(const ofxOceanodeTimelineManager& timeline,
                                    const ofxOceanodeTimelineManager::TimelineWindow& window) const;
    // Brings a window forward once it has drawn (a new one draws unfocused first).
    void focusTimelineWindow(ofxOceanodeTimelineManager& timeline, int id);
    std::string pendingWindowFocus;
    int pendingWindowFocusFrames = 0;
    // The window showing that timeline, brought forward, else a new one on it. Its id.
    int showTimelineInWindow(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineManager::TimelineRef& ref);
    // The editor that handles the requests waiting (drawPendingTrackPopup):
    // the one whose window shows the timeline a request names (opened if
    // none does), else the last one picked, else the first window's; this
    // controller itself while a harness draws it as an editor.
    ofxOceanodeTimelineController& requestTaker(ofxOceanodeTimelineManager& timeline);
    int requestTakerWindow = 0;
    bool drawnAsEditor() const { return lastTimelineDrawFrame >= ImGui::GetFrameCount() - 1; }
    ofxOceanodeContainer* pendingMacroScopeContainer = nullptr;
    // Seeks the timeline shown: its own transport (free, or a dependent
    // one's preview), else the global one it is linked to.
    void seekShownTimeline(ofxOceanodeTimelineManager& timeline, double beat);
    // The loop of the timeline shown, true while it is on: the global
    // transport's for one linked to it, else its own.
    bool shownLoop(const ofxOceanodeTimelineManager& timeline, double& startBeat, double& endBeat) const;
    void setShownLoop(ofxOceanodeTimelineManager& timeline, double startBeat, double endBeat);
    // Whether the timeline shown plays on a transport of its own (its loop
    // and tempo are its own) rather than the global one.
    bool shownOwnsTransport(const ofxOceanodeTimelineManager& timeline) const;
    // The BPM lane of the timeline shown: the global one for a timeline
    // linked to the global transport, its own for a free one, none for a
    // dependent one (it follows its host's tempo).
    const std::vector<ofxOceanodeTimelineCurvePoint>* shownBpmLane(const ofxOceanodeTimelineManager& timeline) const;
    ofxOceanodeTimelineTempoLane shownBpmLaneTarget(ofxOceanodeTimelineManager& timeline);
    // No timeline to show (none made yet on this canvas): how to make one.
    void drawEmptyState(ofxOceanodeTimelineManager& timeline);
    // Shows the timeline a timeline track places, from the next frame.
    void requestPlacedScope(ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track);
    ofxOceanodeTimelineManager::TimelineRef pendingScope;
    bool pendingScopeRequested = false;
    int windowId = 0; // 0: the main controller (no window of its own)
    bool secondary() const { return windowId != 0; }
    // Call before every BeginPopupModal: the modal opens on the main OS window,
    // centred, each time. Left to ImGui it reopens where imgui.ini last had it
    // (often near the top of the screen), and outside the main window
    // multi-viewport docking gives it an OS window of its own, where clicks
    // can land a few pixels off.
    static void centerNextModal();
    bool windowScopeApplied = false;
    ofxOceanodeTimelineManager::TimelineRef appliedWindowRef; // the entry as last applied
    bool windowScopeValid = false; // it could be shown (else its entry is left alone)
    // Scopes whose timeline is gone close (at the top of each draw).
    void dropDeadScopes(ofxOceanodeTimelineManager& timeline);
    std::map<int, std::unique_ptr<ofxOceanodeTimelineController>> windowEditors;
    char timelineNameBuffer[128] = {};
    ofxOceanodeTimelineManager::TimelineRef timelineRenameRef;
    bool requestTimelineRename = false;
    // ---- New Timeline: every new timeline's name, colour and owner ----
    // Asked through the manager (requestNewTimeline) from the canvas and
    // parameter menus and the timelines menu; the main controller shows it.
    void openNewTimelineDialog(ofxOceanodeTimelineManager& timeline,
                               const ofxOceanodeTimelineManager::NewTimelineRequest& request);
    void drawNewTimelinePopup(ofxOceanodeTimelineManager& timeline);
    // The name and colour it gets on the owner picked (while not edited).
    void suggestNewTimelineDefaults(ofxOceanodeTimelineManager& timeline);
    ofxOceanodeTimelineManager::NewTimelineRequest newTimelineRequest;
    std::vector<std::string> newTimelineOwners, newTimelineOwnerLabels;
    int newTimelineOwner = 0;
    char newTimelineName[128] = {};
    bool newTimelineNameEdited = false;
    float newTimelineColor[3] = {};
    bool newTimelineColorEdited = false;
    bool requestNewTimelinePopup = false;
    // ---- Nodes pasted from another preset: paste their timelines too? ----
    void drawClipboardPastePopup(ofxOceanodeTimelineManager& timeline);
    ofxOceanodeTimelineManager::ClipboardPasteRequest clipboardPaste;
    bool requestClipboardPastePopup = false;
    // Back to the canvas at `depth` of the breadcrumb (0: the main canvas).
    void leaveScopesTo(size_t depth);
    void resetEditorForScope();
    // ---- Unfolded macro tracks: a macro's timeline edited in place ----
    // A macro track's timeline placed on the timeline shown: its sequence,
    // where its beats sit there (one repeat of one macro clip) and which
    // placement that is (macro track, clip, repeat and segment ids, outermost
    // first). While rowScope is set, scope() and every beat/pixel conversion
    // (editorView) follow it: the unfolded rows and the clip editor of one of
    // their clips use the arrangement's and editors' own code.
    struct RowScope {
        ofxOceanodeTimelineSequence* sequence = nullptr;
        ofxOceanodeContainer* container = nullptr; // the macro's
        ofxOceanodeTimelineTimeMap map;
        std::string key;
    };
    const RowScope* rowScope = nullptr;
    struct RowScopeGuard {
        ofxOceanodeTimelineController& controller;
        const RowScope* previous;
        RowScopeGuard(ofxOceanodeTimelineController& controller, const RowScope* scope)
        : controller(controller), previous(controller.rowScope) { controller.rowScope = scope; }
        ~RowScopeGuard() { controller.rowScope = previous; }
    };
    // The clip editor shows a clip of an unfolded macro timeline (sequence
    // null: a clip of the scope's own). The ids say which clip, since ids
    // repeat between sequences.
    RowScope editorRowScope;
    // The unfolded macro timeline whose clip's chance knob is dragged
    // (sequence null: a clip of the scope's own).
    RowScope chanceDragScope;
    std::string editorRowTrackId, editorRowClipId;
    std::string editorRowGroup; // the unfolded rows it docks in (macro track ids)
    // The frame openInnerClipEditor ran in: the editor docks under its row
    // from the next frame on (it is not closed for not being docked yet).
    int editorRowOpenedFrame = -1;
    // A track's row of an unfolded timeline, resized from its bottom edge.
    struct InnerResize {
        ofxOceanodeTimelineSequence* sequence = nullptr;
        std::string trackId;
        float startY = 0.0f, startHeight = 0.0f;
    } innerResize;
    bool innerEditorOpen() const {
        return editorState.clipEditorOpen && editorRowScope.sequence != nullptr &&
               editorState.editorTrackId == editorRowTrackId && editorState.editorClipId == editorRowClipId;
    }
    // The open clip editor is on this track of the scope's own timeline.
    bool scopeEditorOnTrack(const std::string& trackId) const {
        return editorState.openTrackIds.count(trackId) > 0 ||
            (editorState.clipEditorOpen && !innerEditorOpen() && editorState.editorTrackId == trackId);
    }
    void closeTrackEditor(const std::string& trackId) {
        editorState.openTrackIds.erase(trackId);
        if(!innerEditorOpen() && editorState.editorTrackId == trackId) editorState.clipEditorOpen = false;
    }
    std::vector<std::pair<float, float>> editorScreenSpans;
    // The sequence whose ids editorState holds: the unfolded timeline the
    // editor's focus is in, else the timeline shown (never the row being
    // drawn: an open row without the focus has its lanes drawn too).
    ofxOceanodeTimelineSequence& editorSequence(ofxOceanodeTimelineManager& timeline) {
        if(innerEditorOpen() && timeline.isLiveSequence(*editorRowScope.sequence)) return *editorRowScope.sequence;
        static ofxOceanodeTimelineSequence none;
        return shownSequence != nullptr ? *shownSequence : none;
    }
    // The rows of unfolded macro timelines that show their lanes: any number
    // of them, in any timeline, each staying open until it is folded again.
    // The one holding the editor's focus is also editorRowScope. at: where
    // the row's lanes are drawn (the placement it was opened in); group: as
    // editorRowGroup.
    struct InnerOpenRow {
        RowScope at;
        std::string group, trackId;
    };
    std::vector<InnerOpenRow> innerOpenRows;
    // Lane ids repeat between timelines, and the lanes of several timelines
    // show at once under an unfolded macro track. What editorState keeps per
    // lane (folded lanes, expanded LFO controls, heights) is therefore kept
    // per unfolded timeline: an InnerLaneState swaps that timeline's in while
    // its lanes are drawn or edited (the timeline shown keeps editorState's
    // own), and tags the lane being resized (laneKeyTag).
    struct LaneViewState {
        std::set<std::string> collapsedLaneIds, expandedLfoControlKeys;
        std::unordered_map<std::string, float> laneEditorHeights;
    };
    std::map<const ofxOceanodeTimelineSequence*, LaneViewState> innerLaneStates;
    const ofxOceanodeTimelineSequence* activeLaneState = nullptr; // whose is swapped in (null: the shown timeline's)
    void swapLaneState(const ofxOceanodeTimelineSequence* sequence) {
        if(sequence == nullptr) return;
        auto& kept = innerLaneStates[sequence];
        std::swap(kept.collapsedLaneIds, editorState.collapsedLaneIds);
        std::swap(kept.expandedLfoControlKeys, editorState.expandedLfoControlKeys);
        std::swap(kept.laneEditorHeights, editorState.laneEditorHeights);
    }
    void activateLaneState(const ofxOceanodeTimelineSequence* sequence) {
        if(sequence == activeLaneState) return;
        swapLaneState(activeLaneState);   // back to the shown timeline's
        swapLaneState(sequence);
        activeLaneState = sequence;
        editorState.laneKeyTag = sequence == nullptr ? std::string()
            : "@" + std::to_string(reinterpret_cast<uintptr_t>(sequence)) + "/";
    }
    // sequence null: the timeline shown.
    struct InnerLaneState {
        ofxOceanodeTimelineController& controller;
        const ofxOceanodeTimelineSequence* previous;
        InnerLaneState(ofxOceanodeTimelineController& controller, const ofxOceanodeTimelineSequence* sequence)
        : controller(controller), previous(controller.activeLaneState) { controller.activateLaneState(sequence); }
        ~InnerLaneState() { controller.activateLaneState(previous); }
    };
    void openInnerClipEditor(const RowScope& at, const std::string& trackId, const std::string& clipId,
                             const std::string& laneId);
    // An arrangement drag of a clip in an unfolded row (applied after the
    // track iteration, through its placement).
    struct InnerClipDrag {
        RowScope at;
        std::string trackId, clipId;
        ClipDragMode mode = ClipDragMode::None;
        ofxOceanodeTimelineClip initial;
        double grabOffsetBeats = 0.0; // press beat minus the clip start, in its own beats
        float originX = 0.0f;         // screen x of timeline beat 0
        float pressX = 0.0f;
        float rowTop = 0.0f, rowBottom = 0.0f;
        bool committed = false;
        bool extend = false;          // Shift on the left edge
    };
    InnerClipDrag innerDrag;
    // Right-clicked clip of an unfolded row (its menu is drawn with the row).
    RowScope innerMenuAt;
    // The New Timeline Clip dialog asked from a row of an unfolded macro track:
    // the placement it was asked in (sequence null: a clip of the scope's own),
    // and the rows a clip editor it opens docks in. pendingStartBeat is then in
    // that timeline's beats.
    RowScope pendingClipScope;
    std::string pendingClipGroup;
    std::string innerMenuTrackId, innerMenuClipId, innerMenuGroup;
    enum class InnerMenuRequest { None, Duplicate, Delete };
    InnerMenuRequest innerMenuRequest = InnerMenuRequest::None; // applied after the track iteration
    // Unfolded macro timelines drawn last frame: edits to them are recorded
    // in their own history, and undo steps whichever was edited last.
    std::vector<ofxOceanodeTimelineSequence*> unfoldedSequences, unfoldedSequencesDrawn;
    // One pass of each macro track on screen (of the timeline shown and of
    // those unfolded in it), as last seen: when it changes, the track's
    // clips that lasted that pass follow (see the arrangement's draw).
    std::map<std::pair<const ofxOceanodeTimelineSequence*, std::string>, double> macroPassSeen;
    std::vector<ofxOceanodeTimelineSequence*> historySequences(ofxOceanodeTimelineManager& timeline) const;
    std::vector<ofxOceanodeTimelineSequence*> undoOrder, redoOrder;
    // Clips of one track to merge into one (applied after the track iteration).
    std::string mergeTrackId;
    std::vector<std::string> mergeClipIds;
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

    // ---- Marker Track (ofxOceanodeTimelineTrack::isMarkerTrack) ----
    // Its markers are edited in the timeline shown (scope()), found by uid:
    // their numbers change as they move.
    std::string trackMarkerTrackId;    // the track of the marker pressed or edited
    int trackMarkerDragUid = -1;       // pressed marker (a drag moves it)
    float trackMarkerDragStartX = 0.0f;
    double trackMarkerDragStartBeat = 0.0;
    bool trackMarkerDragMoved = false;
    int trackMarkerEditUid = -1;       // the marker the popup edits
    bool requestTrackMarkerPopup = false;
    ImVec2 trackMarkerPopupAnchor;
    char trackMarkerTitle[128] = "";
    char trackMarkerDescription[2048] = "";
    double trackMarkerBeatEdit = 0.0;  // the popup's Beat field while it is typed in
    // A marker asked for from the track's + or menu, made at the deferred
    // edit point and then opened in the popup.
    bool requestNewTrackMarker = false;
    std::string newTrackMarkerTrackId;
    double newTrackMarkerBeat = 0.0;
    void openTrackMarkerPopup(const ofxOceanodeTimelineTrack& track, int uid, const ImVec2& anchor);
    void drawTrackMarkerPopup(ofxOceanodeTimelineManager& timeline);

    // ---- View ----
    // Session-only alternate projections of the same clips and routing.
    bool targetsView = false;
    // Repeat, Trim, Stretch; modifiers override it. Not session-only: a
    // window's editor takes it from its window's entry before drawing and
    // gives it back after (TimelineWindow::rightEdgeTool, saved with it).
    int endEditTool = 0;
    bool targetInspectorOpen = false;
    std::string inspectedTargetPath;
    void drawTargetInspector(ofxOceanodeTimelineManager& timeline);
    void drawClipSettingsWindow(ofxOceanodeTimelineManager& timeline, double endBeat);
    bool clipSettingsOpen = false;
    bool clipSettingsFocus = false;
    std::string clipSettingsTrackId, clipSettingsClipId;
    ofxOceanodeTimelineManager::TimelineRef clipSettingsRef;
    // The unfolded macro timeline whose clip the settings window shows
    // (sequence null: a clip of the timeline shown).
    RowScope clipSettingsScope;
    ImVec2 clipSettingsAnchor;
    // ---- Macro Morphing Track (ofxOceanodeTimelineControllerMorph.cpp) ----
    // The windows of the shown timeline's Macro Morphing Track: the routers
    // of its engine, one source (a clip's) and one transition (into a clip,
    // from the clip it overlaps). Their panels are the MacroMixer node's
    // (MacroMixerGui): they queue their edits, which are applied to the
    // track's engine once a frame, after the windows.
    struct MorphPanels;
    // (shared_ptr: the type is complete in that file only.)
    std::shared_ptr<MorphPanels> morphPanels;
    MorphPanels& morphPanelState();
    ofxOceanodeTimelineManager::TimelineRef morphWindowRef;
    bool morphRoutersOpen = false, morphRoutersFocus = false;
    bool morphSourceOpen = false, morphSourceFocus = false;
    bool morphTransitionOpen = false, morphTransitionFocus = false;
    std::string morphSourceClipId;       // the clip whose source the source window shows
    std::string morphTransitionClipId;   // the later clip of the overlap the transition window edits
    // The timeline a morph gesture is on: the shown one, or the one of the
    // unfolded row being drawn or acted on.
    ofxOceanodeTimelineManager::TimelineRef morphRefHere(const ofxOceanodeTimelineManager& timeline) const;
    void showMorphWindowsHere(const ofxOceanodeTimelineManager& timeline);
    void openMorphRouters(const ofxOceanodeTimelineManager& timeline);
    void openMorphSource(const ofxOceanodeTimelineManager& timeline, const std::string& clipId);
    void openMorphTransition(const ofxOceanodeTimelineManager& timeline, const std::string& clipId);
    void drawMorphWindows(ofxOceanodeTimelineManager& timeline);
    // "Add Morphing Source" opens a folder dialog: asked for in the track
    // menu, served after the track loop (see waveFileRequest below).
    bool morphFolderRequest = false;
    ofxOceanodeTimelineManager::TimelineRef morphFolderRef;   // the timeline whose track asked
    void serviceMorphRequests(ofxOceanodeTimelineManager& timeline);
    std::string morphErrorMessage;
    bool requestMorphErrorPopup = false;
    void reportMorphError(const std::string& message);
    // The overlaps of a Macro Morphing Track's clips, drawn over them: laneMin
    // is where beat 0 of the row is, max the row's bottom right.
    void drawMorphOverlaps(const ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
                           ImDrawList* dl, const ImVec2& laneMin, const ImVec2& max,
                           const std::function<float(double)>& beatOffset);
    // The overlap of the track at a beat, or the one into a clip (false: none).
    static bool morphOverlapAt(const ofxOceanodeTimelineTrack& track, double beat, ofxOceanodeTimelineMorphOverlap& found);
    static bool morphOverlapInto(const ofxOceanodeTimelineTrack& track, const std::string& clipId,
                                 ofxOceanodeTimelineMorphOverlap& found);
    bool followPlayhead = false;       // scroll to keep the playhead in view while playing
    float zoomAnchorViewportX = 0.0f;
    double zoomAnchorUntil = 0.0;
    // A two-finger swipe over the clips zooms (vertical) or scrolls
    // (horizontal), never both: it locks to the axis it mostly moves along
    // and keeps it until the wheel goes quiet.
    enum class WheelAxis { None, Vertical, Horizontal };
    WheelAxis wheelAxis = WheelAxis::None;
    ImVec2 wheelAxisTravel = ImVec2(0.0f, 0.0f);
    double lastWheelTime = -1.0;
    bool requestZoomToFit = false;     // fit every clip and marker in the visible width
    bool requestZoomToLoop = false;    // fit the loop region in the visible width (toolbar, while the loop is on)
    uint64_t fittedLoadStamp = 0;      // the latest loaded timeline the view was fitted to (its loadStamp)
    // The time scrollbar: its middle scrolls, a handle at either end of its
    // thumb zooms, the view's other edge staying at the same time.
    enum class ScrollbarDrag { None, Body, Start, End };
    ScrollbarDrag scrollbarDrag = ScrollbarDrag::None;
    double scrollbarDragSeconds = 0.0;   // the scrollbar's length in seconds when the drag began
    double scrollbarFixedSeconds = 0.0;  // the view edge that stays put
    struct { float x1 = 0, y1 = 0, x2 = 0, y2 = 0; } scrollbarThumb; // drawn this frame
    std::vector<double> tapTimes;      // tap tempo (seconds)
    // The BPM fields are text fields (a number typed applies on Enter): the
    // text of each while it is edited, refreshed from the tempo otherwise.
    std::map<unsigned int, std::array<char, 24>> bpmFieldText;

    // ---- Clip box selection ----
    struct ClipScreenRect { std::string trackId, clipId; float x1, y1, x2, y2; };
    std::vector<ClipScreenRect> clipScreenRects; // clips drawn this frame
    bool marqueePending = false;       // pressed on empty track space
    bool marqueeActive = false;        // dragged far enough: selecting
    float marqueeStartX = 0.0f, marqueeStartY = 0.0f;
    bool marqueeAdditive = false;
    double clipMenuHitBeat = 0.0;      // unsnapped position for overlapping clip selection
    double clipMenuBeat = 0.0;         // beat under the right-click that opened a clip menu
    double clipSplitBeat = 0.0;
    bool requestClipSplit = false;
    // Global Track keyframes: asked for while the tracks are drawn, served
    // at the deferred edit point (adding one can make or grow a clip).
    bool requestGlobalKeyframe = false;
    // The unfolded macro timeline the keyframe asked for, and the one edited,
    // are in (sequence null: the scope's own): track ids repeat between
    // timelines, and beats are that timeline's own.
    RowScope globalKeyScope, globalKeyEditScope;
    std::string globalKeyTrackId;
    std::string globalKeyClipId;      // empty: the clip at the beat, or the nearest
    double globalKeyBeat = 0.0;
    // The keyframe a clip menu was opened on (-1: none), one to remove, and
    // the one being dragged along its clip. (A clip of an unfolded row's menu
    // uses clipMenuGlobalKey and clipMenuBeat too, in its timeline's beats.)
    int clipMenuGlobalKey = -1;
    std::string globalKeyEditTrackId;
    std::string globalKeyEditClipId;
    int globalKeyRemoveIndex = -1;
    int globalKeyDragIndex = -1;
    float globalKeyDragOriginX = 0.0f;
    // The easing of the transition after keyframe globalEaseDragIndex,
    // dragged by its handle (sideways: where it bends; up/down: how sharply),
    // or reset to linear (globalEaseResetIndex).
    int globalEaseDragIndex = -1;
    int globalEaseResetIndex = -1;
    ImVec2 globalEaseDragStart;
    float globalEaseDragWidth = 1.0f;
    ofxOceanodeTimelineCurveTension globalEaseDragInitial;
    std::string clipSplitTrackId;
    std::string clipSplitClipId;
    std::string waveSplitTrackId;
    std::string waveSplitClipId;
    // Picking an audio file opens a blocking OS dialog. Asking for one from
    // inside an ImGui popup or child means the dialog runs with that popup
    // still on the stack and the frame half-built, so the request is only
    // recorded here and serviced once the track loop and its popups are done
    // (see the handler next to the wave-split one in draw()).
    enum class ClipFileRequest { None, Save, Load };
    ClipFileRequest clipFileRequest = ClipFileRequest::None;
    std::string clipFileTrackId, clipFileClipId;
    double clipImportBeat = 0.0;
    ofJson clipImportDocument;
    std::map<std::string, std::string> clipImportTargetPaths;
    std::string clipFileErrorMessage;
    bool requestClipImportPopup = false;
    bool requestClipFileErrorPopup = false;
    char clipTargetSearch[128] = {};
    void serviceClipFileRequest(ofxOceanodeTimelineManager& timeline);
    void drawClipImportPopup(ofxOceanodeTimelineManager& timeline);
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
    // combined into one clip); these back the clip editor's add, duplicate
    // and remove lane controls. The actual mutation is deferred to a
    // fixed point in draw(), like clip deletion above, since createLane/
    // removeLane can reallocate the clip's lane vector and the editor keeps
    // pointers into it for the rest of the frame it was requested on.
    bool requestAddLane = false;
    bool requestAddLanePopup = false;   // the track header's + asks the clip editor for its add-lane popup
    ImVec2 addLanePopupAnchor;          // where it sits: 20px right of the +, centred on it
    // That popup's New Clip tab: a clip on the track, made at the same fixed
    // point as a new lane (making it can move the clip the editor holds).
    bool requestTrackNewClip = false;
    std::string trackNewClipTrackId;
    int pendingAddLaneType = 0;
    bool requestDuplicateLane = false;
    std::string pendingDuplicateLaneId;
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
    std::optional<ofColor> noteSetupTrackColor; // new track color from Add to Timeline
    bool noteSetupTrackFixed = false;         // opened from a track row
    std::string noteSetupEditGroupId;         // retargeting this group
    double noteSetupClipStart = 0.0;
    double noteSetupClipDuration = 4.0;
    // Instruments in the patch (ofxOceanodeNodeModel::getTimelineInstrument):
    // picking one fills the three roles and its voice count.
    struct NoteInstrumentCandidate {
        std::string label;       // the node's name
        std::string rolePaths[3];
        std::string durationPath;  // a kit's Duration input (Drum Grid lanes write it)
        std::string filterPath;    // a kit's Filter input (Drum Grid lanes write it)
        std::string stutterPath;   // a kit's Stutter input (Drum Grid lanes write it)
        int voices = 0;
        bool kit = false;
    };
    std::vector<NoteInstrumentCandidate> noteSetupInstruments;
    int noteSetupInstrument = -1;             // index into noteSetupInstruments; -1: parameters by hand
    int noteSetupVoices = 0;                  // the group's override; 0: automatic
    // The new clip's editor: -1 automatic (Drum Grid for a kit, else Piano
    // Roll), 0 Drum Grid, 1 Piano Roll.
    int noteSetupEditor = -1;
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
