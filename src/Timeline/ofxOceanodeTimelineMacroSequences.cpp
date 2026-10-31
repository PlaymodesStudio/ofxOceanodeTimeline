// Macro timelines: the sequence each macro owns, where it is saved and who
// shares it (copies of a library macro share one).

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "Nodes/ofxOceanodeNodeMacro.h"

#include <filesystem>

namespace {
constexpr const char* kSequenceFile = "sequence.json";

// The macro node whose container is `target`, searched from `from` down.
ofxOceanodeNodeMacro* owningMacro(ofxOceanodeContainer& from, const ofxOceanodeContainer& target) {
    for(auto* node : from.getAllModules()) {
        auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
        if(macro == nullptr || !macro->getContainer()) continue;
        if(macro->getContainer().get() == &target) return macro;
        if(auto* inner = owningMacro(*macro->getContainer(), target)) return inner;
    }
    return nullptr;
}

// One spelling per library folder, however the core wrote the path.
std::string libraryKey(const std::string& folder) {
    if(folder.empty()) return std::string();
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(ofToDataPath(folder, true)), error);
    return error ? folder : resolved.string();
}
} // namespace

void ofxOceanodeTimelineManager::pruneMacroSequences() const {
    macroSequences.erase(std::remove_if(macroSequences.begin(), macroSequences.end(),
        [](const auto& entry) { return entry.container.expired(); }), macroSequences.end());
    // A shared sequence resolves through a live copy (the first one may be gone).
    for(auto& entry : macroSequences) {
        entry.sequence->base = const_cast<ofxOceanodeContainer*>(entry.address);
        for(auto& timeline : entry.sequence->timelines) timeline->base = entry.sequence->base;
    }
    for(auto it = sequenceHistories.begin(); it != sequenceHistories.end();) {
        const bool live = std::any_of(macroSequences.begin(), macroSequences.end(),
            [&](const auto& entry) { return entry.sequence.get() == it->first; });
        if(live) ++it; else it = sequenceHistories.erase(it);
    }
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::getMacroSequence(ofxOceanodeContainer& macroContainer) {
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        if(entry.address == &macroContainer) return entry.sequence.get();
    if(container == nullptr) return nullptr;
    auto* macro = owningMacro(*container, macroContainer);
    if(macro == nullptr) return nullptr;
    MacroSequenceEntry entry;
    entry.container = macro->getContainer();
    entry.address = &macroContainer;
    entry.libraryFolder = macro->isLocal() ? std::string() : libraryKey(macro->getCurrentMacroPath());
    // Another copy of the same library macro already has it.
    if(!entry.libraryFolder.empty())
        for(const auto& other : macroSequences)
            if(other.libraryFolder == entry.libraryFolder) entry.sequence = other.sequence;
    if(!entry.sequence) {
        entry.sequence = std::make_shared<ofxOceanodeTimelineSequence>();
        entry.sequence->base = &macroContainer;
    }
    macroSequences.push_back(entry);
    return entry.sequence.get();
}

std::string ofxOceanodeTimelineManager::getMacroSequenceLabel(ofxOceanodeContainer& macroContainer) const {
    if(container != nullptr)
        if(auto* macro = owningMacro(*container, macroContainer)) {
            const auto title = macro->getNodeTitleSuffix();
            if(!title.empty()) return title;
        }
    const auto canvas = macroContainer.getCanvasID();
    const auto last = canvas.rfind(" / ");
    return last == std::string::npos ? canvas : canvas.substr(last + 3);
}

void ofxOceanodeTimelineManager::loadMacroSequence(ofxOceanodeContainer& macroContainer, const std::string& folder) {
    if(container == nullptr) return;
    auto* macro = owningMacro(*container, macroContainer);
    if(macro == nullptr) return; // not a macro's container (e.g. a mixer graph)
    pruneMacroSequences();
    const std::string library = macro->isLocal() ? std::string() : libraryKey(folder);
    // This container's previous sequence (a reload, or a switch between local and library).
    macroSequences.erase(std::remove_if(macroSequences.begin(), macroSequences.end(),
        [&](const auto& entry) { return entry.address == &macroContainer; }), macroSequences.end());
    MacroSequenceEntry entry;
    entry.container = macro->getContainer();
    entry.address = &macroContainer;
    entry.libraryFolder = library;
    // A library definition already in memory is shared as it is (with any edits not yet saved).
    if(!library.empty())
        for(const auto& other : macroSequences)
            if(other.libraryFolder == library) entry.sequence = other.sequence;
    if(!entry.sequence) {
        entry.sequence = std::make_shared<ofxOceanodeTimelineSequence>();
        entry.sequence->base = &macroContainer;
        const std::string file = ofFilePath::join(folder, kSequenceFile);
        if(ofFile::doesFileExist(file)) sequenceFromJson(*entry.sequence, ofLoadJson(file));
    }
    macroSequences.push_back(entry);
}

bool ofxOceanodeTimelineManager::saveMacroSequence(const ofxOceanodeContainer& macroContainer, const std::string& folder) const {
    pruneMacroSequences();
    const auto found = std::find_if(macroSequences.begin(), macroSequences.end(),
        [&](const auto& entry) { return entry.address == &macroContainer; });
    const std::string file = ofFilePath::join(folder, kSequenceFile);
    // Nothing to keep: no file, so an emptied timeline does not come back.
    if(found == macroSequences.end() ||
       (found->sequence->tracks.empty() && found->sequence->timelines.empty() &&
        found->sequence->play == ofxOceanodeTimelinePlaySettings())) {
        if(ofFile::doesFileExist(file)) ofFile::removeFile(file);
        return true;
    }
    return ofSavePrettyJson(file, sequenceToJson(*found->sequence));
}

bool ofxOceanodeTimelineManager::isLiveSequence(const ofxOceanodeTimelineSequence& sequence) const {
    if(isMainSequence(sequence)) return true;
    TimelineRef ref;
    return refOf(sequence, ref); // a canvas's default or further timeline
}

namespace {
// The macro container with this canvas id, searched from `from` down.
ofxOceanodeContainer* containerWithCanvas(ofxOceanodeContainer& from, const std::string& canvasID) {
    for(auto* node : from.getAllModules()) {
        auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
        if(macro == nullptr || !macro->getContainer()) continue;
        auto& inner = *macro->getContainer();
        if(inner.getCanvasID() == canvasID) return &inner;
        if(canvasID.rfind(inner.getCanvasID() + " / ", 0) == 0)
            if(auto* found = containerWithCanvas(inner, canvasID)) return found;
    }
    return nullptr;
}

bool isRootCanvas(const std::string& canvasID) { return canvasID.empty() || canvasID == "Canvas" || canvasID == "0"; }
} // namespace

std::string ofxOceanodeTimelineManager::createMacroTrack(ofxOceanodeTimelineSequence& sequence,
                                                         ofxOceanodeContainer& macroContainer) {
    // The macro's canvas id relative to the sequence's container; only a
    // macro inside it (a sequence never reaches outside its own macro).
    const auto* base = resolverFor(sequence).base;
    const std::string canvas = macroContainer.getCanvasID();
    std::string relative = canvas;
    if(base != nullptr && !isRootCanvas(base->getCanvasID())) {
        const std::string prefix = base->getCanvasID() + " / ";
        if(canvas.rfind(prefix, 0) != 0) return std::string();
        relative = canvas.substr(prefix.size());
    }
    if(isRootCanvas(relative) || getMacroSequence(macroContainer) == nullptr) return std::string();
    const auto id = createTrack(sequence, getMacroSequenceLabel(macroContainer));
    getTrack(sequence, id)->macroCanvas = relative;
    invalidateEvaluationIndexes();
    return id;
}

ofxOceanodeContainer* ofxOceanodeTimelineManager::findMacroContainer(const ofxOceanodeTimelineSequence& sequence,
                                                                     const ofxOceanodeTimelineTrack& track) const {
    if(container == nullptr || !track.isMacroTrack()) return nullptr;
    const auto* base = sequence.base != nullptr ? sequence.base : container;
    if(track.macroCanvas == ".") return const_cast<ofxOceanodeContainer*>(base); // another timeline of its own canvas
    const std::string canvas = isRootCanvas(base->getCanvasID()) ? track.macroCanvas
                                                                 : base->getCanvasID() + " / " + track.macroCanvas;
    return containerWithCanvas(*container, canvas);
}

std::string ofxOceanodeTimelineManager::createMacroClip(ofxOceanodeTimelineSequence& sequence, const std::string& trackId,
                                                        double startBeat, double durationBeats) {
    const auto* track = getTrack(sequence, trackId);
    if(track == nullptr || !track->isMacroTrack()) return std::string();
    auto* macroSequence = placedTimeline(sequence, *track);
    const double length = macroSequence == nullptr ? getBeatsPerBar() : macroTimelineLength(*macroSequence);
    const auto id = createClip(sequence, trackId, track->name, startBeat, durationBeats > 0.0 ? durationBeats : length);
    if(auto* clip = getClip(sequence, trackId, id)) {
        clip->contentDurationBeats = length;
        clip->contentStretch = 1.0;
        clip->repeatContent = true;
    }
    invalidateEvaluationIndexes();
    return id;
}

std::string ofxOceanodeTimelineManager::createMacroTrack(ofxOceanodeContainer& macroContainer) {
    return createMacroTrack(mainSequence, macroContainer);
}

std::string ofxOceanodeTimelineManager::createMacroClip(const std::string& trackId, double startBeat, double durationBeats) {
    return createMacroClip(mainSequence, trackId, startBeat, durationBeats);
}

ofxOceanodeAbstractParameter* ofxOceanodeTimelineManager::macroNodeParameter(const ofxOceanodeContainer& macroContainer,
                                                                           const std::string& name) const {
    if(container == nullptr) return nullptr;
    auto* macro = owningMacro(*container, macroContainer);
    if(macro == nullptr || !macro->getParameterGroup().contains(name)) return nullptr;
    return dynamic_cast<ofxOceanodeAbstractParameter*>(&macro->getParameterGroup().get(name));
}

namespace {
// An absolute path (or canvas id) as seen from inside the canvas `base`:
// empty when it is not inside it.
std::string relativeTo(const std::string& base, const std::string& path) {
    if(isRootCanvas(base)) return path;
    if(path.rfind(base + "::", 0) == 0) return path.substr(base.size() + 2);
    if(path.rfind(base + " / ", 0) == 0) return path.substr(base.size() + 3);
    return std::string();
}
} // namespace

int ofxOceanodeTimelineManager::retargetAllParameterPaths(const std::map<std::string, std::string>& oldToNew) {
    int changed = retargetParameterPaths(mainSequence, oldToNew);
    pruneMacroSequences();
    // Macro timelines hold paths relative to their macro.
    for(const auto& entry : macroSequences) {
        const std::string base = entry.address->getCanvasID();
        std::map<std::string, std::string> relative;
        for(const auto& [from, to] : oldToNew) {
            const auto relativeFrom = relativeTo(base, from), relativeNew = relativeTo(base, to);
            if(!relativeFrom.empty() && !relativeNew.empty()) relative[relativeFrom] = relativeNew;
        }
        if(!relative.empty()) changed += retargetParameterPaths(*entry.sequence, relative);
    }
    return changed;
}

void ofxOceanodeTimelineManager::collectBoundParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const {
    if(container == nullptr) return;
    for(const auto& track : mainSequence.tracks)
        for(const auto& binding : track.bindings)
            if(auto* parameter = container->findTimelineParameter(binding.parameterPath)) result.insert(parameter);
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        for(const auto& track : entry.sequence->tracks)
            for(const auto& binding : track.bindings)
                if(auto* parameter = entry.address->findTimelineParameter(binding.parameterPath)) result.insert(parameter);
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::sequenceForCanvas(ofxOceanodeContainer& canvas) {
    if(&canvas == container || isRootCanvas(canvas.getCanvasID())) return &mainSequence;
    return getMacroSequence(canvas);
}

ofJson ofxOceanodeTimelineManager::moveTracksIntoMacro(ofxOceanodeTimelineSequence& source, ofxOceanodeTimelineSequence& target,
                                                       ofxOceanodeContainer& macroContainer, const std::string& relativeCanvas,
                                                       const std::set<std::string>* onlyIds, const std::string& macroTrackId) {
    const std::string& macro = relativeCanvas;
    auto inside = [&](const std::string& path) {
        return path.rfind(macro + "::", 0) == 0 || path.rfind(macro + " / ", 0) == 0;
    };
    auto strip = [&](const std::string& path) {
        return path.rfind(macro + "::", 0) == 0 ? path.substr(macro.size() + 2) : path.substr(macro.size() + 3);
    };
    // Tracks driving only what is inside the macro (a macro track: a macro inside it).
    std::vector<size_t> moving;
    for(size_t i = 0; i < source.tracks.size(); ++i) {
        const auto& track = source.tracks[i];
        if(track.isFolder || track.isWaveTrack) continue;
        if(onlyIds != nullptr && onlyIds->count(track.id) == 0) continue;
        const bool movable = track.isMacroTrack()
            ? track.macroCanvas.rfind(macro + " / ", 0) == 0
            : !track.bindings.empty() && std::all_of(track.bindings.begin(), track.bindings.end(),
                                                     [&](const auto& binding) { return inside(binding.parameterPath); });
        if(movable) moving.push_back(i);
    }
    ofJson record = {{"macroCanvas", macro}};
    if(moving.empty()) return record;

    // As they were here, for undo; and where the first one was.
    ofxOceanodeTimelineSequence moved;
    double end = 0.0;
    for(const auto index : moving) {
        moved.tracks.push_back(source.tracks[index]);
        for(const auto& clip : source.tracks[index].clips) end = std::max(end, clip.startBeat + clip.durationBeats);
    }
    record["tracks"] = sequenceToJson(moved);
    record["indices"] = moving;
    const std::string firstParent = source.tracks[moving.front()].parentId;
    size_t firstIndex = 0;
    for(size_t i = 0; i < moving.front(); ++i)
        if(source.tracks[i].parentId == firstParent && std::find(moving.begin(), moving.end(), i) == moving.end()) ++firstIndex;

    // Into the macro's timeline, its paths relative to the macro.
    for(auto track : moved.tracks) {
        if(target.findTrack(track.id) != nullptr) continue; // already there (a redo of a restored macro)
        for(auto& binding : track.bindings) binding.parameterPath = strip(binding.parameterPath);
        if(track.isMacroTrack()) track.macroCanvas = strip(track.macroCanvas);
        for(auto& clip : track.clips)
            for(auto& lane : clip.lanes) {
                std::decay_t<decltype(lane.targetProbability)> relative;
                for(const auto& [path, chance] : lane.targetProbability) relative[inside(path) ? strip(path) : path] = chance;
                lane.targetProbability = std::move(relative);
            }
        track.parentId.clear();
        target.tracks.push_back(std::move(track));
    }
    // Out of here, with their clips' group memberships.
    std::set<std::string> movedIds;
    for(const auto& track : moved.tracks) movedIds.insert(track.id);
    for(auto it = moving.rbegin(); it != moving.rend(); ++it) source.tracks.erase(source.tracks.begin() + static_cast<std::ptrdiff_t>(*it));
    for(auto& group : source.clipGroups)
        group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
            [&](const auto& member) { return movedIds.count(member.first) > 0; }), group.members.end());
    source.clipGroups.erase(std::remove_if(source.clipGroups.begin(), source.clipGroups.end(),
        [](const auto& group) { return group.members.size() < 2; }), source.clipGroups.end());
    source.normalizeFolders();

    // A macro track in their place: one clip playing the macro's timeline as they played.
    auto trackId = createMacroTrack(source, macroContainer);
    if(auto* track = getTrack(source, trackId); track != nullptr && !macroTrackId.empty() && source.findTrack(macroTrackId) == nullptr) {
        track->id = macroTrackId;
        trackId = macroTrackId;
    }
    if(!trackId.empty()) {
        createMacroClip(source, trackId, 0.0, end > 0.0 ? end : getBeatsPerBar());
        moveTrack(source, trackId, firstParent, firstIndex);
    }
    record["macroTrack"] = trackId;
    // Their history no longer matches either timeline.
    if(isMainSequence(source)) clearTimelineHistory(); else sequenceHistories.erase(&source);
    sequenceHistories.erase(&target);
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return record;
}

