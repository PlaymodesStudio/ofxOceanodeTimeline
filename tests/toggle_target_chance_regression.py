#!/usr/bin/env python3
"""A Toggle lane's per-target chance decides which blocks reach each target."""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> av("A",0,0,1), bv("B",0,0,1), cv("C",0,0,1);
    ofxOceanodeParameter<float> a,b,c;
    a.bindParameter(av); b.bindParameter(bv); c.bindParameter(cv);
    for(auto* p : {&a,&b,&c}) owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Toggles");
    const auto ab=model.addBinding(t,a), bb=model.addBinding(t,b), cb=model.addBinding(t,c);
    const auto clip=model.createClip(t,"Toggle",0,32);
    const auto lane=model.createLane(t,clip,"Toggle",ofxOceanodeTimelineLaneType::MultiGate);
    for(const auto& id : {ab,bb,cb}) model.addBindingToLane(t,clip,lane,id);
    auto* toggle=model.getLane(t,clip,lane);
    toggle->multiRowCount=1; toggle->multiGateRows.assign(1,{});
    for(int block=0;block<32;++block) toggle->multiGateRows[0].push_back({static_cast<double>(block),0.5,1});
    const std::string pathB=b.getEscapedName(), pathC=c.getEscapedName();
    assert(model.setLaneTargetProbability(t,clip,lane,pathB,0.0f));
    assert(model.setLaneTargetProbability(t,clip,lane,pathC,0.5f));
    assert(!model.setLaneTargetProbability(t,clip,lane,pathC,0.5f));
    // Play straight through (a seek starts a new roll, as for steps).
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    owner.transport->seekToBeat(0); owner.transport->play();
    std::vector<float> rolled(32, -1.0f);
    for(int frame=0; frame<5000; ++frame) {
        owner.transport->advanceFrameStep(1.0/120.0); model.update();
        const double beat=owner.getTransportState().beatPosition;
        if(beat>=31.9) break;
        const int block=static_cast<int>(beat);
        const double phase=beat-block;
        if(phase>0.05 && phase<0.45) {
            near(av,1); near(bv,0);
            if(rolled[block]<0) rolled[block]=cv.get();
            near(cv,rolled[block]); // one roll per block
        } else if(phase>0.55 && phase<0.95) {
            near(av,0); near(cv,0);
        }
    }
    int reachedC=0;
    for(const float value : rolled) { assert(value>=0); if(value>0.5f) ++reachedC; }
    assert(reachedC>4 && reachedC<28);
    // Back to always: the entry goes, every block reaches it.
    assert(model.setLaneTargetProbability(t,clip,lane,pathB,1.0f));
    assert(model.getLane(t,clip,lane)->targetProbability.count(pathB)==0);
    owner.transport->setDriverMode(TransportDriverMode::External);
    owner.transport->seekToBeat(3.1); model.update(); near(bv,1);
    // Persisted; absent in older files.
    const auto saved=model.toJson(); model.fromJson(saved);
    near(model.getLane(t,clip,lane)->targetProbability.at(pathC),0.5);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) for(auto& cl:track["clips"]) for(auto& l:cl["lanes"]) l.erase("targetProbability");
    model.fromJson(legacy); assert(model.getLane(t,clip,lane)->targetProbability.empty());
    std::cout << "Toggle target chance passed: never, sometimes (stable per block), always, persistence.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
