# Timeline architecture

The timeline separates project data and playback, arrangement interaction, and
content editing. An automation clip can contain several lanes; lane types are
where Curve, Step Sequencer, Piano Roll and the other automation editors live.
An LFO is a self-contained clip with its own control lanes.

## Ownership

| Component | Owns |
| --- | --- |
| `Timeline/ofxOceanodeTimeline.*` | Serializable tracks, clips and lanes; editing operations; the per-frame engine (update, apply, overrides), tempo, markers and loop |
| `Timeline/ofxOceanodeTimelineSequence.cpp` | `ofxOceanodeTimelineSequence`: one timeline's tracks, clip groups and id counters, with lookups and new ids. Every canvas keeps its timelines in an `ofxOceanodeTimelineCanvas` (the root's `rootTimelines`, a macro's `macroSequences` entry); each editing operation takes the Sequence to edit (an id-only overload acts on an engine's copy, or on the edit target set with `setEditTarget`) |
| `Timeline/ofxOceanodeTimelineEvaluation.cpp` | Value at a beat (`collectActiveValues`), change beats, piano voicing and mutation, evaluation indexes, play context rolls. Both walks (and the Wave pass) run in an engine and visit `playedSequences()`, each sequence in its own beats through its `ofxOceanodeTimelineTimeMap`: the played timeline through the identity map, then each macro clip repeat's placement of a macro timeline (see Macro timelines below) |
| `Timeline/ofxOceanodeTimelineScheduler.cpp` | Sample-accurate scheduling ahead of the playhead, corrections and latency offsets |
| `Timeline/ofxOceanodeTimelinePlayers.cpp` | Timelines by reference (`TimelineRef`: canvas id and timeline id), every canvas's timelines (make, remove, duplicate, look up), timeline tracks (`createTimelineTrack`, `placedTimeline`, cycle checks), `collectMacroSources`, each timeline's clock source, player and engine, time-source registration, timeline windows (see Several timelines below) |
| `Timeline/ofxOceanodeTimelinePersistence.cpp` | `timeline.json`, `sequence.json`, clip files and presets, and the upgrade of documents written before version 15 |
| `Timeline/ofxOceanodeTimelineHistory.cpp` | Undo: one history per timeline, and the global transport's settings apart |
| `Timeline/ofxOceanodeTimelineInternal.h` | Helpers shared by those files (`ofxOceanodeTimelineDetail`); not part of the API |
| `Controls/ofxOceanodeTimelineController.*` | Window layout, arrangement gestures, clip editor shell, dialogs and deferred structural edits |
| `Editors/ofxOceanodeTimelineClipEditorState.h` | Shell focus, open clip IDs, folding and row layout |
| `Editors/ofxOceanodeTimelineLaneEditors.*` | Concrete lane editor instances and lane-type dispatch |
| `Editors/ofxOceanodeTimelineLaneEditor.h` | Properties/canvas/preview interface and frame-local contexts |
| `Editors/ofxOceanodeTimelineLfoEditor.*` | LFO properties, result view, control curves, point selection and clip preview |
| `Editors/ofxOceanodeTimelineBpmEditor.*` | Tempo editor and its drag/popup state |
| `Editors/ofxOceanodeTimelineWaveVolumeEditor.*` | Track volume editor and its drag/popup state |
| `Editors/ofxOceanodeTimelineEditorView.*` | Tempo-aware beat/pixel conversion, snapping and grid policy |
| `Editors/ofxOceanodeTimelineEditorUi.h`, `Editors/ofxOceanodeTimelineLaneEditorUi.*` | Shared layout, grid controls, styling and point-list editing helpers |

All `Editors/` paths above are under `src/Controls/`.

## Per-type editors

Each automation lane type has a concrete editor class in its own header:

