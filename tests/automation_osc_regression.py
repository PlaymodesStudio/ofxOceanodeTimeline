#!/usr/bin/env python3
"""Exercise continuous parameter automation alongside queued piano notes."""
from pathlib import Path
import sys
base=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(base/'tests'))
import timeline_regression as fixture
cases=fixture.CASES.split('int main()')[0]+r'''
int main(){
    ofSeedRandom();ofxOceanodeContainer owner;
    // Frozen like FrameStep, but scheduling only follows clock-driven transports:
    // External with no clock attached never advances the playhead.
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> pv("Pitch",60,0,127),gv("Gate",0,0,1);
    ofxOceanodeParameter<float> pitch,gate;pitch.bindParameter(pv);gate.bindParameter(gv);
    owner.targets[pitch.getEscapedName()]=&pitch;owner.targets[gate.getEscapedName()]=&gate;
    std::vector<std::unique_ptr<ofParameter<float>>> values;
    std::vector<std::unique_ptr<ofxOceanodeParameter<float>>> parameters;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingLookaheadMs(1000);
    const auto track=model.createTrack("Piano");
    const auto pb=model.addBinding(track,pitch),gb=model.addBinding(track,gate);
    const auto group=model.createNoteGroup(track,"Notes",pb,gb,"");
    std::string lane;const auto clip=model.createNoteGroupClip(track,group,"Phrase",0,4,&lane);
    model.getLane(track,clip,lane)->pianoNotes={{0.8,0.2,64,0.8f,1},{1.2,0.2,67,0.7f,1},{1.6,0.2,69,0.9f,1}};
    struct Curve{std::string track,clip,lane;};std::vector<Curve> curves;
    int changes=0;ofEventListeners listeners;
    for(int i=0;i<24;++i){
        values.push_back(std::make_unique<ofParameter<float>>("Timbre "+std::to_string(i),0,0,1));
        parameters.push_back(std::make_unique<ofxOceanodeParameter<float>>());
        auto&p=*parameters.back();p.bindParameter(*values.back());owner.targets[p.getEscapedName()]=&p;
        listeners.push(values.back()->newListener([&](float&){++changes;}));
        const auto t=model.createTrack("Sound "+std::to_string(i)),b=model.addBinding(t,p);
        const auto c=model.createClip(t,"Curve",0,4),l=model.createLane(t,c,"Curve",ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(t,c,l,b);model.getLane(t,c,l)->curvePoints={{0,0},{4,1}};
        curves.push_back({t,c,l});
        // Continuous controls must use the frame path even with a backend registered.
        ofxOceanodeScheduling::registerParameterTarget(&p,&model,[&](const auto&){assert(false);return true;});
    }
    struct Sent{char role;ofxOceanodeScheduledParameterEvent e;};std::vector<Sent> sent;
    for(auto entry:{std::make_pair(&pitch,'p'),std::make_pair(&gate,'g')})
        ofxOceanodeScheduling::registerParameterTarget(entry.first,&model,[&,role=entry.second](const auto&e){sent.push_back({role,e});return true;});
    owner.transport->play();model.update();
    const auto queued=sent.size();assert(queued>=9);
    for(int frame=1;frame<=40;++frame){
        const float value=frame/100.f;
        for(const auto&c:curves)model.getLane(c.track,c.clip,c.lane)->curvePoints[0].value=value;
        model.update();model.applyAutomation();
        for(const auto&v:values)near(v->get(),value);
        assert(sent.size()==queued); // no repeated note or correction traffic
    }
    assert(changes==24*40);
    for(const auto&s:sent){assert(!s.e.isCorrection);if(s.role=='g'&&ofToFloat(s.e.value)>0)
        assert(std::any_of(sent.begin(),sent.end(),[&](const auto&p){return p.role=='p'&&p.e.dueSteadyTimeUs+2000==s.e.dueSteadyTimeUs;}));}
    ofxOceanodeScheduling::unregisterOwner(&model);
    std::cout<<"PASS: 24 continuous synth controls, 40 automation frames and both frame-application passes produce exactly 960 changes; queued piano notes keep their count, order and pitch lead without corrections or duplicate sends.\n";
}
'''
fixture.main(cases=cases)
