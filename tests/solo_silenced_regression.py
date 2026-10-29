#!/usr/bin/env python3
"""A solo or mute set by hand wins over Toggle lanes on other tracks.

Steaming Stones: a Noise track's Toggle lane drives its own solo and a
mixer channel's Solo. Soloing another track by hand must keep Noise out
(no lane-driven solo), and a Toggle left on when its track stops being
heard must write its rest, or the mixer keeps that channel soloed.
"""
import clip_mute_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::External);
    ofParameter<float> steamValue("Steam",0,0,1), mixValue("MixSolo",0,0,1);
    ofxOceanodeParameter<float> steam, mix;
    steam.bindParameter(steamValue); mix.bindParameter(mixValue);
    for(auto* p : {&steam,&mix}) owner.targets[p->getEscapedName()]=p;
    ofxOceanodeTimelineManager model(&owner); model.setSchedulingEnabled(false);
    auto soloed=[&](const std::string& id){ return model.isTrackSoloed(*model.getTrack(id)); };
    auto frame=[&](double beat){ owner.transport->seekToBeat(beat); model.update(); model.applyAutomation(); };

    const auto st=model.createTrack("Steam");
    const auto sb=model.addBinding(st,steam);
    const auto sc=model.createClip(st,"Steam",0,16);
    const auto sl=model.createLane(st,sc,"Steam",ofxOceanodeTimelineLaneType::Number);
    model.addBindingToLane(st,sc,sl,sb); model.setClipNumber(st,sc,sl,0,0.5f);

    // Noise: one Toggle lane, on 2..6, driving its track's solo and MixSolo.
    const auto nt=model.createTrack("Noise");
    const auto nb=model.addBinding(nt,mix);
    const auto nc=model.createClip(nt,"Noise",0,16);
    const auto nl=model.createLane(nt,nc,"Noise",ofxOceanodeTimelineLaneType::MultiGate);
    model.addBindingToLane(nt,nc,nl,nb);
    auto* toggle=model.getLane(nt,nc,nl);
    toggle->multiRowCount=1; toggle->multiGateRows={{{2,4,1}}};
    model.setLaneDrivesTrackSolo(nt,nc,nl,true);
    owner.transport->play();

    // Without hand solos the lane works as before.
    frame(1); assert(!soloed(nt)); near(mixValue,0); near(steamValue,0.5);
    frame(3); assert(soloed(nt)); near(mixValue,1);
    frame(7); near(mixValue,0);

    // 1. Steam soloed by hand: Noise's lane can no longer solo Noise, so it
    //    never writes MixSolo, and Steam keeps playing through the block.
    assert(model.setTrackSolo(st,true));
    steamValue=0;
    for(double beat : {1.0, 2.5, 3.0, 4.5, 5.9, 7.0}) {
        frame(beat);
        assert(!soloed(nt)); near(mixValue,0); near(steamValue,0.5);
    }
    assert(model.setTrackSolo(st,false));

    // 2. Soloing Steam while the Noise block is on: MixSolo goes back to its
    //    rest instead of staying at 1.
    frame(3); assert(soloed(nt)); near(mixValue,1);
    assert(model.setTrackSolo(st,true));
    frame(3.5); assert(!soloed(nt)); near(mixValue,0); near(steamValue,0.5);
    frame(4); near(mixValue,0); // and stays there
    assert(model.setTrackSolo(st,false));

    // 3. Muting Noise mid-block: rest once, and a muted track can't solo
    //    itself (which would have silenced everything).
    frame(3); near(mixValue,1);
    assert(model.setTrackMuted(nt,true));
    frame(3.5); assert(!soloed(nt)); near(mixValue,0); near(steamValue,0.5);
    mixValue=0.3f; frame(4); near(mixValue,0.3); // released once, then left alone
    assert(model.setTrackMuted(nt,false));

    // 4. A muted lane or clip releases the same way.
    frame(3); near(mixValue,1);
    model.setClipMuted(nt,nc,true);
    frame(3.5); near(mixValue,0);
    model.setClipMuted(nt,nc,false);

    // 5. A clip that simply ends keeps holding its last value, as before.
    toggle->multiGateRows={{{12,4,1}}};
    frame(15.5); near(mixValue,1);
    frame(17); near(mixValue,1);
    std::cout << "Solo silenced passed: hand solos and mutes win over lane solos; silenced toggles rest once; clip ends still hold.\n";
}
'''
if __name__ == '__main__':
    fixture.fixture.main(cases=CASES)
