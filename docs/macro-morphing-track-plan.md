# Macro Morphing Track (MMT): architecture check and implementation plan

Status: implemented (all four waves). What was built is described in
`timeline.md` ("Macro Morphing Track") and `architecture.md` ("Macro Morphing
Track"); this file is kept as the record of the decisions. Differences from
the plan: routers are published by the Timeline node; a timeline holds one
track; only the sources playing at the playhead unfold as editable rows (the
others are read-only); unsaved source edits are saved with the project;
in-place Morph is a per-transition option, tried only when the target is not
already live; pre-loading is a per-track option, off by default.

Written 2026-11-21 as a plan, against ofxOceanodeTimeline `main` (`4f16794`)
and core `megabranch26_A` (`37fe574e`).

Note: `../ofxOceanode/docs/macro-mixer-timeline-study.md` is out of date.
Mixer graphs already play their sources' timelines (as timeline guests with
their own players), and an in-place morph already blends into the destination's
timeline through a per-parameter automation filter.

## 1. What the feature is

A track that holds several macro clips on one lane. Each clip plays one macro
source (a folder). Adjacent clips may overlap; the overlap is a transition
(Mix or Morph) whose progress is the playhead's position inside the overlap.
It is the MacroMixer node laid out in time: clip = source, overlap = transition,
overlap length = Duration.

User-facing surface (from the request):

