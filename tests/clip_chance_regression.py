#!/usr/bin/env python3
"""Verify clip chance: whole-clip probability, re-rolled each time the clip starts."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> controlValue("Control",0,0,1);
    ofxOceanodeParameter<float> control; control.bindParameter(controlValue);
    owner.targets[control.getEscapedName()]=&control;
    ofxOceanodeTimelineManager model(&owner);
    model.setSchedulingEnabled(false);
    const auto t=model.createTrack("Control");
    const auto b=model.addBinding(t,control);
    const auto c=model.createClip(t,"Value",0,4);
    const auto l=model.createLane(t,c,"Value",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(t,c,l,b); model.setClipNumber(t,c,l,0,0.25f);
    owner.transport->seekToBeat(1); owner.transport->play();
    // New clips always play.
    assert(model.getClip(t,c)->chance==1.0f);
    model.update(); model.applyAutomation(); near(controlValue,0.25);
    // 0%: the clip never plays, its content is kept.
    assert(model.setClipChance(t,c,0.0f));
    controlValue=0.9f; model.update(); model.applyAutomation(); near(controlValue,0.9);
    assert(!model.inspectTarget(control.getEscapedName(),1,true).writesValue);
    assert(!model.setClipChance(t,c,-1.0f)); // clamped to 0, unchanged
    // 50%: one roll per start (each seek is a new start), stable while playing through.
    model.setClipChance(t,c,0.5f);
    int played=0;
    for(int start=0;start<200;++start) {
        owner.transport->seekToBeat(1);
        controlValue=0.9f; model.update(); model.applyAutomation();
        const bool plays=std::abs(controlValue.get()-0.25f)<1e-4f;
        for(int i=0;i<3;++i) {
            controlValue=0.9f; model.update(); model.applyAutomation();
            assert((std::abs(controlValue.get()-0.25f)<1e-4f)==plays);
        }
        played+=plays;
    }
    assert(played>60 && played<140);
    // Saved with the clip; older presets without the key play every time.
    const auto saved=model.toJson();
    model.fromJson(saved); near(model.getClip(t,c)->chance,0.5);
    auto legacy=saved;
    for(auto& track:legacy["tracks"]) for(auto& clip:track["clips"]) clip.erase("chance");
    model.fromJson(legacy); near(model.getClip(t,c)->chance,1.0);
    std::cout << "Clip chance passed: default, never, per-start rolls stable within a start, clamping and persistence.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
