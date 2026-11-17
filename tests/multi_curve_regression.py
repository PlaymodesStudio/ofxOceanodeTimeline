#!/usr/bin/env python3
"""Multi Curve lanes: one curve per element of a vector target."""
import timeline_regression as fixture

CASES = fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofSeedRandom();
    ofxOceanodeContainer owner;
    owner.transport->setDriverMode(TransportDriverMode::FrameStep);
    ofParameter<std::vector<float>> output("Levels", {0, 0}, {0, 0}, {10, 10});
    ofxOceanodeParameter<std::vector<float>> target; target.bindParameter(output);
    owner.targets[target.getEscapedName()] = &target;
    ofParameter<float> single("Single", 0, 0, 10);
    ofxOceanodeParameter<float> singleTarget; singleTarget.bindParameter(single);
    owner.targets[singleTarget.getEscapedName()] = &singleTarget;
    TestTimeline model(&owner);
    const auto t = model.createTrack("Curves");
    const auto b = model.addBinding(t, target), s = model.addBinding(t, singleTarget);
    const auto c = model.createClip(t, "Ramps", 0, 4);
    const auto l = model.createLane(t, c, "Multi", ofxOceanodeTimelineLaneType::MultiCurve);
    model.addBindingToLane(t, c, l, b);
    model.addBindingToLane(t, c, l, s);
    auto near = [](float a, float e) {
        if(std::abs(a - e) > 1e-3f) std::cerr << "expected " << e << " got " << a << "\n";
        assert(std::abs(a - e) <= 1e-3f);
    };
    int checks = 0;
    auto evaluate = [&](double beat, std::vector<float> expected) {
        owner.transport->seekToBeat(beat);
        model.update(); model.applyAutomation();
        const auto values = output.get();
        assert(values.size() == expected.size());
        for(size_t i = 0; i < expected.size(); ++i) {
            if(std::abs(values[i] - expected[i]) > 1e-3f) std::cerr << "Beat " << beat << ", curve " << i << " (check " << checks << ")\n";
            near(values[i], expected[i]);
        }
        ++checks;
        near(single.get(), expected.front()); // a single value takes the first curve
    };
    {
        auto& lane = *model.getLane(t, c, l);
        assert(lane.multiCurves.size() == 2); // a new lane has two curves
        assert(lane.multiCurves[0].id != lane.multiCurves[1].id);
        lane.valueMin = 0; lane.valueMax = 10;
        lane.curveInterpolation = "Linear";
        lane.multiCurves[0].curvePoints = {{0, 0}, {4, 1}};
        lane.multiCurves[1].curvePoints = {{0, 1}, {4, 0}};
        lane.multiCurves[1].name = "Depth";
        lane.multiCurveStacked = true;
        ofxOceanodeTimelineSyncMultiCurves(lane);
    }
    evaluate(1.0, {2.5f, 7.5f});
    evaluate(3.0, {7.5f, 2.5f});

    // The curves are saved and loaded.
    { const auto saved = model.toJson(); model.fromJson(saved); }
    {
        const auto& lane = *model.getLane(t, c, l);
        assert(lane.type == ofxOceanodeTimelineLaneType::MultiCurve && lane.multiCurves.size() == 2);
        assert(lane.multiCurves[1].curvePoints.size() == 2 && lane.multiCurves[1].curvePoints.front().value == 1.0f);
        assert(lane.multiCurves[1].curveInterpolation == "Linear");
        assert(lane.multiCurves[0].name.empty() && lane.multiCurves[1].name == "Depth" && lane.multiCurveStacked);
    }
    evaluate(1.0, {2.5f, 7.5f});

    // A third curve is one more element.
    ofxOceanodeTimelineSetMultiCurveCount(*model.getLane(t, c, l), 3, 4.0);
    model.getLane(t, c, l)->multiCurves[2].curvePoints = {{0, 0.5f}, {4, 0.5f}};
    evaluate(1.0, {2.5f, 7.5f, 5.0f});

    // The end of the clip writes every curve's last value.
    owner.transport->seekToBeat(3.99); owner.transport->play(); model.update();
    owner.transport->advanceFrameStep(0.02); model.update();
    near(output.get()[0], 10.0f); near(output.get()[1], 0.0f); near(output.get()[2], 5.0f);
    owner.transport->pause();

    // Reversed, each curve runs backwards.
    assert(model.reverseClip(t, c));
    evaluate(1.0, {7.5f, 2.5f, 5.0f});
    assert(model.reverseClip(t, c));
    evaluate(1.0, {2.5f, 7.5f, 5.0f});

    // Split: both parts keep every curve where it was.
    const auto right = model.splitClip(t, c, 2.0);
    assert(!right.empty());
    evaluate(1.0, {2.5f, 7.5f, 5.0f});
    evaluate(3.0, {7.5f, 2.5f, 5.0f});
    const auto rightLane = model.getClip(t, right)->lanes.front();
    assert(rightLane.multiCurves.size() == 3 && rightLane.multiCurves[0].curvePoints.front().beat == 0.0);
    near(rightLane.multiCurves[0].curvePoints.front().value, 0.5f);

    // Merged again, the curves are joined one by one.
    const auto merged = model.mergeClips(t, c, right);
    assert(!merged.empty());
    assert(model.getClip(t, merged)->lanes.size() == 1);
    assert(model.getClip(t, merged)->lanes.front().multiCurves.size() == 3);
    evaluate(1.0, {2.5f, 7.5f, 5.0f});
    evaluate(3.0, {7.5f, 2.5f, 5.0f});

    const auto mergedLane = model.getClip(t, merged)->lanes.front().id;

    // To a Curve lane and back: the first curve is the Curve lane's.
    assert(model.setClipLaneType(t, merged, mergedLane, ofxOceanodeTimelineLaneType::Curve));
    {
        auto& lane = *model.getLane(t, merged, mergedLane);
        assert(!lane.curvePoints.empty() && lane.curvePoints.front().value == 0.0f);
        lane.curveInterpolation = "Linear";
        lane.curvePoints = {{0, 1}, {4, 1}};
    }
    assert(model.setClipLaneType(t, merged, mergedLane, ofxOceanodeTimelineLaneType::MultiCurve));
    {
        const auto& lane = *model.getLane(t, merged, mergedLane);
        assert(lane.multiCurves.size() == 3 && lane.curvePoints.empty());
        assert(lane.multiCurves[0].curvePoints.size() == 2);
    }
    evaluate(1.0, {10.0f, 7.5f, 5.0f});

    // A lane made a Multi Curve while it drives a vector gets a curve per element.
    {
        const auto c2 = model.createClip(t, "Fit", 8, 4);
        const auto l2 = model.createLane(t, c2, "Curve", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(t, c2, l2, b);
        output = std::vector<float>{0, 0, 0, 0, 0};
        assert(model.setClipLaneType(t, c2, l2, ofxOceanodeTimelineLaneType::MultiCurve));
        assert(model.getLane(t, c2, l2)->multiCurves.size() == 5);
        assert(model.getLane(t, c2, l2)->multiCurves[0].curvePoints.size() == 2); // the Curve lane's ramp

        // Extending the start moves every curve's points with the content.
        auto& lane = *model.getLane(t, c2, l2);
        lane.curveInterpolation = "Linear";
        ofxOceanodeTimelineSyncMultiCurves(lane);
        assert(model.extendClipStart(t, c2, 7.0));
        for(const auto& curve : model.getLane(t, c2, l2)->multiCurves) {
            assert(curve.curvePoints.front().beat == 1.0 && curve.curvePoints.back().beat == 5.0);
        }
        // Consolidating a shortened clip keeps each curve's value at the new end.
        auto& clip = *model.getClip(t, c2);
        const double shortened = clip.durationBeats - 2.0;
        assert(model.setClipTiming(t, c2, clip.startBeat, shortened));
        model.getClip(t, c2)->repeatContent = true;
        assert(model.consolidateClipContent(t, c2));
        for(const auto& curve : model.getLane(t, c2, l2)->multiCurves) {
            near(static_cast<float>(curve.curvePoints.back().beat), 3.0f);
            near(curve.curvePoints.back().value, 0.5f);
            assert(curve.curveTensions.size() + 1 == curve.curvePoints.size());
        }
    }

    // Two points on one beat are a vertical step: the earlier point's value
    // before the beat, the later one's from it on -- kept through save/load,
    // split and reverse.
    {
        const auto t2 = model.createTrack("Step"), b2 = model.addBinding(t2, singleTarget);
        const auto c3 = model.createClip(t2, "Jump", 20, 4);
        const auto l3 = model.createLane(t2, c3, "Curve", ofxOceanodeTimelineLaneType::Curve);
        model.addBindingToLane(t2, c3, l3, b2);
        {
            auto& lane = *model.getLane(t2, c3, l3);
            lane.valueMin = 0; lane.valueMax = 10; lane.curveInterpolation = "Linear";
            lane.curvePoints = {{0, 0}, {2, 0.2f}, {2, 0.9f}, {4, 1}};
            lane.curveTensions.assign(3, {});
        }
        auto at = [&](double beat, float expected) {
            owner.transport->seekToBeat(beat); model.update(); model.applyAutomation();
            near(single.get(), expected);
        };
        auto jump = [&]() { at(21.0, 1.0f); at(21.99, 1.99f); at(22.0, 9.0f); at(22.5, 9.25f); at(23.0, 9.5f); };
        jump();
        { const auto saved = model.toJson(); model.fromJson(saved); }
        const auto loaded = model.getLane(t2, c3, l3)->curvePoints;
        assert(loaded.size() == 4 && loaded[1].value == 0.2f && loaded[2].value == 0.9f);
        jump();
        assert(model.reverseClip(t2, c3));
        at(21.5, 9.25f); at(22.0, 2.0f); at(23.0, 1.0f);
        assert(model.reverseClip(t2, c3));
        jump();
        const auto right2 = model.splitClip(t2, c3, 23.0);
        assert(!right2.empty());
        jump();

        // Playing, the frame before the step already writes the value it steps
        // to: an event scheduled on the step's beat finds it there.
        owner.transport->seekToBeat(21.9); owner.transport->play(); model.update();
        bool early = false;
        for(int frame = 0; frame < 40 && owner.getTransportState().beatPosition < 22.0; ++frame) {
            owner.transport->advanceFrameStep(1.0 / 60.0); model.update();
            const double beat = owner.getTransportState().beatPosition;
            if(beat < 22.0 && single.get() >= 9.0f) early = true;
            if(beat < 21.9) assert(single.get() < 2.0f);
        }
        assert(early);
        owner.transport->pause();
    }
    std::cout << "Multi Curve passed: vector and single targets, save/load, curve count, clip end, "
                 "reverse, split, merge, type changes, fitting to the target, extend, consolidate and same-beat points.\n";
}
'''

if __name__ == '__main__':
    fixture.main(cases=CASES)