ofJson ofxOceanodeTimelineManager::encapsulateParameters(ofxOceanodeContainer& canvas, ofxOceanodeContainer& macroContainer,
                                                         const std::map<std::string, std::string>& oldToNew, bool moveIntoMacro) {
    retargetAllParameterPaths(oldToNew);
    if(!moveIntoMacro) return ofJson();
    auto* source = sequenceForCanvas(canvas);
    auto* target = getMacroSequence(macroContainer);
    const auto relative = relativeTo(canvas.getCanvasID(), macroContainer.getCanvasID());
    if(source == nullptr || target == nullptr || relative.empty()) return ofJson();
    return moveTracksIntoMacro(*source, *target, macroContainer, relative, nullptr, std::string());
}

void ofxOceanodeTimelineManager::replayParameterMove(ofxOceanodeContainer& canvas, ofxOceanodeContainer* macroContainer,
                                                     const std::map<std::string, std::string>& oldToNew,
                                                     const ofJson& record, bool backwards) {
    const bool movedTracks = record.is_object() && record.contains("tracks");
    auto* source = movedTracks ? sequenceForCanvas(canvas) : nullptr;
    if(!backwards) {
        retargetAllParameterPaths(oldToNew);
        if(source == nullptr || macroContainer == nullptr) return;
        auto* target = getMacroSequence(*macroContainer);
        if(target == nullptr) return;
        std::set<std::string> ids;
        for(const auto& track : record["tracks"].value("tracks", ofJson::array())) ids.insert(track.value("id", std::string()));
        moveTracksIntoMacro(*source, *target, *macroContainer, record.value("macroCanvas", std::string()), &ids,
                            record.value("macroTrack", std::string()));
        return;
    }
    if(source != nullptr) {
        // The macro track goes; the tracks come back where they were.
        const auto macroTrack = record.value("macroTrack", std::string());
        if(!macroTrack.empty()) removeTrack(*source, macroTrack);
        ofxOceanodeTimelineSequence restored;
        sequenceFromJson(restored, record["tracks"]);
        const auto indices = record.value("indices", std::vector<size_t>());
        for(size_t i = 0; i < restored.tracks.size(); ++i) {
            if(source->findTrack(restored.tracks[i].id) != nullptr) continue;
            const size_t at = std::min(i < indices.size() ? indices[i] : source->tracks.size(), source->tracks.size());
            source->tracks.insert(source->tracks.begin() + static_cast<std::ptrdiff_t>(at), std::move(restored.tracks[i]));
        }
        source->normalizeFolders();
        if(isMainSequence(*source)) clearTimelineHistory(); else sequenceHistories.erase(source);
    }
    std::map<std::string, std::string> newToOld;
    for(const auto& [from, to] : oldToNew) newToOld[to] = from;
    retargetAllParameterPaths(newToOld);
    invalidateEvaluationIndexes();
    invalidateSchedule();
}

