#!/usr/bin/env python3
"""All clips retain their lane rows; folded rows render time-aligned miniatures."""
import controller_clarity_regression as controller

native = controller.native
native.fixture.CASES = native.fixture.CASES.split('int main()')[0] + r'''
int main() {
    ofDisableDataPath(); ofSeedRandom();
    auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry = std::make_shared<ofxOceanodeNodeRegistry>();
    auto types = std::make_shared<ofxOceanodeTypesRegistry>();
    auto owner = std::make_shared<ofxOceanodeContainer>(registry, types);
    owner->setCanvasID("Canvas");
    auto& model = ofxOceanodeTimeline::attach(*owner);
    model.setEditTarget(model.addTimeline("", "Timeline 1"));
    auto& node = owner->createNode(std::make_unique<ClarityProbe>(), 1);
    auto& parameter = static_cast<ofxOceanodeAbstractParameter&>(node.getParameters().get("Cutoff"));
    const auto track = model.createTrack("Filter");
    model.getTrack(track)->color = ofColor(210, 110, 35);
    const auto binding = model.addBinding(track, parameter);
    const auto first = model.createClip(track, "First", 0, 4);
    const auto a = model.createLane(track, first, "Rise", ofxOceanodeTimelineLaneType::Curve);
    const auto b = model.createLane(track, first, "Fall", ofxOceanodeTimelineLaneType::Curve);
    const auto second = model.createClip(track, "Second", 4, 4);
    const auto c = model.createLane(track, second, "Rise", ofxOceanodeTimelineLaneType::Curve);
    for(const auto& pair : {std::make_pair(first, a), std::make_pair(first, b), std::make_pair(second, c)})
        model.addBindingToLane(track, pair.first, pair.second, binding);
    model.getLane(track, first, b)->curvePoints = {{0, 1}, {4, 0}};
    ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(1200, 900); io.DeltaTime = 1.f / 60;
    unsigned char* atlas; int aw, ah; io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    ofxOceanodeTimelineController ui(owner);
    ui.shownRef = model.getEditTarget(); ui.shownSequence = &model.editSequence();
    ui.editorState.editorTrackId = track; ui.editorState.editorClipId = second;
    ui.editorState.editorLaneId = c; ui.editorState.clipEditorOpen = true;
    for(const auto& lane : {a, b, c}) ui.editorState.laneEditorHeights[lane] = 120;
    auto frame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false, const char* shot = nullptr) {
        io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1150, 800));
        ImGui::Begin("Lane view", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, 80));
        ui.drawLaneEditor(model, *model.getTrack(track), 1100, 16, 2);
        ImGui::End(); ImGui::Render();
        if(shot) capture(shot, atlas, aw, ah);
    };
    frame(); frame();
    // Chance is editable on an unfocused lane and stays separate from its sibling.
    ImGuiWindow* chancePanel=nullptr;
    for(auto* window:ImGui::GetCurrentContext()->Windows)
        if(window->Active && std::string(window->Name).find("##clipProperties"+track+first+a)!=std::string::npos)
            chancePanel=window;
    assert(chancePanel);
    const float clipRight=ImGui::GetStyle().WindowPadding.x+ofxOceanodeTimelineEditorUi::labelWidth+280.0f;
    const ImVec2 chanceAt(clipRight-15.0f,80.0f+6.0f+ImGui::GetTextLineHeight()*0.5f);
    frame(chanceAt); frame(chanceAt,true);
    frame(ImVec2(chanceAt.x,chanceAt.y+75),true); frame(ImVec2(chanceAt.x,chanceAt.y+75));
    assert(std::abs(model.getLane(track,first,a)->chance-0.5f)<0.001f);
    assert(model.getLane(track,first,b)->chance==1.0f);
    assert(ui.editorState.editorLaneId==c); // chance need not focus the lane
    frame(ImVec2(-100,-100),false,"lane-chance");
    model.setLaneChance(track,first,a,1.0f); frame();
    for(int i=0;i<ImGui::GetCurrentContext()->TabBars.GetMapSize();++i)
        if(auto* bar=ImGui::GetCurrentContext()->TabBars.TryGetMapData(i))
            for(auto& tab:bar->Tabs) assert(std::string(ImGui::TabBarGetTabName(bar,&tab))!="Clip");
    const auto saved = model.toJson();
    auto panels = [&]() {
        std::vector<const ImGuiWindow*> panels;
        for(const auto* window : ImGui::GetCurrentContext()->Windows)
            if(window->Active && std::string(window->Name).find("##clipProperties" + track) != std::string::npos)
                panels.push_back(window);
        return panels;
    };
    assert(panels().size() == 3 && ui.editorState.editorClipId == second);
    frame(ImVec2(-100, -100), false, "all-lanes");
    ui.editorState.editorClipId = first; ui.editorState.editorLaneId = a; frame();
    assert(panels().size() == 3); // selecting the other clip does not hide any lane
    for(const auto& lane : {a, b, c}) ui.editorState.collapsedLaneIds.insert(lane);
    frame(); frame(ImVec2(-100, -100), false, "folded-lanes");
    assert(panels().empty() && model.toJson() == saved);
    // The collapsed lane exposes the same chance circle.
    for(int i=0;i<25;++i) frame();
    const ImVec2 foldedChance(clipRight-15.0f,91);
    frame(foldedChance); frame(foldedChance,true);
    frame(ImVec2(foldedChance.x,foldedChance.y+75),true); frame(ImVec2(foldedChance.x,foldedChance.y+75));
    assert(std::abs(model.getLane(track,first,a)->chance-0.5f)<0.001f);
    model.setLaneChance(track,first,a,1.0f); frame();
    // White miniature curve lines span each clip's own X interval in all 3 rows.
    for(int row = 0; row < 3; ++row) {
        const float top = 80 + row * (22 + ImGui::GetStyle().ItemSpacing.y);
        float minX = 1e9f, maxX = -1e9f;
        for(const auto* list : ImGui::GetDrawData()->CmdLists) for(const auto& vertex : list->VtxBuffer)
            if(vertex.col == IM_COL32(210, 110, 35, 200) && vertex.pos.y >= top && vertex.pos.y < top + 22) {
                minX = std::min(minX, vertex.pos.x); maxX = std::max(maxX, vertex.pos.x);
            }
        const float expectedX = ImGui::GetStyle().WindowPadding.x + ofxOceanodeTimelineEditorUi::labelWidth + (row == 2 ? 280 : 0);
        assert(std::abs(minX - expectedX) < 2 && std::abs(maxX - expectedX - 280) < 2);
    }
    const ImVec2 preview(ImGui::GetStyle().WindowPadding.x + ofxOceanodeTimelineEditorUi::labelWidth + 400,
                         80 + 2 * (22 + ImGui::GetStyle().ItemSpacing.y) + 11);
    frame(preview); frame(preview, true); frame(preview);
    assert(ui.editorState.editorClipId == second && ui.editorState.editorLaneId == c);
    assert(model.toJson() == saved);

    const auto other = model.createTrack("Another track");
    model.getTrack(other)->color = ofColor(35, 150, 210);
    const auto otherBinding = model.addBinding(other, parameter);
    const auto otherClip = model.createClip(other, "Later", 8, 8);
    const auto d = model.createLane(other, otherClip, "Ramp", ofxOceanodeTimelineLaneType::Curve);
    model.addBindingToLane(other, otherClip, d, otherBinding);
    ui.editorState.collapsedLaneIds.insert(d);
    auto fullFrame = [&](ImVec2 mouse = ImVec2(-100, -100), bool down = false, const char* shot = nullptr) {
        io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame(); ui.drawBeforeWindows();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1150, 800));
        ImGui::Begin("Track view", nullptr, ImGuiWindowFlags_NoDecoration);
        ui.draw(); ImGui::End(); ImGui::Render();
        if(shot) capture(shot, atlas, aw, ah);
    };
    fullFrame(); fullFrame();
    auto arrow = [&](const std::string& trackId) {
        const auto row = std::find_if(ui.trackHeaderRows.begin(), ui.trackHeaderRows.end(),
            [&](const auto& row) { return row.trackId == trackId; });
        assert(row != ui.trackHeaderRows.end());
        return ImVec2(ui.fileDropBounds.x - ofxOceanodeTimelineEditorUi::labelWidth + 9, (row->minY + row->maxY) * 0.5f);
    };
    auto click = [&](ImVec2 point) { fullFrame(point); fullFrame(point, true); fullFrame(point); };
    click(arrow(other));
    assert(ui.scopeEditorOnTrack(track) && ui.scopeEditorOnTrack(other));
    click(arrow(track));
    assert(!ui.scopeEditorOnTrack(track) && ui.scopeEditorOnTrack(other));
    click(arrow(track));
    assert(ui.scopeEditorOnTrack(track) && ui.scopeEditorOnTrack(other));
    for(const auto& lane : {a, b, c}) ui.editorState.collapsedLaneIds.insert(lane);
    fullFrame(ImVec2(-100, -100), false, "multiple-tracks");
    ui.editorState.editorTrackId = other; ui.editorState.editorClipId = otherClip; ui.editorState.editorLaneId = d;
    ui.selectedClips = {{other, otherClip}}; fullFrame();
    assert(ui.editorScreenSpans.size() == 2);
    const ImVec2 firstPreview(ui.fileDropBounds.x + 70, ui.editorScreenSpans.front().first + 21);
    click(firstPreview);
    if(!ui.lastClickInEditor || ui.selectedClips.count({other, otherClip}) != 1) {
        std::cerr << "Preview click: " << firstPreview.x << "," << firstPreview.y
                  << " lastClick=" << ui.lastClickInEditor << " selection=" << ui.selectedClips.size()
                  << " focused=" << ui.editorState.editorTrackId << "/" << ui.editorState.editorLaneId << " spans=";
        for(const auto& span : ui.editorScreenSpans) std::cerr << " " << span.first << ".." << span.second;
        std::cerr << "\n";
    }
    assert(ui.lastClickInEditor && ui.selectedClips.count({other, otherClip}) == 1);
    assert(ui.editorState.editorTrackId == track && ui.editorState.editorLaneId == a);
    assert(ui.scopeEditorOnTrack(track) && ui.scopeEditorOnTrack(other));

    auto clipPoint = [&](const std::string& clipId) {
        const auto rect = std::find_if(ui.clipScreenRects.begin(), ui.clipScreenRects.end(),
            [&](const auto& rect) { return rect.trackId == track && rect.clipId == clipId; });
        assert(rect != ui.clipScreenRects.end());
        return ImVec2((rect->x1 + rect->x2) * 0.5f, (rect->y1 + rect->y2) * 0.5f);
    };
    auto folded = [&](const std::string& id) { return ui.editorState.collapsedLaneIds.count(id) > 0; };
    const bool otherFolded = folded(d);
    click(clipPoint(first));
    assert(!folded(a) && !folded(b) && folded(c) && folded(d) == otherFolded);
    click(clipPoint(second));
    assert(folded(a) && folded(b) && !folded(c) && folded(d) == otherFolded);
    auto doubleClick = [&](const std::string& clipId) {
        const auto point = clipPoint(clipId);
        for(int i = 0; i < 25; ++i) fullFrame(point); // separate double-click gestures
        click(point); click(point);
        assert(ui.scopeEditorOnTrack(other) && folded(d) == otherFolded);
    };
    doubleClick(second); assert(!ui.scopeEditorOnTrack(track)); // same clip closes the track
    doubleClick(first);
    assert(ui.scopeEditorOnTrack(track) && !folded(a) && !folded(b) && folded(c));
    doubleClick(second); assert(!ui.scopeEditorOnTrack(track)); // another clip closes it too
    doubleClick(second);
    assert(ui.scopeEditorOnTrack(track) && folded(a) && folded(b) && !folded(c));
    doubleClick(first); assert(!ui.scopeEditorOnTrack(track));
    doubleClick(first);
    assert(ui.scopeEditorOnTrack(track) && !folded(a) && !folded(b) && folded(c));
    doubleClick(first); assert(!ui.scopeEditorOnTrack(track));
    doubleClick(second);
    assert(ui.scopeEditorOnTrack(track) && folded(a) && folded(b) && !folded(c));
    fullFrame(ImVec2(-100, -100), false, "clip-focus-folding");

    // A vertical trackpad swipe stops at a view of [0, 2 * last clip end].
    // The last clip is on the other track; changing its length updates the limit.
    ui.followPlayhead = false;
    auto& view = model.getViewState(); view.pixelsPerSecond = 500; view.scrollX = 200;
    fullFrame();
    const ImVec2 zoomPoint(ui.fileDropBounds.x + 400, ui.fileDropBounds.y + 5);
    for(int i = 0; i < 20; ++i) fullFrame(zoomPoint);
    io.AddMouseWheelEvent(0, -100); fullFrame(zoomPoint);
    const float width = ImGui::FindWindowByName("Track view")->WorkRect.GetWidth() - ofxOceanodeTimelineEditorUi::labelWidth;
    const float minimum = width / model.beatToSeconds(32, 120);
    assert(std::abs(view.pixelsPerSecond - minimum) < 0.001f && view.scrollX == 0);
    io.AddMouseWheelEvent(0, -100); fullFrame(zoomPoint);
    assert(std::abs(view.pixelsPerSecond - minimum) < 0.001f && view.scrollX == 0);
    model.setClipTiming(other, otherClip, 8, 12);
    fullFrame(zoomPoint); io.AddMouseWheelEvent(0, -100); fullFrame(zoomPoint);
    assert(std::abs(view.pixelsPerSecond - width / model.beatToSeconds(40, 120)) < 0.001f);
    model.getBpmAutomationPoints() = {{0, 60}, {20, 180}};
    model.setBpmInterpolation("Linear"); model.setBpmAutomationEnabled(true);
    fullFrame(zoomPoint); io.AddMouseWheelEvent(0, -100); fullFrame(zoomPoint);
    assert(std::abs(model.beatToSeconds(20, 120) * view.pixelsPerSecond - width * 0.5f) < 0.01f);
    model.setBpmAutomationEnabled(false);
    // A short clip on another timeline has its own limit, even when the scale
    // needed to fit it is above the ordinary maximum zoom-in setting.
    const auto shortRef = model.addTimeline("", "Short");
    model.setEditTarget(shortRef); ui.enterTimelineScope(model, shortRef);
    const auto shortTrack = model.createTrack("Short");
    model.createClip(shortTrack, "Half beat", 0, 0.5);
    fullFrame();
    const ImVec2 shortZoom(ui.fileDropBounds.x + 400, ui.fileDropBounds.y + 5);
    for(int i = 0; i < 20; ++i) fullFrame(shortZoom);
    io.AddMouseWheelEvent(0, -100); fullFrame(shortZoom);
    assert(std::abs(model.getViewState().pixelsPerSecond - width / model.beatToSeconds(1, 120)) < 0.001f);
    assert(model.getViewState().scrollX == 0);
    ImGui::DestroyContext();
    std::cout << "Lane view passed: three separate rows across two clips, focus switches, "
                 "colored folded miniatures, time alignment, independent track expansion, "
                 "clip-selection folding, focus routing and content-based zoom-out limits.\n";
}
'''

if __name__ == '__main__':
    sources = [str(p) for p in (native.ADDON / 'src').rglob('*.cpp')]
    sources += ['src/Managers/ofxOceanodeScheduling.cpp', 'src/Controls/ofxOceanodeBaseController.cpp']
    includes = [native.ADDON / 'src', *sorted({p.parent for p in (native.ADDON / 'src').rglob('*.h')})]
    native.fixture.main(extra_sources=sources, extra_includes=includes)
