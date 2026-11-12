// Macro canvases: the timelines each macro owns, where they are saved and
// who shares them (copies of a library macro share one list).

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "Nodes/ofxOceanodeNode.h"
#include "Nodes/ofxOceanodeNodeMacro.h"
#include "Nodes/ofxOceanodeMorph.h"
#include "Timeline/ofxOceanodeTimelineInternal.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

namespace {
constexpr const char* kSequenceFile = "sequence.json";

// The macro node whose container is `target`, searched from `from` down:
// through its macros, and through its timeline guests (a MacroMixer's graphs,
// each a canvas holding the source as a Macro).
ofxOceanodeNodeMacro* owningMacro(ofxOceanodeContainer& from, const ofxOceanodeContainer& target) {
    for(auto* node : from.getAllModules()) {
        auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
        if(macro == nullptr || !macro->getContainer()) continue;
        if(macro->getContainer().get() == &target) return macro;
        if(auto* inner = owningMacro(*macro->getContainer(), target)) return inner;
    }
    for(const auto& guest : from.getTimelineGuests())
        if(auto* inner = owningMacro(*guest, target)) return inner;
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
    // A shared list resolves through a live copy (the first one may be gone).
    for(auto& entry : macroSequences) {
        entry.timelines->base = const_cast<ofxOceanodeContainer*>(entry.address);
        for(auto& timeline : entry.timelines->timelines) timeline->base = entry.timelines->base;
    }
    // A history belongs to a live timeline of the root's or a macro's canvas.
    auto liveHistory = [&](const ofxOceanodeTimelineSequence* sequence) {
        for(const auto& timeline : rootTimelines.timelines) if(timeline.get() == sequence) return true;
        for(const auto& entry : macroSequences)
            for(const auto& timeline : entry.timelines->timelines) if(timeline.get() == sequence) return true;
        return false;
    };
    for(auto it = sequenceHistories.begin(); it != sequenceHistories.end();) {
        if(liveHistory(it->first)) ++it; else it = sequenceHistories.erase(it);
    }
}

ofxOceanodeTimelineCanvas* ofxOceanodeTimelineManager::getMacroTimelines(ofxOceanodeContainer& macroContainer) {
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        if(entry.address == &macroContainer) return entry.timelines.get();
    if(container == nullptr) return nullptr;
    auto* macro = owningMacro(*container, macroContainer);
    if(macro == nullptr) return nullptr;
    MacroSequenceEntry entry;
    entry.container = macro->getContainer();
    entry.address = &macroContainer;
    entry.libraryFolder = macro->isLocal() ? std::string() : libraryKey(macro->getCurrentMacroPath());
    // Another copy of the same library macro already has them.
    if(!entry.libraryFolder.empty())
        for(const auto& other : macroSequences)
            if(other.libraryFolder == entry.libraryFolder) entry.timelines = other.timelines;
    if(!entry.timelines) {
        entry.timelines = std::make_shared<ofxOceanodeTimelineCanvas>();
        entry.timelines->base = &macroContainer;
    }
    macroSequences.push_back(entry);
    return entry.timelines.get();
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
    if(macro == nullptr) return; // not a macro's container
    pruneMacroSequences();
    const std::string canvas = macroContainer.getCanvasID();
    const std::string library = macro->isLocal() ? std::string() : libraryKey(folder);
    // This container's previous sequence (a reload, or a switch between local
    // and library): what it played stops and rewinds, and what it drove is no
    // longer marked as timelined, before the loaded one takes its place.
    const auto previous = std::find_if(macroSequences.begin(), macroSequences.end(),
        [&](const auto& entry) { return entry.address == &macroContainer; });
    if(previous != macroSequences.end()) {
        std::vector<TimelineRef> refs;
        for(const auto& timeline : previous->timelines->timelines) refs.push_back({canvas, timeline->timelineId});
        for(const auto& ref : refs) {
            // A timeline of the same id in the loaded sequence keeps this
            // one's engine, which holds this one's content until its next
            // update: rewound below, it would write this one's start (a
            // Project Track's first keyframe) over the values of now, and
            // what the loaded timeline does not drive would stay there.
            for(auto& player : players)
                if(player->ref.canvas == ref.canvas && player->ref.timeline == ref.timeline) player->awaitingSync = true;
            // Its own transport only: the global one is not the macro's to stop.
            if(auto transport = transportFor(ref); transport != nullptr && transport->isDerived()) {
                transport->cancelPendingActions();
                transport->stop();
            }
            const auto* sequence = timelineFor(ref);
            if(sequence == nullptr) continue;
            for(const auto& track : sequence->tracks)
                for(const auto& binding : track.bindings)
                    if(auto* parameter = container->findTimelineParameter(absolutePathIn(canvas, binding.parameterPath)))
                        parameter->setTimelined(false);
        }
        macroSequences.erase(previous);
    }
    MacroSequenceEntry entry;
    entry.container = macro->getContainer();
    entry.address = &macroContainer;
    entry.libraryFolder = library;
    // A library definition already in memory is shared as it is (with any edits not yet saved).
    if(!library.empty())
        for(const auto& other : macroSequences)
            if(other.libraryFolder == library) entry.timelines = other.timelines;
    if(!entry.timelines) {
        entry.timelines = std::make_shared<ofxOceanodeTimelineCanvas>();
        entry.timelines->base = &macroContainer;
        // A macro's own timelines (sequence.json), or, for a preset folder
        // loaded as a Macro (a MacroMixer's source), the preset's own
        // timelines (timeline.json): its root canvas's become this macro's.
        std::string file = ofFilePath::join(folder, kSequenceFile);
        const bool macroFile = ofFile::doesFileExist(file);
        if(!macroFile) file = ofFilePath::join(folder, "timeline.json");
        if(ofFile::doesFileExist(file)) canvasFromJson(*entry.timelines, upgradeTimelineDocument(ofLoadJson(file), macroFile));
    }
    macroSequences.push_back(entry);
    // Older project files kept which timelines played free, and whose clips
    // a macro's nodes followed, apart from the macro: now in its timelines'
    // own settings.
    for(auto& timeline : entry.timelines->timelines) {
        if(legacyFreeTimelines.count({canvas, timeline->timelineId}) > 0) timeline->play.free = true;
        if(legacyClipFollowTimelines.count({canvas, timeline->timelineId}) > 0) setNodesFollowClips({canvas, timeline->timelineId}, true);
    }
    // Its timelines that play on their own and are set to play on load start
    // (while the macro is active).
    for(const auto& timeline : entry.timelines->timelines) playOnLoadPending.insert({canvas, timeline->timelineId});
    invalidateEvaluationIndexes();
}

bool ofxOceanodeTimelineManager::macrosActive(Player& player) const {
    if(container == nullptr || player.ref.canvas.empty()) return true; // refs are normalized: "" is the root
    auto canvas = player.canvas.lock();
    if(canvas == nullptr) return true;
    // Found again only when a canvas holding one of them changed its nodes.
    bool stale = player.macros.empty();
    for(const auto& link : player.macros) {
        std::shared_ptr<ofxOceanodeContainer> held = link.heldByRoot ? nullptr : link.holder.lock();
        const ofxOceanodeContainer* holder = link.heldByRoot ? container : held.get();
        if(holder == nullptr || holder->getNodesRevision() != link.revision) { stale = true; break; }
    }
    if(stale) {
        player.macros.clear();
        std::function<bool(ofxOceanodeContainer&, const std::shared_ptr<ofxOceanodeContainer>&)> find =
            [&](ofxOceanodeContainer& from, const std::shared_ptr<ofxOceanodeContainer>& fromShared) {
                for(auto* node : from.getAllModules()) {
                    auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
                    if(macro == nullptr || !macro->getContainer()) continue;
                    player.macros.push_back({macro, fromShared, fromShared == nullptr, from.getNodesRevision()});
                    if(macro->getContainer() == canvas || find(*macro->getContainer(), macro->getContainer())) return true;
                    player.macros.pop_back();
                }
                // A timeline guest (a MacroMixer's graph) is no macro: only the
                // Macros inside it count.
                for(const auto& guest : from.getTimelineGuests())
                    if(find(*guest, guest)) return true;
                return false;
            };
        if(!find(*container, nullptr)) return true; // not under a macro after all
    }
    for(const auto& link : player.macros)
        if(!link.macro->isActive()) return false;
    return true;
}

bool ofxOceanodeTimelineManager::saveMacroSequence(const ofxOceanodeContainer& macroContainer, const std::string& folder) const {
    pruneMacroSequences();
    const auto found = std::find_if(macroSequences.begin(), macroSequences.end(),
        [&](const auto& entry) { return entry.address == &macroContainer; });
    const std::string file = ofFilePath::join(folder, kSequenceFile);
    // No timeline: no file, so a removed one does not come back.
    if(found == macroSequences.end() || found->timelines->timelines.empty()) {
        if(ofFile::doesFileExist(file)) ofFile::removeFile(file);
        return true;
    }
    auto json = canvasToJson(*found->timelines);
    json["version"] = kPresetVersion;
    return ofSavePrettyJson(file, json);
}


namespace {
// The macro container with this canvas id, searched from `from` down (a
// timeline guest's canvas, a MacroMixer's graph, nests under its host's).
ofxOceanodeContainer* containerWithCanvas(ofxOceanodeContainer& from, const std::string& canvasID) {
    auto within = [&](ofxOceanodeContainer& inner) -> ofxOceanodeContainer* {
        if(inner.getCanvasID() == canvasID) return &inner;
        if(canvasID.rfind(inner.getCanvasID() + " / ", 0) == 0) return containerWithCanvas(inner, canvasID);
        return nullptr;
    };
    for(auto* node : from.getAllModules()) {
        auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
        if(macro == nullptr || !macro->getContainer()) continue;
        if(auto* found = within(*macro->getContainer())) return found;
    }
    for(const auto& guest : from.getTimelineGuests())
        if(auto* found = within(*guest)) return found;
    return nullptr;
}

bool isRootCanvas(const std::string& canvasID) { return canvasID.empty() || canvasID == "Canvas" || canvasID == "0"; }
} // namespace

ofxOceanodeContainer* ofxOceanodeTimelineManager::findMacroContainer(const ofxOceanodeTimelineSequence& sequence,
                                                                     const ofxOceanodeTimelineTrack& track) const {
    if(container == nullptr || !track.isMacroTrack()) return nullptr;
    const auto* base = sequence.base != nullptr ? sequence.base : container;
    if(track.macroCanvas == ".") return const_cast<ofxOceanodeContainer*>(base); // another timeline of its own canvas
    const std::string canvas = isRootCanvas(base->getCanvasID()) ? track.macroCanvas
                                                                 : base->getCanvasID() + " / " + track.macroCanvas;
    return containerWithCanvas(*container, canvas);
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
    int changed = 0;
    for(auto& timeline : rootTimelines.timelines) changed += retargetParameterPaths(*timeline, oldToNew);
    pruneMacroSequences();
    // A macro canvas's timelines hold paths relative to their macro.
    for(const auto& entry : macroSequences) {
        const std::string base = entry.address->getCanvasID();
        std::map<std::string, std::string> relative;
        for(const auto& [from, to] : oldToNew) {
            const auto relativeFrom = relativeTo(base, from), relativeNew = relativeTo(base, to);
            if(!relativeFrom.empty() && !relativeNew.empty()) relative[relativeFrom] = relativeNew;
        }
        if(relative.empty()) continue;
        for(auto& timeline : entry.timelines->timelines) changed += retargetParameterPaths(*timeline, relative);
    }
    return changed;
}

void ofxOceanodeTimelineManager::collectBoundParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const {
    if(container == nullptr) return;
    auto collect = [&](const ofxOceanodeTimelineSequence& sequence, const ofxOceanodeContainer& base) {
        for(const auto& track : sequence.tracks)
            for(const auto& binding : track.bindings)
                if(auto* parameter = base.findTimelineParameter(binding.parameterPath)) result.insert(parameter);
    };
    for(const auto& timeline : rootTimelines.timelines) collect(*timeline, *container);
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        for(const auto& timeline : entry.timelines->timelines) collect(*timeline, *entry.address);
}

void ofxOceanodeTimelineManager::collectProjectDrivenParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const {
    if(container == nullptr) return;
    // As rebuildProjectChannels chooses its channels.
    auto collect = [&](const ofxOceanodeTimelineSequence& sequence, const ofxOceanodeContainer& base) {
        for(const auto& track : sequence.tracks) {
            if(!track.isProjectTrack) continue;
            for(const auto& clip : track.clips) {
                if(clip.projectKeyframes.size() < 2) continue;
                std::map<std::string, const std::string*> first;
                std::set<std::string> differing;
                for(const auto& keyframe : clip.projectKeyframes) {
                    if(!keyframe.values) continue;
                    for(const auto& value : *keyframe.values) {
                        const auto found = first.emplace(value.first, &value.second);
                        if(!found.second && *found.first->second != value.second) differing.insert(value.first);
                    }
                }
                for(const auto& path : differing)
                    if(auto* parameter = base.findTimelineParameter(path))
                        if(projectControlOf(*parameter).control) result.insert(parameter);
            }
        }
    };
    for(const auto& timeline : rootTimelines.timelines) collect(*timeline, *container);
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        for(const auto& timeline : entry.timelines->timelines) collect(*timeline, *entry.address);
}

void ofxOceanodeTimelineManager::removeBindingsOf(ofxOceanodeAbstractParameter& parameter) {
    // Every timeline: by the path each one saves for the parameter.
    auto remove = [&](ofxOceanodeTimelineSequence& sequence) {
        const std::string path = resolverFor(sequence).pathOf(parameter);
        if(path.empty()) return;
        std::vector<std::pair<std::string, std::string>> bindings;
        for(const auto& track : sequence.tracks)
            for(const auto& binding : track.bindings)
                if(binding.parameterPath == path) bindings.emplace_back(track.id, binding.id);
        for(const auto& binding : bindings) removeBinding(sequence, binding.first, binding.second);
    };
    for(auto& timeline : rootTimelines.timelines) remove(*timeline);
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        for(auto& timeline : entry.timelines->timelines) remove(*timeline);
}



ofxOceanodeTimelineSequence* ofxOceanodeTimelineManager::encapsulationTarget(ofxOceanodeContainer& macroContainer, bool& made) {
    made = false;
    auto* list = getMacroTimelines(macroContainer);
    if(list == nullptr) return nullptr;
    if(list->timelines.empty()) made = !createTimeline(macroContainer.getCanvasID(), getMacroSequenceLabel(macroContainer)).empty();
    return list->timelines.empty() ? nullptr : list->timelines.front().get();
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
    TimelineRef targetRef;
    std::string trackId;
    if(refOf(target, targetRef)) trackId = createTimelineTrack(source, targetRef);
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
    sequenceHistories.erase(&source);
    sequenceHistories.erase(&target);
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return record;
}

ofJson ofxOceanodeTimelineManager::encapsulateParameters(ofxOceanodeContainer& canvas, ofxOceanodeContainer& macroContainer,
                                                         const std::map<std::string, std::string>& oldToNew, bool moveIntoMacro) {
    retargetAllParameterPaths(oldToNew);
    if(!moveIntoMacro) return ofJson();
    // From every timeline of the canvas, into the macro's first timeline
    // (made for it when it has none).
    auto* sources = canvasTimelines(canvasIdOf(canvas));
    const auto relative = relativeTo(canvas.getCanvasID(), macroContainer.getCanvasID());
    if(sources == nullptr || relative.empty()) return ofJson();
    bool made = false;
    auto* target = encapsulationTarget(macroContainer, made);
    TimelineRef targetRef;
    if(target == nullptr || !refOf(*target, targetRef)) return ofJson();
    ofJson record = {{"macroCanvas", relative}, {"macroTimeline", targetRef.timeline}, {"madeTimeline", made},
                     {"moves", ofJson::array()}};
    const auto timelines = sources->timelines; // the list is stable while moving
    for(const auto& source : timelines) {
        auto move = moveTracksIntoMacro(*source, *target, macroContainer, relative, nullptr, std::string());
        if(!move.contains("tracks")) continue; // nothing of this timeline went in
        move["sourceTimeline"] = source->timelineId;
        record["moves"].push_back(std::move(move));
    }
    if(record["moves"].empty() && made) {
        removeTimeline(targetRef);
        record["madeTimeline"] = false;
    }
    return record;
}

void ofxOceanodeTimelineManager::replayParameterMove(ofxOceanodeContainer& canvas, ofxOceanodeContainer* macroContainer,
                                                     const std::map<std::string, std::string>& oldToNew,
                                                     const ofJson& record, bool backwards) {
    const bool movedTracks = record.is_object() && record.contains("moves") && record["moves"].is_array();
    auto* sources = movedTracks ? canvasTimelines(canvasIdOf(canvas)) : nullptr;
    if(!backwards) {
        retargetAllParameterPaths(oldToNew);
        if(sources == nullptr || macroContainer == nullptr) return;
        bool made = false;
        auto* target = encapsulationTarget(*macroContainer, made);
        if(target == nullptr) return;
        for(const auto& move : record["moves"]) {
            auto* source = sources->find(move.value("sourceTimeline", std::string()));
            if(source == nullptr) continue;
            std::set<std::string> ids;
            for(const auto& track : move["tracks"].value("tracks", ofJson::array())) ids.insert(track.value("id", std::string()));
            moveTracksIntoMacro(*source, *target, *macroContainer, move.value("macroCanvas", std::string()), &ids,
                                move.value("macroTrack", std::string()));
        }
        return;
    }
    if(sources != nullptr) {
        for(const auto& move : record["moves"]) {
            auto* source = sources->find(move.value("sourceTimeline", std::string()));
            if(source == nullptr) continue;
            // The macro track goes; the tracks come back where they were.
            const auto macroTrack = move.value("macroTrack", std::string());
            if(!macroTrack.empty()) removeTrack(*source, macroTrack);
            ofxOceanodeTimelineSequence restored;
            sequenceFromJson(restored, move["tracks"]);
            const auto indices = move.value("indices", std::vector<size_t>());
            for(size_t i = 0; i < restored.tracks.size(); ++i) {
                if(source->findTrack(restored.tracks[i].id) != nullptr) continue;
                const size_t at = std::min(i < indices.size() ? indices[i] : source->tracks.size(), source->tracks.size());
                source->tracks.insert(source->tracks.begin() + static_cast<std::ptrdiff_t>(at), std::move(restored.tracks[i]));
            }
            source->normalizeFolders();
            sequenceHistories.erase(source);
        }
        // A timeline made for the macro by the move, left with nothing: gone again.
        if(macroContainer != nullptr && record.value("madeTimeline", false)) {
            const TimelineRef targetRef{macroContainer->getCanvasID(), record.value("macroTimeline", std::string())};
            if(const auto* target = findTimeline(targetRef); target != nullptr && target->tracks.empty()) removeTimeline(targetRef);
        }
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
    // A canvas with a timeline is known already: no walk of the graph.
    for(const auto& entry : sequenceEntries())
        if(entry.address != nullptr && !entry.container.expired() && entry.address->getCanvasID() == canvas)
            return entry.container.lock().get();
    return containerWithCanvas(*container, canvas);
}

std::vector<std::string> ofxOceanodeTimelineManager::allCanvases() const {
    std::vector<std::string> canvases{std::string()};
    if(container == nullptr) return canvases;
    std::function<void(ofxOceanodeContainer&)> walk = [&](ofxOceanodeContainer& from) {
        for(auto* node : from.getAllModules()) {
            auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
            if(macro == nullptr || !macro->getContainer()) continue;
            canvases.push_back(macro->getContainer()->getCanvasID());
            walk(*macro->getContainer());
        }
        // A MacroMixer's graphs: the Macros inside them have canvases, the
        // graph host itself has no timeline of its own.
        for(const auto& guest : from.getTimelineGuests()) walk(*guest);
    };
    walk(*container);
    // A macro's canvas after the one it is in.
    std::sort(canvases.begin() + 1, canvases.end());
    canvases.erase(std::unique(canvases.begin() + 1, canvases.end()), canvases.end());
    return canvases;
}

// Here rather than with the rest of the Project Track: walking nodes and
// macros needs the core's node headers, which this file already takes.
namespace {
// The Macro whose canvas `matches`, among `canvas` and the macros in it (and
// a MacroMixer's graphs, whose own Macro is not known here).
bool findProjectOwner(ofxOceanodeContainer& canvas, ofxOceanodeNodeMacro* owner,
                      const std::function<bool(ofxOceanodeContainer&)>& matches,
                      ofxOceanodeNodeMacro*& result, size_t depth = 0) {
    if(depth >= 64) return false;
    if(matches(canvas)) { result = owner; return true; }
    for(auto* node : canvas.getAllModules()) {
        if(node == nullptr) continue;
        if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel()))
            if(auto inner = macro->getContainer())
                if(findProjectOwner(*inner, macro, matches, result, depth + 1)) return true;
    }
    for(const auto& guest : canvas.getTimelineGuests())
        if(guest != nullptr && findProjectOwner(*guest, nullptr, matches, result, depth + 1)) return true;
    return false;
}
}

