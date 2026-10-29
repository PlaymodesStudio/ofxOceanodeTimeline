#!/usr/bin/env python3
"""A Toggle lane bound to its own track's solo drives it from playback."""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> av("A",0,0,1), bv("B",0,0,1);
    ofxOceanodeParameter<float> a,b;
    a.bindParameter(av); b.bindParameter(bv);
    for(auto* p : {&a,&b}) owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Soloed"), other=model.createTrack("Other");
    const auto ab=model.addBinding(t,a), bb=model.addBinding(other,b);
    const auto ac=model.createClip(t,"A",0,8);
    const auto al=model.createLane(t,ac,"A",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(t,ac,al,ab); model.setClipNumber(t,ac,al,0,0.5f);
    const auto bc=model.createClip(other,"B",0,8);
    const auto bl=model.createLane(other,bc,"B",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(other,bc,bl,bb); model.setClipNumber(other,bc,bl,0,0.75f);
    // A toggle lane with no parameter target, only the track solo: on 2..4.
    const auto tc=model.createClip(t,"Solo",0,6);
    const auto tl=model.createLane(t,tc,"Solo",ofxOceanodeTimelineLaneType::MultiGate);
    auto* toggle=model.getLane(t,tc,tl);
    toggle->multiRowCount=1; toggle->multiGateRows={{{2,2,1}}};
    owner.transport->play();
    owner.transport->seekToBeat(3); model.update();
    assert(!model.getTrack(t)->solo); // unbound: nothing happens
    assert(model.setLaneDrivesTrackSolo(t,tc,tl,true));
    assert(model.getTrack(t)->solo);
    model.update(); near(av,0.5);
    bv=0; model.update(); near(bv,0); // the other track is silenced
    owner.transport->seekToBeat(1); model.update();
    assert(!model.getTrack(t)->solo); near(bv,0.75);
    owner.transport->seekToBeat(5); model.update(); assert(!model.getTrack(t)->solo);
    owner.transport->seekToBeat(3); model.update(); assert(model.getTrack(t)->solo);
    owner.transport->seekToBeat(7); model.update(); assert(model.getTrack(t)->solo); // holds outside the clip
    // A muted lane does not drive it; the manual solo stays as set.
    model.setTrackSolo(t,false); model.setLaneMuted(t,tc,tl,true);
    owner.transport->seekToBeat(3); model.update(); assert(!model.getTrack(t)->solo);
    model.setLaneMuted(t,tc,tl,false); model.update(); assert(model.getTrack(t)->solo);
    // Persisted; absent in older files.
    const auto saved=model.toJson(); model.fromJson(saved);
    assert(model.getLane(t,tc,tl)->drivesTrackSolo);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) for(auto& clip:track["clips"]) for(auto& l:clip["lanes"]) l.erase("drivesTrackSolo");
    model.fromJson(legacy); assert(!model.getLane(t,tc,tl)->drivesTrackSolo);
    std::cout << "Track solo toggle passed: playback drives solo, holds outside the clip, lane mute, persistence.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
