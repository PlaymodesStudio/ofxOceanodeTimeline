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
lane properties pad's **Clip** tab and clip context menu. The Clip tab
contains Mute, Repeat (source), Chance, Start, Length (timeline length), Source
(source length) and Stretch; these settings apply to every lane in the clip.
All durations are in beats:

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

**Clip chance** is the probability (0-100%) that the whole clip plays. It is
rolled again each time the clip starts: every play or seek, and every loop
pass. Within one start the result holds for the whole clip, repeats included,
and the scheduler rolls the next loop pass ahead of the wrap so the loop's
first beat stays on time. Set it with the knob at the clip's top-right corner on
the timeline (drag up/down, Shift for fine steps, double-click for 100%), in the
Clip tab, or in the clip's right-click menu. A clip below 100% shows its chance
next to its name; during playback, a clip its roll left out is drawn dimmed.
Chance is saved with the clip; older presets load at 100%.

## Lane properties pad

The left column holds each open clip editor. Its header row shows the clip's
name with **+** (add lane) and **x** (close) at the right. Each lane's pad
starts with fold, M/S and the lane name, then a line naming the lane type and
how many targets it drives (orange when it has none), then its tabs:
**Targets** (what the lane drives), **Lane** (name, type, output range and
the editor's own settings), **Mutate** and **Rndm** (piano rolls only, see
below) and **Clip** (chance and timing, shared by every lane in the clip). Drag the divider in the ruler's top-left corner to make the
column wider or narrower; double-click it for the default width. The width is
saved with the timeline.

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

## Piano note input ordering

Piano input roles determine dispatch order for every instrument: Pitch, Velocity,
then Gate. In scheduled playback Pitch is prepared 2 ms before the note boundary,
Velocity 1 ms before it, and Gate remains on the musical grid. Distinct backend
execution times avoid conflicting writes when an instrument resends all note
inputs from each parameter listener. The ordinary frame path applies inputs in
the same order without adding a frame of gate delay.

The GUI and scheduler correction bookkeeping retain the musical note boundary;
preparing a pitch early does not count as a playhead mismatch or move a note in
the editor. Routing roles, rather than parameter names, select this behavior.

The scheduler evaluates only the paths it can send ahead; unrelated continuous
sound automation remains on the frame path. The target order is sorted once per
evaluation and reused by the scheduler and both frame application passes.

A piano lane's Pitch, Velocity and Gate are scheduled together. If a curve, an
LFO or a live override keeps one of those roles on the frame path, the lane's
other roles stay there too, so Gate never opens with the previous note's pitch.

## Schedule corrections

Events already sent to SuperCollider can't be recalled. After a seek, a stop or
an edit (`invalidateSchedule()`), each path with queued events receives a
correction on the next update. The correction is stamped after that path's queue
and carries the value due at the correction's own instant, so it agrees with any
notes newly scheduled before it. Small disagreements between the playhead and the
backend clock (up to two frames, at least 20 ms) count as timing, not as content
to repair.

Scheduling follows clock-driven transports only (real time and an external
clock). A frame-stepped transport, such as frame mode or an offline render, uses
the frame path, because its playhead does not move with the wall clock that due
times are computed from.

Edits made while playing invalidate the schedule:
- Moving or resizing a clip, adding or removing targets, changing a lane's type,
  removing a lane and toggling bypass do so through the manager.
- Lane editors write the open clip in place. The controller compares
  `lanePlaybackSignature()` of its lanes before and after drawing, and
  invalidates the schedule when it changed. Renames and view settings don't
  count.

## Retrigger gap and lookahead

Back-to-back equal events (repeated notes, adjacent gate steps) close the gate
briefly at the join so the instrument sees a new edge.
- **Frame path:** the gap is at least 10 ms and 1.2 frames, so a frame always
  samples it.
- **Scheduled lanes:** a lane whose targets are all scheduled gets about 3 ms,
  a couple of control blocks. Its gate boundaries execute at their exact
  instant, so the repeated note opens that much closer to the grid.

