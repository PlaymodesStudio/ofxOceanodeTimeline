#include "ofxOceanodeTimelineController.h"
#include "Editors/ofxOceanodeTimelineEditorUi.h"
#include "Editors/ofxOceanodeTimelinePresentation.h"
#include "Editors/ofxOceanodeTimelineLaneEditorUi.h"

#include "ofxOceanodeContainer.h"
#include "ofxOceanodeParameter.h"
#include "ofxOceanodeNode.h"
#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeNodeMacro.h"
#include "Timeline/ofxOceanodeTimeline.h"
#ifdef OFXOCEANODE_USE_MIDI
#include "Managers/ofxOceanodeMidiClock.h"
#endif
#include "ofxOceanodeTransport.h"
#include "imgui.h"
#ifdef TARGET_GLFW_WINDOW
#include "GLFW/glfw3.h"
#ifdef TARGET_OSX
#define GLFW_EXPOSE_NATIVE_COCOA
#include "GLFW/glfw3native.h"
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#endif

#include <algorithm>
#include <limits>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace {
using namespace ofxOceanodeTimelineLaneEditorUi;

// Lane editors write the open clip in place. Compare what it plays before and
// after they draw, so events already queued ahead of the playhead are
// corrected instead of playing the old content.
class ScheduledContentEditGuard {
public:
    ScheduledContentEditGuard(ofxOceanodeTimelineManager& timeline, std::string trackId, std::string clipId)
    : timeline(timeline), trackId(std::move(trackId)), clipId(std::move(clipId)), before(signature()) {}
    ~ScheduledContentEditGuard() { if(signature() != before) timeline.invalidateSchedule(); }
    ScheduledContentEditGuard(const ScheduledContentEditGuard&) = delete;
    ScheduledContentEditGuard& operator=(const ScheduledContentEditGuard&) = delete;
private:
    uint64_t signature() const {
        const auto* clip = timeline.getClip(trackId, clipId);
        if(clip == nullptr) return 0;
        uint64_t hash = clip->lanes.size();
        for(const auto& lane : clip->lanes)
            hash = hash * 1099511628211ULL ^ ofxOceanodeTimelineManager::lanePlaybackSignature(lane);
        return hash;
    }
    ofxOceanodeTimelineManager& timeline;
    std::string trackId, clipId;
    uint64_t before;
};

#ifdef TARGET_GLFW_WINDOW
struct NativeDropWindow {
    GLFWdropfun previous = nullptr;
    bool previewActive = false;
    std::vector<std::string> previewFiles;
    void* cocoaView = nullptr;
    ImGuiContext* context = nullptr;
    std::map<void*, std::function<void(const std::vector<std::string>&, float, float)>> targets;
};
std::map<GLFWwindow*, NativeDropWindow> nativeDropWindows;
bool forwardingNativeDrop = false;

#ifdef TARGET_OSX
// Observe Cocoa's hover notifications while preserving GLFW's destination
// methods. This stays in C++ so existing projects need no new ObjC++ source.
template<typename R, typename... Args>
R cocoaSend(id object, const char* selector, Args... args) {
    return reinterpret_cast<R (*)(id, SEL, Args...)>(objc_msgSend)(object, sel_registerName(selector), args...);
}
struct CocoaDragMethods { IMP entered = nullptr, updated = nullptr, exited = nullptr, perform = nullptr; };
std::map<Class, CocoaDragMethods> cocoaDragMethods;

NativeDropWindow* cocoaDropTarget(id view) {
    for(auto& entry : nativeDropWindows)
        if(entry.second.cocoaView == view) return &entry.second;
    return nullptr;
}
void updateCocoaDrag(id view, id sender) {
    auto* target = cocoaDropTarget(view);
    if(target == nullptr) return;
    if(!target->previewActive) {
        target->previewFiles.clear();
        id classes = cocoaSend<id>(reinterpret_cast<id>(objc_getClass("NSArray")),
            "arrayWithObject:", reinterpret_cast<id>(objc_getClass("NSURL")));
        id pasteboard = cocoaSend<id>(sender, "draggingPasteboard");
        id urls = cocoaSend<id>(pasteboard, "readObjectsForClasses:options:", classes, static_cast<id>(nullptr));
        const auto count = cocoaSend<unsigned long>(urls, "count");
        for(unsigned long i = 0; i < count; ++i) {
            id url = cocoaSend<id>(urls, "objectAtIndex:", i);
            if(!cocoaSend<BOOL>(url, "isFileURL")) continue;
            const char* path = cocoaSend<const char*>(cocoaSend<id>(url, "path"), "UTF8String");
            if(path != nullptr) target->previewFiles.emplace_back(path);
        }
    }
    target->previewActive = true;
}
unsigned long cocoaDragEntered(id view, SEL selector, id sender) {
    updateCocoaDrag(view, sender);
    const auto original = cocoaDragMethods.at(object_getClass(view)).entered;
    return original ? reinterpret_cast<unsigned long (*)(id, SEL, id)>(original)(view, selector, sender) : 1;
}
unsigned long cocoaDragUpdated(id view, SEL selector, id sender) {
    updateCocoaDrag(view, sender);
    const auto original = cocoaDragMethods.at(object_getClass(view)).updated;
    return original ? reinterpret_cast<unsigned long (*)(id, SEL, id)>(original)(view, selector, sender) :
        cocoaDragEntered(view, sel_registerName("draggingEntered:"), sender);
}
void cocoaDragExited(id view, SEL selector, id sender) {
    if(auto* target = cocoaDropTarget(view)) target->previewActive = false;
    const auto original = cocoaDragMethods.at(object_getClass(view)).exited;
    if(original) reinterpret_cast<void (*)(id, SEL, id)>(original)(view, selector, sender);
}
BOOL cocoaDragPerform(id view, SEL selector, id sender) {
    if(auto* target = cocoaDropTarget(view)) target->previewActive = false;
    const auto original = cocoaDragMethods.at(object_getClass(view)).perform;
    return original ? reinterpret_cast<BOOL (*)(id, SEL, id)>(original)(view, selector, sender) : NO;
}
void observeCocoaDrag(GLFWwindow* window, NativeDropWindow& target) {
    id view = cocoaSend<id>(glfwGetCocoaWindow(window), "contentView");
    target.cocoaView = view;
    Class cls = object_getClass(view);
    if(cocoaDragMethods.count(cls)) return;
    auto replace = [&](const char* name, IMP hook, const char* encoding) {
        SEL selector = sel_registerName(name);
        Method method = class_getInstanceMethod(cls, selector);
        IMP previous = method ? method_getImplementation(method) : nullptr;
        // Adding an override avoids changing an inherited NSView method.
        if(!class_addMethod(cls, selector, hook, method ? method_getTypeEncoding(method) : encoding))
            method_setImplementation(class_getInstanceMethod(cls, selector), hook);
        return previous;
    };
    CocoaDragMethods originals;
    originals.entered = replace("draggingEntered:", reinterpret_cast<IMP>(cocoaDragEntered), "Q@:@");
    originals.updated = replace("draggingUpdated:", reinterpret_cast<IMP>(cocoaDragUpdated), "Q@:@");
    originals.exited = replace("draggingExited:", reinterpret_cast<IMP>(cocoaDragExited), "v@:@");
    originals.perform = replace("performDragOperation:", reinterpret_cast<IMP>(cocoaDragPerform), "c@:@");
    cocoaDragMethods.emplace(cls, originals);
}
#endif


bool dropWindowExists(GLFWwindow* window, ImGuiContext* context) {
    if(ImGui::GetCurrentContext() != context) return false;
    for(const auto* viewport : ImGui::GetPlatformIO().Viewports)
        if(viewport->PlatformHandle == window) return true;
    return false;
}

void nativeAudioDrop(GLFWwindow* window, int count, const char** paths) {
    const auto found = nativeDropWindows.find(window);
    if(found == nativeDropWindows.end()) return;
    // Finder drags need not update ImGui/OF mouse hover or mouse position.
    double x, y;
    int left, top;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowPos(window, &left, &top);
    found->second.previewActive = false;
    const auto previous = found->second.previous;
    auto* savedContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(found->second.context);
    const bool desktopCoordinates = ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable;
    std::vector<std::string> files(paths, paths + count);
    for(const auto& target : found->second.targets)
        target.second(files, x + (desktopCoordinates ? left : 0), y + (desktopCoordinates ? top : 0));
    ImGui::SetCurrentContext(savedContext);
    // Preserve existing application callbacks without importing the same drop
    // again through the fallback openFrameworks event listener.
    if(previous != nullptr) {
        forwardingNativeDrop = true;
        previous(window, count, paths);
        forwardingNativeDrop = false;
    }
}
#endif


// Keep long timelines practical: at the minimum zoom, one hour occupies
// about 900 px while still leaving enough scale for beat-based editing.
constexpr float kMinPixelsPerSecond = ofxOceanodeTimelineEditorView::minPixelsPerSecond;
constexpr float kMaxPixelsPerSecond = 600.0f;
constexpr float kSnapMinimumSpacingPixels = 5.0f;

constexpr float kRulerHeight = 64.0f;
constexpr float kHeaderHeight = 25.0f;
constexpr float kRowHeight = 28.0f;
constexpr float kCollapsedHeight = 38.0f;
constexpr float kWaveTrackMinHeight = 48.0f;
constexpr float kWaveTrackMaxHeight = 360.0f;
constexpr float kWaveTrackResizeHandleHeight = 8.0f;
constexpr float kEdgePixels = 8.0f;
// Parameter rows are indented under their track header by half the clip
// editor's indent (20 px), so the hierarchy reads track > row > editor.
constexpr float kParameterRowIndent = 10.0f;

using ofxOceanodeTimelineCurve::CurveInterpolationMode;
using ofxOceanodeTimelineCurve::curveInterpolationMode;
using ofxOceanodeTimelineCurve::curveSegmentShape;
using ofxOceanodeTimelineCurve::valueAtBeat;
using ofxOceanodeTimelineClipTime::cycleDuration;
using ofxOceanodeTimelineClipTime::sourceDuration;
using ofxOceanodeTimelineClipTime::sourceToTimelineBeat;
using ofxOceanodeTimelineClipTime::stretch;
using ofxOceanodeTimelineClipTime::timelineToSourceBeat;

double nextWiderSnapDivision(double beats, double beatsPerBar) {
    // Keep the musical widening predictable: 16th -> 8th -> quarter ->
    // half -> bar, then double the number of bars each time.
    if(beats < 1.0) return beats * 2.0;
    if(beats < 2.0) return 2.0;
    if(beatsPerBar > beats) return beatsPerBar;
    return beats * 2.0;
}

double adaptiveSnapDivision(double baseBeats, double beatsPerBar,
                            float bpm, float pixelsPerSecond) {
    if(baseBeats <= 0.0) return baseBeats;

    const double pixelsPerBeat = 60.0 / std::max(1.0f, bpm) *
                                 std::max(kMinPixelsPerSecond, pixelsPerSecond);
    double beats = baseBeats;
    while(beats * pixelsPerBeat < kSnapMinimumSpacingPixels) {
        const double wider = nextWiderSnapDivision(beats, std::max(1.0, beatsPerBar));
        if(wider <= beats) break;
        beats = wider;
    }
    return beats;
}

const ofxOceanodeTimelineLane* laneForBinding(const ofxOceanodeTimelineClip& clip, const std::string& bindingId) {
    if(clip.isLfo && clip.lfoOutputBindingId == bindingId)
        return clip.lanes.empty() ? nullptr : &clip.lanes.front();
    for(const auto& lane : clip.lanes) {
        if(std::find(lane.bindingIds.begin(), lane.bindingIds.end(), bindingId) != lane.bindingIds.end()) return &lane;
    }
    return nullptr;
}

const char* laneTypeName(ofxOceanodeTimelineLaneType type) {
    switch(type) {
        case ofxOceanodeTimelineLaneType::PianoRoll: return "Piano Roll";
        case ofxOceanodeTimelineLaneType::Number: return "Number";
        case ofxOceanodeTimelineLaneType::Dropdown: return "Dropdown";
        case ofxOceanodeTimelineLaneType::Curve: return "Curve";
        case ofxOceanodeTimelineLaneType::MultiValue: return "Multi Value";
        case ofxOceanodeTimelineLaneType::MultiSlider: return "Multi Slider";
        case ofxOceanodeTimelineLaneType::MultiGate: return "Multi Gate";
        case ofxOceanodeTimelineLaneType::Wave: return "Wave";
        default: return "Step Sequencer";
    }
}

// Every type the user can pick from a combo or menu in this file, with the
// little index ints those sites use mapped to/from it in one place instead
// of a hand-maintained ternary chain per site.
//
// Two counts, one table. Wave is absent from both: it is a track type whose
// clips carry audio files, and a Wave *lane* no longer exists at all. LFO is only
// reachable through kClipCreationOptionCount, i.e. only where a whole clip
// is being created -- an LFO is a self-contained modulator clip with its own
// oscillator-control lanes, so it is a kind of clip to create (alongside a
// Curve or Step Sequencer one, which is how it reads to the user) rather
// than a type an individual lane can be switched to. Selectors that change
// or add a lane pass kLaneTypeOptionCount and so never offer it.
// Toggle is a Multi Gate with one row: on / off blocks for a single value
// (mute, solo, an effect's on), the default for a bool parameter's clips.
// Dropdown is the default for a dropdown parameter's clips.
constexpr const char* kLaneTypeOptions[] = {
    "Step Sequencer", "Curve", "Piano Roll", "Multi Value", "Multi Slider", "Multi Gate", "Toggle", "Number", "Dropdown", "LFO"
};
constexpr int kToggleOptionIndex = 6;
constexpr int kClipCreationOptionCount = static_cast<int>(sizeof(kLaneTypeOptions) / sizeof(kLaneTypeOptions[0]));
constexpr int kLaneTypeOptionCount = kClipCreationOptionCount - 1;
constexpr int kLfoClipOptionIndex = kClipCreationOptionCount - 1;

const char* contentDescription(int option) {
    static const char* descriptions[] = {
        "Timed steps with per-step probability; bar height edits probability.",
        "Continuous automation points, mapped through the lane's output range.",
        "Notes with pitch, gate and velocity targets; supports shared polyphonic voices.",
        "Held numeric regions, one component row per vector element.",
        "A dense step grid of output values; bar height edits the value.",
        "On/off regions, one component row per vector element.",
        "On/off regions for a single value.",
        "Absolute numeric values held until the next change.",
        "Named parameter options held until the next change.",
        "An oscillator with control envelopes and one output target."
    };
    return descriptions[std::max(0, std::min(option, kClipCreationOptionCount - 1))];
}

ofxOceanodeTimelineLaneType laneTypeFromOptionIndex(int index) {
    switch(index) {
        case 1: return ofxOceanodeTimelineLaneType::Curve;
        case 2: return ofxOceanodeTimelineLaneType::PianoRoll;
        case 3: return ofxOceanodeTimelineLaneType::MultiValue;
        case 4: return ofxOceanodeTimelineLaneType::MultiSlider;
        case 5: return ofxOceanodeTimelineLaneType::MultiGate;
        case 7: return ofxOceanodeTimelineLaneType::Number;
        case 8: return ofxOceanodeTimelineLaneType::Dropdown;
        case kToggleOptionIndex: return ofxOceanodeTimelineLaneType::MultiGate;
        default: return ofxOceanodeTimelineLaneType::Step;
    }
}

bool isToggleLane(const ofxOceanodeTimelineLane& lane) {
    return lane.type == ofxOceanodeTimelineLaneType::MultiGate && lane.multiRowCount <= 1;
}
void makeToggleLane(ofxOceanodeTimelineLane* lane) {
    if(lane == nullptr || lane->type != ofxOceanodeTimelineLaneType::MultiGate) return;
    lane->multiRowCount = 1;
    lane->multiGateRows.resize(1);
}
const char* laneDisplayName(const ofxOceanodeTimelineLane& lane) {
    return isToggleLane(lane) ? "Toggle" : laneTypeName(lane.type);
}

int optionIndexForLaneType(ofxOceanodeTimelineLaneType type) {
    switch(type) {
        case ofxOceanodeTimelineLaneType::Number: return 7;
        case ofxOceanodeTimelineLaneType::Dropdown: return 8;
        case ofxOceanodeTimelineLaneType::Curve: return 1;
        case ofxOceanodeTimelineLaneType::PianoRoll: return 2;
        case ofxOceanodeTimelineLaneType::MultiValue: return 3;
        case ofxOceanodeTimelineLaneType::MultiSlider: return 4;
        case ofxOceanodeTimelineLaneType::MultiGate: return 5;
        default: return 0;
    }
}
int optionIndexForLane(const ofxOceanodeTimelineLane& lane) {
    return isToggleLane(lane) ? kToggleOptionIndex : optionIndexForLaneType(lane.type);
}
// A binding's default clip type: a single value's gate is a Toggle
int optionIndexForBinding(const ofxOceanodeTimelineParameterBinding& binding) {
    const bool vector = binding.valueType.find("vector") != std::string::npos;
    if(binding.laneType == ofxOceanodeTimelineLaneType::MultiGate && !vector) return kToggleOptionIndex;
    return optionIndexForLaneType(binding.laneType);
}

// Thick divider between a track's rows and its docked clip editor.
void drawEditorSeparator(const ofxOceanodeTimelineTrack& track, float contentWidth) {
    ImGui::Dummy(ImVec2(contentWidth, 6.0f));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(12, 12, 14, 255));
    dl->AddRectFilled(ImVec2(min.x, min.y + 2.0f), ImVec2(max.x, min.y + 4.0f),
                      IM_COL32(track.color.r, track.color.g, track.color.b, 230));
}

using ofxOceanodeTimelineEditorUi::finishAbsoluteLayout;
using ofxOceanodeTimelineEditorUi::contextMenuSection;
}

ofxOceanodeTimelineController::ofxOceanodeTimelineController(std::shared_ptr<ofxOceanodeContainer> _container)
: ofxOceanodeBaseController("Timeline"), container(std::move(_container)) {
    // Run before ofxImGui's mouse-capture filter, which can consume OS drops.
    fileDropListener = ofEvents().fileDragEvent.newListener(
        [this](ofDragInfo& info) { filesDropped(info); }, OF_EVENT_ORDER_BEFORE_APP - 1);
}

void ofxOceanodeTimelineController::bindNativeFileDrops() {
#ifdef TARGET_GLFW_WINDOW
    const auto* backend = ImGui::GetIO().BackendPlatformName;
    if(backend == nullptr || std::strstr(backend, "glfw") == nullptr) return;
    auto* window = static_cast<GLFWwindow*>(ImGui::GetWindowViewport()->PlatformHandle);
    if(window == nativeFileDropWindow) return;
    nativeFileDropBinding.reset();
    nativeFileDropWindow = window;
    if(window == nullptr) return;
    auto result = nativeDropWindows.emplace(window, NativeDropWindow{});
    auto& binding = result.first->second;
    if(result.second) {
        binding.context = ImGui::GetCurrentContext();
        binding.previous = glfwSetDropCallback(window, nativeAudioDrop);
#ifdef TARGET_OSX
        observeCocoaDrag(window, binding);
#endif
    }
    binding.targets[this] = [this](const std::vector<std::string>& files, float x, float y) {
        queueAudioDrop(files, x, y);
    };
    nativeFileDropBinding = std::shared_ptr<void>(this, [window](void* owner) {
        const auto entry = nativeDropWindows.find(window);
        if(entry == nativeDropWindows.end()) return;
        entry->second.targets.erase(owner);
        if(entry->second.targets.empty()) {
            if(dropWindowExists(window, entry->second.context))
                glfwSetDropCallback(window, entry->second.previous);
            nativeDropWindows.erase(entry);
        }
    });
#endif
}

void ofxOceanodeTimelineController::filesDropped(ofDragInfo& info) {
#ifdef TARGET_GLFW_WINDOW
    if(forwardingNativeDrop) return;
#endif
    if(ImGui::GetCurrentContext() == nullptr || !fileDropMainViewport) return;
    const auto origin = ImGui::GetMainViewport()->Pos;
    std::vector<std::string> files;
    for(const auto& file : info.files) files.push_back(file.string());
    queueAudioDrop(files, info.position.x + origin.x, info.position.y + origin.y);
}

namespace {
// Read only RIFF metadata for hover feedback; never scan/decode the samples.
double droppedWaveDurationSeconds(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    auto u32 = [&]() {
        unsigned char bytes[4] = {};
        file.read(reinterpret_cast<char*>(bytes), 4);
        return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
               uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    };
    char riff[4] = {}, wave[4] = {};
    file.read(riff, 4); u32(); file.read(wave, 4);
    if(!file || std::memcmp(riff, "RIFF", 4) || std::memcmp(wave, "WAVE", 4)) return 0.0;
    uint32_t sampleRate = 0;
    unsigned channels = 0, bits = 0;
    while(file) {
        char chunk[4] = {};
        file.read(chunk, 4);
        const auto size = u32();
        if(!file) break;
        const auto next = file.tellg() + std::streamoff(size) + std::streamoff(size & 1u);
        if(std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            const auto formatAndChannels = u32();
            channels = formatAndChannels >> 16;
            sampleRate = u32();
            u32();
            bits = u32() >> 16;
        } else if(std::memcmp(chunk, "data", 4) == 0) {
            if(!sampleRate || !channels || channels > 16 ||
               (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return 0.0;
            return double(size / (channels * (bits / 8))) / sampleRate;
        }
        file.seekg(next);
    }
    return 0.0;
}
}

void ofxOceanodeTimelineController::updateAudioDropPreview(
    const std::vector<std::string>& files, float x, float y) {
    if(files != audioDropPreviewPaths) {
        audioDropPreviewPaths = files;
        audioDropPreviewFiles.clear();
        for(const auto& path : files) {
            if(ofToLower(ofFilePath::getFileExt(path)) != "wav") continue;
            const double seconds = droppedWaveDurationSeconds(path);
            if(seconds > 0.0) audioDropPreviewFiles.push_back({path, seconds});
        }
    }
    audioDropPreviewX = x;
    audioDropPreviewY = y;
    audioDropPreviewActive = !audioDropPreviewFiles.empty();
}

void ofxOceanodeTimelineController::updateNativeAudioDropPreview() {
#ifdef TARGET_GLFW_WINDOW
    if(nativeFileDropWindow == nullptr) return;
    auto* window = static_cast<GLFWwindow*>(nativeFileDropWindow);
    const auto found = nativeDropWindows.find(window);
    if(found == nativeDropWindows.end() || !found->second.previewActive) {
        audioDropPreviewActive = false;
        return;
    }
    double x, y;
    int left, top;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowPos(window, &left, &top);
    const bool desktop = ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable;
    updateAudioDropPreview(found->second.previewFiles,
        x + (desktop ? left : 0), y + (desktop ? top : 0));
#endif
}

void ofxOceanodeTimelineController::drawAudioDropPreview(ofxOceanodeTimelineManager& timeline) {
    if(!audioDropPreviewActive || !fileDropBounds.inside(audioDropPreviewX, audioDropPreviewY)) return;
    const double beat = std::max(0.0, snapBeat(pixelsToBeat(timeline,
        audioDropPreviewX - fileDropOriginX, fileDropBpm, fileDropEndBeat)));
    float top = audioDropPreviewY;
    float height = kWaveTrackMinHeight;
    bool newTrack = true;
    for(const auto& row : clipDropRows) {
        const auto* track = timeline.getTrack(row.trackId);
        if(track && track->isWaveTrack && audioDropPreviewY >= row.top && audioDropPreviewY < row.bottom) {
            top = row.top;
            height = row.bottom - row.top;
            newTrack = false;
            break;
        }
    }
    if(newTrack) top = std::max(fileDropBounds.y, std::min(top, fileDropBounds.getBottom() - height));
    auto* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(fileDropBounds.x, fileDropBounds.y),
                    ImVec2(fileDropBounds.getRight(), fileDropBounds.getBottom()), true);
    const float left = fileDropOriginX + beatToPixels(timeline, beat, fileDropBpm);
    for(const auto& file : audioDropPreviewFiles) {
        // Match the exact content duration used by createWaveClipFromFile.
        const double durationBeats = std::max(1.0 / kPPQ, file.durationSeconds * std::max(1.0f, fileDropBpm) / 60.0);
        const float right = fileDropOriginX + beatToPixels(timeline, beat + durationBeats, fileDropBpm);
        const ImVec2 min(left, top + 2), max(right, top + height - 2);
        dl->AddRectFilled(min, max, IM_COL32(100, 210, 255, 28), 3.0f);
        dl->AddRect(min, max, IM_COL32(100, 210, 255, 240), 3.0f, 0, 2.0f);
        const auto label = (newTrack ? "+ Wave Track: " : "") + ofFilePath::getFileName(file.path) +
            "  (" + ofToString(file.durationSeconds, 2) + " s)";
        dl->PushClipRect(min, max, true);
        dl->AddText(ImVec2(left + 6, top + 7), IM_COL32(190, 235, 255, 255), label.c_str());
        dl->PopClipRect();
        if(newTrack) top += height;
    }
    dl->PopClipRect();
}

void ofxOceanodeTimelineController::queueAudioDrop(const std::vector<std::string>& files, float x, float y) {
    audioDropPreviewActive = false;
    if(container == nullptr || ImGui::GetCurrentContext() == nullptr ||
       lastTimelineDrawFrame < ImGui::GetFrameCount() - 1 || !fileDropBounds.inside(x, y)) return;
    AudioFileDrop drop;
    for(const auto& path : files) {
        if(ofToLower(ofFilePath::getFileExt(path)) == "wav") drop.files.push_back(path);
    }
    if(drop.files.empty()) return;
    auto& timeline = container->getTimelineManager();
    drop.beat = std::max(0.0, snapBeat(pixelsToBeat(timeline,
        x - fileDropOriginX, fileDropBpm, fileDropEndBeat)));
    for(const auto& row : clipDropRows) {
        const auto* track = timeline.getTrack(row.trackId);
        if(y >= row.top && y < row.bottom && track != nullptr && track->isWaveTrack) {
            drop.trackId = row.trackId;
            break;
        }
    }
    // Import on the next draw, outside all track/clip iteration.
    pendingAudioDrops.push_back(std::move(drop));
}

void ofxOceanodeTimelineController::importDroppedAudio(ofxOceanodeTimelineManager& timeline) {
    if(pendingAudioDrops.empty()) return;
    auto drops = std::move(pendingAudioDrops);
    pendingAudioDrops.clear();
    timeline.beginTimelineEdit();
    for(const auto& drop : drops) {
        for(const auto& path : drop.files) {
            auto trackId = drop.trackId;
            const bool newTrack = trackId.empty();
            if(newTrack) trackId = timeline.createWaveTrack(ofFilePath::getBaseName(path));
            if(!createWaveClipFromFile(timeline, trackId, path, drop.beat, timeline.getBeatsPerBar())) {
                if(newTrack) timeline.removeTrack(trackId);
                continue;
            }
            auto* track = timeline.getTrack(trackId);
            const auto& clip = track->clips.back();
            timeline.renameClip(trackId, clip.id, ofFilePath::getBaseName(path));
            selectedClips.clear();
            selectedClips.insert({trackId, clip.id});
        }
    }
    timeline.endTimelineEdit();
}

bool ofxOceanodeTimelineController::createWaveClipFromFile(ofxOceanodeTimelineManager& timeline,
                                                            const std::string& trackId,
                                                            const std::string& filePath,
                                                            double startBeat,
                                                            double durationBeats) {
    const auto* track = timeline.getTrack(trackId);
    if(track == nullptr || !track->isWaveTrack || filePath.empty()) return false;

    const auto clipId = timeline.createClip(trackId, "Clip", snapBeat(startBeat),
                                             std::max(1.0 / kPPQ, snapBeat(durationBeats)));
    if(clipId.empty()) return false;

    if(auto* clip = timeline.getClip(trackId, clipId)) {
        clip->waveFilePath = filePath;
        clip->waveGain = 1.0f;
        const bool loaded = timeline.reloadWaveform(trackId, clipId);
        if(!loaded || clip->waveFileDurationMs <= 0.0) {
            timeline.removeClip(trackId, clipId);
            return false;
        }
        if(container != nullptr) {
            const double bpm = std::max(1.0f, container->getTransportState().bpm);
            const double sourceBeats = std::max(1.0 / kPPQ,
                clip->waveFileDurationMs * bpm / 60000.0);
            // Audio clips are content-sized by the selected file. This also
            // gives the envelope lane a meaningful source range instead of
            // leaving every file at the one-bar placeholder length.
            clip->contentDurationBeats = sourceBeats;
            clip->durationBeats = sourceBeats;
            clip->repeatContent = false;
            clip->contentStretch = 1.0;
            clip->waveSourceStartBeat = 0.0;
            clip->waveFileDurationBeats = sourceBeats;
            clip->waveReverse = false;
        }
        return true;
    }
    timeline.removeClip(trackId, clipId);
    return false;
}

bool ofxOceanodeTimelineController::replaceWaveClipFromFile(ofxOceanodeTimelineManager& timeline,
                                                            const std::string& trackId,
                                                            const std::string& clipId,
                                                            const std::string& filePath) {
    auto* track = timeline.getTrack(trackId);
    auto* clip = timeline.getClip(trackId, clipId);
    if(track == nullptr || clip == nullptr || !track->isWaveTrack || filePath.empty()) return false;

    const double visibleDuration = std::max(1.0 / kPPQ, clip->durationBeats);
    clip->waveFilePath = filePath;
    clip->waveGain = std::max(0.0f, clip->waveGain);
    if(!timeline.reloadWaveform(trackId, clipId)) return false;

    const double bpm = container != nullptr
        ? std::max(1.0f, container->getTransportState().bpm) : 120.0;
    const double sourceBeats = std::max(1.0 / kPPQ,
        clip->waveFileDurationMs * bpm / 60000.0);
    clip->contentDurationBeats = sourceBeats;
    clip->durationBeats = visibleDuration;
    clip->repeatContent = false;
    clip->contentStretch = visibleDuration / sourceBeats;
    clip->waveSourceStartBeat = 0.0;
    clip->waveFileDurationBeats = sourceBeats;
    clip->waveReverse = false;
    return true;
}

bool ofxOceanodeTimelineController::trimWaveClipStart(ofxOceanodeTimelineManager& timeline,
                                                      const std::string& trackId, const std::string& clipId,
                                                      double newStartBeat) {
    const auto* track = timeline.getTrack(trackId);
    if(track == nullptr) return false;
    std::set<std::string> before;
    for(const auto& clip : track->clips) before.insert(clip.id);
    if(!splitWaveClipAtPlayhead(timeline, trackId, clipId, newStartBeat)) return false;
    std::string rightId;
    if(const auto* after = timeline.getTrack(trackId))
        for(const auto& clip : after->clips) if(before.count(clip.id) == 0) rightId = clip.id;
    if(rightId.empty()) return false;
    const std::string name = timeline.getClip(trackId, clipId) != nullptr ? timeline.getClip(trackId, clipId)->name : std::string();
    timeline.removeClip(trackId, clipId);
    if(!name.empty()) timeline.renameClip(trackId, rightId, name);
    selectedClips.erase({trackId, clipId});
    selectedClips.insert({trackId, rightId});
    if(editorState.editorTrackId == trackId && editorState.editorClipId == clipId) editorState.editorClipId = rightId;
    return true;
}

bool ofxOceanodeTimelineController::splitWaveClipAtPlayhead(ofxOceanodeTimelineManager& timeline,
                                                            const std::string& trackId,
                                                            const std::string& clipId,
                                                            double playheadBeat) {
    const auto* track = timeline.getTrack(trackId);
    const auto* originalClip = timeline.getClip(trackId, clipId);
    if(track == nullptr || originalClip == nullptr || !track->isWaveTrack ||
       originalClip->waveFilePath.empty()) return false;

    const double epsilon = 1.0 / kPPQ * 0.25;
    const double clipStart = originalClip->startBeat;
    const double clipEnd = clipStart + std::max(1.0 / kPPQ, originalClip->durationBeats);
    if(playheadBeat <= clipStart + epsilon || playheadBeat >= clipEnd - epsilon) return false;

    // A repeated clip can only be represented as two independent source
    // ranges when the cut is in its first cycle. New Wave clips are
    // non-repeating, but this guard avoids silently producing a different
    // arrangement for an older preset that had Repeat content enabled.
    const double localTimelineBeat = playheadBeat - clipStart;
    if(originalClip->repeatContent && localTimelineBeat >= cycleDuration(*originalClip) - epsilon)
        return false;

    const ofxOceanodeTimelineClip original = *originalClip;
    const double sourceLength = std::max(1.0 / kPPQ, original.contentDurationBeats);
    const double sourceSplit = ofClamp(timelineToSourceBeat(original, playheadBeat),
                                       epsilon, sourceLength - epsilon);
    const double leftTimelineDuration = localTimelineBeat;
    const double rightTimelineDuration = clipEnd - playheadBeat;
    if(leftTimelineDuration <= epsilon || rightTimelineDuration <= epsilon) return false;

    // Curve points are stored in source-beat space. Add an interpolated cut
    // point to both halves so a volume envelope remains continuous at the
    // split, then shift the right half back to source beat zero.
    auto splitCurveLane = [&](ofxOceanodeTimelineLane& leftLane,
                              ofxOceanodeTimelineLane& rightLane) {
        if(leftLane.curvePoints.empty()) return;
        std::sort(leftLane.curvePoints.begin(), leftLane.curvePoints.end(),
                  [](const auto& a, const auto& b) { return a.beat < b.beat; });
        const auto points = leftLane.curvePoints;
        const auto tensions = leftLane.curveTensions;
        const float splitValue = valueAtBeat(points, tensions, leftLane.curveInterpolation,
                                             sourceSplit, points.front().value);
        std::vector<ofxOceanodeTimelineCurvePoint> leftPoints;
        std::vector<ofxOceanodeTimelineCurvePoint> rightPoints;
        for(const auto& point : points) {
            if(point.beat < sourceSplit - epsilon) leftPoints.push_back(point);
            if(point.beat > sourceSplit + epsilon)
                rightPoints.push_back({point.beat - sourceSplit, point.value});
        }
        leftPoints.push_back({sourceSplit, splitValue});
        rightPoints.insert(rightPoints.begin(), {0.0, splitValue});
        leftLane.curvePoints = std::move(leftPoints);
        rightLane.curvePoints = std::move(rightPoints);
        resetCurveTensions(leftLane);
        resetCurveTensions(rightLane);
    };

    auto splitLaneData = [&](ofxOceanodeTimelineLane& leftLane,
                             ofxOceanodeTimelineLane& rightLane) {
        if(leftLane.type == ofxOceanodeTimelineLaneType::Curve) {
            splitCurveLane(leftLane, rightLane);
            return;
        }
        // Wave clips currently expose only a Curve volume lane, but preserve
        // the source positions of any future auxiliary lane rather than
        // copying points at the wrong location into the right slice.
        for(auto& note : rightLane.pianoNotes) note.startBeat -= sourceSplit;
        for(auto& step : rightLane.step.steps) step.startBeat -= sourceSplit;
        for(auto& row : rightLane.multiValueRows)
            for(auto& region : row) region.startBeat -= sourceSplit;
        for(auto& row : rightLane.multiGateRows)
            for(auto& region : row) region.startBeat -= sourceSplit;
    };

    // Splitting intentionally dissolves a group: the two new clips are now
    // independent edit targets and should not drag an unrelated slice with
    // them.
    if(timeline.getGroupForClip(trackId, clipId) != nullptr)
        timeline.ungroupClip(trackId, clipId);

    const std::string rightClipId = timeline.createClip(trackId, original.name + " slice",
                                                        playheadBeat, rightTimelineDuration);
    if(rightClipId.empty()) return false;

    auto* rightClip = timeline.getClip(trackId, rightClipId);
    if(rightClip == nullptr) return false;
    rightClip->waveFilePath = original.waveFilePath;
    rightClip->waveGain = original.waveGain;
    rightClip->wavePlaybackRate = original.wavePlaybackRate;
    // For forward playback the right slice begins after the cut. For reverse
    // playback the timeline starts at the high end of the source range, so
    // the left slice occupies the high range and the right slice the low one.
    rightClip->waveSourceStartBeat = original.waveReverse
        ? original.waveSourceStartBeat
        : original.waveSourceStartBeat + sourceSplit;
    rightClip->waveFileDurationBeats = original.waveFileDurationBeats;
    rightClip->waveReverse = original.waveReverse;
    rightClip->waveNumChannels = original.waveNumChannels;
    rightClip->waveFileDurationMs = original.waveFileDurationMs;
    rightClip->waveformPeaks = original.waveformPeaks;
    rightClip->contentDurationBeats = sourceLength - sourceSplit;
    rightClip->contentStretch = original.contentStretch;
    rightClip->repeatContent = false;

    std::vector<ofxOceanodeTimelineLane> leftLanes;
    leftLanes.reserve(original.lanes.size());
    for(const auto& sourceLane : original.lanes) {
        auto leftLane = sourceLane;
        auto rightLane = leftLane;
        splitLaneData(leftLane, rightLane);
        leftLanes.push_back(leftLane);
        const std::string newLaneId = timeline.createLane(trackId, rightClipId,
                                                           leftLane.name, leftLane.type);
        if(newLaneId.empty()) continue;
        if(auto* createdLane = timeline.getLane(trackId, rightClipId, newLaneId)) {
            *createdLane = std::move(rightLane);
            createdLane->id = newLaneId;
        }
    }

    if(auto* leftClip = timeline.getClip(trackId, clipId)) {
        leftClip->durationBeats = leftTimelineDuration;
        leftClip->contentDurationBeats = sourceSplit;
        if(original.waveReverse)
            leftClip->waveSourceStartBeat = original.waveSourceStartBeat + sourceLength - sourceSplit;
        leftClip->repeatContent = false;
        leftClip->lanes = std::move(leftLanes);
    }

    if(auto* sortedTrack = timeline.getTrack(trackId)) {
        std::sort(sortedTrack->clips.begin(), sortedTrack->clips.end(),
                  [](const auto& a, const auto& b) {
                      if(std::abs(a.startBeat - b.startBeat) > 1e-9) return a.startBeat < b.startBeat;
                      return a.id < b.id;
                  });
    }
    // A split creates two new edit targets, but should not leave either slice
    // selected. This prevents an immediate group/drag/delete action from
    // affecting both halves just because the split command was invoked.
    selectedClips.clear();
    return true;
}

double ofxOceanodeTimelineController::getContentEndBeat(const ofxOceanodeTimelineManager& timeline) const {
    const double barBeats = timeline.getBeatsPerBar();
    double endBeat = std::max(barBeats, static_cast<double>(visibleBars) * barBeats);
    if(timeline.isLoopEnabled()) endBeat = std::max(endBeat, timeline.getLoopEndBeat());
    for(const auto& point : timeline.getBpmAutomationPoints()) endBeat = std::max(endBeat, point.beat);
    for(const auto& track : timeline.getTracks()) {
        for(const auto& clip : track.clips) {
            endBeat = std::max(endBeat, clip.startBeat + clip.durationBeats);
        }
    }
    return endBeat;
}

ofxOceanodeTimelineEditorView ofxOceanodeTimelineController::editorView(
    const ofxOceanodeTimelineManager& timeline, float fallbackBpm, double endBeat) const {
    return {timeline, fallbackBpm, endBeat, snapEnabled, effectiveRulerSnapBeats};
}

double ofxOceanodeTimelineController::snapBeat(double beat) const {
    return ofxOceanodeTimelineEditorView::snapBeat(beat, snapEnabled, effectiveRulerSnapBeats);
}

double ofxOceanodeTimelineController::editIncrement() const {
    return effectiveRulerSnapBeats > 0.0 ? effectiveRulerSnapBeats : 1.0 / kPPQ;
}

double ofxOceanodeTimelineController::displayGridBeats() const {
    return effectiveRulerSnapBeats > 0.0 ? effectiveRulerSnapBeats : 1.0;
}

float ofxOceanodeTimelineController::beatToPixels(const ofxOceanodeTimelineManager& timeline,
                                                  double beat, float fallbackBpm) const {
    return editorView(timeline, fallbackBpm, 1.0).beatToPixels(beat);
}

double ofxOceanodeTimelineController::pixelsToBeat(const ofxOceanodeTimelineManager& timeline,
                                                   float pixels, float fallbackBpm,
                                                   double endBeatHint) const {
    return editorView(timeline, fallbackBpm, endBeatHint).pixelsToBeat(pixels);
}

// Shared by every place a binding's blend mode can be changed: the
// per-binding row's own context menu, and the track-header menu (so the
// mode is reachable even while the track is collapsed and that row isn't
// drawn at all).
void ofxOceanodeTimelineController::drawBlendModeOptions(ofxOceanodeTimelineManager& timeline,
                                                         const std::string& trackId,
                                                         const ofxOceanodeTimelineParameterBinding& binding) {
    struct BlendModeOption {
        ofxOceanodeTimelineAutomationMode mode;
        const char* label;
        const char* hint;
    };
    static const BlendModeOption options[] = {
        {ofxOceanodeTimelineAutomationMode::Replace, "Replace",
         "Overrides any other binding driving this parameter"},
        {ofxOceanodeTimelineAutomationMode::Add, "Add",
         "Adds this binding's value to whatever else drives this parameter"},
        {ofxOceanodeTimelineAutomationMode::Multiply, "Multiply",
         "Multiplies this binding's value with whatever else drives this parameter"},
        {ofxOceanodeTimelineAutomationMode::Min, "Min",
         "Keeps the lowest value among all contributors"},
        {ofxOceanodeTimelineAutomationMode::Max, "Max",
         "Keeps the highest value among all contributors"},
    };
    for(const auto& option : options) {
        if(ImGui::MenuItem(option.label, nullptr, binding.mode == option.mode))
            timeline.setBindingMode(trackId, binding.id, option.mode);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.hint);
    }
    ImGui::Separator();
    // Add/Multiply/Min/Max have no natural ceiling of their own -- several
    // bindings combining onto one parameter can drive it past its min/max
    // even though each contributor individually stayed in range. This lives
    // right below the blend modes because it's only ever relevant once one
    // of those non-Replace modes is actually in play. It only takes effect
    // via this parameter's first (non-bypassed) binding, the same one whose
    // value type/default already act as the shared source of truth for it.
    const ofxOceanodeTimelineParameterBinding* authoritative = nullptr;
    std::string authoritativeTrack;
    for(const auto& track : timeline.getTracks()) {
        for(const auto& candidate : track.bindings) {
            if(candidate.parameterPath == binding.parameterPath && !candidate.bypass && authoritative == nullptr) {
                authoritative = &candidate;
                authoritativeTrack = track.id;
            }
        }
    }
    if(ImGui::MenuItem("Clamp final output to parameter range", nullptr,
                      authoritative != nullptr && authoritative->clampToParameterRange, authoritative != nullptr))
        timeline.setBindingClamp(authoritativeTrack, authoritative->id, !authoritative->clampToParameterRange);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Applies once to the final combined output, across all tracks driving this parameter.");
    ImGui::TextWrapped("Within a clip: this mode combines lanes. Overlapping clips: latest start wins. Across tracks: this mode combines the track's contribution with earlier tracks; the first contribution is the base.");
}

