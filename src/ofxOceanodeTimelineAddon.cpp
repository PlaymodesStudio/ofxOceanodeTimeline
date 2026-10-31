#include "Nodes/ofxOceanodeNodeMacro.h"
#include "ofxOceanodeTimeline.h"
#include "ofxOceanode.h"
#include "ofxOceanodeTimelineController.h"
#include "lfoTrack.h"
#include "imgui.h"
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
    bool saveMacroPreset(const ofxOceanodeContainer& macroContainer, const std::string& folder) const override {
        return manager.saveMacroSequence(macroContainer, folder);
    }
    void removeParameterBindings(ofxOceanodeAbstractParameter& parameter) override {
        const auto path = host->getTimelineParameterPath(parameter);
        std::vector<std::pair<std::string, std::string>> bindings;
        for(const auto& track : manager.getTracks())
            for(const auto& binding : track.bindings)
                if(binding.parameterPath == path) bindings.emplace_back(track.id, binding.id);
        for(const auto& binding : bindings) manager.removeBinding(binding.first, binding.second);
    }
    void drawParameterMenu(ofxOceanodeAbstractParameter& absParam) override {
        if(manager.isStepLaneCompatible(absParam)){
            auto& timelineManager = manager;
            using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
            // The timelines this parameter can be added to: those of its own
            // canvas and of every canvas around it, from the root down (a
            // timeline drives what is inside its canvas).
            std::vector<std::pair<TimelineRef, ofxOceanodeContainer*>> eligible;
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
                    for(const auto& ref : timelineManager.timelinesOfCanvas(canvas))
                        eligible.push_back({ref, canvasContainer});
                }
            }
            // The path a timeline of that canvas saves for this parameter.
            auto pathFor = [&](ofxOceanodeContainer* canvas) {
                return canvas == host ? host->getTimelineParameterPath(absParam)
                                      : host->getTimelineParameterPath(absParam, *canvas);
            };
            auto addTo = [&](const TimelineRef& ref, const std::string& trackId) {
                auto* sequence = timelineManager.timelineFor(ref);
                if(sequence == nullptr) return;
                const bool isMain = sequence == &timelineManager.getMainSequence();
                if(trackId.empty()) {
                    const auto newTrack = isMain ? timelineManager.createTrack() : timelineManager.createTrack(*sequence, absParam.getName());
                    if(!(isMain ? timelineManager.addBinding(newTrack, absParam) : timelineManager.addBinding(*sequence, newTrack, absParam)).empty() && isMain)
                        timelineManager.requestTrackRename(newTrack, true); // named before it is used
                } else if(isMain) {
                    timelineManager.requestParameterLane(trackId, absParam);
                } else {
                    timelineManager.addBinding(*sequence, trackId, absParam);
                }
                if(!isMain) timelineManager.requestTimelineScope(ref);
            };
            auto trackItems = [&](const TimelineRef& ref, ofxOceanodeContainer* canvas) {
                auto* sequence = timelineManager.timelineFor(ref);
                if(sequence == nullptr) return;
                if(ImGui::Selectable(("New Timeline Track##new" + ofxOceanodeTimelineManager::timeSourceId(ref)).c_str())) addTo(ref, std::string());
                const std::string path = pathFor(canvas);
                bool first = true;
                for(const auto& track : sequence->tracks) {
                    if(track.isWaveTrack || track.isFolder || track.isMacroTrack()) continue;
                    if(first) { ImGui::Separator(); first = false; }
                    const bool bound = std::any_of(track.bindings.begin(), track.bindings.end(),
                                                   [&](const auto& binding) { return binding.parameterPath == path; });
                    if(ImGui::Selectable((track.name + "##track" + track.id).c_str(), bound)) addTo(ref, track.id);
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a lane for this parameter to this track.");
                }
            };
            auto timelineLabel = [&](const TimelineRef& ref) {
                const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
                return (root ? std::string() : ref.canvas + " > ") + timelineManager.timelineName(ref) +
                       "##add" + ofxOceanodeTimelineManager::timeSourceId(ref);
            };
            if(ImGui::BeginMenu("Add to Timeline")){
                if(eligible.size() == 1) {
                    trackItems(eligible.front().first, eligible.front().second); // one timeline: straight to its tracks
                } else {
                    for(const auto& [ref, canvas] : eligible) {
                        if(ImGui::BeginMenu(timelineLabel(ref).c_str())) {
                            trackItems(ref, canvas);
                            ImGui::EndMenu();
                        }
                    }
                }
                ImGui::EndMenu();
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Choose the timeline (this canvas's or one around it), then the track.");
            // Wherever it is bound, in any of those timelines.
            struct BoundLocation { TimelineRef ref; std::string trackId, label, bindingId; };
            std::vector<BoundLocation> boundLocations;
            for(const auto& [ref, canvas] : eligible) {
                auto* sequence = timelineManager.timelineFor(ref);
                if(sequence == nullptr) continue;
                const std::string path = pathFor(canvas);
                for(const auto& track : sequence->tracks)
                    for(const auto& binding : track.bindings)
                        if(binding.parameterPath == path)
                            boundLocations.push_back({ref, track.id, timelineManager.timelineName(ref) + " > " + track.name, binding.id});
            }
            auto removeAt = [&](const BoundLocation& location) {
                if(auto* sequence = timelineManager.timelineFor(location.ref)) {
                    if(sequence == &timelineManager.getMainSequence()) timelineManager.removeBinding(location.trackId, location.bindingId);
                    else timelineManager.removeBinding(*sequence, location.trackId, location.bindingId);
                }
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
                    if(ImGui::Selectable("Add to Timeline as Macro Track")){
                        // A track whose clips play this macro's timeline, with a first clip at the start.
                        const auto trackId = manager.createMacroTrack(*macro->getContainer());
                        if(!trackId.empty()) manager.createMacroClip(trackId, 0.0);
                        ImGui::CloseCurrentPopup();
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("A main-timeline track whose clips play this macro's timeline:\nplace, repeat, stretch (tempo ratio) and offset them like any clip.");
                    {
                        const auto ref = manager.refForCanvas(*macro->getContainer());
                        bool free = manager.isFree(ref);
                        if(ImGui::MenuItem("Play its timeline on its own", nullptr, free)) {
                            manager.setFree(ref, !free);
                            ImGui::CloseCurrentPopup();
                        }
                        if(ImGui::IsItemHovered())
                            ImGui::SetTooltip("Its timeline gets its own transport: play, stop and seek it by itself,\n"
                                              "at its own tempo, alongside the main timeline. Nodes inside the macro\n"
                                              "set to Auto follow it. (Its macro clips still play it too.)");
                        if(ImGui::MenuItem("Open its timeline in a new window")) {
                            manager.openTimelineWindow(ref);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                // An instrument (a synth that takes notes): a piano-roll row
                // playing it, set up in the timeline's dialog.
                ofxOceanodeTimelineInstrument instrument;
                if(model.getTimelineInstrument(instrument) &&
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
    oceanode.addController(std::make_shared<ofxOceanodeTimelineController>(container));
}
