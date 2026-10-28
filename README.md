# ofxOceanodeTimeline

Optional clips, automation and timeline editing for [ofxOceanode](https://github.com/PlaymodesStudio/ofxOceanode).

Install this folder beside `ofxOceanode` in `openFrameworks/addons`. Use the matching extracted version of Oceanode core, and add `ofxOceanodeTimeline` to your app's `addons.make`. Regenerate existing projects with the openFrameworks project generator to refresh source files, include paths and addon flags.

```cpp
#include "ofxOceanode.h"
#include "ofxOceanodeTimeline.h"

void ofApp::setup() {
    oceanode.setup();
    ofxOceanodeTimeline::setup(oceanode);
    // Register/load your presets after attaching the timeline.
}
```

For a headless host, call `ofxOceanodeTimeline::attach(container)` and register `lfoTrack` yourself if needed; attachment forwards to the root host.

The root container owns the timeline service. Macro containers share that service. Repeating `setup` on the same host does nothing. Without attachment, Oceanode has no timeline window, publishing menus or automation processing. Calling `getTimelineManager()` without attachment throws an explanatory exception; linking code that calls it requires this addon.

The existing manager API stays available through `oceanode.getTimelineManager()` and `container.getTimelineManager()`. Existing `timeline.json` files, clip IDs, binding paths and LFO Track node names are unchanged. Timeline JSON is left untouched when saving a project with core alone. To use projects containing LFO Track nodes, attach this addon before loading them.

Includes timeline model and persistence, portable multi-lane clip files with target remapping, clip/lane editors, parameter publishing menus and badges, piano roll and instrument setup, BPM automation and timebase retiming, LFO Track, wave clips and scheduling lookahead. Shared transport, deterministic random utilities and backend scheduling registration stay in core. Node instrument/value-scale metadata remains available in core so instrument addons do not need a timeline dependency.

See [timeline model usage](docs/timeline.md) and `example-timeline` (generate its project files with the OF project generator).

See [architecture and editor extraction](docs/architecture.md) for component ownership and the path to modular lane editors.

## Verification

From the Oceanode core folder:

```sh
python3 tests/integration_compile.py
python3 tests/integration_compile.py --timeline ../ofxOceanodeTimeline
python3 tests/transport_regression.py
python3 ../ofxOceanodeTimeline/tests/timeline_regression.py
python3 ../ofxOceanodeTimeline/tests/editor_regression.py
python3 ../ofxOceanodeTimeline/tests/clip_file_regression.py
python3 ../ofxOceanodeTimeline/tests/track_duplication_regression.py
python3 ../ofxOceanodeTimeline/tests/target_routing_regression.py
python3 ../ofxOceanodeTimeline/tests/clarity_regression.py
python3 ../ofxOceanodeTimeline/tests/controller_clarity_regression.py
python3 ../ofxOceanodeTimeline/tests/attachment_regression.py
python3 ../ofxOceanodeTimeline/tests/drum_grid_regression.py
python3 ../ofxOceanodeTimeline/tests/drum_duration_adopt_regression.py
```

The runtime regressions use real OF parameters and JSON under ASan/UBSan; the editor suite also runs production editors with headless ImGui. Set `OCEANODE_TEST_OF_ROOT` to a compiled macOS OF tree if needed. `OCEANODE_CORE_ROOT` can override the default sibling core path. These checks do not verify visual appearance or exercise audio hardware.

Drag WAV files from Finder onto the timeline to import audio at the drop position (using the current snap setting). Drop onto a Wave Track to add clips there, or elsewhere in the timeline canvas to create a Wave Track for each file. Clips use the file name and full audio duration. Imports support undo/redo; unsupported or unreadable files are ignored.

On macOS, hovering a WAV file over the timeline shows a blue outline at the snapped drop position, sized to the sound duration, with its file name and length in seconds. The preview disappears when the drag exits or finishes.
