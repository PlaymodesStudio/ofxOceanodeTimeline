#!/usr/bin/env python3
"""Verify clip mute and live instrument input after stopping piano playback."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
struct AudioProbe : ofxOceanodeTimelineWaveAudioProvider {
    bool active = false;
    void updateWaveClip(const std::string&, const std::string&, const std::string&,
        float, float, int, double, double, double, double, float, bool, float,
        bool, bool playingClip, bool) override { active = playingClip; }
    void releaseWaveClip(const std::string&, const std::string&) override {}
};
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<float> pitches("Pitch", 72, 0, 127), gates("Gate", 1, 0, 1);
    ofxOceanodeParameter<float> pitch, gate;
    pitch.bindParameter(pitches); gate.bindParameter(gates);
    owner.targets[pitch.getEscapedName()] = &pitch;
    owner.targets[gate.getEscapedName()] = &gate;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    const auto t = model.createTrack("Instrument");
    const auto pb = model.addBinding(t, pitch), gb = model.addBinding(t, gate);
    const auto group = model.createNoteGroup(t, "Notes", pb, gb, "");
    std::string lane;
    const auto c = model.createNoteGroupClip(t, group, "Phrase", 0, 4, &lane);
    model.getLane(t,c,lane)->pianoNotes = {{0, 3, 60, 0.8f, 1}};
    // Merely binding a stopped piano roll must never close live gates or move pitch.
    for(int i=0;i<5;++i) { model.update(); model.applyAutomation(); near(gates,1); near(pitches,72); }
    assert(!model.inspectTarget(gate.getEscapedName(),0,false).writesValue);
    owner.transport->play(); model.update(); near(gates,1); near(pitches,60);
    owner.transport->pause(); model.update(); near(gates,0);
    pitches=76; gates=1;
    for(int i=0;i<5;++i) { model.update(); model.applyAutomation(); near(gates,1); near(pitches,76); }
    // The timeline's own key preview still sounds and releases while stopped.
    model.setPianoKeyPreview(t,c,lane,65); model.update(); near(gates,1); near(pitches,65);
    model.clearPianoKeyPreview(t,c,lane); model.update(); near(gates,0);
    pitches=79; gates=1; model.update(); near(gates,1); near(pitches,79);
    owner.transport->play(); model.update(); near(pitches,60); near(gates,1);
    assert(model.setClipMuted(t,c,true)); near(gates,0);
    assert(model.getClip(t,c)->muted);
    pitches=81; gates=1;
    for(int i=0;i<5;++i) { model.update(); model.applyAutomation(); near(gates,1); near(pitches,81); }
    assert(!model.inspectTarget(pitch.getEscapedName(),0,true).writesValue);
    model.setPianoKeyPreview(t,c,lane,65); model.update(); near(gates,1); near(pitches,65);
    model.clearPianoKeyPreview(t,c,lane); model.update(); near(gates,0);
    pitches=81; gates=1; model.update(); near(pitches,81); near(gates,1);
    assert(model.setClipMuted(t,c,false)); model.update(); near(pitches,60); near(gates,1);
    // Muting the overlapping winner reveals the other clip, without a gate-off.
    std::string upperLane;
    const auto upper = model.createNoteGroupClip(t,group,"Upper",0,4,&upperLane);
    model.getLane(t,upper,upperLane)->pianoNotes = {{0,3,67,0.8f,1}};
    model.update(); near(pitches,67);
    model.setClipMuted(t,upper,true); model.update(); near(pitches,60); near(gates,1);
    // Mute survives serialization, and legacy clips without the key stay audible.
    const auto saved=model.toJson();
    model.fromJson(saved); assert(model.getClip(t,upper)->muted);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) for(auto& clip:track["clips"]) clip.erase("muted");
    model.fromJson(legacy); assert(!model.getClip(t,upper)->muted);
    model.beginTimelineEdit(); model.setClipMuted(t,upper,true); model.endTimelineEdit();
    assert(model.undoTimelineEdit() && !model.getClip(t,upper)->muted);
    assert(model.redoTimelineEdit() && model.getClip(t,upper)->muted);
    // A queued gate-on receives a zero correction when its clip is muted.
    model.setSchedulingEnabled(true);
    model.setSchedulingLookaheadMs(150);
    model.getLane(t,c,lane)->pianoNotes = {{0.1,1,60,0.8f,1}};
    owner.transport->seekToBeat(0); owner.transport->play();
    std::vector<ofxOceanodeScheduledParameterEvent> events;
    ofxOceanodeScheduling::registerParameterTarget(&gate,&model,[&](const auto& event) {
        events.push_back(event); return true;
    });
    model.update();
    assert(std::any_of(events.begin(),events.end(),[](const auto& e){ return !e.isCorrection && ofToFloat(e.value)>0; }));
    model.setClipMuted(t,c,true);
    assert(events.back().isCorrection && ofToFloat(events.back().value)==0);
    ofxOceanodeScheduling::unregisterOwner(&model);
    // Vector instruments also release once, then retain manual polyphonic notes.
    {
        ofxOceanodeContainer polyOwner;
        ofParameter<std::vector<float>> pv("Poly Pitch",{60,64},{0},{127}), gv("Poly Gate",{0,0},{0},{1});
        ofxOceanodeParameter<std::vector<float>> pp,gp; pp.bindParameter(pv); gp.bindParameter(gv);
        polyOwner.targets[pp.getEscapedName()]=&pp; polyOwner.targets[gp.getEscapedName()]=&gp;
        ofxOceanodeTimelineManager poly(&polyOwner); poly.setSchedulingEnabled(false);
        const auto pt=poly.createTrack("Poly");
        const auto p=poly.addBinding(pt,pp),g=poly.addBinding(pt,gp);
        const auto ng=poly.createNoteGroup(pt,"Poly",p,g,"");
        std::string pl; const auto pc=poly.createNoteGroupClip(pt,ng,"Poly",0,4,&pl);
        poly.getLane(pt,pc,pl)->pianoNotes={{0,3,60,0.8f,1}};
        polyOwner.transport->play(); poly.update();
        polyOwner.transport->pause(); poly.update();
        for(float v:gv.get()) near(v,0);
        gv=std::vector<float>{1,1}; pv=std::vector<float>{75,79};
        poly.update(); poly.applyAutomation();
        assert(gv.get()==std::vector<float>({1,1}) && pv.get()==std::vector<float>({75,79}));
    }
    // Ordinary automation and LFO clips stop contributing when muted, too.
    ofParameter<float> controlValue("Control",0,0,1);
    ofxOceanodeParameter<float> control; control.bindParameter(controlValue);
    owner.targets[control.getEscapedName()]=&control;
    const auto ct=model.createTrack("Control");
    const auto cb=model.addBinding(ct,control);
    const auto cc=model.createClip(ct,"Value",0,4);
    const auto cl=model.createLane(ct,cc,"Value",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(ct,cc,cl,cb); model.setClipNumber(ct,cc,cl,0,0.25f);
    model.update(); near(controlValue,0.25);
    model.setClipMuted(ct,cc,true); controlValue=0.9f;
    model.update(); model.applyAutomation(); near(controlValue,0.9);
    const auto lc=model.createLfoClip(ct,cb,"LFO",0,4);
    model.update(); assert(model.inspectTarget(control.getEscapedName(),0,true).writesValue);
    model.setClipMuted(ct,lc,true); controlValue=0.75f;
    model.update(); model.applyAutomation(); near(controlValue,0.75);
    // Audio mute sends an inactive clip to its backend and can resume it.
    AudioProbe audio;
    model.setWaveAudioProvider(&audio);
    const auto wt=model.createWaveTrack("Audio");
    const auto wc=model.createClip(wt,"Sample",0,4);
    model.update(); assert(audio.active);
    model.setClipMuted(wt,wc,true); model.update(); assert(!audio.active);
    model.setClipMuted(wt,wc,false); model.update(); assert(audio.active);
    model.setWaveAudioProvider(nullptr);
    std::cout << "Clip mute passed: stopped live notes, single release, previews, overlaps, persistence, history, queued corrections, polyphony and audio.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
