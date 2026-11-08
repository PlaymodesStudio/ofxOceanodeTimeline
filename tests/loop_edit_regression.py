#!/usr/bin/env python3
"""Editing a timeline's loop preserves its playhead and the shared root clock."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    TestTimeline model(&owner); model.setSchedulingEnabled(false);
    const auto ref=model.getEditTarget();
    const auto otherId=model.createTimeline("","Other");
    const ofxOceanodeTimelineManager::TimelineRef other{"",otherId};
    model.setLoop(true,0,4);
    auto update=[&]() { owner.transport->latchFrameState(); model.update(); };
    auto beat=[&](const auto& timeline) { double result=-1; assert(model.timelinePlayheadBeat(timeline,result)); return result; };
    auto edit=[&](bool enabled,double start,double end) {
        const double before=beat(ref), global=owner.transport->getState().beatPosition, unaffected=beat(other);
        model.setLoop(enabled,start,end); update();
        near(beat(ref),before); near(owner.transport->getState().beatPosition,global); near(beat(other),unaffected);
    };
    owner.transport->play(); owner.transport->advanceFrameStep(5); update();
    near(owner.transport->getState().beatPosition,10); near(beat(ref),2);
    edit(true,0,8); // used to jump from 2 to 10 % 8 = 2 on this pass; test a different range too
    edit(true,0,6); // used to jump from 2 to 10 % 6 = 4
    owner.transport->advanceFrameStep(0.5); update(); near(beat(ref),3);
    edit(true,1,8); // moving the start handle preserves the position too
    owner.transport->advanceFrameStep(2.75); update(); near(beat(ref),1.5); // wraps at the edited end
    edit(true,2,6); // start can move beyond the current playhead
    edit(true,0,1); // end can move behind it, without jumping backwards
    owner.transport->advanceFrameStep(0.5); update(); near(beat(ref),2.5);
    owner.transport->pause(); update(); near(beat(ref),2.5);
    edit(true,0,7); edit(false,0,7); // stopped edits and disabling loop also preserve it
    owner.transport->play(); update(); near(beat(ref),2.5);
    owner.transport->advanceFrameStep(0.5); update(); near(beat(ref),3.5);
    owner.transport->seekToBeat(1); update(); near(beat(ref),1); // an explicit seek still works
    edit(true,0,4);
    owner.transport->advanceFrameStep(2); update(); near(beat(ref),1);
    owner.transport->stop(); update(); near(beat(ref),0);
    // A timeline with its own transport already preserves its position on loop edits.
    model.setFree(ref,true); model.playTimeline(ref); update();
    owner.transport->advanceFrameStep(0.5); update();
    const double own=beat(ref); edit(true,0,8); near(beat(ref),own);
    std::cout<<"Loop edit passed: start/end handles, continued wrapping, paused edits, play/pause, enable/disable, seek/stop, independent timelines and own transport.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