| Lane type | Editor | Implementation |
| --- | --- | --- |
| Step Sequencer | `ofxOceanodeTimelineStepEditor` | Step properties, probability painting and repeated-pattern preview |
| Curve | `ofxOceanodeTimelineCurveEditor` | Interpolation, scale/value snapping, point/tension editing and preview |
| Piano Roll | `ofxOceanodeTimelinePianoRollEditor` | Role controls, note selection/dragging, keyboard audition, velocity/probability strips and preview |
| Number | `ofxOceanodeTimelineNumberEditor` | Shared `HeldValueEditor` mechanics for held numeric entries |
| Dropdown | `ofxOceanodeTimelineDropdownEditor` | Shared `HeldValueEditor` mechanics with bound option names/indices |
| Multi Slider | `ofxOceanodeTimelineMultiSliderEditor` | Dense value grid controls, painting and preview |
| Multi Value | `ofxOceanodeTimelineMultiValueEditor` | Shared `RegionEditor` mechanics with numeric region values |
| Multi Gate / Toggle | `ofxOceanodeTimelineMultiGateEditor` | Shared `RegionEditor` mechanics for on/off regions |
| Drum Grid | `ofxOceanodeTimelineDrumGridEditor` | Kit rows (headers with M/S/length left of the zone, where the piano keyboard sits), step toggling/painting, per-row velocity/pitch/probability/ratchet/nudge/duration strip with full-word tabs, Row properties tab (`extraTabLabel`/`drawExtraTab`), row and step menus, dot-grid preview |

The Number/Dropdown and Multi Value/Multi Gate pairs reuse algorithms through
small shared base classes, with a separate state instance per concrete type.
Toggle remains a single-row Multi Gate, matching the model.

The shell calls `drawTargets`, `drawProperties`, `drawCanvas` and `drawPreview` through
`ofxOceanodeTimelineLaneEditor`. Each editor owns its transient interaction
state. Piano note selection and keyboard audition live in the Piano Roll
editor; curve and held-value point indices live in their respective editors;
region drag state lives in the region editors. The controller no longer draws
these editors or their miniature clip contents through a per-type branch.

The shell owns the clip header, lane folding/resizing, lane focus, common clip
properties and the playhead overlay. It keeps the existing rule that only the
focused lane handles editing gestures. Editors receive a narrow context for
one call, never the controller. They borrow model objects without retaining
pointers into vectors between frames. Persistent content remains in the model,
using the existing JSON format and playback/evaluation code.

An editor can report a request to open its note-group dialog; the shell handles
that request. LFO returns a rename request. Add/remove lane and clip operations
stay at the controller's deferred mutation point so drawing cannot invalidate
track, clip or lane vectors mid-iteration. The controller forwards selection
cleanup and track removal to the editors without accessing their private state.

## Arrangement and scope presentation

Track folders are tracks with `isFolder` (no bindings or clips); a track's
`parentId` names the folder it is in, and the sequence keeps each folder
followed by its contents (`normalizeFolders`). Playback reads mute, solo and
latency through the folders (`isTrackMutedWithFolders`,
`isTrackSoloedWithFolders`, `trackLatencyWithFolders`). The controller draws a
folder as one row with a summary of its contents, hides what is inside a
folded folder, and applies header drags and the Arrange menu through
deferred `moveTrack` requests after the track loop.

Macro timelines: every macro canvas can own any number of timelines
(`ofxOceanodeTimelineMacroSequences.cpp`), none of them special. The
macro's entry (an `ofxOceanodeTimelineCanvas`) is made empty on first use,
its timelines' binding paths are relative to the macro's container, and
they are saved as `sequence.json` in the macro's own folder (its preset
folder, or its library definition, through the core's
`loadMacroPreset`/`saveMacroPreset` hooks). Copies of a library macro share
one list; entries follow the macro's container through a `weak_ptr`, so a
deleted macro's timelines go with it.

