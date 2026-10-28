#!/usr/bin/env python3
"""Verify clip chance: whole-clip probability, re-rolled each time the clip starts."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> controlValue("Control",0,0,1);
    ofxOceanodeParameter<float> control; control.bindParameter(controlValue);
    owner.targets[control.getEscapedName()]=&control;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Control");
    const auto b=model.addBinding(t,control);
    const auto c=model.createClip(t,"Value",0,4);
    const auto l=model.createLane(t,c,"Value",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(t,c,l,b); model.setClipNumber(t,c,l,0,0.25f);
    owner.transport->seekToBeat(1); owner.transport->play();
    // New clips always play.
    assert(model.getClip(t,c)->chance==1.0f);
    model.update(); model.applyAutomation(); near(controlValue,0.25);
    // 0%: the clip never plays, its content is kept.
    assert(model.setClipChance(t,c,0.0f));
    controlValue=0.9f; model.update(); model.applyAutomation(); near(controlValue,0.9);
    assert(!model.inspectTarget(control.getEscapedName(),1,true).writesValue);
    assert(!model.setClipChance(t,c,-1.0f)); // clamped to 0, unchanged
    // 50%: one roll per start (each seek is a new start), stable while playing through.
    model.setClipChance(t,c,0.5f);
    int played=0;
    for(int start=0;start<200;++start) {
        owner.transport->seekToBeat(1);
        controlValue=0.9f; model.update(); model.applyAutomation();
        const bool plays=std::abs(controlValue.get()-0.25f)<1e-4f;
        for(int i=0;i<3;++i) {
            controlValue=0.9f; model.update(); model.applyAutomation();
            assert((std::abs(controlValue.get()-0.25f)<1e-4f)==plays);
        }
        played+=plays;
    }
    assert(played>60 && played<140);
    // A clip its roll leaves out plays as an empty pass: lanes that rest at 0
    // between events (toggles, steps, value regions, piano gates) write that
    // rest instead of holding what the previous pass ended on; lanes with no
    // rest (curves, numbers, sliders) hold, as between clips.
    ofParameter<float> laneValue("Lane",0,0,1), pianoPitch("Pitch",0,0,127), pianoGate("PianoGate",0,0,1);
    ofxOceanodeParameter<float> laneTarget, pitchTarget, gateTarget;
    laneTarget.bindParameter(laneValue); pitchTarget.bindParameter(pianoPitch); gateTarget.bindParameter(pianoGate);
    owner.targets[laneTarget.getEscapedName()]=&laneTarget;
    owner.targets[pitchTarget.getEscapedName()]=&pitchTarget;
    owner.targets[gateTarget.getEscapedName()]=&gateTarget;
    struct LaneCase { ofxOceanodeTimelineLaneType type; float playing; bool rests; };
    for(const auto& lc : std::vector<LaneCase>{
            {ofxOceanodeTimelineLaneType::MultiGate,1.0f,true},
            {ofxOceanodeTimelineLaneType::Step,1.0f,true},
            {ofxOceanodeTimelineLaneType::MultiValue,0.6f,true},
            {ofxOceanodeTimelineLaneType::Number,0.6f,false},
            {ofxOceanodeTimelineLaneType::Curve,0.6f,false},
            {ofxOceanodeTimelineLaneType::MultiSlider,0.6f,false}}) {
        const auto lt=model.createTrack("Lane");
        const auto lb=model.addBinding(lt,laneTarget);
        const auto lclip=model.createClip(lt,"Lane",8,4);
        const auto ll=model.createLane(lt,lclip,"Lane",lc.type);
        model.addBindingToLane(lt,lclip,ll,lb);
        auto* lane=model.getLane(lt,lclip,ll);
        lane->multiRowCount=1; lane->multiGateRows.assign(1,{{0.0,4.0}}); lane->multiValueRows.assign(1,{{0.0,4.0,0.6f}});
        lane->stepCount=4; lane->beatsPerStep=1; lane->step.steps={{3.0,1.0,"",1.0f,1.0f}};
        lane->curvePoints={{0,0.6f},{4,0.6f}}; lane->multiSliderValues.assign(4,0.6f);
        if(lc.type==ofxOceanodeTimelineLaneType::Number) model.setClipNumber(lt,lclip,ll,0,0.6f);
        owner.transport->seekToBeat(11);
        model.update(); model.applyAutomation(); near(laneValue,lc.playing);
        model.setClipChance(lt,lclip,0.0f);
        model.update(); model.applyAutomation(); near(laneValue,lc.rests?0.0f:lc.playing);
        assert(model.inspectTarget(laneTarget.getEscapedName(),11,true).writesValue==lc.rests);
        // Past the clip the target is left alone, as for a clip that played.
        laneValue=0.7f; owner.transport->seekToBeat(13);
        model.update(); model.applyAutomation(); near(laneValue,0.7);
        model.removeTrack(lt);
    }
    {
        const auto pt=model.createTrack("Piano");
        const auto pb=model.addBinding(pt,pitchTarget), pg=model.addBinding(pt,gateTarget);
        const auto group=model.createNoteGroup(pt,"Notes",pb,pg,"");
        std::string pl;
        const auto pc=model.createNoteGroupClip(pt,group,"Phrase",8,4,&pl);
        model.getLane(pt,pc,pl)->pianoNotes={{0,4,60,0.8f,1}};
        owner.transport->seekToBeat(11);
        model.update(); model.applyAutomation(); near(pianoGate,1); near(pianoPitch,60);
        model.setClipChance(pt,pc,0.0f);
        model.update(); model.applyAutomation(); near(pianoGate,0); near(pianoPitch,60);
        model.removeTrack(pt);
    }
    // Saved with the clip; older presets without the key play every time.
    const auto saved=model.toJson();
    model.fromJson(saved); near(model.getClip(t,c)->chance,0.5);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) for(auto& clip:track["clips"]) clip.erase("chance");
    model.fromJson(legacy); near(model.getClip(t,c)->chance,1.0);
    std::cout << "Clip chance passed: default, never, per-start rolls stable within a start, skipped passes rest or hold per lane type, clamping and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
