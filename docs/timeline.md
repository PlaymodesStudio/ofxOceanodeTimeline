# Timeline

After `ofxOceanodeTimeline::setup(oceanode)`, the root container owns one
`ofxOceanodeTimelineManager`; macro containers share it. The manager owns
tracks, parameter bindings, clips and their automation lanes; it is not a
canvas node.

## Tracks, clips, lanes and targets

A **track** organizes clips and the parameter targets available to them. A
**clip** owns timing: its start, timeline length, source length, stretch and
repeats apply to every lane inside it. A **lane** owns editable content and its
output settings. A **target** is a published node parameter (called a binding
in the model API). One lane can drive several targets, and several lanes can
drive one target.

For example, a single “Synth phrase” clip can contain a Piano Roll named
“Melody” driving pitch/gate/velocity, a Curve named “Filter sweep” driving
cutoff, and a Step Sequencer named “Rhythm” driving another control. Moving or
stretching the clip moves or stretches all three lanes together. Independent
timing requires separate clips; **Group clips** coordinates those independent
clips, including clips on different tracks.

Each track is one arrangement row and can contain a sequence of clips,
including overlapping clips. Right-click a clip and use **Clips at this
position** to reach a clip hidden beneath another. Click the left track pad
to select and outline a track; Shift or Cmd/Ctrl-click toggles additional
tracks. Clicking a clip switches back to clip selection, and clicking empty
arrangement space clears the selection. Track selection is editor state and
is not saved in a preset. Higher-level track grouping is reserved for a
future **Create group** action on selected tracks; existing **Group clips**
links clip timing and does not create a track hierarchy.

In a parameter's **Add to Timeline Track** menu, choosing an existing track
adds a lane to the selected clip on that track and opens its editor. An open
clip editor is the next choice, followed by the clip at the playhead or the
track's only ordinary clip. If none applies, a new one-bar clip is created at
the playhead. LFO clips keep their fixed control lanes; this action creates an
ordinary clip alongside them. A parameter already bound to the track can be
added to another clip, and a lane already using it is focused instead of
duplicated.

Right-click a track's left pad and choose **Duplicate track** to create an
independent copy immediately below it. Clips, lanes, target routing, piano
groups, audio settings and track volume automation are copied. Clip groups
within the track are copied independently; links to other tracks stay with
the original. The copy is selected, and duplication can be undone.

The **Clips** view is the default arrangement: each clip appears once, with a
content/target summary and miniature previews. **Targets** shows parameter
rows instead. A clip affecting several parameters appears on several target
rows; these are views of the same clip, not copies. Selection highlights every
appearance. Both views keep the same clip selection and open editor.

Double-click a clip to open its editor. Shared timing is available in the
left pad's **Timing** tab. Lane headers show user names and editor types;
**Content** contains the lane's name, editor type and settings,
and **Targets** contains its destinations. **Add target...** searches compatible
parameters in the current patch; selecting a new parameter
publishes it to the track automatically. Existing target checkboxes remove or
restore individual connections without affecting other clips. Piano role
choosers and the LFO output chooser also search the patch. Piano Roll targets are explicitly
assigned to pitch, gate and velocity roles. Right-click a vertical keyboard key
to select every note at that pitch in the piano-roll lane, including notes
outside the visible window. Shift+right-click adds another pitch to the
selection. Left-click still auditions the key. **+ Lane** chooses content and a
target together; “No target (assign later)” creates editable content without
output. Lane properties scroll when a small editor height cannot fit them.
Closing or folding the editor retains content. Right-click anywhere in a
lane's left properties pad for **Duplicate lane** and **Delete lane**.
**Delete lane and clip** explicitly removes the last lane and its containing clip.

LFO clips have oscillator **control envelopes** and one result/output target.
Their controls do not directly automate node parameters. Audio clips contain
samples; clip gain belongs to the selected sample, while the separately
labelled track volume envelope affects every audio clip on that track. Tempo
automation is global to the timeline. “Components” inside a Multi Value or
Multi Gate lane are vector elements, not independently timed clip lanes.

Right-click a multi-lane automation clip and choose **Break lanes into tracks**
to replace it with one new track per lane, in lane order below the source
track. Each new clip keeps its original timing, source mapping and lane
content, with only that lane's targets. Other clips and targets on the source
track remain. The source track remains available even if it becomes empty.
The new tracks are selected and the action is undoable. LFO control envelopes
stay together because they jointly define one oscillator.

## Reusable clip files

Right-click a clip and choose **Save clip to disk...** to save an
`.oceanodeclip` document. It includes all lanes, their content and settings,
shared source timing, repeats/stretch, and the saved target names and piano
roles. Other clips, project tempo, clip groups, and track volume automation
stay in the project. Captured-state clips are recalled as ordinary clips, so
future state captures cannot overwrite the recalled material.