A macro track (`macroCanvas`, the macro's canvas relative to its sequence,
and `macroTimeline`, which of its timelines) has macro clips that place that
timeline: start, length, repeat, tempo (stretch) and content offset as on
any clip; the placed timeline depends on the host (see Several timelines
below). To play them, the engine's evaluation index rebuild copies every
macro timeline the played timeline reaches (`playedMacros`, also through
macros inside macros) and makes the copies' paths and canvases absolute, so
bindings, the apply pass, the scheduler and the inspector treat them like
the played timeline's bindings. `playedSequences()` gives each macro clip
repeat a placement: a time map, a window of the played timeline's beats
(split where the content offset wraps) and its own instance key, with the
repeat mixed into the pass so each repeat rolls anew. The value walk
evaluates one placement per macro timeline per beat, and combines the
copies' bindings before the played timeline's, so the host layers on top
with its bindings' modes. Mute, solo and latency are read within each
track's own sequence (`sequenceOf`); the macro tracks' latencies add up
around a copy, and a copy's offset wins on a path the host also drives.
Wave tracks play on any timeline: each engine keys its voices apart
(`waveKeyPrefix`, the timeline's time source id).

Inside played macro timelines, gate, toggle and step bindings write their
rest value wherever no clip of theirs plays (`restWhenInactiveCache`, part
of the value at every beat, so the scheduler sends it too): a macro clip's
end, a muted macro track or a skipped clip leave nothing open. Once per
frame `updateMacroGating` switches each gated macro track's macro
(`macroGate`) through the macro node's Active: on from `macroPreRollMs`
(at least the macro's earliest negative latency) before a clip until
`macroTailMs` after it, with the window wrapped at the loop so a loop start
inside a clip keeps it on. Gating owns Active (a binding to it is refused).
The same pass sets the macro node's Snapshot to a clip's `macroSnapshot`
once when the clip comes up (within the pre-roll), again each time it
starts. Both go through the macro node's own parameters
(`macroNodeParameter`), not the core's internals.

Encapsulation: when the selected nodes are automated, the core's canvas
asks whether their tracks stay on the timeline or move into the new macro.
Either way every timeline's paths follow (`retargetAllParameterPaths`,
relative within macro timelines). Moving (`encapsulateParameters`) takes the
tracks driving only the encapsulated nodes out of every timeline of the
canvas, rewrites their paths relative to the new macro, puts them into the
macro's first timeline (`encapsulationTarget`: made for it, named after the
macro, when it has none), and leaves a macro track with one clip in each
source timeline's place. The core keeps the extension's record (one move
per source timeline) with the canvas undo step; `replayParameterMove`
brings the tracks back on undo (removing a timeline the move made, once
empty) and moves them again on redo (skipping tracks a restored macro
already has). Extraction is recorded the same way, paths only.

The controller edits one timeline at a time, the one shown (`shownRef`,
`shownSequence`; `{canvas, ""}` while the canvas shown has none): any
timeline of any canvas, opened from the macro node menu, the timelines
navigator or a canvas's New Timeline, shown on the scope bar with the macro
breadcrumb back (`scopes`, the macro canvases from the root down). Every
model call from the controller and lane editors passes the shown sequence
(`scope(timeline)`; editors get it in their context). The toolbar drives
the timeline shown: its transport (`transportFor`), loop, tempo and meter
(`shownLoop`, `shownBpmLaneTarget`), as its clock source says (below).
Markers are the global transport's, shown on a timeline following it; undo
steps the shown timeline's own history, or the global settings'. With no
timeline on the shown canvas, the window shows how to make one
(`drawEmptyState`); once the canvas has one, it is shown.

