// One timeline's arrangement: lookups and new ids (see ofxOceanodeTimelineSequence).

#include "Timeline/ofxOceanodeTimeline.h"

#include <algorithm>

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
