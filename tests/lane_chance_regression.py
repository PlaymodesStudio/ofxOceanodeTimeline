#!/usr/bin/env python3
"""Lane chance: parent multiplication, stable rolls, rests, shared piano and saves."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    // Independent parent/child rolls give the product, and stay fixed within a start.
    ofxOceanodeTimelineClip parent; parent.id="parent"; parent.chance=0.5f;
    ofxOceanodeTimelineLane a,b; a.id="lane-a"; b.id="lane-b"; a.chance=b.chance=0.5f;
    int parentPlays=0, aPlays=0, bPlays=0, split=0;
    for(uint64_t generation=0; generation<10000; ++generation) {
        ofxOceanodeTimelinePlayContext context{generation, 3, 12345, 91};
        const bool p=!ofxOceanodeTimelineClipChanceSilenced(parent,context);
        const bool x=!ofxOceanodeTimelineLaneChanceSilenced(a,context);
        const bool y=!ofxOceanodeTimelineLaneChanceSilenced(b,context);
        parentPlays+=p; aPlays+=p&&x; bPlays+=p&&y; split+=p&&(x!=y);
        assert(x==!ofxOceanodeTimelineLaneChanceSilenced(a,context));
    }
    assert(parentPlays>4500 && parentPlays<5500);
    assert(aPlays>2100 && aPlays<2900 && bPlays>2100 && bPlays<2900 && split>1000);
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> output("Output",0,0,1), siblingOutput("Sibling",0,0,1);
    ofxOceanodeParameter<float> target,siblingTarget;
    target.bindParameter(output); siblingTarget.bindParameter(siblingOutput);
    owner.targets[target.getEscapedName()]=&target; owner.targets[siblingTarget.getEscapedName()]=&siblingTarget;
    TestTimeline model(&owner); model.setSchedulingEnabled(false);
    using Type=ofxOceanodeTimelineLaneType;
    for(Type type : {Type::Step,Type::MultiGate,Type::MultiValue,Type::Curve,Type::Number,Type::Dropdown,Type::MultiSlider}) {
        const auto t=model.createTrack("Lane chance"), bind=model.addBinding(t,target), otherBind=model.addBinding(t,siblingTarget);
        const auto c=model.createClip(t,"Parent",0,4), l=model.createLane(t,c,"Lane",type);
        const auto other=model.createLane(t,c,"Always",Type::Number);
        model.addBindingToLane(t,c,l,bind); model.addBindingToLane(t,c,other,otherBind);
        auto& lane=*model.getLane(t,c,l);
        lane.stepCount=4; lane.beatsPerStep=1; lane.step.steps={{0,4,"",1,1}};
        lane.curvePoints={{0,0.6f},{4,0.6f}}; lane.multiSliderValues.assign(4,0.6f);
        lane.multiRowCount=1; lane.multiGateRows.assign(1,{{0,4,1}}); lane.multiValueRows.assign(1,{{0,4,0.6f}});
        model.setClipNumber(t,c,other,0,0.8f);
        assert(lane.chance==1);
        owner.transport->seekToBeat(1); owner.transport->play(); model.update(); model.applyAutomation();
        const float playing=output;
        assert(model.setLaneChance(t,c,l,0)); model.update(); model.applyAutomation();
        const bool rests=type==Type::Step || type==Type::MultiGate || type==Type::MultiValue;
        near(output,rests?0:playing); near(siblingOutput,0.8f);
        assert(model.inspectTarget(target.getEscapedName(),1,true).writesValue==rests);
        assert(!model.setLaneChance(t,c,l,-1));
        assert(!model.setLaneChance(t,c,l,std::numeric_limits<float>::quiet_NaN()));
        assert(model.setLaneChance(t,c,l,2)); near(lane.chance,1);
        // Both a clip failure and a lane failure must hold continuous endpoints.
        model.setLaneChance(t,c,l,0); output=0.9f;
        owner.transport->advanceFrameStep(2); model.update(); model.applyAutomation();
        if(!rests) near(output,0.9f);
        model.setLaneChance(t,c,l,0.3f);
        const auto saved=model.toJson(); model.fromJson(saved); near(model.getLane(t,c,l)->chance,0.3f);
        auto legacy=saved;
        for(auto& tr:legacy["tracks"]) for(auto& cl:tr["clips"]) for(auto& ln:cl["lanes"]) ln.erase("chance");
        model.fromJson(legacy); near(model.getLane(t,c,l)->chance,1);
        model.beginTimelineEdit(); model.setLaneChance(t,c,l,0.25f); model.endTimelineEdit();
        assert(model.undoTimelineEdit()); near(model.getLane(t,c,l)->chance,1);
        assert(model.redoTimelineEdit()); near(model.getLane(t,c,l)->chance,0.25f);
        model.removeTrack(t);
    }
    // Parts sharing one instrument roll independently, including identical notes.
    ofParameter<float> pitchValue("Pitch",0,0,127), gateValue("Gate",0,0,1);
    ofxOceanodeParameter<float> pitch,gate; pitch.bindParameter(pitchValue); gate.bindParameter(gateValue);
    owner.targets[pitch.getEscapedName()]=&pitch; owner.targets[gate.getEscapedName()]=&gate;
    const auto t=model.createTrack("Piano"), pb=model.addBinding(t,pitch), gb=model.addBinding(t,gate);
    const auto group=model.createNoteGroup(t,"Shared",pb,gb,"");
    std::string l; const auto c=model.createNoteGroupClip(t,group,"Phrase",0,4,&l);
    const auto other=model.createLane(t,c,"Other",Type::PianoRoll);
    model.addBindingToLane(t,c,other,pb); model.addBindingToLane(t,c,other,gb);
    for(const auto& id:{l,other}) model.getLane(t,c,id)->pianoNotes={{0,4,60,0.8f,1}};
    model.setLaneChance(t,c,l,0);
    owner.transport->seekToBeat(1); model.update(); model.applyAutomation(); near(gateValue,1); near(pitchValue,60);
    model.setLaneChance(t,c,other,0); model.update(); model.applyAutomation(); near(gateValue,0);
    model.setLaneChance(t,c,l,1); model.update(); model.applyAutomation(); near(gateValue,1);
    model.setClipChance(t,c,0); model.update(); model.applyAutomation(); near(gateValue,0);
    // LFO control lanes fall back to the oscillator default when skipped.
    ofxOceanodeTimelineClip lfo;
    ofxOceanodeTimelineLane scale; scale.id="scale"; scale.lfoParameter="scale";
    scale.curveInterpolation="Value"; scale.lfoValue=0; scale.chance=0;
    lfo.lanes.push_back(scale);
    ofxOceanodeTimelinePlayContext context{};
    near(ofxOceanodeTimelineLfo::evaluateParameter(lfo,"scale",1,1,&context),1);
    near(ofxOceanodeTimelineLfo::evaluateParameter(lfo,"scale",1,1),0);
    // LFO Track node samples use the playing timeline's rolls too.
    const auto lt=model.createTrack("LFO chance"), lb=model.addBinding(lt,target);
    const auto lc=model.createLfoClip(lt,lb,"LFO",0,4);
    std::string scaleId;
    for(auto& lane:model.getClip(lt,lc)->lanes) if(lane.lfoParameter=="scale") {
        scaleId=lane.id; lane.curveInterpolation="Value"; lane.lfoValue=0;
    }
    assert(!scaleId.empty()); model.setLaneChance(lt,lc,scaleId,0.5f);
    int silent=0;
    for(int start=0;start<200;++start) {
        owner.transport->seekToBeat(1); model.update(); model.applyAutomation();
        const auto sample=model.evaluateLfoTrack(lt,1);
        assert(sample.active); near(sample.value,output);
        silent+=sample.value==0;
    }
    assert(silent>60 && silent<140);
    model.removeTrack(lt);
    // Lookahead uses the same lane roll and corrects an already queued gate.
    model.removeTrack(t);
    owner.transport->pause(); model.update();
    owner.transport->setDriverMode(TransportDriverMode::External);
    const auto st=model.createTrack("Scheduled"), sp=model.addBinding(st,pitch), sg=model.addBinding(st,gate);
    const auto group2=model.createNoteGroup(st,"Notes",sp,sg,"");
    std::string sl; const auto sc=model.createNoteGroupClip(st,group2,"Scheduled",0,4,&sl);
    model.getLane(st,sc,sl)->pianoNotes={{0.25,0.5,60,0.8f,1}};
    std::vector<ofxOceanodeScheduledParameterEvent> sent;
    ofxOceanodeScheduling::registerParameterTarget(&gate,&model,[&](const auto& event){sent.push_back(event); return true;});
    model.setSchedulingEnabled(true); model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(500);
    model.setLaneChance(st,sc,sl,0);
    owner.transport->seekToBeat(0); owner.transport->play(); model.update(); model.update();
    for(const auto& event:sent) assert(event.value!="1");
    sent.clear(); model.setLaneChance(st,sc,sl,1); model.update();
    assert(std::any_of(sent.begin(),sent.end(),[](const auto& event){return event.value=="1";}));
    sent.clear(); model.setLaneChance(st,sc,sl,0); model.update();
    assert(std::any_of(sent.begin(),sent.end(),[](const auto& event){return event.isCorrection && event.value=="0";}));
    ofxOceanodeScheduling::unregisterOwner(&model);
    std::cout<<"Lane chance passed: product, independent stable rolls, all value lane types, rests/holds, endpoints, siblings, shared piano, LFO defaults, persistence, undo and schedule correction.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
