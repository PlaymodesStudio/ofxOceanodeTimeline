// Macro canvases: the timelines each macro owns, where they are saved and
// who shares them (copies of a library macro share one list).

#include "Timeline/ofxOceanodeTimeline.h"

#include "Managers/ofxOceanodeContainer.h"
#include "ofxOceanodeShared.h"
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
    // A source document no graph plays now resolves through no canvas.
    for(const auto& document : morphDocuments) {
        const auto& list = document.second.timelines;
        if(!list || std::any_of(macroSequences.begin(), macroSequences.end(),
                                [&](const auto& entry) { return entry.timelines == list; })) continue;
        list->base = nullptr;
        for(auto& timeline : list->timelines) timeline->base = nullptr;
    }
}

// ---- Macro Morphing Track: source documents ----

ofxOceanodeTimelineManager::MorphDocument* ofxOceanodeTimelineManager::morphDocumentOf(const std::string& folder) {
    if(folder.empty()) return nullptr;
    auto key = morphDocumentKeys.find(folder);
    if(key == morphDocumentKeys.end()) key = morphDocumentKeys.emplace(folder, libraryKey(folder)).first;
    const auto found = morphDocuments.find(key->second);
    return found == morphDocuments.end() || !found->second.timelines ? nullptr : &found->second;
}

ofxOceanodeTimelineManager::MorphDocument* ofxOceanodeTimelineManager::ensureMorphDocument(const std::string& folder) {
    if(auto* document = morphDocumentOf(folder)) return document;
    if(folder.empty()) return nullptr;
    // As loadMacroSequence reads a source: a macro's own timelines
    // (sequence.json), else a preset's (timeline.json); none: an empty list.
    auto& document = morphDocuments[morphDocumentKeys.at(folder)];
    document.timelines = std::make_shared<ofxOceanodeTimelineCanvas>();
    std::string file = ofFilePath::join(folder, kSequenceFile);
    const bool macroFile = ofFile::doesFileExist(file);
    if(!macroFile) file = ofFilePath::join(folder, "timeline.json");
    if(ofFile::doesFileExist(file)) canvasFromJson(*document.timelines, upgradeTimelineDocument(ofLoadJson(file), macroFile));
    for(auto& timeline : document.timelines->timelines) timeline->loadStamp = ofxOceanodeTimelineSequence::nextLoadStamp();
    document.folder = folder;
    document.macroFile = macroFile;
    document.savedSignature = morphDocumentSignature(document);
    document.seenAt = -1.0f;
    return &document;
}

ofJson ofxOceanodeTimelineManager::morphSourceEditsJson() const {
    ofJson edits = ofJson::array();
    for(const auto& entry : morphDocuments) {
        const auto& document = entry.second;
        if(!document.timelines || morphDocumentSignature(document) == document.savedSignature) continue;
        ofJson item = canvasToJson(*document.timelines);
        item["folder"] = document.folder;
        item["macroFile"] = document.macroFile;
        edits.push_back(std::move(item));
    }
    return edits;
}

void ofxOceanodeTimelineManager::restoreMorphSourceEdits(const ofJson& json) {
    morphDocuments.clear();
    morphDocumentKeys.clear();
    if(!json.is_array()) return;
    for(const auto& item : json) {
        if(!item.is_object() || !item.contains("timelines")) continue;
        const std::string folder = item.value("folder", std::string());
        if(folder.empty()) continue;
        auto& document = morphDocuments[libraryKey(folder)];
        document.timelines = std::make_shared<ofxOceanodeTimelineCanvas>();
        canvasFromJson(*document.timelines, item);
        for(auto& timeline : document.timelines->timelines) timeline->loadStamp = ofxOceanodeTimelineSequence::nextLoadStamp();
        document.folder = folder;
        document.macroFile = item.value("macroFile", false);
        document.savedSignature = 0;   // not the folder's: still to be saved there
        document.seenAt = -1.0f;
    }
}

bool ofxOceanodeTimelineManager::isMorphDocument(const ofxOceanodeTimelineCanvas* timelines) const {
    if(timelines == nullptr) return false;
    for(const auto& entry : morphDocuments) if(entry.second.timelines.get() == timelines) return true;
    return false;
}

void ofxOceanodeTimelineManager::dropMorphDocument(const std::string& folder) {
    if(folder.empty()) return;
    morphDocuments.erase(libraryKey(folder));
}

uint64_t ofxOceanodeTimelineManager::morphDocumentSignature(const MorphDocument& document) const {
    uint64_t hash = 1469598103934665603ull;
    auto add = [&](uint64_t word) { hash = (hash ^ word) * 1099511628211ull; };
    if(!document.timelines) return hash;
    add(document.timelines->timelines.size());
    for(const auto& timeline : document.timelines->timelines) add(sequenceSignature(*timeline));
    return hash;
}

