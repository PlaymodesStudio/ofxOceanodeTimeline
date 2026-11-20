#!/usr/bin/env python3
"""Wave clips: the envelope cache (min/max bins over the whole file, what a pixel column reads) and fades."""
import timeline_regression as fixture

CASES = r'''
#include "Managers/ofxOceanodeContainer.h"
#include "Timeline/ofxOceanodeTimeline.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
void ofxOceanodeAbstractParameter::removeAllConnections() {
    assert(!hasInConnection() && !hasOutConnections() && !isScoped());
}
namespace Wave = ofxOceanodeTimelineWaveform;
// A 16 bit PCM file; statedBytes < 0 writes the true data size.
void writeWav(const std::filesystem::path& path, int channels, const std::vector<int16_t>& samples, long statedBytes = -1) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    out.write("RIFF", 4); u32(36 + bytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(channels); u32(48000); u32(48000 * channels * 2); u16(channels * 2); u16(16);
    out.write("data", 4); u32(statedBytes < 0 ? bytes : static_cast<uint32_t>(statedBytes));
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
}
int main() {
    ofSeedRandom();
    ofxOceanodeContainer project;
    TestTimeline timeline(&project);
    const auto folder = std::filesystem::temp_directory_path() / "oceanode-waveform-cache";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto track = timeline.createTrack("Audio");
    timeline.getTrack(track)->isWaveTrack = true;
    const auto clipId = timeline.createClip(track, "Clip", 0, 4);
    auto load = [&](const std::string& name) -> const ofxOceanodeTimelineClip& {
        auto* clip = timeline.getClip(track, clipId);
        clip->waveFilePath = (folder / name).string();
        assert(timeline.reloadWaveform(track, clipId));
        return *clip;
    };

    // A sine: both sides of every bin, the whole way through.
    std::vector<int16_t> sine(48000);
    for(size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<int16_t>(std::sin(i * 0.2) * 30000.0);
    writeWav(folder / "sine.wav", 1, sine);
    {
        const auto& clip = load("sine.wav");
        const int bins = Wave::binCount(clip.waveformPeaks, clip.waveNumChannels);
        assert(clip.waveNumChannels == 1 && bins == Wave::kMaxBins && std::abs(clip.waveFileDurationMs - 1000.0) < 1e-6);
        for(int bin = 0; bin < bins; ++bin) {
            assert(clip.waveformPeaks[bin * 2] <= clip.waveformPeaks[bin * 2 + 1]);
            assert(std::abs(clip.waveformPeaks[bin * 2 + 1]) <= 0.92f && std::abs(clip.waveformPeaks[bin * 2]) <= 0.92f);
        }
        // Normalize: the gain that takes the loudest peak to full scale, never over it.
        assert(std::abs(Wave::peak(clip, 120.0f) - 30000.0f / 32768.0f) < 0.002f);
        assert(timeline.normalizeWaveClip(timeline.editSequence(), track, clipId));
        const float normalized = timeline.getClip(track, clipId)->waveGain;
        assert(normalized > 1.085f && normalized * Wave::peak(clip, 120.0f) <= 1.0f && normalized * Wave::peak(clip, 120.0f) > 0.998f);
        timeline.getClip(track, clipId)->waveGain = 1.0f;
        float low = 0, high = 0;
        assert(Wave::range(clip.waveformPeaks, 1, 0, 0.25, 0.30, low, high));
        assert(low < -0.9f && high > 0.9f && std::abs(low + high) < 0.02f);
        // Narrower than a bin: interpolated, inside the neighbouring bins' range.
        assert(Wave::range(clip.waveformPeaks, 1, 0, 0.5, 0.5 + 0.1 / bins, low, high) && low <= high && high <= 0.92f && low >= -0.92f);
        assert(!Wave::range(clip.waveformPeaks, 1, 1, 0.0, 1.0, low, high) && !Wave::range(clip.waveformPeaks, 1, 0, 0.5, 0.5, low, high));
    }

    // A length that is no multiple of the bin count: the last frame is in the
    // last bin (the cache used to stop short and stretch what it had).
    std::vector<int16_t> impulse(6143, 0);
    impulse.back() = 32767;
    impulse[3000] = -32768;
    writeWav(folder / "impulse.wav", 1, impulse);
    {
        const auto& clip = load("impulse.wav");
        const int bins = Wave::binCount(clip.waveformPeaks, 1);
        assert(bins == Wave::kMaxBins && clip.waveformPeaks[(bins - 1) * 2 + 1] > 0.99f);
        const int middle = static_cast<int>(3000.0 / 6143.0 * bins);
        assert(clip.waveformPeaks[middle * 2] == -1.0f);
        float low = 0, high = 0;
        assert(Wave::range(clip.waveformPeaks, 1, 0, 0.0, 0.4, low, high) && low == 0.0f && high == 0.0f);
        assert(Wave::range(clip.waveformPeaks, 1, 0, 0.9, 1.0, low, high) && high > 0.99f && low == 0.0f);
    }

    // Fewer frames than bins: a bin a frame, no full-scale filler after them.
    // Stereo: each channel keeps its own bins.
    std::vector<int16_t> tiny;
    for(int i = 0; i < 100; ++i) { tiny.push_back(8192); tiny.push_back(-16384); }
    writeWav(folder / "tiny.wav", 2, tiny);
    {
        const auto& clip = load("tiny.wav");
        assert(clip.waveNumChannels == 2 && Wave::binCount(clip.waveformPeaks, 2) == 100);
        for(int bin = 0; bin < 100; ++bin) {
            assert(clip.waveformPeaks[bin * 2] == 0.25f && clip.waveformPeaks[bin * 2 + 1] == 0.25f);
            assert(clip.waveformPeaks[200 + bin * 2] == -0.5f && clip.waveformPeaks[200 + bin * 2 + 1] == -0.5f);
        }
    }

    // A header stating more data than the file holds: what is there counts.
    writeWav(folder / "cut.wav", 1, std::vector<int16_t>(480, 16384), 96000);
    {
        const auto& clip = load("cut.wav");
        assert(std::abs(clip.waveFileDurationMs - 10.0) < 1e-6 && Wave::binCount(clip.waveformPeaks, 1) == 480);
        for(float value : clip.waveformPeaks) assert(value == 0.5f);
    }

    // Fades: the clip's volume rises over the first and falls over the second,
    // never crossing, and that is what the audio provider is given.
    {
        struct Probe : ofxOceanodeTimelineWaveAudioProvider {
            float volume = -1.0f, gain = -1.0f;
            std::map<std::string, float> volumes; // by clip
            std::map<std::string, bool> stretched;
            void setWaveClipTimestretch(const std::string&, const std::string& clip, bool timestretch) override { stretched[clip] = timestretch; }
            void updateWaveClip(const std::string&, const std::string& clip, const std::string&,
                float clipGain, float trackVolume, int, double, double, double, double, float, bool, float,
                bool, bool, bool) override { gain = clipGain; volume = trackVolume; volumes[clip] = trackVolume; }
            void releaseWaveClip(const std::string&, const std::string&) override {}
        } probe;
        auto* clip = timeline.getClip(track, clipId);
        auto* audio = timeline.getTrack(track);
        audio->waveVolume = 0.8f;
        audio->waveVolumePoints = {{0.0, 0.8f}, {4.0, 0.8f}};
        audio->waveVolumeTensions.assign(1, ofxOceanodeTimelineCurveTension{});
        clip->startBeat = 0; clip->durationBeats = 4; clip->waveGain = 0.5f;
        assert(timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 2.0, 1.0));
        assert(!timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 2.0, 1.0));
        assert(Wave::fadeGain(*clip, 0.0) == 0.0f && Wave::fadeGain(*clip, 1.0) == 0.5f && Wave::fadeGain(*clip, 2.5) == 1.0f);
        assert(std::abs(Wave::fadeGain(*clip, 3.5) - 0.5f) < 1e-6f && Wave::fadeGain(*clip, 4.0) == 0.0f);
        // The one being changed stops at the other; both stay inside the clip.
        assert(timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 9.0, 1.0) && clip->waveFadeInBeats == 3.0);
        assert(!timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 3.0, 9.0) && clip->waveFadeOutBeats == 1.0);
        assert(timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 1.0, 9.0) && clip->waveFadeOutBeats == 3.0);
        assert(timeline.setWaveClipFades(timeline.editSequence(), track, clipId, 2.0, 1.0));
        timeline.setSchedulingEnabled(false);
        timeline.setWaveAudioProvider(&probe);
        project.transport->setDriverMode(TransportDriverMode::External);
        for(const auto& at : std::vector<std::pair<double, float>>{{1.0, 0.4f}, {2.5, 0.8f}, {3.5, 0.4f}}) {
            project.transport->seekToBeat(at.first); timeline.update();
            assert(std::abs(probe.volume - at.second) < 1e-4f && probe.gain == 0.5f);
        }
        // The curve of each fade is a sigmoid segment: an S by default, a line when neutral.
        assert(Wave::fadeGain(*clip, 0.5) < 0.2f && Wave::fadeGain(*clip, 1.5) > 0.8f);
        clip->waveFadeInTension = ofxOceanodeTimelineCurveTension{};
        assert(std::abs(Wave::fadeGain(*clip, 0.5) - 0.25f) < 1e-6f);
        clip->waveFadeInTension = {0.3f, 4.0f};
        // A second clip starting over the first one's end: they crossfade over
        // the overlap, on the curve the incoming clip names; the fade the first
        // had there gives way, and its other end keeps its own.
        const auto second = timeline.createClip(track, "Second", 3.0, 4.0);
        clip = timeline.getClip(track, clipId);
        audio = timeline.getTrack(track);
        const auto* incoming = timeline.getClip(track, second);
        // A neighbour with no file makes no sound: nothing to crossfade with.
        assert(!Wave::resolveFades(*audio, *clip).crossOut);
        timeline.getClip(track, second)->waveFilePath = clip->waveFilePath;
        auto a = Wave::resolveFades(*audio, *clip), b = Wave::resolveFades(*audio, *incoming);
        assert(a.crossOut && !a.crossIn && a.out == 1.0 && a.in == 2.0 && a.crossOutClipId == second && a.outCurve == "Equal power");
        assert(b.crossIn && !b.crossOut && b.in == 1.0 && b.out == 0.0);
        for(double at : {3.0, 3.25, 3.5, 3.9}) {
            const float out = Wave::fadeGain(a, *clip, at), in = Wave::fadeGain(b, *incoming, at - 3.0);
            assert(std::abs(out * out + in * in - 1.0f) < 1e-4f); // equal power
        }
        timeline.getClip(track, second)->waveCrossfadeCurve = "Linear";
        a = Wave::resolveFades(*audio, *clip); b = Wave::resolveFades(*audio, *incoming);
        assert(std::abs(Wave::fadeGain(a, *clip, 3.25) - 0.75f) < 1e-6f && std::abs(Wave::fadeGain(b, *incoming, 0.25) - 0.25f) < 1e-6f);
        // Each clip's own timestretch switch reaches the provider with it.
        assert(!probe.stretched[clipId] && !probe.stretched[second]);
        timeline.getClip(track, second)->waveTimestretch = true;
        timeline.update();
        assert(!probe.stretched[clipId] && probe.stretched[second]);
        // Both reach the audio provider through the crossfade.
        project.transport->seekToBeat(3.25); timeline.update();
        assert(std::abs(probe.volumes[clipId] - 0.8f * 0.75f) < 1e-4f && std::abs(probe.volumes[second] - 0.8f * 0.25f) < 1e-4f);
        assert(Wave::crossfadeCurves().size() == 5 && Wave::crossfadeCurves().front() == "Equal power");
        for(const auto& curve : Wave::crossfadeCurves())
            assert(Wave::crossfadeShape(curve, 0.0f) == 0.0f && std::abs(Wave::crossfadeShape(curve, 1.0f) - 1.0f) < 1e-6f);
        // A muted neighbour, or one lying wholly inside, does not crossfade.
        timeline.getClip(track, second)->muted = true;
        assert(!Wave::resolveFades(*audio, *clip).crossOut && Wave::resolveFades(*audio, *clip).out == 1.0);
        timeline.getClip(track, second)->muted = false;
        timeline.getClip(track, second)->startBeat = 1.0; timeline.getClip(track, second)->durationBeats = 1.0;
        assert(!Wave::resolveFades(*audio, *clip).crossOut && !Wave::resolveFades(*audio, *incoming).crossIn);
        timeline.removeClip(track, second);
        clip = timeline.getClip(track, clipId);
        audio = timeline.getTrack(track);
        clip->waveCrossfadeCurve = "S-curve";
        clip->waveTimestretch = true;
        // The track's volume curve is made of sigmoid segments: straight until
        // bent, and a point added or removed leaves the other segments' bends alone.
        assert(audio->waveVolumeInterpolation == "Sigmoid");
        audio->waveVolumePoints = {{0.0, 0.0f}, {4.0, 1.0f}, {8.0, 0.0f}};
        audio->waveVolumeTensions = {{0.5f, 1.0f}, {0.3f, 3.0f}};
        assert(std::abs(timeline.evaluateWaveTrackVolume(*audio, 1.0) - 0.25f) < 1e-6f);
        audio->waveVolumeTensions[0] = {0.5f, 3.0f};
        assert(timeline.evaluateWaveTrackVolume(*audio, 1.0) < 0.1f && std::abs(timeline.evaluateWaveTrackVolume(*audio, 2.0) - 0.5f) < 1e-5f);
        assert(timeline.addWaveTrackVolumePoint(timeline.editSequence(), track, 2.0, 0.5f));
        assert(audio->waveVolumePoints.size() == 4 && audio->waveVolumeTensions.size() == 3);
        assert(audio->waveVolumeTensions[0].steepness == 3.0f && audio->waveVolumeTensions[1].steepness == 1.0f && audio->waveVolumeTensions[2].inflection == 0.3f);
        assert(timeline.removeWaveTrackVolumePoint(timeline.editSequence(), track, 2.0, 1e-6));
        assert(audio->waveVolumePoints.size() == 3 && audio->waveVolumeTensions.size() == 2);
        assert(audio->waveVolumeTensions[0].steepness == 3.0f && audio->waveVolumeTensions[1].inflection == 0.3f);
        timeline.setWaveAudioProvider(nullptr);
        // Saved and read back, with the track's "waveform follows volume".
        audio->waveVolumeShapesWaveform = true;
        const auto saved = timeline.toJson();
        timeline.fromJson(saved);
        auto* loadedTrack = timeline.getTrack(track);
        assert(loadedTrack && loadedTrack->waveVolumeShapesWaveform && loadedTrack->clips.size() == 1);
        assert(loadedTrack->waveVolumeInterpolation == "Sigmoid" && loadedTrack->waveVolumeTensions.size() == 2 && loadedTrack->waveVolumeTensions[1].steepness == 3.0f);
        {
            // A file from before: its straight segments load as neutral sigmoid ones.
            auto straight = saved;
            for(auto* tracks : {&straight["tracks"], &straight["timelines"][0]["tracks"]}) for(auto& t : *tracks) t["waveVolumeInterpolation"] = "Linear";
            timeline.fromJson(straight);
            const auto* old = timeline.getTrack(track);
            assert(old->waveVolumeInterpolation == "Sigmoid" && old->waveVolumeTensions[0].steepness == 1.0f && old->waveVolumeTensions[1].inflection == 0.5f);
            assert(std::abs(timeline.evaluateWaveTrackVolume(*old, 1.0) - 0.25f) < 1e-6f);
            timeline.fromJson(saved);
            loadedTrack = timeline.getTrack(track);
        }
        assert(loadedTrack->clips[0].waveFadeInBeats == 2.0 && loadedTrack->clips[0].waveFadeOutBeats == 1.0);
        assert(loadedTrack->clips[0].waveFadeInTension.inflection == 0.3f && loadedTrack->clips[0].waveFadeInTension.steepness == 4.0f);
        assert(loadedTrack->clips[0].waveFadeOutTension.steepness == 2.0f && loadedTrack->clips[0].waveCrossfadeCurve == "S-curve" && loadedTrack->clips[0].waveTimestretch);
        auto legacy = saved;
        for(auto* tracks : {&legacy["tracks"], &legacy["timelines"][0]["tracks"]})
            for(auto& t : *tracks) { t.erase("waveVolumeShapesWaveform"); for(auto& c : t["clips"]) { for(const char* key : {"waveFadeInBeats", "waveFadeOutBeats", "waveFadeInTension", "waveFadeOutTension", "waveCrossfadeCurve", "waveTimestretch"}) c.erase(key); } }
        timeline.fromJson(legacy);
        assert(!timeline.getTrack(track)->waveVolumeShapesWaveform && timeline.getTrack(track)->clips[0].waveFadeInBeats == 0.0);
        assert(timeline.getTrack(track)->clips[0].waveFadeInTension.steepness == 2.0f && timeline.getTrack(track)->clips[0].waveCrossfadeCurve == "Equal power" && !timeline.getTrack(track)->clips[0].waveTimestretch);
    }

    // A silent clip has nothing to normalize to; a very quiet one stops at the gain's range.
    {
        writeWav(folder / "silent.wav", 1, std::vector<int16_t>(4800, 0));
        load("silent.wav");
        timeline.getClip(track, clipId)->waveGain = 0.7f;
        assert(!timeline.normalizeWaveClip(timeline.editSequence(), track, clipId) && timeline.getClip(track, clipId)->waveGain == 0.7f);
        writeWav(folder / "quiet.wav", 1, std::vector<int16_t>(4800, 328));
        load("quiet.wav");
        assert(timeline.normalizeWaveClip(timeline.editSequence(), track, clipId) && timeline.getClip(track, clipId)->waveGain == 4.0f);
    }

    // Caches of any other shape are not read.
    float low = 0, high = 0;
    assert(Wave::binCount({0.1f, 0.5f, -0.5f}, 1) == 0 && Wave::binCount({0.1f, 0.5f}, 2) == 0 && Wave::binCount({}, 1) == 0);
    assert(!Wave::range({0.1f, 0.5f, -0.5f}, 1, 0, 0.0, 1.0, low, high));
    assert(Wave::range({-0.2f, 0.4f}, 1, 0, 0.0, 1.0, low, high) && low == -0.2f && high == 0.4f);
    assert(Wave::range({-0.2f, 0.4f}, 1, 0, 0.2, 0.3, low, high) && low == -0.2f && high == 0.4f);
    std::filesystem::remove_all(folder);
    std::cout << "Waveform cache passed: min/max bins, whole-file coverage, short and cut files, stereo, column ranges, fades to the audio provider and saved.\n";
}
'''
if __name__ == '__main__':
    fixture.main(cases=CASES)
