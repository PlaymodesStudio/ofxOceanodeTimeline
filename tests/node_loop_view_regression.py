#!/usr/bin/env python3
"""Nodes following the global transport see a linked timeline's loop.

The root transport never loops (each timeline keeps its own loop), so the
timeline publishes its loop on the root as a LoopView; a node's
getFrameTransportState wraps the root's states through it. This checks the
view against the timeline's own playhead: position, loop range, wraps
(loopCount, previous vs current frame), a live loop edit, play/pause before
the timeline's pass, a second timeline, a free one, and Render Audio's
suspension.
"""
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
    const std::string canvasView=ofxOceanodeTransport::canvasLoopViewId("");
    auto update=[&]() { owner.transport->latchFrameState(); model.update(); };
    auto beat=[&](const auto& timeline) { double result=-1; assert(model.timelinePlayheadBeat(timeline,result)); return result; };
    auto viewed=[&](const std::string& id) {
        auto state=owner.transport->getState();
        assert(owner.transport->applyLoopView(id,state));
        return state;
    };
    auto frame=[&](const std::string& id) {
        auto f=owner.transport->getFrameState();
        assert(owner.transport->applyLoopView(id,f.previous));
        assert(owner.transport->applyLoopView(id,f.current));
        return f;
    };

    model.setLoop(true,0,4);
    owner.transport->play(); owner.transport->advanceFrameStep(5); update();
    near(owner.transport->getState().beatPosition,10); near(beat(ref),2);
    // The canvas's view (and its other ids) is the first timeline's loop.
    for(const auto& id : {canvasView, ofxOceanodeTransport::canvasLoopViewId("Canvas"),
                          ofxOceanodeTimelineManager::timeSourceId(ref),
                          ofxOceanodeTimelineManager::timeSourceId({"", ""})}) {
        const auto s=viewed(id);
        near(s.beatPosition,2); assert(s.loopActive()); near(s.loopStartBeat,0); near(s.loopEndBeat,4);
        assert(s.loopCount==2);
    }
    // A wrap between two frames: previous before the end, current after the start, one more pass.
    owner.transport->advanceFrameStep(0.75); update(); near(viewed(canvasView).beatPosition,3.5);
    owner.transport->advanceFrameStep(0.75); update();
    {
        const auto f=frame(canvasView);
        near(f.previous.beatPosition,3.5); near(f.current.beatPosition,1); near(beat(ref),1);
        assert(f.current.loopCount==f.previous.loopCount+1);
    }
    // A live loop edit keeps the timeline's playhead; the nodes keep it too.
    model.setLoop(true,0,8); update();
    near(viewed(canvasView).beatPosition,beat(ref)); near(beat(ref),1);
    owner.transport->advanceFrameStep(1); update();
    near(viewed(canvasView).beatPosition,3); near(beat(ref),3);
    // Play/pause read before the timeline's pass (a node updating first) keeps that playhead.
    owner.transport->pause(); owner.transport->latchFrameState();
    const double pausedNode=viewed(canvasView).beatPosition;
    model.update(); near(pausedNode,beat(ref)); near(pausedNode,3);
    owner.transport->play(); owner.transport->latchFrameState();
    const double resumedNode=viewed(canvasView).beatPosition;
    model.update(); near(resumedNode,beat(ref));
    // The second linked timeline: its own view, without a loop it is the root's position.
    {
        const auto s=viewed(ofxOceanodeTimelineManager::timeSourceId(other));
        assert(!s.loopActive()); near(s.beatPosition,owner.transport->getState().beatPosition); near(beat(other),s.beatPosition);
    }
    // The first timeline playing free: the canvas follows the first linked one now.
    model.setFree(ref,true); update();
    {
        auto s=owner.transport->getState();
        assert(!owner.transport->applyLoopView(ofxOceanodeTimelineManager::timeSourceId(ref),s));
        near(viewed(canvasView).beatPosition,owner.transport->getState().beatPosition);
    }
    model.setFree(ref,false); update();
    assert(viewed(canvasView).loopActive());
    // Render Audio suspends the loops: the nodes run straight through as well.
    ofxOceanodeTimelineManager::setLoopsSuspended(true); update();
    {
        auto s=owner.transport->getState();
        assert(!owner.transport->applyLoopView(canvasView,s));
    }
    ofxOceanodeTimelineManager::setLoopsSuspended(false); update();
    assert(viewed(canvasView).loopActive());
    std::cout<<"Node loop view passed: canvas/timeline ids, wraps across frames, live edit, play/pause before the pass, second and free timelines, render suspension.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