const std::vector<std::string>& ofxOceanodeTimelineController::patchTargetPaths() const {
    // Walking every parameter of the patch is too much for every frame an
    // editor is open. Reuse the list while the patch has the same nodes with
    // the same parameter counts (cheap to check), and at most for two seconds
    // (renames, flags).
    const uint64_t now = ofGetElapsedTimeMillis();
    uint64_t fingerprint = 1469598103934665603ULL;
    for(auto* node : container->getAllModules()) {
        fingerprint = (fingerprint ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node))) * 1099511628211ULL;
        if(node != nullptr) fingerprint = (fingerprint ^ node->getParameters().size()) * 1099511628211ULL;
    }
    if(patchTargetPathsValid && fingerprint == patchTargetPathsFingerprint && now - patchTargetPathsTime < 2000)
        return patchTargetPathsCache;
    std::set<std::string> paths;
    const auto& timeline = container->getTimelineManager();
    for(auto* node : container->getAllModules()) {
        if(node == nullptr) continue;
        for(auto& entry : node->getParameters()) {
            auto parameter = std::dynamic_pointer_cast<ofxOceanodeAbstractParameter>(entry);
            if(parameter && timeline.isStepLaneCompatible(*parameter) &&
               !(parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly))
                paths.insert(container->getTimelineParameterPath(*parameter));
        }
    }
    patchTargetPathsCache.assign(paths.begin(), paths.end());
    patchTargetPathsTime = now;
    patchTargetPathsFingerprint = fingerprint;
    patchTargetPathsValid = true;
    return patchTargetPathsCache;
}

void ofxOceanodeTimelineController::beginHistoryGesture(ofxOceanodeTimelineManager& timeline) {
    // This also runs before other windows, so parameter-menu actions such as
    // Add to Timeline are captured even while the timeline window is hidden.
    const auto& io = ImGui::GetIO();
    const bool historyKey = (io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_Z, false);
    if(!historyKey && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) ||
       ImGui::IsAnyItemActive() || ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
       ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_S, false) ||
       ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        timeline.beginTimelineEdit();
}

void ofxOceanodeTimelineController::finishHistoryGesture(ofxOceanodeTimelineManager& timeline) {
    if(!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsAnyItemActive() &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        timeline.endTimelineEdit();
}

void ofxOceanodeTimelineController::restoreHistory(ofxOceanodeTimelineManager& timeline, bool redo) {
    if(!(redo ? timeline.redoTimelineEdit() : timeline.undoTimelineEdit())) return;
    // Every editor may retain indices into the content that was replaced.
    // Keep the open clip but discard selections and unfinished gestures.
    laneEditors = {};
    bpmEditor = {};
    waveVolumeEditor = {};
    lfoEditor = {};
    selectedClips.clear();
    selectedTracks.clear();
    clipDragMode = ClipDragMode::None;
    groupDragSnapshot.clear();
    markerDragId.clear();
    loopDragMode = LoopDragMode::None;
    pendingJump = false;
}

void ofxOceanodeTimelineController::drawBeforeWindows() {
    if(container != nullptr)
        laneEditors.updateMonitoring(container->getTimelineManager(), editorState.clipEditorOpen ? editorState.editorLaneId : "");
    drawPendingTrackPopup();
}

void ofxOceanodeTimelineController::draw() {
    if(container == nullptr) return;
    editorScreenValid = false;
    auto& timeline = container->getTimelineManager();
    lastTimelineDrawFrame = ImGui::GetFrameCount();
    const auto& historyIO = ImGui::GetIO();
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !historyIO.WantTextInput && !ImGui::IsAnyItemActive() &&
       !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !historyIO.KeyAlt &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
       (historyIO.KeyCtrl || historyIO.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        restoreHistory(timeline, historyIO.KeyShift);
    importDroppedAudio(timeline);
    beginHistoryGesture(timeline);
    auto& viewState = timeline.getViewState();
    float& pixelsPerSecond = viewState.pixelsPerSecond;
    float& timelineScrollX = viewState.scrollX;
    auto transportState = container->getTransportState();
    auto transport = container->getTransport();
    const bool externalClock = transport != nullptr && transport->hasExternalClock();
    effectiveRulerSnapBeats = adaptiveSnapDivision(rulerSnapBeats,
                                                   timeline.getBeatsPerBar(),
                                                   transportState.bpm,
                                                   pixelsPerSecond);

    if(transport != nullptr &&
       ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
       ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        transport->togglePlay();
        transportState = container->getTransportState();
    }

    // S is deliberately a timeline-level shortcut rather than a text-field
    // shortcut: it cuts the one selected clip (Wave or automation) at the current transport
    // beat and leaves both resulting slices independently editable.
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
       ImGui::IsKeyPressed(ImGuiKey_S, false) && selectedClips.size() == 1) {
        const auto selected = *selectedClips.begin();
        const auto* selectedTrack = timeline.getTrack(selected.first);
        if(selectedTrack != nullptr && selectedTrack->isWaveTrack)
            splitWaveClipAtPlayhead(timeline, selected.first, selected.second,
                                    transportState.beatPosition);
        else if(selectedTrack != nullptr)
            timeline.splitClip(selected.first, selected.second, transportState.beatPosition);
    }

    // ======================= Toolbar =======================
    // Two rows of grouped controls. Row 1: transport, position, tempo, meter,
    // sync. Row 2: loop, grid, view, output timing, tracks. Groups are split
    // by thin vertical rules; transport and toggles are drawn as icons.
    // Darker panel behind both rows: widgets draw on channel 1, the panel is
    // filled on channel 0 once the rows' height is known.
    auto* toolbarDl = ImGui::GetWindowDrawList();
    toolbarDl->ChannelsSplit(2);
    toolbarDl->ChannelsSetCurrent(1);
    const float toolbarPad = 5.0f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + toolbarPad);
    const float toolbarTop = ImGui::GetCursorScreenPos().y - toolbarPad;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    const float toolbarHeight = ImGui::GetFrameHeight();
    enum class ToolIcon { Play, Pause, Stop, ToStart, Loop, Follow, Fit, ZoomIn, ZoomOut, Dots, Tap, Clock, Magnet, Metronome };
    auto drawIcon = [&](ImDrawList* dl, ToolIcon icon, ImVec2 c, float s, ImU32 col) {
        switch(icon) {
            case ToolIcon::Play:
                dl->AddTriangleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.45f), ImVec2(c.x - s * 0.35f, c.y + s * 0.45f),
                                      ImVec2(c.x + s * 0.45f, c.y), col);
                break;
            case ToolIcon::Pause:
                dl->AddRectFilled(ImVec2(c.x - s * 0.38f, c.y - s * 0.42f), ImVec2(c.x - s * 0.1f, c.y + s * 0.42f), col, 1.0f);
                dl->AddRectFilled(ImVec2(c.x + s * 0.1f, c.y - s * 0.42f), ImVec2(c.x + s * 0.38f, c.y + s * 0.42f), col, 1.0f);
                break;
            case ToolIcon::Stop:
                dl->AddRectFilled(ImVec2(c.x - s * 0.36f, c.y - s * 0.36f), ImVec2(c.x + s * 0.36f, c.y + s * 0.36f), col, 1.5f);
                break;
            case ToolIcon::ToStart:
                dl->AddRectFilled(ImVec2(c.x - s * 0.42f, c.y - s * 0.4f), ImVec2(c.x - s * 0.26f, c.y + s * 0.4f), col);
                dl->AddTriangleFilled(ImVec2(c.x + s * 0.42f, c.y - s * 0.42f), ImVec2(c.x + s * 0.42f, c.y + s * 0.42f),
                                      ImVec2(c.x - s * 0.22f, c.y), col);
                break;
            case ToolIcon::Loop: {
                const float r = s * 0.42f;
                dl->PathArcTo(c, r, 0.35f * 3.14159265f, 1.85f * 3.14159265f, 16);
                dl->PathStroke(col, 0, 1.8f);
                const ImVec2 tip(c.x + r * std::cos(0.35f * 3.14159265f), c.y + r * std::sin(0.35f * 3.14159265f));
                dl->AddTriangleFilled(ImVec2(tip.x + s * 0.2f, tip.y - s * 0.02f), ImVec2(tip.x - s * 0.12f, tip.y - s * 0.18f),
                                      ImVec2(tip.x - s * 0.02f, tip.y + s * 0.2f), col);
                break;
            }
            case ToolIcon::Follow: // playhead line with a chevron pulling it along
                dl->AddLine(ImVec2(c.x - s * 0.3f, c.y - s * 0.45f), ImVec2(c.x - s * 0.3f, c.y + s * 0.45f), col, 1.8f);
                dl->AddLine(ImVec2(c.x - s * 0.02f, c.y - s * 0.28f), ImVec2(c.x + s * 0.28f, c.y), col, 1.8f);
                dl->AddLine(ImVec2(c.x + s * 0.28f, c.y), ImVec2(c.x - s * 0.02f, c.y + s * 0.28f), col, 1.8f);
                break;
            case ToolIcon::Fit: { // four corners
                const float o = s * 0.42f, l = s * 0.2f;
                for(int sx = -1; sx <= 1; sx += 2)
                    for(int sy = -1; sy <= 1; sy += 2) {
                        const ImVec2 k(c.x + sx * o, c.y + sy * o);
                        dl->AddLine(k, ImVec2(k.x - sx * l, k.y), col, 1.6f);
                        dl->AddLine(k, ImVec2(k.x, k.y - sy * l), col, 1.6f);
                    }
                break;
            }
            case ToolIcon::ZoomIn:
                dl->AddLine(ImVec2(c.x - s * 0.36f, c.y), ImVec2(c.x + s * 0.36f, c.y), col, 1.8f);
                dl->AddLine(ImVec2(c.x, c.y - s * 0.36f), ImVec2(c.x, c.y + s * 0.36f), col, 1.8f);
                break;
            case ToolIcon::ZoomOut:
                dl->AddLine(ImVec2(c.x - s * 0.36f, c.y), ImVec2(c.x + s * 0.36f, c.y), col, 1.8f);
                break;
            case ToolIcon::Dots:
                for(int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(c.x + i * s * 0.3f, c.y), s * 0.09f, col);
                break;
            case ToolIcon::Tap:
                dl->AddCircle(c, s * 0.4f, col, 16, 1.6f);
                dl->AddCircleFilled(c, s * 0.16f, col);
                break;
            case ToolIcon::Magnet: { // horseshoe magnet, poles up
                const float r = s * 0.3f;
                const float legTop = c.y - s * 0.42f;
                const float bendY = c.y + s * 0.08f;
                dl->PathArcTo(ImVec2(c.x, bendY), r, 0.0f, 3.14159265f, 14);
                dl->PathStroke(col, 0, s * 0.2f);
                dl->AddLine(ImVec2(c.x - r, bendY), ImVec2(c.x - r, legTop), col, s * 0.2f);
                dl->AddLine(ImVec2(c.x + r, bendY), ImVec2(c.x + r, legTop), col, s * 0.2f);
                // Pole tips.
                const float tip = s * 0.14f;
                dl->AddRectFilled(ImVec2(c.x - r - s * 0.11f, legTop - tip * 0.3f), ImVec2(c.x - r + s * 0.11f, legTop + tip), col);
                dl->AddRectFilled(ImVec2(c.x + r - s * 0.11f, legTop - tip * 0.3f), ImVec2(c.x + r + s * 0.11f, legTop + tip), col);
                break;
            }
            case ToolIcon::Metronome:
                dl->AddQuad(ImVec2(c.x - s * 0.38f, c.y + s * 0.42f),
                            ImVec2(c.x + s * 0.38f, c.y + s * 0.42f),
                            ImVec2(c.x + s * 0.18f, c.y - s * 0.42f),
                            ImVec2(c.x - s * 0.18f, c.y - s * 0.42f), col, 1.5f);
                dl->AddLine(ImVec2(c.x, c.y + s * 0.25f), ImVec2(c.x + s * 0.32f, c.y - s * 0.35f), col, 1.8f);
                dl->AddCircleFilled(ImVec2(c.x + s * 0.23f, c.y - s * 0.18f), s * 0.09f, col);
                break;
            case ToolIcon::Clock:
                dl->AddCircle(c, s * 0.42f, col, 20, 1.6f);
                dl->AddLine(c, ImVec2(c.x, c.y - s * 0.28f), col, 1.6f);
                dl->AddLine(c, ImVec2(c.x + s * 0.2f, c.y + s * 0.08f), col, 1.6f);
                break;
        }
    };
    // Square icon button. `on` tints it with `accent` (toggles, playing).
    auto iconButton = [&](const char* id, ToolIcon icon, const char* tooltip, bool on = false,
                          ImU32 accent = IM_COL32(95, 150, 235, 255), bool enabled = true) {
        const ImVec2 size(toolbarHeight + 2.0f, toolbarHeight);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size) && enabled;
        const bool hovered = ImGui::IsItemHovered() && enabled;
        const bool held = ImGui::IsItemActive() && enabled;
        auto* dl = ImGui::GetWindowDrawList();
        ImU32 bg = on ? accent : ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
        if(on && hovered) bg = (accent & 0x00FFFFFF) | 0xE6000000;
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 3.0f);
        const ImU32 fg = !enabled ? IM_COL32(120, 120, 120, 160) : on ? IM_COL32(15, 18, 24, 255) : IM_COL32(225, 228, 235, 255);
        drawIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), size.y * 0.52f, fg);
        if(tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
        return pressed;
    };
    // Thin vertical rule between groups.
    auto groupSeparator = [&]() {
        ImGui::SameLine(0.0f, 9.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + 2.0f), ImVec2(pos.x, pos.y + toolbarHeight - 2.0f),
                                            IM_COL32(255, 255, 255, 38), 1.0f);
        ImGui::Dummy(ImVec2(1.0f, toolbarHeight));
        ImGui::SameLine(0.0f, 9.0f);
    };
    // Small dim caption in front of a group (kept to a word).
    auto caption = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
        ImGui::SameLine(0.0f, 5.0f);
    };
    // ---- Row 1: transport | position | tempo | meter | sync | timebase ----
    if(transport != nullptr) {
        // Following an external clock: position, play state and tempo come from it.
        const char* lockedTip = "Following an external clock";
        if(iconButton("##tbToStart", ToolIcon::ToStart, externalClock ? lockedTip : "Back to the start", false, 0, !externalClock))
            transport->seekToBeat(0.0);
        ImGui::SameLine(0.0f, 2.0f);
        if(iconButton("##tbStop", ToolIcon::Stop, externalClock ? lockedTip : "Stop (back to the start)", false, 0, !externalClock))
            transport->stop();
        ImGui::SameLine(0.0f, 2.0f);
        if(iconButton("##tbPlay", transportState.isPlaying ? ToolIcon::Pause : ToolIcon::Play,
                      externalClock ? lockedTip : (transportState.isPlaying ? "Pause (Space)" : "Play (Space)"),
                      transportState.isPlaying, IM_COL32(90, 205, 125, 255), !externalClock))
            transport->setIsPlaying(!transportState.isPlaying);
        ImGui::SameLine(0.0f, 2.0f);
        if(iconButton("##tbMetronome", ToolIcon::Metronome,
                      timeline.isMetronomeEnabled() ? "Metronome: on (accent on the first beat of each bar)" : "Metronome: off",
                      timeline.isMetronomeEnabled(), IM_COL32(240, 185, 80, 255)))
            timeline.setMetronomeEnabled(!timeline.isMetronomeEnabled());
        ImGui::SameLine(0.0f, 6.0f);
        {
            bool playOnLoad = timeline.getPlayOnPresetLoad();
            if(ImGui::Checkbox("Play on load##tbPlayOnLoad", &playOnLoad)) timeline.setPlayOnPresetLoad(playOnLoad);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Play on preset load: when this preset is loaded, the timeline plays from the start.\n"
                                  "Saved with the preset (off by default).");
        }

        groupSeparator();
        {
            // Position readout: bar . beat . sixteenth, and the song time.
            const double beatsPerBar = std::max(1e-6, timeline.getBeatsPerBar());
            const double beat = std::max(0.0, transportState.beatPosition);
            const int bar = static_cast<int>(std::floor(beat / beatsPerBar)) + 1;
            const double inBar = beat - (bar - 1) * beatsPerBar;
            const int beatInBar = static_cast<int>(std::floor(inBar)) + 1;
            const int sixteenth = static_cast<int>(std::floor((inBar - std::floor(inBar)) * 4.0)) + 1;
            const double seconds = timeline.beatToSeconds(beat, transportState.bpm);
            const int minutes = static_cast<int>(seconds / 60.0);
            char positionText[32];
            char timeText[32];
            std::snprintf(positionText, sizeof(positionText), "%3d. %d. %d", bar, beatInBar, sixteenth);
            std::snprintf(timeText, sizeof(timeText), "%d:%06.3f", minutes, seconds - minutes * 60.0);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 size(170.0f, toolbarHeight);
            ImGui::Dummy(size);
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(12, 14, 18, 255), 3.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(255, 255, 255, 25), 3.0f);
            const float textY = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(pos.x + 8.0f, textY),
                        transportState.isPlaying ? IM_COL32(120, 235, 160, 255) : IM_COL32(230, 232, 238, 255), positionText);
            const float timeWidth = ImGui::CalcTextSize(timeText).x;
            dl->AddText(ImVec2(pos.x + size.x - timeWidth - 8.0f, textY), IM_COL32(150, 155, 165, 255), timeText);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Bar . beat . sixteenth    song time\nBeat %.3f", beat);
        }

        groupSeparator();
        {
            float bpm = transportState.bpm;
            if(externalClock || timeline.isBpmAutomationEnabled()) {
                // Read-only: the tempo comes from the clock or the tempo lane.
                ImGui::BeginDisabled();
                ImGui::SetNextItemWidth(78.0f);
                ImGui::DragFloat("##tbBpmRead", &bpm, 0.0f, 0.0f, 0.0f, "%.1f BPM");
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(externalClock ? ImVec4(0.45f, 0.95f, 0.5f, 1.0f) : ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                                   externalClock ? "EXT" : "AUTO");
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip(externalClock ? "Tempo from the external clock" : "Tempo from the tempo lane");
            } else {
                ImGui::SetNextItemWidth(78.0f);
                if(ImGui::DragFloat("##tbBpm", &bpm, 0.1f, 1.0f, 999.0f, "%.1f BPM")) container->setBpm(bpm);
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Tempo (drag, or double-click to type)");
                ImGui::SameLine(0.0f, 2.0f);
                // Tap tempo: the average of the last taps; a pause of 2 s starts over.
                if(iconButton("##tbTap", ToolIcon::Tap, "Tap tempo: tap on the beat")) {
                    const double now = ofGetElapsedTimef();
                    if(!tapTimes.empty() && now - tapTimes.back() > 2.0) tapTimes.clear();
                    tapTimes.push_back(now);
                    if(tapTimes.size() > 8) tapTimes.erase(tapTimes.begin());
                    if(tapTimes.size() >= 2) {
                        const double interval = (tapTimes.back() - tapTimes.front()) / (tapTimes.size() - 1);
                        if(interval > 0.0) container->setBpm(ofClamp(static_cast<float>(60.0 / interval), 20.0f, 400.0f));
                    }
                }
            }
        }

        groupSeparator();
        {
            // Meter: numerator is free (any meter is valid), the denominator is
            // a note value so getBeatsPerBar() always lands on a sane grid.
            int tsNumerator = timeline.getTimeSignatureNumerator();
            int tsDenominator = timeline.getTimeSignatureDenominator();
            ImGui::SetNextItemWidth(28.0f);
            if(ImGui::DragInt("##timeSigNum", &tsNumerator, 0.1f, 1, 64))
                timeline.setTimeSignature(tsNumerator, tsDenominator);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Time signature");
            ImGui::SameLine(0.0f, 3.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 3.0f);
            ImGui::SetNextItemWidth(30.0f);
            char tsDenomLabel[8];
            std::snprintf(tsDenomLabel, sizeof(tsDenomLabel), "%d", tsDenominator);
            if(ImGui::BeginCombo("##timeSigDenom", tsDenomLabel, ImGuiComboFlags_NoArrowButton)) {
                for(int option : {1, 2, 4, 8, 16, 32}) {
                    char optionLabel[8];
                    std::snprintf(optionLabel, sizeof(optionLabel), "%d", option);
                    if(ImGui::Selectable(optionLabel, option == tsDenominator))
                        timeline.setTimeSignature(tsNumerator, option);
                }
                ImGui::EndCombo();
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Time signature");
        }
#ifdef OFXOCEANODE_USE_MIDI
        if(auto* clockSync = container->getMidiClockSync()) {
            groupSeparator();
            caption("Sync");
            const char* syncModes[] = {"Internal", "MIDI Clock"};
            int syncMode = container->isMidiClockSyncEnabled() ? 1 : 0;
            ImGui::SetNextItemWidth(92.0f);
            if(ImGui::Combo("##transportSync", &syncMode, syncModes, 2))
                container->setMidiClockSyncEnabled(syncMode == 1);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Internal: Oceanode runs its own transport.\n"
                                  "MIDI Clock: follow an external MIDI clock (play/stop, song position, tempo).");
            ImGui::SameLine(0.0f, 2.0f);
            if(iconButton("##midiClockSyncSettingsButton", ToolIcon::Dots, "MIDI clock port and options"))
                ImGui::OpenPopup("##midiClockSyncSettings");
            // Status: a coloured dot and a word; details in the tooltip.
            ImU32 statusColor = 0;
            std::string statusText;
            std::string statusTip;
            if(container->isMidiClockSyncEnabled()) {
                const auto clockStatus = clockSync->status();
                if(clockSync->getPortName().empty()) {
                    statusColor = IM_COL32(255, 165, 50, 255); statusText = "no port"; statusTip = "Choose a MIDI port in the options (...)";
                } else if(!clockSync->isOpen()) {
                    statusColor = IM_COL32(255, 165, 50, 255); statusText = "offline"; statusTip = "Port not available (retrying): " + clockSync->getPortName();
                } else if(!clockStatus.receiving) {
                    statusColor = IM_COL32(255, 90, 90, 255); statusText = "no clock"; statusTip = "No clock arriving on " + clockSync->getPortName();
                } else {
                    statusColor = IM_COL32(110, 240, 130, 255);
                    statusText = clockStatus.playing ? "play" : "stop";
                    statusTip = clockSync->getPortName() + ": " + ofToString(clockStatus.bpm, 1) + " BPM";
                }
                if(!clockSync->isDrivingTransport()) statusTip += "\n(another source drives the transport)";
            } else if(externalClock) {
                statusColor = IM_COL32(110, 240, 130, 255); statusText = "node"; statusTip = "Transport driven by a MIDI Clock node";
            }
            if(!statusText.empty()) {
                ImGui::SameLine(0.0f, 6.0f);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float textWidth = ImGui::CalcTextSize(statusText.c_str()).x;
                ImGui::Dummy(ImVec2(14.0f + textWidth, toolbarHeight));
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(pos.x + 5.0f, pos.y + toolbarHeight * 0.5f), 4.0f, statusColor);
                dl->AddText(ImVec2(pos.x + 13.0f, pos.y + (toolbarHeight - ImGui::GetTextLineHeight()) * 0.5f),
                            IM_COL32(200, 204, 212, 255), statusText.c_str());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", statusTip.c_str());
            }
            if(ImGui::BeginPopup("##midiClockSyncSettings")) {
                if(ImGui::IsWindowAppearing()) midiPortChoices = ofxOceanodeMidiClock::availablePorts();
                const auto& portChoices = midiPortChoices;
                const std::string currentPort = clockSync->getPortName();
                ImGui::SetNextItemWidth(220.0f);
                if(ImGui::BeginCombo("Port", currentPort.empty() ? "None" : currentPort.c_str())) {
                    if(ImGui::Selectable("None", currentPort.empty())) container->setMidiClockSyncPort("");
                    for(const auto& port : portChoices) {
                        if(ImGui::Selectable(port.c_str(), port == currentPort)) container->setMidiClockSyncPort(port);
                    }
                    ImGui::EndCombo();
                }
                bool reaper = clockSync->core().getReaperMode();
                if(ImGui::Checkbox("REAPER mode", &reaper)) clockSync->core().setReaperMode(reaper);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("A Song Position sent just before Start is honoured\n(start from that position instead of the top).");
                bool clockOnly = clockSync->core().getClockOnly();
                if(ImGui::Checkbox("Clock only", &clockOnly)) clockSync->core().setClockOnly(clockOnly);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("For devices that send clock but no Start/Stop:\nplay while ticks arrive, stop when they stop.");
                bool stopOnLoss = clockSync->core().getStopOnClockLoss();
                if(ImGui::Checkbox("Stop when the clock is lost", &stopOnLoss)) clockSync->core().setStopOnClockLoss(stopOnLoss);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("When ticks stop arriving while playing (cable pulled, master gone):\non = stop after half a second, off = hold at the last position.");
                float offsetMs = clockSync->core().getOffsetMs();
                ImGui::SetNextItemWidth(120.0f);
                if(ImGui::DragFloat("Offset (ms)", &offsetMs, 0.5f, -250.0f, 250.0f, "%.1f"))
                    clockSync->core().setOffsetMs(ofClamp(offsetMs, -250.0f, 250.0f));
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Positive runs ahead of the incoming clock (compensates output latency).");
                int tempoWindow = clockSync->core().getTempoWindow();
                ImGui::SetNextItemWidth(120.0f);
                if(ImGui::DragInt("Tempo window (ticks)", &tempoWindow, 0.2f, 2, 96))
                    clockSync->core().setTempoWindow(tempoWindow);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clock ticks averaged for the tempo estimate (24 = one beat).");
                ImGui::EndPopup();
            }
        }
