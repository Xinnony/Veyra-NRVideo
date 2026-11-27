#pragma once
// Audio filters: live dialogue dubbing (#voice-dub).
//
// The renderer applies VoiceFilter AFTER the recording/ASR tap and BEFORE the
// user gain. Recognition therefore keeps hearing the original voice (it would
// otherwise transcribe the dub), and recordings keep the source audio.
//
// Two jobs, both bounded and allocation-free on the audio thread:
//  1. reduce the original dialogue:
//     - multichannel input with a front-centre speaker: dialogue is normally
//       mixed to the centre channel, so only that channel is attenuated (no
//       added latency; centred effects in that channel are reduced as well);
//     - stereo input: an STFT estimate of the centred, speech-band component
//       (inter-channel coherence, 80 Hz - 9 kHz) is subtracted. This is a
//       deterministic DSP estimate, not a trained separation model: centred
//       effects or music vocals in the speech band are reduced too. It adds a
//       fixed kStftSize-frame (21.3 ms at 48 kHz) delay while active.
//  2. mix the translated voice (mono 48 kHz PCM from DubVoiceQueue).
#include "veyra/sink/AudioFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace veyra::sink {

// balance: 0 = original voice only, 0.5 = both at full level, 1 = translated
// voice only. A single slider crosses from one to the other.
inline float originalVoiceGain(float balance){
    if(!std::isfinite(balance))balance=0;
    return std::clamp(2.0f*(1.0f-balance),0.0f,1.0f);
}
inline float dubVoiceGain(float balance){
    if(!std::isfinite(balance))balance=0;
    return std::clamp(2.0f*balance,0.0f,1.0f);
}
inline constexpr float kMaxDubGain=2.0f;

struct VoiceFilterSettings {
    bool enabled=false;
    float balance=0.75f;
    float dubGain=1.0f;
};
// Any thread. Read once per renderer pull.
void setVoiceFilterSettings(const VoiceFilterSettings& settings);
VoiceFilterSettings voiceFilterSettings();

enum class VoiceReduction { None=0, CenterChannel=1, StereoSpectral=2 };
VoiceReduction voiceReductionFor(const AudioFormat& format);
// The mode the most recent renderer used (diagnostics/UI only).
VoiceReduction lastVoiceReduction();

// Translated voice, mono 48 kHz float. Single producer (the UI thread),
// single consumer (the renderer that is currently playing). Neither waits.
class DubVoiceQueue {
public:
    explicit DubVoiceQueue(size_t capacityFrames);
    // Producer. Rejects the whole clip (returns false) when it does not fit or
    // a clear is still pending, so a clip is never truncated mid-word.
    bool push(const float* samples,size_t frames);
    // Producer. Drops everything queued; the consumer applies it on its next pop.
    void clear(){clear_.store(true,std::memory_order_release);}
    // Consumer. `owner` identifies the renderer; a second renderer gets
    // nothing while the first one pulled within the last 300 ms.
    size_t pop(float* out,size_t frames,const void* owner,int64_t nowUs);
    double queuedSeconds()const;
    size_t capacityFrames()const{return buffer_.size()-1;}
    uint64_t rejectedClips()const{return rejected_.load(std::memory_order_relaxed);}
private:
    std::vector<float> buffer_;
    std::atomic<size_t> read_{0},write_{0};
    std::atomic<bool> clear_{false};
    std::atomic<const void*> owner_{nullptr};
    std::atomic<int64_t> ownerSeenUs_{0};
    std::atomic<uint64_t> rejected_{0};
};
DubVoiceQueue& dubVoiceQueue();

// Small radix-2 complex FFT used by the stereo estimate (exposed for tests).
class Fft {
public:
    explicit Fft(size_t size);
    void forward(std::complex<float>* data)const{run(data,false);}
    void inverse(std::complex<float>* data)const; // scaled by 1/size
    size_t size()const{return size_;}
private:
    void run(std::complex<float>* data,bool inverse)const;
    size_t size_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<uint32_t> reverse_;
};

class VoiceFilter {
public:
    static constexpr size_t kStftSize=1024;
    static constexpr size_t kStftHop=256;
    VoiceFilter();
    ~VoiceFilter();
    VoiceFilter(const VoiceFilter&)=delete;
    VoiceFilter& operator=(const VoiceFilter&)=delete;
    // Stream discontinuity (start, seek, device rebuild): history is dropped.
    void reset();
    // Audio owner thread, in place, frame count unchanged. Returns true when
    // the output may exceed the input peak (translated voice mixed in), so the
    // caller should apply its peak limiter.
    bool process(float* pcm,size_t frames,const AudioFormat& format,const void* renderer,int64_t nowUs);
    // Explicit settings for tests; process() reads the global settings.
    bool process(float* pcm,size_t frames,const AudioFormat& format,const VoiceFilterSettings& settings,
                 DubVoiceQueue* dub,const void* renderer,int64_t nowUs);
    VoiceReduction mode()const{return mode_;}
    size_t latencyFrames()const{return mode_==VoiceReduction::StereoSpectral?kStftSize:0;}
private:
    void configure(VoiceReduction mode,const AudioFormat& format);
    void stereoFrame();
    VoiceReduction mode_=VoiceReduction::None;
    AudioFormat format_{};
    bool configured_=false;
    int left_=-1,right_=-1,center_=-1,lfe_=-1;
    float centerGain_=1;     // smoothed per sample
    float reduction_=0;      // smoothed per STFT hop: 0 = untouched
    float targetReduction_=0;
    size_t fadeIn_=0;
    // STFT state (stereo); all channels share one delay so they stay aligned.
    Fft fft_{kStftSize};
    std::array<float,kStftSize> window_{};
    std::array<float,kStftSize/2+1> band_{};
    std::array<float,kStftSize/2+1> mask_{};
    std::array<std::complex<float>,kStftSize> spectrum_{};
    std::vector<float> history_;  // kStftSize frames x 2 (L,R), ring
    std::vector<float> ola_;      // kStftSize frames x 2
    std::vector<float> output_;   // kStftHop frames x 2 ready for output
    std::vector<float> delay_;    // kStftSize frames x channels (other channels)
    size_t historyPos_=0,pending_=0,outputPos_=0,delayPos_=0;
    std::vector<float> dub_;
};

} // namespace veyra::sink