bool ofxOceanodeTimelineManager::morphSourceDirty(const ofxOceanodeTimelineSequence& host, const std::string& sourceId,
                                                  bool exact) {
    auto* document = morphDocumentOf(morphSourceFolder(host, sourceId));
    if(document == nullptr) return false;
    // (Asked every frame by the editor: looked at a few times a second.)
    const float now = ofGetElapsedTimef();
    if(exact || document->seenAt < 0.0f || now - document->seenAt > 0.25f || now < document->seenAt) {
        document->seenSignature = morphDocumentSignature(*document);
        document->seenAt = now;
    }
    return document->seenSignature != document->savedSignature;
}

bool ofxOceanodeTimelineManager::saveMorphSource(const ofxOceanodeTimelineSequence& host, const std::string& sourceId,
                                                 std::string& error) {
    error.clear();
    const std::string folder = morphSourceFolder(host, sourceId);
    auto* document = morphDocumentOf(folder);
    if(document == nullptr) {
        error = "This source's timelines are not loaded: it is read when one of its clips first plays.";
        return false;
    }
    if(!ofDirectory::doesDirectoryExist(document->folder)) {
        error = "The source's folder is gone: " + document->folder;
        return false;
    }
    const std::string file = ofFilePath::join(document->folder, document->macroFile ? kSequenceFile : "timeline.json");
    // The file as it is, in today's format: only its timelines are replaced
    // (a preset's timeline.json keeps its view, tempo and the rest).
    ofJson json = ofJson::object();
    if(ofFile::doesFileExist(file)) {
        json = upgradeTimelineDocument(ofLoadJson(file), document->macroFile);
        if(!json.is_object()) json = ofJson::object();
    }
    json["timelines"] = ofJson::array();
    for(const auto& timeline : document->timelines->timelines) {
        // As a macro saves its own: without what they drive outside the source.
        const bool reachesOut = std::any_of(timeline->tracks.begin(), timeline->tracks.end(), [](const auto& track) {
            return std::any_of(track.bindings.begin(), track.bindings.end(), [](const auto& binding) {
                return ofxOceanodeTimelineOutside::isOutside(binding.parameterPath);
            });
        });
        if(!reachesOut) { json["timelines"].push_back(sequenceToJson(*timeline)); continue; }
        ofxOceanodeTimelineSequence inside = *timeline;
        stripOutsideTargets(inside);
        json["timelines"].push_back(sequenceToJson(inside));
    }
    json["nextTimelineNumber"] = document->timelines->nextTimelineNumber;
    json["version"] = kPresetVersion;
    if(!ofSavePrettyJson(file, json)) {
        error = "Could not write " + file;
        return false;
    }
    document->savedSignature = document->seenSignature = morphDocumentSignature(*document);
    document->seenAt = ofGetElapsedTimef();
    return true;
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

bool ofxOceanodeTimelineManager::macroIsGlobal(ofxOceanodeContainer& macroContainer) const {
    if(container == nullptr) return false;
    auto* macro = owningMacro(*container, macroContainer);
    return macro != nullptr && !macro->isLocal();
}

bool ofxOceanodeTimelineManager::macroCanvasAlive(const ofxOceanodeContainer& canvas) const {
    return container != nullptr && owningMacro(*container, canvas) != nullptr;
}

bool ofxOceanodeTimelineManager::saveGlobalMacro(ofxOceanodeContainer& macroContainer) {
    if(container == nullptr) return false;
    auto* macro = owningMacro(*container, macroContainer);
    return macro != nullptr && !macro->isLocal() && macro->saveGlobalDefinition();
}

void ofxOceanodeTimelineManager::loadMacroSequence(ofxOceanodeContainer& macroContainer, const std::string& folder) {
    if(container == nullptr) return;
    auto* macro = owningMacro(*container, macroContainer);
    if(macro == nullptr) return; // not a macro's container
    pruneMacroSequences();
    const std::string canvas = macroContainer.getCanvasID();
    const std::string library = macro->isLocal() ? std::string() : libraryKey(folder);
    // The canvases above this macro, nearest first, with how far up each is:
    // they keep what its timelines drive outside it.
    std::vector<std::pair<ofxOceanodeTimelineCanvas*, size_t>> keepers;
    {
        const auto levels = ofxOceanodeTimelineOutside::canvasLevels(canvas);
        for(size_t above = levels.size(); above-- > 0;) {
            auto* keeper = above == 0 ? &rootTimelines
                : findCanvasTimelines(ofxOceanodeTimelineOutside::joinLevels(levels, 0, above));
            if(keeper != nullptr) keepers.emplace_back(keeper, levels.size() - above);
        }
        // Timelines of this macro about to be replaced: what they drive
        // outside it, as it is now, stays with those canvases (under the
        // definition they were loaded as), for when it comes back.
        const bool reloads = std::any_of(macroSequences.begin(), macroSequences.end(),
            [&](const auto& entry) { return entry.address == &macroContainer; });
        if(reloads)
            for(const auto& keeper : keepers) {
                const std::string keeperCanvas = ofxOceanodeTimelineOutside::joinLevels(levels, 0, levels.size() - keeper.second);
                keeper.first->outsideTargets = outsideTargetsJson(keeperCanvas, *keeper.first);
            }
    }
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
            // Global Track's first keyframe) over the values of now, and
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
    // A source of a Macro Morphing Track: every graph built from its folder
    // plays the timelines read the first time, with the edits made since
    // (its source document), not the folder's again.
    MorphDocument* document = nullptr;
    if(!entry.timelines && library.empty()) {
        const std::string key = libraryKey(folder);
        for(const auto& sourceFolder : morphSourceFolders())
            if(libraryKey(sourceFolder) == key) { document = &morphDocuments[key]; break; }
        if(document != nullptr && document->timelines) {
            entry.timelines = document->timelines;
            entry.timelines->base = &macroContainer;
            for(auto& timeline : entry.timelines->timelines) timeline->base = &macroContainer;
        }
    }
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
        for(auto& timeline : entry.timelines->timelines) timeline->loadStamp = ofxOceanodeTimelineSequence::nextLoadStamp();
        if(document != nullptr) {
            document->timelines = entry.timelines;
            document->folder = folder;
            document->macroFile = macroFile;
            document->savedSignature = morphDocumentSignature(*document);
            document->seenAt = -1.0f;
        }
    }
    macroSequences.push_back(entry);
    // What these timelines drive outside the macro, from the canvases above
    // it. (While a whole project loads those still hold the one before:
    // each gives its own again once it has loaded, see restoreOutsideTargets.)
    {
        const auto levels = ofxOceanodeTimelineOutside::canvasLevels(canvas);
        bool adopted = false;
        for(const auto& keeper : keepers) {
            if(keeper.first == entry.timelines.get()) continue;
            adopted = adoptOutsideTargets(entry, *keeper.first, keeper.second,
                ofxOceanodeTimelineOutside::joinLevels(levels, levels.size() - keeper.second, levels.size())) || adopted;
        }
        if(adopted) invalidateSchedule();
    }
    // What the timelines of the macros inside this one drive on its canvas
    // is kept here (those macros loaded before it).
    restoreOutsideTargets(canvas, *entry.timelines);
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
    // What the macros inside this one drive on its canvas is saved here.
    const ofJson outside = found == macroSequences.end() ? ofJson::array()
        : outsideTargetsJson(macroContainer.getCanvasID(), *found->timelines);
    // No timeline: no file, so a removed one does not come back.
    if(found == macroSequences.end() || (found->timelines->timelines.empty() && outside.empty())) {
        if(ofFile::doesFileExist(file)) ofFile::removeFile(file);
        return true;
    }
    // Its timelines without what they drive outside the macro: that is
    // saved by the canvas the parameter lives in, so the macro stays the
    // same wherever it is used.
    ofJson json;
    json["timelines"] = ofJson::array();
    for(const auto& timeline : found->timelines->timelines) {
        const bool reachesOut = std::any_of(timeline->tracks.begin(), timeline->tracks.end(), [](const auto& track) {
            return std::any_of(track.bindings.begin(), track.bindings.end(), [](const auto& binding) {
                return ofxOceanodeTimelineOutside::isOutside(binding.parameterPath);
            });
        });
        if(!reachesOut) { json["timelines"].push_back(sequenceToJson(*timeline)); continue; }
        ofxOceanodeTimelineSequence inside = *timeline;
        stripOutsideTargets(inside);
        json["timelines"].push_back(sequenceToJson(inside));
    }
    json["nextTimelineNumber"] = found->timelines->nextTimelineNumber;
    if(!outside.empty()) json["outsideTargets"] = outside;
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
        if(!relative.empty())
            for(auto& timeline : entry.timelines->timelines) changed += retargetParameterPaths(*timeline, relative);
        // What they drive outside the macro, by its path from the root.
        for(auto& timeline : entry.timelines->timelines) for(auto& track : timeline->tracks) for(auto& binding : track.bindings) {
            if(!ofxOceanodeTimelineOutside::isOutside(binding.parameterPath)) continue;
            const auto moved = oldToNew.find(absolutePathIn(base, binding.parameterPath));
            if(moved == oldToNew.end() || moved->second.empty() || moved->second == moved->first) continue;
            binding.parameterPath = ofxOceanodeTimelineOutside::relativePath(base, moved->second);
            binding.missingTarget = false;
            changed++;
        }
    }
    return changed;
}