void ofxOceanodeTimelineManager::captureProjectValues(ofxOceanodeContainer& base,
                                                      const std::unordered_set<ofxOceanodeAbstractParameter*>& bound,
                                                      std::map<std::string, std::string>& values) const {
    std::function<void(ofxOceanodeContainer&, ofxOceanodeNodeMacro*)> collect = [&](ofxOceanodeContainer& canvas, ofxOceanodeNodeMacro* owner) {
        for(auto* node : canvas.getAllModules()) {
            if(node == nullptr) continue;
            auto& model = node->getNodeModel();
            if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&model)) {
                // A Macro's pins mirror the routers inside it: those are stored.
                if(auto inner = macro->getContainer()) collect(*inner, macro);
                continue;
            }
            for(auto& abstract : node->getParameters()) {
                auto* parameter = dynamic_cast<ofxOceanodeAbstractParameter*>(abstract.get());
                if(parameter == nullptr || !ofxOceanodeNode::canSnapshotParameter(*parameter)) continue;
                if(bound.count(parameter) > 0 || !ofxOceanodeMorph::isControl(model, *parameter, owner)) continue;
                const std::string path = base.getTimelineParameterPath(*parameter, base);
                if(path.empty()) continue;
                values[path] = ofxOceanodeTimelineDetail::parameterValueForAutomation(*parameter);
            }
        }
    };
    // The Macro `base` is the canvas of, when it is one.
    ofxOceanodeNodeMacro* owner = nullptr;
    if(container != nullptr)
        findProjectOwner(*container, nullptr, [&](ofxOceanodeContainer& canvas) { return &canvas == &base; }, owner);
    collect(base, owner);
}

