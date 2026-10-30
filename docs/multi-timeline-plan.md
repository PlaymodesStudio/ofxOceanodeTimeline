# Multi-timeline development plan

Status: plan, not started (written 2026-10-31).
Touches `ofxOceanode` (core transport) and `ofxOceanodeTimeline`, and is checked
against `ofxOceanodeSuperCollider` (the timestamping backend and its box synths).

## Goal

1. Several timelines, at any canvas depth, each played, stopped and seeked
   on its own, at any tempo.
2. Any timeline can be placed in another as a clip, positioned and stretched
   like today's macro clips. The same timeline can be placed many times and
   also play free; edits to it reach every instance.
3. Each timeline has its own tempo: a constant, a ratio of the clock, or a
   tempo automation lane.
4. Several timelines open at once, each in its own docked window.

Non-negotiable: timestamped scheduling (SuperCollider OSC bundles) stays as
exact as today, and a single timeline at one tempo behaves exactly as now.

## Vocabulary

| Term | Meaning |
|---|---|
| **Clock** | One per app. Time only (steady-clock microseconds). Runs internally, frame-stepped, or from an external MIDI clock or Link. Keeps running while every timeline is stopped. |
| **Timeline** | Today's `ofxOceanodeTimelineSequence`, which takes over tempo, loop, markers and time signature from the manager. Owned by a canvas (the root canvas or a macro's). A canvas may own several. |
| **Player** | A running instance of a timeline: start instant on the clock, position, play state, loop pass, tempo mode, generation. Maps clock time to the timeline's beats. Today's main timeline becomes the root player. |
| **Derived transport** | An `ofxOceanodeTransport` that reports a player's beat, BPM, play state and generation instead of owning a clock. Containers inside a free timeline's canvas hand it out from `getTransport()`, so nodes follow their own timeline without changes. |
| **Timeline clip** | Today's macro clip, generalized: it places any eligible timeline in another, with stretch, offset, repeat and reverse. |
| **Instance** | One sounding copy of a timeline: a free player, or one repeat of one timeline clip. Instances roll probability and mutation independently (`instanceKey`). |
| **Time source** | What a transport-synced node follows, chosen in its inspector: Free, Auto (the canvas's default timeline), Clock, or a specific player by name. It replaces the "Sync To Transport" checkbox. |

## Decisions (defaults; confirm the open ones before phase 3)

Settled in discussion:

- **Shared clock, not shared beats.** Every timeline turns clock time into its
  own beats. Positions are always computed as `f(clockTime − startInstant)`,
  never accumulated per frame, so timelines with equal or integer-ratio tempos
  stay phase-locked indefinitely.
- **Every transport action is a future instant.** Play, stop and seek of any
  player land at a timestamp on the clock: the next quantize boundary, or at
  least now + scheduler lookahead. Nothing starts or stops "on this frame".
  This is what keeps the first and last events of a launched timeline as
  exact as the rest.
- **Timeline data is shared, instances are many.** A timeline can be nested
  in several places and played free at the same time. "Make unique"
  duplicates it so one instance can be edited apart from the others.
- **Paths are relative to the owner canvas**, so all instances of a timeline
  drive the same parameters (except library-macro copies, which have their
  own nodes). Overlapping instances on one path go through the conflict rule.
- **Nesting rules:** a timeline can be placed only in a timeline whose canvas
  is the same as its owner canvas or an ancestor of it. No cycles, direct or
  indirect, checked on insert and on load.
- **Nested tempo, first version: follow the host.** A clip stretches the
  child's beats linearly and ignores the child's own tempo lane. The only
  non-linear step is clock time to a player's root beats, so
  `ofxOceanodeTimelineTimeMap` and `then()` stay linear. "Keep own tempo" for
  clips is phase 6.
- **Players belong to the owner instance, not the sequence.** Library-macro
  copies share one sequence and each has its own player.
- **Transport-synced nodes choose their Time source.** Phasor, Counter, Ramp,
  `beatTransport`, `lfoTrack` and the SuperCollider boxes get an inspector
  dropdown in place of "Sync To Transport":
  - Free (unsynced, as today with sync off);
  - Auto (the canvas's default timeline);
  - Clock (always running);
  - **This canvas:** each of its timelines by name;
  - **Other canvases:** free players inside macros.

  Only players can be selected. A timeline that only plays as clips is
  listed greyed out ("only plays as clips"). The choice is saved by id
  (canvas path plus timeline id), so renames don't break it. A deleted
  target shows "missing" and the node stays stopped; it never jumps to
  another timeline. Old patches load sync off as Free and sync on as Auto.
- **Each canvas has a default timeline,** which Auto follows: the main
  timeline at the root, the first one in a macro, changeable in the timeline
  list.
- **`beatTransport` controls the timeline it follows.** It gains Play, Stop,
  Seek and Quantize inputs that act on its selected Time source, so a node
  on one canvas can launch a timeline on another. This settles how a free
  timeline is controlled from the patch; the transport bar in that
  timeline's window does the same.

Open (proposed default in brackets):

1. **Conflict rule** when two playing instances drive one path. [Outer canvas
   over inner, as now (main over macro copies); then an explicit per-timeline
   priority; then the instance that started later. All three are decidable
   for future instants, which the scheduler needs.]
2. **What Auto means inside a placed macro.** A placed macro's canvas can
   hand out a clip-local transport:
   - beat = the clip's local beat;
   - BPM = the parent's BPM ÷ the clip's stretch;
   - playing while the clip plays;
   - a new generation at each clip start.

   A phasor inside a fill macro would then restart and stretch with each
   clip. When clips playing the macro overlap, it falls back to the parent.
   Today these nodes follow the root transport, so this changes behaviour.
   [A per-macro setting "nodes follow: parent / clip", defaulting to parent
   for existing sessions and clip for new macros.]
3. **Where a macro instance's mode (Placed or Free), tempo and quantize are
   saved.** [In the macro instance's own preset data via the extension hooks,
   not in the shared `sequence.json`.]
4. **What the global bar's Play means.** [Root player Play/Stop, as now. The
   clock's own run state is visible only with an external clock.]
5. **Wave tracks on non-root players.** [Phase 3b, after free players work for
   automation, because `ofxOceanodeTimelineWaveAudioProvider` assumes one
   playhead.]

## Where things are today

- **Core transport:** `ofxOceanode/src/Managers/ofxOceanodeTransport.{h,cpp}`
  merges the clock and the song position. A single `state` holds the beat,
  BPM, play state, generation and loop. The root container creates it
  (`ofxOceanodeContainer.cpp:145-157`, `transportOwner`). Macro containers
  copy the host's pointer at construction (`ofxOceanodeNodeMacro.cpp:344`,
  `MacroMixerSource.cpp:239`). `ofxOceanodeTime::update` advances and
  latches it once per frame.
- **Transport consumers**, through `getTransport()`, `getTransportState()` or
  `getFrameTransportState()`:
  - 17 files in `ofxOceanode`: phasor, `beatTransport`, buffer, the canvas
    transport bar, the time controller, `midiClockNode`;
  - 12 in `ofxOceanodeSuperCollider`: metro, rhythm, arpeggiator, chord,
    euclidean and grain boxes;
  - 1 in `ofxOceanodeCustomGui`;
  - 6 in the timeline addon.

  Most read `.current.beatPosition`, `.bpm`, `.isPlaying` and
  `.steadyTimeUs`. If the derived transport keeps the same
  `ofxOceanodeTransportState` shape, they need no change.
- **`midiClockNode.cpp:91`** drives `getTransport()` of its own container.
  Inside a free macro that would be a derived transport, so it must always
  drive the root clock.
- **Scheduling API:** `ofxOceanode/src/Managers/ofxOceanodeScheduling.h`.
  Events carry `dueSteadyTimeUs`, `generation` and `isCorrection`. Times are
  already in the clock's domain.
- **Timeline manager** (`src/Timeline/ofxOceanodeTimeline.h`, class at line
  1061). The main timeline is `mainSequence`, used in 176 places. Main-only
  state on the manager:
  - tempo: `bpmAutomation*`, `bpmMinimum`, `bpmMaximum`, `bpmInterpolation`,
    `tempoSecondsAtPoint` cache, `beatToSeconds`, `secondsToBeat`,
    `beatAfterSeconds`;
  - `markers`;
  - loop: `loopEnabled`, `loopStartBeat`, `loopEndBeat`;
  - `timeSignature*` and `masterGridBeats`;
  - `viewState` and `timebase`;
  - all scheduler state: `scheduledPaths`, the cursor, generation and loop
    counters, `scheduleUsesTempoMap`;
  - `updateMacroGating`.
- **Placements:** `PlayedSequence` and `playedSequences()` (header around
  line 1880), built from `ofxOceanodeTimelineTimeMap` (line 720, linear
  `offset + beat × ratio`).
- **Macro timelines:** `src/Timeline/ofxOceanodeTimelineMacroSequences.cpp`
  (`MacroSequenceEntry`, `getMacroSequence`, `sequenceForCanvas`).
  `PlayedMacro` holds the absolute-path copies used for playback.
- **Scheduler:** `src/Timeline/ofxOceanodeTimelineScheduler.cpp`
  (`runScheduler`, `beatAtOffset`, `sendScheduleCorrections`, which already
  takes `onlyPaths`).
- **Controller:** `src/Controls/ofxOceanodeTimelineController.{h,cpp}` (6.8k
  lines). It is a single instance: `ofxOceanodeTimelineAddon.cpp:218` refuses
  a second one. The scope stack (`scopes`, `rowScope`, `scope()`,
  `editorView()`) already lets it edit any sequence through a time map.
- **Tests:** `tests/*_regression.py` (C++ cases compiled through
  `timeline_regression.py`). Core tests are in `../ofxOceanode/tests` and
  include `transport_regression.py` and `integration_compile.py`.

## Phases

Each phase lands on its own, keeps all existing regressions green, and ends
with `docs/architecture.md` updated. Phases 1 and 2 must not change behaviour.

### Phase 0: Baseline

- Run the full regression set in both addons and `tests/perf_benchmark.py`.
  Record the numbers in this file.
- Add a `multi_timeline_regression.py` fixture (empty cases) so later phases
  have somewhere to put their tests.

**Done when:** baseline recorded, all green.

### Phase 1: Core, splitting the clock from the player (`ofxOceanode`)

1. **Clock.** The root transport keeps a clock time that advances whenever the
   clock runs, independent of `isPlaying`:
   - internal: always;
   - frame-step: fixed steps;
   - external: while the external source runs.

   Expose the clock time and its instant (`steadyTimeUs`). The root transport's
   own beat, play state and loop then become the root player's view, computed
   exactly as today.
2. **Derived transports.** An `ofxOceanodeTransport` constructed with a parent
   clock. Its state is computed from:
   - the parent's clock time;
   - its own start instant, start beat and play state;
   - its own BPM, which the timeline pushes per frame from the tempo lane, the
     same way the root transport is fed today;
   - its own loop and generation.

   It is never integrated across frames. Most of today's API carries over:
   - `seekToBeat`, `setLoop` and `setBpm` apply to that player;
   - external clocks can only be claimed by the root transport.
3. **Timestamped actions.** `playAt(us, beat)`, `stopAt(us)`, `seekAt(us, beat)`
   on any transport, root included. A pending action is visible in the state
   (`pendingStartUs` and similar) so the scheduler can schedule across it. The
   existing immediate calls become "now + minimum lead".
4. **Quantize.** A helper that gives the next boundary instant for a grid
   (bar, beat or a fraction) on another transport's beat grid, or on the
   clock.
5. **Container resolution.** Containers resolve `getTransport()` through an
   override: their own derived transport if set, else their host's.
   Re-pointing a macro's container re-points every nested container that
   inherits. Replace the pointer copies at `ofxOceanodeNodeMacro.cpp:344` and
   `MacroMixerSource.cpp:239`, and check that the container knows its host.
6. **Frame updates.** `ofxOceanodeTime::update` advances and latches every
   derived transport after the root, so frame states are consistent within a
   frame.
7. **`midiClockNode`** drives the root clock (add `getRootTransport()` or
   similar), never a derived transport.
8. **Seconds of play per player.** Counter and Ramp read `.seconds`, not the
   beat. Each derived transport keeps its own seconds with today's rules: set
   from the beat on seeks, stops and loop wraps, accumulating real time while
   that player plays, and continuous through its own tempo changes. The Clock
   time source exposes raw clock seconds.
9. **Transport registry and the Time source helper.**
   - **Registry.** The core keeps a registry of selectable transports:
     stable id (canvas path plus timeline id), label, canvas group, a
     "selectable" flag (players only), and a weak pointer. Each entry also
     carries its time signature and loop range. The core knows nothing about
     timelines; the timeline addon adds and removes entries.
   - **Helper.** In `ofxOceanodeNodeModel`: a node enables a "Time source"
     inspector dropdown with one call. It uses the existing dynamic dropdown
     support in `ofxOceanodeParameter.h`. `getTransportState()` and
     `getFrameTransportState()` then resolve to the selected entry, so node
     code that reads them is unchanged.
   - **Old patches.** The helper also loads the old "Sync To Transport" bool
     as Free or Auto.
10. **Generation per player.** Scheduled events carry the generation of the
   player that produced them. Check how `ofxOceanodeSuperCollider`'s handlers
   use `generation` and make them compare against the right player.

**Tests** (`ofxOceanode/tests/transport_regression.py`):
- the root transport is unchanged;
- a derived player starts at a quantized future instant to the microsecond;
- two derived players at 120/120 and 120/90 keep exact phase after long runs
  (no drift);
- stopping one leaves the other's generation untouched;
- a derived transport's seconds of play follow its own seeks, stops and
  tempo;
- the Time source helper resolves Auto, Clock and a named entry, falls back
  to "missing" when the target goes away, and loads the old bool;
- frame-step and external modes behave the same.

**Done when:** all existing core and SuperCollider-backed tests pass, and a
derived transport works standalone.

### Phase 2: Timeline, moving main-only state into the timeline and adding the player (no behaviour change)

1. Move into `ofxOceanodeTimelineSequence` (or a `TempoMap` struct it owns):
   - tempo lane points and tensions, interpolation, min/max, enabled;
   - the tempo-seconds cache, `beatToSeconds`, `secondsToBeat`,
     `beatAfterSeconds`;
   - markers, loop range, time signature, master grid.

   Macro sequences get these fields with "no tempo lane / no loop" defaults.
2. Add an `ofxOceanodeTimelinePlayer` class:
   - sequence, owner container instance, derived transport;
   - mode: Root, Placed or Free;
   - tempo mode: Ratio, Absolute or Automation;
   - quantize and priority;
   - all of today's scheduler state: `scheduledPaths`, `scheduledBackendPaths`,
     cursor beat and pass, the seen generation and loop count,
     `scheduleUsesTempoMap`, `hasEvaluatedTransportBeat`.

   The manager owns one root player wrapping `mainSequence`.
3. Main-timeline code reads its tempo, loop and markers from the root player
   or its sequence, not the manager. Keep the manager's public getters as
   forwarding shims so the controller and the core extension keep compiling.
4. Undo: `TimelineState` already snapshots tempo and markers. Make the
   snapshot follow the sequence, so a macro scope's undo covers its own tempo
   and loop.
5. Persistence: `timeline.json` is unchanged. `sequence.json` gains optional
   tempo, loop, markers and meter. Bump `kPresetVersion` and load older files
   with the defaults.

**Tests:** every existing regression, unchanged; a round trip of
`sequence.json` with the new fields.

**Done when:** `mainSequence` is reached only through the root player (or
`getMainSequence()`), and behaviour is bit-identical. Use
`time_map_regression`'s identity checks as the model.

### Phase 3: Free players for macro timelines

1. **Mode per macro instance.** Placed (today) or Free. A Free instance gets a
   player with a derived transport, and its macro container is re-pointed to
   it (phase 1, step 5). Save it where open decision 3 says.
2. **Evaluation.** `playedSequences()` yields the placements of every playing
   player:
   - the root, through the identity map;
   - each free player, through the map from its own beat to clock time;
   - their macro clips, as today, composed with `then()`.

   The value walk combines them in the order of the conflict rule. Today's
   rule (main over macro copies) is the first tier.
3. **Scheduler.** `runScheduler` loops over players. Each has its own cursor,
   tempo map and loop pass, and invalidation by generation. A seek, stop or
   edit of one player corrects only the paths that player drives
   (`sendScheduleCorrections(..., onlyPaths)`). Events for one path from
   several players merge in due-time order by the conflict rule. A pending
   start or stop counts as a boundary in the lookahead.
4. **Gating.** `updateMacroGating` measures pre-roll and tail against the
   player that drives each macro clip.
5. **Controls.**
   - In the controller, a free macro's scope shows its own transport bar
     (play, stop, seek, loop, tempo mode, quantize) instead of hiding the
     playhead.
   - The canvas transport bar (`ofxOceanodeCanvas.cpp:998`) already reads
     `container->getTransport()`, so in a free macro's canvas it shows that
     player for free.
   - The `beatTransport` node gains Play, Stop, Seek and Quantize inputs
     acting on its Time source.
6. **Transport-synced nodes switch to the Time source helper:**
   - Phasor (`phasor.{h,cpp}`: its sync mode, cycle output and loop-start
     alignment);
   - Counter and Ramp (`phasor.h`);
   - `beatTransport`, which also reads beats-per-bar and loop from the
     selected entry instead of the main timeline (`beatTransport.cpp:31`
     uses `extension->getBeatsPerBar()` today);
   - `lfoTrack`;
   - the SuperCollider boxes (metro, rhythm, arpeggiator, chord, euclidean,
     grain).

   Check whether the audio-rate phasor (`basePhasor` in
   `ofxOceanodeTime`'s audio thread) extrapolates with a BPM between frames.
   If it does, it must use the selected entry's BPM.
7. **Clip-local transport for placed macros**, as open decision 2 settles.
   The timeline addon registers it with the macro's canvas when the macro
   is in "nodes follow: clip" mode.
8. **Wave tracks per player** (3b). Give the audio provider a playhead per
   player, or limit Wave tracks to root and placed timelines until then.

**Tests:**
- start, stop and seek of a free macro while the root plays and while it is
  stopped;
- timestamps of a quantized launch;
- corrections stay isolated to the player that changed;
- conflict ordering on a shared path;
- `lfoTrack` and a phasor inside a free macro follow its player;
- a phasor on the root canvas set to another timeline follows it, and stops
  when that timeline stops;
- `beatTransport` launches a timeline on another canvas;
- a phasor inside a placed macro in "clip" mode restarts and stretches with
  each clip;
- extend `macro_playback_regression.py`, `schedule_correction_regression.py`
  and `scheduling_jitter_regression.py`.

**Done when:** a macro timeline plays independently, sample-accurately, at
its own tempo, with the root timeline playing or stopped.

### Phase 4: Several timelines per canvas, and timeline clips

1. **Registry.** Generalize `MacroSequenceEntry` to a timeline entry: owner
   container (weak), timeline id, name, sequence, library folder. Each canvas
   has a default timeline (today's main or macro sequence) plus any number of
   named ones.
2. **Macro tracks become timeline tracks.** A clip references
   `{canvas, timelineId}`. On load, a missing id means the canvas's default
   timeline, so existing sessions open unchanged. Gating applies only when the
   placed timeline belongs to a macro and gating is on.
3. **Eligibility and cycles.** Same-or-ancestor canvas only. Run a DFS over
   timeline references on insert, paste, load and encapsulation, and refuse or
   flag a cycle.
4. **Make unique.** Duplicate a timeline and retarget one clip, or every clip
   on a track.
5. **Persistence.** Each canvas folder stores its timelines (`sequence.json`
   stays the default; named ones go in `timelines/<id>.json`). Encapsulation,
   extraction and `retargetAllParameterPaths` cover every timeline of the
   canvas.
6. **UI.** A timeline list per canvas (new, rename, duplicate, delete, play
   free), and dragging a timeline onto a track to create a clip.
7. **Choose the timeline first, then the track.** Every "add to timeline"
   action asks which timeline before it asks which track:
   - **Parameter menu** (`drawParameterMenu` in
     `src/ofxOceanodeTimelineAddon.cpp`): "Add to Timeline" opens a list of
     eligible timelines. These are every timeline owned by the parameter's
     canvas or any ancestor canvas, from the root down to the innermost
     canvas, grouped by canvas and labelled with the canvas path. Each one
     opens its tracks: New Track…, then the existing tracks, skipping
     folders, Wave tracks and timeline tracks, with already-bound ones
     marked. Today the menu offers only the main timeline and the immediate
     enclosing macro's timeline, and misses ancestor macros in between.
   - **Default:** the timeline of the focused timeline window, if it is
     eligible, is listed first and highlighted. If only one timeline is
     eligible, skip the timeline level and keep today's one-click flow.
   - **The binding path is written relative to the chosen timeline's owner
     canvas.** After adding, open that timeline in its window (or focus the
     window already showing it).
   - **Piano roll and instrument setup:** the dialog
     (`requestNoteGroupSetup`) starts with a timeline selector, using the
     same eligibility rule.
   - **Macro / timeline tracks:** "Add to Timeline as Macro Track" asks
     which host timeline. Eligible hosts are the timelines of the macro's
     parent canvas and its ancestors, with the cycle check applied.
   - **"Remove from Timeline"** searches every eligible timeline, not just
     the main one, and labels each entry "Timeline ▸ Track".
8. **Several timelines in the Time source dropdown.** Each canvas marks one
   timeline as its default, and Auto follows it. Every named timeline on a
   canvas registers as a Time source entry while it is a player. A timeline
   that only plays as clips is listed greyed out.

**Tests:**
- two timelines on one canvas playing independently;
- a timeline placed twice and also playing free (three instances, independent
  rolls);
- cycle refusal;
- loading old files;
- encapsulating paths across several timelines.

### Phase 5: Several docked windows

1. Split the controller into:
   - shared services: clipboard, preferences, library, history per sequence;
   - an `ofxOceanodeTimelineWindow` per open window, with its target timeline
     and instance, `scopes`, `rowScope`, zoom, scroll, selection,
     `followPlayhead`, `snapEnabled`, and per-window editor caches.

   Do the extraction mechanically first (one window, no behaviour change),
   then allow several.
2. The addon creates windows on demand ("New Timeline Window", or "Open in
   new window" from a timeline or a macro node) with unique ImGui IDs that
   take part in docking. Window layout and targets are saved with the session.
3. Each window header has a timeline selector and that player's transport.
   The clock (tempo when internal, sync source, external status) stays in one
   global place.
4. Space and other transport shortcuts act on the focused window's player.
5. `viewState` and `timebase` move from the manager to the window.

**Tests:** extend `controller_clarity_regression.py` and
`editor_regression.py`. Two windows on one sequence (edit in one, see it in
the other), two windows on different players, and focus-routed shortcuts.

### Phase 6 (optional): Nested clips that keep their own tempo

- A clip option: Tempo set to Follow host or Own. With Own, the clip maps the
  parent's seconds to the child's seconds (times stretch), and the child
  integrates its own tempo lane.
- `ofxOceanodeTimelineTimeMap` becomes a composition of monotone maps:
  linear segments plus tempo integrals, with inverses. The scheduler inverts
  them with the existing tempo-segment math, falling back to bisection.
- **Tests:** extend `time_map_regression.py` for round trips and nesting
  through non-linear maps, and check timestamps against an analytic reference.

## Timing invariants (check in every phase)

1. All due times are steady-clock µs computed from player start instants. No
   per-frame accumulation feeds a timestamp.
2. Transport actions take effect at timestamped future instants.
3. Invalidation and corrections are per player and per path.
4. Conflicts between instances are resolved identically by the frame path
   and by the scheduler for any future instant.
5. Frame-step and offline renders step every player by the same fixed amount
   and remain deterministic.

## Risks

- **Container transport re-pointing in core.** Macros copy the host's
  pointer today, so anything that cached it (nodes holding a
  `shared_ptr<ofxOceanodeTransport>`, `ofxOceanodeTime`, phasors in their
  thread channel) has to be found and made to resolve dynamically.
- **SuperCollider box synths** anchor on `steadyTimeUs` and the generation. A
  player that stops while the clock runs is a new situation for them. Test
  each box inside a free macro.
- **Controller size.** Phase 5 touches most of a 6.8k-line file. The
  mechanical one-window extraction keeps it reviewable.
- **Performance.** Evaluation and scheduling scale with the number of playing
  players times placements. Run `perf_benchmark.py` at the end of phases 3
  and 4 with, for example, eight free players.
