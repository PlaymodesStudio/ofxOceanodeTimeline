# Timeline architecture

The timeline separates project data and playback, arrangement interaction, and
content editing. An automation clip can contain several lanes; lane types are
where Curve, Step Sequencer, Piano Roll and the other automation editors live.
An LFO is a self-contained clip with its own control lanes.

## Ownership

| Component | Owns |
| --- | --- |
| `Timeline/ofxOceanodeTimeline.*` | Serializable tracks, clips and lanes; editing operations; evaluation, scheduling and persistence |
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
per-type editors again. Persistence and scheduling can be split into model
services independently of UI changes.

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