Which paths are scheduled is decided before each frame's evaluation, so the
frame value and the events sent ahead use the same gap.

`setSchedulingLookaheadMs()` is the longest window. By default the scheduler
uses three recent frame intervals plus 10 ms, at least 50 ms (about 60 ms at
60 FPS). A stall raises the window at once, and it shrinks back over a few
seconds. A shorter window leaves less already-sent content, which a stop, seek
or edit can only correct, not cancel.
`setSchedulingLookaheadAdaptive(false)` uses the setting as a fixed window.
It is saved with the timeline as `scheduling.adaptive`.

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

## Per-frame cost

`tests/perf_benchmark.py` builds an optimized (-O2) session and prints timings.
The session has eight 8-voice instruments with 400 scheduled notes each, a
curve and a number lane per track, and a sigmoid tempo map. Run it after
changing the evaluation path.

- **Values:** automation values travel as text, but they never go through OF's
  string helpers. `ofSplitString(..., trim)` builds a named `std::locale` for
  every token, which made a frame take about 100 ms in that session. The
  timeline tokenizes and parses numbers without locale lookups. It writes each
  float with the fewest digits that read back exactly (`ofToString` keeps 6, so
  2000003 became 2e+06).
- **Applying:** a value is applied by comparing typed values, and nothing is set
  or notified when the parameter already holds it.
- **Indexes:** evaluation indexes (bindings by path, piano voicing, shared piano
  parts) are rebuilt only when `evaluationSignature()` changes. That is a
  word-wise hash of the model, of where its containers live, and of each piano
  lane's instrument voice count and kit. Editors that write the model in place
  are therefore still picked up on the next update.
- **Tempo map:** `beatToSeconds()` caches the time at each tempo point and
  checks the map with a short fingerprint. `secondsToBeat()` is its exact
  inverse: closed form for step and linear segments, safeguarded Newton inside
  one segment for curved ones. The scheduler window and the editors'
  pixel-to-beat conversion use it.
- **Scheduler:** it only looks for change beats in clips and lanes that drive a
  scheduled path.
- **Piano held notes:** the held-note lookup binary-searches each voice slot.

## Piano scale overlay

The piano roll's Lane tab has a **Scale**: turn it on and pick the keys with
the presets (rooted on C; the arrows transpose) or the keyboard. Keys outside
the scale are shaded in the roll and on its keyboard. **Snap to scale** puts
drawn and moved notes on the nearest scale key. It is an editing aid only;
playback is unchanged. The Mutate tab's key presets offer **Lane scale** to
copy it.

## Piano note mutation

Each piano note has a **mutation** amount, set in the **MUT** strip under
VEL and PROB (drag, paint across notes or right-click for a value, like the
other strips). It is the chance that the note plays changed in a repeat. A
note that can mutate has a purple outline. Mutations are rolled again every
repeat of the clip and every time the clip starts (play, seek, each loop
pass). The written notes never change. While playing, the repeat under the
playhead shows where its mutated notes actually sound as purple ghosts.

The lane's **Mutate** tab sets what changes. Each aspect has a **Strength**,
the chance it changes when a note mutates (0% turns it off):

- **Pitch** (default 100%): moves the note to another allowed key within
  **Range** semitones. Right-click Range to choose the direction: up or
  down (+/-), up only (+) or down only (-). Click the keyboard to allow or exclude keys, pick a
  preset (Chromatic, Major, Minor, pentatonics, Whole tone, or the keys the
  lane already uses), and transpose the selection with the arrows.
- **Offset** (default off): moves the note up to **Steps** grid steps (the
  lane's Grid) earlier or later, in multiples of **q** steps (Steps 4, q 2:
  2 or 4 steps).
- **Length** (default off): makes the note up to **Steps** grid steps longer
  or shorter, in multiples of **q**.
- **Velocity** (default off): moves the note's velocity up to **Range**.

Right-click a Range or Steps field to choose its direction: both ways (+/-),
only up / later / longer (+), or only down / earlier / shorter (-).

