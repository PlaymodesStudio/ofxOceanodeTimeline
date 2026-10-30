#!/usr/bin/env python3
"""The timeline picker: every timeline, nested as the macros are.

Drives the production controller with native core and headless ImGui. The
breadcrumb is always shown; its last level opens a tree of Main and every
macro's timeline with whether a macro track reaches it from Main; picking a
nested macro opens it with the macros around it in the breadcrumb; and a
macro timeline nothing plays offers to play it from the timeline it is in.
"""
import attachment_regression as native

native.fixture.CASES = r'''
#include "ofxOceanodeTimeline.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#define private public
#include "Controls/ofxOceanodeTimelineController.h"
#undef private
#include "imgui_internal.h"
class PickerProbe : public ofxOceanodeNodeModel {
public:
    PickerProbe() : ofxOceanodeNodeModel("Probe") {}
    ofParameter<float> level;
    void setup() override { addParameter(level.set("Level", 0, 0, 1)); }
};
int main() {
    ofDisableDataPath(); ofSeedRandom();
    auto window=std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry=std::make_shared<ofxOceanodeNodeRegistry>();
    auto types=std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner=std::make_shared<ofxOceanodeContainer>(registry,types);
    owner->setCanvasID("Canvas");
    auto& model=ofxOceanodeTimeline::attach(*owner);
    model.setSchedulingEnabled(false);
    ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,1000); io.DeltaTime=1.f/60;
    unsigned char* atlas; int aw,ah; io.Fonts->GetTexDataAsRGBA32(&atlas,&aw,&ah);
    ofxOceanodeTimelineController ui(owner);
    auto frame=[&](ImVec2 mouse=ImVec2(-100,-100),bool down=false) {
        io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(0,down);
        ImGui::NewFrame();
        ui.drawBeforeWindows();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1250,980));
        ImGui::Begin("Timeline",nullptr,ImGuiWindowFlags_NoDecoration);
        ui.draw(); ImGui::End(); ImGui::Render();
    };
    auto click=[&](ImVec2 at) { frame(at,false); frame(at,true); frame(at,false); frame(); };
    auto centre=[](ImVec2 a,ImVec2 b) { return ImVec2((a.x+b.x)*0.5f,(a.y+b.y)*0.5f); };

    // No macros: the breadcrumb is there, and its list holds Main alone.
    frame(); frame();
    assert(ui.timelinePickerRect.max.x>ui.timelinePickerRect.min.x);
    assert(model.listMacroTimelines().empty());
    click(centre(ui.timelinePickerRect.min,ui.timelinePickerRect.max));
    assert(ui.timelinePickerRows.size()==1 && ui.timelinePickerRows[0].container==nullptr);
    frame(ImVec2(1200,960),false); frame(ImVec2(1200,960),true); frame(ImVec2(1200,960),false); frame();
    assert(ui.timelinePickerRows.empty()); // closed by a click outside

    // Macro A (with Macro B inside it) and Macro C, all on the main canvas.
    auto& aNode=owner->createNode(std::make_unique<ofxOceanodeNodeMacro>(),1);
    auto a=static_cast<ofxOceanodeNodeMacro&>(aNode.getNodeModel()).getContainer();
    auto& aProbe=a->createNode(std::make_unique<PickerProbe>(),1);
    auto& bNode=a->createNode(std::make_unique<ofxOceanodeNodeMacro>(),1);
    auto b=static_cast<ofxOceanodeNodeMacro&>(bNode.getNodeModel()).getContainer();
    auto& bProbe=b->createNode(std::make_unique<PickerProbe>(),1);
    auto& cNode=owner->createNode(std::make_unique<ofxOceanodeNodeMacro>(),2);
    auto c=static_cast<ofxOceanodeNodeMacro&>(cNode.getNodeModel()).getContainer();
    auto level=[](ofxOceanodeNode& node) -> ofxOceanodeAbstractParameter& {
        return static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Level"));
    };

    auto macros=model.listMacroTimelines();
    auto find=[&](ofxOceanodeContainer* macro) {
        return std::find_if(macros.begin(),macros.end(),[&](const auto& entry){return entry.container==macro;});
    };
    assert(macros.size()==3);
    assert(find(a.get())->depth==0 && find(a.get())->parent==nullptr);
    assert(find(b.get())->depth==1 && find(b.get())->parent==a.get());
    assert(find(b.get())==find(a.get())+1); // a macro's macros right after it
    assert(find(c.get())->depth==0);
    // Each level by name.
    const auto& first=macros[0].container==a.get() ? *find(a.get()) : *find(c.get());
    const auto& second=macros[0].container==a.get() ? *find(c.get()) : *find(a.get());
    assert(first.label<=second.label);
    for(const auto& entry:macros) assert(entry.trackCount==0 && !entry.played && !entry.shared && !entry.label.empty());

    // "Add to Macro A Timeline Track": A's timeline gets a track, nothing plays it.
    auto* aSequence=model.getMacroSequence(*a);
    const auto at=model.createTrack(*aSequence,"Level");
    assert(!model.addBinding(*aSequence,at,level(aProbe)).empty());
    macros=model.listMacroTimelines();
    assert(find(a.get())->trackCount==1 && !find(a.get())->played && !model.isMacroTimelinePlayed(*a));

    // The picker lists Main and the three, and opens A.
    frame();
    click(centre(ui.timelinePickerRect.min,ui.timelinePickerRect.max));
    assert(ui.timelinePickerRows.size()==4 && ui.timelinePickerRows[0].container==nullptr);
    for(size_t i=0;i<macros.size();++i) assert(ui.timelinePickerRows[i+1].container==macros[i].container); // as listed
    auto row=[&](ofxOceanodeContainer* macro) {
        const auto found=std::find_if(ui.timelinePickerRows.begin(),ui.timelinePickerRows.end(),[&](const auto& r){return r.container==macro;});
        assert(found!=ui.timelinePickerRows.end());
        return centre(found->min,found->max);
    };
    assert(row(b.get()).x>row(a.get()).x); // tabbed in under A
    click(row(a.get()));
    assert(ui.inMacroScope() && ui.scopes.size()==1 && ui.scopes[0].container==a.get());
    assert(ui.timelinePickerRows.empty());

    // A is not played: "Play it from Main" puts a macro track for it on Main, and Main shows.
    frame();
    assert(ui.timelinePlayRect.max.x>ui.timelinePlayRect.min.x);
    const auto mainTracks=model.getMainSequence().tracks.size();
    click(centre(ui.timelinePlayRect.min,ui.timelinePlayRect.max));
    assert(!ui.inMacroScope());
    assert(model.getMainSequence().tracks.size()==mainTracks+1);
    const auto& macroTrack=model.getMainSequence().tracks.back();
    assert(macroTrack.isMacroTrack() && macroTrack.clips.size()==1 &&
           model.findMacroContainer(model.getMainSequence(),macroTrack)==a.get());
    assert(model.isMacroTimelinePlayed(*a));
    // One undo step on the main timeline takes it back.
    ui.restoreHistory(model,false);
    assert(model.getMainSequence().tracks.size()==mainTracks && !model.isMacroTimelinePlayed(*a));
    ui.restoreHistory(model,true);
    assert(model.isMacroTimelinePlayed(*a));

    // Played: no offer in A's breadcrumb.
    model.requestMacroScope(*a); frame(); frame();
    assert(ui.inMacroScope() && ui.timelinePlayRect.max.x==ui.timelinePlayRect.min.x);

    // B (inside A) from its node menu, with a track: the breadcrumb is Main > A > B.
    auto* bSequence=model.getMacroSequence(*b);
    const auto bt=model.createTrack(*bSequence,"Level");
    assert(!model.addBinding(*bSequence,bt,level(bProbe)).empty());
    model.requestMacroScope(*b); frame(); frame();
    assert(ui.scopes.size()==2 && ui.scopes[0].container==a.get() && ui.scopes[1].container==b.get());
    // The same again keeps it as it is.
    ui.selectedTracks.insert(bt);
    model.requestMacroScope(*b); frame();
    assert(ui.scopes.size()==2 && ui.selectedTracks.count(bt)==1);
    // A's crumb goes back to A.
    ui.leaveScopesTo(1); frame();
    assert(ui.scopes.size()==1 && ui.scopes[0].container==a.get());
    // C from the picker, from inside A: Main > C.
    click(centre(ui.timelinePickerRect.min,ui.timelinePickerRect.max));
    assert(ui.timelinePickerRows.size()==4);
    click(row(c.get()));
    assert(ui.scopes.size()==1 && ui.scopes[0].container==c.get());
    // And B from the picker: Main > A > B.
    click(centre(ui.timelinePickerRect.min,ui.timelinePickerRect.max));
    click(row(b.get()));
    assert(ui.scopes.size()==2 && ui.scopes[0].container==a.get() && ui.scopes[1].container==b.get());

    // B is not played: its offer plays it from A (the macro it is in), and A shows.
    assert(!model.isMacroTimelinePlayed(*b));
    frame();
    assert(ui.timelinePlayRect.max.x>ui.timelinePlayRect.min.x);
    click(centre(ui.timelinePlayRect.min,ui.timelinePlayRect.max));
    assert(ui.scopes.size()==1 && ui.scopes[0].container==a.get());
    assert(aSequence->tracks.size()==2 && aSequence->tracks.back().isMacroTrack() &&
           model.findMacroContainer(*aSequence,aSequence->tracks.back())==b.get());
    assert(model.isMacroTimelinePlayed(*b)); // through A's macro track on Main
    assert(model.getMainSequence().tracks.size()==mainTracks+1); // Main untouched
    // Undone in A's own history.
    ui.restoreHistory(model,false);
    assert(aSequence->tracks.size()==1 && !model.isMacroTimelinePlayed(*b));
    ui.restoreHistory(model,true);
    assert(model.isMacroTimelinePlayed(*b));

    // Without A's macro track on Main, B is placed in A but A is not played: no offer.
    ui.leaveScopesTo(0); frame();
    const auto mainMacroTrack=model.getMainSequence().tracks.back().id;
    model.removeTrack(mainMacroTrack);
    assert(!model.isMacroTimelinePlayed(*a) && !model.isMacroTimelinePlayed(*b));
    model.requestMacroScope(*b); frame(); frame();
    assert(ui.scopes.size()==2 && ui.timelinePlayRect.max.x==ui.timelinePlayRect.min.x);

    // A macro removed: gone from the list, its scope closed.
    ui.leaveScopesTo(0); frame();
    c.reset(); cNode.deleteSelf(); frame();
    macros=model.listMacroTimelines();
    assert(macros.size()==2);

    ImGui::DestroyContext();
    std::cout<<"Timeline picker passed: always shown, Main alone, tree order and depth, played and track counts, open from the tree, breadcrumb follows nesting, same scope kept, play it from Main (undo), play a nested one from its macro (undo in its history), placed but parent silent, macro removed.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
