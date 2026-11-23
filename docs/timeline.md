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
track's only clip. If none applies, a new one-bar clip is created at
the playhead. A parameter already bound to the track can be
added to another clip, and a lane already using it is focused instead of
duplicated.

Right-click a track's left pad and choose **Duplicate track** to create an
independent copy immediately below it. Clips, lanes, target routing, piano
groups, audio settings and track volume automation are copied. Clip groups
within the track are copied independently; links to other tracks stay with
the original. The copy is selected, and duplication can be undone.

**Copy track** (or **Copy folder**, or Cmd+C with tracks selected) copies
tracks to paste into this or any other timeline, in any Timeline window:
right-click a track there and choose **Paste track after this**, or press
Cmd+V (after the selected track, else at the end) or the toolbar's **Paste**.
The pasted tracks keep their targets, clips, note groups and clip groups, and
do not depend on the timeline they came from, which can then be removed. Into
a macro's timeline (or out of one) the targets' paths are rebased, so they
still reach the same parameters. A Macro Morphing Track is never copied; a
Global Track goes only to a timeline of its own canvas without one; a track
playing another timeline stays in that canvas, and never plays the timeline
it is pasted into. Beats are copied as beats: the pasted clips play at the
receiving timeline's tempo and loop.

**Copy clip** (in a clip's menu, or Cmd+C with clips selected) copies clips
with their targets. **Paste clip here** in a track's or a target row's menu
pastes them from the clicked beat, keeping their spacing; Cmd+V pastes them
onto the selected track at the playhead, or onto a new track when none is
selected. The track takes the copied clips' targets it lacks (and their note
group); targets it already has are reused. Audio clips go only on audio
tracks, automation clips only on automation tracks. Every paste can be undone.

The **Clips** view is the default arrangement: each clip appears once, with a
content/target summary and miniature previews. **Targets** shows parameter
rows instead. A clip affecting several parameters appears on several target
rows; these are views of the same clip, not copies. Selection highlights every
appearance. Both views keep the same clip selection and open editor.

