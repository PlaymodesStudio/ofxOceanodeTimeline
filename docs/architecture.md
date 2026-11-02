# Timeline architecture

The timeline separates project data and playback, arrangement interaction, and
content editing. An automation clip can contain several lanes; lane types are
where Curve, Step Sequencer, Piano Roll and the other automation editors live.
An LFO is a self-contained clip with its own control lanes.

## Ownership

| Component | Owns |
| --- | --- |
| `Timeline/ofxOceanodeTimeline.*` | Serializable tracks, clips and lanes; editing operations; the per-frame engine (update, apply, overrides), tempo, markers and loop |
| `Timeline/ofxOceanodeTimelineSequence.cpp` | `ofxOceanodeTimelineSequence`: one timeline's tracks, clip groups and id counters, with lookups and new ids. The manager keeps the main timeline in `mainSequence`; each editing operation also has an overload taking the Sequence to edit (undo, playback and per-frame caches follow the main timeline only) |
| `Timeline/ofxOceanodeTimelineEvaluation.cpp` | Value at a beat (`collectActiveValues`), change beats, piano voicing and mutation, evaluation indexes, play context rolls. Both walks (and the Wave pass) visit `playedSequences()`, each sequence in its own beats through its `ofxOceanodeTimelineTimeMap`: the main timeline through the identity map, then each macro clip repeat's placement of a macro timeline (see Macro timelines below) |
| `Timeline/ofxOceanodeTimelineScheduler.cpp` | Sample-accurate scheduling ahead of the playhead, corrections and latency offsets |
| `Timeline/ofxOceanodeTimelinePlayers.cpp` | Timelines by reference (`TimelineRef`: canvas id and timeline id), a canvas's further timelines, timeline tracks (`createTimelineTrack`, `placedTimeline`, cycle checks), `collectMacroSources`, each timeline's clock source, player and engine, time-source registration, timeline windows (see Several timelines below) |
| `Timeline/ofxOceanodeTimelinePersistence.cpp` | `timeline.json`, clip files and presets |
| `Timeline/ofxOceanodeTimelineHistory.cpp` | Undo history of timeline edits |
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

Macro timelines: every macro can own a sequence
(`ofxOceanodeTimelineMacroSequences.cpp`). It is created on first use, its
binding paths are relative to the macro's container, and it is saved as
`sequence.json` in the macro's own folder (its preset folder, or its library
definition, through the core's `loadMacroPreset`/`saveMacroPreset` hooks).
Copies of a library macro share one sequence; entries follow the macro's
container through a `weak_ptr`, so a deleted macro's sequence goes with it.

