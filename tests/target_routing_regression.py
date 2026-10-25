#!/usr/bin/env python3
"""Patch targets can be published on demand without retargeting other clips."""
import timeline_regression as fixture
CASES=r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections(){assert(!hasInConnection()&&!hasOutConnections()&&!isScoped());}
int main(){
    ofSeedRandom();ofxOceanodeContainer owner;
    ofParameter<float> a("Original",0,0,1),b("Other node",0,0,1),d("Macro / Nested",0,-10,10);
    ofxOceanodeParameter<float> pa,pb,pd;pa.bindParameter(a);pb.bindParameter(b);pd.bindParameter(d);
    for(auto* p:{&pa,&pb,&pd})owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner);
    const auto t=model.createTrack("Track"),bound=model.addBinding(t,pa),c=model.createClip(t,"Clip",0,4);
    const auto l=model.createLane(t,c,"Curve",ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t,c,l,bound);
    const auto sibling=model.duplicateClip(t,c,t,8);
    using Role=ofxOceanodeTimelineTargetRole;
    const auto before=model.toJson();model.beginTimelineEdit();
    assert(model.bindClipTarget(t,c,l,pb.getEscapedName()));model.endTimelineEdit();
    assert(model.getTrack(t)->bindings.size()==2 && model.getLane(t,c,l)->bindingIds.size()==2);
    assert(model.getClip(t,sibling)->lanes[0].bindingIds==std::vector<std::string>{bound});
    assert(pb.isTimelined());assert(model.undoTimelineEdit() && model.toJson()==before && !pb.isTimelined());
    assert(model.redoTimelineEdit() && pb.isTimelined());
    assert(model.bindClipTarget(t,c,l,pb.getEscapedName()));assert(model.getTrack(t)->bindings.size()==2);
    assert(model.getLane(t,c,l)->bindingIds.size()==2); // reuse, no duplicate routes
    const auto piano=model.createLane(t,c,"Piano",ofxOceanodeTimelineLaneType::PianoRoll);
    assert(model.bindClipTarget(t,c,piano,pb.getEscapedName(),Role::Gate));
    auto* notes=model.getLane(t,c,piano);assert(notes->pianoPitchBindingId.empty()&&!notes->pianoGateBindingId.empty());
    assert(model.bindClipTarget(t,c,piano,pa.getEscapedName(),Role::Pitch));
    const auto oldPitch=model.getLane(t,c,piano)->pianoPitchBindingId;
    assert(model.bindClipTarget(t,c,piano,pd.getEscapedName(),Role::Pitch));
    notes=model.getLane(t,c,piano);assert(notes->pianoPitchBindingId!=oldPitch);
    assert(std::find(notes->bindingIds.begin(),notes->bindingIds.end(),oldPitch)==notes->bindingIds.end());
    assert(model.getLane(t,c,l)->bindingIds[0]==bound); // other lane retains original
    const auto lc=model.createLfoClip(t,bound,"LFO",0,4);
    assert(model.bindClipTarget(t,lc,"",pd.getEscapedName(),Role::LfoOutput));
    const auto* oscillator=model.getClip(t,lc);assert(oscillator->lfoOutputMin==-10&&oscillator->lfoOutputMax==10);
    assert(model.getBinding(t,oscillator->lfoOutputBindingId)->parameterPath==pd.getEscapedName());
    const auto stable=model.toJson();
    assert(!model.bindClipTarget(t,c,l,"missing"));
    assert(!model.bindClipTarget(t,c,l,pd.getEscapedName(),Role::Pitch));
    assert(!model.bindClipTarget(t,c,l,pd.getEscapedName(),Role::LfoOutput));
    assert(model.toJson()==stable);
    std::cout<<"Target routing passed: unpublished parameters, shared binding reuse, piano roles, LFO outputs, sibling isolation and undo.\n";
}
'''
if __name__=='__main__':fixture.main(cases=CASES)
