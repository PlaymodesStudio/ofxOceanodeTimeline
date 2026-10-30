#!/usr/bin/env python3
"""Exercise the complete production controller with native core and headless ImGui.

Optional OCEANODE_TIMELINE_CAPTURE_DIR writes CPU-rendered PPM screenshots for
visual inspection. No GPU, audio backend or application window is required.
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
    const auto lc=model.createLfoClip(t,cutoff,"Filter modulation",5,4);
    ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,1000); io.DeltaTime=1.f/60;
    unsigned char* atlas; int aw,ah; io.Fonts->GetTexDataAsRGBA32(&atlas,&aw,&ah);
    ofxOceanodeTimelineController ui(owner);
    auto frame=[&](float width=1250,ImVec2 mouse=ImVec2(-100,-100),bool down=false,const char* screenshot=nullptr) {
        io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
        ImGui::NewFrame();
        ui.drawBeforeWindows();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(width,980));
        ImGui::Begin("Timeline",nullptr,ImGuiWindowFlags_NoDecoration);
        ui.draw(); ImGui::End(); ImGui::Render();
        if(screenshot) capture(screenshot,atlas,aw,ah);
    };
    frame(); frame();
    assert(!ui.targetsView && ui.clipScreenRects.size()==3);
    frame(1250,ImVec2(-100,-100),false,"clips");
    // Track pads own a separate selection; modifiers toggle tracks without
    // selecting their clips or mutating the preset. Both track types participate.
    const auto selectionModel=model.toJson();
    auto padClick=[&](const std::string& track) {
        const auto row=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),
            [&](const auto& r){return r.trackId==track;});
        assert(row!=ui.clipDropRows.end());
        const ImVec2 pos(ui.fileDropBounds.x-120,(row->top+row->bottom)*0.5f);
        frame(1250,pos,false); frame(1250,pos,true); frame(1250,pos,false);
    };
    padClick(t);
    assert(ui.selectedTracks==std::set<std::string>{t} && ui.selectedClips.empty());
    bool outlined=false;
    for(const auto* list:ImGui::GetDrawData()->CmdLists)
        for(const auto& vertex:list->VtxBuffer)
            outlined |= vertex.col==IM_COL32(150,190,255,255);
    assert(outlined);
    capture("track-selected",atlas,aw,ah);
    io.AddKeyEvent(ImGuiMod_Shift,true); padClick(wt);
    assert((ui.selectedTracks==std::set<std::string>{t,wt}));
    for(int i=0;i<25;++i) frame(); // separate pad clicks from double-click rename
    padClick(wt); assert(ui.selectedTracks==std::set<std::string>{t});
    io.AddKeyEvent(ImGuiMod_Shift,false);
    for(int i=0;i<25;++i) frame();
    padClick(wt); assert(ui.selectedTracks==std::set<std::string>{wt});
    const auto firstRect=ui.clipScreenRects.front();
    const ImVec2 clipPos((firstRect.x1+firstRect.x2)*0.5f,(firstRect.y1+firstRect.y2)*0.5f);
    frame(1250,clipPos,false); frame(1250,clipPos,true); frame(1250,clipPos,false);
    assert(ui.selectedTracks.empty() && ui.selectedClips.count({t,c}));
    padClick(t); assert(ui.selectedClips.empty());
    const ImVec2 emptyPos(ui.fileDropBounds.getRight()-20,(firstRect.y1+firstRect.y2)*0.5f);
    frame(1250,emptyPos,false); frame(1250,emptyPos,true); frame(1250,emptyPos,false);
    assert(ui.selectedTracks.empty() && ui.selectedClips.empty());
    assert(model.toJson()==selectionModel);

    // Real track M/S buttons work on automation and audio tracks.
    auto trackToggle=[&](const std::string& track,float offset) {
        const auto row=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),[&](const auto& r){return r.trackId==track;});
        assert(row!=ui.clipDropRows.end());
        const ImVec2 point(ui.fileDropBounds.x+offset,(row->top+row->bottom)*0.5f);
        frame(1250,point,false); frame(1250,point,true); frame(1250,point,false);
    };
    for(const auto& track : {t,wt}) {
        trackToggle(track,-74); assert(model.getTrack(track)->muted);
        trackToggle(track,-74); assert(!model.getTrack(track)->muted);
        trackToggle(track,-49); assert(model.getTrack(track)->solo);
        trackToggle(track,-49); assert(!model.getTrack(track)->solo);
    }

    // Wheel zoom keeps the time under the pointer fixed, including the right
    // edge where the old scroll limit would incorrectly clamp a zoom-in.
    const auto originalView=model.getViewState();
    auto& zoomView=model.getViewState();
    ui.followPlayhead=false;
    zoomView.pixelsPerSecond=80; frame();
    const float zoomVisible=ImGui::FindWindowByName("Timeline")->WorkRect.GetWidth()-ofxOceanodeTimelineEditorUi::labelWidth;
    const float oldZoomMax=std::max(0.0f,static_cast<float>(model.beatToSeconds(ui.getContentEndBeat(model),120)*80)-zoomVisible);
    zoomView.scrollX=oldZoomMax; frame();
    const ImVec2 zoomPoint(ui.fileDropBounds.x+700,ui.fileDropBounds.y+25);
    frame(1250,zoomPoint,false);
    const double timeAtPointer=(zoomView.scrollX+700)/zoomView.pixelsPerSecond;
    io.AddMouseWheelEvent(0,1); frame(1250,zoomPoint,false);
    assert(zoomView.pixelsPerSecond>80 && zoomView.scrollX>oldZoomMax);
    assert(std::abs((zoomView.scrollX+700)/zoomView.pixelsPerSecond-timeAtPointer)<0.0001);
    assert(ui.zoomAnchorUntil>ImGui::GetTime());
    bool zoomGuide=false;
    for(const auto* list:ImGui::GetDrawData()->CmdLists) for(const auto& v:list->VtxBuffer)
        zoomGuide |= v.col==IM_COL32(245,195,90,160);
    assert(zoomGuide);
    const auto afterWheel=zoomView;
    frame(1250,zoomPoint,false); assert(zoomView.scrollX==afterWheel.scrollX);
    // The pinned ruler uses exactly the same anchor and wheel handling.
    const ImVec2 rulerZoom(zoomPoint.x,ui.fileDropBounds.y-15);
    frame(1250,rulerZoom,false);
    const double rulerTime=(zoomView.scrollX+700)/zoomView.pixelsPerSecond;
    io.AddMouseWheelEvent(0,-1); frame(1250,rulerZoom,false);
    assert(std::abs((zoomView.scrollX+700)/zoomView.pixelsPerSecond-rulerTime)<0.0001);
    // Locate and click the actual toolbar button by its hover id.
    auto toolbarPoint=[&](const char* key) {
        const auto id=ImGui::FindWindowByName("Timeline")->GetID(key);
        for(float x=280;x<650;x+=4) {
            const ImVec2 point(x,46); frame(1250,point,false);
            if(ImGui::GetCurrentContext()->HoveredId==id) return point;
        }
        assert(false && "Toolbar button not found"); return ImVec2();
    };
    auto clickZoom=[&]() {
        const auto point=toolbarPoint("##tbZoomIn");
        frame(1250,point,true); frame(1250,point,false);
    };
    owner->getTransport()->seekToBeat(8); zoomView.pixelsPerSecond=140; zoomView.scrollX=100; frame();
    const double playheadSeconds=model.beatToSeconds(8,120);
    const double playheadScreen=playheadSeconds*zoomView.pixelsPerSecond-zoomView.scrollX;
    clickZoom();
    assert(std::abs(playheadSeconds*zoomView.pixelsPerSecond-zoomView.scrollX-playheadScreen)<0.001);
    // With the playhead offscreen, toolbar zoom keeps the viewport center fixed.
    owner->getTransport()->seekToBeat(0); zoomView.scrollX=200; frame();
    const double centerTime=(zoomView.scrollX+zoomVisible*0.5)/zoomView.pixelsPerSecond;
    clickZoom();
    assert(std::abs((zoomView.scrollX+zoomVisible*0.5)/zoomView.pixelsPerSecond-centerTime)<0.0001);
    model.getViewState()=originalView; ui.zoomAnchorUntil=0; frame();
    // Open the real import modal, remap saved targets, and confirm using the
    // mouse. Import stays deferred until confirmation and remains undoable.
    const auto beforeImport=model.toJson()["tracks"];
    const auto originalCount=model.getTrack(t)->clips.size();
    ui.clipImportDocument=model.exportClip(t,c);
    ui.clipFileTrackId=t; ui.clipImportBeat=20;
    ui.clipImportTargetPaths={{pitch,model.getBinding(t,pitch)->parameterPath},
        {gate,model.getBinding(t,gate)->parameterPath},{cutoff,model.getBinding(t,cutoff)->parameterPath}};
    ui.requestClipImportPopup=true;
    frame(); frame(1250,ImVec2(-100,-100),false,"import-clip");
    assert(model.toJson()["tracks"]==beforeImport);
    auto* importWindow=ImGui::FindWindowByName("Load reusable clip"); assert(importWindow && importWindow->Active);
    const ImVec2 confirm(importWindow->Pos.x+ImGui::GetStyle().WindowPadding.x+25,
        importWindow->Pos.y+importWindow->Size.y-ImGui::GetStyle().WindowPadding.y-ImGui::GetFrameHeight()*0.5f);
    frame(1250,confirm,false); frame(1250,confirm,true); frame(1250,confirm,false);
    assert(model.getTrack(t)->clips.size()==originalCount+1);
    const auto importedId=ui.editorState.editorClipId;
    assert(importedId!=c && model.getClip(t,importedId)->lanes.size()==3);
    assert(model.getClip(t,importedId)->startBeat==20);
    assert(model.undoTimelineEdit()); assert(model.getTrack(t)->clips.size()==originalCount);
    assert(model.redoTimelineEdit()); assert(model.getTrack(t)->clips.size()==originalCount+1);
    model.removeClip(t,importedId);
    ui.editorState.clipEditorOpen=false; frame();
    assert(model.toJson()["tracks"]==beforeImport);
    ui.clipImportDocument=model.exportClip(t,c); ui.clipImportTargetPaths.clear();
    ui.requestClipImportPopup=true; frame();
    io.AddKeyEvent(ImGuiKey_Escape,true); frame(); io.AddKeyEvent(ImGuiKey_Escape,false); frame();
    assert(ImGui::GetCurrentContext()->OpenPopupStack.empty() && model.toJson()["tracks"]==beforeImport);
    ui.editorState.editorTrackId=t; ui.editorState.editorClipId=c;
    ui.editorState.editorLaneId=curve; ui.editorState.clipEditorOpen=true;
    const auto before=model.toJson()["tracks"];
    frame(); frame(1250,ImVec2(-100,-100),false,"clip-editor");
    assert(ui.editorState.clipEditorOpen && ui.editorState.editorClipId==c && model.toJson()["tracks"]==before);
    auto selectTab=[&](const char* name) {
        for(int i=0;i<ImGui::GetCurrentContext()->TabBars.GetMapSize();++i) {
            auto* bar=ImGui::GetCurrentContext()->TabBars.TryGetMapData(i);
            if(!bar) continue;
            for(auto& tab:bar->Tabs)
                if(std::string(ImGui::TabBarGetTabName(bar,&tab))==name) bar->NextSelectedTabId=tab.ID;
        }
    };
    selectTab("Lane");
    frame(); frame(1250,ImVec2(-100,-100),false,"content-properties");
    assert(model.toJson()["tracks"]==before);
    selectTab("Clip");
    frame(); frame(1250,ImVec2(-100,-100),false,"clip-properties");
    assert(model.toJson()["tracks"]==before);
    {
        // Mutation: MUT strip handles, outlines and the Mutate tab.
        auto* melody=model.getLane(t,c,piano);
        melody->pianoNotes[1].mutation=0.6f; melody->mutation.scaleMask=0xAB5; melody->mutation.pitchDirection=1;
        selectTab("Mutate");
        frame(); frame(1250,ImVec2(-100,-100),false,"mutation");
        assert(model.getLane(t,c,piano)->mutation.scaleMask==0xAB5);
        {
            // Right-clicking Range opens its direction menu, not the lane menu.
            const ImVec2 range(150,444);
            frame(1250,range,false);
            io.AddMouseButtonEvent(1,true); frame(1250,range,false);
            io.AddMouseButtonEvent(1,false); frame(1250,range,false);
            frame();
            assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
            auto* popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window; assert(popup);
            const ImVec2 downOnly(popup->Pos.x+30,popup->Pos.y+ImGui::GetStyle().WindowPadding.y+
                ImGui::GetTextLineHeight()*0.5f+2*ImGui::GetTextLineHeightWithSpacing());
            frame(1250,downOnly,false); frame(1250,downOnly,true); frame(1250,downOnly,false); frame();
            assert(model.getLane(t,c,piano)->mutation.pitchDirection==2);
            assert(model.getClip(t,c)->lanes.size()==3); // no lane duplicated
        }
        {
            // Rndm tab: right-clicking a deviation slider opens its direction
            // menu, not the lane menu; the pad around it still opens the lane's.
            const auto focusedLane=ui.editorState.editorLaneId;
            selectTab("Rndm");
            frame(); frame(1250,ImVec2(-100,-100),false,"random");
            ImGuiWindow* panel=nullptr;
            for(auto* window:ImGui::GetCurrentContext()->Windows)
                if(window->Active && std::string(window->Name).find("##clipProperties"+t+c+piano)!=std::string::npos) panel=window;
            assert(panel);
            // The tab's content is in the tab's id scope.
            ImGuiID randomTab=0;
            for(int i=0;i<ImGui::GetCurrentContext()->TabBars.GetMapSize();++i)
                if(auto* bar=ImGui::GetCurrentContext()->TabBars.TryGetMapData(i))
                    for(auto& tab:bar->Tabs)
                        if(std::string(ImGui::TabBarGetTabName(bar,&tab))=="Rndm") randomTab=tab.ID;
            assert(randomTab);
            const ImGuiID slider=ImHashStr("##randomOctave",0,randomTab);
            ImVec2 point(-1,-1);
            for(float y=panel->Pos.y;y<panel->Pos.y+panel->Size.y && point.x<0;y+=3) {
                const ImVec2 probe(panel->Pos.x+panel->Size.x-30,y);
                frame(1250,probe,false);
                if(ImGui::GetCurrentContext()->HoveredId==slider) point=probe;
            }
            assert(point.x>=0);
            io.AddMouseButtonEvent(1,true); frame(1250,point,false);
            io.AddMouseButtonEvent(1,false); frame(1250,point,false);
            frame();
            assert(ImGui::GetCurrentContext()->OpenPopupStack.size()==1);
            auto* popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window; assert(popup);
            assert(ImGui::GetCurrentContext()->OpenPopupStack.back().PopupId==ImHashStr("##randomOctaveDirection",0,randomTab));
            const ImVec2 upOnly(popup->Pos.x+30,popup->Pos.y+ImGui::GetStyle().WindowPadding.y+
                ImGui::GetTextLineHeight()*0.5f+1*ImGui::GetTextLineHeightWithSpacing());
            frame(1250,upOnly,false); frame(1250,upOnly,true); frame(1250,upOnly,false); frame();
            assert(model.getLane(t,c,piano)->randomOctaveDirection==1);
            assert(ImGui::GetCurrentContext()->OpenPopupStack.empty());
            assert(model.getClip(t,c)->lanes.size()==3); // no lane duplicated
            // Over the lane's summary line: the lane menu.
            const ImVec2 pad(panel->Pos.x+35,panel->Pos.y+36);
            frame(1250,pad,false);
            io.AddMouseButtonEvent(1,true); frame(1250,pad,false);
            io.AddMouseButtonEvent(1,false); frame(1250,pad,false);
            frame();
            assert(ImGui::GetCurrentContext()->OpenPopupStack.size()==1);
            assert(ImGui::GetCurrentContext()->OpenPopupStack.back().PopupId==ImHashStr(("##laneMenu"+piano).c_str(),0,panel->ID));
            ImGui::ClosePopupToLevel(0,true); frame();
            assert(ImGui::GetCurrentContext()->OpenPopupStack.empty() && model.getClip(t,c)->lanes.size()==3);
            model.getLane(t,c,piano)->randomOctaveDirection=0;
            ui.editorState.editorLaneId=focusedLane;
            selectTab("Mutate"); frame(); frame();
        }
        // Group B: its own keys, the second and third notes in it, edited in the tab.
        {
            auto* lane=model.getLane(t,c,piano);
            ofxOceanodeTimelineMutationGroup b; b.rules=lane->mutation; b.rules.scaleMask=0x295;
            b.rules.lengthStrength=0.5f; b.overrides=ofxOceanodeTimelineMutationRules::ScaleMask|ofxOceanodeTimelineMutationRules::LengthStrength;
            lane->mutationGroups={b};
            lane->pianoNotes[1].mutationGroup=1; lane->pianoNotes[2].mutationGroup=1; lane->pianoNotes[2].mutation=0.3f;
            auto& pianoEditor=static_cast<ofxOceanodeTimelinePianoRollEditor&>(ui.laneEditors.forType(ofxOceanodeTimelineLaneType::PianoRoll));
            pianoEditor.mutationEditGroup=1;
            frame(); frame(1250,ImVec2(-100,-100),false,"mutation-group");
            lane=model.getLane(t,c,piano);
            assert(lane->mutationGroups.size()==1 && lane->mutationGroups[0].overrides==b.overrides);
            lane->mutationGroups.clear(); lane->pianoNotes[1].mutationGroup=0; lane->pianoNotes[2].mutationGroup=0;
            lane->pianoNotes[2].mutation=0.0f; pianoEditor.mutationEditGroup=0;
        }
        // Scale overlay in the Lane tab and the roll.
        {
            auto* lane=model.getLane(t,c,piano);
            lane->pianoScaleEnabled=true; lane->pianoScaleMask=0x5AD;
            selectTab("Lane");
            frame(); frame(1250,ImVec2(-100,-100),false,"scale");
            lane=model.getLane(t,c,piano);
            assert(lane->pianoScaleEnabled && lane->pianoScaleMask==0x5AD);
            lane->pianoScaleEnabled=false; lane->pianoScaleMask=0xAB5;
        }
        // The piano roll's key scrollbar and zoom buttons work on an
        // unfocused lane too (another lane being edited).
        {
            assert(ui.editorState.editorLaneId!=piano);
            auto click=[&](ImVec2 at){ frame(1250,at,false); frame(1250,at,true); frame(1250,at,false); for(int i=0;i<25;++i) frame(); };
            const int low=model.getLane(t,c,piano)->pianoLowPitch, high=model.getLane(t,c,piano)->pianoHighPitch;
            click(ImVec2(240,302)); // "+": zoom in
            auto* lane=model.getLane(t,c,piano);
            assert(lane->pianoHighPitch-lane->pianoLowPitch < high-low);
            const int zoomedLow=lane->pianoLowPitch;
            click(ImVec2(240,600)); // track near the bottom: jump down
            lane=model.getLane(t,c,piano);
            assert(lane->pianoLowPitch < zoomedLow);
            assert(ui.editorState.editorLaneId!=piano);
            lane->pianoLowPitch=low; lane->pianoHighPitch=high;
        }
        model.getLane(t,c,piano)->pianoNotes[1].mutation=0.0f; model.getLane(t,c,piano)->mutation.scaleMask=0xFFF; model.getLane(t,c,piano)->mutation.pitchDirection=0;
    }
    selectTab("Targets"); frame();
    ui.targetsView=true;
    frame(); frame(1250,ImVec2(-100,-100),false,"targets");
    assert(ui.editorState.clipEditorOpen && ui.clipScreenRects.size()>3);
    // A collapsed track keeps its open editor anchored below its clip row.
    model.getTrack(t)->collapsed=true; frame(); assert(ui.editorState.clipEditorOpen);
    model.getTrack(t)->collapsed=false;
    frame(460); frame(460,ImVec2(-100,-100),false,"narrow");
    ui.targetsView=false;
    ui.editorState.editorClipId=lc; ui.editorState.editorLaneId=model.getClip(t,lc)->lanes.front().id;
    frame(); frame(1250,ImVec2(-100,-100),false,"lfo");
    ui.editorState.editorTrackId=wt; ui.editorState.editorClipId=wc;
    frame(); frame(1250,ImVec2(-100,-100),false,"audio");
    ui.editorState.clipEditorOpen=false;
    ui.targetInspectorOpen=true; ui.inspectedTargetPath=model.getBinding(t,cutoff)->parameterPath;
    frame(); frame(1250,ImVec2(-100,-100),false,"inspector");
    assert(model.toJson()["tracks"]==before);
    ui.targetInspectorOpen=false;
    // Exercise real mouse gestures with the explicit tool, checking shared
    // source mapping rather than merely calling the model API.
    for(int tool=0;tool<3;++tool) {
        auto* clip=model.getClip(t,c); clip->durationBeats=4; clip->contentDurationBeats=4;
        clip->contentStretch=1; clip->repeatContent=true;
        ui.endEditTool=tool; ui.snapEnabled=false;
        // Separate clicks by more than ImGui's double-click interval.
        for(int settle=0; settle<25; ++settle) frame();
        const auto rect=ui.clipScreenRects.front();
        const float x=rect.x2-2, y=(rect.y1+rect.y2)*0.5f;
        frame(1250,ImVec2(x,y),false); frame(1250,ImVec2(x,y),true);
        frame(1250,ImVec2(x+70,y),true); frame(1250,ImVec2(x+70,y),false);
        clip=model.getClip(t,c);
        assert(clip->durationBeats>4.5);
        if(tool==0) { assert(clip->repeatContent); assert(clip->contentDurationBeats==4); }
        if(tool==1) { assert(!clip->repeatContent); assert(clip->contentDurationBeats>4.5); }
        if(tool==2) { assert(!clip->repeatContent); assert(clip->contentDurationBeats==4); assert(clip->contentStretch>1); }
    }
    // Cmd+drag (stretch) of the right edge back past the start: the clip
    // flips to the left of its start and plays backwards, stretched there.
    {
        // (beats 2-4: clear of the LFO clip on this track, which would take the click)
        auto* clip=model.getClip(t,c); clip->startBeat=2; clip->durationBeats=2; clip->contentDurationBeats=2;
        clip->contentStretch=1; clip->repeatContent=true; clip->contentOffsetBeats=0;
        const auto curveBefore=*model.getLane(t,c,curve);
        ui.endEditTool=2; ui.snapEnabled=false; // the Stretch tool (Cmd+drag picks it too)
        for(int settle=0;settle<25;++settle) frame();
        const auto rect=*std::find_if(ui.clipScreenRects.begin(),ui.clipScreenRects.end(),[&](const auto& r){return r.clipId==c;});
        const float beatPixels=(rect.x2-rect.x1)/2.0f;
        const float x=rect.x2-2, y=(rect.y1+rect.y2)*0.5f;
        frame(1250,ImVec2(x,y),false); frame(1250,ImVec2(x,y),true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(x+(rect.x1-beatPixels-x)*i/6.0f,y),true); // to one beat before the start
        frame(1250,ImVec2(rect.x1-beatPixels,y),false); frame();
        clip=model.getClip(t,c);
        assert(std::abs(clip->startBeat-1.0)<0.05 && std::abs(clip->durationBeats-1.0)<0.05); // left of the old start
        const auto& curveAfter=*model.getLane(t,c,curve);
        const float endValue=ofxOceanodeTimelineCurve::valueAtBeat(curveBefore.curvePoints,curveBefore.curveTensions,curveBefore.curveInterpolation,2.0,0.0f);
        assert(std::abs(curveAfter.curvePoints.front().value-endValue)<1e-4f); // starts where it ended: backwards
        assert(model.reverseClip(t,c)); // and back, for the tests below
        clip=model.getClip(t,c); clip->startBeat=0; clip->durationBeats=4; clip->contentDurationBeats=4;
    }
    // A one-shot's effective mapping is derived from its length, even when
    // an external producer left an old stretch field. Drag and numeric edits
    // must both preserve that effective mapping when enabling repeats.
    {
        auto* oneShot=model.getClip(t,c);
        oneShot->durationBeats=4; oneShot->contentDurationBeats=2;
        oneShot->contentStretch=1; oneShot->repeatContent=false;
        ui.endEditTool=0;
        for(int settle=0;settle<25;++settle) frame();
        const auto rect=ui.clipScreenRects.front();
        const float x=rect.x2-2, y=(rect.y1+rect.y2)*0.5f;
        frame(1250,ImVec2(x,y),false); frame(1250,ImVec2(x,y),true);
        frame(1250,ImVec2(x+70,y),true); frame(1250,ImVec2(x+70,y),false);
        oneShot=model.getClip(t,c);
        assert(oneShot->repeatContent && oneShot->contentDurationBeats==2);
        assert(std::abs(oneShot->contentStretch-2)<0.00001);
    }
    // Clip menus use the same timing panel and remain balanced in an
    // auto-sized popup, including when the main window is narrow.
    for(int settle=0;settle<25;++settle) frame();
    auto menuRect=ui.clipScreenRects.front();
    ImVec2 menuPoint(menuRect.x1+30,(menuRect.y1+menuRect.y2)*0.5f);
    frame(1250,menuPoint,false);
    io.AddMouseButtonEvent(1,true); frame(1250,menuPoint,false);
    io.AddMouseButtonEvent(1,false); frame(1250,menuPoint,false,"clip-menu");
    io.AddKeyEvent(ImGuiKey_Escape,true); frame();
    io.AddKeyEvent(ImGuiKey_Escape,false); frame();
    // With keyboard navigation disabled, dismiss the menu by clicking outside.
    frame(1250,ImVec2(1100,900),false); frame(1250,ImVec2(1100,900),true);
    frame(1250,ImVec2(1100,900),false); frame();
    assert(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    // Cross-track groups keep their source mappings on repeated drag frames;
    // audio members still stretch their whole slice.
    auto* grouped=model.getClip(t,c); grouped->durationBeats=4; grouped->contentDurationBeats=4;
    grouped->contentStretch=1; grouped->repeatContent=true;
    auto* audio=model.getClip(wt,wc); audio->startBeat=4; audio->durationBeats=4;
    audio->contentDurationBeats=4; audio->waveFileDurationBeats=4;
    assert(!model.groupClips({{t,c},{wt,wc}}).empty());
    ui.endEditTool=0;
    for(int settle=0;settle<25;++settle) frame();
    auto groupRect=ui.clipScreenRects.front();
    const float gx=groupRect.x2-2, gy=(groupRect.y1+groupRect.y2)*0.5f;
    frame(1250,ImVec2(gx,gy),false); frame(1250,ImVec2(gx,gy),true);
    frame(1250,ImVec2(gx+35,gy),true); frame(1250,ImVec2(gx+70,gy),true);
    frame(1250,ImVec2(gx+70,gy),false);
    assert(model.getClip(t,c)->repeatContent && model.getClip(t,c)->contentDurationBeats==4);
    assert(model.getClip(wt,wc)->durationBeats>4.5 && !model.getClip(wt,wc)->repeatContent);
    assert(model.getClip(wt,wc)->contentDurationBeats==4);
    model.ungroupClip(t,c);
    // Structural edits stay deferred, retaining the selected target.
    ui.editorState.editorTrackId=t; ui.editorState.editorClipId=c; ui.editorState.clipEditorOpen=true;
    ui.pendingAddLaneType=1; ui.pendingAddLaneTarget=cutoff; ui.requestAddLane=true;
    frame();
    const auto added=ui.editorState.editorLaneId;
    assert(model.getLane(t,c,added)->bindingIds==std::vector<std::string>{cutoff});
    assert(model.getClip(t,c)->lanes.size()==4);
    ui.pendingRemoveLaneId=added; ui.requestRemoveLane=true; frame();
    assert(model.getClip(t,c)->lanes.size()==3);
    // Right-click the properties pad below its header, including over a
    // control, then activate both menu actions using actual mouse events.
    auto laneAction = [&](const std::string& laneId, int action) {
        frame(); frame();
        const std::string key="##clipProperties"+t+c+laneId;
        ImGuiWindow* panel=nullptr;
        for(auto* window:ImGui::GetCurrentContext()->Windows)
            if(window->Active && std::string(window->Name).find(key)!=std::string::npos) panel=window;
        assert(panel);
        const ImVec2 point(panel->Pos.x+35,panel->Pos.y+36);
        frame(1250,point,false);
        io.AddMouseButtonEvent(1,true); frame(1250,point,false);
        io.AddMouseButtonEvent(1,false); frame(1250,point,false);
        frame();
        assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
        auto* popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        assert(popup);
        const ImVec2 item(popup->Pos.x+30,popup->Pos.y+ImGui::GetStyle().WindowPadding.y+
            ImGui::GetTextLineHeight()*0.5f+action*ImGui::GetTextLineHeightWithSpacing());
        frame(1250,item,false); frame(1250,item,true); frame(1250,item,false);
        frame();
    };
    laneAction(piano,0);
    assert(model.getClip(t,c)->lanes.size()==4);
    const auto duplicate=ui.editorState.editorLaneId;
    assert(duplicate!=piano && model.getLane(t,c,duplicate)->bindingIds==model.getLane(t,c,piano)->bindingIds);
    laneAction(duplicate,1);
    assert(model.getClip(t,c)->lanes.size()==3 && !model.getLane(t,c,duplicate));
    // Deliver an actual OF file-drag event, then let the production draw
    // import it. The WAV fixture contains one second of mono PCM silence.
    const auto samplePath=std::filesystem::temp_directory_path() /
        ("timeline-drop-"+ofToString(ofGetElapsedTimeMicros())+".WAV");
    {
        std::ofstream sample(samplePath,std::ios::binary);
        auto le=[&](uint32_t value,int bytes) {
            for(int i=0;i<bytes;++i) sample.put(static_cast<char>((value>>(i*8))&255));
        };
        sample.write("RIFF",4); le(16036,4); sample.write("WAVEfmt ",8);
        le(16,4); le(1,2); le(1,2); le(8000,4); le(16000,4); le(2,2); le(16,2);
        sample.write("data",4); le(16000,4);
        for(int i=0;i<16000;++i) sample.put(0);
    }
    ui.editorState.clipEditorOpen=false;
    ui.snapEnabled=true;
    model.getViewState().scrollX=80;
    frame();
    auto dropAt=[&](float x,float y,const std::string& path) {
        // A Finder drag can arrive while ImGui still has no mouse position
        // or hovered window. Only the OS drop coordinates identify its target.
        frame(1250,ImVec2(-FLT_MAX,-FLT_MAX));
        frame(1250,ImVec2(-FLT_MAX,-FLT_MAX));
        assert(ImGui::GetCurrentContext()->HoveredWindow==nullptr);
        ofDragInfo event; event.files={path};
        const auto origin=ImGui::GetMainViewport()->Pos;
        event.position=glm::vec2(x-origin.x,y-origin.y);
        ofNotifyEvent(ofEvents().fileDragEvent,event);
    };
    auto waveRow=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),
        [&](const auto& row){return row.trackId==wt;});
    assert(waveRow!=ui.clipDropRows.end());
    const float dx=ui.fileDropBounds.x+100, dy=(waveRow->top+waveRow->bottom)*0.5f;
    const auto originalClips=model.getTrack(wt)->clips.size();
    dropAt(dx,dy,samplePath.string());
    assert(ui.pendingAudioDrops.size()==1);
    const double expectedBeat=ui.pendingAudioDrops.front().beat;
    frame();
    assert(model.getTrack(wt)->clips.size()==originalClips+1);
    const auto imported=model.getTrack(wt)->clips.back();
    assert(imported.waveFilePath==samplePath.string() && imported.waveNumChannels==1);
    assert(imported.waveFileDurationMs==1000 && !imported.repeatContent);
    assert(std::abs(imported.startBeat-expectedBeat)<1e-6);
    assert(std::abs(imported.durationBeats-owner->getTransportState().bpm/60.0)<1e-6);
    assert(model.undoTimelineEdit()); assert(model.getTrack(wt)->clips.size()==originalClips);
    assert(model.redoTimelineEdit()); assert(model.getTrack(wt)->clips.size()==originalClips+1);
    const auto trackCount=model.getTracks().size();
    auto normalRow=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),
        [&](const auto& row){return row.trackId==t;});
    assert(normalRow!=ui.clipDropRows.end());
    const float normalY=(normalRow->top+normalRow->bottom)*0.5f;
    dropAt(dx,normalY,samplePath.string()); frame();
    assert(model.getTracks().size()==trackCount+1);
    assert(model.getTracks().back().isWaveTrack && model.getTracks().back().clips.size()==1);
    assert(model.undoTimelineEdit()); assert(model.getTracks().size()==trackCount);
    // Unsupported files, unreadable WAVs, and drops outside the canvas leave
    // no empty tracks or placeholder clips behind.
    dropAt(dx,normalY,"/tmp/not-audio.txt"); frame();
    dropAt(dx,normalY,"/tmp/missing-timeline-drop.wav"); frame();
    dropAt(ui.fileDropBounds.x-40,normalY,samplePath.string()); frame();
    assert(model.getTracks().size()==trackCount);
    assert(model.getTrack(wt)->clips.size()==originalClips+1);
    // A secondary platform viewport supplies desktop coordinates directly;
    // it never emits the main OF window's fileDragEvent.
    frame();
    ui.fileDropMainViewport=false;
    ui.queueAudioDrop({samplePath.string()},dx,normalY);
    assert(ui.pendingAudioDrops.size()==1);
    frame();
    assert(model.getTracks().size()==trackCount+1);
    assert(model.undoTimelineEdit());
    // Hover feedback uses the file's duration and the same snap/zoom mapping
    // as the import, while leaving the model and undo history untouched.
    frame();
    const auto hoverModel=model.toJson();
    const auto previewRow=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),
        [&](const auto& row){return row.trackId==wt;});
    assert(previewRow!=ui.clipDropRows.end());
    const float px=ui.fileDropBounds.x+100, py=(previewRow->top+previewRow->bottom)*0.5f;
    ui.updateAudioDropPreview({samplePath.string()},px,py);
    assert(ui.audioDropPreviewActive && ui.audioDropPreviewFiles.size()==1);
    assert(ui.audioDropPreviewFiles.front().durationSeconds==1.0);
    frame(1250,ImVec2(-FLT_MAX,-FLT_MAX),false,"audio-drop-preview");
    float previewLeft=FLT_MAX, previewRight=-FLT_MAX;
    for(const auto* list : ImGui::GetDrawData()->CmdLists)
        for(const auto& vertex : list->VtxBuffer)
            if(vertex.col==IM_COL32(100,210,255,240)) {
                previewLeft=std::min(previewLeft,vertex.pos.x);
                previewRight=std::max(previewRight,vertex.pos.x);
            }
    const double previewBeat=std::max(0.0,ui.snapBeat(ui.pixelsToBeat(model,
        px-ui.fileDropOriginX,ui.fileDropBpm,ui.fileDropEndBeat)));
    const float expectedLeft=ui.fileDropOriginX+ui.beatToPixels(model,previewBeat,ui.fileDropBpm);
    const float expectedRight=ui.fileDropOriginX+ui.beatToPixels(model,
        previewBeat+ui.fileDropBpm/60.0,ui.fileDropBpm);
    assert(std::abs(previewLeft-expectedLeft)<2 && std::abs(previewRight-expectedRight)<2);
    assert(model.toJson()==hoverModel && ui.pendingAudioDrops.empty());
    ui.updateAudioDropPreview({"/tmp/missing-timeline-preview.wav"},px,py);
    assert(!ui.audioDropPreviewActive && ui.audioDropPreviewFiles.empty());
    ui.updateAudioDropPreview({samplePath.string()},px,py);
    assert(ui.audioDropPreviewActive);
    ui.updateAudioDropPreview({},px,py); // cancelled/exited drag
    assert(!ui.audioDropPreviewActive);
    std::filesystem::remove(samplePath);
    // Duplicate track through its real context menu, with the mutation deferred
    // until after track drawing. Its new row is selected and the edit is undoable.
    frame();
    const auto trackBefore=model.toJson()["tracks"];
    const auto row=std::find_if(ui.clipDropRows.begin(),ui.clipDropRows.end(),[&](const auto& r){return r.trackId==t;});
    assert(row!=ui.clipDropRows.end());
    const ImVec2 pad(ui.fileDropBounds.x-120,(row->top+row->bottom)*0.5f);
    frame(1250,pad,false);io.AddMouseButtonEvent(1,true);frame(1250,pad,false);
    io.AddMouseButtonEvent(1,false);frame(1250,pad,false);frame();
    assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
    auto* trackMenu=ImGui::GetCurrentContext()->OpenPopupStack.back().Window;assert(trackMenu);
    const ImVec2 duplicatePoint(trackMenu->Pos.x+50,trackMenu->Pos.y+trackMenu->Size.y-
        ImGui::GetStyle().WindowPadding.y-ImGui::GetTextLineHeight()*0.5f-ImGui::GetTextLineHeightWithSpacing());
    frame(1250,duplicatePoint,false);frame(1250,duplicatePoint,true);frame(1250,duplicatePoint,false);
    assert(model.getTracks().size()==trackBefore.size()+1 && ui.selectedTracks.size()==1);
    const auto duplicatedTrack=*ui.selectedTracks.begin();assert(duplicatedTrack!=t);
    assert(model.getTrack(duplicatedTrack)->clips.size()==model.getTrack(t)->clips.size());
    assert(model.undoTimelineEdit() && model.toJson()["tracks"]==trackBefore);frame();
    model.beginTimelineEdit();ui.breakClipLanesRequest={t,c};frame();
    assert(!model.getClip(t,c) && ui.selectedTracks.size()==3);
    for(const auto& id:ui.selectedTracks) {
        const auto* track=model.getTrack(id);assert(track && track->clips.size()==1 && track->clips[0].lanes.size()==1);
    }
    assert(model.undoTimelineEdit() && model.toJson()["tracks"]==trackBefore);
    // Reproduce the reported restriction: bind to a second node parameter
    // that has never been published to this track or any other track.
    auto& otherNode=owner->createNode(std::make_unique<ClarityProbe>(),2);
    const auto newTarget=owner->getTimelineParameterPath(static_cast<ofxOceanodeAbstractParameter&>(otherNode.getParameters().get("Cutoff")));
    const auto paths=ui.patchTargetPaths();
    const auto pathIndex=std::find(paths.begin(),paths.end(),newTarget)-paths.begin();
    assert(pathIndex<paths.size());
    ui.editorState.editorTrackId=t;ui.editorState.editorClipId=c;ui.editorState.editorLaneId=curve;ui.editorState.clipEditorOpen=true;
    frame();
    for(int i=0;i<ImGui::GetCurrentContext()->TabBars.GetMapSize();++i) {
        auto* bar=ImGui::GetCurrentContext()->TabBars.TryGetMapData(i);if(!bar)continue;
        for(auto& tab:bar->Tabs)if(std::string(ImGui::TabBarGetTabName(bar,&tab))=="Targets")bar->NextSelectedTabId=tab.ID;
    }
    frame();frame();
    ImGuiWindow* targetPanel=nullptr;
    const std::string targetPanelKey="##clipProperties"+t+c+curve;
    for(auto* window:ImGui::GetCurrentContext()->Windows)
        if(window->Active && std::string(window->Name).find(targetPanelKey)!=std::string::npos)targetPanel=window;
    assert(targetPanel);
    const ImVec2 addTargetPoint(targetPanel->Pos.x+40,targetPanel->Pos.y+66);
    frame(1250,addTargetPoint,false);frame(1250,addTargetPoint,true);frame(1250,addTargetPoint,false);
    frame(1250,ImVec2(-100,-100),false,"patch-targets");
    assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
    auto* targetPopup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window;assert(targetPopup);
    const auto beforeTargets=model.toJson()["tracks"];
    const ImVec2 targetItem(targetPopup->Pos.x+40,targetPopup->Pos.y+targetPopup->WindowPadding.y+
        ImGui::GetFrameHeight()+ImGui::GetStyle().ItemSpacing.y+
        pathIndex*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*0.5f);
    frame(1250,targetItem,false);frame(1250,targetItem,true);frame(1250,targetItem,false);
    assert(model.getTrack(t)->bindings.size()==4);
    std::string newBinding;
    for(const auto& binding:model.getTrack(t)->bindings)if(binding.parameterPath==newTarget)newBinding=binding.id;
    assert(!newBinding.empty());
    const auto& routed=model.getLane(t,c,curve)->bindingIds;
    assert(std::find(routed.begin(),routed.end(),newBinding)!=routed.end());
    assert(model.undoTimelineEdit() && model.toJson()["tracks"]==beforeTargets);

    // Parameter menus add a lane to the selected clip, reusing a binding
    // if it already exists on the track, and focus the visible lane editor.
    auto& addedParameter=static_cast<ofxOceanodeAbstractParameter&>(otherNode.getParameters().get("Pitch"));
    ui.targetsView=false; ui.selectedClips={{t,c}};
    const auto laneCount=model.getClip(t,c)->lanes.size();
    const auto clipCount=model.getTrack(t)->clips.size();
    assert(model.requestParameterLane(t,addedParameter)); frame(); frame();
    assert(model.getClip(t,c)->lanes.size()==laneCount+1 && model.getTrack(t)->clips.size()==clipCount);
    const auto addedLane=ui.editorState.editorLaneId;
    assert(ui.editorState.clipEditorOpen && ui.editorState.editorClipId==c);
    assert(model.getLane(t,c,addedLane)->bindingIds.size()==1);
    assert(model.requestParameterLane(t,addedParameter)); frame();
    assert(model.getClip(t,c)->lanes.size()==laneCount+1 && ui.editorState.editorLaneId==addedLane);
    const auto second=model.createClip(t,"Second phrase",20,4);
    ui.selectedClips={{t,second}};
    assert(model.requestParameterLane(t,addedParameter)); frame();
    assert(model.getClip(t,second)->lanes.size()==1 && ui.editorState.editorClipId==second);
    assert(model.getClip(t,second)->lanes.front().bindingIds==model.getLane(t,c,addedLane)->bindingIds);
    // An empty track gets a visible clip/lane at the playhead. A wave track
    // rejects automation, and requests cannot survive a timeline clear.
    const auto emptyTrack=model.createTrack("Empty");
    assert(model.requestParameterLane(emptyTrack,addedParameter)); frame();
    assert(model.getTrack(emptyTrack)->clips.size()==1);
    assert(model.getTrack(emptyTrack)->clips.front().lanes.size()==1);
    assert(ui.editorState.editorTrackId==emptyTrack && ui.editorState.clipEditorOpen);
    assert(!model.requestParameterLane(wt,addedParameter));
    assert(!model.requestParameterLane("missing",addedParameter));
    const auto lfoOnly=model.createTrack("LFO only");
    const auto lb=model.addBinding(lfoOnly,addedParameter);
    const auto oldLfo=model.createLfoClip(lfoOnly,lb,"LFO",0,4);
    assert(model.requestParameterLane(lfoOnly,addedParameter)); frame();
    assert(model.getClip(lfoOnly,oldLfo)->isLfo && model.getTrack(lfoOnly)->clips.size()==2);
    assert(!model.getClip(lfoOnly,ui.editorState.editorClipId)->isLfo);

    // A note selection owns Delete before the arrangement can remove its clip,
    // even if the shell's last-click flag is stale and the mouse has moved away.
    const auto dt=model.createTrack("Delete test");
    const auto dc=model.createClip(dt,"Keep clip",0,4);
    const auto dl=model.createLane(dt,dc,"Notes",ofxOceanodeTimelineLaneType::PianoRoll);
    model.getLane(dt,dc,dl)->pianoNotes={{0,1,60,1,1},{1,1,64,1,1}};
    ui.editorState.editorTrackId=dt; ui.editorState.editorClipId=dc;
    ui.editorState.editorLaneId=dl; ui.editorState.clipEditorOpen=true;
    ui.selectedClips={{dt,dc}}; ui.lastClickInEditor=false;
    frame(); frame();
    ui.laneEditors.pianoRoll.pianoSelectionLaneId=dl;
    ui.laneEditors.pianoRoll.pianoSelectedNoteIndices={0};
    io.AddKeyEvent(ImGuiKey_Delete,true); frame();
    assert(model.getClip(dt,dc) && model.getLane(dt,dc,dl)->pianoNotes.size()==1);
    assert(model.getLane(dt,dc,dl)->pianoNotes.front().pitch==64);
    io.AddKeyEvent(ImGuiKey_Delete,false); frame();

    {
        // Grouped clips on one track move together.
        for(int i=0;i<25;++i) frame();
        model.groupClips({{t,c},{t,lc}});
        frame(); frame();
        auto rectOf=[&](const std::string& clipId) {
            const auto it=std::find_if(ui.clipScreenRects.begin(),ui.clipScreenRects.end(),[&](const auto& r){return r.clipId==clipId;});
            assert(it!=ui.clipScreenRects.end()); return *it;
        };
        const double introStart=model.getClip(t,c)->startBeat, lfoStart=model.getClip(t,lc)->startBeat;
        const auto rect=rectOf(c);
        const ImVec2 from((rect.x1+rect.x2)*0.5f,(rect.y1+rect.y2)*0.5f);
        const float beatPixels=(rect.x2-rect.x1)/static_cast<float>(model.getClip(t,c)->durationBeats);
        frame(1250,from,false); frame(1250,from,true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(from.x+beatPixels*i/6.0f,from.y),true);
        frame(1250,ImVec2(from.x+beatPixels,from.y),false); frame();
        assert(std::abs(model.getClip(t,c)->startBeat-(introStart+1.0))<1e-6);
        assert(std::abs(model.getClip(t,lc)->startBeat-(lfoStart+1.0))<1e-6);
        model.ungroupClip(t,c);
    }
    // Track folders: drag a header into a folder, fold it, drag it back out.
    for(int i=0;i<25;++i) frame(); // no double-click with the clicks above
    const auto folder=model.createFolder("Group");
    frame(); frame();
    auto headerRow=[&](const std::string& id) {
        const auto row=std::find_if(ui.trackHeaderRows.begin(),ui.trackHeaderRows.end(),[&](const auto& r){return r.trackId==id;});
        return row==ui.trackHeaderRows.end() ? nullptr : &*row;
    };
    assert(headerRow(folder) && headerRow(folder)->isFolder);
    auto dragHeader=[&](const std::string& id,ImVec2 to) {
        const auto* from=headerRow(id); assert(from);
        const ImVec2 start(ui.fileDropBounds.x-150,(from->minY+from->maxY)*0.5f);
        frame(1250,start,false); frame(1250,start,true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(start.x,start.y+(to.y-start.y)*i/6.0f),true);
        frame(1250,to,false); frame(); frame();
    };
    const auto* folderRow=headerRow(folder);
    dragHeader(dt,ImVec2(ui.fileDropBounds.x-150,(folderRow->minY+folderRow->maxY)*0.5f));
    assert(model.getTrack(dt)->parentId==folder);
    assert(headerRow(dt) && headerRow(dt)->depth==1);
    frame(1250,ImVec2(-100,-100),false,"track-folder");
    // Fold: the track inside is no longer drawn; a click on the summary unfolds.
    for(int i=0;i<25;++i) frame();
    folderRow=headerRow(folder);
    const ImVec2 chevron(8,(folderRow->minY+folderRow->maxY)*0.5f);
    frame(1250,chevron,false); frame(1250,chevron,true); frame(1250,chevron,false); frame();
    assert(model.getTrack(folder)->collapsed && headerRow(dt)==nullptr);
    for(int i=0;i<25;++i) frame();
    folderRow=headerRow(folder);
    const ImVec2 summary(ui.fileDropBounds.getRight()-40,(folderRow->minY+folderRow->maxY)*0.5f);
    frame(1250,summary,false); frame(1250,summary,true); frame(1250,summary,false); frame();
    assert(!model.getTrack(folder)->collapsed && headerRow(dt)!=nullptr);
    // Drag it above the first row: back at the top level, first.
    for(int i=0;i<25;++i) frame();
    dragHeader(dt,ImVec2(ui.fileDropBounds.x-150,ui.trackHeaderRows.front().minY+2.0f));
    assert(model.getTrack(dt)->parentId.empty() && model.getTracks().front().id==dt);
    // Never into itself: a folder dropped on its own row stays where it was.
    folderRow=headerRow(folder);
    dragHeader(folder,ImVec2(ui.fileDropBounds.x-150,(folderRow->minY+folderRow->maxY)*0.5f+20.0f));
    assert(model.getTrack(folder)->parentId.empty());

    {
        // Track heights: a normal track's bottom edge resizes every normal
        // track at once (saved with the view); a Wave track resizes alone.
        // The edge wins over the clip under it, which does not move.
        const bool wasTargets=ui.targetsView; ui.targetsView=false;
        for(int i=0;i<25;++i) frame();
        auto height=[&](const std::string& id){ const auto* r=headerRow(id); assert(r); return r->maxY-r->minY; };
        const float waveBefore=height(wt);
        assert(std::abs(height(t)-80.0f)<0.5f && std::abs(height(dt)-80.0f)<0.5f);
        const auto rect=std::find_if(ui.clipScreenRects.begin(),ui.clipScreenRects.end(),[&](const auto& r){return r.trackId==t && r.clipId==c;});
        assert(rect!=ui.clipScreenRects.end());
        const double startBefore=model.getClip(t,c)->startBeat;
        const ImVec2 edge((rect->x1+rect->x2)*0.5f,headerRow(t)->maxY-2.0f);
        frame(1250,edge,false); frame(1250,edge,true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(edge.x+i*10.0f,edge.y+40.0f*i/6.0f),true);
        frame(1250,ImVec2(edge.x+60.0f,edge.y+40.0f),false); frame(); frame();
        assert(std::abs(model.getViewState().trackHeight-120.0f)<1.0f);
        assert(std::abs(height(t)-120.0f)<1.0f && std::abs(height(dt)-120.0f)<1.0f); // all at once
        assert(std::abs(height(wt)-waveBefore)<0.5f);                               // the Wave track keeps its own
        assert(model.getClip(t,c)->startBeat==startBefore);                          // the clip stayed put
        frame(1250,ImVec2(-100,-100),false,"track-height");
        // The Wave track's edge resizes it alone (the others made small so it is in view).
        model.getViewState().trackHeight=kMinTrackHeight;
        for(int i=0;i<25;++i) frame();
        const ImVec2 waveEdge(ui.fileDropBounds.getRight()-40.0f,headerRow(wt)->maxY-2.0f);
        frame(1250,waveEdge,false); frame(1250,waveEdge,true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(waveEdge.x,waveEdge.y+30.0f*i/6.0f),true);
        frame(1250,ImVec2(waveEdge.x,waveEdge.y+30.0f),false); frame(); frame();
        assert(std::abs(height(wt)-(waveBefore+30.0f))<1.0f && std::abs(height(t)-kMinTrackHeight)<1.0f);
        // Both saved and read back.
        const auto saved=model.toJson();
        model.getViewState().trackHeight=80.0f; model.fromJson(saved);
        assert(std::abs(model.getViewState().trackHeight-kMinTrackHeight)<1.0f);
        assert(std::abs(model.getTrack(wt)->waveTrackHeight-(waveBefore+30.0f))<1.0f);
        model.getViewState().trackHeight=80.0f; model.getTrack(wt)->waveTrackHeight=waveBefore;
        ui.targetsView=wasTargets;
        for(int i=0;i<25;++i) frame();
    }

    {
        // A macro's own timeline: opened from its node menu, edited in its own
        // scope with its own history, saved with the macro.
        auto& macroNode=owner->createNode(std::make_unique<ofxOceanodeNodeMacro>(),1);
        auto inside=static_cast<ofxOceanodeNodeMacro&>(macroNode.getNodeModel()).getContainer();
        auto& innerProbe=inside->createNode(std::make_unique<ClarityProbe>(),1);
        auto& innerCutoff=static_cast<ofxOceanodeAbstractParameter&>(innerProbe.getParameters().get("Cutoff"));
        auto* pattern=model.getMacroSequence(*inside);
        assert(pattern && pattern->tracks.empty() && pattern->base==inside.get());
        const auto mainBefore=model.toJson()["tracks"];
        const auto pt=model.createTrack(*pattern,"Pattern");
        const auto pb=model.addBinding(*pattern,pt,innerCutoff);
        assert(!pb.empty() && pattern->tracks[0].bindings[0].parameterPath=="Synth_1/Cutoff"); // relative to the macro
        assert(model.addBinding(*pattern,pt,static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Cutoff"))).empty());
        const auto pc=model.createClip(*pattern,pt,"Phrase",0,4);
        const auto pl=model.createLane(*pattern,pt,pc,"Cutoff",ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(*pattern,pt,pc,pl,pb);
        assert(model.toJson()["tracks"]==mainBefore); // the main timeline is untouched

        // "Edit Timeline": the editor shows the macro's sequence.
        for(int i=0;i<25;++i) frame();
        model.requestMacroScope(*inside);
        frame(); frame();
        assert(ui.inMacroScope() && &ui.scope(model)==pattern && ui.scopes.back().label.size());
        const auto rect=std::find_if(ui.clipScreenRects.begin(),ui.clipScreenRects.end(),[&](const auto& r){return r.clipId==pc;});
        assert(rect!=ui.clipScreenRects.end() && rect->trackId==pt);
        assert(ui.clipScreenRects.size()==1); // only the macro's clip (its ids may repeat the main timeline's)
        frame(1250,ImVec2(-100,-100),false,"macro-scope");
        // A real drag moves the macro's clip; undo puts it back, in this scope only.
        const ImVec2 from((rect->x1+rect->x2)*0.5f,(rect->y1+rect->y2)*0.5f);
        const float beatPixels=(rect->x2-rect->x1)/4.0f;
        frame(1250,from,false); frame(1250,from,true);
        for(int i=1;i<=6;++i) frame(1250,ImVec2(from.x+beatPixels*2*i/6.0f,from.y),true);
        frame(1250,ImVec2(from.x+beatPixels*2,from.y),false); frame();
        assert(std::abs(model.getClip(*pattern,pt,pc)->startBeat-2.0)<1e-6);
        assert(model.toJson()["tracks"]==mainBefore);
        assert(ui.inMacroScope());
        ui.restoreHistory(model,false);
        assert(std::abs(model.getClip(*pattern,pt,pc)->startBeat)<1e-6);
        ui.restoreHistory(model,true);
        assert(std::abs(model.getClip(*pattern,pt,pc)->startBeat-2.0)<1e-6);
        assert(model.toJson()["tracks"]==mainBefore);

        // Saved with the macro, read back from its folder.
        const auto folder=std::filesystem::temp_directory_path()/"oceanode-macro-sequence";
        std::filesystem::create_directories(folder);
        assert(model.saveMacroSequence(*inside,folder.string()));
        assert(std::filesystem::exists(folder/"sequence.json"));
        // The core's own save and load of the macro's folder reach it too.
        std::filesystem::remove(folder/"sequence.json");
        assert(inside->savePreset(folder.string()) && std::filesystem::exists(folder/"sequence.json"));
        model.loadMacroSequence(*inside,folder.string());
        auto* reloaded=model.getMacroSequence(*inside);
        assert(reloaded && reloaded->tracks.size()==1 && reloaded->tracks[0].clips.size()==1);
        assert(std::abs(reloaded->tracks[0].clips[0].startBeat-2.0)<1e-6);
        assert(inside->loadPreset(folder.string()));
        reloaded=model.getMacroSequence(*inside);
        assert(reloaded && reloaded->tracks.size()==1 && std::abs(reloaded->tracks[0].clips[0].startBeat-2.0)<1e-6);
        frame(); frame(); // the old scope's sequence was replaced: back to the main timeline
        assert(!ui.inMacroScope());
        // An emptied timeline leaves no file behind.
        model.removeTrack(*reloaded,reloaded->tracks[0].id);
        assert(model.saveMacroSequence(*inside,folder.string()) && !std::filesystem::exists(folder/"sequence.json"));
        std::filesystem::remove_all(folder);

        // Back to Main from the breadcrumb, and closed when the macro goes away.
        model.requestMacroScope(*inside); frame(); assert(ui.inMacroScope());
        ui.leaveScopesTo(0); frame(); assert(!ui.inMacroScope());
        {
            // A macro track on the main timeline: its clips play the macro's timeline.
            const auto mt=model.createMacroTrack(*inside);
            assert(!mt.empty() && model.getTrack(mt)->macroCanvas==inside->getCanvasID());
            const auto mc=model.createMacroClip(mt,0.0);
            assert(!mc.empty() && model.getClip(mt,mc)->contentDurationBeats>=model.getBeatsPerBar());
            assert(model.createLane(mt,mc,"x",ofxOceanodeTimelineLaneType::Curve).empty()); // no lanes in a macro clip
            for(int i=0;i<25;++i) frame();
            const auto mrect=std::find_if(ui.clipScreenRects.begin(),ui.clipScreenRects.end(),[&](const auto& r){return r.clipId==mc && r.trackId==mt;});
            assert(mrect!=ui.clipScreenRects.end());
            const ImVec2 at((mrect->x1+mrect->x2)*0.5f,(mrect->y1+mrect->y2)*0.5f);
            frame(1250,at,false); frame(1250,at,true); frame(1250,at,false); frame(1250,at,true); frame(1250,at,false);
            frame(); frame();
            assert(ui.inMacroScope() && &ui.scope(model)==model.getMacroSequence(*inside)); // double-click opens it
            {
                // The main playhead shows where the macro clip has the macro's timeline.
                double macroBeat=-1;
                owner->getTransport()->seekToBeat(1.5);
                assert(model.macroPlayheadBeat(*inside,macroBeat) && std::abs(macroBeat-1.5)<1e-6);
                frame(); frame(1250,ImVec2(-100,-100),false,"macro-scope-playhead");
                owner->getTransport()->seekToBeat(0.0);
            }
            ui.leaveScopesTo(0); frame();
            // Saved and read back; a renumbered macro carries its tracks along.
            const auto saved=model.toJson();
            model.fromJson(saved);
            assert(model.getTrack(mt)->macroCanvas==inside->getCanvasID() && model.getClip(mt,mc));
            model.retargetParameterPaths({{inside->getCanvasID()+"::Synth_1/Cutoff","Macro 9::Synth_1/Cutoff"}});
            assert(model.getTrack(mt)->macroCanvas=="Macro 9");
            model.removeTrack(mt);
        }
        model.requestMacroScope(*inside); frame(); assert(ui.inMacroScope());
        inside.reset();
        macroNode.deleteSelf();
        frame(); frame();
        assert(!ui.inMacroScope());
    }

    assert(model.requestParameterLane(t,addedParameter)); model.clear();
    assert(model.consumePendingParameterLanes().empty());
    ImGui::DestroyContext();
    std::cout<<"Controller clarity passed: both views, mixed clip, focus, folded track, narrow layout, LFO, audio, read-only inspector, track selection and outlines, track folders and header drag-and-drop, track heights (all at once, Wave alone), macro timeline scope (undo, save, breadcrumb), macro tracks, portable clip import/cancel/undo, mouse timing tools, Cmd+drag reverse, cross-track groups, clip menus and deferred lane routing.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