// ---- Timelines by reference ----

ofxOceanodeContainer* ofxOceanodeTimelineManager::canvasContainer(const std::string& canvas) const {
    if(container == nullptr) return nullptr;
    if(isRootCanvas(canvas) || canvas == container->getCanvasID()) return container;
    return containerWithCanvas(*container, canvas);
}

ofxOceanodeTimelineManager::TimelineRef ofxOceanodeTimelineManager::refForCanvas(const ofxOceanodeContainer& canvas,
                                                                                 const std::string& timeline) const {
    if(&canvas == container || isRootCanvas(canvas.getCanvasID())) return {std::string(), timeline};
    return {canvas.getCanvasID(), timeline};
}

ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::timelineFor(const TimelineRef& ref) {
    ofxOceanodeTimelineSequence* base = nullptr;
    if(isRootCanvas(ref.canvas)) base = &mainSequence;
    else if(auto* canvas = canvasContainer(ref.canvas)) base = getMacroSequence(*canvas);
    if(base == nullptr || ref.timeline.empty()) return base;
    for(auto& timeline : base->timelines)
        if(timeline->timelineId == ref.timeline) return timeline.get();
    return nullptr;
}

bool ofxOceanodeTimelineManager::refOf(const ofxOceanodeTimelineSequence& sequence, TimelineRef& ref) const {
    auto within = [&](const ofxOceanodeTimelineSequence& base, const std::string& canvas) {
        if(&base == &sequence) { ref = {canvas, std::string()}; return true; }
        for(const auto& timeline : base.timelines)
            if(timeline.get() == &sequence) { ref = {canvas, timeline->timelineId}; return true; }
        return false;
    };
    if(within(mainSequence, std::string())) return true;
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        if(within(*entry.sequence, entry.address->getCanvasID())) return true;
    return false;
}

std::string ofxOceanodeTimelineManager::timelineName(const TimelineRef& ref) {
    auto* sequence = timelineFor(ref);
    if(sequence != nullptr && !sequence->play.name.empty()) return sequence->play.name;
    if(!ref.timeline.empty()) return ref.timeline;
    if(isRootCanvas(ref.canvas)) return "Main";
    if(auto* canvas = canvasContainer(ref.canvas)) return getMacroSequenceLabel(*canvas);
    return ref.canvas;
}

std::vector<ofxOceanodeTimelineManager::TimelineRef> ofxOceanodeTimelineManager::allTimelines() {
    std::vector<TimelineRef> refs{{}};
    for(const auto& timeline : mainSequence.timelines) refs.push_back({std::string(), timeline->timelineId});
    pruneMacroSequences();
    std::vector<std::pair<std::string, const ofxOceanodeTimelineSequence*>> macros;
    for(const auto& entry : macroSequences) macros.push_back({entry.address->getCanvasID(), entry.sequence.get()});
    std::sort(macros.begin(), macros.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for(const auto& [canvas, sequence] : macros) {
        refs.push_back({canvas, std::string()});
        for(const auto& timeline : sequence->timelines) refs.push_back({canvas, timeline->timelineId});
    }
    return refs;
}