| Action | Where |
| --- | --- |
| Add Macro Morphing Track | timeline actions menu |
| Macro Morphing Routers (window, like the node's ROUTERS section) | track menu |
| Add Morphing Source (folder picker) | track menu |
| Add to Timeline Macro Morphing Track | right-click on a macro node header |
| Source info (like the node's MACROS section) | double-click a clip |
| Mix / Morph and policies for that transition | double-click an overlap |
| Unfold each clip's tracks and lanes under the track | as a Macro Track does |

A new source is appended after the last clip, overlapping it by 4 s. Clips can
be moved to change the overlap. The overlap is drawn as a rectangle.

## 2. What exists today (verified in code)

Core, `../ofxOceanode/src/Nodes`:

- `MacroMixerRuntime` is already node-independent
  (`explicit MacroMixerRuntime(ofxOceanodeContainer& parent)`,
  `MacroMixerRuntime.h:16`). It owns the contract (routers), sources, policies,
  and two graphs `current`, `next` (`.h:156`).
- Progress is clock-only: `t = (clock() - startMs) / durationMs`
  (`MacroMixerRuntime.cpp:808`). There is no setter.
- The end of a transition is irreversible: promotion at `t >= 1`
  (`MacroMixerRuntime.cpp:883`) and `applyMorph(progress >= 1)` commits the
  topology (`MacroMixerSource.cpp:2139`).
- Two graphs at most ("two-instance limit", `MacroMixerRuntime.h:19`). A graph
  is built synchronously on the main thread; about 190 ms for a BLOOBS source
  according to `docs/macro-mixer-sc-servers-plan.md` (not measured here).
- Sources are folders only. A canvas macro can be a source through its saved
  folder (local `Macro_N` folder or library folder).
- Router ports, the transport parameters and all ImGui (`drawRouters`,
  `drawMacros`, `drawSource`, `drawMorphPolicies`, `drawMixOptions`) are private
  to the node `ofxOceanodeMacroMixer`.
- Each graph's host is a timeline guest (`MacroMixerSource.cpp:259`), so the
  addon's walks and `findTimelineParameter` reach it.

Timeline addon:

- There is no track-kind enum. A macro track is `!macroCanvas.empty()`
  (`ofxOceanodeTimeline.h:1201`). The macro is per **track**; a clip only adds
  `macroSnapshot` and `reversed`.
- A macro track points to a **live macro canvas**, not to a folder.
- Clip overlaps are allowed, but "the later one plays"
  (`ofxOceanodeTimelineEvaluation.cpp:648`). No weighted blend of two played
  sequences exists.
- Unfolding (`drawUnfoldedRows`, controller `:3096`) assumes one macro per
  track: one set of inner rows shared by all the track's clips.
- Library macros share one sequence list by folder key
  (`libraryKey`, `ofxOceanodeTimelineMacroSequences.cpp:38`).
- Wave tracks already crossfade overlapping clips: a UI/model precedent.

## 3. Architecture

### 3.1 Routers are published on the Timeline node (decided)

The track owns its engine; no MacroMixer node is involved. Each MMT holds one
`MacroMixerRuntime`, created by the timeline manager with the timeline's canvas
container as parent, and keyed by track id. The track persists the engine's
state (contract, sources, kept-out nodes, default policies) in its own JSON,
through the runtime's existing `saveState()` / `restoreState()`.

The routers reach the patch through the **Timeline** node
(`src/Nodes/timelineTransport.{h,cpp}`, the node that points to one timeline):

- When the selected timeline has an MMT, the node adds a separator labelled
  `ROUTERS` and then one port per router: input routers as inputs, output
  routers as outputs, with the router's name and type.
- Each frame the node writes its input ports into the track's engine
  (`setInputs`, `triggerInput`) and publishes the engine's outputs
  (`outputValue`, `takeOutput`).
- Editing the routers in "Macro Morphing Routers", selecting another timeline,
  or adding / removing an MMT reinstalls the ports. A router that keeps its id
  keeps its cables.
- **Load order.** Node presets and cables load before the timeline does (the
  node already notes this for its timeline reference). So the node saves a copy
  of its router port list in its own preset, installs those ports in
  `loadBeforeConnections` so the cables reconnect, and reconciles with the
  track's contract once the timeline has loaded.
- With no Timeline node pointing at the timeline, the track still plays; its
  outputs go nowhere. The routers window says so.

The port code is the MacroMixer node's today (`installOutputs`, `syncInputs`,
`publish`). It moves into a shared helper so both nodes use it.

**Several Timeline nodes on one timeline (decided).** Every Timeline node that
points at the timeline gets the full `ROUTERS` group, inputs and outputs.

- Outputs: the track's engine is read once per frame and the values are handed
  to every node. Trigger outputs are consumed on read (`takeOutput`), so the
  track reads them once and fans them out; a node never reads the engine
  directly.
- Inputs: an input router has one value in the engine. A node writes its port
  into the engine only when that port's value changes, so the last change from
  any node wins; a node whose port did not change does not overwrite the
  others. Trigger inputs from any node fire.
- The nodes' input ports are not synchronised with each other: a node shows
  the last value it received, which may differ from the engine's.

**One MMT per timeline (decided).** A timeline holds at most one Macro
Morphing Track, so the Timeline node has a single `ROUTERS` group and port
names never collide. "Add Macro Morphing Track" is disabled when the timeline
already has one; `createMorphTrack`, `duplicateTrack`, paste, clip-file import
and encapsulation into a macro refuse to make a second; loading a document
that has two keeps the first as a morph track and reports the other.

Track fields: `isMorphTrack` and the engine state blob.

### 3.2 Engine: declarative, playhead-driven scenes (core)

The track must be able to scrub, jump and loop, so it cannot issue "start"
commands. Each frame it tells the engine what should be true:

```cpp
struct MacroMixerScene {
    std::string origin;            // source id, empty = nothing playing
    std::string target;            // source id, empty = origin alone
    float progress = 0;            // 0..1, already eased, any direction
    bool morph = false;            // Mix or Morph
    bool logBlend = false;
    MacroMixerMorphPolicies morphPolicies;          // this transition
    std::map<std::string, Router> routerOverrides;  // mix policy per router
};
void MacroMixerRuntime::present(const MacroMixerScene&);
```

`present` reconciles the live graphs with the scene:

- origin alone -> one graph; a pending transition is committed (scene moved to
  the target) or cancelled (scene moved back to the origin);
- origin + target -> both prepared, `progress` applied as given;
- a scene that names graphs that are not live -> instantiate (a stall, as
  Start! has today).

Required changes: commit/promotion become explicit instead of `progress >= 1`;
readiness fences report "not ready" (hold progress) instead of moving a clock;
policies arrive per transition instead of being derived from the target id.
The clock-driven `request()` stays and is reimplemented on top of the same
internals, so the node keeps working unchanged in manual mode.

### 3.3 Transition modes and their cost

| Mode | Engine behaviour | Scrubbing |
| --- | --- | --- |
| **Mix** | two graphs, router outputs mixed by policy | stateless in progress: free in both directions |
| **Morph, two graphs** (today's fallback) | destination's controls start from the origin's values, outputs crossfade | reversible below 1; rebuilt when entered from the right |
| **Morph in place** | one graph, nodes matched and controls interpolated | A->B and B->A are not symmetric; commit is irreversible |

Rule for in-place Morph: entering an overlap forwards from clip A reuses A's
graph. Any other entry (backwards from B, a seek into the overlap, a loop wrap)
builds a fresh A graph and prepares A->B on it at the current progress.
Leaving on the right commits, leaving on the left cancels. This is correct but
costs one graph build per non-forward entry.

Mix ships first. In-place Morph is the last phase because it carries the open
risks (section 6).

### 3.4 Source timelines follow their clip

Today a mixer source's timeline runs on its own player. In an MMT it must run
on clip time, as a macro clip does:

- the guest timelines of a track's engine are marked as placed, not self-playing;
- `playedSequences` emits a placement per live MMT clip with the clip's time
  map (start, stretch, offset, repeat), and for MMT tracks emits **both** clips
  inside an overlap instead of "later wins";
- each graph's routers do the blending, so the timeline needs no weighted
  combine in Mix and two-graph Morph. Each sequence only writes to its own
  graph's nodes;
- the canvas id `MacroMixer/<n>` changes on every build, so graph creation,
  promotion and release must invalidate the evaluation indexes and schedule.

### 3.5 Editable source sequences

A Macro Track edits a live macro's sequence. An MMT source is a folder that may
have no live graph at the moment. So the addon keeps one **source document** per
source: the source's timeline list, loaded from the folder at import
(`sequence.json`, else `timeline.json`), held in `macroSequences` under the
existing shared-by-folder key. Every graph built from that source binds to the
same list (as copies of a library macro do), so edits are heard at once and are
not lost when the graph is rebuilt.

Unfolding an MMT shows one header per distinct source, with that source's
tracks and lanes underneath; placements are the clips of that source.

### 3.6 Model

- Track: `isMorphTrack`, the engine state blob (3.1), and reuse of
  `macroUnfolded` / `macroFoldedTimelines`.
- Clip: `morphSource` (source id of the track's engine), and `morphIn`, the transition **into** this
  clip: `{ morph, biPow, logBlend, morphPolicies, routerOverrides }`. Keeping it
  on the later clip matches the node ("policies into that source") and survives
  reordering.
- Invariants, enforced when dragging: at most two clips at any beat; a clip may
  only overlap its neighbours; no clip fully inside another.
- Gap between clips: nothing plays (empty scene). Zero overlap: hard switch.
- Overlap of 4 s is converted to beats with the tempo at the insert point, and
  clamped to half of the shorter clip.
- Default clip length: one pass of the source's first timeline; a source with
  no timeline gets a fixed default (proposed: 8 bars).

## 4. Decisions (accepted 2026-11-21 with "move on with your plan")

1. **Phasing (3.3).** Mix first, two-graph Morph second, in-place Morph last.
2. **Saving edited source timelines (3.5).** Proposed: edits stay in memory,
   marked dirty, and are written to the source folder by an explicit
   "Save Source" item (like "Save Macro" on a macro track), never silently.
   This writes into folders that may be library macros or root presets.
3. **Stall on seek.** Phase 1 accepts a frame stall whenever a scene needs a
   graph that is not live (seek, loop wrap, overlap start). A later phase
   pre-builds the next clip's graph ahead of its start.

## 5. Work packages for agents

Rules for every agent: do not build, compile or run anything (Eloi compiles and
tests). Write regression tests in the repo's existing compiled-test style but
do not run them. Do not commit. Both repos have uncommitted edits (this repo:
`ofxOceanodeTimelineController.cpp`, `ofxOceanodeTimelineLaneEditors.cpp`,
`ofxOceanodeTimelineLfoEditor.cpp`; core: `ofxOceanodeCanvas.cpp` and two test
files): work on top of them, never revert them.

Two writers never share a file; the order below guarantees it.

### Wave 1 (three agents in parallel)

**WP1, core engine: scenes.** Files: `MacroMixerRuntime.{h,cpp}`,
`MacroMixerSource.{h,cpp}`.
- Add `MacroMixerScene` and `present()` as in 3.2, Mix and two-graph Morph only.
- Split commit out of `Graph::applyMorph`; add explicit commit / cancel.
- Clock-free readiness (hold progress while not ready).
- Per-transition morph policies, Log Blend and router mix-policy overrides.
- Reimplement `request()` on the same internals; manual behaviour unchanged.
- Tests: `tests/macro_mixer_*` style. Progress forwards, backwards, jump
  across both ends, scene change to a third source, empty scene.
- Done when: the existing mixer tests need no change and the new ones describe
  the behaviours above.

**WP2, core node: shared GUI and shared ports.** Files:
`ofxOceanodeMacroMixer.{h,cpp}`, new `MacroMixerGui.{h,cpp}`, new
`MacroMixerPorts.{h,cpp}`.
- Move `drawRouters`, `drawMixOptions`, `drawSource` / `drawSourceHeader`,
  `drawMorphPolicies`, `drawDefaultPolicies`, the blocked-node dialog into free
  functions that take a runtime and emit edits through callbacks.
- Move `installOutputs`, `syncInputs`, `publish` into a helper that installs a
  contract's routers as ports on any `ofxOceanodeNodeModel` and syncs them with
  a given runtime; it writes an input only when its port changes, can publish
  outputs from values read by the caller, and keeps cables of routers whose id is
  unchanged.
- The MacroMixer node calls both: no visible change to it.
- Does not touch Runtime or Source files (WP1 owns them).

**WP3, addon model.** Files: `ofxOceanodeTimeline.h`, `ofxOceanodeTimeline.cpp`,
`ofxOceanodeTimelinePersistence.cpp`, `ofxOceanodeTimelineHistory.cpp`.
- Fields from 3.6 (engine state stored as an opaque JSON blob); `createMorphTrack`, `createMorphClip`, `appendMorphClip`
  (4 s overlap), `morphOverlaps(track)` (adjacent pairs with their ranges),
  overlap invariants in `setClipTiming` / resize / trim for morph tracks.
- One morph track per timeline (3.1): refusals in create, duplicate, paste,
  import, encapsulation and load.
- JSON for track and clip fields, preset version bump and upgrade,
  `exportClip` / `importClip`, signatures hash the new fields.
- Refusals as for macro tracks: `addBinding`, `createLane`, `mergeClips`;
  `duplicateTrack`, `duplicateClip`, `splitClip`, `reverseClip` behaviour.
- Tests: new `tests/macro_morphing_track_regression.py` (model, persistence
  round trip, undo, invariants).

### Wave 2 (two agents in parallel, after wave 1)

**WP4, addon playback.** Files: `ofxOceanodeTimelineEvaluation.cpp`,
`ofxOceanodeTimelinePlayers.cpp`, `ofxOceanodeTimelineMacroSequences.cpp`,
`ofxOceanodeTimelineScheduler.cpp`, `src/Nodes/timelineTransport.{h,cpp}`.
- Engines: one `MacroMixerRuntime` per timeline's morph track, created and released with
  the track, restored from and saved to the track's state blob, updated and
  drawn each frame.
- Timeline node: the `ROUTERS` separator and ports as in 3.1, using
  `MacroMixerPorts`; several nodes per timeline (output fan-out read once per
  frame, inputs written on change); saved port list and `loadBeforeConnections`.
- Per frame, for each timeline's morph track: compute the scene at the playhead (clip alone,
  overlap with `progress = shape(position in overlap, biPow)`, or empty) and
  call `present()` on the track's engine. Also while stopped and scrubbing.
- Source documents (3.5): load at import, share by folder key, bind graphs.
- Placed source timelines (3.4): guest timelines of a track's engine do not
  self-play; `playedSequences` emits clip placements, both clips in an overlap.
- Invalidate indexes / schedule when graphs change.
- Muted clip, track mute / solo, loop wrap, reversed clip.
- Tests: extend the new regression file and the pattern of
  `macro_mixer_timeline_regression.py`.

**WP5, addon UI.** Files: `ofxOceanodeTimelineController.{h,cpp}`,
`ofxOceanodeTimelineControllerTimelines.cpp`, `ofxOceanodeTimelineAddon.cpp`.
- "Add Macro Morphing Track" in `drawTimelineActions`: creates an empty track;
  disabled, with the reason, when the timeline already has one.
- Track menu: "Macro Morphing Routers" (non-modal window, pattern of
  `drawClipSettingsWindow`, body from `MacroMixerGui`, edits go to the track's engine and reinstall the
  Timeline node's ports), "Add Morphing Source"
  (folder dialog, import, append clip).
- `drawNodeMenu`: "Add to Timeline Macro Morphing Track" on macro nodes, with
  a submenu of the timelines that have a morph track; uses the macro's saved folder and reports why when
  it cannot (unsaved macro, contract mismatch).
- Clip drawing with the source name; overlap rectangle from `morphOverlaps`.
- Double-click an overlap: transition window (Mix / Morph, Bi Pow, Log Blend,
  morph policies, per-router mix policy). Double-click a clip: source window
  (`MacroMixerGui` source panel: path, checks, kept-out, Refresh).
- Drag code respects the overlap invariants.

### Wave 3 (one agent)

**WP6, unfolded rows.** Files: controller only.
- `drawUnfoldedRows` / `placementsOf` iterate distinct sources of a morph
  track: one header per source, its inner tracks and lanes, placements filtered
  to that source's clips, row keys include the source id.
- "Save Source" item and dirty mark (decision 3).
- Inner rows need a resolver to show targets: use the live graph's container
  when that source is live. Behaviour when it is not live is an open point
  (section 6).

### Wave 4 (one agent, core then addon)

**WP7, in-place Morph.** Entry rule from 3.3 in `present()`; destination
timeline on the origin's live keys (reuse `loadMorphTimelines` and the
automation filter); Details entry in the transition window saying which mode
actually ran and why.

**WP8, pre-build and docs.** Build the next clip's graph ahead of the overlap
(reuse the macro track's pre-roll idea), skip sources with a Global Portal;
update `docs/timeline.md`, `docs/architecture.md`, core `docs/macro-mixer.md`,
and mark the old study as superseded.

Test points for Eloi: after wave 2 (tracks play in Mix, all menus and windows
work), after wave 3 (editing), after wave 4 (Morph in place).

## 6. Open risks

- **Backward time inside a transition** is untested for per-node `morphApply`,
  the SuperCollider bridge processors (gain smoothing) and the texture FadeOut
  renderer. Trigger routers under scrubbing may fire oddly.
- **In-place Morph and timelines.** Today the origin's timelines are replaced
  when the morph begins and the blend goes from a start *value* into the
  destination's timeline. Inside an overlap that means A's automation stops
  moving at the overlap start. A real A-to-B timeline blend is not designed
  here.
- **Editing a source that is not live.** Lane editors may need the live
  parameter (range, type). Not verified. Options: build an idle graph while its
  rows are unfolded, or show those rows read-only.
- **Scheduled paths.** The sample-accurate scheduler sends values ahead of the
  playhead; the automation filter never covers scheduled paths.
- **Same source in two adjacent clips** (A overlapping A) shares one sequence
  and one set of gating state. Proposed: treat as a continuation with no
  transition.
- **Idle pre-built graphs** have side effects: Global Portals publish on load,
  GPU allocation, possibly SC synths.
- **Undo of router and source edits.** They change the engine, whose state is
  a blob in the track; restoring an older blob must rebuild the engine and the
  Timeline node's ports, and removing a router drops its cables, which the
  timeline history cannot bring back.
- **Ports that depend on another object's state.** A Timeline node's ports now
  change when a track is edited elsewhere. Cables are lost when the selected
  timeline changes or the MMT is deleted.