An unfolded macro track (`macroUnfolded`, saved with the track) draws its
macro's tracks as rows under it (`drawUnfoldedRows` in the controller). Each
macro clip repeat and offset segment is a placement (`macroClipPlacements`,
shared with the macro clip's own preview): a time map from the macro
timeline's beats to the beats shown, composed through nested unfolded macro
tracks. Editing a row's clip, or drawing its clip editor, sets the
controller's `rowScope` to that placement: `scope()` returns the macro's
sequence and `editorView()` carries the map (`ofxOceanodeTimelineEditorView`
draws sequence beat b at `offset + b * ratio`), so the model calls and lane
editors are the ordinary ones. Requests the clip editor defers (lanes,
targets, rename) go to the edited clip's sequence. Unfolded sequences are
recorded in their own history, and undo steps the timeline edited last.

Clips and Targets are session-only projections of the same model, selected by
`targetsView` in the controller. Clips draws one arrangement rectangle per clip;
Targets projects clips onto binding rows. Neither projection changes clip
identity, routing, playback precedence or the preset format.

`Editors/ofxOceanodeTimelinePresentation.h` centralizes content summaries,
terminology, clip timing and the shared properties panel. The controller uses
it in automation/audio editors and arrangement menus; LFO uses the same panel.
Lane timing is never edited in lane properties. `drawTargets` on the editor
interface lets Piano Roll own its role-specific routing controls separately
from content settings (Drum Grid uses the same role bindings and draws its own
Gate/Pitch routing). Generic lane targets remain in the shell.

A clip's `contentOffsetBeats` rotates where its content starts playing; all
clip-time conversion (`ofxOceanodeTimelineClipTime`) applies it, so editors
and evaluation see the same rotation. Edits that cut or re-lay content
(split, trim, grow left, consolidate, a new content length) first bake the
offset into the lanes (`bakeContentOffset`), and arrangement previews draw
`clipAsPlayed()`, so nothing draws across the wrap. The Tempo field is the
same setting as Stretch, shown as a speed ratio.

`reverseClip` plays a clip backwards at the same speed, in place. Automation
clips get their content rewritten as its time-mirror (`mirrorClipContent`:
curves with sigmoid bends mirrored exactly and Log / Exp segments laid out
as fitted pieces, held values shifted to the segment they mirror, events
and gates mirrored, step, slider and drum patterns mirrored, or first
unrolled when the content holds a partial pattern) with the content offset
at which the mirror plays exactly what the clip played
(`reversedContentOffset`), so playback, scheduling and editors need nothing
new. A macro clip keeps its macro's shared timeline and sets `reversed`:
evaluation keeps a mirrored copy of each played macro timeline and the
placement reads it. Wave clips toggle Reverse. A Stretch drag of the right
edge back past the start reverses the clip from its drag-start state.

`mergeClips` joins two clips of one track into one plain clip (stretch 1, no
repeat or offset): each clip's lanes are laid out as they played (repeats
unrolled, stretch applied, pattern lanes as one pattern over the whole clip),
lanes on the same parameters with the same kind of lane become one lane, and
the later clip's data wins where the clips overlap. The list form merges any
number of clips by folding the pair merge in start order, after checking every
clip first so a refusal changes nothing.

`resizeClipEnd` is the canonical Repeat / Trim / Stretch operation for numeric
length edits and edge drags. Drag snapshots restore the original source mapping
before each application, including group edits, to avoid accumulating rounding
or stretch changes between frames. Audio always normalizes its full slice.

The evaluation indexes are rebuilt when `evaluationSignature()` changes: a
hash of every played sequence (tracks, bindings, lanes, play settings), taken
once per update pass (`evaluationHashedThisPass`, reset by `beginParameterCache`
and `endParameterCache`); the GUI's playhead lookups read the last pass's
copies without hashing. Within a pass, whether a
sequence has any solo is cached (`anySoloCache`), the engines share the
primary's parameter cache, instrument roles are adopted only when the model
changed or every few frames, and macro gating queries the placements once per
loop range.

