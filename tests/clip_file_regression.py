#!/usr/bin/env python3
"""Portable multi-lane clip files: remapping, isolation, media and failure handling."""
import timeline_regression as fixture

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
int main() {
    ofSeedRandom();
    ofxOceanodeContainer oldProject, newProject;
    ofParameter<float> oldPitch("Old pitch",60,0,127), oldGate("Old gate",0,0,1), oldFilter("Old filter",0,0,1);
    ofParameter<float> newPitch("New pitch",60,0,127), newGate("New gate",0,0,1), newFilter("New filter",0,0,1);
    ofxOceanodeParameter<float> op,og,of,np,ng,nf;
    op.bindParameter(oldPitch); og.bindParameter(oldGate); of.bindParameter(oldFilter);
    np.bindParameter(newPitch); ng.bindParameter(newGate); nf.bindParameter(newFilter);
    for(auto* p:{&op,&og,&of}) oldProject.targets[p->getEscapedName()]=p;
    for(auto* p:{&np,&ng,&nf}) newProject.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager source(&oldProject), dest(&newProject);
    const auto t=source.createTrack("Laser"), pb=source.addBinding(t,op), gb=source.addBinding(t,og), fb=source.addBinding(t,of);
    const auto c=source.createClip(t,"Mixed phrase",12,8);
    const auto piano=source.createLane(t,c,"Melody",ofxOceanodeTimelineLaneType::PianoRoll);
    source.addBindingToLane(t,c,piano,pb); source.addBindingToLane(t,c,piano,gb);
    auto* notes=source.getLane(t,c,piano); notes->pianoPitchBindingId=pb; notes->pianoGateBindingId=gb;
    notes->pianoNotes={{0,1,64,0.7f,0.8f}}; notes->pianoLowPitch=48; notes->pianoHighPitch=84;
    const auto group=source.createNoteGroup(t,"Instrument",pb,gb,""); source.setNoteGroupVoices(t,group,4);
    const auto curve=source.createLane(t,c,"Shape",ofxOceanodeTimelineLaneType::Curve);
    source.addBindingToLane(t,c,curve,fb);
    source.getLane(t,c,curve)->curvePoints={{0,0.2f},{4,0.9f}};
    source.getLane(t,c,curve)->curveTensions={{0.3f,0.7f}};
    const auto step=source.createLane(t,c,"Rhythm",ofxOceanodeTimelineLaneType::Step);
    source.addBindingToLane(t,c,step,gb); source.setClipStep(t,c,step,0,"1",0.5);
    source.getClip(t,c)->contentDurationBeats=4; source.getClip(t,c)->contentStretch=2;
    source.getClip(t,c)->repeatContent=true;
    const auto unused=source.createClip(t,"Not exported",99,4);
    source.createTrack("Not exported either");
    const auto sourceBefore=source.toJson();
    auto document=source.exportClip(t,c);
    assert(source.toJson()==sourceBefore && document["track"]["clips"].size()==1);
    assert(document["track"]["clips"][0]["startBeat"]==0 && document["track"]["bindings"].size()==3);
    assert(!document.contains("tempo") && !document.contains("clipGroups"));
    std::map<std::string,std::string> mapping{{pb,np.getEscapedName()},{gb,ng.getEscapedName()},{fb,nf.getEscapedName()}};
    std::string error;
    dest.beginTimelineEdit();
    auto imported=dest.importClip(document,"",3,mapping,&error);
    dest.endTimelineEdit();
    assert(error.empty() && !imported.second.empty());
    const auto* copy=dest.getClip(imported.first,imported.second);
    assert(copy && copy->lanes.size()==3 && copy->startBeat==3 && copy->durationBeats==8);
    assert(copy->contentDurationBeats==4 && copy->contentStretch==2 && copy->repeatContent);
    assert(copy->lanes[0].pianoNotes[0].pitch==64 && copy->lanes[0].pianoLowPitch==48);
    assert(copy->lanes[1].curvePoints.size()==2 && copy->lanes[1].curveTensions[0].inflection==0.3f);
    const auto* track=dest.getTrack(imported.first);
    assert(track->bindings.size()==3 && track->noteGroups.size()==1 && track->noteGroups[0].voices==4);
    for(const auto& lane:copy->lanes) for(const auto& id:lane.bindingIds) {
        const auto* b=dest.getBinding(imported.first,id); assert(b);
        assert(b->parameterPath==np.getEscapedName() || b->parameterPath==ng.getEscapedName() || b->parameterPath==nf.getEscapedName());
    }
    assert(dest.undoTimelineEdit() && dest.getTracks().empty()); assert(dest.redoTimelineEdit());
    // Import into the same track reuses targets but never reuses clip/lane ids.
    const auto again=dest.importClip(document,imported.first,5,mapping,&error);
    assert(!again.second.empty() && again.second!=imported.second);
    copy=dest.getClip(imported.first,imported.second);
    const auto* second=dest.getClip(again.first,again.second);
    for(size_t i=0;i<copy->lanes.size();++i) assert(copy->lanes[i].id!=second->lanes[i].id);
    assert(dest.getTrack(imported.first)->bindings.size()==3);
    // Existing piano clips keep their routing and voice configuration when
    // imported targets reuse an ungrouped binding on that track.
    const auto existingTrack=dest.createTrack("Existing clips");
    const auto ep=dest.addBinding(existingTrack,np), eg=dest.addBinding(existingTrack,ng);
    const auto ec=dest.createClip(existingTrack,"Keep me",0,4);
    const auto el=dest.createLane(existingTrack,ec,"Existing piano",ofxOceanodeTimelineLaneType::PianoRoll);
    auto* existingLane=dest.getLane(existingTrack,ec,el);
    existingLane->bindingIds={ep,eg}; existingLane->pianoPitchBindingId=ep; existingLane->pianoVelocityBindingId=eg;
    auto encodedExisting=[&](){
        const auto snapshot=dest.toJson();
        for(const auto& tr:snapshot["tracks"]) if(tr["id"]==existingTrack)
            for(const auto& cl:tr["clips"]) if(cl["id"]==ec) return cl;
        return ofJson();
    };
    const auto unchanged=encodedExisting();
    assert(!dest.importClip(document,existingTrack,6,mapping,&error).second.empty());
    assert(encodedExisting()==unchanged && dest.getTrack(existingTrack)->noteGroups.empty());
    // A matching name in another project never creates implicit connections.
    auto unassigned=dest.importClip(document,"",0,{},&error);
    assert(!unassigned.second.empty() && dest.getTrack(unassigned.first)->bindings.empty());
    const auto* detached=dest.getClip(unassigned.first,unassigned.second);
    assert(detached->lanes.size()==3 && detached->lanes[0].pianoNotes.size()==1);
    for(const auto& lane:detached->lanes) assert(lane.bindingIds.empty());
    // Reconnect afterwards with the normal lane target controls.
    const auto reconnect=dest.addBinding(unassigned.first,nf);
    assert(dest.addBindingToLane(unassigned.first,unassigned.second,detached->lanes[1].id,reconnect));
    // Partial remapping preserves every lane and all piano role meaning.
    auto partial=dest.importClip(document,"",0,{{pb,np.getEscapedName()}},&error);
    const auto* part=dest.getClip(partial.first,partial.second);
    assert(part->lanes.size()==3 && !part->lanes[0].pianoPitchBindingId.empty() && part->lanes[0].pianoGateBindingId.empty());
    const auto beforeFailure=dest.toJson();
    assert(dest.importClip(document,"",0,{{pb,"Unavailable"}},&error).second.empty());
    assert(!error.empty() && dest.toJson()==beforeFailure);
    assert(dest.importClip(document,"",0,{{pb,np.getEscapedName()},{gb,np.getEscapedName()}},&error).second.empty());
    assert(dest.toJson()==beforeFailure);
    auto bad=document; bad["version"]=999;
    assert(dest.importClip(bad,"",0,{},&error).second.empty() && dest.toJson()==beforeFailure);
    bad=document; bad["track"]["clips"][0]["lanes"][0]["stepCount"]="bad";
    assert(dest.importClip(bad,"",0,{},&error).second.empty() && dest.toJson()==beforeFailure);
    const auto folder=std::filesystem::temp_directory_path()/("oceanode-clips-"+ofToString(ofGetSystemTimeMicros()));
    std::filesystem::create_directories(folder);
    const auto file=folder/"phrase.oceanodeclip";
    assert(source.saveClipFile(t,c,file.string(),&error));
    auto loaded=dest.readClipFile(file.string(),&error);
    assert(!loaded.is_null() && error.empty());
    auto disk=dest.importClip(loaded,"",9,mapping,&error); assert(!disk.second.empty());
    // LFO control lanes survive; remapping changes only its final output.
    const auto lfo=source.createLfoClip(t,fb,"Movement",0,4);
    auto oscillator=dest.importClip(source.exportClip(t,lfo),"",0,{{fb,nf.getEscapedName()}},&error);
    const auto* lc=dest.getClip(oscillator.first,oscillator.second);
    assert(lc && lc->isLfo && !lc->lanes.empty() && !lc->lfoOutputBindingId.empty());
    assert(dest.getBinding(oscillator.first,lc->lfoOutputBindingId)->parameterPath==nf.getEscapedName());
    // Exported audio brings its own media copy and survives moving the bundle.
    const auto audio=folder/"sample.wav";
    {
        std::ofstream out(audio,std::ios::binary);
        auto le=[&](uint32_t n,int bytes){for(int i=0;i<bytes;++i) out.put((n>>(i*8))&255);};
        out.write("RIFF",4);le(16036,4);out.write("WAVEfmt ",8);le(16,4);le(1,2);le(1,2);
        le(8000,4);le(16000,4);le(2,2);le(16,2);out.write("data",4);le(16000,4);
        for(int i=0;i<16000;++i) out.put(0);
    }
    const auto wt=source.createWaveTrack("Audio"), wc=source.createClip(wt,"Slice",0,2);
    auto* wave=source.getClip(wt,wc);wave->waveFilePath=audio.string();wave->waveGain=0.7f;
    wave->waveReverse=true;wave->waveSourceStartBeat=0.2;wave->waveFileDurationBeats=4;
    wave->contentDurationBeats=1;wave->contentStretch=2;wave->repeatContent=false;
    const auto waveFile=folder/"slice.oceanodeclip";
    assert(source.saveClipFile(wt,wc,waveFile.string(),&error));
    const auto relocated=folder/"moved";std::filesystem::create_directories(relocated);
    std::filesystem::rename(waveFile,relocated/waveFile.filename());
    std::filesystem::rename(waveFile.string()+".media",relocated/(waveFile.filename().string()+".media"));
    std::filesystem::remove(audio);
    auto audioDocument=dest.readClipFile((relocated/waveFile.filename()).string(),&error);
    auto audioImport=dest.importClip(audioDocument,"",7,{},&error);
    const auto* w=dest.getClip(audioImport.first,audioImport.second);
    assert(w && w->waveReverse && w->waveGain==0.7f && std::abs(w->waveSourceStartBeat/w->waveFileDurationBeats-0.05)<1e-6 && w->waveNumChannels==1);
    assert(std::filesystem::exists(w->waveFilePath));
    const auto beforeMismatch=dest.toJson();
    assert(dest.importClip(document,audioImport.first,0,mapping,&error).second.empty() && dest.toJson()==beforeMismatch);
    assert(dest.readClipFile((folder/"missing").string(),&error).is_null() && !error.empty());
    assert(source.toJson()["tracks"][0]["clips"][0]==sourceBefore["tracks"][0]["clips"][0]);
    std::filesystem::remove_all(folder);
    std::cout<<"Portable clip regression passed: mixed lanes, roles, explicit/partial/unassigned remapping, unique ids, history, malformed input, LFO and relocated audio media.\n";
}
'''
if __name__ == '__main__':
    fixture.main(cases=CASES)
