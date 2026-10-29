// One timeline's arrangement: lookups and new ids (see ofxOceanodeTimelineSequence).

#include "Timeline/ofxOceanodeTimeline.h"

#include <algorithm>
#include <functional>

ofxOceanodeTimelineTrack* ofxOceanodeTimelineSequence::findTrack(const std::string& trackId) {
    auto it = std::find_if(tracks.begin(), tracks.end(), [&](auto& track) { return track.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineTrack* ofxOceanodeTimelineSequence::findTrack(const std::string& trackId) const {
    auto it = std::find_if(tracks.begin(), tracks.end(), [&](const auto& track) { return track.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

ofxOceanodeTimelineClip* ofxOceanodeTimelineSequence::findClip(const std::string& trackId, const std::string& clipId) {
    auto* track = findTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineClip* ofxOceanodeTimelineSequence::findClip(const std::string& trackId,
                                                                     const std::string& clipId) const {
    const auto* track = findTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto& clip) { return clip.id == clipId; });
    return it == track->clips.end() ? nullptr : &*it;
}

ofxOceanodeTimelineLane* ofxOceanodeTimelineSequence::findLane(const std::string& trackId, const std::string& clipId,
                                                               const std::string& laneId) {
    auto* clip = findClip(trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineLane* ofxOceanodeTimelineSequence::findLane(const std::string& trackId, const std::string& clipId,
                                                                     const std::string& laneId) const {
    const auto* clip = findClip(trackId, clipId);
    if(clip == nullptr) return nullptr;
    auto it = std::find_if(clip->lanes.begin(), clip->lanes.end(), [&](const auto& lane) { return lane.id == laneId; });
    return it == clip->lanes.end() ? nullptr : &*it;
}

ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineSequence::findBinding(const std::string& trackId,
                                                                              const std::string& bindingId) {
    auto* track = findTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](auto& binding) { return binding.id == bindingId; });
    return it == track->bindings.end() ? nullptr : &*it;
}

const ofxOceanodeTimelineParameterBinding* ofxOceanodeTimelineSequence::findBinding(const std::string& trackId,
                                                                                    const std::string& bindingId) const {
    const auto* track = findTrack(trackId);
    if(track == nullptr) return nullptr;
    auto it = std::find_if(track->bindings.begin(), track->bindings.end(), [&](const auto& binding) { return binding.id == bindingId; });
    return it == track->bindings.end() ? nullptr : &*it;
}

std::string ofxOceanodeTimelineSequence::makeId(const char* prefix, uint64_t number) {
    return std::string(prefix) + "_" + ofToString(number);
}

std::string ofxOceanodeTimelineSequence::makeUniqueTrackName(const std::string& requestedName) const {
    const std::string base = requestedName.empty() ? "Timeline Track" : requestedName;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) { return track.name == candidate; });
    };
    if(!exists(base)) return base;
    for(int suffix = 2; ; ++suffix) {
        const std::string candidate = base + " " + ofToString(suffix);
        if(!exists(candidate)) return candidate;
    }
}

std::string ofxOceanodeTimelineSequence::makeUniqueClipName(const ofxOceanodeTimelineTrack& track,
                                                            const std::string& requestedName) {
    const std::string base = requestedName.empty() ? "Clip" : requestedName;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(track.clips.begin(), track.clips.end(), [&](const auto& clip) {
            return clip.name == candidate;
        });
    };
    if(!exists(base)) return base;
    for(int suffix = 2; ; ++suffix) {
        const std::string candidate = base + " " + ofToString(suffix);
        if(!exists(candidate)) return candidate;
    }
}

std::string ofxOceanodeTimelineSequence::makeUniqueTrackId() const {
    std::string id;
    uint64_t number = nextTrackNumber;
    do {
        id = makeId("timeline_track", number++);
    } while(findTrack(id) != nullptr);
    return id;
}

std::string ofxOceanodeTimelineSequence::makeUniqueBindingId() const {
    std::string id;
    uint64_t number = nextBindingNumber;
    do {
        id = makeId("timeline_binding", number++);
        bool used = false;
        for(const auto& track : tracks) {
            used = std::any_of(track.bindings.begin(), track.bindings.end(), [&](const auto& binding) { return binding.id == id; });
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineSequence::makeUniqueClipId() const {
    std::string id;
    uint64_t number = nextClipNumber;
    do {
        id = makeId("timeline_clip", number++);
        bool used = false;
        for(const auto& track : tracks) {
            used = std::any_of(track.clips.begin(), track.clips.end(), [&](const auto& clip) {
                return clip.id == id;
            });
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineSequence::makeUniqueLaneId() const {
    std::string id;
    uint64_t number = nextLaneNumber;
    do {
        id = makeId("timeline_lane", number++);
        bool used = false;
        for(const auto& track : tracks) {
            for(const auto& clip : track.clips) {
                used = std::any_of(clip.lanes.begin(), clip.lanes.end(), [&](const auto& lane) {
                    return lane.id == id;
                });
                if(used) break;
            }
            if(used) break;
        }
        if(!used) return id;
    } while(true);
}

std::string ofxOceanodeTimelineSequence::makeUniqueGroupId() const {
    std::string id;
    uint64_t number = nextGroupNumber;
    do {
        id = makeId("timeline_group", number++);
    } while(std::any_of(clipGroups.begin(), clipGroups.end(), [&](const auto& group) { return group.id == id; }));
    return id;
}

const ofxOceanodeTimelineTrack* ofxOceanodeTimelineSequence::parentOf(const ofxOceanodeTimelineTrack& track) const {
    if(track.parentId.empty()) return nullptr;
    const auto* parent = findTrack(track.parentId);
    return parent != nullptr && parent->isFolder ? parent : nullptr;
}

int ofxOceanodeTimelineSequence::depthOf(const ofxOceanodeTimelineTrack& track) const {
    int depth = 0;
    for(const auto* parent = parentOf(track); parent != nullptr && depth <= static_cast<int>(tracks.size()); parent = parentOf(*parent))
        ++depth;
    return depth;
}

bool ofxOceanodeTimelineSequence::isWithin(const std::string& trackId, const std::string& folderId) const {
    if(trackId == folderId) return true;
    const auto* track = findTrack(trackId);
    for(size_t steps = 0; track != nullptr && steps <= tracks.size(); ++steps) {
        track = parentOf(*track);
        if(track != nullptr && track->id == folderId) return true;
    }
    return false;
}

size_t ofxOceanodeTimelineSequence::subtreeEnd(size_t index) const {
    if(index >= tracks.size()) return tracks.size();
    const int depth = depthOf(tracks[index]);
    size_t end = index + 1;
    while(end < tracks.size() && depthOf(tracks[end]) > depth) ++end;
    return end;
}

void ofxOceanodeTimelineSequence::normalizeFolders() {
    // Parents must exist, be folders and never contain their own child.
    for(auto& track : tracks) {
        if(track.parentId.empty()) continue;
        const auto* parent = findTrack(track.parentId);
        if(parent == nullptr || !parent->isFolder || isWithin(track.parentId, track.id)) track.parentId.clear();
    }
    if(std::none_of(tracks.begin(), tracks.end(), [](const auto& track) { return !track.parentId.empty(); })) return;
    // Depth-first, siblings in their current order.
    std::vector<size_t> order;
    order.reserve(tracks.size());
    std::vector<bool> placed(tracks.size(), false);
    std::function<void(const std::string&)> place = [&](const std::string& parentId) {
        for(size_t i = 0; i < tracks.size(); ++i) {
            if(placed[i] || tracks[i].parentId != parentId) continue;
            placed[i] = true;
            order.push_back(i);
            if(tracks[i].isFolder) place(tracks[i].id);
        }
    };
    place(std::string());
    bool moved = false;
    for(size_t i = 0; i < order.size(); ++i) moved = moved || order[i] != i;
    if(!moved) return; // already in order: tracks (and pointers to them) stay put
    std::vector<ofxOceanodeTimelineTrack> ordered;
    ordered.reserve(tracks.size());
    for(const size_t i : order) ordered.push_back(std::move(tracks[i]));
    tracks = std::move(ordered);
}
