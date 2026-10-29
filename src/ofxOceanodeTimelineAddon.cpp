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
        for(const auto& track : manager.getTracks())
            for(const auto& binding : track.bindings)
                if(auto* parameter = host->findTimelineParameter(binding.parameterPath)) result.insert(parameter);
    }
    void retargetParameterPaths(const std::map<std::string, std::string>& oldToNew) override {
        manager.retargetParameterPaths(oldToNew);
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