**Mutation groups** give different notes different rules, such as a narrower
pitch range or other keys for the bass. A lane has group A (the rules above)
and up to three more, B to D, each with its own shade of violet on the
notes' MUT handles and outlines. Every note starts in A.

- **Assign:** right-click a note's MUT handle. Next to its mutation value, the
  group chips move the note to a group, and **+** creates a group and moves
  the note there. If the note is part of a selection, the whole selection
  moves.
- **Edit:** the chips at the top of the Mutate tab pick the group to edit;
  its notes light up in the roll. A new group follows A: a value it hasn't
  changed is shown dimmed and keeps following A. Change it to give the group
  its own value; right-click a changed value and choose **Same as A** to
  follow A again.
- **Delete:** right-click a group's chip. Its notes go back to A.

**Drift** (top of the Mutate tab, for the whole lane, 0-100%): the chance
each note mutates from what it played in the previous repeat instead of as
written, so changes accumulate across repeats and loop passes; 100% always
builds on the previous repeat. Presets saved with the earlier Drift checkbox
load as 0% or 100%. Drifting pitch stays within the
roll's Low..High keys. Stopping the transport, or editing the notes, starts
again from the written notes.

**Mono** (monophonic, also for the whole lane): mutated timing
never makes notes overlap. A note that is lengthened, or that runs into a
shifted note, ends where the next note starts; a note shifted onto another
note's start keeps its written timing.

The VEL, PROB and MUT handles of every note show even when the note itself is
outside the keys the roll shows.

Rolls also depend on the lane's Seed. Voices follow the mutated notes, so
a moved note takes its voice where it now plays; the scheduler uses the same
mutations as the frame path. Mutation amounts, groups and rules are saved;
older presets load with no mutation.

## Piano random deviation

A piano roll's **Rndm** tab moves every note at random, with no per-note
amount and no groups:

- **Octave Deviation** (0-100%): the chance a note plays whole octaves away;
  **Range** (1-4 oct) is up to how many.
- **Pitch Deviation** (0-100%): the chance a note plays semitones away;
  **Range** (1-24 st) is up to how many. With the lane's scale overlay on and
  **Snap to scale**, it lands only on scale keys.

Right-click either slider of a deviation (Deviation or Range) for its
direction: **+/-** (up or down), **+** (up only) or **-** (down only); the
sliders show it. That menu belongs to the slider: right-clicking anywhere
else on the pad still opens the lane's menu. A move that would leave MIDI
0..127 goes the other way when both directions are allowed, else the note
keeps its pitch.

Deviation is rolled for each note in every repeat and every start (like
mutation, and with the lane's Seed), after mutation and on top of it: Drift
builds on mutated notes, never on deviated ones. Voices, the scheduler and the
roll's ghosts follow the deviated notes. Saved with the lane; older presets
load with no deviation.

## Gates, chance notes and duplicated lanes

- **Gate release:** a piano gate held open by the timeline is closed once when
  nothing holds it any more. That covers playback stopping, its last clip being
  muted or removed, an undo or a preset load replacing the content, and its
  binding being bypassed. After that it is left alone for live input.
- **Step and note probability:** steps and piano notes roll again in every
  repeat of their clip and every time the clip starts (play, seek, each loop
  pass), so a note at 90% plays about 90% of the times, even in a clip that
  never repeats.
- **Chance notes:** a piano note with a probability below 1 takes a voice only
  in the repeats where it sounds. A note that fails its roll doesn't cut the
  note before it, steal its slot or move the pitch. These voicings are
  computed per repeat and cached; the frame value and the scheduler's probes
  use the same ones.
- **Duplicated lanes:**
  - A duplicated piano lane gets its own probability seed.
  - Lanes sharing an instrument play a note that appears in more than one of
    them (same start and key) once, so a fresh duplicate doesn't double its
    source. Edited apart, both parts play.
- **Non-finite values:** a non-finite number is saved as `null`. On load, a
  `null` member falls back to its default and a `null` array entry reads as 0,
  instead of discarding the track that holds it.