Use **Load clip...** in the toolbar or **Load clip from disk...** in a track's
context menu. Choose an existing compatible track or a new track, set the
start beat, and map each saved parameter to a parameter in the current
project. The chooser lists compatible parameters in the current patch. Nothing
connects automatically based on matching names. **Unassigned** keeps the
lane content without a connection; use the lane's **Targets** tab later to
assign targets and piano roles. LFO files keep their internal control lanes
and remap their final output. Loading is one undoable edit and creates fresh
clip and lane identities.

For audio, saving also creates an adjacent `<filename>.media` folder with a
copy of the source file. Move or share that folder together with the clip
document. File references are relative, so the bundle can move between
projects and computers. Slices, gain, reverse, and playback settings are
preserved; beat-domain source offsets adapt to the destination tempo while
pointing at the same audio. If media is missing, **Replace sample** relinks
the imported clip. Audio files load onto audio tracks.

In the piano roll's **VEL** and **PROB** strips, click and drag across the
vertical handles to paint several notes in one gesture. Fast strokes interpolate
values for every crossed handle, and notes starting together receive the same
value. Dragging repeated or stretched copies edits their shared source notes.
Right-click a handle to enter an exact value.

## Timing tools

The **Right edge** tool also controls edits to **Timeline length** in the
lane properties pad's **Timing** tab and clip context menu. The Timing tab
contains Mute clip, Start, Timeline length, Source length, Stretch and Repeat source;
these settings apply to every lane in the clip. All durations are in beats:

| Tool | Effect |
| --- | --- |
| Repeat | Change the visible length, preserving one source cycle and its stretch; enable repeats |
| Trim / extend | Change the source window at the existing stretch; disable repeats; hidden notes/points remain stored |
| Stretch | Fit the existing source into a new timeline length; disable repeats |

Shift-dragging a right edge temporarily selects Trim / extend. Cmd/Ctrl-dragging
it selects Stretch. Audio always stretches the complete source slice, which
changes playback speed. Left-edge trim and Cmd/Ctrl-drag on a clip body (copy)
retain their existing shortcuts. Drag feedback identifies the operation,
source/timeline lengths and the number of affected clips.

**Source length** is the length of one source cycle. **Stretch** is timeline
beats per source beat, displayed as a percentage (200% takes twice as long).
Editing Stretch preserves the repeat setting and scales the visible duration.
Changing Repeat source on a one-shot uses its current effective stretch;
turning it off fits the full source into the visible duration. **Consolidate
content** is the explicit operation that discards hidden source data.

**Mute clip** is also available in the clip's right-click menu. Muted clips
keep their content and timing, appear dimmed with a Muted label, and do not
contribute automation, notes or audio. Mute is saved with the clip and can be
undone. Muting a sounding piano clip releases its gates unless another clip
or a live preview still drives them.

When playback stops, piano rolls release their gates once and stop writing
pitch, gate and velocity. The instrument's own keyboard can then play freely
while the timeline remains stopped. The piano roll's keyboard preview still
works and releases its notes normally.

## Understanding an output

Open **What drives this target?** in the toolbar, a target row's context menu,
or an assigned lane target's **Inspect** button. At the playhead it shows:

1. Lane values inside clips, including shared piano voice pools.
2. Which contributing clip wins each track target. The latest start wins;
   later clip order breaks equal-start ties.
3. Binding contributions in track order, their blend modes, bypass and live
   overrides. The first contribution supplies the base value.
4. The final timeline output, parameter range clamping and missing targets.

The binding's blend mode combines ordinary lanes within the winning clip and
also its contribution with earlier bindings/tracks. It does not blend
overlapping clips on the same binding. The clamp control edits the parameter's
authoritative first non-bypassed binding, so it consistently controls the final
combined output from every entry point. Outside clips, ordinary values hold;
gate-like outputs may instead contribute zero.

The inspector uses the playback evaluator without writing parameters or
scheduling events. The read-only API is
`inspectTarget(parameterPath, beat, isPlaying)`. Programmatic end edits can use
`resizeClipEnd(trackId, clipId, duration, ofxOceanodeTimelineEndEdit::Repeat)`
(or `Trim` / `Stretch`) to match numeric and mouse editing semantics.

## Minimal usage

```cpp
auto& timeline = oceanode.getTimelineManager();
std::string trackId = timeline.createTrack("Additive Automation");

auto& parameter = static_cast<ofxOceanodeAbstractParameter&>(
    node.getParameters().get("Pitch"));
std::string bindingId = timeline.addBinding(trackId, parameter);
std::string clipId = timeline.createClip(trackId, "Phrase", 0.0, 8.0);
std::string laneId = timeline.createLane(
    trackId, clipId, "Pitch", ofxOceanodeTimelineLaneType::Step);
timeline.addBindingToLane(trackId, clipId, laneId, bindingId);
timeline.setClipStep(trackId, clipId, laneId, 0.0, "60", 1.0);
timeline.setClipStep(trackId, clipId, laneId, 1.0, "64", 0.5);
timeline.setClipStep(trackId, clipId, laneId, 1.5, "67", 2.5);
```

