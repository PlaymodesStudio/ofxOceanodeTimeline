#!/usr/bin/env python3
"""A request from a parameter menu (Add to Timeline > New Track...) reaches the
window showing the timeline: the main controller, drawn as the app draws it
(drawBeforeWindows only, never as an editor), opens the timeline's window,
shows the dialog, and the dialog survives the window being brought forward.
"""
import attachment_regression as native
from pathlib import Path

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
// Inspect transient shell state only; all drawing and mutations use production code.
#define private public
#include "Controls/ofxOceanodeTimelineController.h"
#undef private
#include "imgui_internal.h"
#include "Controls/Editors/ofxOceanodeTimelineEditorUi.h"
#include "Controls/Editors/ofxOceanodeTimelinePianoRollEditor.h"
#include <cstdlib>
class ClarityProbe : public ofxOceanodeNodeModel {
public:
    ClarityProbe() : ofxOceanodeNodeModel("Synth") {}
    ofParameter<float> pitch, gate, cutoff;
    void setup() override {
        addParameter(pitch.set("Pitch", 60, 0, 127));
        addParameter(gate.set("Gate", 0, 0, 1));
        addParameter(cutoff.set("Cutoff", 0.5, 0, 1));
    }
};
void capture(const char* name, unsigned char* atlas, int aw, int ah) {
    const char* folder = std::getenv("OCEANODE_TIMELINE_CAPTURE_DIR");
    if(folder == nullptr) return;
    auto* data = ImGui::GetDrawData();
    const int w = static_cast<int>(data->DisplaySize.x), h = static_cast<int>(data->DisplaySize.y);
    std::vector<unsigned char> pixels(w*h*3, 25);
    for(int listIndex=0; listIndex<data->CmdListsCount; ++listIndex) {
        const auto* list = data->CmdLists[listIndex];
        for(const auto& command : list->CmdBuffer) {
            if(command.UserCallback) continue;
            for(unsigned i=0; i+2<command.ElemCount; i+=3) {
                const auto& a=list->VtxBuffer[list->IdxBuffer[command.IdxOffset+i]+command.VtxOffset];
                const auto& b=list->VtxBuffer[list->IdxBuffer[command.IdxOffset+i+1]+command.VtxOffset];
                const auto& c=list->VtxBuffer[list->IdxBuffer[command.IdxOffset+i+2]+command.VtxOffset];
                const float area=(b.pos.x-a.pos.x)*(c.pos.y-a.pos.y)-(b.pos.y-a.pos.y)*(c.pos.x-a.pos.x);
                if(std::abs(area)<0.00001f) continue;
                const int x0=std::max(0,static_cast<int>(std::max(command.ClipRect.x,std::floor(std::min({a.pos.x,b.pos.x,c.pos.x})))));
                const int y0=std::max(0,static_cast<int>(std::max(command.ClipRect.y,std::floor(std::min({a.pos.y,b.pos.y,c.pos.y})))));
                const int x1=std::min(w,static_cast<int>(std::min(command.ClipRect.z,std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x})))));
                const int y1=std::min(h,static_cast<int>(std::min(command.ClipRect.w,std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y})))));
                for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
                    const float px=x+0.5f, py=y+0.5f;
                    const float v=((px-a.pos.x)*(c.pos.y-a.pos.y)-(py-a.pos.y)*(c.pos.x-a.pos.x))/area;
                    const float z=((b.pos.x-a.pos.x)*(py-a.pos.y)-(b.pos.y-a.pos.y)*(px-a.pos.x))/area;
                    const float u=1-v-z;
                    if(u<0 || v<0 || z<0) continue;
                    const int tx=std::clamp(static_cast<int>((u*a.uv.x+v*b.uv.x+z*c.uv.x)*aw),0,aw-1);
                    const int ty=std::clamp(static_cast<int>((u*a.uv.y+v*b.uv.y+z*c.uv.y)*ah),0,ah-1);
                    const float alpha=(u*((a.col>>24)&255)+v*((b.col>>24)&255)+z*((c.col>>24)&255))/255.f * atlas[(ty*aw+tx)*4+3]/255.f;
                    for(int channel=0;channel<3;++channel) {
                        const float color=u*((a.col>>(8*channel))&255)+v*((b.col>>(8*channel))&255)+z*((c.col>>(8*channel))&255);
                        auto& pixel=pixels[(y*w+x)*3+channel]; pixel=static_cast<unsigned char>(pixel*(1-alpha)+color*alpha);
                    }
                }
            }
        }
    }
    std::filesystem::create_directories(folder);
    std::ofstream file(std::string(folder)+"/"+name+".ppm",std::ios::binary);
    file<<"P6\n"<<w<<" "<<h<<"\n255\n"; file.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
}
int main() {
    ofDisableDataPath(); ofSeedRandom();
    auto window=std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry=std::make_shared<ofxOceanodeNodeRegistry>();
    auto types=std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner=std::make_shared<ofxOceanodeContainer>(registry,types);
    owner->setCanvasID("Canvas");
    auto& model=ofxOceanodeTimeline::attach(*owner);
    model.setEditTarget(model.addTimeline("", "Timeline 1"));
    // A macro's timeline (its first), made on first use as the macro's own one was.
    auto macroTimelineOf = [&](ofxOceanodeContainer& canvas) -> ofxOceanodeTimelineSequence* {
        auto* list = model.getMacroTimelines(canvas);
        if(list == nullptr) return nullptr;
        if(list->timelines.empty()) model.createTimeline(canvas.getCanvasID(), model.getMacroSequenceLabel(canvas));
        return list->timelines.empty() ? nullptr : list->timelines.front().get();
    };
    auto& node=owner->createNode(std::make_unique<ClarityProbe>(),1);
    auto bind=[&](const std::string& track,const char* name) {
        return model.addBinding(track,static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get(name)));
    };
    const auto t=model.createTrack("Synth phrases");
    const auto pitch=bind(t,"Pitch"), gate=bind(t,"Gate"), cutoff=bind(t,"Cutoff");
    const auto c=model.createClip(t,"Intro",0,4);
    const auto piano=model.createLane(t,c,"Melody",ofxOceanodeTimelineLaneType::PianoRoll);
    model.addBindingToLane(t,c,piano,pitch); model.addBindingToLane(t,c,piano,gate);
    model.getLane(t,c,piano)->pianoNotes={{0,1,60,0.8f,1},{1,1,64,0.7f,1},{2,1,67,0.8f,1}};
    const auto curve=model.createLane(t,c,"Filter sweep",ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t,c,curve,cutoff);
    const auto step=model.createLane(t,c,"Rhythm",ofxOceanodeTimelineLaneType::Step);
    model.addBindingToLane(t,c,step,gate); model.setClipStep(t,c,step,0,"1",0.25);
    const auto wt=model.createWaveTrack("Samples");
    const auto wc=model.createClip(wt,"Audio slice",4,4);
    const auto lc=[&]{ const auto lfoClipId=model.createClip(t,"Filter modulation",5,4); const auto lfoLaneId=model.createLane(t,lfoClipId,"LFO",ofxOceanodeTimelineLaneType::Lfo); const std::string lfoTarget=cutoff; if(!lfoTarget.empty()) model.addBindingToLane(t,lfoClipId,lfoLaneId,lfoTarget); return lfoClipId; }();
    ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,1000); io.DeltaTime=1.f/60;
    unsigned char* atlas; int aw,ah; io.Fonts->GetTexDataAsRGBA32(&atlas,&aw,&ah);
    // Tracks start at the minimum height; the fixed screen points below
    // were laid out for 80px rows.
    assert(std::abs(model.getViewState().trackHeight-kMinTrackHeight)<0.01f);
    model.getViewState().trackHeight=80.0f;
    ofxOceanodeTimelineController ui(owner);

    // The app: the main controller draws no window of its own; the core calls
    // drawBeforeWindows every frame and draw() never.
    auto frame=[&](ImVec2 mouse=ImVec2(-100,-100),bool down=false) {
        io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
        ImGui::NewFrame();
        ui.drawBeforeWindows();
        ui.drawPopups();
        ImGui::Render();
    };
    frame(); frame();
    assert(model.getTimelineWindows().empty() && model.editSequence().tracks.size()==2);
    auto& cutoffParam=static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Cutoff"));
    const auto ref=model.getEditTarget();
    model.requestParameterClip({ref,"",owner->getTimelineParameterPath(cutoffParam)});
    frame(); frame(); frame();
    // The timeline's window opened for the request; its dialog is up.
    assert(model.getTimelineWindows().size()==1 && model.getTimelineWindows().front().ref==ref);
    auto* dialog=ImGui::FindWindowByName("Add to Timeline##parameterClip");
    assert(dialog!=nullptr && dialog->Active && ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)); // (no window is current here: any-id only)
    const ImVec2 confirm(dialog->Pos.x+ImGui::GetStyle().WindowPadding.x+25,
        dialog->Pos.y+dialog->Size.y-ImGui::GetStyle().WindowPadding.y-ImGui::GetFrameHeight()*0.5f);
    frame(confirm,false); frame(confirm,true); frame(confirm,false); frame(); frame();
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size==0);
    assert(model.editSequence().tracks.size()==3);
    const auto& made=model.editSequence().tracks.back();
    assert(made.name=="Cutoff" && made.clips.size()==1 && made.bindings.size()==1);
    assert(made.bindings.front().parameterPath==owner->getTimelineParameterPath(cutoffParam));
    // The window's editor shows the timeline the track went to.
    assert(ui.windowEditors.size()==1 && ui.windowEditors.begin()->second->shownSequence==model.findTimeline(ref));
    ImGui::DestroyContext();
    std::cout << "Window request passed: Add to Timeline from a parameter menu opens the timeline's window, its dialog stays up, the track is made.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