#endif
        groupSeparator();
        caption("Timebase");
        const char* timebaseModes[] = {"Beats", "Time"};
        int timebaseMode = timeline.getTimebase() == ofxOceanodeTimelineTimebase::Time ? 1 : 0;
        ImGui::SetNextItemWidth(72.0f);
        if(ImGui::Combo("##timelineTimebase", &timebaseMode, timebaseModes, 2))
            timeline.setTimebase(timebaseMode == 1
                ? ofxOceanodeTimelineTimebase::Time
                : ofxOceanodeTimelineTimebase::Beats);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Beats: tempo changes preserve musical positions.\n"
                              "Time: manual tempo changes preserve absolute timing.");
    }

    // ---- Row 2: history | loop | grid | view | output timing | tracks ----
    ImGui::BeginDisabled(!timeline.canUndoTimelineEdit());
    if(ImGui::Button("Undo##timeline")) restoreHistory(timeline, false);
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo timeline action (Cmd/Ctrl+Z)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!timeline.canRedoTimelineEdit());
    if(ImGui::Button("Redo##timeline")) restoreHistory(timeline, true);
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo timeline action (Shift+Cmd/Ctrl+Z)");
    groupSeparator();
    {
        bool loopEnabled = timeline.isLoopEnabled();
        const bool localLoopAvailable = !externalClock;
        const char* loopTip = localLoopAvailable ? (loopEnabled ? "Loop on" : "Loop off")
            : "Local loop unavailable while following an external clock";
        if(iconButton("##tbLoop", ToolIcon::Loop, loopTip,
                      loopEnabled && localLoopAvailable,
                      IM_COL32(120, 135, 240, 255), localLoopAvailable))
            timeline.setLoopEnabled(!loopEnabled);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginDisabled(!loopEnabled || !localLoopAvailable);
        double loopStart = timeline.getLoopStartBeat();
        double loopEnd = timeline.getLoopEndBeat();
        ImGui::SetNextItemWidth(56.0f);
        if(ImGui::InputDouble("##loopStart", &loopStart, 0.0, 0.0, "%.3g"))
            timeline.setLoopRange(snapBeat(loopStart), loopEnd);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Loop start (beats)");
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("-");
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::SetNextItemWidth(56.0f);
        if(ImGui::InputDouble("##loopEnd", &loopEnd, 0.0, 0.0, "%.3g"))
            timeline.setLoopRange(loopStart, std::max(loopStart + 1.0 / kPPQ, snapBeat(loopEnd)));
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Loop end (beats)");
        ImGui::EndDisabled();
    }

    groupSeparator();
    if(iconButton("##tbSnapToggle", ToolIcon::Magnet,
                  snapEnabled ? "Snap to grid: on (clips, markers, loop, playhead)" : "Snap to grid: off",
                  snapEnabled, IM_COL32(235, 95, 95, 255)))
        snapEnabled = !snapEnabled;
    ImGui::SameLine(0.0f, 4.0f);
    {
        int rulerDivision = divisionIndexForBeats(rulerSnapBeats);
        ImGui::SetNextItemWidth(64.0f);
        if(ImGui::BeginCombo("##tbSnap", kDivisionOptions[rulerDivision].label)) {
            for(int i = 0; i < kDivisionOptionCount; ++i) {
                if(ImGui::Selectable(kDivisionOptions[i].label, i == rulerDivision)) {
                    rulerSnapBeats = kDivisionOptions[i].beats;
                    effectiveRulerSnapBeats = adaptiveSnapDivision(rulerSnapBeats,
                                                                   timeline.getBeatsPerBar(),
                                                                   transportState.bpm,
                                                                   pixelsPerSecond);
                }
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Grid and snap division");
    }

    groupSeparator();
    {
        if(iconButton("##tbZoomOut", ToolIcon::ZoomOut, "Zoom out"))
            pixelsPerSecond = ofClamp(pixelsPerSecond / 1.4f, kMinPixelsPerSecond, kMaxPixelsPerSecond);
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(58.0f);
        ImGui::DragFloat("##tbPxPerSec", &pixelsPerSecond, 1.0f, kMinPixelsPerSecond, kMaxPixelsPerSecond, "%.0f px/s");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom: pixels per second (mouse wheel over the timeline also zooms)");
        ImGui::SameLine(0.0f, 2.0f);
        if(iconButton("##tbZoomIn", ToolIcon::ZoomIn, "Zoom in"))
            pixelsPerSecond = ofClamp(pixelsPerSecond * 1.4f, kMinPixelsPerSecond, kMaxPixelsPerSecond);
        ImGui::SameLine(0.0f, 4.0f);
        if(iconButton("##tbFit", ToolIcon::Fit, "Zoom to fit every clip and marker")) requestZoomToFit = true;
        ImGui::SameLine(0.0f, 4.0f);
        if(iconButton("##tbFollow", ToolIcon::Follow, followPlayhead ? "Follow playhead: on" : "Follow playhead: off",
                      followPlayhead, IM_COL32(240, 185, 80, 255)))
            followPlayhead = !followPlayhead;
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::SetNextItemWidth(62.0f);
        ImGui::DragInt("##tbBars", &visibleBars, 0.25f, 1, 256, "%d bars");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Timeline length");
    }

    groupSeparator();
    {
        // Timestamped events: discrete lanes (steps, notes, gates, multi-value
        // blocks) are handed to a backend that can execute them at an exact
        // instant, a lookahead ahead of the playhead. Off sends everything on
        // the frame that crosses it.
        const bool timeStamped = timeline.isSchedulingEnabled();
        if(iconButton("##tbTimeStamped", ToolIcon::Clock,
                      "Timestamped: send steps, notes and gates to SuperCollider with their exact time\n"
                      "(timetagged OSC bundles) instead of on the GUI frame that crosses them.\n"
                      "Curves and parameters without a timestamping backend are unaffected.",
                      timeStamped, IM_COL32(90, 205, 205, 255)))
            timeline.setSchedulingEnabled(!timeStamped);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginDisabled(!timeStamped);
        float lookahead = static_cast<float>(timeline.getSchedulingLookaheadMs());
        ImGui::SetNextItemWidth(78.0f);
        if(ImGui::DragFloat("##schedulingLookahead", &lookahead, 1.0f, 0.0f, 1000.0f, "max %.0f ms"))
            timeline.setSchedulingLookaheadMs(lookahead);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Longest lookahead window (now using %.0f ms).\n"
                              "The window follows recent frame intervals (3 frames + 10 ms, at least 50 ms)\n"
                              "up to this limit. Longer survives bigger GUI stalls; shorter leaves less\n"
                              "already-sent content that stop, seek or edits can only correct.",
                              timeline.effectiveSchedulingLookaheadMs());
        ImGui::EndDisabled();
    }

    groupSeparator();
    // A Wave Track is created together with its first audio clip. Selecting
    // the file here avoids leaving an empty track behind and makes the
    // waveform visible immediately after the track is added.
    if(ImGui::Button("+ Wave Track")) {
        waveFileRequest = WaveFileRequest::NewTrack;
        waveFileRequestBeat = transportState.beatPosition;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New track with an audio file");
    auto sameLineIfRoom = [&](float width) {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        if(ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right) ImGui::SameLine();
    };
    sameLineIfRoom(ImGui::CalcTextSize("Load clip...").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button("Load clip...")) {
        clipFileRequest = ClipFileRequest::Load;
        clipFileTrackId = selectedTracks.size() == 1 ? *selectedTracks.begin() : std::string();
        clipImportBeat = snapBeat(transportState.beatPosition);
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Recall a saved clip with all its lanes and choose new parameter targets.");
    ImGui::PopStyleVar(3);
    {
        // Stop just short of the next row (the style's item spacing below the rows).
        const float bottom = ImGui::GetItemRectMax().y + std::max(1.0f, ImGui::GetStyle().ItemSpacing.y - 1.0f);
        const float left = ImGui::GetWindowPos().x;
        const float right = left + ImGui::GetWindowWidth();
        toolbarDl->ChannelsSetCurrent(0);
        toolbarDl->AddRectFilled(ImVec2(left, toolbarTop), ImVec2(right, bottom), IM_COL32(17, 18, 21, 255));
        toolbarDl->AddLine(ImVec2(left, bottom), ImVec2(right, bottom), IM_COL32(0, 0, 0, 200), 1.0f);
        toolbarDl->ChannelsMerge();
    }

    // Stale entries can accumulate in selectedClips if their clip was
    // removed some other way (e.g. loading a different preset) without
    // going through this controller's own delete paths below -- filter
    // them out here, once per frame, before anything reads the selection.
    for(auto it = selectedClips.begin(); it != selectedClips.end();) {
        if(timeline.getClip(it->first, it->second) == nullptr) it = selectedClips.erase(it);
        else ++it;
    }
    for(auto it = selectedTracks.begin(); it != selectedTracks.end();) {
        if(timeline.getTrack(*it) == nullptr) it = selectedTracks.erase(it);
        else ++it;
    }
    // Delete acts on the timeline selection, but only while the Timeline
    // window owns keyboard focus and no text/widget is editing. Defer the
    // actual removals until after the track iteration below, just like the
    // context-menu delete path, because removing a clip can reallocate its
    // track's clip vector.
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
       !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
       !selectedClips.empty() && !lastClickInEditor && // in the editor, Delete edits the clip's content
       !(editorState.clipEditorOpen && laneEditors.hasNoteSelection(editorState.editorLaneId)) &&
       !(editorState.clipEditorOpen && lfoEditor.hasPointSelection(editorState.editorClipId)) && // Delete removes the selected LFO point instead

       (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        keyboardClipDeletionRequests.assign(selectedClips.begin(), selectedClips.end());
        selectedClips.clear();
    }
    // Grouping/ungrouping is reachable two ways, per the request that
    // prompted this: a toolbar button here for whatever is currently
    // Shift+click-selected, and (see drawClipMenu below) a right-click
    // menu item on the clip itself.
    if(selectedClips.size() >= 2) {
        ImGui::SameLine();
        const std::string label = "Group clips (" + ofToString(selectedClips.size()) + ")##groupSelectedClips";
        if(ImGui::Button(label.c_str())) {
            timeline.groupClips(std::vector<std::pair<std::string, std::string>>(
                selectedClips.begin(), selectedClips.end()));
            // Selection is left as-is (not cleared) -- the clips just
            // grouped are exactly what's still highlighted, which is the
            // most useful confirmation that the group now contains them.
        }
    } else if(selectedClips.size() == 1 && timeline.getGroupForClip(selectedClips.begin()->first, selectedClips.begin()->second) != nullptr) {
        ImGui::SameLine();
        if(ImGui::Button("Ungroup clips##ungroupSelectedClip")) {
            timeline.ungroupClip(selectedClips.begin()->first, selectedClips.begin()->second);
        }
    }

    ImGui::TextUnformatted("View"); ImGui::SameLine();
    if(ImGui::RadioButton("Clips", !targetsView)) targetsView = false;
    ImGui::SameLine();
    if(ImGui::RadioButton("Targets", targetsView)) targetsView = true;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Parameter destinations. A clip may appear on several rows; all appearances refer to the same clip.");
    sameLineIfRoom(220.0f);
    ImGui::SetNextItemWidth(140.0f);
    const char* edgeTools[] = {"Repeat", "Trim / extend", "Stretch"};
    ImGui::Combo("Right edge", &endEditTool, edgeTools, 3);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Also used by Timeline length fields. Audio always stretches its full source slice.\nShift overrides with Trim / extend; Cmd/Ctrl overrides with Stretch.");
    sameLineIfRoom(ImGui::CalcTextSize("What drives this target?").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button("What drives this target?")) targetInspectorOpen = !targetInspectorOpen;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Inspect lanes, winning clips, cross-track blending and final output at the playhead.");
    if(editorState.clipEditorOpen) {
        const auto* track = timeline.getTrack(editorState.editorTrackId);
        const auto* clip = timeline.getClip(editorState.editorTrackId, editorState.editorClipId);
        const auto* lane = timeline.getLane(editorState.editorTrackId, editorState.editorClipId, editorState.editorLaneId);
        if(track != nullptr && clip != nullptr) {
            const std::string scope = lane == nullptr || track->isWaveTrack ? std::string()
                : std::string(clip->isLfo ? " > Control: " : " > Lane: ") + lane->name;
            ImGui::TextWrapped("Editing: Track: %s > Clip: %s%s", track->name.c_str(), clip->name.c_str(), scope.c_str());
        }
    }
    if(targetInspectorOpen) drawTargetInspector(timeline);

    const double endBeat = getContentEndBeat(timeline);
    const float availableWidth = std::max(320.0f, ImGui::GetContentRegionAvail().x);
    if(requestZoomToFit) {
        requestZoomToFit = false;
        double lastBeat = 0.0;
        for(const auto& fitTrack : timeline.getTracks())
            for(const auto& fitClip : fitTrack.clips)
                lastBeat = std::max(lastBeat, fitClip.startBeat + fitClip.durationBeats);
        for(const auto& marker : timeline.getMarkers()) lastBeat = std::max(lastBeat, marker.beat);
        if(lastBeat <= 0.0) lastBeat = timeline.getBeatsPerBar() * 4.0;
        const double seconds = std::max(0.001, timeline.beatToSeconds(lastBeat, transportState.bpm));
        const float usable = std::max(40.0f, availableWidth - kLabelWidth - 24.0f);
        pixelsPerSecond = ofClamp(static_cast<float>(usable / seconds), kMinPixelsPerSecond, kMaxPixelsPerSecond);
        timelineScrollX = 0.0f;
    }
    const float timelineWidth = std::max(availableWidth - kLabelWidth,
                                         beatToPixels(timeline, endBeat, transportState.bpm));
    const float contentWidth = kLabelWidth + timelineWidth;
    auto beatOffset = [&](double beat) { return beatToPixels(timeline, beat, transportState.bpm); };
    auto beatAtOffset = [&](float pixels) { return pixelsToBeat(timeline, pixels, transportState.bpm, endBeat); };

    // Horizontal scrolling is fully manual (timelineScrollX) rather than a
    // native ImGui ScrollX, specifically so the label/properties column (the
    // first kLabelWidth pixels of every row) can stay visually pinned while
    // only the beat-based content to its right slides. A native ScrollX
    // would shift the whole child uniformly, dragging the labels off-screen
    // together with the timeline content.
    const float scrollbarHeight = ImGui::GetStyle().ScrollbarSize;
    const float maxTimelineScrollX = std::max(0.0f, timelineWidth - (availableWidth - kLabelWidth));
    if(followPlayhead && transportState.isPlaying) {
        // Page along with the playhead: when it nears the right edge (or is
        // off screen to the left), bring it back near the left side.
        const float visibleWidth = std::max(1.0f, availableWidth - kLabelWidth);
        const float playheadX = beatOffset(transportState.beatPosition);
        if(playheadX > timelineScrollX + visibleWidth * 0.9f || playheadX < timelineScrollX)
            timelineScrollX = playheadX - visibleWidth * 0.1f;
    }
    timelineScrollX = ofClamp(timelineScrollX, 0.0f, maxTimelineScrollX);
    // Mouse wheel over the timeline zooms around the mouse; over the label column
    // it scrolls vertically (tracks area only). Shared by the pinned ruler strip
    // and the scrolling tracks viewport; call inside the child it applies to.
    auto handleTimelineWheel = [&](bool allowVerticalScroll) {
        if(!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
        const bool zoomGesture = std::abs(ImGui::GetIO().MouseWheel) > 0.001f;
        if(zoomGesture) {
            const float oldPixelsPerSecond = pixelsPerSecond;
            const float mouseViewportX = ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x;
            if(mouseViewportX >= kLabelWidth) {
                const float secondsAtMouse = std::max(0.0f, timelineScrollX + mouseViewportX - kLabelWidth) /
                                             std::max(kMinPixelsPerSecond, oldPixelsPerSecond);
                const float zoomFactor = static_cast<float>(std::pow(1.12f, ImGui::GetIO().MouseWheel));
                pixelsPerSecond = ofClamp(oldPixelsPerSecond * zoomFactor,
                                          kMinPixelsPerSecond, kMaxPixelsPerSecond);
                timelineScrollX = ofClamp(kLabelWidth + secondsAtMouse * pixelsPerSecond - mouseViewportX,
                                          0.0f, maxTimelineScrollX);
            } else if(allowVerticalScroll) {
                ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * 55.0f));
            }
        }
        if(!zoomGesture && std::abs(ImGui::GetIO().MouseWheelH) > 0.001f)
            timelineScrollX = ofClamp(timelineScrollX - ImGui::GetIO().MouseWheelH * 55.0f, 0.0f, maxTimelineScrollX);
    };

    // The ruler (seconds / beats) lives in its own non-scrolling strip so it stays
    // pinned at the top while the tracks below scroll vertically.
    ImGui::BeginChild("##TimelineRulerStrip", ImVec2(0, kRulerHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    handleTimelineWheel(false);
    effectiveRulerSnapBeats = adaptiveSnapDivision(rulerSnapBeats,
                                                   timeline.getBeatsPerBar(),
                                                   transportState.bpm,
                                                   pixelsPerSecond);
    drawRuler(timeline, kLabelWidth, timelineWidth, endBeat, transportState.beatPosition, transportState.bpm);
    ImGui::EndChild();

    ImGui::BeginChild("##TimelineViewport", ImVec2(0, -scrollbarHeight), false,
                      ImGuiWindowFlags_NoScrollWithMouse);
    const float zoneLeft = ImGui::GetWindowPos().x + kLabelWidth;
    const auto dropWindowPos = ImGui::GetWindowPos();
    const auto dropWindowSize = ImGui::GetWindowSize();
    fileDropBounds.set(zoneLeft, dropWindowPos.y,
        std::max(0.0f, dropWindowSize.x - kLabelWidth - ImGui::GetStyle().ScrollbarSize),
        dropWindowSize.y);
    fileDropMainViewport = ImGui::GetWindowViewport() == ImGui::GetMainViewport();
    bindNativeFileDrops();
    handleTimelineWheel(true);
    // The wheel gesture may have changed the scale this frame. Refresh the
    // derived snap before handling timeline input.
    effectiveRulerSnapBeats = adaptiveSnapDivision(rulerSnapBeats,
                                                   timeline.getBeatsPerBar(),
                                                   transportState.bpm,
                                                   pixelsPerSecond);
    fileDropOriginX = zoneLeft - timelineScrollX;
    fileDropBpm = transportState.bpm;
    fileDropEndBeat = endBeat;
    if(timeline.isBpmLaneVisible())
        bpmEditor.draw(timeline, *container, editorView(timeline, transportState.bpm, endBeat),
                       contentWidth, transportState.beatPosition);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Reset once per frame, before any track's handleClip can set it --
    // unlike the per-track clipInteractionClaimedThisFrame below, this one
    // has to survive across every track's iteration so the "clicked empty
    // space" deselect check after the loop knows whether *any* clip on
    // *any* track already claimed this frame's click.
    anyClipInteractionClaimedThisFrame = false;
    bool emptyTrackAreaClickedThisFrame = false;
    bool trackPadClickedThisFrame = false;
    clipScreenRects.clear(); // refilled by drawClip below, for box selection
    clipDropRows.clear();    // refilled per clip row below, for Cmd+drag duplicate

    for(const auto& track : timeline.getTracks()) {
        const float headerY = ImGui::GetCursorPosY();
        // Keep layout state immutable for this frame. Clicking collapse mutates
        // the model, but mixing the old Dummy height with the new branch later
        // in this loop makes SetCursorPos extend the child and asserts in End().
        const bool trackCollapsed = track.collapsed;
        // A Wave Track has no per-binding rows to expand into (see
        // ofxOceanodeTimelineTrack::isWaveTrack) -- it always renders as the
        // same single always-visible clip row a collapsed track would use,
        // regardless of its own collapsed flag.
        const bool singleRowTrack = !targetsView || trackCollapsed || track.isWaveTrack;
        const float trackHeaderHeight = track.isWaveTrack
            ? ofClamp(track.waveTrackHeight, kWaveTrackMinHeight, kWaveTrackMaxHeight)
            : (singleRowTrack ? (!targetsView ? 80.0f : kCollapsedHeight) : kHeaderHeight);
        bool expandTrackForEditor = false;
        ImGui::SetCursorPos(ImVec2(0, headerY));
        // Keep the header as a visual/layout item only. A full-width
        // InvisibleButton steals the overlap from the color button on some
        // ImGui versions, making the picker look permanently disabled.
        ImGui::Dummy(ImVec2(contentWidth, trackHeaderHeight));
        const ImVec2 headerMin = ImGui::GetItemRectMin();
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        if(singleRowTrack) {
            dl->AddRectFilled(headerMin, headerMax, mutedTrackColor(track.color, 0.17f));
            dl->AddRectFilled(headerMin, ImVec2(headerMin.x + kLabelWidth, headerMax.y),
                              mutedTrackColor(track.color, 0.27f, 0.42f));
        } else {
            dl->AddRectFilled(headerMin, headerMax, mutedTrackColor(track.color, 0.27f, 0.42f));
        }
        const float headerTextY = headerMin.y + (trackHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f;
        // A Wave Track has nothing to expand/collapse, so it gets no
        // chevron -- just its name, flush where the chevron would be.
        if(targetsView && !track.isWaveTrack)
            dl->AddText(ImVec2(headerMin.x + 7, headerTextY), IM_COL32(235, 235, 235, 255), trackCollapsed ? ">" : "v");
        dl->PushClipRect(headerMin, ImVec2(headerMin.x + kLabelWidth - 85, headerMax.y), true);
        dl->AddText(ImVec2(headerMin.x + 22, headerTextY), IM_COL32(235, 235, 235, 255), track.name.c_str());
        dl->PopClipRect();
        // A binding's target parameter can disappear (node deleted, preset
        // loaded on a different graph, ...). The manager keeps the binding
        // and its data instead of dropping it, but that's invisible unless
        // the editor surfaces it somewhere - otherwise automation silently
        // stops applying with no clue why.
        const bool hasMissingTarget = std::any_of(track.bindings.begin(), track.bindings.end(),
            [](const auto& binding) { return binding.missingTarget && !binding.bypass; });
        if(hasMissingTarget) {
            const float nameWidth = ImGui::CalcTextSize(track.name.c_str()).x;
            dl->AddText(ImVec2(headerMin.x + 22 + nameWidth + 6, headerTextY),
                       IM_COL32(255, 165, 60, 255), "[missing target]");
        }

        const bool trackPadHovered = hoverRect(headerMin, ImVec2(headerMin.x + kLabelWidth - 85, headerMax.y));
        if(trackPadHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            trackPadClickedThisFrame = true;
            selectedClips.clear();
            const auto& io = ImGui::GetIO();
            if(io.KeyShift || io.KeyCtrl || io.KeySuper) {
                if(selectedTracks.count(track.id)) selectedTracks.erase(track.id);
                else selectedTracks.insert(track.id);
            } else {
                selectedTracks = {track.id};
            }
        }
        if(trackPadHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            selectedClips.clear();
            if(!selectedTracks.count(track.id)) selectedTracks = {track.id};
        }
        if(trackPadHovered && !ImGui::IsAnyItemActive())
            ImGui::SetTooltip("Select track. Shift or Cmd/Ctrl-click to toggle multiple tracks.\nDouble-click to rename. Right-click for track actions.");
        if(selectedTracks.count(track.id)) {
            const ImVec2 padMax(headerMin.x + kLabelWidth, headerMax.y);
            dl->AddRectFilled(headerMin, padMax, IM_COL32(120, 170, 255, 35));
            dl->AddRect(ImVec2(headerMin.x + 1, headerMin.y + 1),
                        ImVec2(headerMax.x - 1, headerMax.y - 1), IM_COL32(150, 190, 255, 255), 0.0f, 0, 2.0f);
            dl->AddLine(ImVec2(padMax.x - 1, headerMin.y + 1),
                        ImVec2(padMax.x - 1, headerMax.y - 1), IM_COL32(150, 190, 255, 255), 2.0f);
        }

        const ImVec2 headerInteractionMax(headerMin.x + kLabelWidth - 85,
                                          headerMax.y);
        if(hoverRect(headerMin, headerInteractionMax)) {
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                pendingTrackId = track.id;
                pendingNewTrackDialog = false;
                std::strncpy(pendingTrackName, track.name.c_str(), sizeof(pendingTrackName) - 1);
                pendingTrackName[sizeof(pendingTrackName) - 1] = '\0';
                requestRenamePopup = true;
            } else if(targetsView && !track.isWaveTrack && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.x < headerMin.x + 22) {
                if(auto* editTrack = timeline.getTrack(track.id)) editTrack->collapsed = !trackCollapsed;
            } else if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                pendingTrackId = track.id;
                pendingWaveClipBeat = transportState.beatPosition;
                ImGui::OpenPopup(("##trackMenu" + track.id).c_str());
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(headerMin.x + kLabelWidth - 82, headerMin.y + (trackHeaderHeight - 19.0f) * 0.5f));
        if(playbackToggle("trackMute" + track.id, "M", track.muted, "Mute track"))
            timeline.setTrackMuted(track.id, !track.muted);
        ImGui::SameLine();
        if(playbackToggle("trackSolo" + track.id, "S", track.solo, "Solo track (multiple solos allowed)"))
            timeline.setTrackSolo(track.id, !track.solo);

        // Use a ColorButton + explicit picker popup. ColorEdit4 with a local
        // temporary swatch was easy to click past and appeared non-persistent.
        ImGui::SetCursorPos(ImVec2(kLabelWidth - 28, headerY + (trackHeaderHeight - 19.0f) * 0.5f));
        ImGui::SetNextItemAllowOverlap();
        const ImVec4 color(track.color.r / 255.0f, track.color.g / 255.0f, track.color.b / 255.0f, track.color.a / 255.0f);
        const std::string colorButtonId = "##trackColorButton" + track.id;
        if(ImGui::ColorButton(colorButtonId.c_str(), color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 19))) ImGui::OpenPopup(("##trackColorPopup" + track.id).c_str());
        if(ImGui::BeginPopup(("##trackColorPopup" + track.id).c_str())) {
            float rgba[4] = {color.x, color.y, color.z, color.w};
            if(ImGui::ColorPicker4("##picker", rgba, ImGuiColorEditFlags_AlphaBar)) {
                if(auto* editTrack = timeline.getTrack(track.id)) editTrack->color = ofColor(
                    static_cast<unsigned char>(ofClamp(rgba[0] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(rgba[1] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(rgba[2] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(rgba[3] * 255.0f, 0.0f, 255.0f)));
            }
            ImGui::EndPopup();
        }
        if(ImGui::BeginPopup(("##trackMenu" + track.id).c_str())) {
            ImGui::TextUnformatted(track.name.c_str());
            contextMenuSection("Appearance");
            float menuColor[4] = {color.x, color.y, color.z, color.w};
            if(ImGui::ColorEdit4("Track color", menuColor, ImGuiColorEditFlags_AlphaBar)) {
                if(auto* editTrack = timeline.getTrack(track.id)) editTrack->color = ofColor(
                    static_cast<unsigned char>(ofClamp(menuColor[0] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(menuColor[1] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(menuColor[2] * 255.0f, 0.0f, 255.0f)),
                    static_cast<unsigned char>(ofClamp(menuColor[3] * 255.0f, 0.0f, 255.0f)));
            }
            if(!track.isWaveTrack && ImGui::MenuItem(trackCollapsed ? "Show target rows" : "Hide target rows")) {
                targetsView = true;
                if(auto* editTrack = timeline.getTrack(track.id)) editTrack->collapsed = !trackCollapsed;
            }

            contextMenuSection("Create");
            if(ImGui::MenuItem(track.isWaveTrack ? "Add new wave clip" : "New clip")) {
                if(track.isWaveTrack) {
                    // Wave clips need a file before they are useful, so this
                    // asks for one and creates the clip around it.
                    waveFileRequest = WaveFileRequest::NewClip;
                    waveFileRequestTrackId = track.id;
                    waveFileRequestBeat = pendingWaveClipBeat;
                } else {
                    pendingTrackId = track.id;
                    pendingClipName[0] = '\0';
                    pendingClipBindingId = track.bindings.empty() ? std::string() : track.bindings.front().id;
                    pendingClipLaneType = track.bindings.empty() ? 1 : optionIndexForBinding(track.bindings.front());
                    pendingStartBeat = snapBeat(pendingWaveClipBeat);
                    pendingDurationBeats = timeline.getBeatsPerBar();
                    requestClipPopup = true;
                }
                ImGui::CloseCurrentPopup();
            }
            if(ImGui::MenuItem("Load clip from disk...")) {
                clipFileRequest = ClipFileRequest::Load;
                clipFileTrackId = track.id;
                clipImportBeat = snapBeat(pendingWaveClipBeat);
            }
            if(!track.isWaveTrack) {
                const bool canCaptureState = std::any_of(track.bindings.begin(), track.bindings.end(), [](const auto& binding) {
                    return binding.valueType == typeid(float).name() ||
                           binding.valueType == typeid(int).name() ||
                           binding.valueType == typeid(bool).name() ||
                           binding.valueType == typeid(std::vector<float>).name() ||
                           binding.valueType == typeid(std::vector<int>).name() ||
                           binding.valueType == typeid(std::vector<bool>).name();
                });
                if(ImGui::MenuItem("Capture state as curve points", nullptr, false, canCaptureState)) {
                    timeline.captureStateAsCurvePoints(track.id, std::string(), transportState.beatPosition);
                    ImGui::CloseCurrentPopup();
                }
            }

            if(track.isWaveTrack || !track.bindings.empty()) contextMenuSection("Automation");
            if(track.isWaveTrack) {
                if(ImGui::MenuItem("Edit track volume automation", nullptr, false, !track.clips.empty())) {
                    timeline.createWaveVolumeLane(track.id, track.clips.front().id);
                    editorState.editorTrackId = track.id;
                    editorState.editorClipId = track.clips.front().id;
                    editorState.editorLaneId.clear();
                    editorState.clipEditorOpen = true;
                    editorState.foldedClipEditors.erase(editorState.editorClipId);
                }
                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Volume automation applies to the whole track");
            }
            if(!track.bindings.empty()) {
                // Reachable here too (not just the per-binding row's own
                // menu) because that row only exists while the track is
                // expanded -- a collapsed track would otherwise have no way
                // to reach blend mode at all.
                if(ImGui::BeginMenu("Blend mode")) {
                    if(track.bindings.size() == 1) {
                        drawBlendModeOptions(timeline, track.id, track.bindings.front());
                    } else {
                        for(const auto& binding : track.bindings) {
                            const std::string label = compactParameterName(binding.parameterPath);
                            if(ImGui::BeginMenu(label.c_str())) {
                                drawBlendModeOptions(timeline, track.id, binding);
                                ImGui::EndMenu();
                            }
                        }
                    }
                    ImGui::EndMenu();
                }
                contextMenuSection("Parameters");
                // Same reasoning as "Blend mode" above -- give a collapsed
                // track a way to unbind a parameter without having to
                // expand it first to reach the per-binding row's own menu.
                if(ImGui::BeginMenu("Remove from Timeline")) {
                    for(const auto& binding : track.bindings) {
                        const std::string label = compactParameterName(binding.parameterPath);
                        if(ImGui::Selectable(label.c_str())) {
                            removeBindingTrackId = track.id;
                            removeBindingId = binding.id;
                            requestRemoveBinding = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndMenu();
                }
            }

            contextMenuSection("Track");
            if(ImGui::MenuItem("Rename track")) {
                pendingTrackId = track.id;
                pendingNewTrackDialog = false;
                std::strncpy(pendingTrackName, track.name.c_str(), sizeof(pendingTrackName) - 1);
                pendingTrackName[sizeof(pendingTrackName) - 1] = '\0';
                requestRenamePopup = true;
                ImGui::CloseCurrentPopup();
            }
            if(ImGui::MenuItem("Duplicate track")) {
                duplicateTrackId = track.id;
                ImGui::CloseCurrentPopup();
            }
            if(ImGui::MenuItem("Remove track")) {
                trackDeletionId = track.id;
                requestTrackDeletion = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        auto drawGrid = [&](const ImVec2& min, const ImVec2& max) {
            if(timeline.isLoopEnabled()) {
                const float loopX1 = min.x + beatOffset(timeline.getLoopStartBeat());
                const float loopX2 = min.x + beatOffset(timeline.getLoopEndBeat());
                dl->AddRectFilled(ImVec2(std::max(min.x, loopX1), min.y), ImVec2(std::min(max.x, loopX2), max.y), IM_COL32(95, 105, 220, 22));
                dl->AddLine(ImVec2(loopX1, min.y), ImVec2(loopX1, max.y), IM_COL32(130, 145, 235, 95));
                dl->AddLine(ImVec2(loopX2, min.y), ImVec2(loopX2, max.y), IM_COL32(130, 145, 235, 95));
            }
            const double grid = displayGridBeats();
            // Only the lines that are on screen.
            const double visibleFrom = beatAtOffset(std::max(0.0f, zoneLeft - min.x));
            const double visibleTo = std::min(endBeat, beatAtOffset(std::max(0.0f, max.x - min.x)) + grid);
            const int firstLine = std::max(0, static_cast<int>(std::floor(visibleFrom / grid)));
            const int lines = static_cast<int>(std::ceil(visibleTo / grid));
            for(int i = firstLine; i <= lines; ++i) {
                const double beat = i * grid;
                const float x = min.x + beatOffset(beat);
                const bool isBar = std::fmod(beat, timeline.getBeatsPerBar()) < 0.001;
                const bool quarter = std::fmod(beat, 1.0) < 0.001;
                if(isBar || quarter || beatOffset(beat + grid) - beatOffset(beat) >= 4.0f)
                    dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), isBar ? kBar : quarter ? IM_COL32(92, 92, 92, 145) : kGrid, isBar ? 1.5f : 1.0f);
            }
        };

        auto drawClip = [&](const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane* lane,
                            const ImVec2& min, const ImVec2& max, bool drawChrome = true,
                            bool drawContent = true) {
            const float x1 = min.x + beatOffset(clip.startBeat);
            const float x2 = min.x + beatOffset(clip.startBeat + clip.durationBeats);
            const float left = std::max(min.x, x1), right = std::min(max.x, x2);
            if(right <= left) return;
            if(drawChrome) {
                clipScreenRects.push_back({track.id, clip.id, left + 1.0f, min.y + 3.0f, right - 1.0f, max.y - 3.0f});
                // Square, no outline; a selected clip is shown by a brighter fill.
                const bool clipSelected = selectedClips.count({track.id, clip.id}) > 0;
                auto lift = [&](unsigned char c) { return static_cast<int>(c + (255 - c) * (clipSelected ? 0.45f : 0.0f)); };
                const ImU32 fill = IM_COL32(lift(track.color.r), lift(track.color.g), lift(track.color.b),
                                            clip.muted ? 45 : (clipSelected ? 235 : (lane == nullptr ? 75 : 185)));
                dl->AddRectFilled(ImVec2(left + 1, min.y + 3), ImVec2(right - 1, max.y - 3), fill);
                // A clip's persistent group and its (separate, ephemeral)
                // Shift+click selection are drawn as extra outlines on top
                // of the normal fill/border above, so a grouped clip that's
                // also currently selected shows both at once.
                if(const auto* group = timeline.getGroupForClip(track.id, clip.id)) {
                    // Colour by a hash of the group id so every member of
                    // the same group reads as one thing across every track
                    // it touches, while a second group visible at the same
                    // time gets a visibly different colour instead of
                    // reusing one fixed colour for "grouped".
                    const size_t hash = std::hash<std::string>{}(group->id);
                    const ImU32 groupColor = IM_COL32(180 + (hash % 60), 150 + ((hash >> 8) % 90), 255 - ((hash >> 16) % 110), 235);
                    dl->AddRect(ImVec2(left - 1, min.y + 1), ImVec2(right + 1, max.y - 1), groupColor, 0.0f, 0, 2.5f);
                    dl->AddText(ImVec2(right - 13, min.y + 3), groupColor, "G");
                }
                if(right - left > 45) {
                    std::string label = clip.name + (clip.muted ? " [Muted]" : "");
                    label += " [" + ofxOceanodeTimelinePresentation::clipSummary(clip, track.isWaveTrack) + "]";
                    const float labelRight = right - (timeline.getGroupForClip(track.id, clip.id) ? 17.0f : 3.0f);
                    dl->PushClipRect(ImVec2(left + 4.0f, min.y + 3.0f), ImVec2(labelRight, max.y - 3.0f), true);
                    dl->AddText(ImVec2(left + 6, min.y + 6), IM_COL32(245, 250, 255, 255), label.c_str());
                    dl->PopClipRect();
                }
            }
            if(!drawContent) return;
            // Repeats that are off screen are not drawn (a short repeating
            // source on a long clip can have thousands of them).
            int firstVisibleCycle = 0;
            int lastVisibleCycle = std::numeric_limits<int>::max();
            if(clip.repeatContent) {
                const double cycleLength = std::max(1e-6, cycleDuration(clip));
                const double visibleFrom = beatAtOffset(left - min.x) - clip.startBeat;
                const double visibleTo = beatAtOffset(right - min.x) - clip.startBeat;
                firstVisibleCycle = std::max(0, static_cast<int>(std::floor(visibleFrom / cycleLength)));
                lastVisibleCycle = static_cast<int>(std::min(1e9, std::ceil(visibleTo / cycleLength)));
            }
            if(track.isWaveTrack) {
                dl->PushClipRect(ImVec2(left + 1.0f, min.y + 3.0f), ImVec2(right - 1.0f, max.y - 3.0f), true);
                if(clip.waveNumChannels > 0 && !clip.waveformPeaks.empty()) {
                    constexpr int kMiniPointsPerChannel = 2000;
                    const double miniContent = sourceDuration(clip);
                    const double fileContent = std::max(miniContent + clip.waveSourceStartBeat,
                        clip.waveFileDurationBeats > 0.0
                            ? clip.waveFileDurationBeats
                            : clip.waveFileDurationMs > 0.0
                            ? clip.waveFileDurationMs * std::max(1.0f, transportState.bpm) / 60000.0
                            : miniContent);
                    const double sourceStart = ofClamp(clip.waveSourceStartBeat / fileContent, 0.0, 1.0);
                    const double sourceSpan = ofClamp(miniContent / fileContent, 0.0, 1.0 - sourceStart);
                    const int miniCycles = clip.repeatContent
                        ? std::max(1, static_cast<int>(std::ceil(clip.durationBeats / cycleDuration(clip)))) : 1;
                    const int channels = std::max(1, clip.waveNumChannels);
                    const float channelHeight = (max.y - min.y - 6.0f) / channels;
                    for(int cycle = firstVisibleCycle; cycle < miniCycles && cycle <= lastVisibleCycle; ++cycle) {
                        for(int ch = 0; ch < channels; ++ch) {
                            const float channelTop = min.y + 3.0f + ch * channelHeight;
                            const float midY = channelTop + channelHeight * 0.5f;
                            const float half = channelHeight * 0.45f;
                            for(int pt = 0; pt < kMiniPointsPerChannel; pt += 4) {
                                const double sourceBeatAtPoint = miniContent * (static_cast<double>(pt) / kMiniPointsPerChannel);
                                const float x = min.x + beatOffset(sourceToTimelineBeat(clip, sourceBeatAtPoint, cycle));
                                if(x < left || x > right) continue;
                                const double normalizedPoint = static_cast<double>(pt) / kMiniPointsPerChannel;
                                const double filePoint = clip.waveReverse
                                    ? sourceStart + (1.0 - normalizedPoint) * sourceSpan
                                    : sourceStart + normalizedPoint * sourceSpan;
                                const int peakPoint = ofClamp(static_cast<int>(filePoint * (kMiniPointsPerChannel - 1)),
                                                              0, kMiniPointsPerChannel - 1);
                                const size_t index = static_cast<size_t>(ch) * kMiniPointsPerChannel + peakPoint;
                                if(index >= clip.waveformPeaks.size()) continue;
                                const float peak = ofClamp(clip.waveformPeaks[index] * clip.waveGain, -1.0f, 1.0f);
                                dl->AddLine(ImVec2(x, midY), ImVec2(x, midY - peak * half), IM_COL32(245, 245, 245, 170), 1.0f);
                            }
                        }
                    }
                }
                dl->PopClipRect();
                return;
            }
            const auto previewView = editorView(timeline, transportState.bpm, endBeat);
            const ofxOceanodeTimelineClipPreview preview{timeline, *container, track, clip, lane,
                previewView, min, max, left, right, firstVisibleCycle, lastVisibleCycle};
            if(clip.isLfo) lfoEditor.drawPreview(preview);
            else if(lane != nullptr) laneEditors.forType(lane->type).drawPreview(preview);

        };

        bool clipInteractionClaimedThisFrame = false;
        auto handleClip = [&](const ofxOceanodeTimelineClip& clip, const ofxOceanodeTimelineLane* lane,
                              const ImVec2& min, const ImVec2& max) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            // Never let a click that lands on the pinned label/properties
            // column be mistaken for a click on scrolled-away clip content
            // that happens to fall at the same screen x.
            if(mouse.x < zoneLeft) return;
            const float x1 = min.x + beatOffset(clip.startBeat);
            const float x2 = min.x + beatOffset(clip.startBeat + clip.durationBeats);
            if(!hoverRect(min, max) || mouse.x < x1 || mouse.x > x2) return;
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                selectedTracks.clear();
            const std::string menuId = "##clipMenu" + track.id + "_" + clip.id;
            const std::pair<std::string, std::string> memberKey(track.id, clip.id);
            if(clipDragMode == ClipDragMode::None) {
                const auto count = ofxOceanodeTimelinePresentation::targetCount(clip);
                std::string projection;
                if(targetsView && !currentRowBindingId.empty() && !clip.isLfo) {
                    const auto sources = std::count_if(clip.lanes.begin(), clip.lanes.end(), [&](const auto& source) {
                        return std::find(source.bindingIds.begin(), source.bindingIds.end(), currentRowBindingId) != source.bindingIds.end();
                    });
                    projection = ofToString(sources) + " lane(s) in this clip drive this target. Inspect the target for combined output.\n";
                }
                ImGui::SetTooltip("Clip: %s\n%s\nOne clip: moving or stretching affects all its lanes and %zu targets.\n%sDouble-click to open content. Right edge: %s.\nShift: trim / extend. Cmd/Ctrl: stretch edge or duplicate body.",
                    clip.name.c_str(), ofxOceanodeTimelinePresentation::clipSummary(clip, track.isWaveTrack).c_str(), count, projection.c_str(),
                    track.isWaveTrack ? "Stretch audio (changes speed)" : ofxOceanodeTimelinePresentation::endEditName(
                        static_cast<ofxOceanodeTimelineEndEdit>(endEditTool)));
                const float hoverEdgeZone = std::min(kEdgePixels, (x2 - x1) * 0.25f);
                if(mouse.x >= x2 - hoverEdgeZone || mouse.x <= x1 + hoverEdgeZone)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if(clipInteractionClaimedThisFrame) return;
                clipInteractionClaimedThisFrame = true;
                anyClipInteractionClaimedThisFrame = true;
                pendingTrackId = track.id;
                pendingClipId = clip.id;
                pendingLaneId = lane != nullptr ? lane->id : (clip.lanes.empty() ? "" : clip.lanes.front().id);
                clipMenuHitBeat = beatAtOffset(mouse.x - min.x);
                clipMenuBeat = snapBeat(clipMenuHitBeat); // "Split here"
                // Right-clicking a clip that's part of the current multi-
                // selection keeps that whole selection (so the popup's
                // "Group" action groups all of it); right-clicking outside
                // it collapses the selection down to just this clip first,
                // the same convention the left-click branch below uses.
                if(selectedClips.count(memberKey) == 0 || selectedClips.size() <= 1) {
                    selectedClips.clear();
                    selectedClips.insert(memberKey);
                }
                ImGui::OpenPopup(menuId.c_str());
            }
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if(clipInteractionClaimedThisFrame || clipDragMode != ClipDragMode::None) return;
                // Shift is already the edge-drag modifier for Resize below,
                // so Shift+click only takes over the *non-edge* case: a
                // pure multi-selection toggle, not the start of any drag.
                // Edge zones shrink on narrow clips so a short clip can still be moved.
                const float edgeZone = std::min(kEdgePixels, (x2 - x1) * 0.25f);
                const bool edge = mouse.x >= x2 - edgeZone;
                // The left edge trims; with Shift it can also extend the
                // clip to the left (adding empty time in front).
                const bool leftEdge = !edge && mouse.x <= x1 + edgeZone;
                if(ImGui::GetIO().KeyShift && !edge && !leftEdge) {
                    clipInteractionClaimedThisFrame = true;
                    anyClipInteractionClaimedThisFrame = true;
                    if(selectedClips.count(memberKey) > 0) selectedClips.erase(memberKey);
                    else selectedClips.insert(memberKey);
                    return;
                }
                clipInteractionClaimedThisFrame = true;
                anyClipInteractionClaimedThisFrame = true;
                pendingTrackId = track.id;
                pendingClipId = clip.id;
                pendingLaneId = lane != nullptr ? lane->id : (clip.lanes.empty() ? "" : clip.lanes.front().id);
                const double clickedBeat = beatAtOffset(mouse.x - min.x);
                const bool editorShowingThisClip = editorState.clipEditorOpen && editorState.editorTrackId == track.id &&
                    editorState.editorClipId == clip.id && editorState.foldedClipEditors.count(clip.id) == 0;
                if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editorShowingThisClip) {
                    // Double-clicking the clip whose editor is open closes it again.
                    editorState.clipEditorOpen = false;
                } else if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editorState.editorTrackId = track.id;
                    editorState.editorClipId = clip.id;
                    editorState.editorLaneId = lane != nullptr ? lane->id : std::string();
                    editorAnchorBindingId = currentRowBindingId;
                    editorState.clipEditorOpen = true;
                    editorState.foldedClipEditors.erase(editorState.editorClipId);
                    if(singleRowTrack && targetsView) {
                        // Layout stays single-row for the rest of this frame,
                        // but the model expands now (irrelevant for a Wave
                        // Track, which stays single-row regardless -- see
                        // singleRowTrack above) so the editor is docked
                        // directly below the track on the next frame.
                        if(!track.isWaveTrack) {
                            if(auto* editTrack = timeline.getTrack(track.id)) editTrack->collapsed = false;
                        }
                        expandTrackForEditor = true;
                    }
                    pendingStartBeat = snapBeat(std::max(0.0, clickedBeat - clip.startBeat));
                    pendingDurationBeats = 1.0;
                } else {
                    // A plain click on a clip outside the current multi-
                    // selection starts a fresh, single-clip selection;
                    // clicking a clip that's already part of a bigger
                    // selection keeps the whole thing so it can be dragged
                    // together below instead of collapsing mid-drag.
                    if(selectedClips.count(memberKey) == 0 || selectedClips.size() <= 1) {
                        selectedClips.clear();
                        selectedClips.insert(memberKey);
                    }
                    // On macOS Cmd is the stretch modifier. Ctrl is reserved
                    // by the canvas/ImGui interaction layer and can turn a
                    // drag into a context-menu gesture.
                    // ImGui deliberately maps macOS Cmd to its logical Ctrl
                    // modifier (and physical Ctrl to Super). Using KeySuper
                    // here was why Cmd+drag never selected Stretch.
                    const bool commandDown = ImGui::GetIO().KeyCtrl ||
                        (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl) != 0;
                    // Wave clips are complete audio slices: every edge drag
                    // (plain, Cmd or Shift) stretches the whole source.
                    // Cmd on the body (not on an edge) drags out a copy instead.
                    clipDragMode = edge ? (track.isWaveTrack ? ClipDragMode::Stretch
                                           : ImGui::GetIO().KeyShift ? ClipDragMode::Resize
                                           : commandDown ? ClipDragMode::Stretch
                                                         : endEditTool == 1 ? ClipDragMode::Resize
                                                         : endEditTool == 2 ? ClipDragMode::Stretch : ClipDragMode::Repeat)
                                        : leftEdge ? ClipDragMode::TrimStart
                                        : commandDown ? ClipDragMode::Duplicate
                                        : ClipDragMode::Move;
                    clipDragStartMouseX = mouse.x;
                    clipDragStartMouseY = mouse.y;
                    trimExtends = leftEdge && ImGui::GetIO().KeyShift;
                    dragSourceBindingId = currentRowBindingId;
                    dragSourceLaneId = lane != nullptr ? lane->id : (clip.lanes.empty() ? std::string() : clip.lanes.front().id);
                    clipDragCommitted = false;
                    trimPreviewBeat = clip.startBeat;
                    dragRowTop = min.y;
                    dragRowBottom = max.y;
                    draggingTrackId = track.id;
                    draggingClipId = clip.id;
                    dragOffsetBeats = clickedBeat - clip.startBeat;
                    dragInitialContentDuration = clip.contentDurationBeats;
                    dragInitialContentStretch = ofxOceanodeTimelineClipTime::effectiveStretch(clip);
                    dragTimelineOriginX = min.x;

                    // A clip that's grouped, or that's part of a multi-
                    // selection bigger than itself, drags/stretches as one
                    // unit: snapshot every member's current timing now so
                    // the whole set is scaled from this one shared
                    // reference each frame (see groupDragSnapshot's
                    // declaration in the header for why).
                    groupDragSnapshot.clear();
                    std::vector<std::pair<std::string, std::string>> dragMembers;
                    if(const auto* group = timeline.getGroupForClip(track.id, clip.id)) {
                        dragMembers = group->members;
                    } else if(selectedClips.count(memberKey) > 0 && selectedClips.size() > 1) {
                        dragMembers.assign(selectedClips.begin(), selectedClips.end());
                    }
                    if(dragMembers.size() > 1) {
                        double anchorBeat = clip.startBeat;
                        for(const auto& member : dragMembers) {
                            if(const auto* memberClip = timeline.getClip(member.first, member.second)) {
                                groupDragSnapshot.push_back({member.first, member.second, memberClip->startBeat,
                                    memberClip->durationBeats, memberClip->contentDurationBeats, ofxOceanodeTimelineClipTime::effectiveStretch(*memberClip)});
                                anchorBeat = std::min(anchorBeat, memberClip->startBeat);
                            }
                        }
                        groupDragAnchorBeat = anchorBeat;
                    }
                }
            }
        };

        // Popups must be submitted every frame, even after the mouse leaves
        // the clip that opened them.
        auto drawClipMenu = [&](const ofxOceanodeTimelineClip& clip) {
            const std::string menuId = "##clipMenu" + track.id + "_" + clip.id;
            if(!ImGui::BeginPopup(menuId.c_str())) return;
            ImGui::TextUnformatted(clip.name.c_str());
            contextMenuSection("Clip");
            if(ImGui::MenuItem("Mute clip", nullptr, clip.muted))
                timeline.setClipMuted(track.id, clip.id, !clip.muted);
            if(ImGui::MenuItem("Rename clip")) {
                requestClipRename(track.id, clip);
                ImGui::CloseCurrentPopup();
            }
            if(ImGui::MenuItem("Save clip to disk...")) {
                clipFileRequest = ClipFileRequest::Save;
                clipFileTrackId = track.id;
                clipFileClipId = clip.id;
            }
            if(ImGui::BeginMenu("Clips at this position")) {
                for(const auto& candidate : track.clips) {
                    if(clipMenuHitBeat < candidate.startBeat ||
                       clipMenuHitBeat >= candidate.startBeat + candidate.durationBeats) continue;
                    if(ImGui::MenuItem((candidate.name + "##" + candidate.id).c_str(), nullptr, selectedClips.count({track.id, candidate.id}) > 0)) {
                        selectedClips = {{track.id, candidate.id}};
                        editorState.editorTrackId = track.id;
                        editorState.editorClipId = candidate.id;
                        editorState.editorLaneId = candidate.lanes.empty() ? std::string() : candidate.lanes.front().id;
                        editorState.clipEditorOpen = true;
                        editorState.foldedClipEditors.erase(candidate.id);
                    }
                }
                ImGui::EndMenu();
            }
            contextMenuSection("Timing");
            // These values are edited on the selected clip, never on the
            // track or on the other clips in it.
            if(auto* editClip = timeline.getClip(track.id, clip.id)) {
                ofxOceanodeTimelinePresentation::drawClipTiming(timeline, track, *editClip,
                    editorView(timeline, transportState.bpm, endBeat),
                    static_cast<ofxOceanodeTimelineEndEdit>(endEditTool), false);
                contextMenuSection("Content");
                if(!track.isWaveTrack && !clip.isLfo) {
                    if(ImGui::MenuItem("Break lanes into tracks", nullptr, false, clip.lanes.size() > 1))
                        breakClipLanesRequest = {track.id, clip.id};
                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Replace this clip with one new track per lane, preserving timing, content and targets.");
                }
                if(ImGui::MenuItem("Consolidate content"))
                    timeline.consolidateClipContent(track.id, clip.id);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Permanently remove source data hidden beyond the clip's right edge");
            }
            auto* selectedLane = timeline.getLane(track.id, clip.id, pendingLaneId);
            if(clip.isStateCapture && clip.lanes.size() > 1) {
                if(ImGui::MenuItem("Separate parameter clips")) {
                    requestSeparateStateCaptureClip = true;
                    separateStateCaptureTrackId = track.id;
                    separateStateCaptureClipId = clip.id;
                    ImGui::CloseCurrentPopup();
                }
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Create one independently movable/stretchable clip per parameter");
            }
            if(!track.isWaveTrack) {
                // Any automation or LFO clip can be cut in two at the playhead (S).
                const double playheadBeat = container->getTransportState().beatPosition;
                const bool playheadInside = playheadBeat > clip.startBeat + 1.0 / 96.0 &&
                    playheadBeat < clip.startBeat + clip.durationBeats - 1.0 / 96.0;
                const bool mouseInside = clipMenuBeat > clip.startBeat + 1.0 / 96.0 &&
                    clipMenuBeat < clip.startBeat + clip.durationBeats - 1.0 / 96.0;
                if(ImGui::MenuItem("Split here", nullptr, false, mouseInside)) {
                    requestClipSplit = true;
                    clipSplitTrackId = track.id;
                    clipSplitClipId = clip.id;
                    clipSplitBeat = clipMenuBeat;
                    ImGui::CloseCurrentPopup();
                }
                if(ImGui::MenuItem("Split at playhead", "S", false, playheadInside)) {
                    requestClipSplit = true;
                    clipSplitTrackId = track.id;
                    clipSplitClipId = clip.id;
                    clipSplitBeat = playheadBeat;
                    ImGui::CloseCurrentPopup();
                }
                if(!playheadInside && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Move the playhead inside the clip to split it there");
            }
            if(track.isWaveTrack) {
                const double playheadBeat = container->getTransportState().beatPosition;
                const bool mouseInside = clipMenuBeat > clip.startBeat + 1.0 / 96.0 &&
                    clipMenuBeat < clip.startBeat + clip.durationBeats - 1.0 / 96.0;
                const bool playheadInside = playheadBeat > clip.startBeat + 1.0 / 96.0 &&
                    playheadBeat < clip.startBeat + clip.durationBeats - 1.0 / 96.0;
                if(ImGui::MenuItem("Split here", nullptr, false, mouseInside)) {
                    requestWaveSplit = true;
                    waveSplitTrackId = track.id;
                    waveSplitClipId = clip.id;
                    clipSplitBeat = clipMenuBeat;
                    ImGui::CloseCurrentPopup();
                }
                if(ImGui::MenuItem("Split at playhead", "S", false, playheadInside)) {
                    requestWaveSplit = true;
                    waveSplitTrackId = track.id;
                    waveSplitClipId = clip.id;
                    clipSplitBeat = playheadBeat;
                    ImGui::CloseCurrentPopup();
                }
                auto* menuEditClip = timeline.getClip(track.id, clip.id);
                bool reverse = menuEditClip != nullptr && menuEditClip->waveReverse;
                if(ImGui::MenuItem("Reverse", nullptr, reverse) && menuEditClip != nullptr)
                    menuEditClip->waveReverse = !reverse;
                contextMenuSection("Automation");
                if(ImGui::MenuItem("Edit track volume automation")) {
                    timeline.createWaveVolumeLane(track.id, clip.id);
                    editorState.editorTrackId = track.id;
                    editorState.editorClipId = clip.id;
                    editorState.editorLaneId.clear();
                    editorState.clipEditorOpen = true;
                    editorState.foldedClipEditors.erase(editorState.editorClipId);
                }
            } else if(clip.isLfo) {
                // An LFO clip's lanes are its oscillator controls (frequency,
                // skew, pulse width, ...), not automation lanes the user
                // points at a parameter. Retyping one, or adding a sibling to
                // it, would quietly break the clip, so this menu offers none
                // of that -- the LFO editor (double-click the clip) is where
                // it is edited.
                contextMenuSection("Editor");
                ImGui::TextDisabled("LFO clip -- double-click to edit");
            } else {
                contextMenuSection("Lane");
                const auto selectedType = selectedLane == nullptr ? ofxOceanodeTimelineLaneType::Step : selectedLane->type;
                if(ImGui::BeginMenu("Lane type", selectedLane != nullptr)) {
                    for(int optionIndex = 0; optionIndex < kLaneTypeOptionCount; ++optionIndex) {
                        const auto candidateType = laneTypeFromOptionIndex(optionIndex);
                        const bool current = selectedLane != nullptr ? optionIndexForLane(*selectedLane) == optionIndex
                                                                     : selectedType == candidateType;
                        if(ImGui::MenuItem(kLaneTypeOptions[optionIndex], nullptr, current)) {
                            timeline.setClipLaneType(track.id, clip.id, selectedLane->id, candidateType);
                            if(optionIndex == kToggleOptionIndex)
                                makeToggleLane(timeline.getLane(track.id, clip.id, selectedLane->id));
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            if(selectedLane != nullptr && !clip.isLfo && selectedLane->type != ofxOceanodeTimelineLaneType::PianoRoll && !track.bindings.empty() && ImGui::BeginMenu("Lane targets")) {
                for(const auto& candidate : track.bindings) {
                    const bool assigned = std::find(selectedLane->bindingIds.begin(), selectedLane->bindingIds.end(), candidate.id) != selectedLane->bindingIds.end();
                    if(ImGui::MenuItem(candidate.parameterPath.c_str(), nullptr, assigned, true)) {
                        if(assigned) timeline.removeBindingFromLane(track.id, clip.id, selectedLane->id, candidate.id);
                        else timeline.addBindingToLane(track.id, clip.id, selectedLane->id, candidate.id);
                    }
                }
                ImGui::EndMenu();
            }
            if(!clip.isLfo && !track.bindings.empty() && ImGui::BeginMenu("New lane")) {
                for(const auto& candidate : track.bindings) {
                    const std::string label = compactParameterName(candidate.parameterPath);
                    if(ImGui::MenuItem(label.c_str())) {
                        const auto laneId = timeline.createLane(track.id, clip.id, candidate.parameterPath, candidate.laneType);
                        if(!laneId.empty()) {
                            timeline.addBindingToLane(track.id, clip.id, laneId, candidate.id);
                            pendingLaneId = laneId;
                        }
                    }
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", candidate.parameterPath.c_str());
                }
                ImGui::EndMenu();
            }
            // handleClip already made sure this clip's own key is part of
            // selectedClips by the time either popup that opens this menu
            // (this one, or the binding-row menu) is shown, so "Group"
            // always groups the clip(s) actually selected right now.
            const bool canGroupSelection = selectedClips.size() >= 2 && selectedClips.count({track.id, clip.id}) > 0;
            const bool isGrouped = timeline.getGroupForClip(track.id, clip.id) != nullptr;
            if(canGroupSelection || isGrouped) contextMenuSection("Grouping");
            if(canGroupSelection) {
                const std::string groupLabel = "Group " + ofToString(selectedClips.size()) + " clips";
                if(ImGui::MenuItem(groupLabel.c_str())) {
                    timeline.groupClips(std::vector<std::pair<std::string, std::string>>(
                        selectedClips.begin(), selectedClips.end()));
                    ImGui::CloseCurrentPopup();
                }
            }
            if(isGrouped) {
                if(ImGui::MenuItem("Ungroup clips")) {
                    timeline.ungroupClip(track.id, clip.id);
                    ImGui::CloseCurrentPopup();
                }
            }
            contextMenuSection("Remove");
            if(ImGui::MenuItem("Delete clip")) {
                clipDeletionTrackId = track.id;
                clipDeletionClipId = clip.id;
                requestClipDeletion = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        };

        if(singleRowTrack) {
            const ImVec2 min = headerMin, max = headerMax, laneMin(min.x + kLabelWidth - timelineScrollX, min.y);
            dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
            drawGrid(laneMin, max);
            // Two passes: every clip's box/label first, then every lane's content on
            // top, so overlapping clips' translucent boxes never dim curves drawn
            // earlier -- all curves in a collapsed track read equally bright.
            for(const auto& clip : track.clips) {
                drawClip(clip, clip.lanes.empty() ? nullptr : &clip.lanes.front(), laneMin, max, true, false);
            }
            for(const auto& clip : track.clips) {
                if(clip.lanes.empty() || clip.isLfo || track.isWaveTrack) {
                    // An LFO clip's lanes are its oscillator controls, not
                    // separate curves: its waveform is drawn once (drawing it
                    // per lane stacked it into a brighter, thicker line).
                    drawClip(clip, clip.lanes.empty() ? nullptr : &clip.lanes.front(), laneMin, max, false, true);
                } else {
                    const size_t bands = std::min<size_t>(3, clip.lanes.size());
                    for(size_t laneIndex = 0; laneIndex < bands; ++laneIndex) {
                        const float top = min.y + 22.0f;
                        const float bandHeight = (max.y - top) / std::max<size_t>(1, bands);
                        const ImVec2 previewMin(laneMin.x, top + laneIndex * bandHeight);
                        const ImVec2 previewMax(max.x, top + (laneIndex + 1) * bandHeight);
                        drawClip(clip, &clip.lanes[laneIndex], previewMin, previewMax, false, true);
                    }
                }
            }
            // Later clips are drawn on top, so hit-test in reverse order and
            // let the visually topmost overlapping clip own the gesture.
            clipDropRows.push_back({track.id, std::string(), min.y, max.y});
            for(auto clipIt = track.clips.rbegin(); clipIt != track.clips.rend(); ++clipIt) {
                const auto* frontLane = clipIt->lanes.empty() ? nullptr : &clipIt->lanes.front();
                handleClip(*clipIt, frontLane, laneMin, max);
            }
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
               !clipInteractionClaimedThisFrame &&
               hoverRect(min, max) &&
               ImGui::GetIO().MousePos.x >= zoneLeft &&
               !(track.isWaveTrack && ImGui::GetIO().MousePos.y >= max.y - kWaveTrackResizeHandleHeight)) {
                emptyTrackAreaClickedThisFrame = true;
            }
            // Let a right-click in empty arrangement space open the track
            // menu, with a new clip placed at the clicked beat. A clip
            // that was actually hit has already claimed the gesture above
            // and keeps its own clip menu.
            if(!clipInteractionClaimedThisFrame &&
               hoverRect(min, max) &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
               ImGui::GetIO().MousePos.x >= zoneLeft) {
                pendingTrackId = track.id;
                pendingWaveClipBeat = snapBeat(std::max(0.0,
                    beatAtOffset(ImGui::GetIO().MousePos.x - laneMin.x)));
                ImGui::OpenPopup(("##trackMenu" + track.id).c_str());
            }
            const float px = laneMin.x + beatOffset(transportState.beatPosition);
            if(px >= zoneLeft && px <= max.x) dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), kPlayhead, 2);
            dl->PopClipRect();
            for(const auto& clip : track.clips) drawClipMenu(clip);
            if(track.isWaveTrack) {
                const ImVec2 resizeMin(headerMin.x, headerMax.y - kWaveTrackResizeHandleHeight);
                const ImVec2 resizeMax(headerMax.x, headerMax.y);
                const bool resizeHovered = hoverRect(resizeMin, resizeMax);
                if(resizeHovered || waveTrackResizeId == track.id) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    dl->AddLine(ImVec2(headerMin.x, headerMax.y - 1.0f),
                                ImVec2(headerMax.x, headerMax.y - 1.0f),
                                resizeHovered || waveTrackResizeId == track.id
                                    ? IM_COL32(220, 220, 245, 220)
                                    : IM_COL32(150, 150, 170, 150), 2.0f);
                }
                if(resizeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    waveTrackResizeId = track.id;
                    waveTrackResizeStartMouseY = ImGui::GetIO().MousePos.y;
                    waveTrackResizeStartHeight = trackHeaderHeight;
                }
                if(waveTrackResizeId == track.id) {
                    if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        if(auto* editTrack = timeline.getTrack(track.id)) {
                            editTrack->waveTrackHeight = ofClamp(
                                waveTrackResizeStartHeight +
                                    (ImGui::GetIO().MousePos.y - waveTrackResizeStartMouseY),
                                kWaveTrackMinHeight, kWaveTrackMaxHeight);
                        }
                    }
                    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) waveTrackResizeId.clear();
                }
            }
            ImGui::SetCursorPosY(headerY + trackHeaderHeight);
            if(track.isWaveTrack) {
                if(editorState.clipEditorOpen && editorState.editorTrackId == track.id) {
                    editorScreenTop = ImGui::GetCursorScreenPos().y;
                    drawEditorSeparator(track, contentWidth);
                    drawWaveTrackEditor(timeline, track, contentWidth, endBeat, transportState.beatPosition);
                    editorScreenBottom = ImGui::GetCursorScreenPos().y;
                    editorScreenValid = true;
                }
            } else if(editorState.clipEditorOpen && editorState.editorTrackId == track.id && !expandTrackForEditor) {
                editorScreenTop = ImGui::GetCursorScreenPos().y;
                drawEditorSeparator(track, contentWidth);
                drawLaneEditor(timeline, track, contentWidth, endBeat, transportState.beatPosition);
                editorScreenBottom = ImGui::GetCursorScreenPos().y;
                editorScreenValid = true;
            }
        } else {
            int index = 0;
            // Rows sharing a clip.id (a clip with more than one lane,
            // combining e.g. a curve and a step pattern on this track) are
            // bracketed together below so it reads as one entity rather
            // than a coincidence.
            std::unordered_map<std::string, std::vector<std::pair<float, float>>> multiLaneClipRowSpans;
            // Each movable unit (a parameter row, or a note group with its
            // member rows) with its screen extent, for reordering.
            struct RowUnit {
                std::vector<std::string> bindingIds;
                float top = 0.0f;
                float bottom = 0.0f;
            };
            std::vector<RowUnit> rowUnits;
            // Model changes requested from row menus are applied after the
            // rows are drawn (not while iterating them).
            int deferredMoveUnitFrom = -1;
            int deferredMoveUnitTo = -1;
            std::string deferredGroupId;
            enum class DeferredGroupAction { None, Ungroup, Remove, NewClip, LeaveGroup };
            DeferredGroupAction deferredGroupAction = DeferredGroupAction::None;
            std::string deferredLeaveBindingId;
            double deferredClipBeat = 0.0;
            // Row height drag (applied to the model before this frame's layout).
            if(rowResizeTrackId == track.id) {
                if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const float newHeight = ofClamp(rowResizeStartHeight + ImGui::GetIO().MousePos.y - rowResizeStartY, 20.0f, 300.0f);
                    if(auto* editTrack = timeline.getTrack(track.id)) {
                        for(auto& editBinding : editTrack->bindings)
                            if(editBinding.id == rowResizeBindingId) editBinding.rowHeight = newHeight;
                        for(auto& editGroup : editTrack->noteGroups)
                            if(editGroup.id == rowResizeBindingId) editGroup.rowHeight = newHeight;
                    }
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                } else {
                    rowResizeTrackId.clear();
                    rowResizeBindingId.clear();
                }
            }
            // Which row the open clip editor docks under: the row it was
            // opened from, else the first row automated by the edited clip.
            const bool editorOnThisTrack = editorState.clipEditorOpen && editorState.editorTrackId == track.id;
            std::string editorAnchorId;
            if(editorOnThisTrack) {
                auto hasBinding = [&](const std::string& id) {
                    return !id.empty() && std::any_of(track.bindings.begin(), track.bindings.end(),
                                                      [&](const auto& b) { return b.id == id; });
                };
                if(hasBinding(editorAnchorBindingId)) editorAnchorId = editorAnchorBindingId;
                else {
                    const auto clipIt = std::find_if(track.clips.begin(), track.clips.end(),
                        [&](const auto& c) { return c.id == editorState.editorClipId; });
                    if(clipIt != track.clips.end()) {
                        for(const auto& b : track.bindings) {
                            if(laneForBinding(*clipIt, b.id) != nullptr) { editorAnchorId = b.id; break; }
                        }
                    }
                }
            }
            bool editorDrawn = false;
            std::pair<float, float> editorSpanScreen{0.0f, 0.0f};
            float rowY = headerY + kHeaderHeight;
            for(const auto& binding : track.bindings) {
                const auto* noteGroup = ofxOceanodeTimelineManager::findNoteGroupForBinding(track, binding.id);
                const auto groupMembers = noteGroup != nullptr ? noteGroup->members() : std::vector<std::string>();
                const bool firstGroupMember = noteGroup != nullptr && groupMembers.front() == binding.id;
                const bool lastGroupMember = noteGroup != nullptr && groupMembers.back() == binding.id;
                const bool anchorIsHere = noteGroup != nullptr
                    ? (lastGroupMember && noteGroup->contains(editorAnchorId))
                    : binding.id == editorAnchorId;
                auto dockEditorHere = [&]() {
                    if(!editorOnThisTrack || editorDrawn || !anchorIsHere) return;
                    // Dock the clip editor directly under the row it edits;
                    // the remaining rows continue below it.
                    ImGui::SetCursorPos(ImVec2(0, rowY));
                    const float editorTopScreen = ImGui::GetCursorScreenPos().y;
                    drawEditorSeparator(track, contentWidth);
                    drawLaneEditor(timeline, track, contentWidth, endBeat, transportState.beatPosition);
                    rowY = ImGui::GetCursorPosY();
                    editorSpanScreen = {editorTopScreen, ImGui::GetCursorScreenPos().y};
                    editorScreenTop = editorSpanScreen.first;
                    editorScreenBottom = editorSpanScreen.second;
                    editorScreenValid = true;
                    editorDrawn = true;
                };
                if(firstGroupMember) {
                    // ---- Note group row: one row for a piano roll's pitch/gate/velocity ----
                    const auto& group = *noteGroup;
                    const float groupHeight = ofClamp(group.rowHeight, 20.0f, 300.0f);
                    ImGui::SetCursorPos(ImVec2(0, rowY));
                    ImGui::InvisibleButton(("##noteGroup" + track.id + group.id).c_str(), ImVec2(contentWidth, groupHeight));
                    rowY += groupHeight;
                    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax(), laneMin(min.x + kLabelWidth - timelineScrollX, min.y);
                    const ImVec2 labelMin(min.x + kParameterRowIndent, min.y); // rows sit indented under their track
                    rowUnits.push_back({groupMembers, min.y, max.y});
                    constexpr float kGroupResizeGrip = 5.0f;
                    const ImVec2 gripMin(labelMin.x, max.y - kGroupResizeGrip), gripMax(min.x + kLabelWidth, max.y);
                    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                    const bool gripHovered = rowResizeTrackId.empty() && bindingDragTrackId.empty() && !popupOpen &&
                        hoverRect(gripMin, gripMax);
                    dl->AddRectFilled(labelMin, max, mutedTrackColor(track.color, 0.17f));
                    dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
                    drawGrid(laneMin, max);
                    dl->PopClipRect();
                    dl->AddRectFilled(labelMin, ImVec2(min.x + kLabelWidth, max.y), mutedTrackColor(track.color, 0.38f, 0.55f));
                    dl->AddLine(ImVec2(labelMin.x, max.y - 0.5f), ImVec2(min.x + kLabelWidth, max.y - 0.5f), IM_COL32(0, 0, 0, 140), 1.0f); // row divider, on the resize edge
                    // Expand arrow: shows the member parameter rows underneath.
                    const ImVec2 arrowMin(labelMin.x + 2.0f, min.y + 2.0f), arrowMax(labelMin.x + 20.0f, min.y + 24.0f);
                    const bool arrowHovered = !popupOpen && hoverRect(arrowMin, arrowMax);
                    const ImU32 arrowColor = arrowHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 220, 230, 230);
                    const ImVec2 arrowCenter(labelMin.x + 11.0f, min.y + 13.0f);
                    if(group.expanded)
                        dl->AddTriangleFilled(ImVec2(arrowCenter.x - 5, arrowCenter.y - 3), ImVec2(arrowCenter.x + 5, arrowCenter.y - 3),
                                              ImVec2(arrowCenter.x, arrowCenter.y + 4), arrowColor);
                    else
                        dl->AddTriangleFilled(ImVec2(arrowCenter.x - 3, arrowCenter.y - 5), ImVec2(arrowCenter.x - 3, arrowCenter.y + 5),
                                              ImVec2(arrowCenter.x + 4, arrowCenter.y), arrowColor);
                    const bool arrowClicked = arrowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                    if(arrowClicked) {
                        if(auto* editGroup = timeline.getNoteGroup(track.id, group.id)) editGroup->expanded = !editGroup->expanded;
                    }
                    // Name, and which parameters it drives.
                    const float labelRight = min.x + kLabelWidth - 6.0f;
                    dl->PushClipRect(ImVec2(labelMin.x + 20.0f, min.y), ImVec2(labelRight, max.y), true);
                    dl->AddText(ImVec2(labelMin.x + 22.0f, min.y + 6.0f), IM_COL32(245, 250, 255, 255), group.name.c_str());
                    struct RoleTag { const char* label; const std::string* id; };
                    const RoleTag roleTags[3] = {{"pitch", &group.pitchBindingId}, {"gate", &group.gateBindingId}, {"vel", &group.velocityBindingId}};
                    const bool tagsOnOwnLine = groupHeight >= 40.0f;
                    float tagX = tagsOnOwnLine ? labelMin.x + 22.0f : labelMin.x + 30.0f + ImGui::CalcTextSize(group.name.c_str()).x;
                    const float tagY = tagsOnOwnLine ? min.y + 23.0f : min.y + 6.0f;
                    for(const auto& tag : roleTags) {
                        if(tag.id->empty()) continue;
                        const auto* member = timeline.getBinding(track.id, *tag.id);
                        const bool missing = member == nullptr || member->missingTarget;
                        const ImVec2 textSize = ImGui::CalcTextSize(tag.label);
                        const ImVec2 chipMin(tagX, tagY - 1.0f), chipMax(tagX + textSize.x + 8.0f, tagY + textSize.y + 1.0f);
                        dl->AddRectFilled(chipMin, chipMax, missing ? IM_COL32(170, 50, 50, 220) : IM_COL32(20, 20, 24, 120), 3.0f);
                        dl->AddText(ImVec2(tagX + 4.0f, tagY), missing ? IM_COL32(255, 225, 225, 255) : IM_COL32(215, 222, 235, 235), tag.label);
                        tagX = chipMax.x + 4.0f;
                    }
                    dl->PopClipRect();
                    const bool labelHovered = !popupOpen && hoverRect(min, ImVec2(min.x + kLabelWidth, max.y));
                    if(labelHovered && !arrowHovered && !gripHovered && rowResizeTrackId.empty() && bindingDragTrackId.empty()) {
                        std::string tip = group.name + " (piano roll)";
                        const char* roleNames[3] = {"Pitch", "Gate", "Velocity"};
                        for(int role = 0; role < 3; ++role) {
                            const auto* member = roleTags[role].id->empty() ? nullptr : timeline.getBinding(track.id, *roleTags[role].id);
                            tip += std::string("\n") + roleNames[role] + ": " +
                                (member == nullptr ? std::string("none") : member->parameterPath + (member->missingTarget ? "  (missing)" : ""));
                        }
                        tip += "\nRight-click for targets, clips and more";
                        ImGui::SetTooltip("%s", tip.c_str());
                    }
                    // Resize from the bottom edge (double-click: default height).
                    if(gripHovered || (rowResizeTrackId == track.id && rowResizeBindingId == group.id)) {
                        dl->AddLine(ImVec2(gripMin.x, max.y - 1.5f), ImVec2(gripMax.x, max.y - 1.5f), IM_COL32(255, 210, 90, 220), 3.0f);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    }
                    if(gripHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        rowResizeTrackId = track.id;
                        rowResizeBindingId = group.id;
                        rowResizeStartY = ImGui::GetIO().MousePos.y;
                        rowResizeStartHeight = groupHeight;
                    }
                    if(gripHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if(auto* editGroup = timeline.getNoteGroup(track.id, group.id)) editGroup->rowHeight = 48.0f;
                        rowResizeTrackId.clear();
                        rowResizeBindingId.clear();
                    }
                    // Press on the name and drag vertically to move the whole group.
                    if(track.bindings.size() > groupMembers.size() && labelHovered && !arrowHovered && !gripHovered &&
                       rowResizeTrackId.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        bindingDragTrackId = track.id;
                        bindingDragId = groupMembers.front();
                        bindingDragStartY = ImGui::GetIO().MousePos.y;
                        bindingDragActive = false;
                    }
                    const std::string groupMenuId = "##noteGroupMenu" + track.id + group.id;
                    if(hoverRect(min, max) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        const bool overTimelineZone = ImGui::GetIO().MousePos.x >= zoneLeft;
                        const double clickedBeat = overTimelineZone ? beatAtOffset(ImGui::GetIO().MousePos.x - laneMin.x) : 0.0;
                        bool overClip = false;
                        if(overTimelineZone) {
                            for(const auto& clip : track.clips) {
                                for(const auto& lane : clip.lanes) {
                                    if(lane.type != ofxOceanodeTimelineLaneType::PianoRoll) continue;
                                    if(std::none_of(lane.bindingIds.begin(), lane.bindingIds.end(),
                                                    [&](const auto& id) { return group.contains(id); })) continue;
                                    if(clickedBeat >= clip.startBeat && clickedBeat <= clip.startBeat + clip.durationBeats) overClip = true;
                                }
                            }
                        }
                        if(!overClip) {
                            pendingStartBeat = snapBeat(std::max(0.0, overTimelineZone ? clickedBeat : transportState.beatPosition));
                            ImGui::OpenPopup(groupMenuId.c_str());
                        }
                    }
                    if(ImGui::BeginPopup(groupMenuId.c_str())) {
                        ImGui::TextUnformatted(group.name.c_str());
                        contextMenuSection("Create and edit");
                        if(ImGui::MenuItem("New piano roll clip here")) {
                            deferredGroupId = group.id;
                            deferredGroupAction = DeferredGroupAction::NewClip;
                            deferredClipBeat = pendingStartBeat;
                        }
                        if(ImGui::MenuItem("Instrument / note routing...")) {
                            openNoteGroupSetup(timeline, nullptr, track.id, group.id);
                        }
                        contextMenuSection("View");
                        if(ImGui::MenuItem(group.expanded ? "Hide parameter rows" : "Show parameter rows")) {
                            if(auto* editGroup = timeline.getNoteGroup(track.id, group.id)) editGroup->expanded = !editGroup->expanded;
                        }
                        {
                            const auto* pitchMember = timeline.getBinding(track.id, groupMembers.front());
                            auto* canvasParameter = pitchMember == nullptr ? nullptr : container->findTimelineParameter(pitchMember->parameterPath);
                            if(ImGui::MenuItem("Show in Canvas", nullptr, false, canvasParameter != nullptr) && canvasParameter != nullptr)
                                container->showParameterInCanvas(*canvasParameter);
                        }
                        contextMenuSection("Order");
                        const int unitIndex = static_cast<int>(rowUnits.size()) - 1;
                        if(ImGui::MenuItem("Move up", nullptr, false, unitIndex > 0)) {
                            deferredMoveUnitFrom = unitIndex;
                            deferredMoveUnitTo = unitIndex - 1;
                        }
                        if(ImGui::MenuItem("Move down", nullptr, false, groupMembers.back() != track.bindings.back().id)) {
                            deferredMoveUnitFrom = unitIndex;
                            deferredMoveUnitTo = unitIndex + 2; // slot after the next unit
                        }
                        contextMenuSection("Group");
                        if(ImGui::MenuItem("Remove note routing group (keep targets)")) {
                            deferredGroupId = group.id;
                            deferredGroupAction = DeferredGroupAction::Ungroup;
                        }
                        if(ImGui::MenuItem("Remove from Timeline")) {
                            deferredGroupId = group.id;
                            deferredGroupAction = DeferredGroupAction::Remove;
                        }
                        ImGui::EndPopup();
                    }
                    // The group's piano-roll clips, drawn once.
                    auto groupLane = [&](const ofxOceanodeTimelineClip& clip) -> const ofxOceanodeTimelineLane* {
                        for(const auto& lane : clip.lanes) {
                            if(lane.type != ofxOceanodeTimelineLaneType::PianoRoll) continue;
                            if(std::any_of(lane.bindingIds.begin(), lane.bindingIds.end(),
                                           [&](const auto& id) { return group.contains(id); })) return &lane;
                        }
                        return nullptr;
                    };
                    dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
                    for(const auto& clip : track.clips) {
                        if(const auto* lane = groupLane(clip)) drawClip(clip, lane, laneMin, max);
                    }
                    clipDropRows.push_back({track.id, groupMembers.front(), min.y, max.y});
                    currentRowBindingId = groupMembers.front();
                    for(auto clipIt = track.clips.rbegin(); clipIt != track.clips.rend(); ++clipIt) {
                        if(const auto* lane = groupLane(*clipIt)) handleClip(*clipIt, lane, laneMin, max);
                    }
                    currentRowBindingId.clear();
                    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clipInteractionClaimedThisFrame &&
                       hoverRect(min, max) && ImGui::GetIO().MousePos.x >= zoneLeft) {
                        emptyTrackAreaClickedThisFrame = true;
                    }
                    const float px = laneMin.x + beatOffset(transportState.beatPosition);
                    if(px >= zoneLeft && px <= max.x) dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), kPlayhead, 2);
                    dl->PopClipRect();
                }
                if(noteGroup != nullptr && !noteGroup->expanded) {
                    if(lastGroupMember) dockEditorHere();
                    continue;
                }
                const float rowHeight = ofClamp(binding.rowHeight, 20.0f, 300.0f);
                ImGui::SetCursorPos(ImVec2(0, rowY));
                ImGui::InvisibleButton(("##binding" + track.id + binding.id).c_str(), ImVec2(contentWidth, rowHeight));
                rowY += rowHeight;
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax(), laneMin(min.x + kLabelWidth - timelineScrollX, min.y);
                const ImVec2 labelMin(min.x + kParameterRowIndent, min.y); // rows sit indented under their track
                if(noteGroup != nullptr && !rowUnits.empty()) rowUnits.back().bottom = max.y;
                else rowUnits.push_back({{binding.id}, min.y, max.y});
                // Member rows of an expanded note group: indented, and they
                // show only automation other than the group's piano roll.
                const float memberIndent = noteGroup != nullptr ? 14.0f : 0.0f;
                auto rowLane = [&](const ofxOceanodeTimelineClip& clip) -> const ofxOceanodeTimelineLane* {
                    const auto* lane = laneForBinding(clip, binding.id);
                    if(lane != nullptr && noteGroup != nullptr && lane->type == ofxOceanodeTimelineLaneType::PianoRoll) return nullptr;
                    return lane;
                };
                // Bottom edge of the label column: drag to resize the row.
                constexpr float kResizeGrip = 5.0f;
                const ImVec2 gripMin(labelMin.x, max.y - kResizeGrip), gripMax(min.x + kLabelWidth, max.y);
                const bool gripHovered = rowResizeTrackId.empty() && bindingDragTrackId.empty() &&
                    hoverRect(gripMin, gripMax) &&
                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                dl->AddRectFilled(labelMin, max, mutedTrackColor(track.color, 0.17f));
                dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
                drawGrid(laneMin, max);
                dl->PopClipRect();
                bool automated = false;
                for(const auto& clip : track.clips) automated = automated || rowLane(clip) != nullptr;
                dl->AddRectFilled(labelMin, ImVec2(min.x + kLabelWidth, max.y),
                                  mutedTrackColor(track.color, automated ? 0.34f : 0.23f, 0.48f));
                if(noteGroup != nullptr) {
                    // Tie the member row to its group row above.
                    dl->AddLine(ImVec2(labelMin.x + 7.0f, min.y), ImVec2(labelMin.x + 7.0f, lastGroupMember ? min.y + 14.0f : max.y),
                                IM_COL32(track.color.r, track.color.g, track.color.b, 200), 2.0f);
                    dl->AddLine(ImVec2(labelMin.x + 7.0f, min.y + 14.0f), ImVec2(labelMin.x + 12.0f, min.y + 14.0f),
                                IM_COL32(track.color.r, track.color.g, track.color.b, 200), 2.0f);
                }
                dl->AddLine(ImVec2(labelMin.x, max.y - 0.5f), ImVec2(min.x + kLabelWidth, max.y - 0.5f), IM_COL32(0, 0, 0, 140), 1.0f); // row divider, on the resize edge
                // Blend mode used to be findable only by right-clicking this
                // row (or the track header) and opening a nested "Blend
                // mode" submenu -- easy to lose track of, which is also why
                // the in-label "[Mode]" text this chip replaces wasn't a
                // real fix: it lived inside the PushClipRect below and
                // simply didn't render once parameterPath filled the label
                // column. This chip sits in its own fixed, never-clipped
                // spot at the row's right edge, always shows the current
                // mode, and opens the same options in a single click.
                const float blendChipWidth = 80.0f;
                const ImVec2 blendChipMin(min.x + kLabelWidth - blendChipWidth - 6.0f, min.y + 4.0f);
                const ImVec2 blendChipMax(min.x + kLabelWidth - 6.0f, min.y + std::min(rowHeight, kRowHeight) - 4.0f);
                const bool blendChipActive = binding.mode != ofxOceanodeTimelineAutomationMode::Replace;
                dl->AddRectFilled(blendChipMin, blendChipMax,
                                  blendChipActive ? IM_COL32(235, 165, 65, 220) : IM_COL32(90, 90, 95, 150), 3.0f);
                dl->PushClipRect(ImVec2(labelMin.x + 5.0f, min.y), ImVec2(blendChipMin.x - 4.0f, max.y), true);
                {
                    std::string rowLabel = binding.parameterPath;
                    if(noteGroup != nullptr) {
                        const char* role = noteGroup->pitchBindingId == binding.id ? "pitch"
                            : noteGroup->gateBindingId == binding.id ? "gate" : "vel";
                        rowLabel = std::string(role) + ": " + rowLabel;
                    }
                    if(binding.bypass) rowLabel += "  (bypassed)";
                    dl->AddText(ImVec2(labelMin.x + 7 + memberIndent, min.y + 6),
                                automated || noteGroup != nullptr ? IM_COL32(245, 250, 255, 255) : IM_COL32(180, 180, 180, 255),
                                rowLabel.c_str());
                }
                dl->PopClipRect();
                {
                    const std::string blendChipLabel = ofxOceanodeTimelineManager::modeToString(binding.mode) + " v";
                    dl->PushClipRect(blendChipMin, blendChipMax, true);
                    dl->AddText(ImVec2(blendChipMin.x + 4.0f, blendChipMin.y + 3.0f),
                               blendChipActive ? IM_COL32(30, 20, 5, 255) : IM_COL32(225, 225, 230, 235), blendChipLabel.c_str());
                    dl->PopClipRect();
                }
                if(hoverRect(blendChipMin, blendChipMax))
                    ImGui::SetTooltip("Blend mode -- how this parameter combines with other clips/tracks driving it");
                if(gripHovered || (rowResizeTrackId == track.id && rowResizeBindingId == binding.id)) {
                    dl->AddLine(ImVec2(gripMin.x, max.y - 1.5f), ImVec2(gripMax.x, max.y - 1.5f), IM_COL32(255, 210, 90, 220), 3.0f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
                if(gripHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    rowResizeTrackId = track.id;
                    rowResizeBindingId = binding.id;
                    rowResizeStartY = ImGui::GetIO().MousePos.y;
                    rowResizeStartHeight = rowHeight;
                }
                if(gripHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click the edge to restore the default height.
                    if(auto* editTrack = timeline.getTrack(track.id))
                        for(auto& editBinding : editTrack->bindings)
                            if(editBinding.id == binding.id) editBinding.rowHeight = kRowHeight;
                    rowResizeTrackId.clear();
                    rowResizeBindingId.clear();
                }
                // Press on the parameter name and drag vertically to reorder the
                // rows (a group's member rows move with their group).
                if(noteGroup == nullptr && track.bindings.size() > 1 && !gripHovered && rowResizeTrackId.empty() &&
                   hoverRect(min, ImVec2(blendChipMin.x - 2.0f, max.y)) &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    bindingDragTrackId = track.id;
                    bindingDragId = binding.id;
                    bindingDragStartY = ImGui::GetIO().MousePos.y;
                    bindingDragActive = false;
                }
                const std::string blendChipPopupId = "##blendModeChip" + track.id + binding.id;
                if(hoverRect(blendChipMin, blendChipMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    anyClipInteractionClaimedThisFrame = true; // not a clip click, but not empty timeline space either
                    ImGui::OpenPopup(blendChipPopupId.c_str());
                }
                if(ImGui::BeginPopup(blendChipPopupId.c_str())) {
                    // Flattened to just this one binding -- the track-header
                    // entry point still nests a per-binding submenu because
                    // it has to ask *which* binding first; this chip already
                    // belongs to exactly one.
                    drawBlendModeOptions(timeline, track.id, binding);
                    ImGui::EndPopup();
                }

                const std::string bindingMenuId = "##bindingMenu" + track.id + binding.id;
                if(hoverRect(min, max) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    const bool overTimelineZone = ImGui::GetIO().MousePos.x >= zoneLeft;
                    const double clickedBeat = overTimelineZone
                        ? beatAtOffset(ImGui::GetIO().MousePos.x - laneMin.x)
                        : 0.0;
                    bool overClip = false;
                    if(overTimelineZone) {
                        for(const auto& clip : track.clips) {
                            if(laneForBinding(clip, binding.id) != nullptr &&
                               clickedBeat >= clip.startBeat && clickedBeat <= clip.startBeat + clip.durationBeats) {
                                overClip = true;
                                break;
                            }
                        }
                    }
                    if(!overClip) {
                        pendingStartBeat = snapBeat(std::max(0.0, clickedBeat));
                        ImGui::OpenPopup(bindingMenuId.c_str());
                    }
                }
                if(ImGui::BeginPopup(bindingMenuId.c_str())) {
                    const std::string parameterLabel = compactParameterName(binding.parameterPath);
                    ImGui::TextUnformatted(parameterLabel.c_str());
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", binding.parameterPath.c_str());
                    contextMenuSection("Navigation");
                    {
                        // Same as the Custom GUI's "Show in Canvas": select the owning node and centre on it.
                        auto* boundParameter = container->findTimelineParameter(binding.parameterPath);
                        if(ImGui::MenuItem("Show in Canvas", nullptr, false, boundParameter != nullptr) && boundParameter != nullptr)
                            container->showParameterInCanvas(*boundParameter);
                    }
                    contextMenuSection(noteGroup == nullptr ? "Order" : "Note routing");
                    if(noteGroup == nullptr) {
                        const int unitIndex = static_cast<int>(rowUnits.size()) - 1;
                        if(ImGui::MenuItem("Move up", nullptr, false, unitIndex > 0)) {
                            deferredMoveUnitFrom = unitIndex;
                            deferredMoveUnitTo = unitIndex - 1;
                        }
                        if(ImGui::MenuItem("Move down", nullptr, false, binding.id != track.bindings.back().id)) {
                            deferredMoveUnitFrom = unitIndex;
                            deferredMoveUnitTo = unitIndex + 2; // slot after the next unit
                        }
                    } else {
                        ImGui::TextDisabled("Part of \"%s\"", noteGroup->name.c_str());
                        if(ImGui::MenuItem("Remove from note routing group")) {
                            deferredGroupId = noteGroup->id;
                            deferredGroupAction = DeferredGroupAction::LeaveGroup;
                            deferredLeaveBindingId = binding.id;
                        }
                    }
                    contextMenuSection("Create");
                    if(ImGui::MenuItem("New clip here")) {
                        pendingTrackId = track.id;
                        pendingClipBindingId = binding.id;
                        pendingClipLaneType = optionIndexForBinding(binding);
                        pendingClipName[0] = '\0';
                        pendingDurationBeats = timeline.getBeatsPerBar();
                        requestClipPopup = true;
                        ImGui::CloseCurrentPopup();
                    }
                    const bool canCaptureState = binding.valueType == typeid(float).name() ||
                        binding.valueType == typeid(int).name() || binding.valueType == typeid(bool).name() ||
                        binding.valueType == typeid(std::vector<float>).name() ||
                        binding.valueType == typeid(std::vector<int>).name() ||
                        binding.valueType == typeid(std::vector<bool>).name();
                    if(ImGui::MenuItem("Capture state as curve points", nullptr, false, canCaptureState)) {
                        timeline.captureStateAsCurvePoints(track.id, binding.id, transportState.beatPosition);
                        ImGui::CloseCurrentPopup();
                    }
                    if(ImGui::MenuItem("What drives this target?")) {
                        inspectedTargetPath = binding.parameterPath;
                        targetInspectorOpen = true;
                    }
                    contextMenuSection("Automation");
                    // When more than one binding (possibly on other tracks)
                    // drives the same parameter, this decides how this one
                    // combines with the others -- e.g. a piano-roll gate set
                    // to Multiply onto a level curve driven by Replace.
                    if(ImGui::BeginMenu("Blend mode")) {
                        drawBlendModeOptions(timeline, track.id, binding);
                        ImGui::EndMenu();
                    }
                    if(ImGui::MenuItem("Bypass", nullptr, binding.bypass)) {
                        // Stop automating this parameter (clips stay; the value is left alone).
                        if(auto* editBinding = timeline.getBinding(track.id, binding.id)) {
                            editBinding->bypass = !editBinding->bypass;
                            timeline.invalidateSchedule();
                        }
                    }
                    contextMenuSection("Remove");
                    if(ImGui::Selectable("Remove from Timeline")) {
                        removeBindingTrackId = track.id;
                        removeBindingId = binding.id;
                        requestRemoveBinding = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
                for(const auto& clip : track.clips) {
                    const auto* lane = rowLane(clip);
                    if(lane == nullptr) continue;
                    drawClip(clip, lane, laneMin, max);
                    const size_t sourceCount = std::count_if(clip.lanes.begin(), clip.lanes.end(), [&](const auto& source) {
                        return std::find(source.bindingIds.begin(), source.bindingIds.end(), binding.id) != source.bindingIds.end();
                    });
                    if(sourceCount > 1) {
                        const float badgeX = std::max(zoneLeft + 4.0f, laneMin.x + beatOffset(clip.startBeat) + 4.0f);
                        const std::string badge = ofToString(sourceCount) + " lanes -> this target";
                        if(rowHeight > 42.0f) dl->AddText(ImVec2(badgeX, max.y - 16.0f), IM_COL32(255, 235, 160, 255), badge.c_str());
                    }
                    if(clip.lanes.size() > 1) multiLaneClipRowSpans[clip.id].push_back({min.y, max.y});
                }
                clipDropRows.push_back({track.id, binding.id, min.y, max.y});
                currentRowBindingId = binding.id;
                for(auto clipIt = track.clips.rbegin(); clipIt != track.clips.rend(); ++clipIt) {
                    const auto* lane = rowLane(*clipIt);
                    if(lane != nullptr) handleClip(*clipIt, lane, laneMin, max);
                }
                currentRowBindingId.clear();
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   !clipInteractionClaimedThisFrame &&
                   hoverRect(min, max) &&
                   ImGui::GetIO().MousePos.x >= zoneLeft) {
                    emptyTrackAreaClickedThisFrame = true;
                }
                const float px = laneMin.x + beatOffset(transportState.beatPosition);
                if(px >= zoneLeft && px <= max.x) dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), kPlayhead, 2);
                dl->PopClipRect();
                ++index;
                if(noteGroup == nullptr || lastGroupMember) dockEditorHere();
            }
            // Clip context menus (opened from any row's clip), once per track.
            for(const auto& clip : track.clips) drawClipMenu(clip);
            if(!multiLaneClipRowSpans.empty()) {
                const float contentOriginX = zoneLeft - timelineScrollX;
                const float rowRight = ImGui::GetWindowPos().x + contentWidth;
                for(const auto& [clipId, rowSpans] : multiLaneClipRowSpans) {
                    if(rowSpans.size() < 2) continue;
                    const auto clipIt = std::find_if(track.clips.begin(), track.clips.end(),
                        [&](const auto& c) { return c.id == clipId; });
                    if(clipIt == track.clips.end()) continue;
                    const float x1 = contentOriginX + beatOffset(clipIt->startBeat);
                    const float x2 = contentOriginX + beatOffset(clipIt->startBeat + clipIt->durationBeats);
                    if(x2 < zoneLeft || x1 > rowRight) continue;
                    const float barX = std::max(zoneLeft + 2.0f, x1 - 4.0f);
                    // One bar per group of rows on the same side of an inline clip editor.
                    for(int side = 0; side < 2; ++side) {
                        bool any = false;
                        float top = 0.0f, bottom = 0.0f;
                        for(const auto& span : rowSpans) {
                            const bool below = editorDrawn && span.first >= editorSpanScreen.second - 0.5f;
                            if(below != (side == 1)) continue;
                            top = any ? std::min(top, span.first) : span.first;
                            bottom = any ? std::max(bottom, span.second) : span.second;
                            any = true;
                        }
                        if(any) dl->AddLine(ImVec2(barX, top + 6.0f), ImVec2(barX, bottom - 6.0f), IM_COL32(255, 210, 90, 210), 2.0f);
                    }
                    for(const auto& span : rowSpans) {
                        const float midY = (span.first + span.second) * 0.5f;
                        dl->AddLine(ImVec2(barX, midY), ImVec2(barX + 4.0f, midY), IM_COL32(255, 210, 90, 210), 2.0f);
                    }
                }
            }
            // Moves a unit (a row, or a note group with its members) to an
            // insertion slot between units, by rewriting the binding order.
            auto moveUnit = [&](int from, int slot) {
                const int unitCount = static_cast<int>(rowUnits.size());
                if(from < 0 || from >= unitCount) return;
                slot = std::max(0, std::min(slot, unitCount));
                const int to = slot > from ? slot - 1 : slot;
                if(to == from) return;
                std::vector<RowUnit> order = rowUnits;
                const RowUnit moved = order[from];
                order.erase(order.begin() + from);
                order.insert(order.begin() + to, moved);
                std::vector<std::string> ids;
                for(const auto& unit : order) ids.insert(ids.end(), unit.bindingIds.begin(), unit.bindingIds.end());
                timeline.setBindingOrder(track.id, ids);
            };
            if(deferredMoveUnitFrom >= 0) moveUnit(deferredMoveUnitFrom, deferredMoveUnitTo);
            if(deferredGroupAction != DeferredGroupAction::None) {
                switch(deferredGroupAction) {
                    case DeferredGroupAction::Ungroup:
                        timeline.removeNoteGroup(track.id, deferredGroupId, false);
                        break;
                    case DeferredGroupAction::Remove:
                        if(editorState.clipEditorOpen && editorState.editorTrackId == track.id) editorState.clipEditorOpen = false;
                        timeline.removeNoteGroup(track.id, deferredGroupId, true);
                        break;
                    case DeferredGroupAction::LeaveGroup:
                        if(const auto* group = timeline.getNoteGroup(track.id, deferredGroupId)) {
                            auto roles = *group;
                            for(auto* role : {&roles.pitchBindingId, &roles.gateBindingId, &roles.velocityBindingId})
                                if(*role == deferredLeaveBindingId) role->clear();
                            timeline.setNoteGroupRoles(track.id, deferredGroupId,
                                                       roles.pitchBindingId, roles.gateBindingId, roles.velocityBindingId);
                        }
                        break;
                    case DeferredGroupAction::NewClip: {
                        std::string laneId;
                        const std::string clipId = timeline.createNoteGroupClip(
                            track.id, deferredGroupId, std::string(), deferredClipBeat, timeline.getBeatsPerBar(), &laneId);
                        if(!clipId.empty()) {
                            if(const auto* group = timeline.getNoteGroup(track.id, deferredGroupId)) {
                                const auto members = group->members();
                                editorAnchorBindingId = members.empty() ? std::string() : members.front();
                            }
                            editorState.editorTrackId = track.id;
                            editorState.editorClipId = clipId;
                            editorState.editorLaneId = laneId;
                            editorState.clipEditorOpen = true;
                            editorState.foldedClipEditors.erase(clipId);
                        }
                        break;
                    }
                    default: break;
                }
            }
            // Row drag: show where the row (or group) will land, apply on release.
            if(bindingDragTrackId == track.id && !track.bindings.empty()) {
                const float mouseY = ImGui::GetIO().MousePos.y;
                const int unitCount = static_cast<int>(rowUnits.size());
                auto dropSlot = [&]() {
                    int slot = 0;
                    for(const auto& unit : rowUnits) if(mouseY > (unit.top + unit.bottom) * 0.5f) ++slot;
                    return std::min(slot, unitCount);
                };
                if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if(std::abs(mouseY - bindingDragStartY) > 4.0f) bindingDragActive = true;
                    if(bindingDragActive && unitCount > 0) {
                        const int slot = dropSlot();
                        const float lineY = slot < unitCount ? rowUnits[slot].top : rowUnits.back().bottom;
                        const float left = ImGui::GetWindowPos().x;
                        dl->AddLine(ImVec2(left, lineY), ImVec2(left + contentWidth, lineY),
                                    IM_COL32(255, 210, 90, 235), 2.0f);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    }
                } else {
                    if(bindingDragActive) {
                        int from = -1;
                        for(int i = 0; i < unitCount; ++i) {
                            const auto& ids = rowUnits[i].bindingIds;
                            if(std::find(ids.begin(), ids.end(), bindingDragId) != ids.end()) from = i;
                        }
                        moveUnit(from, dropSlot());
                    }
                    bindingDragTrackId.clear();
                    bindingDragId.clear();
                    bindingDragActive = false;
                }
            }
            ImGui::SetCursorPosY(rowY);
            if(editorOnThisTrack && !editorDrawn) {
                editorScreenTop = ImGui::GetCursorScreenPos().y;
                drawEditorSeparator(track, contentWidth);
                drawLaneEditor(timeline, track, contentWidth, endBeat, transportState.beatPosition);
                editorScreenBottom = ImGui::GetCursorScreenPos().y;
                editorScreenValid = true;
            }
        }
    }

    // Empty track space: a click moves the playhead (on release), a drag
    // draws a box that selects the clips it touches (Shift adds to the
    // selection).
    if(emptyTrackAreaClickedThisFrame) {
        marqueePending = true;
        marqueeActive = false;
        marqueeStartX = ImGui::GetIO().MousePos.x;
        marqueeStartY = ImGui::GetIO().MousePos.y;
        marqueeAdditive = ImGui::GetIO().KeyShift;
    }
    if(marqueePending) {
        const ImVec2 mouseNow = ImGui::GetIO().MousePos;
        const ImVec2 boxMin(std::min(marqueeStartX, mouseNow.x), std::min(marqueeStartY, mouseNow.y));
        const ImVec2 boxMax(std::max(marqueeStartX, mouseNow.x), std::max(marqueeStartY, mouseNow.y));
        auto clipsInBox = [&]() {
            std::vector<std::pair<std::string, std::string>> hits;
            for(const auto& rect : clipScreenRects) {
                if(rect.x2 < boxMin.x || rect.x1 > boxMax.x || rect.y2 < boxMin.y || rect.y1 > boxMax.y) continue;
                hits.emplace_back(rect.trackId, rect.clipId);
            }
            return hits;
        };
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if(!marqueeActive && (std::abs(mouseNow.x - marqueeStartX) >= std::max(4.0f, ImGui::GetIO().MouseDragThreshold) ||
                                  std::abs(mouseNow.y - marqueeStartY) >= std::max(4.0f, ImGui::GetIO().MouseDragThreshold))) {
                marqueeActive = true;
            }
            if(marqueeActive) {
                auto* boxDl = ImGui::GetWindowDrawList();
                boxDl->PushClipRectFullScreen();
                // Highlight what the box will select.
                for(const auto& rect : clipScreenRects) {
                    if(rect.x2 < boxMin.x || rect.x1 > boxMax.x || rect.y2 < boxMin.y || rect.y1 > boxMax.y) continue;
                    boxDl->AddRectFilled(ImVec2(rect.x1, rect.y1), ImVec2(rect.x2, rect.y2), IM_COL32(255, 255, 255, 70));
                }
                boxDl->AddRectFilled(boxMin, boxMax, IM_COL32(120, 170, 255, 40));
                boxDl->AddRect(boxMin, boxMax, IM_COL32(150, 190, 255, 200));
                boxDl->PopClipRect();
            }
        } else {
            if(marqueeActive) {
                selectedTracks.clear();
                if(!marqueeAdditive) selectedClips.clear();
                for(const auto& hit : clipsInBox()) selectedClips.insert(hit);
            } else if(container->getTransport() != nullptr) {
                const float timelineMouseX = marqueeStartX - zoneLeft + timelineScrollX;
                const double rawBeat = ofClamp(
                    pixelsToBeat(timeline, timelineMouseX, transportState.bpm, endBeat), 0.0, endBeat);
                container->getTransport()->seekToBeat(snapBeat(rawBeat));
            }
            marqueePending = false;
            marqueeActive = false;
        }
    }

    // A plain click that landed on no clip at all (every clip's handleClip
    // leaves anyClipInteractionClaimedThisFrame false) drops the current
    // multi-selection, the same way clicking empty space deselects in any
    // selection-based editor. Gated to this child window so it doesn't fire
    // for the Group/Ungroup toolbar buttons above it (those are outside
    // "##TimelineViewport" and handle the click themselves).
    // A click inside the clip editor is editing that clip, not empty space:
    // keep the selection so S / Delete / Group still act on it.
    const bool clickInEditor = editorScreenValid &&
        ImGui::GetIO().MousePos.y >= editorScreenTop && ImGui::GetIO().MousePos.y <= editorScreenBottom;
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        lastClickInEditor = clickInEditor;
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !anyClipInteractionClaimedThisFrame && !clickInEditor &&
       !ImGui::GetIO().KeyShift && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        selectedClips.clear();
        if(!trackPadClickedThisFrame) selectedTracks.clear();
    }

    if(!pendingTargetAssignment.path.empty()) {
        const auto request = std::move(pendingTargetAssignment);
        pendingTargetAssignment = {};
        timeline.bindClipTarget(request.trackId, request.clipId, request.laneId, request.path, request.role);
    }

    if(!breakClipLanesRequest.second.empty()) {
        const auto source = breakClipLanesRequest;
        breakClipLanesRequest = {};
        const auto created = timeline.breakClipLanesIntoTracks(source.first, source.second);
        if(!created.empty()) {
            selectedClips.clear(); selectedTracks.clear();
            for(const auto& clip : created) selectedTracks.insert(clip.first);
            if(editorState.editorTrackId == source.first && editorState.editorClipId == source.second) {
                editorState.clipEditorOpen = false;
                editorState.editorTrackId.clear(); editorState.editorClipId.clear(); editorState.editorLaneId.clear();
                laneEditors.clearSelection();
            }
        }
    }

    if(!duplicateTrackId.empty()) {
        const auto id = timeline.duplicateTrack(duplicateTrackId);
        duplicateTrackId.clear();
        if(!id.empty()) {
            selectedClips.clear();
            selectedTracks = {id};
        }
    }

    if(requestTrackDeletion) {
        // Clear controller-only state before the manager destroys all of the
        // track's bindings, clips and lanes.
        if(const auto* deletedTrack = timeline.getTrack(trackDeletionId)) {
            for(const auto& clip : deletedTrack->clips) {
                for(const auto& lane : clip.lanes) {
                    editorState.collapsedLaneIds.erase(lane.id);
                    editorState.laneEditorHeights.erase(lane.id);
                }
            }
        }
        if(editorState.clipEditorOpen && editorState.editorTrackId == trackDeletionId) {
            editorState.clipEditorOpen = false;
            editorState.editorTrackId.clear();
            editorState.editorClipId.clear();
            editorState.editorLaneId.clear();
            laneEditors.clearSelection();
        }
        if(draggingTrackId == trackDeletionId) {
            clipDragMode = ClipDragMode::None;
            draggingTrackId.clear();
            draggingClipId.clear();
            groupDragSnapshot.clear();
        }
        laneEditors.forgetTrack(trackDeletionId);
        // Persistent group membership for this track's clips is cleaned up
        // inside timeline.removeTrack() itself; only the ephemeral
        // selection is this controller's own responsibility.
        for(auto it = selectedClips.begin(); it != selectedClips.end();) {
            if(it->first == trackDeletionId) it = selectedClips.erase(it); else ++it;
        }
        selectedTracks.erase(trackDeletionId);
        timeline.removeTrack(trackDeletionId);
        requestTrackDeletion = false;
        trackDeletionId.clear();
    }

    if(requestClipDeletion) {
        if(const auto* deletedClip = timeline.getClip(clipDeletionTrackId, clipDeletionClipId)) {
            for(const auto& lane : deletedClip->lanes) {
                editorState.collapsedLaneIds.erase(lane.id);
                editorState.laneEditorHeights.erase(lane.id);
            }
        }
        if(editorState.clipEditorOpen && editorState.editorTrackId == clipDeletionTrackId && editorState.editorClipId == clipDeletionClipId) {
            editorState.clipEditorOpen = false;
            editorState.editorTrackId.clear();
            editorState.editorClipId.clear();
            editorState.editorLaneId.clear();
            laneEditors.clearSelection();
        }
        if(draggingTrackId == clipDeletionTrackId && draggingClipId == clipDeletionClipId) {
            clipDragMode = ClipDragMode::None;
            draggingTrackId.clear();
            draggingClipId.clear();
            groupDragSnapshot.clear();
        }
        selectedClips.erase({clipDeletionTrackId, clipDeletionClipId});
        // removeClip() itself drops this clip from any group and dissolves
        // that group if it falls below two members -- nothing further to
        // clean up on the model side here.
        timeline.removeClip(clipDeletionTrackId, clipDeletionClipId);
        requestClipDeletion = false;
        clipDeletionTrackId.clear();
        clipDeletionClipId.clear();
    }

    if(requestSeparateStateCaptureClip) {
        const std::string trackId = separateStateCaptureTrackId;
        const std::string clipId = separateStateCaptureClipId;
        requestSeparateStateCaptureClip = false;
        separateStateCaptureTrackId.clear();
        separateStateCaptureClipId.clear();
        if(timeline.separateStateCaptureClip(trackId, clipId) > 1 &&
           editorState.clipEditorOpen && editorState.editorTrackId == trackId && editorState.editorClipId == clipId) {
            // The original keeps its first lane, but the editor may have
            // been focused on any of the lanes that just moved to new clips.
            editorState.clipEditorOpen = false;
            editorState.editorTrackId.clear();
            editorState.editorClipId.clear();
            editorState.editorLaneId.clear();
        }
    }

    if(!keyboardClipDeletionRequests.empty()) {
        for(const auto& deletion : keyboardClipDeletionRequests) {
            const auto& trackId = deletion.first;
            const auto& clipId = deletion.second;
            const auto* deletedClip = timeline.getClip(trackId, clipId);
            if(deletedClip == nullptr) continue;
            for(const auto& lane : deletedClip->lanes) {
                editorState.collapsedLaneIds.erase(lane.id);
                editorState.laneEditorHeights.erase(lane.id);
            }
            if(editorState.clipEditorOpen && editorState.editorTrackId == trackId && editorState.editorClipId == clipId) {
                editorState.clipEditorOpen = false;
                editorState.editorTrackId.clear();
                editorState.editorClipId.clear();
                editorState.editorLaneId.clear();
            }
            if(draggingTrackId == trackId && draggingClipId == clipId) {
                clipDragMode = ClipDragMode::None;
                draggingTrackId.clear();
                draggingClipId.clear();
                groupDragSnapshot.clear();
            }
            timeline.removeClip(trackId, clipId);
        }
        keyboardClipDeletionRequests.clear();
    }

    if(waveFileRequest != WaveFileRequest::None) {
        const auto request = waveFileRequest;
        const std::string requestTrackId = waveFileRequestTrackId;
        const std::string requestClipId = waveFileRequestClipId;
        const double requestBeat = waveFileRequestBeat;
        waveFileRequest = WaveFileRequest::None;
        waveFileRequestTrackId.clear();
        waveFileRequestClipId.clear();
        const auto result = ofSystemLoadDialog(
            request == WaveFileRequest::ReplaceClip ? "Replace wave file" : "Load wave file", false);
        if(result.bSuccess) {
            if(request == WaveFileRequest::NewTrack) {
                // The track is only worth creating once a file is chosen --
                // cancelling used to be the difference between an empty track
                // and none at all.
                const auto trackId = timeline.createWaveTrack();
                createWaveClipFromFile(timeline, trackId, result.filePath,
                                       requestBeat, timeline.getBeatsPerBar());
            } else if(request == WaveFileRequest::NewClip) {
                createWaveClipFromFile(timeline, requestTrackId, result.filePath,
                                       requestBeat, timeline.getBeatsPerBar());
            } else {
                replaceWaveClipFromFile(timeline, requestTrackId, requestClipId, result.filePath);
            }
        }
    }

    if(requestClipSplit) {
        requestClipSplit = false;
        timeline.splitClip(clipSplitTrackId, clipSplitClipId, clipSplitBeat);
        clipSplitTrackId.clear();
        clipSplitClipId.clear();
    }
    if(requestWaveSplit) {
        const std::string splitTrackId = waveSplitTrackId;
        const std::string splitClipId = waveSplitClipId;
        requestWaveSplit = false;
        waveSplitTrackId.clear();
        waveSplitClipId.clear();
        splitWaveClipAtPlayhead(timeline, splitTrackId, splitClipId, clipSplitBeat);
    }

    if(requestRemoveBinding) {
        timeline.removeBinding(removeBindingTrackId, removeBindingId);
        requestRemoveBinding = false;
        removeBindingTrackId.clear();
        removeBindingId.clear();
    }

    if(requestAddLane) {
        requestAddLane = false;
        const int optionIndex = ofClamp(pendingAddLaneType, 0, kLaneTypeOptionCount - 1);
        const auto laneType = laneTypeFromOptionIndex(optionIndex);
        const char* laneName = kLaneTypeOptions[optionIndex];
        const auto newLaneId = timeline.createLane(editorState.editorTrackId, editorState.editorClipId, laneName, laneType);
        if(!newLaneId.empty()) {
            editorState.editorLaneId = newLaneId;
            if(!pendingAddLaneTarget.empty()) timeline.addBindingToLane(editorState.editorTrackId,
                editorState.editorClipId, newLaneId, pendingAddLaneTarget);
            pendingAddLaneTarget.clear();
        }
        if(!newLaneId.empty() && optionIndex == kToggleOptionIndex)
            makeToggleLane(timeline.getLane(editorState.editorTrackId, editorState.editorClipId, newLaneId));
    }

    if(requestDuplicateLane) {
        requestDuplicateLane = false;
        const auto newId = timeline.duplicateLane(editorState.editorTrackId, editorState.editorClipId,
                                                   pendingDuplicateLaneId);
        if(!newId.empty()) {
            const auto height = editorState.laneEditorHeights.find(pendingDuplicateLaneId);
            if(height != editorState.laneEditorHeights.end()) {
                const float copiedHeight = height->second;
                editorState.laneEditorHeights[newId] = copiedHeight;
            }
            editorState.editorLaneId = newId;
            laneEditors.clearSelection();
        }
        pendingDuplicateLaneId.clear();
    }

    if(requestRemoveLane) {
        requestRemoveLane = false;
        if(auto* clip = timeline.getClip(editorState.editorTrackId, editorState.editorClipId)) {
            if(clip->lanes.size() <= 1) {
                editorState.collapsedLaneIds.erase(pendingRemoveLaneId);
                editorState.laneEditorHeights.erase(pendingRemoveLaneId);
                timeline.removeClip(editorState.editorTrackId, editorState.editorClipId);
                editorState.clipEditorOpen = false;
                editorState.editorTrackId.clear();
                editorState.editorClipId.clear();
                editorState.editorLaneId.clear();
            } else {
                editorState.collapsedLaneIds.erase(pendingRemoveLaneId);
                editorState.laneEditorHeights.erase(pendingRemoveLaneId);
                timeline.removeLane(editorState.editorTrackId, editorState.editorClipId, pendingRemoveLaneId);
                if(editorState.editorLaneId == pendingRemoveLaneId) {
                    if(auto* clip2 = timeline.getClip(editorState.editorTrackId, editorState.editorClipId)) {
                        if(!clip2->lanes.empty()) editorState.editorLaneId = clip2->lanes.front().id;
                    }
                }
            }
        }
        pendingRemoveLaneId.clear();
    }

    if(clipDragMode != ClipDragMode::None && clipDragCommitted) {
        if(const auto* activeClip = timeline.getClip(draggingTrackId, draggingClipId)) {
            const char* action = clipDragMode == ClipDragMode::Repeat ? "Repeat source"
                : clipDragMode == ClipDragMode::Stretch ? "Stretch source"
                : clipDragMode == ClipDragMode::Resize ? "Trim / extend source window (hidden data retained)"
                : clipDragMode == ClipDragMode::TrimStart ? "Trim clip start"
                : clipDragMode == ClipDragMode::Duplicate ? "Duplicate clip" : "Move clip";
            ImGui::SetTooltip("%s\nTimeline: %.3f beats | Source: %.3f beats | Stretch: %.2f%%\nAffects %zu clip(s), with all their lanes.", action,
                activeClip->durationBeats, activeClip->contentDurationBeats,
                ofxOceanodeTimelineClipTime::effectiveStretch(*activeClip) * 100.0,
                std::max<size_t>(1, groupDragSnapshot.size()));
        }
    }
    if(clipDragMode != ClipDragMode::None) {
        // A duplicate can be dropped straight up or down onto another row,
        // so vertical movement starts that drag too.
        const float dragThreshold = std::max(3.0f, ImGui::GetIO().MouseDragThreshold);
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left) && !clipDragCommitted &&
           (std::abs(ImGui::GetIO().MousePos.x - clipDragStartMouseX) >= dragThreshold ||
            (clipDragMode == ClipDragMode::Duplicate &&
             std::abs(ImGui::GetIO().MousePos.y - clipDragStartMouseY) >= dragThreshold))) {
            clipDragCommitted = true;
        }
        if(clipDragMode == ClipDragMode::Duplicate) {
            // Where the copy would land: the row under the mouse, at the
            // snapped beat the grabbed point is dragged to.
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            const ClipDropRow* dropRow = nullptr;
            for(const auto& row : clipDropRows) {
                if(mousePos.y >= row.top && mousePos.y < row.bottom) { dropRow = &row; break; }
            }
            const auto* sourceClip = timeline.getClip(draggingTrackId, draggingClipId);
            const double dropStart = std::max(0.0, snapBeat(beatAtOffset(mousePos.x - dragTimelineOriginX) - dragOffsetBeats));
            // Same row: the whole clip. Any other row: the grabbed lane, now
            // driving that row's parameter. Another track's single row: the
            // whole clip, mapped onto that track's matching parameters.
            const bool sameRow = dropRow != nullptr && dropRow->trackId == draggingTrackId &&
                (dropRow->bindingId.empty() || dropRow->bindingId == dragSourceBindingId);
            const std::string dropBindingId = dropRow == nullptr || sameRow ? std::string() : dropRow->bindingId;
            if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if(clipDragCommitted && sourceClip != nullptr && dropRow != nullptr) {
                    ImDrawList* ghostDl = ImGui::GetWindowDrawList();
                    const float x1 = dragTimelineOriginX + beatOffset(dropStart);
                    const float x2 = dragTimelineOriginX + beatOffset(dropStart + sourceClip->durationBeats);
                    const ImVec2 ghostMin(x1, dropRow->top + 2.0f), ghostMax(x2, dropRow->bottom - 2.0f);
                    ghostDl->AddRectFilled(ghostMin, ghostMax, IM_COL32(255, 255, 255, 40), 3.0f);
                    ghostDl->AddRect(ghostMin, ghostMax, IM_COL32(255, 255, 255, 200), 3.0f, 0, 1.5f);
                    ghostDl->AddText(ImVec2(x1 + 5.0f, dropRow->top + 4.0f), IM_COL32(255, 255, 255, 220),
                                     ("+ " + sourceClip->name).c_str());
                }
            } else {
                if(clipDragCommitted && sourceClip != nullptr && dropRow != nullptr) {
                    const std::string newClipId = timeline.duplicateClip(draggingTrackId, draggingClipId,
                        dropRow->trackId, dropStart, dragSourceLaneId, dropBindingId);
                    if(newClipId.empty()) {
                        ofLogWarning("ofxOceanodeTimelineController")
                            << "Cannot duplicate \"" << sourceClip->name << "\" there: "
                            << "no matching parameter on that track, or a piano roll dropped outside a note group, "
                            << "or a Wave clip outside a Wave track";
                    } else {
                        selectedClips.clear();
                        selectedClips.insert({dropRow->trackId, newClipId});
                    }
                }
                clipDragMode = ClipDragMode::None;
                clipDragCommitted = false;
                draggingTrackId.clear();
                draggingClipId.clear();
                groupDragSnapshot.clear();
            }
        } else if(ImGui::IsMouseDown(ImGuiMouseButton_Left) && clipDragMode == ClipDragMode::TrimStart) {
            // Preview only; the trim is applied on release.
            if(clipDragCommitted) {
                if(const auto* clip = timeline.getClip(draggingTrackId, draggingClipId)) {
                    const auto* trimTrack = timeline.getTrack(draggingTrackId);
                    const double mouseBeat = beatAtOffset(ImGui::GetIO().MousePos.x - dragTimelineOriginX);
                    // Repeating automation can grow to the left (its content repeats);
                    // one-shot and Wave clips can only be shortened.
                    // Shift extends any (non-Wave, non-LFO) clip with empty time.
                    const bool canGrow = trimTrack != nullptr && !trimTrack->isWaveTrack &&
                        (clip->repeatContent || (trimExtends && !clip->isLfo));
                    const double lowest = canGrow ? 0.0 : clip->startBeat;
                    const double highest = clip->startBeat + clip->durationBeats - 1.0 / kPPQ;
                    trimPreviewBeat = ofClamp(snapBeat(mouseBeat), lowest, highest);
                    ImDrawList* trimDl = ImGui::GetWindowDrawList();
                    const float oldX = dragTimelineOriginX + beatOffset(clip->startBeat);
                    const float newX = dragTimelineOriginX + beatOffset(trimPreviewBeat);
                    if(newX > oldX)
                        trimDl->AddRectFilled(ImVec2(oldX, dragRowTop), ImVec2(newX, dragRowBottom), IM_COL32(0, 0, 0, 140));
                    else if(newX < oldX)
                        trimDl->AddRectFilled(ImVec2(newX, dragRowTop + 3.0f), ImVec2(oldX, dragRowBottom - 3.0f),
                                              trimTrack != nullptr ? IM_COL32(trimTrack->color.r, trimTrack->color.g, trimTrack->color.b, 70)
                                                                   : IM_COL32(200, 200, 200, 60));
                    trimDl->AddLine(ImVec2(newX, dragRowTop), ImVec2(newX, dragRowBottom), IM_COL32(255, 210, 90, 240), 2.0f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
        } else if(ImGui::IsMouseDown(ImGuiMouseButton_Left) && !clipDragCommitted) {
            // Not a drag yet.
        } else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if(auto* clip = timeline.getClip(draggingTrackId, draggingClipId)) {
                const double mouseBeat = beatAtOffset(ImGui::GetIO().MousePos.x - dragTimelineOriginX);
                // groupDragSnapshot holds the anchor clip itself too when a
                // drag started on a grouped/multi-selected clip -- look up
                // its own snapshot entry so the group-wide math below is
                // relative to where *everything* was when the drag began,
                // not just where the anchor is right now.
                const auto anchorSnapshot = groupDragSnapshot.empty() ? groupDragSnapshot.end()
                    : std::find_if(groupDragSnapshot.begin(), groupDragSnapshot.end(), [&](const auto& entry) {
                        return entry.trackId == draggingTrackId && entry.clipId == draggingClipId;
                    });
                if(clipDragMode == ClipDragMode::Move) {
                    const double newStart = snapBeat(mouseBeat - dragOffsetBeats);
                    if(!groupDragSnapshot.empty() && anchorSnapshot != groupDragSnapshot.end()) {
                        // Clamp the shared offset once, so the earliest member
                        // stops at beat 0 and the spacing between members holds.
                        double earliest = anchorSnapshot->startBeat;
                        for(const auto& entry : groupDragSnapshot) earliest = std::min(earliest, entry.startBeat);
                        const double deltaBeats = std::max(newStart - anchorSnapshot->startBeat, -earliest);
                        for(const auto& entry : groupDragSnapshot) {
                            timeline.setClipTiming(entry.trackId, entry.clipId,
                                                   std::max(0.0, entry.startBeat + deltaBeats), entry.durationBeats);
                        }
                    } else {
                        timeline.setClipTiming(draggingTrackId, draggingClipId, newStart, clip->durationBeats);
                    }
                }
                else {
                    const double duration = std::max(1.0 / kPPQ, snapBeat(mouseBeat - clip->startBeat));
                    if(!groupDragSnapshot.empty() && anchorSnapshot != groupDragSnapshot.end()) {
                        // Express the drag as a scale factor on the anchor
                        // clip's own original span, then apply that same
                        // factor to every member's original span (from the
                        // snapshot) around the group's shared left edge --
                        // the group-wide equivalent of what the single-clip
                        // branch below does for just one clip.
                        const double scale = duration / std::max(1.0 / kPPQ, anchorSnapshot->durationBeats);
                        for(const auto& entry : groupDragSnapshot) {
                            const double newMemberStart = groupDragAnchorBeat + (entry.startBeat - groupDragAnchorBeat) * scale;
                            const double newMemberDuration = std::max(1.0 / kPPQ, entry.durationBeats * scale);
                            timeline.setClipTiming(entry.trackId, entry.clipId, std::max(0.0, newMemberStart), newMemberDuration);
                            if(auto* member = timeline.getClip(entry.trackId, entry.clipId)) {
                                member->contentDurationBeats = entry.contentDurationBeats;
                                member->contentStretch = std::max(1.0 / 1024.0, entry.contentStretch);
                                // Restore the source mapping before applying a fresh
                                // delta; never accumulate edits over previous frames.
                                member->repeatContent = true;
                                const auto operation = clipDragMode == ClipDragMode::Repeat ? ofxOceanodeTimelineEndEdit::Repeat
                                    : clipDragMode == ClipDragMode::Stretch ? ofxOceanodeTimelineEndEdit::Stretch : ofxOceanodeTimelineEndEdit::Trim;
                                timeline.resizeClipEnd(entry.trackId, entry.clipId, newMemberDuration, operation);
                            }
                        }
                    } else {
                        clip->contentDurationBeats = dragInitialContentDuration;
                        clip->contentStretch = std::max(1.0 / 1024.0, dragInitialContentStretch);
                        clip->repeatContent = true;
                        const auto operation = clipDragMode == ClipDragMode::Repeat ? ofxOceanodeTimelineEndEdit::Repeat
                            : clipDragMode == ClipDragMode::Stretch ? ofxOceanodeTimelineEndEdit::Stretch : ofxOceanodeTimelineEndEdit::Trim;
                        timeline.resizeClipEnd(draggingTrackId, draggingClipId, duration, operation);
                    }
                }
            }
        } else {
            if(clipDragMode == ClipDragMode::TrimStart && clipDragCommitted) {
                const auto* trimTrack = timeline.getTrack(draggingTrackId);
                const auto* trimClip = timeline.getClip(draggingTrackId, draggingClipId);
                if(trimTrack != nullptr && trimClip != nullptr && std::abs(trimPreviewBeat - trimClip->startBeat) > 1e-9) {
                    if(trimTrack->isWaveTrack) trimWaveClipStart(timeline, draggingTrackId, draggingClipId, trimPreviewBeat);
                    else if(trimExtends) timeline.extendClipStart(draggingTrackId, draggingClipId, trimPreviewBeat);
                    else timeline.trimClipStart(draggingTrackId, draggingClipId, trimPreviewBeat);
                }
            }
            clipDragMode = ClipDragMode::None;
            clipDragCommitted = false;
            draggingTrackId.clear();
            draggingClipId.clear();
            groupDragSnapshot.clear();
        }
    }
    // Absolute positioning is used throughout the ruler, tracks and docked
    // editor. Always finish the scrolling child with a submitted item so a
    // state change on this frame (collapse/close/delete) cannot leave ImGui's
    // final cursor position uncommitted.
    updateNativeAudioDropPreview();
    drawAudioDropPreview(timeline);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();

    // Horizontal scrolling is manual (timelineScrollX) so the label column
    // can stay pinned (see the comment above BeginChild), which meant losing
    // ImGui's native scrollbar along with native ScrollX. This redraws an
    // equivalent scrollbar by hand, in the strip reserved below the viewport,
    // spanning only the content zone (the label column never scrolls).
    {
        const ImVec2 viewportMin = ImGui::GetItemRectMin();
        const ImVec2 viewportMax = ImGui::GetItemRectMax();
        const float trackLeft = viewportMin.x + kLabelWidth;
        const float trackRight = viewportMax.x;
        const float trackWidth = std::max(1.0f, trackRight - trackLeft);
        ImDrawList* footerDl = ImGui::GetWindowDrawList();
        const ImVec2 trackMin(trackLeft, viewportMax.y);
        const ImVec2 trackMax(trackRight, viewportMax.y + scrollbarHeight);
        footerDl->AddRectFilled(trackMin, trackMax, IM_COL32(20, 20, 20, 255));

        const float thumbWidth = maxTimelineScrollX <= 0.0f ? trackWidth
            : std::max(24.0f, trackWidth * trackWidth / (trackWidth + maxTimelineScrollX));
        const float thumbTravel = std::max(0.0f, trackWidth - thumbWidth);
        const float thumbX = trackLeft + (maxTimelineScrollX > 0.0f
            ? thumbTravel * ofClamp(timelineScrollX / maxTimelineScrollX, 0.0f, 1.0f) : 0.0f);
        const ImVec2 thumbMin(thumbX, trackMin.y + 2.0f);
        const ImVec2 thumbMax(thumbX + thumbWidth, trackMax.y - 2.0f);

        ImGui::SetCursorScreenPos(trackMin);
        ImGui::InvisibleButton("##timelineHScrollbar", ImVec2(trackWidth, scrollbarHeight));
        const bool thumbHovered = ImGui::IsItemHovered();
        const bool thumbActive = ImGui::IsItemActive();
        if(maxTimelineScrollX > 0.0f) {
            if(ImGui::IsItemClicked() && !hoverRect(thumbMin, thumbMax)) {
                const float targetThumbX = ofClamp(ImGui::GetIO().MousePos.x - thumbWidth * 0.5f,
                                                   trackLeft, trackLeft + thumbTravel);
                timelineScrollX = ofClamp((targetThumbX - trackLeft) / std::max(1.0f, thumbTravel) * maxTimelineScrollX,
                                          0.0f, maxTimelineScrollX);
            }
            if(thumbActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                timelineScrollX = ofClamp(timelineScrollX +
                    ImGui::GetIO().MouseDelta.x / std::max(1.0f, thumbTravel) * maxTimelineScrollX,
                    0.0f, maxTimelineScrollX);
            }
        }
        footerDl->AddRectFilled(thumbMin, thumbMax,
                                thumbActive ? IM_COL32(150, 150, 160, 255)
                                            : thumbHovered ? IM_COL32(120, 120, 130, 255) : IM_COL32(90, 90, 100, 255), 3.0f);
    }

    serviceClipFileRequest(timeline);

    if(requestClipPopup) { ImGui::OpenPopup("New Timeline Clip"); requestClipPopup = false; }
    drawClipPopup(timeline);
    if(requestClipRenamePopup) {
        ImGui::OpenPopup("Rename Timeline Clip");
        requestClipRenamePopup = false;
    }
    drawClipRenamePopup(timeline);
    drawClipImportPopup(timeline);
    finishHistoryGesture(timeline);
}

