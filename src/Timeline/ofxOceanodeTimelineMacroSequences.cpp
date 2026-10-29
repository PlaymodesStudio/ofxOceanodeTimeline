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
    for(auto& entry : macroSequences)
        entry.sequence->base = const_cast<ofxOceanodeContainer*>(entry.address);
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
    if(found == macroSequences.end() || found->sequence->tracks.empty()) {
        if(ofFile::doesFileExist(file)) ofFile::removeFile(file);
        return true;
    }
    return ofSavePrettyJson(file, sequenceToJson(*found->sequence));
}

bool ofxOceanodeTimelineManager::isLiveSequence(const ofxOceanodeTimelineSequence& sequence) const {
    if(isMainSequence(sequence)) return true;
    pruneMacroSequences();
    return std::any_of(macroSequences.begin(), macroSequences.end(),
        [&](const auto& entry) { return entry.sequence.get() == &sequence; });
}
