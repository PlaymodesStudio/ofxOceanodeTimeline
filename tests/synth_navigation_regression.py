#!/usr/bin/env python3
"""Drive synth navigation menus with real nodes/routing and headless ImGui.

Canvas reveal is recorded by the fixture; native canvas and plugin windows are
outside this headless test. Dockable window opening/focusing uses real ImGui.
"""
import attachment_regression as native

original_production = native.fixture.native_production

def production():
    return original_production().replace(
        'bool ofxOceanodeContainer::showParameterInCanvas(ofxOceanodeAbstractParameter&) { assert(false); return false; }',
        'ofxOceanodeAbstractParameter* navigationCanvasTarget = nullptr;\n'
        'bool ofxOceanodeContainer::showParameterInCanvas(ofxOceanodeAbstractParameter& p) { navigationCanvasTarget=&p; return true; }')

native.fixture.native_production = production
native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#define private public
#include "Controls/ofxOceanodeTimelineController.h"
#undef private
#include "imgui_internal.h"
extern ofxOceanodeAbstractParameter* navigationCanvasTarget;
class NavigationSynth : public ofxOceanodeNodeModel {
public:
    NavigationSynth() : ofxOceanodeNodeModel("Navigation Synth") {}
    ofParameter<float> pitch,gate;
    ofParameter<bool> show;
    bool externalEditor=false;
    int openRequests=0;
    void setup() override {
        addParameter(pitch.set("Pitch",60,0,127));
        addParameter(gate.set("Gate",0,0,1));
        addParameter(show.set("Show",false));
    }
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override {
        out={};
        out.pitch=&static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Pitch"));
        out.gate=&static_cast<ofxOceanodeAbstractParameter&>(getParameterGroup().get("Gate"));
        out.voices=1; return true;
    }
    std::string getWindowTitle() override { return externalEditor ? "" : "Navigation editor "+ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return externalEditor ? nullptr : &show; }
    bool canShowNodeWindow() override { return externalEditor || ofxOceanodeNodeModel::canShowNodeWindow(); }
    void showNodeWindow() override {
        ++openRequests;
        if(!externalEditor) ofxOceanodeNodeModel::showNodeWindow();
    }
};
int main() {
    ofDisableDataPath(); ofSeedRandom();
    auto window=std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry=std::make_shared<ofxOceanodeNodeRegistry>();
    auto types=std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner=std::make_shared<ofxOceanodeContainer>(registry,types); owner->setCanvasID("Canvas");
    auto& model=ofxOceanodeTimeline::attach(*owner);
    auto& first=owner->createNode(std::make_unique<NavigationSynth>(),1);
    auto& second=owner->createNode(std::make_unique<NavigationSynth>(),2);
    auto& synth=static_cast<NavigationSynth&>(first.getNodeModel());
    auto& plugin=static_cast<NavigationSynth&>(second.getNodeModel()); plugin.externalEditor=true;
    auto& pitch=static_cast<ofxOceanodeAbstractParameter&>(first.getParameters().get("Pitch"));
    auto& gate=static_cast<ofxOceanodeAbstractParameter&>(first.getParameters().get("Gate"));
    auto& pluginPitch=static_cast<ofxOceanodeAbstractParameter&>(second.getParameters().get("Pitch"));
    const auto t=model.createTrack("Synth"), pb=model.addBinding(t,pitch), gb=model.addBinding(t,gate);
    const auto ng=model.createNoteGroup(t,"Notes",pb,gb,"");
    std::string lane; const auto c=model.createNoteGroupClip(t,ng,"Phrase",0,4,&lane);
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
    io.DisplaySize=ImVec2(1280,1000); io.DeltaTime=1.f/60;
    unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    ofxOceanodeTimelineController ui(owner);
    auto frame=[&](ImVec2 mouse=ImVec2(-100,-100),bool down=false) {
        io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
        ImGui::NewFrame(); ui.drawBeforeWindows();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1250,980));
        ImGui::Begin("Timeline",nullptr,ImGuiWindowFlags_NoDecoration); ui.draw(); ImGui::End();
        if(synth.show) {
            ImGui::SetNextWindowPos(ImVec2(800,400),ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(300,180),ImGuiCond_Once);
            ImGui::Begin(synth.getWindowTitle().c_str()); ImGui::TextUnformatted("Synth controls"); ImGui::End();
        }
        ImGui::Render();
    };
    frame(); frame();
    auto closePopup=[&](){io.AddKeyEvent(ImGuiKey_Escape,true);frame();io.AddKeyEvent(ImGuiKey_Escape,false);frame();};
    auto openMenu=[&](bool clip) {
        const auto row=ui.clipDropRows.front();
        const ImVec2 point(clip ? ui.fileDropBounds.x+50 : ui.fileDropBounds.x-120,(row.top+row.bottom)*0.5f);
        frame(point); io.AddMouseButtonEvent(1,true);frame(point);
        io.AddMouseButtonEvent(1,false);frame(point);frame();
        assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
    };
    auto menuPoint=[&](const char* label) {
        auto* popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window; assert(popup);
        const auto id=popup->GetID(label);
        for(float y=popup->Pos.y+5;y<popup->Pos.y+popup->Size.y-5;y+=3) {
            const ImVec2 point(popup->Pos.x+45,y);frame(point);
            if(ImGui::GetCurrentContext()->HoveredId==id) return point;
            for(int index=0;index<4;++index)
                if(ImGui::GetCurrentContext()->HoveredId==ImHashStr(label,0,popup->GetID(index))) return point;
        }
        std::cerr << "Missing action: " << label << " in " << popup->Name << "\n";
        assert(false && "Navigation menu action not found"); return ImVec2();
    };
    auto click=[&](const char* label){const auto point=menuPoint(label);frame(point,true);frame(point,false);};
    // Both context menus open the actual dockable editor and focus its window.
    for(bool clip : {false,true}) {
        openMenu(clip); click("Show synth in window"); assert(synth.show && synth.openRequests>0);
        frame();frame();
        assert(ImGui::GetCurrentContext()->NavWindow &&
               std::string(ImGui::GetCurrentContext()->NavWindow->Name)==synth.getWindowTitle());
        synth.show=false;frame();
        navigationCanvasTarget=nullptr;
        openMenu(clip);click("Show synth in canvas");
        assert(navigationCanvasTarget==&pitch || navigationCanvasTarget==&gate);
    }
    // A second instrument on the track doesn't affect a clip routed only to the first.
    const auto pluginBinding=model.addBinding(t,pluginPitch);
    openMenu(true);click("Show synth in window");assert(synth.show);synth.show=false;frame();
    // Mixed tracks choose an explicit synth; plugin windows dispatch their native action.
    openMenu(false);
    const auto parent=menuPoint("Show synth in window");
    for(int i=0;i<30;++i)frame(parent);
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size==2);
    click(plugin.getParameterGroup().getName().c_str());
    assert(plugin.openRequests==1 && ui.pendingSynthWindowFocusFrames==0);
    // Routing the clip to both instruments exposes the same explicit choices.
    const auto extra=model.createLane(t,c,"Plugin",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(t,c,extra,pluginBinding);
    openMenu(true);const auto mixed=menuPoint("Show synth in canvas");
    for(int i=0;i<30;++i)frame(mixed);
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size==2);
    navigationCanvasTarget=nullptr;click(plugin.getParameterGroup().getName().c_str());assert(navigationCanvasTarget==&pluginPitch);
    ImGui::DestroyContext();
    std::cout << "Synth navigation passed: track/clip menus, routing scope, dockable focus, canvas target, multiple synths and native editor actions.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
