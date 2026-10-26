#!/usr/bin/env python3
"""Exercise track/lane mute and solo through real playback and scheduling."""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> pv("Pitch",72,0,127), gv("Gate",1,0,1), cv("Control",0,0,1);
    ofxOceanodeParameter<float> pitch,gate,control;
    pitch.bindParameter(pv); gate.bindParameter(gv); control.bindParameter(cv);
    for(auto* p : {&pitch,&gate,&control}) owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Notes"), other=model.createTrack("Other");
    const auto pb=model.addBinding(t,pitch), gb=model.addBinding(t,gate);
    const auto ng=model.createNoteGroup(t,"Notes",pb,gb,"");
    std::string lane;
    const auto c=model.createNoteGroupClip(t,ng,"Phrase",0,4,&lane);
    model.getLane(t,c,lane)->pianoNotes={{0,3,60,0.8f,1}};
    const auto second=model.duplicateLane(t,c,lane);
    model.getLane(t,c,second)->pianoNotes={{0,3,67,0.8f,1}};
    owner.transport->play();
    model.setLaneMuted(t,c,second,true); model.update(); near(pv,60); near(gv,1);
    model.setLaneMuted(t,c,lane,true); near(gv,0);
    pv=79; gv=1; model.update(); near(pv,79); near(gv,1);
    model.setLaneMuted(t,c,lane,false); model.update(); near(pv,60);
    model.setLaneMuted(t,c,second,false);
    model.setLaneSolo(t,c,second,true); model.update(); near(pv,67);
    model.setLaneSolo(t,c,lane,true); model.setLaneMuted(t,c,second,true); model.update(); near(pv,60);
    model.setLaneSolo(t,c,lane,false); model.update(); near(gv,0); // muted solo suppresses other lanes
    model.setLaneMuted(t,c,second,false); model.update(); near(gv,1); near(pv,67);
    model.setTrackMuted(t,true); near(gv,0);
    model.setTrackMuted(t,false); model.update(); near(gv,1);
    model.setTrackSolo(other,true); near(gv,0);
    model.setTrackSolo(t,true); model.update(); near(gv,1);
    model.setTrackMuted(t,true); near(gv,0);
    model.setTrackMuted(t,false); model.setTrackSolo(other,false); model.setTrackSolo(t,false);
    // All flags persist; undo/redo includes both lane and track controls.
    model.beginTimelineEdit(); model.setTrackMuted(t,true); model.setLaneMuted(t,c,second,true); model.endTimelineEdit();
    assert(model.undoTimelineEdit()); assert(!model.getTrack(t)->muted && !model.getLane(t,c,second)->muted);
    assert(model.redoTimelineEdit()); assert(model.getTrack(t)->muted && model.getLane(t,c,second)->muted);
    model.setTrackSolo(t,true);
    const auto saved=model.toJson(); model.fromJson(saved);
    assert(model.getTrack(t)->muted && model.getTrack(t)->solo);
    assert(model.getLane(t,c,second)->solo && model.getLane(t,c,second)->muted);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) {
        track.erase("muted"); track.erase("solo");
        for(auto& clip:track["clips"]) for(auto& l:clip["lanes"]) { l.erase("muted"); l.erase("solo"); }
    }
    model.fromJson(legacy); assert(!model.getTrack(t)->muted && !model.getTrack(t)->solo);
    assert(!model.getLane(t,c,second)->muted && !model.getLane(t,c,second)->solo);
    // A queued gate-on gets corrected when its lane or track becomes inaudible.
    model.setLaneMuted(t,c,second,true);
    model.getLane(t,c,lane)->pianoNotes={{0.1,1,60,0.8f,1}};
    model.setSchedulingEnabled(true); model.setSchedulingLookaheadAdaptive(false); model.setSchedulingLookaheadMs(150);
    std::vector<ofxOceanodeScheduledParameterEvent> events;
    ofxOceanodeScheduling::registerParameterTarget(&gate,&model,[&](const auto& event){ events.push_back(event); return true; });
    for(int mode=0;mode<3;++mode) {
        events.clear(); owner.transport->seekToBeat(0); model.update();
        assert(std::any_of(events.begin(),events.end(),[](const auto& e){return !e.isCorrection && ofToFloat(e.value)>0;}));
        if(mode==0) model.setLaneMuted(t,c,lane,true);
        if(mode==1) model.setTrackMuted(t,true);
        if(mode==2) model.setTrackSolo(other,true);
        model.update(); assert(events.back().isCorrection && ofToFloat(events.back().value)==0);
        model.setLaneMuted(t,c,lane,false); model.setTrackMuted(t,false); model.setTrackSolo(other,false);
    }
    ofxOceanodeScheduling::unregisterOwner(&model);
    // Number automation, LFO controls and audio follow their enablement too.
    const auto cb=model.addBinding(other,control), cc=model.createClip(other,"Value",0,4);
    const auto cl=model.createLane(other,cc,"Value",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(other,cc,cl,cb); model.setClipNumber(other,cc,cl,0,0.25f);
    model.setSchedulingEnabled(false); model.update(); near(cv,0.25);
    model.setLaneMuted(other,cc,cl,true); cv=0.9f; model.update(); near(cv,0.9);
    const auto lfo=model.createLfoClip(other,cb,"LFO",0,4);

    auto* oscillator=model.getClip(other,lfo);
    std::string frequency,scale;
    for(auto& lane:oscillator->lanes) {
        if(lane.lfoParameter=="frequency") {
            frequency=lane.id; lane.curveInterpolation="Value";
            lane.lfoValue=ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane,1);
        }
        if(lane.lfoParameter=="scale") {
            scale=lane.id; lane.curveInterpolation="Value";
            lane.lfoValue=ofxOceanodeTimelineLfo::laneNormalizedFromValue(lane,0.25);
        }
    }
    assert(!frequency.empty() && !scale.empty());
    near(ofxOceanodeTimelineLfo::cyclesAt(*oscillator,1),1);
    near(ofxOceanodeTimelineLfo::evaluateParameter(*oscillator,"scale",0,1),0.25);
    model.setLaneMuted(other,lfo,frequency,true);
    near(ofxOceanodeTimelineLfo::cyclesAt(*oscillator,1),0.25);
    model.setLaneMuted(other,lfo,frequency,false);
    model.setLaneSolo(other,lfo,frequency,true);
    near(ofxOceanodeTimelineLfo::evaluateParameter(*oscillator,"scale",0,1),1);
    model.setLaneSolo(other,lfo,scale,true);
    near(ofxOceanodeTimelineLfo::evaluateParameter(*oscillator,"scale",0,1),0.25);
    model.setLaneMuted(other,lfo,scale,true);
    near(ofxOceanodeTimelineLfo::evaluateParameter(*oscillator,"scale",0,1),1);
    model.setTrackMuted(other,true); model.update(); assert(!model.evaluateLfoTrack(other,0).active);
    model.setTrackMuted(other,false); assert(model.evaluateLfoTrack(other,0).active);
    AudioProbe audio; model.setWaveAudioProvider(&audio);
    const auto wt=model.createWaveTrack("Audio"), wc=model.createClip(wt,"Sample",0,4);
    model.update(); assert(audio.active);
    model.setTrackMuted(wt,true); model.update(); assert(!audio.active);
    model.setTrackMuted(wt,false); model.update(); assert(audio.active);
    model.setTrackSolo(t,true); model.update(); assert(!audio.active);
    model.setTrackSolo(wt,true); model.update(); assert(audio.active);
    model.setWaveAudioProvider(nullptr);
    std::cout << "Mute/solo passed: lane and track playback, shared piano roles, gates, persistence, history, queued corrections, automation, LFO and audio.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