void ofxOceanodeTimelineManager::collectBoundParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const {
    if(container == nullptr) return;
    auto collect = [&](const ofxOceanodeTimelineSequence& sequence, const ofxOceanodeContainer& base) {
        for(const auto& track : sequence.tracks)
            for(const auto& binding : track.bindings)
                if(auto* parameter = findTarget(base, binding.parameterPath)) result.insert(parameter);
    };
    for(const auto& timeline : rootTimelines.timelines) collect(*timeline, *container);
    pruneMacroSequences();
    for(const auto& entry : macroSequences)
        for(const auto& timeline : entry.timelines->timelines) collect(*timeline, *entry.address);
}

void ofxOceanodeTimelineManager::collectGlobalDrivenParameters(std::unordered_set<ofxOceanodeAbstractParameter*>& result) const {
    if(container == nullptr) return;
    // As rebuildGlobalChannels chooses its channels.
    auto collect = [&](const ofxOceanodeTimelineSequence& sequence, const ofxOceanodeContainer& base) {
        for(const auto& track : sequence.tracks) {
            if(!track.isGlobalTrack) continue;
            for(const auto& clip : track.clips) {
                if(clip.globalKeyframes.size() < 2) continue;
                std::map<std::string, const std::string*> first;
                std::set<std::string> differing;
                for(const auto& keyframe : clip.globalKeyframes) {
                    if(!keyframe.values) continue;
                    for(const auto& value : *keyframe.values) {
                        const auto found = first.emplace(value.first, &value.second);
                        if(!found.second && *found.first->second != value.second) differing.insert(value.first);
                    }
                }
                for(const auto& path : differing)
                    if(auto* parameter = base.findTimelineParameter(path))
                        if(globalControlOf(*parameter).control) result.insert(parameter);
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
        const auto resolver = resolverFor(sequence);
        std::string path = resolver.pathOf(parameter);
        if(path.empty()) path = resolver.outsidePathOf(parameter); // driven from inside a macro below it
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

// ---- The automation of copied nodes ----

namespace {
constexpr const char* kClipboardFile = "clipboardTimelines.json";
}

std::string ofxOceanodeTimelineManager::clipboardPresetIdentity() {
    const std::string path = ofxOceanodeShared::getCurrentPresetPath();
    if(!path.empty()) return "preset:" + path;
    // Never saved: the same one for as long as the application runs.
    static const std::string run = "unsaved:" + ofToString(ofGetSystemTimeMicros()) + "-" + ofToString(ofRandom(1e9f), 0);
    return run;
}

void ofxOceanodeTimelineManager::saveClipboardAutomation(ofxOceanodeContainer& canvas, const std::vector<ofxOceanodeNode*>& nodes,
                                                         const std::string& folder) {
    if(container == nullptr || nodes.empty()) return;
    const std::string copied = canvasIdOf(canvas);
    // What was copied, as paths relative to the canvas start: a node's
    // parameters, and everything inside a macro.
    std::vector<std::string> nodePrefixes, macroPrefixes;
    for(auto* node : nodes) {
        if(node == nullptr) continue;
        nodePrefixes.push_back(node->getParameters().getEscapedName() + "/");
        auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel());
        if(macro == nullptr || !macro->getContainer()) continue;
        const std::string inner = relativeTo(copied, macro->getContainer()->getCanvasID());
        if(!inner.empty()) macroPrefixes.push_back(inner);
    }
    auto isCopied = [&](const std::string& path) {
        if(path.empty()) return false;
        for(const auto& prefix : nodePrefixes) if(path.rfind(prefix, 0) == 0) return true;
        for(const auto& prefix : macroPrefixes)
            if(path.rfind(prefix + "::", 0) == 0 || path.rfind(prefix + " / ", 0) == 0) return true;
        return false;
    };
    // The canvas's timelines and those of every canvas around it, from the root down.
    std::vector<std::string> canvases{std::string()};
    for(size_t at = copied.find(" / "); !copied.empty(); at = copied.find(" / ", at + 3)) {
        canvases.push_back(at == std::string::npos ? copied : copied.substr(0, at));
        if(at == std::string::npos) break;
    }
    ofJson timelines = ofJson::array();
    for(const auto& owner : canvases) {
        // The copied canvas as that timeline's paths name it ("": its own).
        const std::string within = owner == copied ? std::string() : relativeTo(owner, copied);
        auto toCopied = [&](const std::string& path) {
            const std::string relative = within.empty() ? path : relativeTo(within, path);
            return isCopied(relative) ? relative : std::string();
        };
        for(const auto& ref : timelinesOfCanvas(owner)) {
            const auto* source = findTimeline(ref);
            if(source == nullptr) continue;
            // A wave, Global or macro track drives no copied parameter by a binding.
            auto drives = [&](const ofxOceanodeTimelineTrack& track) {
                if(track.isFolder || track.isWaveTrack || track.isGlobalTrack || track.isMacroTrack()) return false;
                return std::any_of(track.bindings.begin(), track.bindings.end(),
                                   [&](const auto& binding) { return !toCopied(binding.parameterPath).empty(); });
            };
            // (Copies are made every time nodes are: nothing is, of a timeline that drives none.)
            if(std::none_of(source->tracks.begin(), source->tracks.end(), drives)) continue;
            auto kept = *source;
            kept.tracks.clear();
            for(const auto& original : source->tracks) {
                if(!drives(original)) continue;
                auto track = original;
                // A binding to anything else is left without a path: it is dropped on paste.
                for(auto& binding : track.bindings) binding.parameterPath = toCopied(binding.parameterPath);
                for(auto& clip : track.clips)
                    for(auto& lane : clip.lanes) {
                        std::decay_t<decltype(lane.targetProbability)> relative;
                        for(const auto& [path, chance] : lane.targetProbability)
                            if(const auto moved = toCopied(path); !moved.empty()) relative[moved] = chance;
                        lane.targetProbability = std::move(relative);
                    }
                track.parentId.clear(); // its folder stays behind
                kept.tracks.push_back(std::move(track));
            }
            if(kept.tracks.empty()) continue;
            for(auto& group : kept.clipGroups)
                group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
                    [&](const auto& member) { return kept.findTrack(member.first) == nullptr; }), group.members.end());
            kept.clipGroups.erase(std::remove_if(kept.clipGroups.begin(), kept.clipGroups.end(),
                [](const auto& group) { return group.members.size() < 2; }), kept.clipGroups.end());
            kept.normalizeFolders();
            timelines.push_back({{"name", timelineName(ref)}, {"sequence", sequenceToJson(kept)}});
        }
    }
    if(timelines.empty()) return; // nothing drives them: no file, no question on paste
    ofSavePrettyJson(ofFilePath::join(folder, kClipboardFile),
                     {{"version", kPresetVersion}, {"preset", clipboardPresetIdentity()}, {"timelines", timelines}});
}

void ofxOceanodeTimelineManager::offerClipboardAutomation(ofxOceanodeContainer& canvas, const std::string& folder,
                                                          const std::map<std::string, std::string>& oldToNew) {
    const std::string file = ofFilePath::join(folder, kClipboardFile);
    if(container == nullptr || !ofFile::doesFileExist(file)) return;
    const ofJson document = ofLoadJson(file);
    if(!document.is_object() || !document.contains("timelines") || !document["timelines"].is_array()) return;
    if(document.value("version", kPresetVersion) > kPresetVersion) return; // written by a newer version
    // Pasted where it was copied: its timelines are here already.
    if(document.value("preset", std::string()) == clipboardPresetIdentity()) return;
    ClipboardPasteRequest request;
    request.canvas = canvasIdOf(canvas);
    for(const auto& [from, to] : oldToNew) {
        const std::string relative = relativeTo(request.canvas, to);
        if(!relative.empty()) request.paths[from] = relative;
    }
    request.timelines = ofJson::array();
    for(const auto& entry : document["timelines"]) {
        if(!entry.is_object() || !entry.contains("sequence")) continue;
        // Its tracks that drive something pasted (a node may have failed to).
        int tracks = 0;
        for(const auto& track : entry["sequence"].value("tracks", ofJson::array())) {
            bool drives = false;
            for(const auto& binding : track.value("bindings", ofJson::array()))
                drives = drives || request.paths.count(binding.value("parameterPath", std::string())) > 0;
            tracks += drives ? 1 : 0;
        }
        if(tracks == 0) continue;
        request.tracks += tracks;
        request.names.push_back(entry.value("name", std::string("Timeline")));
        request.timelines.push_back(entry);
    }
    if(request.timelines.empty()) return;
    pendingClipboardPaste = std::move(request);
    clipboardPasteRequested = true;
}

std::vector<ofxOceanodeTimelineManager::TimelineRef>
ofxOceanodeTimelineManager::pasteClipboardAutomation(const ClipboardPasteRequest& request) {
    std::vector<TimelineRef> made;
    if(container == nullptr || canvasContainer(request.canvas) == nullptr || !request.timelines.is_array()) return made;
    for(const auto& entry : request.timelines) {
        const std::string name = entry.value("name", std::string());
        const auto ref = addTimeline(request.canvas, name);
        auto* sequence = ref.timeline.empty() ? nullptr : timelineFor(ref);
        if(sequence == nullptr) continue;
        const std::string given = sequence->play.name;
        sequenceFromJson(*sequence, entry["sequence"]); // keeps its new id and its canvas
        sequence->play.name = given;
        sequence->play.nodesFollowClips = false; // one per canvas: not this one's to take
        // What did not come along: bindings to nodes that were not pasted,
        // then the tracks left driving nothing.
        std::vector<std::pair<std::string, std::string>> gone;
        for(const auto& track : sequence->tracks)
            for(const auto& binding : track.bindings)
                if(request.paths.count(binding.parameterPath) == 0) gone.emplace_back(track.id, binding.id);
        for(const auto& [trackId, bindingId] : gone) removeBinding(*sequence, trackId, bindingId);
        std::vector<std::string> empty;
        for(const auto& track : sequence->tracks) if(track.bindings.empty()) empty.push_back(track.id);
        for(const auto& trackId : empty) removeTrack(*sequence, trackId);
        // Onto the pasted nodes.
        retargetParameterPaths(*sequence, request.paths);
        for(auto& track : sequence->tracks) {
            for(auto& binding : track.bindings) binding.missingTarget = false;
            for(auto& clip : track.clips)
                for(auto& lane : clip.lanes) {
                    std::decay_t<decltype(lane.targetProbability)> moved;
                    for(const auto& [path, chance] : lane.targetProbability)
                        if(const auto to = request.paths.find(path); to != request.paths.end()) moved[to->second] = chance;
                    lane.targetProbability = std::move(moved);
                }
        }
        // The root's timelines flag what they drive here (a macro's: its engine).
        if(request.canvas.empty())
            for(const auto& track : sequence->tracks)
                for(const auto& binding : track.bindings) refreshTimelineFlag(binding.parameterPath);
        sequence->loadStamp = ofxOceanodeTimelineSequence::nextLoadStamp(); // the editor fits it like a loaded one
        sequenceHistories.erase(sequence);
        made.push_back(ref);
    }
    invalidateEvaluationIndexes();
    invalidateSchedule();
    return made;
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

// Here rather than with the rest of the Global Track: walking nodes and
// macros needs the core's node headers, which this file already takes.
namespace {
// The Macro whose canvas `matches`, among `canvas` and the macros in it (and
// a MacroMixer's graphs, whose own Macro is not known here).
bool findGlobalOwner(ofxOceanodeContainer& canvas, ofxOceanodeNodeMacro* owner,
                      const std::function<bool(ofxOceanodeContainer&)>& matches,
                      ofxOceanodeNodeMacro*& result, size_t depth = 0) {
    if(depth >= 64) return false;
    if(matches(canvas)) { result = owner; return true; }
    for(auto* node : canvas.getAllModules()) {
        if(node == nullptr) continue;
        if(auto* macro = dynamic_cast<ofxOceanodeNodeMacro*>(&node->getNodeModel()))
            if(auto inner = macro->getContainer())
                if(findGlobalOwner(*inner, macro, matches, result, depth + 1)) return true;
    }
    for(const auto& guest : canvas.getTimelineGuests())
        if(guest != nullptr && findGlobalOwner(*guest, nullptr, matches, result, depth + 1)) return true;
    return false;
}
}

void ofxOceanodeTimelineManager::captureGlobalValues(ofxOceanodeContainer& base,
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
        findGlobalOwner(*container, nullptr, [&](ofxOceanodeContainer& canvas) { return &canvas == &base; }, owner);
    collect(base, owner);
}

ofxOceanodeTimelineManager::GlobalControl ofxOceanodeTimelineManager::globalControlOf(ofxOceanodeAbstractParameter& parameter) const {
    GlobalControl result;
    auto* model = parameter.getNodeModel();
    if(model == nullptr || container == nullptr) return result;
    if(dynamic_cast<ofxOceanodeNodeMacro*>(model) != nullptr) { result.control = false; return result; }
    ofxOceanodeNodeMacro* owner = nullptr;
    findGlobalOwner(*container, nullptr, [&](ofxOceanodeContainer& canvas) {
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

std::vector<float> ofxOceanodeTimelineManager::blendGlobalNumbers(const std::vector<float>& from, const std::vector<float>& to,
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


// ---- Targets outside a macro ----

// (The paths themselves, ofxOceanodeTimelineOutside, are with the resolver in
// ofxOceanodeTimeline.cpp.)

std::vector<ofxOceanodeContainer*> ofxOceanodeTimelineManager::targetCanvases(const ofxOceanodeTimelineSequence& sequence) const {
    std::vector<ofxOceanodeContainer*> result;
    if(container == nullptr) return result;
    auto* base = sequence.base != nullptr ? sequence.base : container;
    result.push_back(base);
    if(base == container) return result;
    const auto levels = ofxOceanodeTimelineOutside::canvasLevels(base->getCanvasID());
    for(size_t above = levels.size(); above-- > 0;) {
        auto* canvas = above == 0 ? container
            : containerWithCanvas(*container, ofxOceanodeTimelineOutside::joinLevels(levels, 0, above));
        if(canvas != nullptr && canvas != base) result.push_back(canvas);
    }
    return result;
}

namespace {
// Which definition a macro's timelines belong to: none for a local macro, a
// library Macro's folder by its last two names (the same on every machine).
std::string definitionKey(const std::string& libraryFolder) {
    if(libraryFolder.empty()) return std::string();
    std::filesystem::path folder(libraryFolder);
    if(!folder.has_filename()) folder = folder.parent_path(); // a trailing slash
    const std::string name = folder.filename().string();
    const std::string bank = folder.parent_path().filename().string();
    return bank.empty() ? name : bank + "/" + name;
}

// A text member (empty when missing or not text: a hand-edited file).
std::string textOf(const ofJson& json, const char* key) {
    const auto found = json.find(key);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string();
}

const std::array<const char*, 6> kOutsideRoles{{"pitch", "gate", "velocity", "duration", "filter", "stutter"}};
template<typename Lane>
auto roleSlots(Lane& lane) {
    return std::array<decltype(&lane.pianoPitchBindingId), 6>{{&lane.pianoPitchBindingId, &lane.pianoGateBindingId,
        &lane.pianoVelocityBindingId, &lane.drumDurationBindingId, &lane.drumFilterBindingId, &lane.drumStutterBindingId}};
}
} // namespace

ofJson ofxOceanodeTimelineManager::outsideTargetsJson(const std::string& canvas, const ofxOceanodeTimelineCanvas& own) const {
    ofJson result = ofJson::array();
    if(container == nullptr) return result;
    pruneMacroSequences();
    using namespace ofxOceanodeTimelineOutside;
    const auto here = canvasLevels(canvas);
    // (Copies of a library Macro share their timelines: listed once.)
    std::set<std::pair<const ofxOceanodeTimelineCanvas*, size_t>> listed;
    for(const auto& entry : macroSequences) {
        const auto below = canvasLevels(entry.address->getCanvasID());
        if(below.size() <= here.size() || !std::equal(here.begin(), here.end(), below.begin())) continue;
        const size_t depth = below.size() - here.size();
        if(!listed.emplace(entry.timelines.get(), depth).second) continue;
        const std::string macro = joinLevels(below, here.size(), below.size());
        const std::string definition = definitionKey(entry.libraryFolder);
        for(const auto& timeline : entry.timelines->timelines) for(const auto& track : timeline->tracks) {
            for(const auto& binding : track.bindings) {
                // Only those whose parameter lives in this canvas (or a macro in it).
                if(ofxOceanodeTimelineOutside::levels(binding.parameterPath) != depth) continue;
                ofJson link;
                link["macro"] = macro;
                if(!definition.empty()) link["definition"] = definition;
                link["timeline"] = timeline->timelineId;
                link["track"] = track.id;
                link["binding"] = bindingToJson(binding);
                link["binding"]["parameterPath"] = binding.parameterPath.substr(depth * 3); // as this canvas has it
                link["lanes"] = ofJson::array();
                for(const auto& clip : track.clips) for(const auto& lane : clip.lanes) {
                    ofJson roles = ofJson::array();
                    const auto slots = roleSlots(lane);
                    for(size_t i = 0; i < slots.size(); ++i) if(*slots[i] == binding.id) roles.push_back(kOutsideRoles[i]);
                    if(roles.empty() && std::find(lane.bindingIds.begin(), lane.bindingIds.end(), binding.id) == lane.bindingIds.end()) {
                        // (A lane that took it off its Targets list.)
                        if(std::find(lane.hiddenBindingIds.begin(), lane.hiddenBindingIds.end(), binding.id) != lane.hiddenBindingIds.end())
                            link["hiddenIn"].push_back(ofJson{{"clip", clip.id}, {"lane", lane.id}});
                        continue;
                    }
                    ofJson use{{"clip", clip.id}, {"lane", lane.id}};
                    if(!roles.empty()) use["roles"] = roles;
                    const auto chance = lane.targetProbability.find(binding.parameterPath);
                    if(chance != lane.targetProbability.end()) use["probability"] = chance->second;
                    link["lanes"].push_back(std::move(use));
                }
                result.push_back(std::move(link));
            }
        }
    }
    // Those kept for a macro that holds another definition now wait for it
    // (a macro that is gone takes its own with it).
    const std::string prefix = here.empty() ? std::string() : joinLevels(here, 0, here.size()) + " / ";
    for(const auto& link : own.outsideTargets) {
        if(!link.is_object()) continue;
        const std::string macro = textOf(link, "macro");
        if(macro.empty()) continue;
        const auto holder = std::find_if(macroSequences.begin(), macroSequences.end(), [&](const auto& entry) {
            return entry.address->getCanvasID() == prefix + macro;
        });
        if(holder != macroSequences.end() && definitionKey(holder->libraryFolder) != textOf(link, "definition"))
            result.push_back(link);
    }
    return result;
}

void ofxOceanodeTimelineManager::restoreOutsideTargets(const std::string& canvas, const ofxOceanodeTimelineCanvas& keeper) {
    if(container == nullptr) return;
    pruneMacroSequences();
    using namespace ofxOceanodeTimelineOutside;
    const auto here = canvasLevels(canvas);
    // How far below this canvas a macro is (0: not below it).
    auto depthOf = [&](const MacroSequenceEntry& entry, std::string* macro = nullptr) -> size_t {
        const auto below = canvasLevels(entry.address->getCanvasID());
        if(below.size() <= here.size() || !std::equal(here.begin(), here.end(), below.begin()) ||
           entry.timelines.get() == &keeper) return 0;
        if(macro != nullptr) *macro = joinLevels(below, here.size(), below.size());
        return below.size() - here.size();
    };
    bool changed = false;
    // Those they had from this canvas go first (a library Macro's timelines
    // can stay in memory from one project to the next), every macro's before
    // any is given its own: copies of a library Macro share their timelines.
    for(const auto& entry : macroSequences)
        if(const size_t depth = depthOf(entry))
            for(auto& timeline : entry.timelines->timelines) changed = stripOutsideTargets(*timeline, depth) || changed;
    for(const auto& entry : macroSequences) {
        std::string macro;
        if(const size_t depth = depthOf(entry, &macro)) changed = adoptOutsideTargets(entry, keeper, depth, macro) || changed;
    }
    if(changed) { invalidateEvaluationIndexes(); invalidateSchedule(); }
}

bool ofxOceanodeTimelineManager::adoptOutsideTargets(const MacroSequenceEntry& entry, const ofxOceanodeTimelineCanvas& keeper,
                                                     size_t depth, const std::string& macro) {
    if(container == nullptr || entry.address == nullptr || !entry.timelines || keeper.outsideTargets.empty()) return false;
    const std::string definition = definitionKey(entry.libraryFolder);
    std::string up;
    for(size_t i = 0; i < depth; ++i) up += "../";
    bool adopted = false;
    for(const auto& link : keeper.outsideTargets) {
        if(!link.is_object() || textOf(link, "macro") != macro || textOf(link, "definition") != definition) continue;
        const auto saved = link.find("binding");
        if(saved == link.end() || !saved->is_object()) continue;
        auto* sequence = entry.timelines->find(textOf(link, "timeline"));
        // (A track removed from the macro since: nothing left to drive it.)
        auto* track = sequence == nullptr ? nullptr : getTrack(*sequence, textOf(link, "track"));
        const std::string local = textOf(*saved, "parameterPath");
        if(track == nullptr || local.empty()) continue;
        const std::string path = up + local;
        std::string id;
        for(const auto& existing : track->bindings) if(existing.parameterPath == path) id = existing.id;
        if(id.empty()) {
            ofxOceanodeTimelineParameterBinding binding;
            try { bindingFromJson(*saved, binding); } catch(const std::exception&) { continue; }
            binding.parameterPath = path;
            // Its saved id, unless the macro uses it by now.
            binding.id = textOf(*saved, "id");
            bool taken = binding.id.empty();
            for(const auto& other : sequence->tracks)
                for(const auto& existing : other.bindings) taken = taken || existing.id == binding.id;
            if(taken) { binding.id = sequence->makeUniqueBindingId(); ++sequence->nextBindingNumber; }
            id = binding.id;
            track->bindings.push_back(std::move(binding));
        }
        adopted = true;
        const auto uses = link.find("lanes");
        if(uses == link.end() || !uses->is_array()) continue;
        for(const auto& use : *uses) {
            if(!use.is_object()) continue;
            auto* lane = getLane(*sequence, track->id, textOf(use, "clip"), textOf(use, "lane"));
            if(lane == nullptr) continue; // removed from the macro: the target stays on its track
            if(std::find(lane->bindingIds.begin(), lane->bindingIds.end(), id) == lane->bindingIds.end())
                lane->bindingIds.push_back(id);
            const auto roles = use.find("roles");
            if(roles != use.end() && roles->is_array()) {
                const auto slots = roleSlots(*lane);
                for(const auto& role : *roles)
                    for(size_t i = 0; i < slots.size(); ++i)
                        if(role.is_string() && role.get<std::string>() == kOutsideRoles[i] && slots[i]->empty()) *slots[i] = id;
            }
            const auto chance = use.find("probability");
            if(chance != use.end() && chance->is_number()) lane->targetProbability[path] = chance->get<float>();
        }
        // The lanes that took it off their Targets list.
        const auto hidden = link.find("hiddenIn");
        if(hidden == link.end() || !hidden->is_array()) continue;
        for(const auto& use : *hidden) {
            if(!use.is_object()) continue;
            auto* lane = getLane(*sequence, track->id, textOf(use, "clip"), textOf(use, "lane"));
            if(lane == nullptr) continue;
            auto has = [&](const std::vector<std::string>& ids) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
            if(!has(lane->bindingIds) && !has(lane->hiddenBindingIds)) lane->hiddenBindingIds.push_back(id);
        }
    }
    return adopted;
}

bool ofxOceanodeTimelineManager::stripOutsideTargets(ofxOceanodeTimelineSequence& sequence, size_t levels) {
    auto leaves = [levels](const std::string& path) {
        const size_t up = ofxOceanodeTimelineOutside::levels(path);
        return up > 0 && (levels == 0 || up == levels);
    };
    bool changed = false;
    for(auto& track : sequence.tracks) {
        std::set<std::string> gone;
        for(const auto& binding : track.bindings) if(leaves(binding.parameterPath)) gone.insert(binding.id);
        if(gone.empty()) continue;
        changed = true;
        for(auto& clip : track.clips) for(auto& lane : clip.lanes) {
            lane.bindingIds.erase(std::remove_if(lane.bindingIds.begin(), lane.bindingIds.end(),
                [&](const std::string& id) { return gone.count(id) > 0; }), lane.bindingIds.end());
            lane.hiddenBindingIds.erase(std::remove_if(lane.hiddenBindingIds.begin(), lane.hiddenBindingIds.end(),
                [&](const std::string& id) { return gone.count(id) > 0; }), lane.hiddenBindingIds.end());
            for(auto* slot : roleSlots(lane)) if(gone.count(*slot) > 0) slot->clear();
            for(auto chance = lane.targetProbability.begin(); chance != lane.targetProbability.end();)
                chance = leaves(chance->first) ? lane.targetProbability.erase(chance) : std::next(chance);
        }
        for(auto& group : track.noteGroups)
            for(auto* slot : {&group.pitchBindingId, &group.gateBindingId, &group.velocityBindingId,
                              &group.durationBindingId, &group.filterBindingId, &group.stutterBindingId})
                if(gone.count(*slot) > 0) slot->clear();
        track.bindings.erase(std::remove_if(track.bindings.begin(), track.bindings.end(),
            [&](const auto& binding) { return leaves(binding.parameterPath); }), track.bindings.end());
    }
    return changed;
}