A macro track (`macroCanvas`, the macro's canvas relative to its sequence)
has macro clips that place the macro's timeline: start, length, repeat,
tempo (stretch) and content offset as on any clip; the placed timeline
depends on the host (see Several timelines below). To play them, the
evaluation index rebuild copies every macro timeline the main timeline
reaches (`playedMacros`, also through macros inside macros) and makes the
copies' paths and canvases absolute, so bindings, the apply pass, the
scheduler and the inspector treat them like main-timeline bindings.
`playedSequences()` gives each macro clip repeat a placement: a time map,
a window of main-timeline beats (split where the content offset wraps) and
its own instance key, with the repeat mixed into the pass so each repeat
rolls anew. The value walk evaluates one placement per macro timeline per
beat, and combines the copies' bindings before the main timeline's, so the
main timeline layers on top with its bindings' modes. Mute, solo and
latency are read within each track's own sequence (`sequenceOf`); the
macro tracks' latencies add up around a copy, and a copy's offset wins on a
path the main timeline also drives. Wave tracks play on the main timeline
only.

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
tracks driving only the encapsulated nodes out of the canvas's timeline
(main at the root, else that macro's), rewrites their paths relative to the
new macro, and puts a macro track with one clip in their place. The core
keeps the extension's record with the canvas undo step;
`replayParameterMove` brings the tracks back on undo and moves them again on
redo (skipping tracks a restored macro already has). Extraction is recorded
the same way, paths only.

The controller edits one sequence at a time, its scope: the root canvas's
default timeline (`scopes` empty), or any other (opened with "Edit Timeline"
in the macro node menu or from the timelines navigator, shown on the scope
bar with the path back). Every model call from the controller and lane editors passes the
scope's sequence (`scope(timeline)`; editors get it in their context). The
toolbar drives the timeline shown: its transport (`transportFor`), loop and
tempo (`shownLoop`, `shownBpmLaneTarget`), as its clock source says (below).
Markers belong to the root canvas's default timeline; undo steps the
scope's own history. With no timeline on the shown canvas yet
(`timelineExists` false), the window shows how to make one
(`drawEmptyState`).

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
the later clip's data wins where the clips overlap.

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
reports, "" for the root) and its id within that canvas ("" for the canvas's
default timeline: `mainSequence` at the root, the macro's sequence inside it).
A default sequence keeps its canvas's further timelines in
`ofxOceanodeTimelineSequence::timelines` (each with its `timelineId`), so they
are shared, pruned and saved with it: the root's in `timeline.json`
(`"timelines"`), a macro's in its `sequence.json`. Undo snapshots of a
sequence (`sequenceToJson(sequence, false)`) leave them out.
`timelineFor(ref)`, `refOf(sequence)` and `canvasContainer(canvas)` translate
(`canvasContainer` finds a canvas with a timeline through the macro sequence
entries before walking the graph). A further timeline's id comes from the
canvas's saved `nextTimelineNumber`, so a deleted timeline's id is never given
to a new one: tracks, windows and time sources that still name it stay
unbound. An undo step of the main timeline leaves the canvas's further
timelines and the windows as they are (`timelineEditData` drops them;
`restoreTimelineState` keeps them), and each further timeline keeps a
history of its own.

**No timeline exists until it is made.** A canvas's default timeline (the
root's included) is there (`timelineExists`) once it was made on purpose
(`play.created`, set by `addTimeline`) or has tracks; a new project or macro
has none. `addTimeline` makes a canvas's default timeline first, then further
ones. Deleting a default timeline (`removeTimeline` with an empty id) empties
it (tracks, groups, settings) and keeps the canvas's further timelines.
There is no main timeline: the root canvas's default one is a timeline like
any other (named "Timeline 1" until renamed).

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
meter, launch quantize, play on load, colour, and (a macro's own) whether
nodes inside follow its clips. The whole struct is saved as the timeline's
`"play"` (`playSettingsToJson`); older files kept the free set (`"players"`)
and the clip-following macros (`"clipFollow"`) apart and are read into the
settings (`legacyFreeTimelines`, applied as each macro's sequence loads).

**Clock sources** (`clockSourceOf`). A timeline placed as clips by any
timeline (`placedTimelines`, placed to first host, refreshed each update pass
and after edits through `invalidateEvaluationIndexes`) is **Dependent**,
whatever its setting. Otherwise it is **Free** (`play.free`) or **Global**
(the default). The root canvas's default timeline, while Global, is played by
the manager itself on the root transport as before (`mainPlayedByPrimary`);
free or dependent, the manager's own walks skip it (`evaluatedSequences`,
`playedSequences`, `collectMacroSources` return nothing) and an engine plays
it like any other timeline.

Every other existing timeline has a **player** (`Player`, `updatePlayers`): a
transport and an **engine**, a second `ofxOceanodeTimelineManager` with
`primary` set that evaluates, schedules and applies an absolute-path copy of
the timeline (`makeAbsolute`, rebuilt when `sequenceSignature` changes) on
that transport. Global: the transport is the root transport itself
(`engineSharesRoot`): the engine reads the global tempo, tempo lane and loop
(adopted from the manager each frame, so lookahead follows the global map)
and never sets them; its meter is its own. Free: a transport derived from the
root's clock (`ofxOceanodeTransport::makeDerived`) with the timeline's own
tempo, loop and meter (`adoptPlaySettings`). Dependent: a derived transport
too, at its host's tempo now (`hostBpmFor`, through the host's transport) with
its own loop and meter; it is the timeline's **preview**, run only while it
plays or has a pending action (and once more after it stops), so an idle
dependent timeline costs nothing per frame (its transport still takes its
host's tempo). A preview is heard over the timelines placing it: the
primary's `applyAutomation` applies the previewing engines' values last. Engines read their primary's macro
sequences (`sequenceEntries`), run inner canvases first and before the main
timeline, so the outer value lands last; a path the main timeline binds is
left out of an engine's scheduling (`pathDrivenElsewhere`). A macro's default
timeline playing free sets its container's transport override, so nodes
inside follow it; it lets go when it stops being free.

`transportFor(ref)` is the root transport for a Global timeline, a player's
own otherwise (nullptr until its player runs). `playTimeline`, `stopTimeline`
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

Macros whose own timeline has `nodesFollowClips` get a driven transport
(`ofxOceanodeTransport::setDrivenState`) as their canvas override:
`updateClipTransports`, at the end of the primary's `evaluateAutomation`,
sets it to the first placement of the macro's copy at the playhead of a
playing timeline (beat, host tempo over the map's ratio, restart when the
placement changes), the main timeline's or an engine's.

`updateTimeSources` registers every existing timeline with the root transport
as a time source (`timeSourceId(ref)`: `canvas#timeline`, `root#default` for
the root canvas's default): the root transport itself for a Global one, the
player's transport otherwise.

The **metronome** (`setMetronomeEnabled(on, ref)`) follows one timeline's
transport, whatever its clock source, in that timeline's meter
(`metronomeRef`); it moves with the timeline's transport when its clock
source changes and goes with the timeline.

The controller shows any timeline (`enterTimelineScope`; `EditorScope::ref`).
`drawScopeBar` (`ofxOceanodeTimelineControllerTimelines.cpp`) draws the scope
bar at the top of the window: a badge with the shown timeline's name, tinting
the bar (teal for the root canvas's timelines, purple for a macro's own, teal
for a macro's further ones, or the timeline's own colour), the path of scopes
back, the timelines navigator (every timeline under its canvas with its clock
source, tempo, meter and where its tempo comes from) and the actions menu,
then the player: the **Free** checkbox (disabled, "Dependent on <host>",
while placed) and the settings popup (loop range, meter, quantize). The
toolbar below drives the timeline shown: transport buttons (the global
transport's, or its own with quantize and, when free, Play on load), the
metronome, position, tempo (the global BPM, its own BPM / clock ratio / lane,
or its host's greyed out), meter (the global one for the root's default
timeline while Global, else its own), loop (the global transport's, or its
own), and the **BPM Lane** toggle (the global lane for a Global timeline, the
timeline's own for a free one, none for a dependent one), all edited through
`ofxOceanodeTimelineBpmEditor` on an `ofxOceanodeTimelineTempoLane` target.
BPM fields apply on Enter. Further windows (`getTimelineWindows`, saved as
`"windows"`) are editors of their own (`windowId` > 0) the main controller
creates and draws from `drawBeforeWindows`; each swaps in its own
`ofxOceanodeTimelineViewState` while it draws, so zoom and scroll are per
window.

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
