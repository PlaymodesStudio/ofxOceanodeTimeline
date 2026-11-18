#!/usr/bin/env python3
"""A piano roll lane added to a clip joins the note group of the target it is given."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> pitch("Pitch",0,0,127), gate("Gate",0,0,1), cut("Cut",0,0,1);
    ofxOceanodeParameter<float> p,g,c; p.bindParameter(pitch); g.bindParameter(gate); c.bindParameter(cut);
    owner.targets[p.getEscapedName()]=&p; owner.targets[g.getEscapedName()]=&g; owner.targets[c.getEscapedName()]=&c;
    TestTimeline model(&owner);
    model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Wood");
    const auto pb=model.addBinding(t,p), gb=model.addBinding(t,g), cb=model.addBinding(t,c);
    const auto group=model.createNoteGroup(t,"Wood",pb,gb,"");
    std::string first;
    const auto clip=model.createNoteGroupClip(t,group,"Phrase",0,8,&first);
    model.getLane(t,clip,first)->pianoNotes={{0,2,60,0.8f,1}};
    owner.transport->seekToBeat(1); owner.transport->play();
    model.update(); model.applyAutomation(); near(gate,1); near(pitch,60);

    // "+", Piano Roll, Pitch target: the lane takes the whole group, as the first one has.
    for(const auto& picked : {pb, gb}) {
        const auto second=model.createLane(t,clip,"Piano Roll",ofxOceanodeTimelineLaneType::PianoRoll);
        assert(model.addBindingToLane(t,clip,second,picked));
        const auto* lane=model.getLane(t,clip,second);
        assert(lane->pianoPitchBindingId==pb && lane->pianoGateBindingId==gb && lane->pianoVelocityBindingId.empty());
        assert((lane->bindingIds==std::vector<std::string>{pb,gb}));
        // The first lane still sounds while the new one is empty...
        gate=0; pitch=0; owner.transport->seekToBeat(1);
        model.update(); model.applyAutomation(); near(gate,1); near(pitch,60);
        // ...and both play once it has notes (a later note of its own here).
        model.getLane(t,clip,second)->pianoNotes={{4,2,67,0.8f,1}};
        owner.transport->seekToBeat(5);
        model.update(); model.applyAutomation(); near(gate,1); near(pitch,67);
        owner.transport->seekToBeat(1);
        model.update(); model.applyAutomation(); near(gate,1); near(pitch,60);
        assert(model.removeLane(t,clip,second));
    }
    // A target outside any note group is still taken alone, into the next free role.
    const auto loose=model.createLane(t,clip,"Piano Roll",ofxOceanodeTimelineLaneType::PianoRoll);
    assert(model.addBindingToLane(t,clip,loose,cb));
    assert(model.getLane(t,clip,loose)->pianoPitchBindingId==cb && model.getLane(t,clip,loose)->pianoGateBindingId.empty());
    assert((model.getLane(t,clip,loose)->bindingIds==std::vector<std::string>{cb}));
    // Other lane types on a group's parameter are untouched.
    const auto curve=model.createLane(t,clip,"Curve",ofxOceanodeTimelineLaneType::Curve);
    assert(model.addBindingToLane(t,clip,curve,gb));
    assert((model.getLane(t,clip,curve)->bindingIds==std::vector<std::string>{gb}));
    std::cout<<"Piano lane note group passed: joins the group from pitch or gate, first lane keeps sounding, loose targets, other lane types.\n";
}
'''
if __name__=='__main__': fixture.main(cases=CASES)
