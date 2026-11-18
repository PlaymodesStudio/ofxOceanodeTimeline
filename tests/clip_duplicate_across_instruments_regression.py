#!/usr/bin/env python3
"""Duplicate a piano roll clip onto a track that plays another instrument."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    ofParameter<float> pitchA("PitchA",60,0,127), gateA("GateA",0,0,1), pitchB("PitchB",60,0,127), gateB("GateB",0,0,1),
                       pitchC("PitchC",60,0,127), gateC("GateC",0,0,1), cut("Cut",0,0,1);
    ofxOceanodeParameter<float> pa,ga,pb,gb,pc,gc,cu;
    pa.bindParameter(pitchA);ga.bindParameter(gateA);pb.bindParameter(pitchB);gb.bindParameter(gateB);
    pc.bindParameter(pitchC);gc.bindParameter(gateC);cu.bindParameter(cut);
    for(auto* p : std::vector<ofxOceanodeAbstractParameter*>{&pa,&ga,&pb,&gb,&pc,&gc,&cu}) owner.targets[p->getEscapedName()]=p;
    TestTimeline model(&owner);
    // Two instruments, each on its own track with its own note group.
    const auto wood=model.createTrack("Wood"), marimba=model.createTrack("Marimba");
    const auto woodPitch=model.addBinding(wood,pa), woodGate=model.addBinding(wood,ga);
    const auto marPitch=model.addBinding(marimba,pb), marGate=model.addBinding(marimba,gb);
    const auto woodGroup=model.createNoteGroup(wood,"Wood",woodPitch,woodGate,"");
    const auto marGroup=model.createNoteGroup(marimba,"Marimba",marPitch,marGate,"");
    assert(!woodGroup.empty() && !marGroup.empty());
    const auto phrase=model.createNoteGroupClip(wood,woodGroup,"Phrase",0,8);
    model.getClip(wood,phrase)->lanes.front().pianoNotes={{0,1,64,0.7f,1},{2,1,67,0.7f,1}};

    // Dropped on the other track's single row (no binding named): the notes
    // arrive, now driving that track's note group.
    const auto copy=model.duplicateClip(wood,phrase,marimba,16);
    assert(!copy.empty());
    const auto* copied=model.getClip(marimba,copy);
    assert(copied && copied->startBeat==16 && copied->durationBeats==8 && copied->lanes.size()==1);
    const auto& lane=copied->lanes.front();
    assert(lane.type==ofxOceanodeTimelineLaneType::PianoRoll && lane.pianoNotes.size()==2 && lane.pianoNotes[1].pitch==67);
    assert(lane.pianoPitchBindingId==marPitch && lane.pianoGateBindingId==marGate && lane.pianoVelocityBindingId.empty());
    assert((lane.bindingIds==std::vector<std::string>{marPitch,marGate}));
    assert(lane.id!=model.getClip(wood,phrase)->lanes.front().id);
    // The source is untouched, and the copy is independent.
    assert(model.getClip(wood,phrase)->lanes.front().pianoPitchBindingId==woodPitch);
    model.getClip(marimba,copy)->lanes.front().pianoNotes[0].pitch=72;
    assert(model.getClip(wood,phrase)->lanes.front().pianoNotes[0].pitch==64);

    // Dropped on the note group's own row: the same result.
    const auto onRow=model.duplicateClip(wood,phrase,marimba,32,model.getClip(wood,phrase)->lanes.front().id,marPitch);
    assert(!onRow.empty() && model.getClip(marimba,onRow)->lanes.front().pianoGateBindingId==marGate);

    // A second lane on a parameter the destination lacks is left behind; the notes still go.
    const auto curve=model.createLane(wood,phrase,"Curve",ofxOceanodeTimelineLaneType::Curve);
    const auto woodCut=model.addBinding(wood,cu);
    model.addBindingToLane(wood,phrase,curve,woodCut);
    const auto mixed=model.duplicateClip(wood,phrase,marimba,48);
    assert(!mixed.empty() && model.getClip(marimba,mixed)->lanes.size()==1 &&
           model.getClip(marimba,mixed)->lanes.front().pianoPitchBindingId==marPitch);

    // A track with no note group still refuses piano notes, and nothing is created.
    const auto plain=model.createTrack("Plain");
    model.addBinding(plain,pc); model.addBinding(plain,gc);
    const auto clipsBefore=model.getTrack(plain)->clips.size();
    model.removeLane(wood,phrase,curve);
    assert(model.duplicateClip(wood,phrase,plain,0).empty() && model.getTrack(plain)->clips.size()==clipsBefore);

    // Same-parameter tracks keep mapping by parameter, as before.
    const auto twin=model.createTrack("Wood 2");
    const auto twinPitch=model.addBinding(twin,pa), twinGate=model.addBinding(twin,ga);
    model.createNoteGroup(twin,"Wood 2",twinPitch,twinGate,"");
    const auto same=model.duplicateClip(wood,phrase,twin,0);
    assert(!same.empty() && model.getClip(twin,same)->lanes.front().pianoPitchBindingId==twinPitch &&
           model.getClip(twin,same)->lanes.front().pianoGateBindingId==twinGate);
    std::cout<<"Clip duplication across instruments passed: track row, group row, mixed lanes, no note group, same parameters.\n";
}
'''
if __name__=='__main__': fixture.main(cases=CASES)