void ofxOceanodeTimelineController::serviceClipFileRequest(ofxOceanodeTimelineManager& timeline) {
    const auto request = clipFileRequest;
    clipFileRequest = ClipFileRequest::None;
    if(request == ClipFileRequest::None) return;
    clipFileErrorMessage.clear();
    if(request == ClipFileRequest::Save) {
        const auto* clip = timeline.getClip(clipFileTrackId, clipFileClipId);
        if(clip == nullptr) return;
        std::string name = clip->name;
        for(auto& c : name) if(c == '/' || c == '\\' || c == ':') c = '_';
        const auto result = ofSystemSaveDialog(name + ".oceanodeclip", "Save reusable clip");
        if(!result.bSuccess) return;
        std::string path = result.filePath;
        if(ofFilePath::getFileExt(path).empty()) path += ".oceanodeclip";
        if(!timeline.saveClipFile(clipFileTrackId, clipFileClipId, path, &clipFileErrorMessage))
            requestClipFileErrorPopup = true;
    } else {
        const auto result = ofSystemLoadDialog("Load reusable Oceanode clip", false);
        if(!result.bSuccess) return;
        clipImportDocument = ofxOceanodeTimelineManager::readClipFile(result.filePath, &clipFileErrorMessage);
        if(clipImportDocument.is_null()) { requestClipFileErrorPopup = true; return; }
        clipImportTargetPaths.clear();
        clipTargetSearch[0] = '\0';
        const bool wave = clipImportDocument["track"].value("isWaveTrack", false);
        const auto* track = timeline.getTrack(clipFileTrackId);
        if(track == nullptr || track->isWaveTrack != wave) clipFileTrackId.clear();
        requestClipImportPopup = true;
    }
}