One track can contain several clips, and one clip can contain Step, Curve, Number and
Piano Roll lanes. Ordinary lanes sharing a binding combine using its automation
mode (`Replace`, `Add`, `Multiply`, `Min` or `Max`). Overlapping clips on the
same binding use the clip starting latest, with later track order breaking ties.
A parameter can also be bound on several tracks; binding modes control how
those contributions combine.

Clip source duration and visible duration are independent. Clips can be moved,
cropped, stretched or repeated without changing lane data. “Consolidate
content” permanently removes source data hidden beyond a cropped right edge.

Bindings persist in the preset's `timeline.json` file. Targets are referenced
through the same parameter path convention used by Custom GUI. Missing targets
remain in the timeline data and are marked in memory instead of deleting their
lanes.

## Undo and redo

With the Timeline window focused, **Cmd/Ctrl+Z** undoes an action and
**Shift+Cmd/Ctrl+Z** redoes it. The toolbar also has **Undo** and **Redo** buttons.
Text fields keep their normal text undo while editing.

History covers tracks, bindings, clips, lane contents and settings, clip groups,
markers, loop ranges, tempo automation and manual tempo edits. A complete drag,
paint stroke or popup edit is one action. Navigation, playback and live parameter
values do not enter history; zoom, scroll and playhead position stay where they
are when undoing. Canvas history keeps its own existing shortcuts when the canvas
has focus.

Like canvas history, timeline history is limited to 100 actions and 64 MB, and a
new edit clears redo. Clearing or loading a timeline resets history. Unrecorded
external edits invalidate older history rather than overwriting newer data.
For programmatic edits, wrap related mutations in `beginTimelineEdit()` and
`endTimelineEdit()`, then use `undoTimelineEdit()` / `redoTimelineEdit()`.

## Duplicating lanes

Open a clip editor, right-click a lane's left properties pad and choose
**Duplicate lane** (also available on a collapsed lane's header). The copy
appears immediately below the original, expanded and selected. It keeps all
content, settings, probability seed, bindings and editor height; editing either
lane leaves the other's content independent. Both remain inside the same clip
and share its timing, stretching and repeats. The model API is
`duplicateLane(trackId, clipId, laneId)` and returns the new lane ID.

Piano-roll lanes with the same pitch/gate/velocity bindings in one clip share
a voice pool. Use separate lanes for bass and melody: their notes play together
within the instrument's or note group's voice limit. A part's Mono setting
still limits that part; scalar targets remain monophonic. The same allocation
is used for playback and scheduled note events.

Other ordinary automation lane types can also be duplicated, with their
binding's existing blend mode determining the combined output. LFO control
lanes and audio volume envelopes have fixed roles and cannot be duplicated.

## Metronome and piano-roll grid

The metronome icon beside Play toggles a short audible click during playback.
The first beat of each bar is higher and louder; the remaining beats are softer.
Clicks follow the time-signature denominator (for example, eighth notes in 7/8),
transport tempo, pauses, seeks and loops. Audio goes to the default output device
through a separate output stream, opened only while the metronome is enabled.
On macOS it uses CoreAudio and the output device's current sample rate to avoid
changing the hardware clock and shifting the pitch of existing playback. If the
device rate cannot be read, the metronome stays disabled.
The toggle is a session preference and is not saved in presets.

Piano-roll vertical lines follow the timeline's grid division and absolute beat
positions, including bar/quarter-note emphasis and zoom-dependent subdivision
visibility. Moving, stretching or repeating a clip does not shift these lines.
The lane's Grid setting still controls note insertion and editing; coloured
repeat boundaries remain visible.

## Number clips

Choose **Number** in the clip type selector or lane type menu. Click in the
editor to add a timed number and type its value. Each entry holds its absolute
value until the next entry; there is no interpolation or range mapping.
New lanes start with 0 at beat 0. The first entry also supplies the value before
its timestamp, and the last holds through the clip's end.

Drag a marker to move it using the timeline grid. Double-click or right-click
its marker to edit its value or exact source beat. Shift-click a marker, or use
**Delete number** in its popup, to remove it. Clip stretch, repetition, splitting
and preset save/load preserve the number sequence. Parameter range clamping
and track blend modes apply as usual.

The model API provides `setClipNumber(trackId, clipId, laneId, beat, value)`;
setting a number at an existing timestamp replaces that entry. Number lanes
store absolute values in `curvePoints`, with step interpolation.

## Dropdown clips

**Dropdown** sequences a dropdown parameter (an int or float parameter with
dropdown options): it is the default clip type when such a parameter is
published to the timeline. Click in the editor to choose an option from that
point on; the bound parameter's option names are listed in the popup. Each
choice holds until the next one and is drawn as a block labelled with the
option name, coloured per option. Moving, editing the beat and deleting work
as in Number clips (**Delete option change**).

A Dropdown lane is a Number lane holding option indices: the same
`curvePoints` storage, `setClipNumber` API, splitting and stretching. Values
are always applied as whole indices. Option names are read live from the
parameter, so a lane whose parameter is missing shows indices and edits them
numerically. Switching a Number lane to Dropdown rounds its values to indices.