ofxOceanodeTimelineManager::ProjectControl ofxOceanodeTimelineManager::projectControlOf(ofxOceanodeAbstractParameter& parameter) const {
    ProjectControl result;
    auto* model = parameter.getNodeModel();
    if(model == nullptr || container == nullptr) return result;
    if(dynamic_cast<ofxOceanodeNodeMacro*>(model) != nullptr) { result.control = false; return result; }
    ofxOceanodeNodeMacro* owner = nullptr;
    findProjectOwner(*container, nullptr, [&](ofxOceanodeContainer& canvas) {
        for(auto* node : canvas.getAllModules())
            if(node != nullptr && &node->getNodeModel() == model) return true;
        return false;
    }, owner);
    result.control = ofxOceanodeMorph::isControl(*model, parameter, owner);
    if(result.control) {
        const auto motion = ofxOceanodeMorph::motionOf(*model, parameter);
        result.discrete = motion.discrete;
        result.logarithmic = motion.logarithmic;
    }
    return result;
}

std::vector<float> ofxOceanodeTimelineManager::blendProjectNumbers(const std::vector<float>& from, const std::vector<float>& to,
                                                                   float t, bool integers, bool logarithmic) {
    if(!integers) return ofxOceanodeMorph::blend(from, to, t, logarithmic);
    auto whole = [](const std::vector<float>& values) {
        std::vector<int> result(values.size());
        for(size_t i = 0; i < values.size(); ++i) result[i] = static_cast<int>(std::lround(values[i]));
        return result;
    };
    const auto blended = ofxOceanodeMorph::blend(whole(from), whole(to), t);
    return std::vector<float>(blended.begin(), blended.end());
}
