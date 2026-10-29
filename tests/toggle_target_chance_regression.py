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
    // Multi Gate: each row's blocks roll on their own, per target.
    {
        ofxOceanodeContainer host;
        host.transport->setDriverMode(TransportDriverMode::FrameStep);
        ofParameter<std::vector<float>> xv("X",{0,0},{0,0},{1,1}), yv("Y",{0,0},{0,0},{1,1});
        ofxOceanodeParameter<std::vector<float>> x,y;
        x.bindParameter(xv); y.bindParameter(yv);
        for(auto* p : {&x,&y}) host.targets[p->getEscapedName()]=p;
        ofxOceanodeTimelineManager rows(&host); rows.setSchedulingEnabled(false);
        const auto rt=rows.createTrack("Gates");
        const auto xb=rows.addBinding(rt,x), yb=rows.addBinding(rt,y);
        const auto rc=rows.createClip(rt,"Gates",0,32);
        const auto rl=rows.createLane(rt,rc,"Gates",ofxOceanodeTimelineLaneType::MultiGate);
        rows.addBindingToLane(rt,rc,rl,xb); rows.addBindingToLane(rt,rc,rl,yb);
        auto* gates=rows.getLane(rt,rc,rl);
        gates->multiRowCount=2; gates->multiGateRows.assign(2,{});
        for(int block=0;block<32;++block)
            for(auto& row : gates->multiGateRows) row.push_back({static_cast<double>(block),0.5,1});
        assert(rows.setLaneTargetProbability(rt,rc,rl,y.getEscapedName(),0.5f));
        host.transport->seekToBeat(0); host.transport->play();
        std::vector<std::vector<float>> rolledRows(32);
        for(int frame=0; frame<5000; ++frame) {
            host.transport->advanceFrameStep(1.0/120.0); rows.update();
            const double beat=host.getTransportState().beatPosition;
            if(beat>=31.9) break;
            const int block=static_cast<int>(beat);
            const double phase=beat-block;
            const auto xs=xv.get(), ys=yv.get();
            assert(xs.size()==2 && ys.size()==2);
            if(phase>0.05 && phase<0.45) {
                near(xs[0],1); near(xs[1],1);
                if(rolledRows[block].empty()) rolledRows[block]=ys;
                near(ys[0],rolledRows[block][0]); near(ys[1],rolledRows[block][1]);
            } else if(phase>0.55 && phase<0.95) {
                near(ys[0],0); near(ys[1],0);
            }
        }
        int reached=0, split=0;
        for(const auto& value : rolledRows) {
            assert(value.size()==2);
            reached+=(value[0]>0.5f)+(value[1]>0.5f);
            split+=(value[0]>0.5f)!=(value[1]>0.5f);
        }
        assert(reached>8 && reached<56);
        assert(split>0); // rows roll independently
    }
    std::cout << "Toggle target chance passed: never, sometimes (stable per block), always, persistence, Multi Gate rows.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
