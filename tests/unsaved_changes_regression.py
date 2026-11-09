#!/usr/bin/env python3
"""Native save guard integration, real presets/macros/timelines, ASan/UBSan.

Uses the core headless fixture. Does not exercise native window close dispatch.
"""
from pathlib import Path
import tempfile
import attachment_regression as native
from new_preset_regression import block
from of_environment import container_source

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "ofxOceanode.h"
#include "imgui_internal.h"
void ofxOceanode::openWithDefaultApp(const std::string&) { assert(false); }
class Probe : public ofxOceanodeNodeModel {
public:
    Probe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> value;
    void setup() override { addParameter(value.set("Value", 0.f, 0.f, 100.f)); }
};
int main(int argc, char** argv) {
    assert(argc == 3);
    namespace fs = std::filesystem;
    fs::current_path(argv[2]); ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1200, 800};
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    types->registerType<float>("float");
    registry->registerModel<Probe>("Test"); registry->registerModel<ofxOceanodeNodeMacro>("Macro");
    auto root = std::make_shared<ofxOceanodeContainer>(registry, types);
    root->setCanvasID("Canvas");
    ofxOceanodePresetsController presets(root);
    auto& timeline = ofxOceanodeTimeline::attach(*root);
    assert(!presets.hasUnsavedChanges()); // attaching an empty optional addon is clean
    root->setBpm(90); assert(presets.hasUnsavedChanges());
    root->setBpm(120); assert(!presets.hasUnsavedChanges());
    auto& node = root->createNode(std::make_unique<Probe>(), 1);
    assert(presets.hasUnsavedChanges());
    presets.rememberSavedState(); assert(!presets.hasUnsavedChanges());
    node.getNodeGui().setPosition({30, 40}); assert(presets.hasUnsavedChanges());
    node.getNodeGui().setPosition({0, 0}); presets.rememberSavedState();
    auto& value = node.getNodeModel().getParameter<float>("Value");
    value = 4; assert(!presets.hasUnsavedChanges()); // runtime/automation alone
    auto before = node.saveParametersToJson(false);
    value = 7; root->recordUserEditedValues(node, before, node.saveParametersToJson(false));
    assert(presets.hasUnsavedChanges());
    presets.rememberSavedState(); assert(!presets.hasUnsavedChanges());
    value = 10; assert(!presets.hasUnsavedChanges());
    before = node.saveParametersToJson(false);
    value = 12; root->recordUserEditedValues(node, before, node.saveParametersToJson(false));
    assert(presets.hasUnsavedChanges());
    int proceeded = 0;
    presets.requestDestructiveAction([&](){ ++proceeded; });
    assert(presets.pendingAction && !presets.actionApproved);
    presets.requestDestructiveAction([&](){ proceeded += 100; });
    presets.update(); assert(proceeded == 0); // first action retained, no early execution
    presets.cancelPendingAction(); presets.update(); assert(proceeded == 0);
    presets.requestDestructiveAction([&](){ ++proceeded; });
    presets.actionApproved = true; // Don't Save
    presets.update(); assert(proceeded == 1 && !presets.pendingAction);
    presets.update(); assert(proceeded == 1); // once, outside the draw pass

    // Render both real modal flows and cancel with Escape.
    auto frame = [&](){
        io.DeltaTime = 1.f / 60; ImGui::NewFrame(); ImGui::Begin("DockSpace");
        presets.drawPopups(); ImGui::End(); ImGui::Render();
    };
    presets.popupRequest = ofxOceanodePresetsController::PopupRequest::None;
    presets.requestDestructiveAction([&](){ ++proceeded; }); frame();
    assert(presets.pendingAction);
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    assert(!presets.pendingAction);
    io.AddKeyEvent(ImGuiKey_Escape, false); frame();
    presets.requestDestructiveAction([&](){ ++proceeded; });
    presets.savingBeforeAction = true;
    presets.popupRequest = ofxOceanodePresetsController::PopupRequest::SaveAs; frame();
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    assert(!presets.pendingAction && !presets.savingBeforeAction);
    io.AddKeyEvent(ImGuiKey_Escape, false); frame();

    // Save success continues, a failed write cancels, and cancellation can never
    // leave the action armed for a later ordinary save.
    presets.requestDestructiveAction([&](){ ++proceeded; });
    presets.savingBeforeAction = true;
    presets.bankPresets["Bank"] = {}; presets.banks = {"Bank"};
    presets.beginSavePreset("Kept", "Bank", -1);
    assert(!presets.pendingSave && presets.actionApproved);
    assert(fs::exists("Presets/Bank/1--Kept/modules.json"));
    presets.update(); assert(proceeded == 2 && !presets.hasUnsavedChanges());
    value = 13; root->recordUserEditedValues(node, before, node.saveParametersToJson(false));
    std::ofstream("Presets/Blocked") << "a file blocks this bank";
    presets.bankPresets["Blocked"] = {};
    presets.requestDestructiveAction([&](){ ++proceeded; });
    presets.savingBeforeAction = true;
    presets.beginSavePreset("CannotSave", "Blocked", -1);
    assert(!presets.pendingAction && !presets.actionApproved && presets.hasUnsavedChanges());
    presets.update(); assert(proceeded == 2);
    presets.beginSavePreset("Later", "Bank", -1); presets.update(); assert(proceeded == 2);

    // New and Load leave the current graph and identity intact until approved.
    node.getNodeGui().setPosition({1, 2});
    const auto oldPath = ofxOceanodeShared::getCurrentPresetPath();
    presets.loadPreset(0, "Bank");
    assert(presets.pendingAction && root->getAllModules().size() == 1);
    assert(ofxOceanodeShared::getCurrentPresetPath() == oldPath);
    presets.cancelPendingAction();
    presets.newPreset(); assert(presets.pendingAction && root->getAllModules().size() == 1);
    presets.cancelPendingAction();

    // Root and local macro timeline edits are protected; clock and zoom are not.
    presets.rememberSavedState();
    timeline.getViewState().scrollX = 321; timeline.getViewState().pixelsPerSecond = 77;
    root->getTransport()->play(); root->getTransport()->seekToBeat(8);
    assert(!presets.hasUnsavedChanges());
    timeline.setEditTarget(timeline.addTimeline("", "Timeline 1"));
    const auto track = timeline.createTrack("Track");
    assert(presets.hasUnsavedChanges());
    presets.rememberSavedState();
    timeline.createClip(track, "Clip", 0, 4); assert(presets.hasUnsavedChanges());
    auto& macroNode = root->createNode(std::make_unique<ofxOceanodeNodeMacro>(), 1);
    auto& macro = static_cast<ofxOceanodeNodeMacro&>(macroNode.getNodeModel());
    presets.rememberSavedState();
    auto& insideNode = macro.getContainer()->createNode(std::make_unique<Probe>(), 1);
    assert(presets.hasUnsavedChanges());
    presets.rememberSavedState();
    timeline.createTimeline(macro.getContainer()->getCanvasID(), "Macro timeline");
    assert(presets.hasUnsavedChanges());
    presets.rememberSavedState();
    assert(macro.saveGlobalDefinitionAs("Macros/Saved/Global", false));
    presets.rememberSavedState();
    assert(!presets.hasUnsavedChanges());
    insideNode.getNodeGui().setPosition({20, 30});
    assert(presets.hasUnsavedChanges()); // a changed library macro also blocks quit/load
    presets.requestDestructiveAction([&](){ ++proceeded; });
    presets.savingBeforeAction = true;
    presets.beginSavePreset("Reviewed", "Bank", -1);
    assert(presets.pendingSave && !presets.pendingSave->reviews.empty()); frame();
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    assert(!presets.pendingSave && !presets.pendingAction && !presets.actionApproved);
    io.AddKeyEvent(ImGuiKey_Escape, false); frame();
    presets.beginSavePreset("Reviewed", "Bank", -1);
    assert(presets.pendingSave); // another ordinary save must not revive the canceled action
    presets.pendingSave.reset();
    presets.newPreset(); assert(presets.pendingAction);
    presets.actionApproved = true; presets.update();
    assert(root->getAllModules().empty() && !presets.hasUnsavedChanges());
    presets.loadPreset(0, "Bank");
    assert(root->getAllModules().size() == 1 && !presets.hasUnsavedChanges());
    assert(ofxOceanodeShared::getCurrentPresetName() == "Kept");
    assert(proceeded == 2);

    // Exercise the real Save As -> replacement confirmation -> normal Save
    // pipeline, including the nested Cancel returning to the unchanged form.
    auto activate = [&](const char* windowName, const char* label){
        auto* window = ImGui::FindWindowByName(windowName); assert(window);
        ImGui::ActivateItemByID(window->GetID(label)); frame();
    };
    auto openReplacement = [&](){
        presets.requestDestructiveAction([&](){ ++proceeded; });
        presets.savingBeforeAction = true;
        presets.popupRequest = ofxOceanodePresetsController::PopupRequest::SaveAs;
        frame(); frame();
        activate("Save Preset As", "Replace...");
        assert(presets.replacementIndex == 0 && presets.replacementName == "Kept");
        assert(!presets.pendingSave && !presets.actionApproved);
    };
    root->getAllModules().front()->getNodeGui().setPosition({99, 88});
    const auto savedBeforeReplace = ofLoadJson("Presets/Bank/1--Kept/modules.json");
    openReplacement();
    activate("Replace Preset?", "Cancel");
    assert(presets.pendingAction && presets.savingBeforeAction);
    assert(ofLoadJson("Presets/Bank/1--Kept/modules.json") == savedBeforeReplace);
    activate("Save Preset As", "Replace...");
    activate("Replace Preset?", "Replace");
    assert(presets.actionApproved && !presets.pendingSave);
    assert(presets.bankPresets["Bank"].size() == 2); // Kept and Later only
    assert(ofLoadJson("Presets/Bank/1--Kept/modules.json") != savedBeforeReplace);
    presets.update(); assert(proceeded == 3);

    auto& reviewNode = root->createNode(std::make_unique<ofxOceanodeNodeMacro>(), 1);
    auto& reviewMacro = static_cast<ofxOceanodeNodeMacro&>(reviewNode.getNodeModel());
    auto& child = reviewMacro.getContainer()->createNode(std::make_unique<Probe>(), 1);
    assert(reviewMacro.saveGlobalDefinitionAs("Macros/Saved/Review", false));
    child.getNodeGui().setPosition({66, 55});
    const auto beforeReviewSave = ofLoadJson("Presets/Bank/1--Kept/modules.json");
    openReplacement(); activate("Replace Preset?", "Replace");
    assert(presets.pendingSave && !presets.pendingSave->createNew && presets.pendingSave->index == 0);
    assert(!presets.pendingSave->reviews.empty() && !presets.actionApproved);
    assert(ofLoadJson("Presets/Bank/1--Kept/modules.json") == beforeReviewSave);
    frame(); activate("Review Global Macros", "Cancel preset save");
    assert(!presets.pendingAction && !presets.pendingSave);
    assert(ofLoadJson("Presets/Bank/1--Kept/modules.json") == beforeReviewSave);
    openReplacement(); activate("Replace Preset?", "Replace");
    frame(); activate("Review Global Macros", "Save this macro");
    assert(!presets.pendingSave && presets.actionApproved);
    assert(presets.bankPresets["Bank"].size() == 2);
    presets.update(); assert(proceeded == 4);
    assert(!presets.hasUnsavedChanges());
    ImGui::DestroyContext(context);
    std::cout << "Unsaved changes passed: authored/runtime values, graph/local/global macros, timelines, deferred actions, cancellation, save success and failure.\n";
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='preset-guard-access-') as temp:
        access = Path(temp) / 'PresetGuardTestAccess.h'
        access.write_text((native.CORE / 'src/Controls/ofxOceanodePresetsController.h').read_text().replace('private:', 'public:'))
        native.fixture.PRELUDE = '#include "' + str(access) + '"\n' + native.fixture.PRELUDE
        source = container_source()
        native.fixture.SUPPORT += '\n#include "ofxOceanodeCustomGuiExtension.h"\n'
        native.fixture.SUPPORT += '\nnamespace {\n' + block(source, 'std::string userEditedValueKey(') + '\n}\n'
        for signature in ('void ofxOceanodeContainer::recordUserEditedValues(',
                          'bool ofxOceanodeContainer::wasValueUserEdited(',
                          'ofJson ofxOceanodeContainer::getMidiBindingsJsonForReview('):
            native.fixture.SUPPORT += block(source, signature) + '\n'
        sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
        sources += ['src/Controls/ofxOceanodeBaseController.cpp', 'src/Controls/ofxOceanodePresetsController.cpp']
        includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
        native.fixture.main(extra_sources=sources, extra_includes=includes)
