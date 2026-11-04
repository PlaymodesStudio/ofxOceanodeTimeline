#include "Nodes/ofxOceanodeNodeMacro.h"
#include "ofxOceanodeTimeline.h"
#include "ofxOceanode.h"
#include "ofxOceanodeTimelineController.h"
#include "lfoTrack.h"
#include "timelineTransport.h"
#include "imgui.h"
#include <algorithm>
#include <stdexcept>

namespace {
class TimelineExtension : public ofxOceanodeAutomationExtension {
public:
    explicit TimelineExtension(ofxOceanodeContainer* owner) : host(owner), manager(owner) {}
    ofxOceanodeContainer* host;
    ofxOceanodeTimelineManager manager;
    void clear() override { manager.clear(); }
    void beginParameterCache() override { manager.beginParameterCache(); }
    void evaluateAutomation() override { manager.evaluateAutomation(); }
    void applyAutomation() override { manager.applyAutomation(); }
    void endParameterCache() override { manager.endParameterCache(); }
    bool loadPreset(const std::string& path) override { return manager.loadPreset(path); }
    bool savePreset(const std::string& path) const override { return manager.savePreset(path); }
    bool getPlayOnPresetLoad() const override { return manager.getPlayOnPresetLoad(); }
    bool isBpmAutomationEnabled() const override { return manager.isBpmAutomationEnabled(); }
    bool preservesSecondsOnBpmChange() const override { return manager.getTimebase() == ofxOceanodeTimelineTimebase::Time; }
    void retimeForBpmChange(double before, double after) override { manager.retimeForBpmChange(before, after); }
    double getBeatsPerBar() const override { return manager.getBeatsPerBar(); }
    bool didLoopWrapThisFrame() const override { return manager.didLoopWrapThisFrame(); }
    bool getParameterTrackColor(const ofxOceanodeAbstractParameter& p, ofColor& color) const override {
        return manager.getParameterTrackColor(p, color);
    }
    void collectBoundParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const override {
        manager.collectBoundParameters(result);
    }
    void retargetParameterPaths(const std::map<std::string, std::string>& oldToNew) override {
        manager.retargetAllParameterPaths(oldToNew);
    }
    ofJson encapsulateParameters(ofxOceanodeContainer& canvas, ofxOceanodeContainer& macroContainer,
                                 const std::map<std::string, std::string>& oldToNew, bool moveIntoMacro) override {
        return manager.encapsulateParameters(canvas, macroContainer, oldToNew, moveIntoMacro);
    }
    void replayParameterMove(ofxOceanodeContainer& canvas, ofxOceanodeContainer* macroContainer,
                             const std::map<std::string, std::string>& oldToNew, const ofJson& record,
                             bool backwards) override {
        manager.replayParameterMove(canvas, macroContainer, oldToNew, record, backwards);
    }
    void loadMacroPreset(ofxOceanodeContainer& macroContainer, const std::string& folder) override {
        manager.loadMacroSequence(macroContainer, folder);
    }
    std::vector<CanvasTimeline> canvasTimelines(const ofxOceanodeContainer& canvas) override {
        std::vector<CanvasTimeline> result;
        const auto& windows = manager.getTimelineWindows();
        for(const auto& listed : manager.timelinesOfCanvas(canvas.getCanvasID())) {
            const auto ref = ofxOceanodeTimelineManager::normalizedRef(listed);
            if(!manager.timelineExists(ref)) continue;
            const bool open = std::any_of(windows.begin(), windows.end(), [&](const auto& window) {
                return ofxOceanodeTimelineManager::normalizedRef(window.ref) == ref;
            });
            result.push_back({ref.canvas, ref.timeline, manager.timelineName(ref), open});
        }
        return result;
    }
    void setTimelineWindowOpen(const std::string& canvas, const std::string& timeline, bool open,
                               const std::string& label) override {
        manager.setHostedTimelineWindow({canvas, timeline}, open, label);
    }
    bool saveMacroPreset(const ofxOceanodeContainer& macroContainer, const std::string& folder) const override {
        return manager.saveMacroSequence(macroContainer, folder);
    }
    void removeParameterBindings(ofxOceanodeAbstractParameter& parameter) override {
        manager.removeBindingsOf(parameter);
    }
    void drawParameterMenu(ofxOceanodeAbstractParameter& absParam) override {
        if(manager.isStepLaneCompatible(absParam)){
            auto& timelineManager = manager;
            using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
            // The timelines this parameter can be added to: those of its own
            // canvas and of every canvas around it, from the root down (a
            // timeline drives what is inside its canvas).
            std::vector<std::pair<TimelineRef, ofxOceanodeContainer*>> eligible;
            std::vector<std::pair<std::string, ofxOceanodeContainer*>> eligibleCanvases;
            {
                std::vector<std::string> canvases{std::string()};
                const std::string own = absParam.getNodeModel() != nullptr ? absParam.getNodeModel()->getParents() : std::string();
                for(size_t at = own.find(" / "); ; at = own.find(" / ", at + 3)) {
                    const std::string prefix = at == std::string::npos ? own : own.substr(0, at);
                    if(!prefix.empty() && prefix != "Canvas" && prefix != "0" &&
                       std::find(canvases.begin(), canvases.end(), prefix) == canvases.end())
                        canvases.push_back(prefix);
                    if(at == std::string::npos) break;
                }
                for(const auto& canvas : canvases) {
                    auto* canvasContainer = timelineManager.canvasContainer(canvas);
                    if(canvasContainer == nullptr) continue;
                    eligibleCanvases.push_back({canvas, canvasContainer});
                    // A macro with no timeline yet is offered too: a New
                    // Timeline entry that makes its own.
                    auto refs = timelineManager.timelinesOfCanvas(canvas);
                    if(std::none_of(refs.begin(), refs.end(), [](const TimelineRef& ref) { return ref.timeline.empty(); }))
                        refs.insert(refs.begin(), TimelineRef{canvas, std::string()});
                    for(const auto& ref : refs)
                        eligible.push_back({ref, canvasContainer});
                }
            }
            // The path a timeline of that canvas saves for this parameter.
            auto pathFor = [&](ofxOceanodeContainer* canvas) {
                return canvas == host ? host->getTimelineParameterPath(absParam)
                                      : host->getTimelineParameterPath(absParam, *canvas);
            };
            // A new clip driving it on a track of that timeline (a new track
            // when trackId is empty), a new lane in one of the track's clips,
            // or one more target of a lane, set up in the editor's dialog.
            using Kind = ofxOceanodeTimelineManager::ParameterClipRequest::Kind;
            auto requestClip = [&](const TimelineRef& ref, ofxOceanodeContainer* canvas, const std::string& trackId,
                                   Kind kind = Kind::Clip) {
                timelineManager.requestParameterClip({ref, trackId, pathFor(canvas), kind});
            };
            auto timelineHeading = [&](const TimelineRef& ref) {
                const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
                return (root ? std::string() : ref.canvas + " > ") + timelineManager.timelineName(ref);
            };
            // A new timeline (on the parameter's canvas or one around it), with
            // the parameter on a first track: named, coloured and placed in
            // the New Timeline dialog, then shown in the editor.
            auto requestNewTimeline = [&](const std::string& owner) {
                ofxOceanodeTimelineManager::NewTimelineRequest request;
                for(const auto& entry : eligibleCanvases) request.canvases.push_back(entry.first);
                request.canvas = owner;
                request.parameter = &absParam;
                timelineManager.requestNewTimeline(request);
            };
            if(ImGui::BeginMenu("Add to Timeline")){
                // Every timeline it can join, each with its tracks under it
                // (New Track... first), in one list.
                for(const auto& [ref, canvas] : eligible) {
                    ImGui::PushID(ofxOceanodeTimelineManager::timeSourceId(ref).c_str());
                    if(!timelineManager.timelineExists(ref)) {
                        // A macro with no timeline yet: its own is made in the New
                        // Timeline dialog (name, colour, owner), this parameter on it.
                        const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
                        if(ImGui::Selectable(((root ? std::string("Main canvas") : ref.canvas) + " > New Timeline...").c_str()))
                            requestNewTimeline(ref.canvas);
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip(root ? "The main canvas has no timeline yet: make one,\nwith this parameter on its first track."
                                                   : "This macro has no timeline yet: make its own,\nwith this parameter on its first track.");
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::TextDisabled("%s", timelineHeading(ref).c_str());
                    ImGui::Indent();
                    if(ImGui::Selectable("New Track...")) requestClip(ref, canvas, std::string());
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A new track with a clip driving this parameter.");
                    if(auto* sequence = timelineManager.findTimeline(ref)) {
                        const std::string path = pathFor(canvas);
                        for(const auto& track : sequence->tracks) {
                            if(track.isWaveTrack || track.isFolder || track.isMacroTrack()) continue;
                            const bool bound = std::any_of(track.bindings.begin(), track.bindings.end(),
                                                           [&](const auto& binding) { return binding.parameterPath == path; });
                            // A track already driving it reads as such.
                            const std::string label = track.name + (bound ? "  (bound)" : "") + "##track" + track.id;
                            if(ImGui::BeginMenu(label.c_str())) {
                                const bool hasClip = std::any_of(track.clips.begin(), track.clips.end(),
                                                                 [](const auto& clip) { return !clip.isLfo; });
                                const bool hasLane = std::any_of(track.clips.begin(), track.clips.end(),
                                                                 [](const auto& clip) { return !clip.isLfo && !clip.lanes.empty(); });
                                if(ImGui::MenuItem("New Target...", nullptr, false, hasLane)) requestClip(ref, canvas, track.id, Kind::Target);
                                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip(hasLane ? "Make an existing lane of this track drive this parameter too."
                                                              : "This track has no lane to drive this parameter yet.");
                                if(ImGui::MenuItem("New Lane...", nullptr, false, hasClip)) requestClip(ref, canvas, track.id, Kind::Lane);
                                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip(hasClip ? "A new lane driving this parameter, in one of this track's clips."
                                                              : "This track has no clip to add a lane to yet.");
                                if(ImGui::MenuItem("New Clip...")) requestClip(ref, canvas, track.id);
                                if(ImGui::IsItemHovered()) ImGui::SetTooltip("A new clip on this track driving this parameter.");
                                ImGui::EndMenu();
                            }
                        }
                    }
                    ImGui::Unindent();
                    ImGui::PopID();
                }
                ImGui::Separator();
                if(ImGui::Selectable("New Timeline...")) requestNewTimeline(eligibleCanvases.empty() ? std::string() : eligibleCanvases.back().first);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("A new timeline with this parameter on its first track:\n"
                                      "on this canvas or one around it (in a macro without a timeline yet,\n"
                                      "it becomes the macro's own).");
                ImGui::EndMenu();
            }
            // Wherever it is bound, in any of those timelines.
            struct BoundLocation { TimelineRef ref; std::string trackId, label, bindingId; };
            std::vector<BoundLocation> boundLocations;
            for(const auto& [ref, canvas] : eligible) {
                auto* sequence = timelineManager.findTimeline(ref);
                if(sequence == nullptr) continue;
                const std::string path = pathFor(canvas);
                for(const auto& track : sequence->tracks)
                    for(const auto& binding : track.bindings)
                        if(binding.parameterPath == path)
                            boundLocations.push_back({ref, track.id, timelineManager.timelineName(ref) + " > " + track.name, binding.id});
            }
            auto removeAt = [&](const BoundLocation& location) {
                if(auto* sequence = timelineManager.timelineFor(location.ref))
                    timelineManager.removeBinding(*sequence, location.trackId, location.bindingId);
            };
            if(boundLocations.size() == 1){
                if(ImGui::Selectable("Remove from Timeline")) removeAt(boundLocations.front());
            }else if(boundLocations.size() > 1 && ImGui::BeginMenu("Remove from Timeline")){
                for(const auto& location : boundLocations)
                    if(ImGui::MenuItem((location.label + "##remove" + location.bindingId +
                                        ofxOceanodeTimelineManager::timeSourceId(location.ref)).c_str())) removeAt(location);
                ImGui::EndMenu();
            }
            {
                // Pitch, gate and velocity as one piano-roll row; the
                // timeline shows a dialog to pick the other two (or the
                // whole instrument, when the node is one).
                const std::string valueType = absParam.valueType();
                ofxOceanodeTimelineInstrument instrument;
                const bool isInstrument = absParam.getNodeModel() != nullptr &&
                    absParam.getNodeModel()->getTimelineInstrument(instrument);
                if((isInstrument || valueType == typeid(float).name() || valueType == typeid(int).name() ||
                    valueType == typeid(bool).name() || valueType == typeid(std::vector<float>).name() ||
                    valueType == typeid(std::vector<int>).name()) &&
                   ImGui::Selectable("Add to Timeline as Piano Roll...")){
                    timelineManager.requestNoteGroupSetup(&absParam);
                }
            }
        }

    }
    // The canvas's right-click menu: a new timeline on that canvas (the
    // main one, or a macro's at any depth), shown in the Timeline window.
    void drawCanvasMenu(ofxOceanodeContainer& canvas) override {
        const bool root = &canvas == host || &canvas == &host->getAutomationHost();
        const std::string canvasId = root ? std::string() : canvas.getCanvasID();
        ImGui::Separator();
        if(ImGui::Selectable("New Timeline")) {
            // Its name, colour and owner (this canvas preselected) in the New Timeline dialog.
            ofxOceanodeTimelineManager::NewTimelineRequest request;
            request.canvas = canvasId;
            manager.requestNewTimeline(request);
            ImGui::CloseCurrentPopup();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", root ? "A new timeline, on the main canvas or a macro's, shown in the Timeline window."
                                         : "A new timeline, on this macro's canvas (its own one when it has none yet)\nor another, shown in the Timeline window.");
    }
    void drawNodeMenu(ofxOceanodeNodeModel& model) override {
                // A macro's own timeline, opened in the timeline editor.
                if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&model); macro != nullptr && macro->getContainer()){
                    ImGui::Separator();
                    if(ImGui::Selectable("Edit Timeline")){
                        manager.requestMacroScope(*macro->getContainer());
                        ImGui::CloseCurrentPopup();
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("Open this macro's own timeline in the timeline window.\nIt is saved with the macro (copies of a library macro share it).");
                    {
                        // A track whose clips play this macro's timeline, with a first clip at
                        // the start, on a timeline of the canvas the macro is in or one around it.
                        using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
                        const auto placed = manager.refForCanvas(*macro->getContainer());
                        std::vector<TimelineRef> hosts;
                        std::vector<std::string> canvases{std::string()};
                        const std::string own = model.getParents();
                        for(size_t at = own.find(" / "); ; at = own.find(" / ", at + 3)) {
                            const std::string prefix = at == std::string::npos ? own : own.substr(0, at);
                            if(!prefix.empty() && prefix != "Canvas" && prefix != "0" &&
                               std::find(canvases.begin(), canvases.end(), prefix) == canvases.end())
                                canvases.push_back(prefix);
                            if(at == std::string::npos) break;
                        }
                        for(const auto& canvas : canvases) {
                            if(manager.canvasContainer(canvas) == nullptr) continue;
                            // An empty macro around it can host it too (adding the track makes its timeline).
                            auto refs = manager.timelinesOfCanvas(canvas);
                            if(std::none_of(refs.begin(), refs.end(), [](const TimelineRef& ref) { return ref.timeline.empty(); }))
                                refs.insert(refs.begin(), TimelineRef{canvas, std::string()});
                            for(const auto& ref : refs)
                                if(!manager.timelineReaches(placed, ref)) hosts.push_back(ref);
                        }
                        auto addTo = [&](const TimelineRef& host) {
                            auto* sequence = manager.timelineFor(host);
                            if(sequence == nullptr) return;
                            const auto trackId = manager.createTimelineTrack(*sequence, placed);
                            if(!trackId.empty()) manager.createMacroClip(*sequence, trackId, 0.0);
                            if(!(host.canvas.empty() && host.timeline.empty())) manager.requestTimelineScope(host);
                            ImGui::CloseCurrentPopup();
                        };
                        const char* tip = "A track whose clips play this macro's timeline:\n"
                                          "place, repeat, stretch and offset them like any clip.";
                        if(hosts.size() <= 1) {
                            if(ImGui::Selectable("Add to Timeline as Macro Track") && !hosts.empty()) addTo(hosts.front());
                            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                        } else if(ImGui::BeginMenu("Add to Timeline as Macro Track")) {
                            for(const auto& host : hosts) {
                                const bool root = host.canvas.empty() || host.canvas == "Canvas" || host.canvas == "0";
                                const std::string label = (root ? std::string() : host.canvas + " > ") + manager.timelineName(host) +
                                                          "##host" + ofxOceanodeTimelineManager::timeSourceId(host);
                                if(ImGui::MenuItem(label.c_str())) addTo(host);
                            }
                            ImGui::EndMenu();
                        } else if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                    }
                    {
                        using ClockSource = ofxOceanodeTimelineManager::ClockSource;
                        const auto ref = manager.refForCanvas(*macro->getContainer());
                        const bool exists = manager.timelineExists(ref);
                        const ClockSource source = manager.clockSourceOf(ref);
                        const bool free = source == ClockSource::Free;
                        const bool dependent = source == ClockSource::Dependent;
                        if(ImGui::MenuItem("Free tempo for its timeline", nullptr, free, exists && !dependent)) {
                            manager.setFree(ref, !free);
                            ImGui::CloseCurrentPopup();
                        }
                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip(dependent ? "Its timeline is placed as clips in another: it depends on that one."
                                              : "Its timeline gets its own transport: play, stop and seek it by itself,\n"
                                                "at its own tempo, alongside the global transport. Nodes inside the macro\n"
                                                "set to Auto follow it. Off: linked to the global transport.");
                        bool followsClips = manager.nodesFollowClips(ref.canvas);
                        if(ImGui::MenuItem("Nodes inside follow its clips", nullptr, followsClips, exists && !free)) {
                            manager.setNodesFollowClips(ref.canvas, !followsClips);
                            ImGui::CloseCurrentPopup();
                        }
                        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip(free ? "Its timeline is free: nodes inside follow it."
                                                   : "Nodes inside set to Auto (Phasor, LFO Track, Beat Transport...) follow\n"
                                                     "the macro clip playing it: its beat, restarting with each clip and repeat,\n"
                                                     "at its stretched tempo. Off: they follow the timeline around the macro.");
                        if(ImGui::MenuItem("Open its timeline in a new window", nullptr, false, exists)) {
                            manager.openTimelineWindow(ref);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                // An instrument (a synth that takes notes): a piano-roll row
                // playing it, set up in the timeline's dialog.
                ofxOceanodeTimelineInstrument instrument;
                if(model.getTimelineInstrument(instrument) && instrument.chords && instrument.gate != nullptr){
                    // A chord bank: a track with a Chords lane, one row per chord.
                    ImGui::Separator();
                    if(ImGui::Selectable("Add to Timeline as Chord Track")){
                        const std::string trackId = manager.createTrack(model.getParameterGroup().getName());
                        if(!manager.requestParameterLane(trackId, *instrument.gate)) manager.removeTrack(trackId);
                        ImGui::CloseCurrentPopup();
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("A track whose clips hold the chords: one row per chord of the bank,\n"
                                          "drag in a row to play that chord for that long.");
                }else if(model.getTimelineInstrument(instrument) &&
                   (instrument.gate != nullptr || instrument.pitch != nullptr)){
                    ImGui::Separator();
                    if(ImGui::Selectable("Add to Timeline as Instrument...")){
                        manager.requestNoteGroupSetup(
                            instrument.gate != nullptr ? instrument.gate : instrument.pitch);
                        ImGui::CloseCurrentPopup();
                    }
                }
    }
};
}

// Compatibility accessors are linked only when the optional addon is present.
ofxOceanodeTimelineManager& ofxOceanodeContainer::getTimelineManager() {
    auto* extension = dynamic_cast<TimelineExtension*>(getAutomationExtension());
    if(!extension) throw std::logic_error("Call ofxOceanodeTimeline::setup(oceanode) after oceanode.setup()");
    return extension->manager;
}
const ofxOceanodeTimelineManager& ofxOceanodeContainer::getTimelineManager() const {
    return const_cast<ofxOceanodeContainer*>(this)->getTimelineManager();
}

ofxOceanodeTimelineManager& ofxOceanodeTimeline::attach(ofxOceanodeContainer& container) {
    auto& root = container.getAutomationHost();
    if(auto* extension = dynamic_cast<TimelineExtension*>(root.getAutomationExtension())) return extension->manager;
    if(root.getAutomationExtension()) throw std::logic_error("An automation extension is already attached");
    root.setAutomationExtension(std::make_unique<TimelineExtension>(&root));
    return root.getTimelineManager();
}

void ofxOceanodeTimeline::setup(ofxOceanode& oceanode) {
    auto container = oceanode.getContainer();
    if(!container) throw std::logic_error("Call oceanode.setup() before ofxOceanodeTimeline::setup(oceanode)");
    attach(*container);
    if(oceanode.getController<ofxOceanodeTimelineController>()) return;
    oceanode.registerModel<lfoTrack>("Generators");
    oceanode.registerModel<timelineTransport>("Timeline");
    oceanode.addController(std::make_shared<ofxOceanodeTimelineController>(container));
}
