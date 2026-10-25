# Timeline

After `ofxOceanodeTimeline::setup(oceanode)`, the root container owns one
`ofxOceanodeTimelineManager`; macro containers share it. The manager owns
tracks, parameter bindings, clips and their automation lanes; it is not a
canvas node.

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

## Duplicating lanes

Open a clip editor and click **D** beside a lane's header, or right-click its
header and choose **Duplicate lane** (also available when collapsed). The copy
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
