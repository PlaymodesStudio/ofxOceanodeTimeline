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
            const std::string timelineParameterPath = host->getTimelineParameterPath(absParam);
            struct BoundTimelineLocation {
                std::string trackId;
                std::string trackName;
                std::string bindingId;
            };
            std::vector<BoundTimelineLocation> boundLocations;
            for(const auto& timelineTrack : timelineManager.getTracks()){
                for(const auto& binding : timelineTrack.bindings){
                    if(binding.parameterPath == timelineParameterPath){
                        boundLocations.push_back({timelineTrack.id, timelineTrack.name, binding.id});
                    }
                }
            }

            if(ImGui::BeginMenu("Add to Timeline Track")){
                if(ImGui::Selectable("New Timeline Track")){
                    const std::string trackId = timelineManager.createTrack();
                    if(!timelineManager.addBinding(trackId, absParam).empty()) {
                        // The timeline controller owns the modal so
                        // the name is chosen before the track is
                        // used in the rest of the UI.
                        timelineManager.requestTrackRename(trackId, true);
                    }
                }
                if(!timelineManager.getTracks().empty()){
                    ImGui::Separator();
                    for(const auto& timelineTrack : timelineManager.getTracks()){
                        if(timelineTrack.isWaveTrack) continue;
                        const bool alreadyBound = std::any_of(timelineTrack.bindings.begin(), timelineTrack.bindings.end(), [&](const auto& binding){
                            return binding.parameterPath == timelineParameterPath;
                        });
                        if(ImGui::Selectable(timelineTrack.name.c_str(), alreadyBound)){
                            timelineManager.requestParameterLane(timelineTrack.id, absParam);
                        }
                        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a lane for this parameter and open its clip.");
                    }
                }
                ImGui::EndMenu();
            }
            // Inside a macro: its own timeline too (a macro's timeline only
            // drives what is inside that macro).
            if(auto* model = absParam.getNodeModel()) {
                const std::string canvas = model->getParents();
                auto* inside = canvas.empty() || canvas == "Canvas" || canvas == "0" ? nullptr : host->getContainerForCanvasID(canvas);
                auto* sequence = inside == nullptr || inside == host ? nullptr : timelineManager.getMacroSequence(*inside);
                if(sequence != nullptr) {
                    const std::string label = "Add to " + timelineManager.getMacroSequenceLabel(*inside) + " Timeline Track";
                    if(ImGui::BeginMenu(label.c_str())) {
                        auto addTo = [&](const std::string& trackId) {
                            if(!timelineManager.addBinding(*sequence, trackId, absParam).empty())
                                timelineManager.requestMacroScope(*inside);
                        };
                        if(ImGui::Selectable("New Timeline Track")) {
                            const auto trackId = timelineManager.createTrack(*sequence, absParam.getName());
                            addTo(trackId);
                        }
                        for(const auto& track : sequence->tracks) {
                            if(track.isFolder) continue;
                            if(ImGui::Selectable((track.name + "##macroTrack" + track.id).c_str())) addTo(track.id);
                        }
                        ImGui::EndMenu();
                    }
                    if(ImGui::IsItemHovered())
                        ImGui::SetTooltip("This macro's own timeline: saved with the macro, shared by copies of a library macro.");
                }
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
            if(boundLocations.size() == 1){
                if(ImGui::Selectable("Remove from Timeline")){
                    timelineManager.removeBinding(boundLocations.front().trackId,
                                                  boundLocations.front().bindingId);
                }
            }else if(boundLocations.size() > 1 && ImGui::BeginMenu("Remove from Timeline")){
                for(const auto& location : boundLocations){
                    if(ImGui::MenuItem(location.trackName.c_str())){
                        timelineManager.removeBinding(location.trackId, location.bindingId);
                    }
                }
                ImGui::EndMenu();
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
