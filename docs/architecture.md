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
tempo (stretch) and content offset as on any clip. To play them, the
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

The controller edits one sequence at a time, its scope: the main timeline, or
a macro's (opened with "Edit Timeline" in the macro node menu, shown as a
breadcrumb). Every model call from the controller and lane editors passes the
scope's sequence (`scope(timeline)`; editors get it in their context). The
tempo lane, loop, markers, seeking and the playhead belong to the main
timeline and are hidden in a macro scope; undo steps the scope's own history.

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

`mergeClips` joins two clips of one track into one plain clip (stretch 1, no
repeat or offset): each clip's lanes are laid out as they played (repeats
unrolled, stretch applied, pattern lanes as one pattern over the whole clip),
lanes on the same parameters with the same kind of lane become one lane, and
the later clip's data wins where the clips overlap.

`resizeClipEnd` is the canonical Repeat / Trim / Stretch operation for numeric
length edits and edge drags. Drag snapshots restore the original source mapping
before each application, including group edits, to avoid accumulating rounding
or stretch changes between frames. Audio always normalizes its full slice.

`inspectTarget` rebuilds derived evaluation indexes and calls
`collectActiveValues` with an optional contribution collector. It shares lane
evaluation, clip arbitration, voice allocation, blending and final clamping
with playback; the ordinary playback path does not allocate contribution rows.
Inspection does not apply piano parameter ranges, write target values, alter
source data or schedule events. Shared piano lanes are reported as one voice
pool contribution with their contributing source lane IDs.

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