Double-click a clip to open its editor. The hamburger button before the
track header's **+** opens a floating **Clip settings** window for shared timing. Lane headers show user names and editor types;
**Content** contains the lane's name, editor type and settings,
and **Targets** contains its destinations. **Add target...** searches compatible
parameters in the current patch; selecting a new parameter
publishes it to the track automatically. Existing target checkboxes remove or
restore individual connections without affecting other clips. The list is the
track's: a target given to one lane is offered, unchecked, to its other lanes.
The **-** at the right of a target takes it off this lane's list (the lane no
longer drives it); the track's other lanes keep it, checked or not, as they
have it. Once no lane of the track lists it, the track lets go of the
target. A parameter is highlighted in the canvas, in its track's color, only
while some lane drives it: one that is unchecked in every lane is not.
**Add target...**
gives it back to a lane that removed it. Piano role
choosers also search the patch. Piano Roll targets are explicitly
assigned to pitch, gate and velocity roles. Right-click a vertical keyboard key
to select every note at that pitch in the piano-roll lane, including notes
outside the visible window. Shift+right-click adds another pitch to the
selection. Left-click still auditions the key. A note sounds while it is drawn,
grabbed, moved or resized (the lane's **Live edit sound**, on by default). **+ Lane** chooses content and a
target together; “No target (assign later)” creates editable content without
output. Lane properties scroll when a small editor height cannot fit them.
Closing or folding the editor retains content. Right-click anywhere in a
lane's left properties pad for **Duplicate lane** and **Delete lane**. Right-click a
target's row in the **Targets** list, checked or not, for that target's own
menu: **Show in Canvas** opens the canvas its node is on (the macro it is
in, when it is inside one) and centres on the node. A piano roll or drum
grid lane, which has no such list, keeps **Show in canvas** in the lane's
menu.
**Delete lane and clip** explicitly removes the last lane and its containing clip.

Several tracks can keep their lane editors open together. Each open track shows
every clip's lanes as separate rows, in clip order. Selecting a clip on an open
track expands all of that clip's lanes and folds the other clips' lanes on the
same track. Double-clicking a clip opens a closed track this way; when that
track is already open, double-clicking any of its clips folds the whole track.
Other tracks retain their own expansion state. A folded lane keeps a
compact data preview in the clip's color, aligned to its position in the
arrangement. Click the preview to focus its lane; double-click to expand it.

Zooming out stops when the last clip's end is halfway across the view, leaving
an equal amount of empty space to its right. This limit uses clips from every
track in the timeline, including hidden or muted tracks, and updates as their
lengths change. Reaching the limit returns the view to the origin.

The toolbar's **Zoom to fit** button (four corners, also Alt+Z) fits every
clip and marker. **Zoom to loop** (the same corners around an L, to its left)
fits the loop region instead; it is disabled while Loop is off. A loop longer
than the widest clip-based view is still shown whole.

The zoom slider under the track pads zooms around the playhead while it is in
view, else around the view's centre. With Cmd (or Ctrl) held it does what a
Cmd wheel zoom does: the playhead comes to the middle of the view and the zoom
happens there.

### Track pads

Every pad in the left column (a track, a folder, a row of an unfolded macro
track) lays its controls out in rows from its top. The first row has what
every track has: from the left, the fold arrow, the colour, the name, then the
hamburger (Clip settings) and **+**; against the right edge, **M** and **S**.
A track without one of these keeps the others where they are. What only one
kind of track has goes on a second row, against the right edge: a Global
Track's **<** **+ Key** **>**. A macro track's pad has that first row only
(the timelines it plays show under it when it is unfolded), so its header is a
line shorter than the other tracks'. A
pad too short for two rows keeps them on the first, before **M**.

An **LFO** is a lane type like Curve or Step Sequencer: a clip can hold any
number of LFO lanes alongside its other lanes, each with its own **Targets**
and **Lane** tabs, mute/solo, chance and output range (Min/Max, set from the
first target's range). Its canvas shows the oscillator's result over the clip,
following the clip's repeats, stretch and offset. Below it, indented, are its
nine **control sublanes** (Frequency, Roundness, Skew, Pulse width, Pow,
BiPow, Phase offset, Scale, Y offset): envelopes over the clip that shape the
oscillator. They start folded to one line each (showing the control's value
and a miniature of its envelope); each expands, resizes, mutes and solos
(among its siblings) on its own, and folding the LFO lane hides them all. Controls do not drive parameters
themselves. Switching a lane away from LFO keeps its controls for switching
back. The **LFO Track** node follows the first LFO lane of each clip on its
track. Clips holding an LFO lane cannot be merged. Audio clips contain
samples; clip gain belongs to the selected sample, while the separately
labelled track volume envelope affects every audio clip on that track. Tempo
automation is global to the timeline. “Components” inside a Multi Value or
Multi Gate lane are vector elements, not independently timed clip lanes.

Right-click a multi-lane automation clip and choose **Break lanes into tracks**
to replace it with one new track per lane, in lane order below the source
track. Each new clip keeps its original timing, source mapping and lane
content, with only that lane's targets. Other clips and targets on the source
track remain. The source track remains available even if it becomes empty.
The new tracks are selected and the action is undoable. An LFO lane moves
with its control sublanes, which jointly define one oscillator.

## Reusable clip files

Right-click a clip and choose **Save clip to disk...** to save an
`.oceanodeclip` document. It includes all lanes, their content and settings,
shared source timing, repeats/stretch, and the saved target names and piano
roles. Other clips, project tempo, clip groups, and track volume automation
stay in the project. Captured-state clips are recalled as ordinary clips, so
future state captures cannot overwrite the recalled material.

Use **Load clip...** in the timeline's **...** menu or **Load clip from disk...** in a track's
context menu. Choose an existing compatible track or a new track, set the
start beat, and map each saved parameter to a parameter in the current
project. The chooser lists compatible parameters in the current patch. Nothing
connects automatically based on matching names. **Unassigned** keeps the
lane content without a connection; use the lane's **Targets** tab later to
assign targets and piano roles. LFO lanes keep their control sublanes and
remap their targets like any lane. Loading is one undoable edit and creates fresh
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
floating **Clip settings** window and clip context menu. Clip settings
contains Mute, Repeat (source), Chance, Start, Length (timeline length), Source
(source length) and Stretch; these settings apply to every lane in the clip.
All durations are in beats:

| Tool | Effect |
| --- | --- |
| Repeat | Change the visible length, preserving one source cycle and its stretch; enable repeats |
| Trim | Change the source window at the existing stretch; disable repeats; hidden notes/points remain stored |
| Stretch | Fit the existing source into a new timeline length; disable repeats |

The tool is chosen per timeline window and saved with the window in the
preset (`rightEdgeTool` in its `"windows"` entry); a new window, and a preset
saved before, start on Repeat.

Moving either loop handle preserves the red playhead's current position,
both while playing and while paused. Playback continues towards the edited
boundary. Moving the end behind the playhead leaves playback beyond that
boundary until an explicit seek or restart enters the loop again.

A plain drag of a right edge uses the selected tool; the other two stay on
the modifier keys, so all three are always in reach. Shift-dragging selects
Trim and Cmd/Ctrl-dragging Stretch, except that the key of the selected tool
selects Repeat instead (Shift wins when both are held):

| Right edge tool | Drag | Shift-drag | Cmd/Ctrl-drag |
| --- | --- | --- | --- |
| Repeat | Repeat | Trim | Stretch |
| Trim | Trim | Repeat | Stretch |
| Stretch | Stretch | Trim | Repeat |

The selector's tooltip shows what each key does now. Audio always stretches
the complete source slice, which
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
Clip settings window, or in the clip's right-click menu. A clip below 100% shows its chance
next to its name; during playback, a clip its roll left out is drawn dimmed.
Chance is saved with the clip; older presets load at 100%.

Each lane also has a **Chance** circle at the top-right corner of its clip content, in both
expanded and collapsed rows. Drag vertically to set it, hold Shift for fine
adjustment, or double-click to reset to 100%. Its independent roll stays fixed
for the same clip start. The effective probability is clip chance multiplied
by lane chance: 50% × 50% gives 25%. The circle's tooltip shows both values.
Skipped gates, steps and value regions rest at zero; curves, numbers, dropdowns
and sliders hold their previous value. Piano parts sharing an instrument roll
independently. Skipped LFO control envelopes use the oscillator's default for
that control. Lane chance is saved and undoable; older lanes load at 100%.

Deleting a Curve point preserves the Sigmoid or Log/Exp settings of all
surviving segments. When two segments merge, the merged span keeps the
incoming segment's settings. The same rule applies to LFO control curves.

## Lane properties pad

The left column holds each open clip editor. The track header's arrow opens
or closes its lanes, **+** adds a lane or clip, and the hamburger opens shared
Clip settings. When a track contains several clips, the floating window has a
clip picker. A lane made for a parameter is named by the parameter's whole
path, node included (`MacroMixer_1/Target`), however it was added; rename it in
its **Lane** tab. Each lane's pad starts with fold and the lane name, with M/S
against its right edge (as on a track pad), followed directly by its tabs:
**Targets** (what the lane drives), **Lane** (name, type, output range and
the editor's own settings), **Mutate** and **Rndm** (piano rolls only, see
below). Drag the divider in the ruler's top-left corner to make the
column wider or narrower; double-click it for the default width. The width is
saved with the timeline.

Drag an expanded lane's bottom divider to reduce it to its header row (fold,
Mute, Solo and name). Resized lanes retain their normal editable canvas at every
height. Only the fold arrow switches to the compact preview. The minimum follows
the current font size.

When playback stops, piano rolls release their gates once and stop writing
pitch, gate and velocity. The instrument's own keyboard can then play freely
while the timeline remains stopped. The piano roll's keyboard preview still
works and releases its notes normally.

## Understanding an output

Open **What drives this target?** in a target row's context menu,
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
// A canvas has any number of timelines and none to begin with: make one
// (on the main canvas here), then edit it through its sequence.
const auto ref = timeline.addTimeline("", "Additive");
auto& sequence = *timeline.timelineFor(ref);
std::string trackId = timeline.createTrack(sequence, "Additive Automation");

auto& parameter = static_cast<ofxOceanodeAbstractParameter&>(
    node.getParameters().get("Pitch"));
std::string bindingId = timeline.addBinding(sequence, trackId, parameter);
std::string clipId = timeline.createClip(sequence, trackId, "Phrase", 0.0, 8.0);
std::string laneId = timeline.createLane(
    sequence, trackId, clipId, "Pitch", ofxOceanodeTimelineLaneType::Step);
timeline.addBindingToLane(sequence, trackId, clipId, laneId, bindingId);
timeline.setClipStep(sequence, trackId, clipId, laneId, 0.0, "60", 1.0);
timeline.setClipStep(sequence, trackId, clipId, laneId, 1.0, "64", 0.5);
timeline.setClipStep(sequence, trackId, clipId, laneId, 1.5, "67", 2.5);
```

Code that keeps one timeline can set it as the edit target once
(`timeline.setEditTarget(ref)`) and use the id-only overloads
(`createTrack(name)`, `addBinding(trackId, parameter)`...) as before.

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

With a timeline window focused, **Cmd/Ctrl+Z** undoes an action and
**Shift+Cmd/Ctrl+Z** redoes it.
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
For programmatic edits, wrap related mutations in `beginTimelineEdit(sequence)`
and `endTimelineEdit(sequence)`, then use `undoTimelineEdit(sequence)` /
`redoTimelineEdit(sequence)` (the id-only forms act on the edit target and
the global settings together).

Every timeline keeps its own history, and the global transport's settings
(markers, tempo lane, loop, meter) keep theirs. Undo steps whatever was
edited last, so an edit made in an unfolded macro track (see below) is undone
from the timeline it is unfolded in as well. A timeline's clock source is
not undone.

## Unfolding a macro track

A macro track's clips play its macro's own timeline. **Open its timeline**
in the macro track's or a macro clip's right-click menu opens that timeline on
its own (as the macro node's **Edit Timeline** does); the scope bar's path
leads back.
To see it in place instead, click the arrow on the macro track's header (or
use **Unfold its tracks here** in its menu). The macro track then holds the
timelines its clips play, each on a row of its own: the timeline the track
places first, then any other timeline of the same macro that no track places
(they play along under the same clips). A timeline's row shows its name, how many
tracks it has and where the macro clips play it; its arrow folds its tracks
away (the row then previews them, a thin line of clips per track, and a click
on that preview unfolds it again), and a double-click opens the timeline on
its own. Which timelines are folded is saved with the track.

A macro track made by placing a macro's timeline is named after the macro,
as **MT:** and the title in its node's header (its local name, or the
library Macro's), since it holds every timeline of that macro. The name
follows the macro: rename it, or load another Macro in its node, and the track
(and its clips still named after it) is renamed with it, unless you gave the
track a name of your own
(rename it like any track; a track placing another timeline of the same
canvas keeps that timeline's name).

Under each timeline's row its tracks show as
rows, tabbed in one step (one more step for each macro inside a macro, and for
each folder): the indentation alone shows the levels. A folder of that timeline has its row too,
with its tracks tabbed in under it: its **M** and **S** apply to everything
inside, its row previews those tracks, and its arrow folds them away (a click
on the preview of a folded folder unfolds it). Folding a folder is the
timeline's own state, so it is folded wherever that timeline shows. Each
track's row shows its clips where the
macro clips play them, through their repeats, tempo, content offset and
reverse, so you can see the macro's contents against the rest of the
arrangement.

Every level is tabbed in by the same step in the left column (a folder's
tracks, a macro track's timelines, their tracks, a track's lanes, an LFO
lane's controls). A row's indentation is a color code of what it is inside:
one strip per level, each in the header color of that level (a folder, the
macro track, one of its timelines, the track a lane belongs to). A level's
strip runs down the left edge for as long as its contents go, so the strips
show at a glance where each folder, macro track, timeline and track ends.
Rows stack with no gap between them and no line is drawn over the left
column, so the strips run unbroken; a track's separator line shows over its
clips only (and over its own header while you hover or drag it).
A lane's header is its track's header color, 25% darker, folded or not.

The line at the bottom of a track's row is its separator: drag it to resize
that row (double-click it for the default height). While the track's lanes
show under it, the line in the track's color between the row and its first
lane is that handle, here and on a track of the timeline shown: it sits
directly between the two headers, with no gap above or under it. The height is the track's
own, saved with the macro's timeline, and used wherever that timeline is
unfolded. Each lane of the docked clip editor is resized from its own bottom
edge.

A clip on these rows is drawn in its track's color, 25% darker, whatever its
state; the clip being edited or dragged has a light outline. A clip's name
(here and on a track of the timeline shown) is drawn dark on a light title
band and light on a dark one, so it reads on any track color.

Clips on these rows are edited in place: drag one to move it, drag its right
edge to repeat, trim or stretch it (the **Right edge** tool and its modifiers
apply), drag its left edge to trim it, and double-click it to open its content
editor under its row. The arrow on a track's row does the same, as on a
track's own pad: it shows the track's lanes under the row (on its first clip
with lanes) and folds them away again. Any number of rows can show their
lanes at once, in any of the timelines and at any depth: opening, editing or
folding one leaves the others as they are, and each stays open until its own
arrow (or a double-click on one of its clips) folds it. The editor has everything it has in the timeline's own window:
each lane's canvas and its **Targets** and **Lane** tabs (so **Add target...**
gives the lane, and its track, a new target from here), tabbed in under the
row. **Add target...** lists what that timeline can drive, one folded branch
per place to look in: the parameters of the nodes inside its macro, then
those of every canvas above the macro up to the main one. Each branch names
its canvas and who saves a target taken from it; unfold the one to pick
from (it stays unfolded for the next lane). Typing in **Search parameters**
lists the matches of all of them at once, under the same headings. The
piano roll's and the drum grid's target pickers are laid out the same way.
The main timeline's lanes keep their plain list of the patch's parameters.

A parameter taken from a canvas above the macro is an **outside target**.
The lane drives it like any other, wherever the timeline plays (under a
macro track or on its own), and the lane's **Targets** list names, after
the parameter, the canvas it lives in: **(Canvas)** for the main canvas,
else that macro's title. It is not saved with the macro: the canvas the parameter lives in
saves it, under the Macro node the way down to the lane starts from (the
project for a parameter of the main canvas, the macro around it for one of
that macro's canvas). The macro itself stays as it was, so a library Macro
keeps working in every project that uses it, and **Save Macro** leaves its
outside targets out. Copies of the same library Macro share their timelines,
and so their outside targets. A Macro node switched to another library Macro
keeps the outside targets of the one it held, for when it is switched back. A track deleted from the macro takes its
outside targets with it the next time the project is saved; a deleted lane
leaves the target on its track. The right-click menu also renames, mutes, duplicates and
deletes it. The rows' **M** and **S** mute and solo within the macro's
timeline. Edits change the macro's timeline itself, so every macro clip
playing it follows; moves and snapping are in that timeline's own beats.
Where a reversed macro clip plays the timeline mirrored, its clips are shown
but not edited. A macro track inside the macro unfolds the same way.

A track on these rows is selected like a track of the timeline shown: click
its pad (Shift or Cmd/Ctrl-click for several), and its pad lights up with an
outline around its row. Double-click its name to rename it. Right-click its
pad for its track menu: its color, a new clip at the playhead, the blend mode
and bypass of each of its targets, **Remove from Timeline** for a target, its
latency, **Rename track**, **Duplicate track**, **Remove track** and opening
the macro's timeline on its own. They act on the macro's timeline itself, so
every macro clip playing it follows. Dragging tracks to reorder them, folders
and loading clips from disk are still done in the timeline's own window.

A macro track's own menu has **Save Macro** for a Macro of the library (a
global one): it writes the Macro back to the library as it is now, with the
changes made to its timelines here, and its other instances reload it. A
local Macro is saved with the preset, so the item is disabled for it.

Clips and lanes are made on these rows too. Right-click empty space of a
track's row where a macro clip plays the timeline forward: **New clip here**
opens the New Timeline Clip dialog for that track of the macro's timeline,
with the start in that timeline's own beats. Before a row's **M**, as on a track's
own pad, are the clip settings button (timing, stretch and repeats of a clip
of the track, in a window) and, for a track whose clips have lanes, a **+**
that adds another lane to the clip being edited (opening its editor first), or
a new clip on the track. A piano roll clip on a track that has no note routing
group yet opens the macro's timeline to set the group up there. Adding
tracks is still done in the timeline itself.

A Wave track's clips show their waveform on its row (and as a thin trace in
the previews of a folded timeline, a folder and the macro clip itself). They
are moved, trimmed and stretched there like any clip; a Wave clip's file and
properties are edited in the macro's own timeline.

A Global Track of the macro's timeline shows its keyframes on its row, with
the easing between them, and they are edited there as on its own track: drag a
keyframe to move it, drag an easing handle to shape the transition
(double-click: linear), double-click a keyframe to put the playhead on it, and
right-click to add a keyframe or to copy, paste or delete keyframes. A click
selects a keyframe (a white frame around it), Shift or Cmd/Ctrl-click adds
or removes one, and a right-click on a keyframe that is not selected selects
it alone; the selection is within one clip. Dragging one of several
selected keyframes moves them all together, at their distances, as far as
the first and last stay inside the clip. **Copy** and **Delete** act on
the selected keyframes. **Paste here** and **Paste at playhead** put the
copied ones from that position on, keeping their spacing and easings and
overwriting a keyframe already there; they can go to another clip or another
timeline's Global Track. With keyframes selected, Cmd/Ctrl+C copies them and
Delete removes them (not the clip they are in); Cmd/Ctrl+V pastes copied
keyframes at the playhead, into the clip keyframes were last selected in.
Copying keyframes replaces copied clips or tracks, and the other way round.
The row's pad
has the same **< + Key >** buttons: **+ Key** stores the macro's canvas where
the playhead plays that timeline (so the playhead has to be on a macro clip
playing it forward), and **<** and **>** step the playhead through the
keyframes of every repeat. While the playhead is on a keyframe **+ Key** is
filled with the track's colour: pressing it overwrites that keyframe. A macro clip's own preview marks each keyframe with
a line.

The unfolded state is saved with the track. Structural work (new tracks,
targets, note groups) still happens in the macro's timeline itself, opened by
**Edit macro timeline** (or a double-click on the macro clip).

## Macro Morphing Track

A Macro Morphing Track plays macro sources as clips on one lane and, where two
clips overlap, mixes or morphs one source into the next over the overlap. It
is the MacroMixer node laid out in time: the same engine, routers, sources and
policies, driven by the playhead in either direction. A timeline holds one at
most.

**Creating it.** The timeline's actions menu has **Add Macro Morphing Track**.
The track starts empty.

**Routers.** **Macro Morphing Routers** in the track's menu opens the routers
window (the MacroMixer inspector's Routers and default policies). The routers
are published as ports on every **Timeline** node that points at this
timeline, after a `ROUTERS` separator: inputs are written into the sources,
outputs carry the mix. Several Timeline nodes may point at one timeline; each
publishes the outputs, and the last input to change wins. Without such a node
the track still plays and draws, and its outputs go nowhere.

**Sources and clips.** **Add Morphing Source** in the track's menu asks for a
macro folder; a macro's header menu has **Add to Timeline Macro Morphing
Track**, which adds the macro as it was last saved (a local macro at any
depth, once its preset is saved). Either adds the source to the track's engine
and a clip after the last one, overlapping it by 4 seconds (never more than
half of the shorter clip). A clip lasts one pass of the source's first
timeline and repeats when it is made longer.

**Overlaps.** Clips obey the track's overlap rules: at most two play at any
beat, and no clip lies inside another. Moving or resizing a clip is held to
the nearest valid timing; a move takes a clip past a neighbour when it fits
beyond it at its length and that is nearer to where it was dragged. An overlap
is drawn as a box labelled **Mix** or **Morph**.

**Transition settings.** Double-click an overlap (or **Transition...** in the
later clip's menu): Mix or Morph, Bi Pow (the shape of the progress), Log
Blend, and this transition's own morph policies and router mix policies over
the track's defaults. They are stored on the later clip.

- **Mix:** both sources run; router outputs are mixed by each router's policy.
- **Morph:** the incoming source also starts from the outgoing source's
  control values and glides to its own.
- **Morph, In place:** the outgoing source's graph morphs into the incoming
  one, when the two sources have matching nodes: one graph runs instead of
  two and nothing loads at the overlap. The outgoing source's own automation
  stops when the overlap starts. When the sources do not allow it, or the
  overlap is entered from the incoming source (scrubbing backwards), the morph
  runs on two graphs; the window says which is running and why.

**Source window.** Double-click a clip outside an overlap (or **Source...**
in the clip's menu): the source's folder, problems, checks, kept-out nodes,
**Refresh**, **Open Source** and **Save Source**. **Open Source** shows the
source's patch in a canvas tab while the source plays alone; the tab closes
when a crossfade starts or the source stops playing. Once the patch has been
opened, **Save Source** writes it too: the nodes as they are at that moment
(values included) and their cables, with the nodes kept out of the graph
written back as the folder had them. Both are disabled during a crossfade. The button on the track's pad opens the
track's menu, the same one a right-click on its header opens.

**Pre-loading.** A source loads when the playhead reaches its clip, which can
stall that frame. **Pre-load next source (s)** in the track's menu loads the
next clip's source that long before the clip starts. Loaded ahead, the source
runs silent: its portals publish, it takes GPU memory and its synths may
start. It is off by default, and is not used for a Morph set to run in place.

**Unfolding.** The arrow on the track's header shows the
sources under the track. The sources playing at the playhead (one, or the two
of a transition) show their tracks as a macro track does: editable in place.
The other sources are under an **Other tracks** row: dimmed, read-only rows
with their clips as bars; a double-click moves the playhead onto that source.
The arrow on **Other tracks** folds all of them away at once (the row then
shows where their clips are, and a click on it unfolds them again); it is
saved with the project. A source's parameters exist
only while it plays, which is why only those rows are editable.

**Source timelines.** A source's timelines never play on their own: they
follow the clip playing the source, and so do the timelines of the macros
inside it, at any depth. Edits to them are kept by source folder: they survive
the source's graph being rebuilt, last for the session, and reach the
source's folder only with **Save Source**. A `*` on a clip or on a source's
header marks changes (to its timelines, or to its patch once opened) not saved
to the folder. **Refresh** reads the folder again and drops them. Before the
project is closed or replaced, the "Save changes?" dialog lists the sources
with unsaved changes and offers **Save sources**, which writes them to their
folders; saving the project itself never does. A changed patch can only be
written while its source plays alone, so save it before the next crossfade.

**Editing while two sources play.** Routers, sources and policies cannot
change while two sources play: one of them (the one further along) stays
live, the other loads again on the next frame.

## Marker Track

A Marker Track holds markers: moments of the timeline, each with a title, a
description and a colour. The colour fills the marker's section, from the
marker to the next one (the last one to the end). Markers are numbered in
time order from 0. The number is not stored: moving a marker past another
renumbers both. A Marker Track plays nothing and has no clips, targets, M or
S. A timeline has one at most, so it is never duplicated.

**Creating it.** Use **+ Markers** on the toolbar, or **Add Marker Track** in
the timeline's actions menu.

**Markers.** The track pad's **+** adds a marker at the playhead. **<** and
**>** move the playhead to the previous or next marker. On the track:

- Double-click empty space to add a marker there. **Add marker here** in the
  track's right-click menu does the same.
- Click a marker to move the playhead to it.
- Drag a marker to move it (snapped to the grid).
- Double-click or right-click a marker to edit it.

A new marker opens its popup at once. The popup shows the marker's number,
its title, description, colour and beat, and has **Delete marker**. Edits
apply as you type and make one undo step when the popup closes.

**Go To Marker.** A Timeline node whose timeline has a Marker Track shows a
**MARKERS** separator with a **Go To Marker** int input. When the value
changes to a marker's number, that timeline's playhead goes there, as
Restart seeks. A number with no marker does nothing, and neither does -1.
To jump to the same marker again, send another value first, for example -1:
an input set to the same number every frame must not hold the playhead
there. The value is never saved, so loading a preset does not move the
playhead. The node saves that it has the port, so its cable survives a
project load. The port and separator go away when the timeline has no
Marker Track.

## Several timelines, each with its clock source

There is no main timeline, and no fixed Timeline window: every timeline
lives in a window of its own, opened when the timeline is made and listed in
the **Window** menu under **Timelines** (ticked while it has a window; untick
to close its windows, the timeline stays). Every canvas (the main one and
every macro's) can own any number of timelines, and none exists until you
make one: **New Timeline** in a canvas's right-click menu, **Add to Timeline
> New Timeline...** in a parameter's right-click menu, or **New timeline...**
in a timeline window's **...** menu. The new one opens in a new window,
brought forward (in the asking window itself while that shows a canvas with
no timeline). The scope bar at the top of each window says which timeline it
shows: a badge with its name (teal for a timeline of the main canvas, purple
for a macro's own, or its own **Color**; always 160 px wide, a longer name
shortened and shown whole in its tooltip), the path to it (**Macro 1 > ...**,
each step a way back), the timelines navigator (every timeline under its
canvas, with its clock source, tempo, meter and where its tempo comes from;
pick one to show it in this window), and the **...** menu: new, duplicate,
rename, colour, delete (the timeline goes with its tracks, and its windows
close), place another timeline here, or open this one **in a new window**
(a second view of it). A macro's timelines
are listed with the macro's node menu too (**Edit Timeline** and the rest:
one item when it has one timeline, a submenu when several, **New
Timeline...** when none). A track that places a timeline shows the placed timeline's name
after its own, in that kind's colour. A custom GUI widget's right-click menu
offers the same **Add to Timeline** items as the parameter in its node.

**Clock source.** The preset's global transport belongs to no timeline. The
scope bar also holds the shown timeline's player: the **Free** checkbox and
what it plays on. Off (the default), the timeline is **linked to the global
transport**: it plays, stops and seeks with it, at the global tempo and
meter, like any other timeline linked to it. Its loop is its own, like every
timeline's: the global transport itself never loops (the Global menu's
transport time runs on past any loop end), the timeline wraps that
transport's position in its loop, so several timelines linked to the global
transport can each loop over a range of their own while the rest of the
preset follows the transport straight on. On, it is
**free**: its own transport on the shared clock, played, stopped and seeked
by itself at its own tempo and loop, while the global transport plays or is
stopped. A macro's own timeline can also be switched from the macro node's
menu (**Free tempo for its timeline**). A timeline placed as clips in another
timeline is **dependent** on it, whatever the checkbox says (it is greyed out
until the timeline track placing it is removed): it plays where the clips
place it, at that timeline's tempo, beat for beat through the clip's
stretch; its playhead follows the timeline placing it while that one plays,
as a precomposition follows its composition. Its own meter can still differ
(3/4 inside a 4/4 timeline). Its Play previews it alone, at its host's tempo,
and never plays the host; while it previews, what it drives is heard over
the timeline placing it.

**The toolbar** drives the timeline shown, whatever its clock source:

- **Transport**: the global transport's buttons, or (free, dependent) its own
  **Play**, **Stop** (back to its start), **|<**, plus **Quantize** (Play and
  Stop take effect at once, or on the next beat or bar of the global
  transport while it plays, else of the clock, so timelines launched apart
  start in phase; the start is a timestamped instant, so its first events
  reach SuperCollider as exactly as the rest) and, when free, **Play on
  load**. Space plays and pauses the timeline shown.
- **Metronome**: clicks along the timeline shown, in its meter, whatever its
  clock source (the global transport's, its own, or a dependent timeline's
  preview). Switching it on in another timeline moves it there.
- **Position** (bar . beat . sixteenth, song time).
- **Tempo**: the global BPM for a timeline linked to the global transport
  (**LANE** or **EXT** when the global BPM lane or an external clock sets it);
  a free timeline's own fixed **BPM**, the clock's tempo times a ratio (**x
  Clock**: it follows tempo changes) or its BPM lane (**Lane**); a dependent
  timeline's host tempo, greyed out (**DEP**). BPM fields apply when you
  press Enter; **Tap** taps it.
- **Meter**: the global time signature for a timeline linked to the global
  transport, else the timeline's own (edited in the player's **...** popup
  too).
- **Loop**: the timeline's own, whatever its clock source (linked to the
  global transport, it wraps that transport's position in it; the transport
  itself runs on). The ruler edits the same one.
- **BPM Lane**: shows the BPM lane under the ruler: the global tempo's for a
  timeline linked to the global transport, the timeline's own when it is
  free (none when dependent). Its **Enable** box turns the lane on; off, the
  lane shows the flat BPM beside it. Hidden, an enabled lane still plays.

These settings are saved with each timeline (in the preset, or in the macro's
`sequence.json`, so a macro's timelines travel with the macro into the
library and back), its clock source included. A macro's timeline playing free
is also what the macro's canvas plays to: nodes inside it whose Time Source
is Auto follow it. Its Wave tracks play too (a dependent timeline's only
while it previews).

**Placing a timeline as clips.** A macro node's **Add to Timeline as Macro
Track** asks which of the macro's timelines to place (when it has several)
and which timeline it goes on (the main canvas's, or one of a macro around
it). **Add a timeline track...** (in the
same **...** menu) adds a track to the timeline shown whose clips play another
timeline, exactly like a macro track: move, repeat, stretch, offset and
reverse the clips. Any timeline of the same canvas, or of a canvas inside it,
can be placed; one that would end up playing itself (directly or through the
timelines it places) is not offered. A new clip lasts one pass of what it
plays: the end of the last clip, at least a bar of that timeline's meter. A
macro placed as a whole plays every timeline of the macro, so its pass is the
longest of them, whichever one was picked. Macro clips follow that length. A
macro clip never plays less than one whole pass: when a clip inside one of
those timelines is moved or resized so the longest one ends later (on an
unfolded row, or in the timeline's own window while this one is open), every
macro clip of the track takes the longer pass as the edit is released, and so
does a macro clip that is older than its timelines the first time it is
shown. When the longest timeline gets shorter, the macro clips whose pass was
its old length take the new one. A clip lasting a whole number of passes (one,
usually) is resized to as many of the new pass; one cut to some other length
keeps that length. A clip whose pass is longer than its timelines keeps it:
**Fit to its longest timeline** in its right-click menu gives it the current
pass. The
placed timeline follows the host beat for beat: a 4-beat timeline fills 4
host beats; stretch the clip (a Stretch drag of its right edge, Cmd+drag
unless Stretch is the selected **Right edge** tool, or the Tempo field) to
play it slower or faster, and **Restore length** puts it back to
its natural speed with as many repeats. Its data is shared, so an edit
reaches every instance, while each instance has its own playhead and rolls
its own chance and mutations. **Duplicate this timeline** makes an
independent copy. Tracks of a placed timeline drive the same parameters
wherever it plays, so overlapping instances on one parameter resolve like any
two timelines do (below).

**Nodes inside a placed macro.** Normally nodes inside a macro (Time Source
Auto) follow the timeline around it. With **Nodes inside follow its clips**
(in the macro node's menu, per timeline, and in the timeline's player
popup), they follow the clips playing that timeline instead: its beat,
restarting with each clip and repeat, at the clip's stretched tempo, and
stopped where no clip plays it. A Phasor inside a fill macro then restarts
with each fill. One timeline of a macro at most; the choice is saved with
the timeline.

**Who wins on a parameter.** Timelines are evaluated inner canvases first,
so where two drive the same parameter at the same moment the outer one's
value lands. The timeline whose value lands last sends the parameter
through its scheduler; the others send it at frame rate.

**What a timeline can drive.** A timeline drives what is inside its canvas:
its own nodes and those of the macros in it, never a parameter of a canvas
above it. **Add to Timeline** offers a parameter to the timelines of its
canvas and of the canvases around it; the piano-roll dialog lists the shown
timeline's canvas.

**Nodes that follow a timeline.** Nodes synced to the transport (Phasor,
Counter, Ramp, Beat Transport, the SuperCollider boxes) have a **Time Source**
in the inspector in place of *Sync To Transport*: **Free** (not synced),
**Auto** (the timeline its canvas plays: the global transport, or its macro's
own timeline when that plays free), **Clock** (always running at the global
tempo), or any timeline by name, this canvas's first: the global transport
for one linked to it, its own transport otherwise (a dependent timeline's
preview). The choice is saved by id, so renaming a timeline keeps it; a
deleted one shows *(missing)* and the node holds stopped (a node saved
before timelines had ids of their own still follows its canvas's first
timeline). **Global transport** in that list always follows the global
transport, wherever the node is. **Beat Transport** has the same choice on the node itself, its
**Transport** dropdown: **Global Transport** (the default), then every
timeline that has a transport, this canvas's first (a dependent timeline's
is its preview). It reads that transport's beat, tempo, play state, meter and
loop, and plays, stops and seeks it (its **Play**, **Stop** and **Seek**
inputs), quantized to the next beat or bar. Patches saved before keep their
choice (*Auto (this canvas)* for an older Auto). The **Timeline** node plays,
stops and restarts any timeline from the patch.

**Adding a parameter.** **Add to Timeline** in a parameter's right-click menu
first asks which timeline (those of the parameter's canvas and of every canvas
around it), then which track; with only one timeline it goes straight to the
tracks. Its **New Timeline** submenu takes a name and creates a timeline with
the parameter on its first track, on the parameter's canvas or one around it,
and shows it (a canvas with no timeline offers only that). **Remove from
Timeline** finds the parameter in all of them. The **LFO Track** node picks
a timeline, then one of its tracks.

**Windows.** Each timeline opens in a window of its own, docked with the
timeline windows already there, with its own zoom, scroll and **Right edge**
tool (saved with the window). **Open in a new window** (or **Open its timeline in a new window**
from a macro node) opens one more on that timeline; **Edit Timeline** on a
macro node and **Add to Timeline** bring the window showing the timeline
forward, or open one. A window can show any other timeline from its own
menu. Windows are saved with the preset; a preset saved without any (an
older one, or every window closed) opens one on the main canvas's first
timeline, and the **Window** menu's **Timelines** list opens the rest.

**Inside a MacroMixer.** A MacroMixer loads each source preset as a Macro in
a graph of its own; the graph is a guest of the mixer's canvas (its canvas id
nests under it, `MacroMixer/7 / Macro 1`), and the timeline walks into it as
into any macro. A preset folder loaded as a Macro has no `sequence.json`: its
`timeline.json` is read instead, so the preset's root timelines become the
source Macro's, and the Macros inside it load theirs as usual. A free one
set to **Play on load** starts when its graph starts (at activation, or as
a morph into it begins); one linked to the global transport follows the
global transport of the preset hosting the mixer. They stop with their
graph. When a macro's timelines are reloaded into a running canvas (a Macro
loading another preset, a morph in place), what the previous ones played
stops and rewinds first. A MacroMixer opens a window on a source's timeline when
the source's **Windows** list ticks it: titled after the source ("Timeline:
Timeline 1 (2--BLOOBS_Symetric)"), keyed on that name so its docking is found
again, and never saved in the preset's `timeline.json` (the mixer opens it
again). A track bound to a MacroMixer's **Target** starts its transition when
the value it writes changes, as a cable does: every automation value is written
inside an `AutomationWriteScope`, so listeners can ask the parameter
`isReceivingFromAutomation()`. Unchanged values are not written, so a held
value does not restart the fade.

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
binding's existing blend mode determining the combined output. An LFO lane
is duplicated with its control sublanes; audio volume envelopes have a fixed
role and cannot be duplicated.

## Metronome and piano-roll grid

The metronome icon beside Play toggles a short audible click along the
timeline shown, whatever its clock source (the global transport, its own, or
a dependent timeline's preview), in that timeline's meter. The first beat of
each bar is higher and louder; the remaining beats are softer. Clicks follow
the time-signature denominator (for example, eighth notes in 7/8), that
transport's tempo, pauses, seeks and loops. Audio goes to the default output device
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

Cmd+C copies the selected piano-roll notes and Cmd+X cuts them. Cmd+V pastes
them with the earliest note at the mouse position (snapped to the lane's grid
when Snap is on), keeping pitches, velocities, probabilities and mutation, and
leaves them selected. The clipboard is shared by every piano roll for the
session, so notes can go to another clip, track or timeline. A monophonic lane
drops the notes the pasted ones overlap.

Cmd+dragging a clip onto another track copies it there. Lanes follow the
destination's binding of the same parameter; a Piano Roll or Drum Grid whose
parameters that track lacks plays the track's own note group instead, so a
phrase can be copied from one instrument to another.

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

## Colour lanes

A colour parameter (`ofFloatColor` or `ofColor`) can be published to a track
like any other, and its clips get a **Colour** lane by default. The lane has
two bands over the same time axis:

- **Colours** (above): each key is a small swatch at its beat, and the band
  is painted in the colour the lane plays, so a blend shows as it will look.
  Click to add a key (it takes the colour playing there), drag a key to move
  it, double-click or right-click it for the colour picker (with alpha),
  Shift-click to delete. The last key cannot be deleted.
- **Blend** (below): one sigmoid per pair of consecutive keys, from 0 at a key
  to 1 at the next. Drag it sideways to move where it bends and up or down to
  steepen it, as a Global Track's easing; double-click for a straight blend.
  There is always exactly one blend curve between two keys.

Colours blend channel by channel (red, green, blue and alpha) along that
sigmoid; after the last key the colour holds.

A Colour lane keeps its points like a Number lane (`curvePoints`, with one
`curveTensions` entry per segment for the blend). A point's value is an index
into the lane's `colors` (saved as `[r, g, b, a]`), so splitting, trimming and
reversing a clip work as they do for held values. Splitting a clip in the
middle of a blend starts the right-hand part on the earlier key's colour.
Two Colour lanes are never joined into one when clips are merged.

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
- **Missing targets:** a binding or Global Track parameter that is not found
  (its node deleted, or kept out of a Macro Morphing source) costs a scan of
  its whole canvas. It is looked up again after 30 frames, or at once when a
  node is made or deleted anywhere (`ofxOceanodeNode::graphGeneration`). A
  parameter added to an existing node is picked up within the 30 frames.
- **Global Track repeats:** the host applies automation up to three times a
  frame (before the nodes, after the morph engines, after the nodes). A Global
  Track writes a value only when it differs from what it last wrote, so a
  repeat with the same inputs writes nothing and is skipped. The inputs: the
  evaluation, the playhead, the node graph, the automation filters
  (`ofxOceanodeAbstractParameter::automationFilterGeneration`), the channels
  and preset loads. Repeats still run while a channel has an automation filter
  (a morph blending it in).
- **Macro Morphing Track:** the Timeline node publishes the routers on update
  and only the Texture routers on draw, so what is connected to them runs once
  a frame. The parameter lookup cache is dropped after the engines update
  only when an engine updated inside that pass (none does when a Timeline
  node points at the timeline).

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

## Drum Grid

A **Drum Grid** lane (`ofxOceanodeTimelineLaneType::DrumGrid`, saved as
`"laneType": "DrumGrid"`) is a step sequencer for a drum kit instrument: one
row per kit piece. It plays through the same role bindings as a piano roll:
`pianoGateBindingId` (required) and `pianoPitchBindingId` (optional), plus its
own optional `drumDurationBindingId` (the kit's Duration input:
`ofxOceanodeTimelineInstrument::duration`, kept on the note group as
`durationBindingId`; piano lanes of the group ignore it) and `drumFilterBindingId`
(the kit's Filter input: `ofxOceanodeTimelineInstrument::filter`, a bipolar
-1 low-pass fully closed .. 0 open .. +1 high-pass fully closed; on the group as
`filterBindingId`) and `drumStutterBindingId` (the kit's Stutter input:
`ofxOceanodeTimelineInstrument::stutter`, a retrigger rate as a MIDI note while
the hit's gate is open, 0 = off; on the group as `stutterBindingId`). A group
that never had a Duration / Filter / Stutter role (projects
saved before the instrument offered one) adopts it automatically as soon as its
instrument reports the input: the binding is created, set on the group and its
Drum Grid lanes, without an undo entry (undo history keeps working). Once a
group has had the role, set or cleared by the user, it is never adopted again;
groups save this as `"durationDecided"` / `"filterDecided"` / `"stutterDecided"`. Rows are drawn in
their pad's colour from `ofxOceanodeTimelineInstrument::rowColors` (index =
slot, read every frame, never saved; rows without one use the track colour).
`pianoVelocityBindingId` is unused, the velocity travels as the gate level.
Row `r` drives component `row.slot` of every vector.

**Creating one.** "Add to Timeline as Instrument..." on a node whose
`getTimelineInstrument()` reports `kit = true` opens the instrument dialog with
**Editor: Drum Grid** preselected (Piano Roll stays available, and is the
default for other instruments). The new lane gets one row per instrument voice,
named from `ofxOceanodeTimelineInstrument::rowNames` ("Row n" past its end).
A kit note group's row menu offers **New drum grid clip here** next to the
piano-roll item, and "Drum Grid" is in the clip and lane type lists.

**Hits are positions, the grid is for editing.** A row holds hits at beat
positions from the start of its pattern (like a piano note's `startBeat`), not
step indices. The lane's grid only decides where new hits snap and which
columns are drawn: either the timeline's master grid (the toolbar's grid
division, `getMasterGridBeats()`, saved with the timeline as
`"masterGridBeats"`; 1/16 while it is None) or a fixed division of the lane
(`drumStepFollow` false, `beatsPerStep`). New Drum Grid lanes, and lanes
converted from the first format, follow the master. A following lane reads the
master every frame, so its columns and its paint/move step change as soon as
the toolbar grid does. The canvas shows the lane's grid next to the strip tabs
("Grid: Master (32nd)" or "Grid: 16th (fixed)"); click it to switch between
the master and a fixed division (also in the Lane tab). It is shown while the
lane is tall enough for the strip. The divisions include dotted and triplet values (half, quarter, 8th,
16th, 32nd, each plain, D and T). Changing the grid never moves a hit or
changes what plays: hits placed on 1/32 keep their places on a 1/16 grid.

**Evaluation**, per row, at the clip's source beat (clip position, stretch and
repeats, lane and track mute/solo and binding blend modes work as for every
lane):

- A row repeats every `length` beats (1/24..64; rows of different lengths make
  a polymeter) and restarts at every clip repeat. Hits at or past the length
  are kept but silent ("hidden"): shortening a row never deletes hits, and
  lengthening it back brings them back unchanged. Fills, shifts, quantize,
  randomize and Clear leave hidden hits alone; they are saved like any hit.
- A hit plays at its position. Swing (0..75%) delays hits on odd positions of
  the lane's swing step (`drumSwingBeats`, 1/16 by default) by that much of the
  step; hits off the swing step are not swung.
- Each hit stores its own `length` (beats) and `gate` (fraction of it): its
  gate stays open length x gate, at its velocity, so playback never depends on
  the grid. New hits get the grid step as length and the row's gate. A ratchet
  of N divides the hit's length into N sub-hits, each open sub-length x gate.
- Gate component = the velocity while a hit is open, else 0 (`1` for a
  non-float target). Pitch component = 60 + the hit's semitone offset, held
  after the gate closes so a tail keeps its tuning. Duration component = the
  hit's decay multiplier (x0.1..x24, x1 = as set), Filter component its
  filter (-1..1, 0 = open) and Stutter component its stutter note (0..127,
  0 = off), all held the same way. Before a row's first hit they hold its last
  audible hit's values (60 / x1 / 0 / 0 for a row with no hits). The stutter
  lasts while the hit's gate is open, so its length and gate set how long it
  buzzes.
- A hit that starts while (or less than one retrigger gap after) the previous
  hit of its row was open opens after the retrigger gap, so ratchets,
  back-to-back hits and pattern/clip wraps always give a fresh rising edge.
- Vectors are written at max(highest slot + 1, the instrument's voices, the
  parameter's current width): nothing broadcasts. Rows without a hit hold 0
  gate and 60 pitch.
- Probability uses the lane's `probabilitySeed` and is rolled per pattern cycle,
  per clip repeat and per clip start (play, seek, loop pass), like steps and
  piano notes. "Probability" off plays every hit.

**Scheduling.** Each hit's start (pitch, duration), gate opening and close are
change beats for the scheduler, which hands the Gate/Pitch/Duration/Filter values
ahead to backends that execute timestamped values (the instrument's scheduled
inputs; Stutter too). Pitch, Duration, Filter and Stutter are prepared 2 ms
ahead of Gate, so a hit's tuning, decay, filter and stutter are in place when
it fires; the role paths are scheduled together or
not at all, like a piano lane's roles. The grid is not involved.

**Editor.** Row headers (left of the timeline zone): a swatch and the name in
the row's colour, M and S, and the length as a field with - and + (in steps of
the current grid when it divides evenly, else beats, e.g. `1.5b`): drag the
field like a slider, double-click / Ctrl-click to type, - / + change it by one
grid step. Taller rows put the field on a second line. Click a header to focus
its row, right-click for its menu (rename, length in steps or beats, gate of
new hits, slot, clear, fill every 2/3/4/8 steps, shift by a step, quantize to
the grid, randomize, copy/paste, add/remove). Each row's first cycle ends at a
thick line in its colour with a handle: drag it to set the length (grid
steps; Alt: free; the tooltip shows "5 steps" / "1.5 b"), double-click it for
the clip's length. Its repeats after it are ghosted, with a fainter line at
each further cycle start; hidden hits show as faint outlines where they would
play. In the grid:

- A click on an empty cell adds a hit at the cell's start; a click on a hit
  removes it. **A cell that holds a hit (on or off the grid) grabs that hit**:
  the click removes it, it never stacks a second hit at the snapped position.
  A hit wider than its cell is grabbed anywhere along its drawn span.
- Dragging from an empty cell paints hits into the empty cells it crosses;
  Cmd/Ctrl-drag erases. Dragging a hit sideways moves it by whole grid steps,
  keeping an off-grid offset (Alt: freely); vertically (or Shift-drag) sets its
  velocity. Right-click a hit for its fields: beat, length, gate, velocity,
  pitch, probability, ratchet, duration, Snap to grid, Delete.
- Hits are drawn at their true position and length; hits off the current grid
  have an amber outline. Fill height shows velocity, a dotted top edge a
  probability below 100%, ticks the ratchet, numbers the pitch offset; repeats
  of a row past its first pattern are shaded.

The strip under the grid paints the focused row's Velocity, Pitch (semitones,
bipolar), Probability, Ratchet, Gate, Duration, Filter or Stutter, one bar per hit at
its x, in the row's colour (log scale over x0.1..x24 for Duration, bars growing
from the x1 line, guides at x0.25..x16; painting snaps to x0.125, 0.25, 0.5, 1,
2, 4, 8, 16 or 24, Alt paints unsnapped. Filter: bipolar around open, low-pass
(LP, blue) below, high-pass (HP, orange) above, snapping to open near the
centre, Alt unsnapped, double-click a bar to open it; hovering shows
"LP 40 %" / "HP 25 %" / "open". Stutter: bars from the bottom, height = the
note on a linear 0..127 scale, no bar when off; painting snaps to semitones,
Alt unsnapped, the bottom few pixels or a double-click turn it off; labels and
hover show the note and its rate, e.g. "C3 · 131 Hz", or "off"). For the
others Alt drags finely; right-click resets a bar.
The strip's parameter is picked from a row of tabs with full names on top of
the strip (inside the timeline zone, short names only when the editor is very
narrow) or from the **Row** properties tab, which also holds the focused row's
name, slot, length and gate. The **Lane** tab holds the grid (Master or a
division), swing and its step, probability and seed, and the rows.

```json
"drumGrid": {
  "format": 2,
  "stepFollow": true,
  "swing": 0.0,
  "swingBeats": 0.25,
  "rows": [
    {"slot": 0, "name": "Kick", "length": 4.0, "gate": 0.5, "mute": false, "solo": false,
     "hits": [{"beat": 0.0, "length": 0.25, "gate": 0.5, "velocity": 0.8, "pitch": 0,
               "probability": 1.0, "ratchet": 1, "duration": 1.0, "filter": 0.0,
               "stutter": 0.0}, ...]}
  ]
}
```

Only Drum Grid lanes write `drumGrid`, so other lanes save exactly as before.
The lane also saves `"drumDurationBindingId"` when set, and note groups save
`"duration"` and `"durationDecided"`. Missing or malformed fields load as their
defaults (a hit without `duration` plays at x1, without `filter` open, without `stutter` off) and values are clamped into
range.

**The first, index-based shape** (no `"format"`: rows of `"steps"` with
`"on"`, a length in steps, `"nudge"`) is converted on load so it plays
identically: an on step i becomes a hit at i x `beatsPerStep` (a nudged one
at the exact place it played, swing included), length `beatsPerStep`, gate the
row's gate; row length = steps x `beatsPerStep`; the lane swings on
`beatsPerStep` as before and edits on the master grid; on steps past a row's
length (which never played) are kept as hidden hits. Probability rolls are keyed so converted hits roll as
before. It is saved back in format 2 (an older build would read such a lane as
empty rows).
