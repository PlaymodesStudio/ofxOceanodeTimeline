# Timeline architecture

The timeline has three main responsibilities: project data and playback,
arrangement interaction, and content editing. Keep their ownership separate as
new features are added.

## Current boundaries

| Component | Owns |
| --- | --- |
| `Timeline/ofxOceanodeTimeline.*` | Serializable tracks, clips and lanes; editing operations; evaluation, scheduling and persistence |
| `Controls/ofxOceanodeTimelineController.*` | Window layout, arrangement, selection, deferred structural edits and routing to editors |
| `Controls/Editors/ofxOceanodeTimelineBpmEditor.*` | Tempo editor drawing, point drag, tension drag and numeric popup state |
| `Controls/Editors/ofxOceanodeTimelineWaveVolumeEditor.*` | Track volume editor drawing, point drag and numeric popup state |
| `Controls/Editors/ofxOceanodeTimelineEditorView.*` | Tempo-aware beat/pixel conversion, snapping and grid policy |
| `Controls/Editors/ofxOceanodeTimelineEditorUi.h` | Shared layout dimensions, colors and small ImGui layout/menu helpers |

The controller owns editor instances for its lifetime. Editors own transient
interaction state and borrow model objects only during `draw()`. They do not
retain pointers into track, clip or lane vectors between frames. Persistent
content stays in the model and continues to use the existing JSON format.

`EditorView` is a short-lived value constructed for the current draw call. Pixel
offsets are relative to beat zero; the caller supplies the screen origin,
including scrolling. This keeps an editor aligned with the arrangement even
when its properties panel changes width. All conversions use the model's tempo
map, including the inverse used for mouse input. The view borrows the live
model rather than caching tempo conversions while a BPM point is being edited.

The BPM editor also receives the container to set manual tempo through the
same transport path as the toolbar. Neither extracted editor depends on the
controller or receives access to its private state.

## Continuing the extraction

The LFO, piano roll, curve, held-value, step and multi-row editors are still in
the controller. The arrangement and dialogs also remain there. Move these in
small, behavior-preserving changes:

1. Extract each editor's canvas/properties behavior and drag/selection state.
   Share the view and UI helpers above. Keep a concrete editor class for each
   distinct interaction model; value and gate regions can share region tools.
2. Separate the clip editor shell (header, fold/resize, lane ordering and focus)
   from individual lane editors. It should dispatch the selected lane type and
   own the focused lane ID. This preserves the current rule that only one lane
   handles editing gestures at a time.
3. Return explicit requests for structural changes such as add/remove lane,
   close and rename. Apply them at the controller's existing deferred mutation
   point so an editor cannot invalidate vectors being drawn. Avoid passing the
   entire controller into a widget or exposing it through friendship.
4. Extract arrangement gestures and instrument/rename dialogs independently.
   Keep arrangement selection and clip dragging together, and keep piano note
   selection with the piano editor.
5. Split model services such as persistence and scheduling separately when
   needed. UI modularity does not require changing playback or serialization.

Use direct composition and explicit arguments. Introduce a virtual editor
interface or registration system only when dynamic editor registration is
actually needed. Share curve evaluation math through
`ofxOceanodeTimelineCurve`; preserve each editor's existing insertion, deletion,
scale and gesture rules until those differences are intentionally redesigned.

## Verification

Run the core's `tests/integration_compile.py --timeline <addon path>` to compile
all translation units, including new editor sources. The addon's
`tests/timeline_regression.py` checks model behavior and the shared view's
mapping and snap policy under ASan/UBSan. These checks do not exercise ImGui
mouse gestures or visual layout; those still require a host application.

Regenerate host projects with the openFrameworks project generator after
adding source files.
