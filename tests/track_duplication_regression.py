#!/usr/bin/env python3
"""Duplicate complete tracks without sharing content, routing ids or clip groups."""
import timeline_regression as fixture
CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    ofParameter<float> pitch("Pitch",60,0,127), gate("Gate",0,0,1);
    ofxOceanodeParameter<float> p,g; p.bindParameter(pitch);g.bindParameter(gate);
    owner.targets[p.getEscapedName()]=&p;owner.targets[g.getEscapedName()]=&g;
    ofxOceanodeTimelineManager model(&owner);
    const auto t=model.createTrack("Instrument"), next=model.createTrack("Next");
    const auto pb=model.addBinding(t,p),gb=model.addBinding(t,g);
    const auto group=model.createNoteGroup(t,"Notes",pb,gb,"");model.setNoteGroupVoices(t,group,3);
    const auto c=model.createNoteGroupClip(t,group,"Phrase",1,8);
    auto* lane=&model.getClip(t,c)->lanes.front();lane->pianoNotes={{0,1,64,0.7f,1}};
    const auto curve=model.createLane(t,c,"Curve",ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(t,c,curve,gb);
    model.getLane(t,c,curve)->curvePoints={{0,0.2f},{4,0.8f}};
    const auto lfo=model.createLfoClip(t,gb,"LFO",9,4);
    const auto originalGroup=model.groupClips({{t,c},{t,lfo}});
    model.getTrack(t)->collapsed=true;
    model.getTrack(t)->color=ofColor(15,25,35);
    model.getBinding(t,pb)->hasLiveOverride=true;model.getBinding(t,pb)->liveOverrideValue="80";
    const auto before=model.toJson();
    model.beginTimelineEdit();const auto copyId=model.duplicateTrack(t);model.endTimelineEdit();
    assert(!copyId.empty() && copyId!=t);
    const auto* copy=model.getTrack(copyId);const auto* source=model.getTrack(t);
    assert(model.getTracks()[1].id==copyId && model.getTracks()[2].id==next);
    assert(copy->name!=source->name && copy->collapsed && copy->color==source->color);
    assert(copy->bindings.size()==2 && copy->clips.size()==2 && copy->noteGroups.size()==1);
    assert(copy->bindings[0].id!=pb && copy->bindings[1].id!=gb && !copy->bindings[0].hasLiveOverride);
    assert(source->bindings[0].hasLiveOverride);
    assert(copy->noteGroups[0].voices==3 && copy->noteGroups[0].id!=source->noteGroups[0].id);
    const auto copiedClip=copy->clips[0].id,copiedLane=copy->clips[0].lanes[0].id;
    assert(copiedClip!=c && copiedLane!=source->clips[0].lanes[0].id);
    assert(copy->clips[0].startBeat==1 && copy->clips[0].durationBeats==8 && copy->clips[0].lanes.size()==2);
    assert(copy->clips[0].lanes[0].pianoNotes[0].pitch==64);
    assert(copy->clips[0].lanes[0].pianoPitchBindingId==copy->noteGroups[0].pitchBindingId);
    assert(copy->clips[0].lanes[0].pianoGateBindingId==copy->noteGroups[0].gateBindingId);
    assert(copy->clips[1].isLfo && copy->clips[1].lfoOutputBindingId==copy->bindings[1].id);
    for(const auto& cl:copy->clips)for(const auto& ln:cl.lanes)for(const auto& b:ln.bindingIds)
        assert(b!=pb && b!=gb && model.getBinding(copyId,b));
    const auto* clonedGroup=model.getGroupForClip(copyId,copiedClip);
    assert(clonedGroup && clonedGroup->id!=originalGroup && clonedGroup->members.size()==2);
    for(const auto& member:clonedGroup->members)assert(member.first==copyId);
    assert(model.toJson()["tracks"][0]==before["tracks"][0]);
    const auto duplicated=model.toJson();
    assert(model.undoTimelineEdit() && model.toJson()==before);
    assert(model.redoTimelineEdit() && model.toJson()==duplicated);
    model.getLane(copyId,copiedClip,copiedLane)->pianoNotes[0].pitch=72;
    assert(model.getClip(t,c)->lanes[0].pianoNotes[0].pitch==64);
    model.removeTrack(t);
    assert(model.getTrack(copyId) && model.getBinding(copyId,model.getTrack(copyId)->bindings[0].id));
    // Audio settings, cached media and the track's volume envelope survive too.
    const auto wt=model.createWaveTrack("Audio"),wc=model.createClip(wt,"Slice",3,2);
    auto* wave=model.getClip(wt,wc);wave->waveFilePath="sample.wav";wave->waveGain=0.4f;
    wave->waveReverse=true;wave->waveSourceStartBeat=1;wave->waveformPeaks={0.1f,0.5f};
    model.getTrack(wt)->waveVolumePoints={{0,0.2f},{4,0.7f}};model.getTrack(wt)->waveTrackHeight=170;
    const auto wd=model.duplicateTrack(wt);const auto* audio=model.getTrack(wd);
    assert(audio->isWaveTrack && audio->waveTrackHeight==170 && audio->waveVolumePoints[1].value==0.7f);
    assert(audio->clips[0].id!=wc && audio->clips[0].waveGain==0.4f && audio->clips[0].waveReverse);
    assert(audio->clips[0].waveSourceStartBeat==1 && audio->clips[0].waveformPeaks.size()==2);
    // Break one mixed clip into lane-ordered tracks while retaining its
    // sibling clips and every source binding. Undo restores all relationships.
    const auto beforeBreak=model.toJson();
    const auto sourceClip=*model.getClip(copyId,copiedClip);
    model.beginTimelineEdit();
    const auto split=model.breakClipLanesIntoTracks(model.getTrack(copyId)->id,model.getClip(copyId,copiedClip)->id);
    assert(split.size()==2 && !model.getClip(copyId,copiedClip));
    assert(model.getTrack(copyId)->clips.size()==1 && model.getTrack(copyId)->bindings.size()==2);
    for(size_t i=0;i<split.size();++i) {
        const auto* track=model.getTrack(split[i].first);const auto* cl=model.getClip(split[i].first,split[i].second);
        assert(track && cl && track->clips.size()==1 && cl->lanes.size()==1);
        assert(cl->startBeat==sourceClip.startBeat && cl->durationBeats==sourceClip.durationBeats);
        assert(cl->contentDurationBeats==sourceClip.contentDurationBeats && cl->contentStretch==sourceClip.contentStretch);
        assert(cl->repeatContent==sourceClip.repeatContent && cl->lanes[0].name==sourceClip.lanes[i].name);
        assert(cl->id!=sourceClip.id && cl->lanes[0].id!=sourceClip.lanes[i].id);
        assert(model.getTracks()[1+i].id==track->id);
        assert(!model.getGroupForClip(track->id,cl->id));
        for(const auto& id:cl->lanes[0].bindingIds)assert(model.getBinding(track->id,id));
    }
    assert(model.getTrack(split[0].first)->bindings.size()==2 && model.getTrack(split[0].first)->noteGroups.size()==1);
    assert(model.getTrack(split[1].first)->bindings.size()==1 && model.getTrack(split[1].first)->noteGroups.empty());
    assert(model.getClip(split[0].first,split[0].second)->lanes[0].pianoNotes[0].pitch==72);
    assert(model.getClip(split[1].first,split[1].second)->lanes[0].curvePoints.size()==2);
    assert(model.breakClipLanesIntoTracks(wt,wc).empty());
    assert(model.breakClipLanesIntoTracks(copyId,model.getTrack(copyId)->clips[0].id).empty()); // LFO
    model.endTimelineEdit();assert(model.undoTimelineEdit() && model.toJson()==beforeBreak);
    assert(model.redoTimelineEdit() && !model.getClip(copyId,copiedClip));
    const auto empty=model.createTrack("Empty");assert(!model.duplicateTrack(empty).empty());
    const auto stable=model.toJson();assert(model.duplicateTrack("missing").empty() && model.toJson()==stable);
    std::cout<<"Track duplication passed: independent ids, multi-lane content, routing, note groups, LFO, clip groups, audio envelope, breaking lanes into tracks, order and undo/redo.\n";
}
'''
if __name__=='__main__': fixture.main(cases=CASES)
