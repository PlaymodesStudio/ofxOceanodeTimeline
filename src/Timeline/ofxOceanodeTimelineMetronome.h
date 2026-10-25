#pragma once

#include "ofSoundStream.h"
#include "ofxOceanodeTransport.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#ifdef TARGET_OSX
#include <CoreAudio/CoreAudio.h>
#endif

// A private output stream: no sound assets or changes to the host's audio callback.
// Transport is sampled once per buffer; real-time playback advances per sample.
class ofxOceanodeTimelineMetronome : public ofBaseSoundOutput {
public:
    explicit ofxOceanodeTimelineMetronome(std::shared_ptr<ofxOceanodeTransport> transport)
    : transport(std::move(transport)) {}
    ~ofxOceanodeTimelineMetronome() { stream.close(); }

    bool start() {
        ofSoundStreamSettings settings;
        settings.setOutListener(this);
        settings.numInputChannels = 0;
        settings.numOutputChannels = 2;
        settings.sampleRate = 48000;
#ifdef TARGET_OSX
        settings.setApi(ofSoundDevice::Api::OSX_CORE);
        // RtAudio can change the shared hardware clock when opening a stream.
        // Match the active device rate so existing players keep their pitch.
        AudioDeviceID device = kAudioObjectUnknown;
        UInt32 size = sizeof(device);
        AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                &size, &device) != noErr || device == kAudioObjectUnknown) {
            ofLogWarning("ofxOceanodeTimeline") << "Cannot read default metronome output device";
            return false;
        }
        Float64 deviceRate = 0;
        size = sizeof(deviceRate);
        address.mSelector = kAudioDevicePropertyNominalSampleRate;
        if(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &deviceRate) != noErr ||
                !std::isfinite(deviceRate) || deviceRate < 1 || deviceRate > 768000) {
            ofLogWarning("ofxOceanodeTimeline") << "Cannot read metronome output sample rate";
            return false;
        }
        settings.sampleRate = static_cast<unsigned int>(std::lround(deviceRate));
#endif
        settings.bufferSize = 256;
        settings.numBuffers = 2;
        return stream.setup(settings);
    }

    void setMeter(int numerator, int denominator) {
        meter.store((static_cast<uint64_t>(numerator) << 32) | static_cast<uint32_t>(denominator));
    }

    void audioOut(ofSoundBuffer& buffer) override {
        render(buffer, transport->getState(), ofxOceanodeTransport::steadyNowUs());
    }

    // Also usable without an audio device for deterministic render checks.
    void render(ofSoundBuffer& buffer, const ofxOceanodeTransportState& state, uint64_t nowUs) {
        buffer.set(0.0f);
        if(!state.isPlaying) {
            tracking = false;
            age = duration;
            return;
        }
        const auto signature = meter.load();
        const int numerator = std::max(1, static_cast<int>(signature >> 32));
        const double pulseBeats = 4.0 / std::max(1u, static_cast<uint32_t>(signature));
        const double sampleRate = std::max(1u, buffer.getSampleRate());
        const double elapsed = nowUs >= state.steadyTimeUs ? (nowUs - state.steadyTimeUs) / 1000000.0 : 0.0;
        for(size_t frame = 0; frame < buffer.getNumFrames(); ++frame) {
            double beat = state.beatPosition;
            if(state.driverMode == TransportDriverMode::RealTime)
                beat += (elapsed + frame / sampleRate) * state.bpm / 60.0;
            uint64_t loop = state.loopCount;
            if(state.loopActive() && beat >= state.loopEndBeat) {
                const double length = state.loopEndBeat - state.loopStartBeat;
                loop += static_cast<uint64_t>(std::floor((beat - state.loopStartBeat) / length));
                beat = state.loopStartBeat + std::fmod(beat - state.loopStartBeat, length);
            }
            const auto pulse = static_cast<int64_t>(std::floor((beat + 1e-9) / pulseBeats));
            const bool reset = !tracking || generation != state.generation || lastMeter != signature;
            const bool crossed = !reset && (loop > lastLoop || (loop == lastLoop && pulse > lastPulse));
            // Starting/seeking between beats waits for the next beat. A frame
            // clock may report slightly after a boundary, so allow 20 ms there.
            const bool atBoundary = (beat - pulse * pulseBeats) * 60.0 / std::max(1.0f, state.bpm) < 0.020;
            if(reset) age = duration;
            if((crossed && (loop == lastLoop || atBoundary)) || (reset && atBoundary)) {
                age = 0.0;
                strong = pulse % numerator == 0;
            }
            if(reset || crossed) {
                lastPulse = pulse;
                lastLoop = loop;
            }
            tracking = true;
            generation = state.generation;
            lastMeter = signature;
            if(age < duration) {
                const double attack = std::min(1.0, age / 0.001);
                const double envelope = attack * std::exp(-age * 180.0) * (1.0 - age / duration);
                const float sample = static_cast<float>((strong ? 0.32 : 0.20) * envelope *
                    std::sin(6.283185307179586 * (strong ? 1760.0 : 1174.66) * age));
                for(size_t channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer[frame * buffer.getNumChannels() + channel] = sample;
                age += 1.0 / sampleRate;
            }
        }
    }

private:
    std::shared_ptr<ofxOceanodeTransport> transport;
    std::atomic<uint64_t> meter{(uint64_t(4) << 32) | 4};
    bool tracking = false, strong = false;
    int64_t lastPulse = 0;
    uint64_t generation = 0, lastLoop = 0, lastMeter = 0;
    static constexpr double duration = 0.035;
    double age = duration;
    // Declared last so its callback is stopped before callback state is destroyed.
    ofSoundStream stream;
};
