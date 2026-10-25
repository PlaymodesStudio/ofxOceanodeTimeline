#!/usr/bin/env python3
"""Regress piano input ordering using the production timeline and real OF parameters."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
struct Sent { char role; ofxOceanodeScheduledParameterEvent event; };
int main(){
    ofSeedRandom();
    // Deliberately misleading names: ordering must use piano routing roles.
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> pv("Z Frequency",48,0,127),gv("A Trigger",0,0,1),vv("B Level",0,0,1);
    ofxOceanodeParameter<float> pitch,gate,velocity;
    pitch.bindParameter(pv);gate.bindParameter(gv);velocity.bindParameter(vv);
    for(auto* p:{&pitch,&gate,&velocity})owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    auto track=model.createTrack("Instrument");
    auto pb=model.addBinding(track,pitch),gb=model.addBinding(track,gate),vb=model.addBinding(track,velocity);
    auto group=model.createNoteGroup(track,"Notes",pb,gb,vb);
    std::string lane;auto clip=model.createNoteGroupClip(track,group,"Phrase",0,4,&lane);
    model.getLane(track,clip,lane)->pianoNotes={{0,0.25,60,0.8f,1},{0.5,0.25,67,0.7f,1}};
    std::vector<char> applied;
    auto pl=pv.newListener([&](float&){applied.push_back('p');});
    auto vl=vv.newListener([&](float&){applied.push_back('v');});
    auto gl=gv.newListener([&](float&){applied.push_back('g');assert(pv==60 || pv==67);});
    owner.transport->play();model.update();
    assert(applied==std::vector<char>({'p','v','g'}));
    owner.transport->pause();model.update();
    // Schedule a note far enough ahead to observe the distinct prep times.
    model.getLane(track,clip,lane)->pianoNotes={{0.2,0.2,64,0.8f,1},{0.6,0.2,69,0.7f,1}};
    owner.transport->seekToBeat(0);owner.transport->play();
    model.setSchedulingEnabled(true);model.setSchedulingLookaheadMs(500);
    std::vector<Sent> sent;
    for(auto entry:{std::make_pair(&pitch,'p'),std::make_pair(&velocity,'v'),std::make_pair(&gate,'g')})
        ofxOceanodeScheduling::registerParameterTarget(entry.first,&model,[&,role=entry.second](const auto& e){sent.push_back({role,e});return true;});
    model.update();
    std::vector<Sent> ons;
    for(const auto& s:sent)if(s.role=='g' && !s.event.isCorrection && ofToFloat(s.event.value)>0)ons.push_back(s);
    assert(ons.size()==2);
    for(const auto& on:ons){
        const auto gateUs=on.event.dueSteadyTimeUs;
        assert(std::any_of(sent.begin(),sent.end(),[&](const auto& s){return s.role=='p' && s.event.dueSteadyTimeUs+2000==gateUs;}));
        assert(std::any_of(sent.begin(),sent.end(),[&](const auto& s){return s.role=='v' && s.event.dueSteadyTimeUs+1000==gateUs;}));
    }
    assert(sent[0].role=='p' && sent[1].role=='v' && sent[2].role=='g');
    const auto count=sent.size();for(int i=0;i<10;++i)model.update();assert(sent.size()==count);
    ofxOceanodeScheduling::unregisterOwner(&model);
    std::cout<<"PASS: frame Pitch/Velocity/Gate order; all instruments use routed roles; 2 ms pitch and 1 ms velocity leads; multiple notes; stable lookahead.\n";
}
'''
fixture.main(cases=cases)