`inspectTarget` rebuilds derived evaluation indexes and calls
`collectActiveValues` with an optional contribution collector. It shares lane
evaluation, clip arbitration, voice allocation, blending and final clamping
with playback; the ordinary playback path does not allocate contribution rows.
Inspection does not apply piano parameter ranges, write target values, alter
source data or schedule events. Shared piano lanes are reported as one voice
pool contribution with their contributing source lane IDs.

## Several timelines and their clock sources

A timeline is named by a `TimelineRef`: its canvas (the canvas id the core
reports, "" for the root) and its id within that canvas (`tl_N`; an empty
id names no timeline). Each canvas keeps its timelines in an
`ofxOceanodeTimelineCanvas` (`timelines`, `nextTimelineNumber`, `base`): the
root's `rootTimelines`, a macro's `macroSequences` entry (shared by copies of
a library macro), saved as `"timelines"` in `timeline.json` and in the
macro's `sequence.json`. `canvasTimelines(canvas)` finds a canvas's list
(making a macro's entry on first use), `timelineFor(ref)` / `findTimeline`,
`refOf(sequence)` and `canvasContainer(canvas)` translate (`canvasContainer`
finds a canvas with timelines through the entries before walking the
graph). A timeline's id comes from the canvas's saved `nextTimelineNumber`,
so a removed timeline's id is never given to a new one: tracks, windows and
time sources that still name it stay unbound.

**There is no main or default timeline.** A canvas has any number of
timelines, none special, and a new project or macro has none
(`timelineExists` is a lookup). `addTimeline` makes one (named, or
"Timeline N"); `removeTimeline` removes it with its tracks (clips placing it
elsewhere then play nothing). Documents written before version 15 kept a
canvas's default timeline at their top level: `upgradeTimelineDocument`
makes it `tl_1` (when it was made on purpose, used, free or set up), names it
in the saved windows, players, clip-follow entries and macro tracks that
referred to it by an empty id, and `updateTimeSources` registers
`root#default` / `<canvas>#default` as hidden aliases of a canvas's `tl_1`,
so nodes saved before keep following it.

Timeline tracks generalize macro tracks: `macroCanvas` is the placed
timeline's canvas relative to the host's (`"."` for the host's own canvas) and
`macroTimeline` which of its timelines. `collectMacroSources` keys sources by
canvas and timeline, skips one that an ancestor (or the playing timeline
itself) already is, and `createTimelineTrack` refuses a timeline outside the
host's canvas or one that reaches the host (`timelineReaches`). Played copies
carry `PlayedMacro::timeline`; `playedMacroAt(track)` looks them up. A new
clip placing a timeline (`createMacroClip`) lasts one pass of it: the end of
its last clip, at least a bar of its own meter (`macroTimelineLength`,
`beatsPerBarOf`), and the placed timeline follows the host's tempo beat for
beat through the clip's stretch (there is no per-clip "own tempo").

Each timeline has `ofxOceanodeTimelinePlaySettings` (`play`): name, clock
source (`free`), tempo (fixed, ratio of the clock, or a BPM lane), loop,
meter, launch quantize, play on load, colour, and (a macro's) whether nodes
inside follow its clips (`nodesFollowClips`, one timeline of a canvas at
most: `setNodesFollowClips` clears the others). The whole struct is saved as
the timeline's `"play"` (`playSettingsToJson`); older files kept the free set
(`"players"`) and the clip-following macros (`"clipFollow"`) apart and are
read into the settings (`legacyFreeTimelines`, `legacyClipFollowTimelines`,
applied as each macro's timelines load).

**The global transport is owned by no timeline.** Its tempo, BPM lane,
meter and markers are the manager's own fields (the preset's, saved at
the top level of `timeline.json`). A timeline follows it unless it plays
free or is placed in another. It is never looped: the loop is each
timeline's own (`play.loopEnabled`, `loopStartBeat`, `loopEndBeat`, whatever
its clock source; files before version 16 kept the global one at the top
level, read into the root canvas's linked timelines).

**Clock sources** (`clockSourceOf`). A timeline placed as clips by any
timeline (`placedTimelines`, placed to first host, refreshed each update pass
and after edits through `invalidateEvaluationIndexes`) is **Dependent**,
whatever its setting. Otherwise it is **Free** (`play.free`) or **Global**
(the default). `followsGlobalTransport(sequence)` says which.

**The primary plays nothing itself.** Its `evaluatedSequences()`,
`playedSequences()` and `collectMacroSources()` return nothing; it keeps
the timelines, the global transport's settings, the players and the undo
histories. Every existing timeline has a **player** (`Player`,
`updatePlayers`): a transport and an **engine**, a second
`ofxOceanodeTimelineManager` with `primary` set that evaluates, schedules
and applies an absolute-path copy of the timeline (`playedSequence`,
`makeAbsolute`, rebuilt when `sequenceSignature` changes) on that
transport. Global: the transport is the root transport itself
(`engineSharesRoot`): the engine reads the global tempo, tempo lane and
meter (adopted from the manager each frame, so lookahead follows the
global map) and never sets them; the loop is the timeline's own, and the
root transport is never given one: the engine wraps the root's position
into its loop itself (`loopedState`, through `playState`: the pass as
`loopCount`, so wraps, lookahead past the loop end and the playhead shown
work as on a looping transport while the global transport runs on). Free: a transport derived from the root's
clock (`ofxOceanodeTransport::makeDerived`) with the timeline's own tempo,
loop and meter (`adoptPlaySettings`). Dependent: a derived transport too, at
its host's tempo now (`hostBpmFor`, through the host's transport) with its
own loop and meter; it is the timeline's **preview**, run only while it
plays or has a pending action (and once more after it stops), so an idle
dependent timeline costs nothing per frame (its transport still takes its
host's tempo). A preview is heard over the timelines placing it: the
primary's `applyAutomation` applies the previewing engines' values last.
Engines read their primary's macro entries (`sequenceEntries`) and run
inner canvases first, so the outer value lands last; on a path several
timelines drive, the engine whose value lands last schedules it and the
others leave it to the frame path (`lastDriverForPath`,
`pathDrivenElsewhere`). A macro canvas's timeline playing free sets its
container's transport override, so nodes inside follow it; it lets go when
it stops being free. Engines share the primary's chance-roll salt, so what
the arrangement shows as rolled out is what plays.

Runtime state keyed by track and binding ids lives in the engine: live
overrides and piano key previews (`setLiveOverride(sequence, ...)`,
`setPianoKeyPreview(sequence, ...)` reach the engine playing the timeline
through `syncedEnginePlaying`; the primary keeps the explicit overrides in
`liveOverrides` and `reapplyTransients` puts them back after a copy
rebuild), state capture (`dropStateCaptureOverride`, `noteStateCaptured`),
driven solos and Wave voices (`releaseWaveClipIfNeeded`, prefixed).

**The primary answers through its engines, on demand.** `updatePlayers`
is two steps the primary can also take between frames: `syncPlayerList`
(the players as they should be, made and dropped without running any) and
`syncPlayer` (an engine's copy rebuilt when its source's signature
changed, its play settings adopted). `syncedEngine(ref)` /
`syncedEnginePlaying(sequence)` give an engine current with its timeline,
making its player if it has none yet. So a playback question asked of the
primary is the engine's answer: `inspectTarget` (the edit target's engine,
else the engine driving the path; the sequence overload absolutizes the
path for the timeline's engine), `isTrackSoloed` (the engine's driven
solos), `isGatedMacroActive`; and a live change made through the primary
reaches the engine at once: `refreshPlaybackEnablement` syncs every copy
and refreshes every engine (a muted lane's gates close now), a lane set to
drive its track's solo applies on the engine, the id-only live override
and key preview overloads go to the edit target's engine, `setTrackSolo`
drops the engine's driven solo, and `invalidateSchedule` on the primary
reaches every engine's schedule (the copy rebuild itself leaves the
schedule alone: the scheduler repairs a direct content change as before).
The primary's `applyAutomation` applies every running engine again (the
host calls it after the nodes ran). The content signature covers what the
copy plays (mutes, hand solos, macro gate and snapshot, binding clamp,
live overrides set on the source...); a driven solo is not content.

`transportFor(ref)` is the root transport for a Global timeline (before its
player exists too), a player's own otherwise (nullptr until its player runs). `playTimeline`, `stopTimeline`
and `seekTimeline` act on it: a transport that is not derived (the global
one) at once, a derived one timestamped (`playAt`, `stopAt`): quantized to
the root's grid while it plays, else the clock's, and at least half the
lookahead ahead. While a start is pending within the window, the engine
schedules from that instant (`schedulingTransport` in `evaluateAutomation`),
so its first events are exact; a pending stop caps the window
(`scheduleHorizonUs`). An engine's Wave voices go to the same audio provider,
keyed apart by `waveKeyPrefix`; clips its copy loses are released
(`releaseWaveClips`).

**Playheads.** `timelinePlayheadBeat` gives a dependent timeline the position
the first playing timeline placing it maps it to (`placedPlayheadBeat`,
searched through the manager and every engine, `forEachPlayingManager`), as a
precomposition's playhead follows the composition playing it; otherwise its
transport's beat. Playing a dependent timeline previews it alone and never
plays its host.

Macros one of whose timelines has `nodesFollowClips` (`clipFollowTimeline`)
get a driven transport (`ofxOceanodeTransport::setDrivenState`) as their
canvas override: `updateClipTransports`, at the end of the primary's
`evaluateAutomation`, sets it to the first placement of that timeline's copy
at the playhead of a playing engine (beat, host tempo over the map's ratio,
restart when the placement changes).

`updateTimeSources` registers every timeline with the root transport as a
time source (`timeSourceId(ref)`: `canvas#timeline`, `root#tl_1` for the
root canvas's first): the root transport itself for a Global one, the
player's transport otherwise; a canvas's `tl_1` is registered a second time
as `canvas#default`, hidden, for nodes saved before version 15.

The **undo histories** are the primary's: one per timeline
(`sequenceHistories`, typed snapshots of its tracks, groups, settings and
counters: `captureSequence`, `restoreSequence`; the clock source is not
undone) and the global settings' (`TimelineState`: markers, tempo lane,
loop, meter, timebase, scheduling; `beginGlobalEdit`...). The controller
records which was edited last (`undoOrder`, `nullptr` for the global
settings). The id-only `beginTimelineEdit()` family covers the edit target
and the global settings in one gesture.

The **metronome** (`setMetronomeEnabled(on, ref)`) follows one timeline's
transport, whatever its clock source, in that timeline's meter
(`metronomeRef`); it moves with the timeline's transport when its clock
source changes and goes with the timeline.

The controller shows any timeline (`enterTimelineScope`, `showCanvas`;
`shownRef`). `drawScopeBar` (`ofxOceanodeTimelineControllerTimelines.cpp`)
draws the scope bar at the top of the window: a badge with the shown
timeline's name, tinting the bar (teal for the root canvas's timelines,
purple for a macro's, or the timeline's own colour), the macro breadcrumb
back, the timelines navigator (every timeline under its canvas with its
clock source, tempo, meter and where its tempo comes from) and the actions
menu, then the player: the **Free** checkbox (disabled, "Dependent on
<host>", while placed) and the settings popup (loop range, meter and
quantize for a timeline on its own transport; for a macro's timeline,
whether nodes inside follow its clips). The toolbar below drives the
timeline shown: transport buttons (the global transport's, or its own with
quantize and, when free, Play on load), the metronome, position, tempo (the
global BPM, its own BPM / clock ratio / lane, or its host's greyed out),
meter (the global one for a timeline linked to the global transport, else
its own), loop (always the timeline's own), and the **BPM Lane**
toggle (the global lane for a Global timeline, the timeline's own for a free
one, none for a dependent one), all edited through
`ofxOceanodeTimelineBpmEditor` on an `ofxOceanodeTimelineTempoLane` target.
BPM fields apply on Enter. There is no fixed Timeline window: every timeline
is shown in a window of its own (`getTimelineWindows`, saved as `"windows"`,
each with its view). The main controller (`windowId` 0) reports
`drawsOwnWindows()` to the core, so the core draws no window named after it
and the Window menu lists none; instead it creates and draws one editor per
window (`windowId` > 0) from `drawBeforeWindows`, swapping each window's
`ofxOceanodeTimelineViewState` in while it draws, so zoom and scroll are per
window and saved with it. A new timeline (the New Timeline dialog, drawn from
`drawPopups`) opens in a new window, brought forward; a request naming a
timeline ("Add to Timeline", a track rename, a piano roll setup, "Edit
Timeline" on a macro node) goes to the window showing it (`requestTaker`,
`showTimelineInWindow`), opened when none does. Deleting a timeline closes
its windows; a preset saved without windows opens one on the root canvas's
first timeline; the Window menu's **Timelines** list (`drawWindowMenuItems`)
opens and closes the rest. A harness that draws the main controller as an
editor itself (`drawnAsEditor`) gets the requests on it.

A timeline never drives anything above its canvas: binding paths are
relative to the timeline's canvas (`resolverFor(sequence).pathOf` refuses a
parameter outside it), the parameter menu offers a parameter only to the
timelines of its canvas and the canvases around it, and the piano-roll setup
dialog lists the shown timeline's canvas only.

## Coordinates

`EditorView` is constructed for the current draw call. Pixel offsets are
relative to beat zero; the caller supplies the screen origin, including
scrolling. All conversions use the model's tempo map, including the inverse
used for mouse input. The view borrows the live model rather than caching tempo
conversions while a BPM point is being edited.

Curve drawing and evaluation share `ofxOceanodeTimelineCurve`. Lane-specific
insertion, deletion, scales and gestures remain explicit in each editor.

## Extending the architecture

To add a lane editor, implement `drawProperties`, `drawCanvas` and `drawPreview`
in a new class, then add its instance and dispatch case to `LaneEditors`.
Model serialization, creation and evaluation still need to understand a new
lane type. The editor interface is not a dynamic registration system.

Arrangement gestures and instrument/rename dialogs can be extracted separately.
The clip editor shell can also become its own component without moving the
per-type editors again. Persistence, scheduling, evaluation and history
already live in their own translation units; they are still members of the
manager, so they can become separate model services independently of UI
changes.

## Verification

Run the core's `tests/integration_compile.py --timeline <addon path>` to check
all translation units. Run the addon's `tests/timeline_regression.py` for model
and shared coordinate/snap behavior. `tests/editor_regression.py` links the
production editor classes with headless ImGui and tests properties, canvases,
read-only previews, clip-stack balance, focus routing and mouse edits. The
runtime suites use ASan/UBSan with real OF parameter and JSON implementations.
They do not verify visual appearance or audio hardware; use a host application
for those checks.

Regenerate host projects with the openFrameworks project generator after
adding source files.

`tests/clarity_regression.py` checks inspector/playback agreement, overlap ties,
clamping, overrides, missing targets, timing tools, persistence and history.
`tests/controller_clarity_regression.py` draws the complete production controller
with native core and headless ImGui, exercising both projections, mixed lanes,
small windows, audio/LFO scope, mouse tools and deferred target assignment.
Set `OCEANODE_TIMELINE_CAPTURE_DIR` to capture CPU-rendered PPM screenshots of
that suite for visual inspection. It does not exercise GPU rendering or audio.
