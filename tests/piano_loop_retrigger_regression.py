#!/usr/bin/env python3
"""A legato chord the timeline loop wraps back onto closes and reopens its gates.

Notes that run to the loop end and a chord on the loop start share voice slots:
without a gap at the wrap the gate stays open and a struck instrument
(MarimbaPro) never strikes the loop's first chord again."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main(){
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<std::vector<float>> pitches("Pitch",{60},{0},{127}), gates("Gate",{0},{0},{1});
    ofxOceanodeParameter<std::vector<float>> pt,gt; pt.bindParameter(pitches); gt.bindParameter(gates);
    owner.targets[pt.getEscapedName()]=&pt; owner.targets[gt.getEscapedName()]=&gt;
    TestTimeline model(&owner); model.setSchedulingEnabled(false);
    auto track=model.createTrack("Marimba");
    auto pb=model.addBinding(track,pt),gb=model.addBinding(track,gt);
    auto group=model.createNoteGroup(track,"Notes",pb,gb,"");
    std::string lane; auto clip=model.createNoteGroupClip(track,group,"Phrase",0,4,&lane);
    // Two legato chords: the second runs to the loop end, the first starts on the loop start.
    model.getLane(track,clip,lane)->pianoNotes={{0,2,72,0.8f,1},{0,2,75,0.8f,1},{0,2,79,0.8f,1},
                                               {2,2,72,0.8f,1},{2,2,76,0.8f,1},{2,2,80,0.8f,1}};
    model.setLoop(true,0,4);
    std::vector<float> previous(3,0.0f);
    std::vector<std::vector<int>> strikes; // per chord opening: voices whose gate rose
    auto gateListener=gates.newListener([&](std::vector<float>& v){
        std::vector<int> rose;
        for(size_t i=0;i<3 && i<v.size();++i) if(v[i]>0 && previous[i]<=0) rose.push_back(static_cast<int>(i));
        if(!rose.empty()) { strikes.push_back(rose); for(size_t i=0;i<3;++i) assert(pitches.get().size()>=3); }
        previous.assign(v.begin(),v.end()); previous.resize(3,0.0f);
    });
    auto update=[&](){ owner.transport->latchFrameState(); model.update(); };
    owner.transport->play(); update();
    for(int i=0;i<200;++i){ owner.transport->advanceFrameStep(0.03125); update(); } // three loop passes
    // Every chord, the loop's first one included on each pass, strikes all three voices.
    assert(strikes.size()>=6);
    for(const auto& chord:strikes) assert(chord.size()==3);
    std::cout<<"Piano loop retrigger passed: legato chords strike every voice on each loop pass.\n";
}
'''
fixture.main(cases=CASES)