void ofxOceanodeTimelineController::drawClipImportPopup(ofxOceanodeTimelineManager& timeline) {
    if(requestClipFileErrorPopup) {
        ImGui::OpenPopup("Clip file error"); requestClipFileErrorPopup = false;
    }
    if(ImGui::BeginPopupModal("Clip file error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
        ImGui::TextUnformatted(clipFileErrorMessage.c_str());
        ImGui::PopTextWrapPos();
        if(ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(requestClipImportPopup) {
        ImGui::OpenPopup("Load reusable clip"); requestClipImportPopup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Load reusable clip", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(clipImportDocument.is_null()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const auto& track = clipImportDocument["track"];
    const auto& clip = track["clips"][0];
    const std::string name = clip.value("name", std::string("Clip"));
    const bool wave = track.value("isWaveTrack", false);
    ImGui::TextWrapped("%s | %.3f beats | %zu lanes", name.c_str(), clip.value("durationBeats", 4.0),
        clip.contains("lanes") && clip["lanes"].is_array() ? clip["lanes"].size() : 0);
    const auto* destination = timeline.getTrack(clipFileTrackId);
    if(ImGui::BeginCombo("Destination track", destination ? destination->name.c_str() : "New track")) {
        if(ImGui::Selectable("New track", clipFileTrackId.empty())) clipFileTrackId.clear();
        for(const auto& candidate : timeline.getTracks()) {
            if(candidate.isWaveTrack != wave) continue;
            if(ImGui::Selectable((candidate.name + "##" + candidate.id).c_str(), clipFileTrackId == candidate.id))
                clipFileTrackId = candidate.id;
        }
        ImGui::EndCombo();
    }
    ImGui::InputDouble("Start beat", &clipImportBeat, 0.25, 1.0, "%.3f");
    if(!track["bindings"].empty()) {
        ImGui::TextWrapped("Choose a new target for each saved parameter. Unassigned lanes keep all their content; connect them later in the lane's Targets tab.");
        const auto& candidates = patchTargetPaths();
        ImGui::InputText("Filter targets", clipTargetSearch, sizeof(clipTargetSearch));
        ImGui::BeginChild("##savedClipTargets", ImVec2(0, 240), true);
        for(const auto& binding : track["bindings"]) {
            const auto id = binding.value("id", std::string());
            const auto label = binding.value("parameterPath", std::string("Target"));
            ImGui::PushID(id.c_str());
            ImGui::TextWrapped("%s", label.c_str());
            // Make the saved pitch/gate/velocity meaning visible even when node names change.
            std::set<std::string> uses;
            if(clip.value("lfoOutputBindingId", std::string()) == id) uses.insert("LFO output");
            if(clip.contains("lanes")) for(const auto& lane : clip["lanes"]) {
                const auto laneName = lane.value("name", std::string("Lane"));
                if(lane.value("pianoPitchBindingId", std::string()) == id) uses.insert(laneName + ": pitch");
                if(lane.value("pianoGateBindingId", std::string()) == id) uses.insert(laneName + ": gate");
                if(lane.value("pianoVelocityBindingId", std::string()) == id) uses.insert(laneName + ": velocity");
            }
            for(const auto& use : uses) ImGui::TextDisabled("%s", use.c_str());
            auto& selected = clipImportTargetPaths[id];
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##target", selected.empty() ? "Unassigned" : selected.c_str())) {
                if(ImGui::Selectable("Unassigned", selected.empty())) selected.clear();
                for(const auto& path : candidates) {
                    if(clipTargetSearch[0] && ofToLower(path).find(ofToLower(std::string(clipTargetSearch))) == std::string::npos) continue;
                    if(ImGui::Selectable(path.c_str(), selected == path)) selected = path;
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if(wave) {
        const auto media = clip.value("waveFilePath", std::string());
        if(!media.empty() && !ofFile::doesFileExist(media))
            ImGui::TextWrapped("Audio source is missing. You can load the clip and use Replace sample to relink it.");
    }
    if(!clipFileErrorMessage.empty()) ImGui::TextWrapped("%s", clipFileErrorMessage.c_str());
    if(ImGui::Button("Load clip")) {
        const auto imported = timeline.importClip(clipImportDocument, clipFileTrackId, clipImportBeat,
            clipImportTargetPaths, &clipFileErrorMessage);
        if(!imported.second.empty()) {
            selectedTracks.clear(); selectedClips = {imported};
            editorState.editorTrackId = imported.first;
            editorState.editorClipId = imported.second;
            const auto* added = timeline.getClip(imported.first, imported.second);
            editorState.editorLaneId = added->lanes.empty() ? std::string() : added->lanes.front().id;
            editorState.clipEditorOpen = true;
            clipImportDocument = ofJson();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        clipImportDocument = ofJson(); ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::drawPendingTrackPopup() {
    // Deliberately independent of draw() and this controller's own window:
    // see the declaration's comment in the header for why. This must work
    // even on a frame where this controller's ImGui::Begin() is never
    // called at all.
    if(container == nullptr) return;
    auto& timeline = container->getTimelineManager();
    // If the timeline was hidden last frame, commit actions performed by
    // canvas parameter menus. Visible-window gestures finish after draw(),
    // including edits applied on mouse release.
    if(lastTimelineDrawFrame < ImGui::GetFrameCount() - 1)
        finishHistoryGesture(timeline);
    beginHistoryGesture(timeline);

    for(const auto& request : timeline.consumePendingParameterLanes()) {
        const auto* track = timeline.getTrack(request.first);
        const auto* binding = timeline.getBinding(request.first, request.second);
        if(track == nullptr || track->isWaveTrack || binding == nullptr) continue;
        auto usable = [&](const std::string& id) {
            const auto* clip = timeline.getClip(request.first, id);
            return clip != nullptr && !clip->isLfo;
        };
        std::string clipId;
        // A selected clip takes priority over a different open editor.
        for(const auto& selected : selectedClips)
            if(selected.first == request.first && usable(selected.second)) { clipId = selected.second; break; }
        if(clipId.empty() && editorState.clipEditorOpen && editorState.editorTrackId == request.first && usable(editorState.editorClipId))
            clipId = editorState.editorClipId;
        const double beat = container->getTransportState().beatPosition;
        if(clipId.empty()) {
            const ofxOceanodeTimelineClip* atPlayhead = nullptr;
            const ofxOceanodeTimelineClip* onlyClip = nullptr;
            size_t count = 0;
            for(const auto& clip : track->clips) {
                if(clip.isLfo) continue;
                onlyClip = &clip; ++count;
                if(beat >= clip.startBeat && beat < clip.startBeat + clip.durationBeats &&
                   (atPlayhead == nullptr || clip.startBeat >= atPlayhead->startBeat)) atPlayhead = &clip;
            }
            if(atPlayhead != nullptr) clipId = atPlayhead->id;
            else if(count == 1) clipId = onlyClip->id;
        }
        if(clipId.empty()) clipId = timeline.createClip(request.first, track->name, snapBeat(beat), timeline.getBeatsPerBar());
        auto* clip = timeline.getClip(request.first, clipId);
        if(clip == nullptr) continue;
        std::string laneId;
        for(const auto& lane : clip->lanes)
            if(std::find(lane.bindingIds.begin(), lane.bindingIds.end(), request.second) != lane.bindingIds.end()) { laneId = lane.id; break; }
        if(laneId.empty()) {
            laneId = timeline.createLane(request.first, clipId, compactParameterName(binding->parameterPath), binding->laneType);
            timeline.addBindingToLane(request.first, clipId, laneId, request.second);
            auto* lane = timeline.getLane(request.first, clipId, laneId);
            auto* parameter = container->findTimelineParameter(binding->parameterPath);
            if(lane != nullptr && binding->valueType == typeid(bool).name()) makeToggleLane(lane);
            if(lane != nullptr && parameter != nullptr) {
                if(binding->valueType == typeid(float).name()) {
                    lane->valueMin = parameter->cast<float>().getParameter().getMin();
                    lane->valueMax = parameter->cast<float>().getParameter().getMax();
                } else if(binding->valueType == typeid(int).name()) {
                    lane->valueMin = parameter->cast<int>().getParameter().getMin();
                    lane->valueMax = parameter->cast<int>().getParameter().getMax();
                }
            }
        }
        editorState.editorTrackId = request.first; editorState.editorClipId = clipId; editorState.editorLaneId = laneId;
        editorState.clipEditorOpen = true;
        editorState.foldedClipEditors.erase(clipId); editorState.collapsedLaneIds.erase(laneId);
        selectedClips = {{request.first, clipId}}; selectedTracks.clear();
        timeline.getTrack(request.first)->collapsed = false;
        timeline.invalidateSchedule();
    }

    std::string requestedRename;
    bool isNewTrack = false;
    if(timeline.consumePendingTrackRename(requestedRename, &isNewTrack)) {
        pendingTrackId = requestedRename;
        if(const auto* track = timeline.getTrack(pendingTrackId)) {
            std::strncpy(pendingTrackName, track->name.c_str(), sizeof(pendingTrackName) - 1);
            pendingTrackName[sizeof(pendingTrackName) - 1] = '\0';
            pendingNewTrackDialog = isNewTrack;
            requestRenamePopup = true;
        }
    }
    if(requestRenamePopup) {
        // Pin it to the main viewport and center it, regardless of which
        // window/viewport happens to be focused when this fires -- see
        // drawRenamePopup for the SetNextWindowPos/SetNextWindowViewport
        // calls that actually do this.
        ImGui::OpenPopup(pendingNewTrackDialog ? "New Timeline Track" : "Rename Timeline Track");
        requestRenamePopup = false;
    }
    drawRenamePopup(timeline);

    // "Add to Timeline as Piano Roll..." from a parameter's right-click menu.
    if(auto* noteSource = timeline.consumePendingNoteGroupSetup())
        openNoteGroupSetup(timeline, noteSource, std::string(), std::string());
    if(requestNoteSetupPopup) {
        ImGui::OpenPopup("Piano Roll##noteGroupSetup");
        requestNoteSetupPopup = false;
    }
    drawNoteGroupSetupPopup(timeline);
}

void ofxOceanodeTimelineController::jumpToBeat(double beat) {
    auto transport = container == nullptr ? nullptr : container->getTransport();
    if(transport == nullptr || transport->hasExternalClock()) return;
    const auto state = transport->getState();
    if(quantizeJumps && state.isPlaying) {
        // Wait for the next bar line; the jump keeps the position within the bar.
        const double beatsPerBar = std::max(1.0, container->getTimelineManager().getBeatsPerBar());
        double at = std::ceil(state.beatPosition / beatsPerBar - 1e-9) * beatsPerBar;
        if(at <= state.beatPosition + 1e-6) at += beatsPerBar;
        pendingJump = true;
        pendingJumpTarget = std::max(0.0, beat);
        pendingJumpAt = at;
        pendingJumpLoopCount = state.loopCount;
        return;
    }
    pendingJump = false;
    transport->seekToBeat(std::max(0.0, beat));
}

void ofxOceanodeTimelineController::drawMarkerRenamePopup(ofxOceanodeTimelineManager& timeline) {
    if(requestMarkerRename) {
        ImGui::OpenPopup("Rename Marker");
        requestMarkerRename = false;
    }
    if(!ImGui::BeginPopupModal("Rename Marker", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputText("Name", markerRenameBuffer, sizeof(markerRenameBuffer),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool valid = markerRenameBuffer[0] != '\0';
    ImGui::BeginDisabled(!valid);
    const bool saved = ImGui::Button("Save");
    ImGui::EndDisabled();
    if((entered || saved) && valid) {
        timeline.renameMarker(markerRenameId, markerRenameBuffer);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::drawRuler(ofxOceanodeTimelineManager& timeline, float labelWidth,
                                              float timelineWidth, double endBeat,
                                              double beatPosition, float bpm) {
    const auto& viewState = timeline.getViewState();
    const float pixelsPerSecond = viewState.pixelsPerSecond;
    const float timelineScrollX = viewState.scrollX;
    const float width = labelWidth + timelineWidth;
    ImGui::SetNextItemAllowOverlap(); // the tempo-lane toggle sits on top of it
    ImGui::InvisibleButton("##timelineRuler", ImVec2(width, kRulerHeight));
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(35, 35, 35, 255));
    const float rulerDividerY = min.y + 31.0f;
    dl->AddLine(ImVec2(min.x, rulerDividerY), ImVec2(max.x, rulerDividerY), IM_COL32(80, 80, 80, 180));
    dl->AddText(ImVec2(min.x + 7, min.y + 8), IM_COL32(165, 165, 165, 255), "SECONDS");
    dl->AddText(ImVec2(min.x + 7, rulerDividerY + 8), IM_COL32(220, 220, 220, 255), "BEATS");
    const float zoneLeft = min.x + labelWidth;
    const ImVec2 laneMin(zoneLeft - timelineScrollX, min.y);
    dl->PushClipRect(ImVec2(zoneLeft, min.y), ImVec2(max.x, max.y), true);
    const double grid = displayGridBeats();
    const double beatsPerBar = std::max(1.0, timeline.getBeatsPerBar());
    const float barSpacingPixels = beatToPixels(timeline, beatsPerBar, bpm);
    int barLabelStep = 1;
    while(barSpacingPixels * barLabelStep < 38.0f) barLabelStep *= 2;
    // Only the lines that are on screen.
    const double visibleFromBeat = pixelsToBeat(timeline, timelineScrollX, bpm, endBeat);
    const double visibleToBeat = std::min(endBeat, pixelsToBeat(timeline, timelineScrollX + timelineWidth, bpm, endBeat) + grid);
    const int firstGridLine = std::max(0, static_cast<int>(std::floor(visibleFromBeat / grid)));
    const int gridLines = static_cast<int>(std::ceil(visibleToBeat / grid));
    for(int i = firstGridLine; i <= gridLines; ++i) {
        const double beat = i * grid;
        const float x = laneMin.x + beatToPixels(timeline, beat, bpm);
        const bool bar = std::fmod(beat, beatsPerBar) < 0.001;
        const bool quarter = std::fmod(beat, 1.0) < 0.001;
        if(bar || quarter || beatToPixels(timeline, beat + grid, bpm) - beatToPixels(timeline, beat, bpm) >= 4.0f)
            dl->AddLine(ImVec2(x, rulerDividerY), ImVec2(x, max.y), bar ? kBar : quarter ? IM_COL32(95, 95, 95, 165) : kGrid, bar ? 1.5f : 1.0f);
        const int barNumber = static_cast<int>(std::round(beat / beatsPerBar));
        if(bar && barNumber % barLabelStep == 0) {
            char beatLabel[16];
            std::snprintf(beatLabel, sizeof(beatLabel), "%d", barNumber + 1);
            dl->AddText(ImVec2(x + 5, rulerDividerY + 5), IM_COL32(235, 235, 235, 255), beatLabel);
        }
    }

    const int totalSeconds = static_cast<int>(std::floor(timeline.beatToSeconds(endBeat, bpm)));
    // Tick every second (fewer when zoomed far out); label every Nth tick so
    // labels stay about 38 px apart and don't shift while scrolling.
    const int secondsStep = std::max(1, static_cast<int>(std::ceil(3.0 / std::max(0.001f, pixelsPerSecond))));
    const int labelEvery = std::max(1, static_cast<int>(std::ceil(38.0 / std::max(0.001f, pixelsPerSecond * secondsStep))));
    const int firstSecond = std::max(0, static_cast<int>(std::floor(timelineScrollX / std::max(0.001f, pixelsPerSecond))) / secondsStep * secondsStep);
    const int lastSecond = std::min(totalSeconds, static_cast<int>(std::ceil((timelineScrollX + timelineWidth) / std::max(0.001f, pixelsPerSecond))) + secondsStep);
    for(int second = firstSecond; second <= lastSecond; second += secondsStep) {
        const float x = laneMin.x + second * pixelsPerSecond;
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, rulerDividerY), IM_COL32(90, 90, 100, 145), 1.0f);
        if((second / secondsStep) % labelEvery == 0) {
            char secondsLabel[20];
            std::snprintf(secondsLabel, sizeof(secondsLabel), "%ds", second);
            dl->AddText(ImVec2(x + 4.0f, min.y + 5.0f), IM_COL32(175, 175, 185, 255), secondsLabel);
        }
    }

    if(timeline.isLoopEnabled()) {
        const float loopX1 = laneMin.x + beatToPixels(timeline, timeline.getLoopStartBeat(), bpm);
        const float loopX2 = laneMin.x + beatToPixels(timeline, timeline.getLoopEndBeat(), bpm);
        dl->AddRectFilled(ImVec2(loopX1, min.y), ImVec2(loopX2, max.y), IM_COL32(95, 105, 220, 48));
        dl->AddRectFilled(ImVec2(loopX1 - 3.0f, min.y), ImVec2(loopX1 + 3.0f, max.y), IM_COL32(150, 165, 255, 230));
        dl->AddRectFilled(ImVec2(loopX2 - 3.0f, min.y), ImVec2(loopX2 + 3.0f, max.y), IM_COL32(150, 165, 255, 230));
    }
    // Markers: a flag in the lower part of the seconds strip, with a line down
    // through the ruler. Click jumps there, drag moves it, double-click renames,
    // right-click for the menu.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    std::string hoveredMarkerId;
    for(const auto& marker : timeline.getMarkers()) {
        const float x = laneMin.x + beatToPixels(timeline, marker.beat, bpm);
        if(x < zoneLeft - 200.0f || x > max.x) continue;
        const ImVec2 textSize = ImGui::CalcTextSize(marker.name.c_str());
        const ImVec2 flagMin(x, min.y + 15.0f), flagMax(x + textSize.x + 8.0f, rulerDividerY - 1.0f);
        const bool flagHovered = ImGui::IsItemHovered() && hoverRect(ImVec2(x - 3.0f, flagMin.y), flagMax) && mouse.x >= zoneLeft;
        if(flagHovered) hoveredMarkerId = marker.id;
        const bool active = flagHovered || markerDragId == marker.id;
        dl->AddLine(ImVec2(x, min.y + 15.0f), ImVec2(x, max.y), IM_COL32(255, 196, 64, active ? 255 : 200), 1.5f);
        dl->AddRectFilled(flagMin, flagMax, active ? IM_COL32(255, 206, 90, 255) : IM_COL32(230, 170, 50, 225), 2.0f);
        dl->AddText(ImVec2(x + 4.0f, flagMin.y + 0.5f), IM_COL32(30, 20, 0, 255), marker.name.c_str());
    }
    const float px = laneMin.x + beatToPixels(timeline, beatPosition, bpm);
    if(px >= zoneLeft && px <= max.x) dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), kPlayhead, 2.5f);
    dl->PopClipRect();

    const bool hovered = ImGui::IsItemHovered() && mouse.x >= zoneLeft;
    const double rawMouseBeat = ofClamp(
        pixelsToBeat(timeline, mouse.x - laneMin.x, bpm, endBeat), 0.0, endBeat);
    const double mouseBeat = snapBeat(rawMouseBeat);
    if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        markerMenuId = hoveredMarkerId;
        markerMenuBeat = mouseBeat;
        ImGui::OpenPopup("##rulerMarkerMenu");
    }
    if(ImGui::BeginPopup("##rulerMarkerMenu")) {
        const auto* marker = markerMenuId.empty() ? nullptr : timeline.getMarker(markerMenuId);
        if(marker != nullptr) {
            ImGui::TextUnformatted(marker->name.c_str());
            contextMenuSection("Marker");
            if(ImGui::MenuItem("Jump here")) jumpToBeat(marker->beat);
            if(ImGui::MenuItem("Rename...")) {
                markerRenameId = marker->id;
                std::strncpy(markerRenameBuffer, marker->name.c_str(), sizeof(markerRenameBuffer) - 1);
                markerRenameBuffer[sizeof(markerRenameBuffer) - 1] = '\0';
                requestMarkerRename = true;
            }
        } else {
            ImGui::TextUnformatted("Ruler");
            contextMenuSection("Markers");
            if(ImGui::MenuItem("Add marker here")) timeline.addMarker(markerMenuBeat);
            if(ImGui::MenuItem("Add marker at playhead", "M")) timeline.addMarker(snapBeat(beatPosition));
        }
        contextMenuSection("Playback");
        ImGui::MenuItem("Wait for the next bar to jump", nullptr, &quantizeJumps);
        if(marker != nullptr) {
            contextMenuSection("Remove");
            if(ImGui::MenuItem("Delete marker")) timeline.removeMarker(markerMenuId);
        }
        ImGui::EndPopup();
    }
    if(!hoveredMarkerId.empty() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if(const auto* marker = timeline.getMarker(hoveredMarkerId)) {
            markerRenameId = marker->id;
            std::strncpy(markerRenameBuffer, marker->name.c_str(), sizeof(markerRenameBuffer) - 1);
            markerRenameBuffer[sizeof(markerRenameBuffer) - 1] = '\0';
            requestMarkerRename = true;
        }
        markerDragId.clear();
    } else if(!hoveredMarkerId.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        markerDragId = hoveredMarkerId;
        markerDragStartX = mouse.x;
        markerDragMoved = false;
    } else if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const double loopLength = timeline.getBeatsPerBar();
        const double loopStart = snapBeat(std::max(0.0, mouseBeat - loopLength * 0.5));
        timeline.setLoopEnabled(true);
        timeline.setLoopRange(loopStart, loopStart + loopLength);
        loopDragMode = LoopDragMode::None;
    } else if(hovered && markerDragId.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float loopStartX = laneMin.x + beatToPixels(timeline, timeline.getLoopStartBeat(), bpm);
        const float loopEndX = laneMin.x + beatToPixels(timeline, timeline.getLoopEndBeat(), bpm);
        if(timeline.isLoopEnabled() && std::abs(mouse.x - loopStartX) <= 7.0f) {
            loopDragMode = LoopDragMode::Start;
        } else if(timeline.isLoopEnabled() && std::abs(mouse.x - loopEndX) <= 7.0f) {
            loopDragMode = LoopDragMode::End;
        } else if(timeline.isLoopEnabled() && ImGui::GetIO().KeyShift &&
                  mouseBeat > timeline.getLoopStartBeat() && mouseBeat < timeline.getLoopEndBeat()) {
            loopDragMode = LoopDragMode::Move;
            loopDragAnchorBeat = mouseBeat;
            loopDragStartBeat = timeline.getLoopStartBeat();
            loopDragEndBeat = timeline.getLoopEndBeat();
        } else if(container->getTransport() != nullptr) {
            loopDragMode = LoopDragMode::Scrub;
            container->getTransport()->seekToBeat(mouseBeat);
        }
    }
    if(loopDragMode != LoopDragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if(loopDragMode == LoopDragMode::Scrub) {
            if(container->getTransport() != nullptr) container->getTransport()->seekToBeat(mouseBeat);
        } else if(loopDragMode == LoopDragMode::Start) {
            timeline.setLoopRange(std::min(mouseBeat, timeline.getLoopEndBeat() - 1.0 / kPPQ), timeline.getLoopEndBeat());
        } else if(loopDragMode == LoopDragMode::End) {
            timeline.setLoopRange(timeline.getLoopStartBeat(), std::max(mouseBeat, timeline.getLoopStartBeat() + 1.0 / kPPQ));
        } else if(loopDragMode == LoopDragMode::Move) {
            const double length = loopDragEndBeat - loopDragStartBeat;
            const double start = std::max(0.0, loopDragStartBeat + mouseBeat - loopDragAnchorBeat);
            timeline.setLoopRange(start, start + length);
        }
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) loopDragMode = LoopDragMode::None;
    if(!markerDragId.empty()) {
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if(std::abs(mouse.x - markerDragStartX) >= std::max(3.0f, ImGui::GetIO().MouseDragThreshold)) markerDragMoved = true;
            if(markerDragMoved) {
                timeline.moveMarker(markerDragId, mouseBeat);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        } else {
            // A click (no drag) on a marker jumps there.
            if(!markerDragMoved) {
                if(const auto* marker = timeline.getMarker(markerDragId)) jumpToBeat(marker->beat);
            }
            markerDragId.clear();
        }
    }

    // Tempo (BPM) lane visibility: a checkbox in the ruler's label column, and the
    // same toggle on right-click anywhere in that column.
    {
        bool tempoVisible = timeline.isBpmLaneVisible();
        ImGui::SetCursorScreenPos(ImVec2(min.x + labelWidth - 104.0f, min.y + 5.0f));
        if(ImGui::Checkbox("Tempo lane##rulerTempoLane", &tempoVisible)) timeline.setBpmLaneVisible(tempoVisible);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Show or hide the tempo (BPM) automation row.\nHidden, tempo automation still plays.");
        if(hoverRect(min, ImVec2(zoneLeft, max.y)) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##rulerLabelMenu");
        if(ImGui::BeginPopup("##rulerLabelMenu")) {
            ImGui::TextUnformatted("Ruler");
            contextMenuSection("Lanes");
            if(ImGui::MenuItem("Show tempo (BPM) lane", nullptr, timeline.isBpmLaneVisible()))
                timeline.setBpmLaneVisible(!timeline.isBpmLaneVisible());
            ImGui::EndPopup();
        }
    }
}

void ofxOceanodeTimelineController::drawSharedClipProperties(ofxOceanodeTimelineManager& timeline,
        const ofxOceanodeTimelineTrack& track, ofxOceanodeTimelineClip& clip,
        float contentWidth, double endBeat) {
    ofxOceanodeTimelinePresentation::drawClipPropertiesPanel(timeline, track, clip,
        editorView(timeline, container->getTransportState().bpm, endBeat),
        static_cast<ofxOceanodeTimelineEndEdit>(endEditTool));
}

void ofxOceanodeTimelineController::drawTargetInspector(ofxOceanodeTimelineManager& timeline) {
    ImGui::PushID("targetInspector");
    if(ImGui::BeginChild("##panel", ImVec2(0, 280.0f), true)) {
        std::vector<std::string> paths;
        for(const auto& track : timeline.getTracks())
            for(const auto& binding : track.bindings)
                if(std::find(paths.begin(), paths.end(), binding.parameterPath) == paths.end())
                    paths.push_back(binding.parameterPath);
        if(std::find(paths.begin(), paths.end(), inspectedTargetPath) == paths.end())
            inspectedTargetPath = paths.empty() ? std::string() : paths.front();
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 70.0f));
        if(ImGui::BeginCombo("Target", inspectedTargetPath.empty() ? "No targets published" : inspectedTargetPath.c_str())) {
            for(const auto& path : paths)
                if(ImGui::Selectable(path.c_str(), path == inspectedTargetPath)) inspectedTargetPath = path;
            ImGui::EndCombo();
        }
        if(!inspectedTargetPath.empty()) {
            const auto transport = container->getTransportState();
            const auto explanation = timeline.inspectTarget(inspectedTargetPath, transport.beatPosition, transport.isPlaying);
            ImGui::Text("At beat %.3f", transport.beatPosition);
            ImGui::SameLine();
            if(ImGui::SmallButton("Show in Canvas"))
                if(auto* parameter = container->findTimelineParameter(inspectedTargetPath)) container->showParameterInCanvas(*parameter);
            ImGui::TextWrapped("Lanes combine inside a clip; latest-starting contributing clip wins per target on a track (later clip order breaks ties). Track outputs then combine in track order.");
            bool hasBase = false;
            for(const auto& binding : explanation.bindings) {
                const auto* track = timeline.getTrack(binding.trackId);
                if(track == nullptr) continue;
                ImGui::Separator();
                ImGui::TextWrapped("Track: %s | %s | %s", track->name.c_str(),
                    binding.bypass ? "Bypassed" : binding.liveOverride ? "Live override"
                    : binding.active ? "Contributing" : "No active contribution",
                    binding.active ? binding.value.c_str() : "");
                if(binding.active) {
                    ImGui::TextDisabled("%s: %s", hasBase ? "Combine with earlier tracks" : "First contribution",
                        hasBase ? ofxOceanodeTimelineManager::modeToString(binding.mode).c_str() : "Base value");
                    hasBase = true;
                }
                for(const auto& contribution : explanation.contributions) {
                    if(contribution.trackId != binding.trackId || contribution.bindingId != binding.bindingId) continue;
                    const auto* clip = timeline.getClip(binding.trackId, contribution.clipId);
                    if(clip == nullptr) continue;
                    std::string sources;
                    for(const auto& laneId : contribution.laneIds) {
                        const auto* lane = timeline.getLane(binding.trackId, contribution.clipId, laneId);
                        if(lane != nullptr) sources += (sources.empty() ? "" : " + ") + lane->name;
                    }
                    if(sources.empty()) sources = "LFO result";
                    ImGui::Bullet(); ImGui::SameLine();
                    ImGui::TextWrapped("%s / %s = %s [%s]", clip->name.c_str(), sources.c_str(), contribution.value.c_str(),
                        contribution.winningClip ? "winning clip" : binding.liveOverride ? "overridden live" : "overridden by another clip");
                }
                if(std::count_if(explanation.contributions.begin(), explanation.contributions.end(), [&](const auto& c) {
                    return c.bindingId == binding.bindingId && c.trackId == binding.trackId && c.winningClip;
                }) > 1) ImGui::TextDisabled("Lanes inside the winning clip: %s", ofxOceanodeTimelineManager::modeToString(binding.mode).c_str());
            }
            ImGui::Separator();
            if(explanation.missingTarget) ImGui::TextWrapped("Missing target: content and routing are retained. Reconnect the parameter to resume output.");
            else if(!explanation.writesValue) ImGui::TextWrapped("No timeline write at this beat. Ordinary values hold; gate-like outputs may contribute zero outside clips.");
            else {
                ImGui::TextWrapped("Final timeline output: %s", explanation.value.c_str());
                if(explanation.clamped) ImGui::TextWrapped("Before parameter range clamp: %s", explanation.unclampedValue.c_str());
                ImGui::TextDisabled("Parameter range clamp: %s", explanation.clampEnabled ? "On" : "Off");
            }
        } else ImGui::TextWrapped("Publish a node parameter to a timeline track to inspect its automation.");
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void ofxOceanodeTimelineController::drawWaveClipProperties(ofxOceanodeTimelineManager& timeline,
                                                            const ofxOceanodeTimelineTrack& track,
                                                            ofxOceanodeTimelineClip& clip) {
    if(!track.isWaveTrack) return;

    ImGui::SeparatorText("Audio clip properties");
    ImGui::Text("Name: %s", clip.name.c_str());
    if(ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click to rename this clip");
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) requestClipRename(track.id, clip);
    }
    ImGui::Text("Channels: %d   Duration: %.3fs", clip.waveNumChannels,
                clip.waveFileDurationMs / 1000.0);
    const std::string sampleName = clip.waveFilePath.empty()
        ? "No file loaded" : ofFilePath::getFileName(clip.waveFilePath);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 4.0f);
    ImGui::TextWrapped("Sample: %s", sampleName.c_str());
    ImGui::PopTextWrapPos();
    if(!clip.waveFilePath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", clip.waveFilePath.c_str());
    if(ImGui::Button("Replace sample", ImVec2(-1.0f, 0.0f))) {
        waveFileRequest = WaveFileRequest::ReplaceClip;
        waveFileRequestTrackId = track.id;
        waveFileRequestClipId = clip.id;
    }
    if(ImGui::Button("Reload waveform", ImVec2(-1.0f, 0.0f))) timeline.reloadWaveform(track.id, clip.id);

    ImGui::TextUnformatted("Playback rate");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##wavePlaybackRate", &clip.wavePlaybackRate, 0.01f, 0.1f, 4.0f, "%.2fx");
    ImGui::Checkbox("Reverse", &clip.waveReverse);
    ImGui::TextUnformatted("Clip gain");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##waveGain", &clip.waveGain, 0.01f, 0.0f, 4.0f, "%.2f");
    ImGui::TextWrapped("Clip gain affects this sample. Track volume affects all clips.");
    ImGui::Separator();
}

void ofxOceanodeTimelineController::drawWaveTrackEditor(
    ofxOceanodeTimelineManager& timeline, const ofxOceanodeTimelineTrack& track,
    float contentWidth, double endBeat, double beatPosition) {
    const float timelineScrollX = timeline.getViewState().scrollX;
    auto* editTrack = timeline.getTrack(track.id);
    auto* clip = timeline.getClip(editorState.editorTrackId, editorState.editorClipId);
    if(editTrack == nullptr || clip == nullptr) {
        editorState.clipEditorOpen = false;
        return;
    }

    drawSharedClipProperties(timeline, track, *clip, contentWidth, endBeat);
    if(ImGui::SmallButton("Close editor##wave")) editorState.clipEditorOpen = false;
    const float editorHeight = 300.0f;
    ImGui::Dummy(ImVec2(contentWidth, editorHeight));
    const ImVec2 editorMin = ImGui::GetItemRectMin();
    const float gap = 8.0f;
    const float propertiesWidth = std::min(kLabelWidth - 10.0f, contentWidth * 0.38f);
    const float automationWidth = std::max(40.0f, contentWidth - propertiesWidth - gap);

    ImGui::SetCursorScreenPos(editorMin);
    // Let the properties column scroll vertically if a narrow panel needs it;
    // the sample label and controls must never paint outside the column.
    ImGui::BeginChild("##waveClipPropertiesPanel", ImVec2(propertiesWidth, editorHeight), true);
    drawWaveClipProperties(timeline, track, *clip);
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(editorMin.x + propertiesWidth + gap, editorMin.y));
    ImGui::BeginChild("##waveTrackVolumePanel", ImVec2(automationWidth, editorHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextWrapped("Track: %s / Volume automation - applies to every audio clip", track.name.c_str());
    waveVolumeEditor.draw(timeline, *editTrack,
                          editorView(timeline, container->getTransportState().bpm, endBeat),
                          automationWidth, std::max(40.0f, ImGui::GetContentRegionAvail().y), beatPosition,
                          editorMin.x + kLabelWidth - timelineScrollX);
    ImGui::EndChild();
    finishAbsoluteLayout(ImVec2(editorMin.x, editorMin.y + editorHeight));
}

void ofxOceanodeTimelineController::drawLaneEditor(ofxOceanodeTimelineManager& timeline,
                                                   const ofxOceanodeTimelineTrack& track,
                                                   float contentWidth, double endBeat,
                                                   double beatPosition) {
    const float timelineScrollX = timeline.getViewState().scrollX;
    const ScheduledContentEditGuard scheduledEdits(timeline, editorState.editorTrackId, editorState.editorClipId);
    auto* clip = timeline.getClip(editorState.editorTrackId, editorState.editorClipId);
    if(clip == nullptr || (clip->lanes.empty() && !track.isWaveTrack)) {
        editorState.clipEditorOpen = false;
        return;
    }
    if(clip->isLfo) {
        lfoEditor.setTargetPaths(patchTargetPaths());
        if(lfoEditor.draw(timeline, *container, track, *clip,
                          editorView(timeline, container->getTransportState().bpm, endBeat),
                          editorState, contentWidth, beatPosition, static_cast<ofxOceanodeTimelineEndEdit>(endEditTool)))
            requestClipRename(track.id, *clip);
        const auto path = lfoEditor.consumeOutputTargetRequest();
        if(!path.empty()) pendingTargetAssignment = {track.id, clip->id, {}, path, ofxOceanodeTimelineTargetRole::LfoOutput};
        return;
    }
    if(!clip->lanes.empty() && timeline.getLane(editorState.editorTrackId, editorState.editorClipId, editorState.editorLaneId) == nullptr) {
        editorState.editorLaneId = clip->lanes.front().id;
    }
    const float fallbackBpm = container->getTransportState().bpm;
    auto beatOffset = [&](double beat) { return beatToPixels(timeline, beat, fallbackBpm); };
    auto beatAtOffset = [&](float pixels) { return pixelsToBeat(timeline, pixels, fallbackBpm, endBeat); };

    // A clip can hold more than one lane (e.g. a curve and a step pattern
    // combined together). The clip gets one header row (name / add lane /
    // close); each lane below it is its own independently collapsible
    // block, stacked vertically and indented to read as nested under the
    // clip. Only the focused lane (editorState.editorLaneId) reacts to canvas
    // clicks/drags -- the drag/selection state further down (piano note
    // drag, curve point drag, step painting) is shared single-lane state,
    // so letting two expanded lanes both respond to the mouse in the same
    // frame would let them stomp on each other's notes/points. Click a
    // lane's header (or its canvas) to focus it before editing it.
    const float clipIndent = 20.0f;
    ImGui::Dummy(ImVec2(contentWidth, 22.0f));
    {
        const ImVec2 clipHeaderMin = ImGui::GetItemRectMin();
        const ImVec2 clipHeaderMax = ImGui::GetItemRectMax();
        const float headerZoneLeft = clipHeaderMin.x + kLabelWidth;
        ImDrawList* headerDl = ImGui::GetWindowDrawList();
        headerDl->AddRectFilled(ImVec2(clipHeaderMin.x + clipIndent, clipHeaderMin.y), ImVec2(headerZoneLeft, clipHeaderMax.y),
                                mutedTrackColor(track.color, 0.25f, 0.44f));
        headerDl->AddLine(ImVec2(headerZoneLeft - 1.0f, clipHeaderMin.y), ImVec2(headerZoneLeft - 1.0f, clipHeaderMax.y),
                          IM_COL32(track.color.r, track.color.g, track.color.b, 210));
        ImGui::SetCursorScreenPos(ImVec2(clipHeaderMin.x + 5.0f + clipIndent, clipHeaderMin.y + 2.0f));
        const bool folded = editorState.foldedClipEditors.count(clip->id) > 0;
        if(ImGui::SmallButton(folded ? ">##foldClipEditor" : "v##foldClipEditor")) {
            if(folded) editorState.foldedClipEditors.erase(clip->id); else editorState.foldedClipEditors.insert(clip->id);
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip(folded ? "Unfold the clip editor" : "Fold the clip editor to this header");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(track.color.r / 255.0f, track.color.g / 255.0f, track.color.b / 255.0f, 1.0f),
                           "%s", clip->name.c_str());
        if(ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Double-click to rename this clip");
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) requestClipRename(track.id, *clip);
        }
        ImGui::SameLine();
        if(ImGui::SmallButton("+ Lane##addLane")) {
            pendingAddLaneTarget.clear();
            pendingAddLaneChoice = 1;
            ImGui::OpenPopup("##addLaneTypePopup");
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add another lane to this clip");
        ImGui::SameLine();
        if(ImGui::SmallButton("Close##clipEditor")) editorState.clipEditorOpen = false;
        if(ImGui::BeginPopup("##addLaneTypePopup")) {
            if(track.isWaveTrack) {
                if(ImGui::MenuItem("Edit track volume automation")) {
                    timeline.createWaveVolumeLane(track.id, clip->id);
                    editorState.editorLaneId.clear();
                }
            } else {
                ImGui::TextUnformatted("New lane in this clip");
                ImGui::Combo("Content", &pendingAddLaneChoice, kLaneTypeOptions, kLaneTypeOptionCount);
                ImGui::TextWrapped("%s", contentDescription(pendingAddLaneChoice));
                const auto* selected = timeline.getBinding(track.id, pendingAddLaneTarget);
                if(ImGui::BeginCombo(pendingAddLaneChoice == 2 ? "Pitch target" : "Target",
                                    selected == nullptr ? "No target (assign later)" : selected->parameterPath.c_str())) {
                    if(ImGui::Selectable("No target (assign later)", selected == nullptr)) pendingAddLaneTarget.clear();
                    for(const auto& binding : track.bindings)
                        if(ImGui::Selectable(binding.parameterPath.c_str(), binding.id == pendingAddLaneTarget)) pendingAddLaneTarget = binding.id;
                    ImGui::EndCombo();
                }
                if(pendingAddLaneChoice == 2) ImGui::TextWrapped("Assign pitch, gate and velocity in the lane's Targets tab after creating it.");
                ImGui::TextWrapped("All lanes share this clip's timing, stretch and repeats.");
                if(ImGui::Button("Add lane")) {
                    requestAddLane = true;
                    pendingAddLaneType = pendingAddLaneChoice;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        finishAbsoluteLayout(ImVec2(clipHeaderMin.x, clipHeaderMax.y));
    }
    if(editorState.foldedClipEditors.count(clip->id) > 0) return; // folded: only the header row is shown
    // Lane timing lives in the pinned properties pad, alongside Targets and Content.
    if(clip->lanes.empty()) drawSharedClipProperties(timeline, track, *clip, contentWidth, endBeat);

    if(track.isWaveTrack && clip != nullptr)
        drawWaveClipProperties(timeline, track, *clip);

    if(clip->lanes.empty()) return;

    const auto& availableTargets = patchTargetPaths();
    for(size_t laneIndex = 0; laneIndex < clip->lanes.size(); ++laneIndex) {
    auto* lane = &clip->lanes[laneIndex];
    const bool isFocused = lane->id == editorState.editorLaneId;
    const auto view = editorView(timeline, fallbackBpm, endBeat);
    ofxOceanodeTimelineLaneEditorContext context{timeline, *container, track, *clip, *lane,
                                               view, editorState.editorLaneId, isFocused, {}};
    context.targetPaths = availableTargets;
    auto& laneEditor = laneEditors.forType(lane->type);
    auto drawLaneMenu = [&](bool propertiesPanel) {
        const std::string menuId = "##laneMenu" + lane->id;
        // The entire properties pad owns the menu, including controls and
        // empty space. Collapsed lanes retain their header context menu.
        const bool open = propertiesPanel
            ? ImGui::BeginPopupContextWindow(menuId.c_str(), ImGuiPopupFlags_MouseButtonRight)
            : ImGui::BeginPopupContextItem(menuId.c_str());
        if(open) {
            editorState.editorLaneId = lane->id;
            if(ImGui::MenuItem("Duplicate lane")) {
                requestDuplicateLane = true;
                pendingDuplicateLaneId = lane->id;
            }
            const bool lastLane = clip->lanes.size() == 1;
            if(ImGui::MenuItem(lastLane ? "Delete lane and clip" : "Delete lane")) {
                requestRemoveLane = true;
                pendingRemoveLaneId = lane->id;
            }
            ImGui::EndPopup();
        }
    };
    auto drawLanePlayback = [&]() {
        if(playbackToggle("laneMute" + lane->id, "M", lane->muted, "Mute lane"))
            timeline.setLaneMuted(track.id, clip->id, lane->id, !lane->muted);
        ImGui::SameLine();
        if(playbackToggle("laneSolo" + lane->id, "S", lane->solo, "Solo lane within this clip (multiple solos allowed)"))
            timeline.setLaneSolo(track.id, clip->id, lane->id, !lane->solo);
        ImGui::SameLine();
    };
    const std::string laneLabel = lane->name.empty() ? laneDisplayName(*lane) : lane->name;
    if(editorState.collapsedLaneIds.count(lane->id) > 0) {
        ImGui::Dummy(ImVec2(contentWidth, 22.0f));
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float rowZoneLeft = rowMin.x + kLabelWidth;
        ImDrawList* rowDl = ImGui::GetWindowDrawList();
        rowDl->AddRectFilled(ImVec2(rowMin.x + clipIndent, rowMin.y), ImVec2(rowZoneLeft, rowMax.y), mutedTrackColor(track.color, 0.18f, 0.30f));
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 5.0f + clipIndent, rowMin.y + 2.0f));
        drawLanePlayback();
        if(ImGui::SmallButton((">##laneExpand" + lane->id).c_str())) editorState.collapsedLaneIds.erase(lane->id);
        ImGui::SameLine();
        ImGui::Selectable((laneLabel + "##laneHeaderCollapsed" + lane->id).c_str(), isFocused,
                          ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0));
        if(ImGui::IsItemClicked()) editorState.editorLaneId = lane->id;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Track: %s > Clip: %s > Lane: %s\n%s / %zu target(s)%s",
            track.name.c_str(), clip->name.c_str(), laneLabel.c_str(), laneDisplayName(*lane), lane->bindingIds.size(),
            lane->bindingIds.empty() ? " - No target" : "");
        drawLaneMenu(false);
        finishAbsoluteLayout(ImVec2(rowMin.x, rowMax.y));
        continue;
    }

    const float defaultEditorHeight = laneEditors.defaultHeight(lane->type);
    const auto laneHeightIt = editorState.laneEditorHeights.find(lane->id);
    const float editorHeight = laneHeightIt != editorState.laneEditorHeights.end() ? laneHeightIt->second : defaultEditorHeight;
    // Reserve the whole dock row first, then give only the timeline half an
    // input item. This leaves the left configuration zone interactive.
    ImGui::Dummy(ImVec2(contentWidth, editorHeight));
    const ImVec2 editorMin = ImGui::GetItemRectMin();
    const ImVec2 editorMax = ImGui::GetItemRectMax();
    const float zoneLeft = editorMin.x + kLabelWidth;
    const ImVec2 timelineMin(zoneLeft - timelineScrollX, editorMin.y);
    const float editorCanvasStartX = std::max(zoneLeft, timelineMin.x);
    ImGui::SetCursorScreenPos(ImVec2(editorCanvasStartX, timelineMin.y));
    ImGui::InvisibleButton(("##clipEditorCanvas" + editorState.editorTrackId + editorState.editorClipId + lane->id).c_str(),
                           ImVec2(std::max(1.0f, contentWidth - kLabelWidth - (editorCanvasStartX - timelineMin.x)),
                                  std::max(1.0f, editorHeight - kLaneResizeHandleHeight)));
    const bool editorCanvasHovered = ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x >= zoneLeft;
    if(!isFocused && editorCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) editorState.editorLaneId = lane->id;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Leave the hierarchy gutter to the left of the lane indentation
    // untouched; both the editor and its properties panel belong visually
    // below the indented clip header.
    dl->AddRectFilled(ImVec2(editorMin.x + clipIndent, editorMin.y), editorMax,
                      mutedTrackColor(track.color, 0.13f, 0.26f));
    dl->AddRectFilled(ImVec2(editorMin.x + clipIndent, editorMin.y), ImVec2(zoneLeft, editorMax.y),
                      mutedTrackColor(track.color, 0.25f, 0.44f));
    dl->AddLine(ImVec2(zoneLeft - 1.0f, editorMin.y), ImVec2(zoneLeft - 1.0f, editorMax.y), IM_COL32(track.color.r, track.color.g, track.color.b, isFocused ? 210 : 120));

    // The bottom strip is excluded from both the canvas item and properties
    // child. Track the drag directly from its rectangle so later editor
    // widgets cannot replace the handle's active ImGui id mid-drag.
    {
        const ImVec2 handleMin(editorMin.x + clipIndent, editorMax.y - kLaneResizeHandleHeight);
        const ImVec2 handleMax(editorMax.x, editorMax.y);
        const bool resizeHovered = hoverRect(handleMin, handleMax);
        if(resizeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            editorState.resizingLaneId = lane->id;
            editorState.laneResizeStartMouseY = ImGui::GetIO().MousePos.y;
            editorState.laneResizeStartHeight = editorHeight;
            editorState.editorLaneId = lane->id;
        }
        const bool resizeActive = editorState.resizingLaneId == lane->id && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if(resizeHovered || resizeActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(resizeActive) {
            editorState.laneEditorHeights[lane->id] = ofClamp(editorState.laneResizeStartHeight + ImGui::GetIO().MousePos.y - editorState.laneResizeStartMouseY,
                                                  kLaneEditorMinHeight, kLaneEditorMaxHeight);
        }
        if(editorState.resizingLaneId == lane->id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) editorState.resizingLaneId.clear();
        const float separatorY = editorMax.y - kLaneResizeHandleHeight * 0.5f;
        dl->AddLine(ImVec2(handleMin.x, separatorY), ImVec2(handleMax.x, separatorY),
                    resizeActive ? IM_COL32(205, 205, 215, 235)
                                 : resizeHovered ? IM_COL32(160, 160, 170, 205)
                                                 : IM_COL32(80, 80, 86, 110),
                    resizeActive ? 2.0f : 1.0f);
    }

    ImGui::SetCursorScreenPos(ImVec2(editorMin.x + 5.0f + clipIndent, editorMin.y + 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 4.0f));
    const float pianoKeyboardReserve = laneEditors.keyboardReserve(lane->type);
    ImGui::BeginChild(("##clipProperties" + editorState.editorTrackId + editorState.editorClipId + lane->id).c_str(),
                      ImVec2(kLabelWidth - 10.0f - clipIndent - pianoKeyboardReserve,
                             editorHeight - 8.0f - kLaneResizeHandleHeight), false,
                      ImGuiWindowFlags_None);
    drawLanePlayback();
    if(ImGui::SmallButton(("v##laneCollapse" + lane->id).c_str())) editorState.collapsedLaneIds.insert(lane->id);
    ImGui::SameLine();
    if(ImGui::Selectable((laneLabel + "##laneHeader" + lane->id).c_str(), isFocused,
                         ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0))) {
        editorState.editorLaneId = lane->id;
    }
    ImGui::TextDisabled("%s%s", laneDisplayName(*lane), isFocused ? " / Editing" : "");
    if(lane->bindingIds.empty()) ImGui::TextDisabled("No target");
    if(ImGui::BeginTabBar("##clipPropertyTabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        if(ImGui::BeginTabItem("Targets")) {
            if(lane->type == ofxOceanodeTimelineLaneType::PianoRoll) {
                laneEditor.drawTargets(context);
            } else {
                ImGui::SetNextItemWidth(-1.0f);
                if(ImGui::BeginCombo("##addPatchTarget", "Add target...")) {
                    ImGui::InputText("Search parameters", laneTargetSearch, sizeof(laneTargetSearch));
                    for(const auto& path : context.targetPaths) {
                        if(laneTargetSearch[0] && ofToLower(path).find(ofToLower(std::string(laneTargetSearch))) == std::string::npos) continue;
                        if(ImGui::Selectable(path.c_str())) context.requestedTargetPath = path;
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped("This lane drives:");
                if(lane->bindingIds.empty()) ImGui::TextDisabled("No target");
                for(const auto& candidate : track.bindings) {
                    const bool assigned = std::find(lane->bindingIds.begin(), lane->bindingIds.end(), candidate.id) != lane->bindingIds.end();
                    bool enabled = assigned;
                    if(ImGui::Checkbox((compactParameterName(candidate.parameterPath) + "##target" + candidate.id).c_str(), &enabled)) {
                        if(enabled) timeline.addBindingToLane(track.id, clip->id, lane->id, candidate.id);
                        else timeline.removeBindingFromLane(track.id, clip->id, lane->id, candidate.id);
                    }
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", candidate.parameterPath.c_str(), candidate.missingTarget ? " (missing target)" : "");
                    if(assigned && ImGui::SmallButton(("Inspect##" + candidate.id).c_str())) {
                        inspectedTargetPath = candidate.parameterPath;
                        targetInspectorOpen = true;
                    }
                }
            }
            if(!context.requestedTargetPath.empty())
                pendingTargetAssignment = {track.id, clip->id, lane->id, context.requestedTargetPath, context.requestedTargetRole};
            if(!context.noteGroupToEdit.empty()) openNoteGroupSetup(timeline, nullptr, track.id, context.noteGroupToEdit);
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Content")) {
            char laneName[256];
            std::snprintf(laneName, sizeof(laneName), "%s", lane->name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::InputText("##laneName", laneName, sizeof(laneName))) lane->name = laneName;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Lane name. Editor type: %s", laneDisplayName(*lane));
            int type = optionIndexForLane(*lane);
            ImGui::SetNextItemWidth(-1.0f);
            if(ImGui::Combo("##laneType", &type, kLaneTypeOptions, kLaneTypeOptionCount)) {
                timeline.setClipLaneType(track.id, clip->id, lane->id, laneTypeFromOptionIndex(type));
                if(type == kToggleOptionIndex) makeToggleLane(lane);
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", contentDescription(type));
            if(lane->type != ofxOceanodeTimelineLaneType::PianoRoll &&
               lane->type != ofxOceanodeTimelineLaneType::MultiGate &&
               !ofxOceanodeTimelineIsHeldValueLane(lane->type)) {
                ImGui::TextUnformatted("Output range");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat("##rangeMin", &lane->valueMin, 0.01f, -99999, 99999, "Min %.4g");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat("##rangeMax", &lane->valueMax, 0.01f, -99999, 99999, "Max %.4g");
            }
            laneEditors.forType(lane->type).drawProperties(context);
            if(!context.noteGroupToEdit.empty())
                openNoteGroupSetup(timeline, nullptr, track.id, context.noteGroupToEdit);

            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Timing")) {
            ImGui::TextUnformatted("Clip timing");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Clip: %s\nTiming applies to every lane in this clip.", clip->name.c_str());
            ofxOceanodeTimelinePresentation::drawClipTiming(timeline, track, *clip, view,
                static_cast<ofxOceanodeTimelineEndEdit>(endEditTool), false);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    drawLaneMenu(true);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    finishAbsoluteLayout(ImVec2(editorMin.x, editorMax.y));

    const float clipX1 = timelineMin.x + beatOffset(clip->startBeat);
    const float clipX2 = timelineMin.x + beatOffset(clip->startBeat + clip->durationBeats);
    const float left = std::max(zoneLeft, clipX1);
    const float right = std::min(editorMax.x, clipX2);
    if(right <= left) {
        finishAbsoluteLayout(ImVec2(editorMin.x, editorMax.y));
        continue;
    }

    // Everything below draws actual clip content (notes / curve / steps),
    // which must never visually bleed into the pinned properties column
    // (and, for a piano roll, its pinned keyboard strip) to its left.
    dl->PushClipRect(ImVec2(zoneLeft - pianoKeyboardReserve, editorMin.y), ImVec2(editorMax.x, editorMax.y), true);

    laneEditors.forType(lane->type).drawCanvas(context, {editorMin, editorMax, timelineMin,
                                   zoneLeft, left, right, editorCanvasHovered, beatPosition});
    const float playheadX = timelineMin.x + beatOffset(beatPosition);
    if(playheadX >= zoneLeft && playheadX <= editorMax.x) dl->AddLine(ImVec2(playheadX, editorMin.y), ImVec2(playheadX, editorMax.y), kPlayhead, 2.0f);
    dl->PopClipRect();
    finishAbsoluteLayout(ImVec2(editorMin.x, editorMax.y));
    } // end for(laneIndex : clip->lanes)
}

namespace {
// How strongly a parameter name suggests a piano-roll role (0 pitch, 1 gate,
// 2 velocity): 2 for the plain names ("Gate", "Pitch", "Velocity"), 1 when the
// name contains one, 0 for none. Names of controls that only shape a note
// (an envelope's velocity, a key-tracking amount, a jitter) suggest nothing.
int noteRoleScore(const std::string& name, int role) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    auto has = [&](const char* word) { return lower.find(word) != std::string::npos; };
    static const std::vector<std::vector<std::string>> exact = {
        {"pitch", "note", "midi", "midi note", "notes", "pitches"},
        {"gate", "gates", "trigger/gate", "trigger", "trig", "on", "note on"},
        {"velocity", "vel", "velocities"}};
    if(std::find(exact[role].begin(), exact[role].end(), lower) != exact[role].end()) return 2;
    if(has("env") || has("track") || has("jit") || has("rand") || has("mod") || has("sens") ||
       has("glide") || has("time") || has("rate")) return 0;
    switch(role) {
        case 0: return has("pitch") || has("note") ? 1 : 0;
        case 1: return has("gate") || has("trig") ? 1 : 0;
        default: return has("velocity") ? 1 : 0;
    }
}

bool isNoteRoleType(const std::string& valueType) {
    return valueType == typeid(float).name() || valueType == typeid(int).name() || valueType == typeid(bool).name() ||
           valueType == typeid(std::vector<float>).name() || valueType == typeid(std::vector<int>).name();
}

bool containsCaseInsensitive(const std::string& text, const char* filter) {
    if(filter == nullptr || filter[0] == '\0') return true;
    std::string a = text, b = filter;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return std::tolower(c); });
    return a.find(b) != std::string::npos;
}
}

void ofxOceanodeTimelineController::openNoteGroupSetup(ofxOceanodeTimelineManager& timeline,
                                                      ofxOceanodeAbstractParameter* source,
                                                      const std::string& trackId, const std::string& editGroupId,
                                                      double clipStart, double clipDuration) {
    noteSetupCandidates.clear();
    noteSetupInstruments.clear();
    noteSetupInstrument = -1;
    noteSetupVoices = 0;
    for(auto& path : noteSetupRolePaths) path.clear();
    noteSetupFilter[0] = '\0';
    noteSetupTrackId = trackId;
    noteSetupTrackFixed = !trackId.empty();
    noteSetupEditGroupId = editGroupId;
    const auto transportState = container->getTransportState();
    noteSetupClipStart = clipStart >= 0.0 ? clipStart : snapBeat(std::max(0.0, transportState.beatPosition));
    noteSetupClipDuration = clipDuration > 0.0 ? clipDuration : timeline.getBeatsPerBar();

    // Parameters the roles can use: the source node's first, then every node in the patch.
    ofxOceanodeNodeModel* sourceModel = source == nullptr ? nullptr : source->getNodeModel();
    auto addFromGroup = [&](ofParameterGroup& group, bool sameNode) {
        for(auto& entry : group) {
            auto parameter = std::dynamic_pointer_cast<ofxOceanodeAbstractParameter>(entry);
            if(parameter == nullptr || !isNoteRoleType(parameter->valueType())) continue;
            if(parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly) continue;
            if(std::any_of(noteSetupCandidates.begin(), noteSetupCandidates.end(),
                           [&](const auto& c) { return c.parameter == parameter.get(); })) continue;
            NoteRoleCandidate candidate;
            candidate.parameter = parameter.get();
            candidate.path = container->getTimelineParameterPath(*parameter);
            candidate.label = candidate.path;
            candidate.sameNode = sameNode;
            noteSetupCandidates.push_back(candidate);
        }
    };
    // An instrument node: its note inputs as one choice.
    int sourceInstrument = -1;
    auto addInstrument = [&](ofxOceanodeNodeModel* model) {
        ofxOceanodeTimelineInstrument instrument;
        if(model == nullptr || !model->getTimelineInstrument(instrument)) return;
        NoteInstrumentCandidate candidate;
        candidate.label = model->getParameterGroup().getName();
        ofxOceanodeAbstractParameter* roles[3] = {instrument.pitch, instrument.gate, instrument.velocity};
        for(int role = 0; role < 3; ++role)
            if(roles[role] != nullptr) candidate.rolePaths[role] = container->getTimelineParameterPath(*roles[role]);
        if(candidate.rolePaths[0].empty() && candidate.rolePaths[1].empty()) return;
        candidate.voices = instrument.voices;
        candidate.kit = instrument.kit;
        if(model == sourceModel) sourceInstrument = static_cast<int>(noteSetupInstruments.size());
        noteSetupInstruments.push_back(candidate);
    };
    if(sourceModel != nullptr) {
        addFromGroup(sourceModel->getParameterGroup(), true);
        addInstrument(sourceModel);
    }
    ofxOceanodeContainer* candidateContainer = container.get();
    if(sourceModel != nullptr && sourceModel->getHostContainer() != nullptr)
        candidateContainer = sourceModel->getHostContainer();
    for(auto* node : candidateContainer->getAllModules()) {
        if(node == nullptr) continue;
        addFromGroup(node->getParameters(), false);
        if(&node->getNodeModel() != sourceModel) addInstrument(&node->getNodeModel());
    }
    // The instrument whose note inputs are exactly these roles, if any.
    auto matchInstrument = [&]() {
        for(size_t i = 0; i < noteSetupInstruments.size(); ++i) {
            const auto& candidate = noteSetupInstruments[i];
            if(std::equal(std::begin(candidate.rolePaths), std::end(candidate.rolePaths), std::begin(noteSetupRolePaths)))
                return static_cast<int>(i);
        }
        return -1;
    };

    if(!editGroupId.empty()) {
        // Retargeting: start from the group's current roles.
        if(const auto* group = timeline.getNoteGroup(trackId, editGroupId)) {
            const std::string* roles[3] = {&group->pitchBindingId, &group->gateBindingId, &group->velocityBindingId};
            for(int role = 0; role < 3; ++role) {
                if(const auto* binding = roles[role]->empty() ? nullptr : timeline.getBinding(trackId, *roles[role]))
                    noteSetupRolePaths[role] = binding->parameterPath;
            }
            std::strncpy(noteSetupName, group->name.c_str(), sizeof(noteSetupName) - 1);
            noteSetupName[sizeof(noteSetupName) - 1] = '\0';
            noteSetupVoices = group->voices;
            noteSetupInstrument = matchInstrument();
        }
    } else {
        if(sourceInstrument >= 0) {
            // Opened from an instrument: it is the target as a whole.
            noteSetupInstrument = sourceInstrument;
            for(int role = 0; role < 3; ++role)
                noteSetupRolePaths[role] = noteSetupInstruments[sourceInstrument].rolePaths[role];
        } else if(source != nullptr) {
            // The clicked parameter takes the role its name suggests (pitch if
            // none); its node's other parameters fill the rest, plain names first.
            const std::string sourcePath = container->getTimelineParameterPath(*source);
            int sourceRole = 0;
            for(int role = 2; role >= 0; --role)
                if(noteRoleScore(source->getName(), role) >= noteRoleScore(source->getName(), sourceRole)
                   && noteRoleScore(source->getName(), role) > 0) sourceRole = role;
            noteSetupRolePaths[sourceRole] = sourcePath;
            for(int role = 0; role < 3; ++role) {
                if(!noteSetupRolePaths[role].empty()) continue;
                int bestScore = 0;
                for(const auto& candidate : noteSetupCandidates) {
                    if(!candidate.sameNode || candidate.path == sourcePath) continue;
                    if(std::find(std::begin(noteSetupRolePaths), std::end(noteSetupRolePaths), candidate.path)
                       != std::end(noteSetupRolePaths)) continue;
                    const int score = noteRoleScore(candidate.parameter->getName(), role);
                    if(score > bestScore) { bestScore = score; noteSetupRolePaths[role] = candidate.path; }
                }
            }
            noteSetupInstrument = matchInstrument();
        }
        const std::string name = sourceModel != nullptr ? sourceModel->getParameterGroup().getName() : std::string("Notes");
        std::strncpy(noteSetupName, name.c_str(), sizeof(noteSetupName) - 1);
        noteSetupName[sizeof(noteSetupName) - 1] = '\0';
        // Default to a track where one of these parameters already is, else a new track.
        if(noteSetupTrackId.empty()) {
            for(const auto& track : timeline.getTracks()) {
                if(track.isWaveTrack) continue;
                if(std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& b) {
                       return !b.parameterPath.empty() && std::find(std::begin(noteSetupRolePaths), std::end(noteSetupRolePaths),
                                                                    b.parameterPath) != std::end(noteSetupRolePaths);
                   })) {
                    noteSetupTrackId = track.id;
                    break;
                }
            }
        }
    }
    requestNoteSetupPopup = true;
}

void ofxOceanodeTimelineController::drawNoteGroupSetupPopup(ofxOceanodeTimelineManager& timeline) {
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if(!ImGui::BeginPopupModal("Piano Roll##noteGroupSetup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const bool editing = !noteSetupEditGroupId.empty();
    ImGui::TextDisabled(editing ? "What this piano roll plays"
                                : "One timeline row that plays an instrument's notes");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("Name", noteSetupName, sizeof(noteSetupName));

    if(!editing && !noteSetupTrackFixed) {
        const auto* currentTrack = noteSetupTrackId.empty() ? nullptr : timeline.getTrack(noteSetupTrackId);
        ImGui::SetNextItemWidth(260.0f);
        if(ImGui::BeginCombo("Track", currentTrack == nullptr ? "New track" : currentTrack->name.c_str())) {
            if(ImGui::Selectable("New track", currentTrack == nullptr)) noteSetupTrackId.clear();
            for(const auto& track : timeline.getTracks()) {
                if(track.isWaveTrack) continue;
                if(ImGui::Selectable((track.name + "##" + track.id).c_str(), track.id == noteSetupTrackId))
                    noteSetupTrackId = track.id;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    // The instrument: its Gate, Pitch and Velocity in one choice.
    const bool hasInstrument = noteSetupInstrument >= 0 && noteSetupInstrument < static_cast<int>(noteSetupInstruments.size());
    {
        const char* none = "Parameters chosen by hand";
        ImGui::SetNextItemWidth(260.0f);
        if(ImGui::BeginCombo("Instrument", hasInstrument ? noteSetupInstruments[noteSetupInstrument].label.c_str() : none)) {
            if(ImGui::Selectable(none, !hasInstrument)) noteSetupInstrument = -1;
            for(size_t i = 0; i < noteSetupInstruments.size(); ++i) {
                const auto& candidate = noteSetupInstruments[i];
                const std::string label = candidate.label + (candidate.kit ? "  (kit)" : "") + "##instrument" + ofToString(i);
                if(ImGui::Selectable(label.c_str(), static_cast<int>(i) == noteSetupInstrument)) {
                    noteSetupInstrument = static_cast<int>(i);
                    for(int role = 0; role < 3; ++role) noteSetupRolePaths[role] = candidate.rolePaths[role];
                }
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Synths and samplers that take notes. Every note keeps its own voice.");
    }
    const auto* instrument = hasInstrument ? &noteSetupInstruments[noteSetupInstrument] : nullptr;
    if(instrument != nullptr && instrument->kit)
        ImGui::TextDisabled("A kit: C1 plays pad 1, C#1 pad 2...");
    else {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Voices", &noteSetupVoices);
        noteSetupVoices = ofClamp(noteSetupVoices, 0, 128);
        ImGui::SameLine();
        if(noteSetupVoices == 0)
            ImGui::TextDisabled(instrument != nullptr ? "(%d, the instrument's)" : "(as many as overlap)",
                                instrument != nullptr ? instrument->voices : 0);
        if(ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How many notes play at once. A note past that takes the voice of the oldest one.\n0: the instrument's own voice count.");
    }

    // The parameters, by hand (or to check what the instrument uses).
    ImGui::SetNextItemOpen(instrument == nullptr, ImGuiCond_Appearing);
    if(ImGui::TreeNode("Parameters")) {
        const char* roleLabels[3] = {"Pitch", "Gate", "Velocity"};
        for(int role = 0; role < 3; ++role) {
            std::string preview = "None";
            for(const auto& candidate : noteSetupCandidates)
                if(candidate.path == noteSetupRolePaths[role]) preview = candidate.label;
            if(preview == "None" && !noteSetupRolePaths[role].empty()) preview = noteSetupRolePaths[role];
            ImGui::SetNextItemWidth(260.0f);
            if(ImGui::BeginCombo(roleLabels[role], preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if(ImGui::IsWindowAppearing()) {
                    noteSetupFilter[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##filter", "Search parameters", noteSetupFilter, sizeof(noteSetupFilter));
                if(ImGui::Selectable("None", noteSetupRolePaths[role].empty())) noteSetupRolePaths[role].clear();
                // Grouped by node, the node it was opened from first.
                std::string lastNode;
                for(const auto& candidate : noteSetupCandidates) {
                    if(!containsCaseInsensitive(candidate.label, noteSetupFilter)) continue;
                    const std::string node = candidate.label.substr(0, candidate.label.find(" / "));
                    if(node != lastNode) {
                        ImGui::SeparatorText(node.c_str());
                        lastNode = node;
                    }
                    const std::string name = candidate.label.substr(std::min(candidate.label.size(), node.size() + 3));
                    if(ImGui::Selectable((name + "##" + candidate.path).c_str(), candidate.path == noteSetupRolePaths[role])) {
                        // A parameter plays one role only.
                        for(auto& other : noteSetupRolePaths) if(other == candidate.path) other.clear();
                        noteSetupRolePaths[role] = candidate.path;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::TreePop();
    }
    // Picking parameters by hand that are not exactly an instrument's: no instrument.
    if(hasInstrument && !std::equal(std::begin(noteSetupRolePaths), std::end(noteSetupRolePaths),
                                    std::begin(noteSetupInstruments[noteSetupInstrument].rolePaths)))
        noteSetupInstrument = -1;
    const bool valid = !noteSetupRolePaths[0].empty() || !noteSetupRolePaths[1].empty();
    if(!valid) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Choose an instrument, or at least a pitch or a gate parameter");
    ImGui::Spacing();

    ImGui::BeginDisabled(!valid);
    const bool confirmed = ImGui::Button(editing ? "Save" : "Create");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();

    if(confirmed && valid) {
        const std::string name = noteSetupName[0] == '\0' ? std::string("Notes") : std::string(noteSetupName);
        std::string trackId = noteSetupTrackId;
        if(trackId.empty() || timeline.getTrack(trackId) == nullptr) trackId = timeline.createTrack(name);
        // The binding for each role: the track's existing row for that
        // parameter, or a new one.
        std::string roleIds[3];
        for(int role = 0; role < 3; ++role) {
            const std::string& path = noteSetupRolePaths[role];
            if(path.empty()) continue;
            if(const auto* track = timeline.getTrack(trackId)) {
                for(const auto& binding : track->bindings)
                    if(binding.parameterPath == path) roleIds[role] = binding.id;
            }
            if(!roleIds[role].empty()) continue;
            ofxOceanodeAbstractParameter* parameter = nullptr;
            for(const auto& candidate : noteSetupCandidates)
                if(candidate.path == path) parameter = candidate.parameter;
            if(parameter == nullptr) parameter = container->findTimelineParameter(path);
            if(parameter != nullptr) roleIds[role] = timeline.addBinding(trackId, *parameter);
        }
        if(editing) {
            timeline.setNoteGroupRoles(trackId, noteSetupEditGroupId, roleIds[0], roleIds[1], roleIds[2]);
            timeline.renameNoteGroup(trackId, noteSetupEditGroupId, name);
            timeline.setNoteGroupVoices(trackId, noteSetupEditGroupId, noteSetupVoices);
        } else {
            const std::string groupId = timeline.createNoteGroup(trackId, name, roleIds[0], roleIds[1], roleIds[2]);
            if(!groupId.empty()) {
                timeline.setNoteGroupVoices(trackId, groupId, noteSetupVoices);
                std::string laneId;
                const std::string clipId = timeline.createNoteGroupClip(trackId, groupId, name, noteSetupClipStart,
                                                                        noteSetupClipDuration, &laneId);
                if(auto* track = timeline.getTrack(trackId)) track->collapsed = false;
                if(!clipId.empty()) {
                    // Open the new clip's editor right under the group row.
                    const auto* group = timeline.getNoteGroup(trackId, groupId);
                    const auto members = group == nullptr ? std::vector<std::string>() : group->members();
                    editorAnchorBindingId = members.empty() ? std::string() : members.front();
                    editorState.editorTrackId = trackId;
                    editorState.editorClipId = clipId;
                    editorState.editorLaneId = laneId;
                    editorState.clipEditorOpen = true;
                    editorState.foldedClipEditors.erase(clipId);
                }
            }
        }
        noteSetupCandidates.clear();
        noteSetupInstruments.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::requestClipRename(const std::string& trackId,
                                                       const ofxOceanodeTimelineClip& clip) {
    clipRenameTrackId = trackId;
    clipRenameClipId = clip.id;
    std::strncpy(pendingClipRenameName, clip.name.c_str(), sizeof(pendingClipRenameName) - 1);
    pendingClipRenameName[sizeof(pendingClipRenameName) - 1] = '\0';
    requestClipRenamePopup = true;
}

void ofxOceanodeTimelineController::drawClipRenamePopup(ofxOceanodeTimelineManager& timeline) {
    if(!ImGui::BeginPopupModal("Rename Timeline Clip", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool nameEntered = ImGui::InputText(
        "Name", pendingClipRenameName, sizeof(pendingClipRenameName),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool savePressed = ImGui::Button("Save");
    ImGui::SameLine();
    const bool cancelPressed = ImGui::Button("Cancel");
    if((nameEntered || savePressed) && pendingClipRenameName[0] != '\0') {
        timeline.renameClip(clipRenameTrackId, clipRenameClipId, pendingClipRenameName);
        ImGui::CloseCurrentPopup();
    } else if(cancelPressed) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::drawRenamePopup(ofxOceanodeTimelineManager& timeline) {
    const char* popupTitle = pendingNewTrackDialog ? "New Timeline Track" : "Rename Timeline Track";
    // This can be requested from a node's right-click menu on the main
    // canvas while this controller's own window is elsewhere (a background
    // docked tab, or a separate OS-level window under multi-viewport
    // docking) -- without an explicit position/viewport, ImGui would anchor
    // the popup to wherever it happened to be called from, which is exactly
    // the "shows up in the Timeline window instead of on the canvas" bug
    // this fixes. Force it onto the main viewport, centered, every time.
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if(!ImGui::BeginPopupModal(popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputText("Name", pendingTrackName, sizeof(pendingTrackName),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool nameValid = pendingTrackName[0] != '\0';
    ImGui::BeginDisabled(!nameValid);
    const bool confirmed = ImGui::Button(pendingNewTrackDialog ? "Create" : "Save");
    ImGui::EndDisabled();
    if((confirmed || entered) && nameValid) {
        timeline.renameTrack(pendingTrackId, pendingTrackName);
        pendingNewTrackDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ofxOceanodeTimelineController::drawClipPopup(ofxOceanodeTimelineManager& timeline) {
    if(!ImGui::BeginPopupModal("New Timeline Clip", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::InputText("Name", pendingClipName, sizeof(pendingClipName));
    // One list, LFO included. This used to be two combos -- an "Automation
    // clip / LFO clip" question here and an "Automation type" one below --
    // which asked twice for a single decision and made the LFO look like a
    // different category of thing rather than another kind of clip.
    ImGui::Combo("Start with", &pendingClipLaneType, kLaneTypeOptions, kClipCreationOptionCount);
    const bool isLfoClip = pendingClipLaneType == kLfoClipOptionIndex;
    ImGui::TextWrapped("%s", contentDescription(pendingClipLaneType));
    ImGui::TextWrapped(isLfoClip ? "LFO clip: control envelopes shape an oscillator that drives the output target."
        : "Creates one content lane inside a clip. Add more lanes later; all share the clip timing.");
    if(const auto* track = timeline.getTrack(pendingTrackId)) {
        const auto* selectedBinding = pendingClipBindingId.empty()
            ? nullptr : timeline.getBinding(pendingTrackId, pendingClipBindingId);
        const std::string bindingPreview = selectedBinding == nullptr
            ? "No target" : compactParameterName(selectedBinding->parameterPath);
        if(ImGui::BeginCombo("Target", bindingPreview.c_str())) {
            for(const auto& binding : track->bindings) {
                const std::string label = compactParameterName(binding.parameterPath);
                if(ImGui::Selectable(label.c_str(), binding.id == pendingClipBindingId)) {
                    pendingClipBindingId = binding.id;
                    // Picking a parameter pre-selects the type it is bound
                    // with -- but never over an explicit LFO choice, which
                    // is not a lane type and must not be silently undone.
                    if(!isLfoClip) pendingClipLaneType = optionIndexForBinding(binding);
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", binding.parameterPath.c_str());
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputDouble("Start beat", &pendingStartBeat, editIncrement(), 1.0, "%.4f");
    ImGui::InputDouble("Duration", &pendingDurationBeats, editIncrement(), 1.0, "%.4f");
    const bool pianoRollClip = !isLfoClip &&
        laneTypeFromOptionIndex(pendingClipLaneType) == ofxOceanodeTimelineLaneType::PianoRoll;
    if(pianoRollClip) ImGui::TextDisabled("Next: choose the pitch, gate and velocity parameters");
    if(ImGui::Button(pianoRollClip ? "Next..." : "Create Clip")) {
        const double startBeat = snapBeat(pendingStartBeat);
        const double durationBeats = std::max(1.0 / kPPQ, snapBeat(pendingDurationBeats));
        const auto* pianoTrack = timeline.getTrack(pendingTrackId);
        const auto* pianoGroup = pianoTrack == nullptr ? nullptr
            : ofxOceanodeTimelineManager::findNoteGroupForBinding(*pianoTrack, pendingClipBindingId);
        if(pianoRollClip && pianoGroup != nullptr) {
            // The row already plays in a note group: a new clip for that group.
            std::string laneId;
            const std::string groupId = pianoGroup->id;
            const std::string clipId = timeline.createNoteGroupClip(pendingTrackId, groupId, pendingClipName,
                                                                    startBeat, durationBeats, &laneId);
            if(!clipId.empty()) {
                editorAnchorBindingId = pendingClipBindingId;
                editorState.editorTrackId = pendingTrackId;
                editorState.editorClipId = clipId;
                editorState.editorLaneId = laneId;
                editorState.clipEditorOpen = true;
                editorState.foldedClipEditors.erase(clipId);
            }
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if(pianoRollClip) {
            // A piano roll plays several parameters: pick them (this row's
            // parameter is the starting point), then the group row is made.
            const auto* binding = timeline.getBinding(pendingTrackId, pendingClipBindingId);
            auto* source = binding == nullptr ? nullptr : container->findTimelineParameter(binding->parameterPath);
            openNoteGroupSetup(timeline, source, pendingTrackId, std::string(), startBeat, durationBeats);
            if(source == nullptr && binding != nullptr) noteSetupRolePaths[0] = binding->parameterPath;
            if(pendingClipName[0] != '\0') {
                std::strncpy(noteSetupName, pendingClipName, sizeof(noteSetupName) - 1);
                noteSetupName[sizeof(noteSetupName) - 1] = '\0';
            }
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto clipId = isLfoClip
            ? timeline.createLfoClip(pendingTrackId, pendingClipBindingId,
                                     pendingClipName[0] == '\0' ? "LFO" : pendingClipName,
                                     startBeat, durationBeats)
            : timeline.createClip(pendingTrackId, pendingClipName[0] == '\0' ? "Clip" : pendingClipName,
                                  startBeat, durationBeats);
        if(!isLfoClip) {
            if(const auto* binding = timeline.getBinding(pendingTrackId, pendingClipBindingId)) {
            const auto laneType = laneTypeFromOptionIndex(pendingClipLaneType);
            const auto laneId = timeline.createLane(pendingTrackId, clipId, binding->parameterPath, laneType);
            if(!laneId.empty()) {
                if(pendingClipLaneType == kToggleOptionIndex) makeToggleLane(timeline.getLane(pendingTrackId, clipId, laneId));
                timeline.addBindingToLane(pendingTrackId, clipId, laneId, binding->id);
                if(auto* lane = timeline.getLane(pendingTrackId, clipId, laneId)) {
                    if(laneType != ofxOceanodeTimelineLaneType::PianoRoll &&
                       laneType != ofxOceanodeTimelineLaneType::MultiGate &&
                       laneType != ofxOceanodeTimelineLaneType::Wave &&
                       !ofxOceanodeTimelineIsHeldValueLane(laneType)) {
                        if(auto* parameter = container->findTimelineParameter(binding->parameterPath)) {
                            if(binding->valueType == typeid(float).name()) {
                                lane->valueMin = parameter->cast<float>().getParameter().getMin();
                                lane->valueMax = parameter->cast<float>().getParameter().getMax();
                            } else if(binding->valueType == typeid(int).name()) {
                                lane->valueMin = static_cast<float>(parameter->cast<int>().getParameter().getMin());
                                lane->valueMax = static_cast<float>(parameter->cast<int>().getParameter().getMax());
                            }
                        }
                    }
                }
            }
            }
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if(ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
