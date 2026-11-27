#include "veyra/sink/VoiceFilter.h"
#include "veyra/sink/AudioGain.h"

#include <numbers>
#include <utility>

namespace veyra::sink {
namespace {
// The renderer mix and DubVoiceQueue both run at 48 kHz (see VoiceFilter.h).
constexpr int kAudioRate=48000;
std::atomic<bool> gEnabled{false};
std::atomic<float> gBalance{0.75f};
std::atomic<float> gDubGain{1.0f};
std::atomic<int> gLastMode{0};

int channelIndex(const AudioFormat& format,uint32_t speaker){
    if(!(format.mask&speaker))return -1;
    // Interleaved channels follow ascending speaker-bit order.
    return std::popcount(format.mask&(speaker-1));
}
float smoothstep(float edge0,float edge1,float x){
    const float t=std::clamp((x-edge0)/(edge1-edge0),0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}
} // namespace

void setVoiceFilterSettings(const VoiceFilterSettings& s){
    gBalance.store(std::isfinite(s.balance)?std::clamp(s.balance,0.0f,1.0f):0.0f);
    gDubGain.store(std::isfinite(s.dubGain)?std::clamp(s.dubGain,0.0f,kMaxDubGain):0.0f);
    gEnabled.store(s.enabled);
}
VoiceFilterSettings voiceFilterSettings(){
    return {gEnabled.load(),gBalance.load(),gDubGain.load()};
}
VoiceReduction voiceReductionFor(const AudioFormat& format){
    if(!format.valid())return VoiceReduction::None;
    if(format.channels>=3&&(format.mask&SPEAKER_FRONT_CENTER))return VoiceReduction::CenterChannel;
    constexpr uint32_t stereo=SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    if((format.mask&stereo)==stereo)return VoiceReduction::StereoSpectral;
    return VoiceReduction::None;
}
VoiceReduction lastVoiceReduction(){return VoiceReduction(gLastMode.load(std::memory_order_relaxed));}

// ---------------------------------------------------------------- queue
DubVoiceQueue::DubVoiceQueue(size_t capacityFrames):buffer_(capacityFrames+1,0.0f){}

bool DubVoiceQueue::push(const float* samples,size_t frames){
    if(!frames)return true;
    if(!samples||clear_.load(std::memory_order_acquire)){rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
    const size_t size=buffer_.size();
    const size_t r=read_.load(std::memory_order_acquire),w=write_.load(std::memory_order_relaxed);
    const size_t used=(w+size-r)%size;
    if(frames>capacityFrames()-used){rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
    const size_t first=std::min(frames,size-w);
    std::copy_n(samples,first,buffer_.data()+w);
    std::copy_n(samples+first,frames-first,buffer_.data());
    write_.store((w+frames)%size,std::memory_order_release);
    return true;
}

size_t DubVoiceQueue::pop(float* out,size_t frames,const void* owner,int64_t nowUs){
    const void* previous=owner_.load(std::memory_order_relaxed);
    if(previous&&previous!=owner&&nowUs-ownerSeenUs_.load(std::memory_order_relaxed)<300000)return 0;
    owner_.store(owner,std::memory_order_relaxed);
    ownerSeenUs_.store(nowUs,std::memory_order_relaxed);
    const size_t size=buffer_.size();
    if(clear_.load(std::memory_order_acquire)){
        read_.store(write_.load(std::memory_order_acquire),std::memory_order_release);
        clear_.store(false,std::memory_order_release);
        return 0;
    }
    const size_t r=read_.load(std::memory_order_relaxed),w=write_.load(std::memory_order_acquire);
    const size_t take=std::min(frames,(w+size-r)%size);
    const size_t first=std::min(take,size-r);
    std::copy_n(buffer_.data()+r,first,out);
    std::copy_n(buffer_.data(),take-first,out+first);
    read_.store((r+take)%size,std::memory_order_release);
    return take;
}

double DubVoiceQueue::queuedSeconds()const{
    const size_t size=buffer_.size();
    const size_t used=(write_.load(std::memory_order_acquire)+size-read_.load(std::memory_order_acquire))%size;
    return double(used)/double(kAudioRate);
}

DubVoiceQueue& dubVoiceQueue(){
    static DubVoiceQueue queue(size_t(kAudioRate)*20);
    return queue;
}

// ---------------------------------------------------------------- FFT
Fft::Fft(size_t size):size_(size),twiddle_(size/2),reverse_(size){
    unsigned bits=0;while((size_t(1)<<bits)<size)++bits;
    for(size_t i=0;i<size;++i){
        uint32_t r=0;for(unsigned b=0;b<bits;++b)if(i&(size_t(1)<<b))r|=1u<<(bits-1-b);
        reverse_[i]=r;
    }
    for(size_t k=0;k<size/2;++k){
        const double angle=-2.0*std::numbers::pi*double(k)/double(size);
        twiddle_[k]={float(std::cos(angle)),float(std::sin(angle))};
    }
}
void Fft::run(std::complex<float>* data,bool inverse)const{
    for(size_t i=0;i<size_;++i)if(i<reverse_[i])std::swap(data[i],data[reverse_[i]]);
    for(size_t length=2;length<=size_;length<<=1){
        const size_t half=length/2,step=size_/length;
        for(size_t start=0;start<size_;start+=length)
            for(size_t j=0;j<half;++j){
                auto w=twiddle_[j*step];if(inverse)w=std::conj(w);
                const auto a=data[start+j],b=data[start+j+half]*w;
                data[start+j]=a+b;data[start+j+half]=a-b;
            }
    }
}
void Fft::inverse(std::complex<float>* data)const{
    run(data,true);
    const float scale=1.0f/float(size_);
    for(size_t i=0;i<size_;++i)data[i]*=scale;
}

// ---------------------------------------------------------------- filter
VoiceFilter::VoiceFilter(){
    constexpr size_t n=kStftSize;
    // sqrt periodic Hann for analysis and synthesis: at 75% overlap the
    // product sums to exactly 2, compensated in stereoFrame().
    for(size_t i=0;i<n;++i)window_[i]=float(std::sqrt(0.5-0.5*std::cos(2.0*std::numbers::pi*double(i)/double(n))));
    for(size_t k=0;k<=n/2;++k){
        const float hz=float(k)*float(kAudioRate)/float(n);
        band_[k]=smoothstep(80.0f,200.0f,hz)*(1.0f-smoothstep(5000.0f,9000.0f,hz));
    }
    history_.assign(n*2,0.0f);ola_.assign(n*2,0.0f);output_.assign(kStftHop*2,0.0f);
    dub_.resize(4800);
}
VoiceFilter::~VoiceFilter()=default;

void VoiceFilter::reset(){
    configured_=false;mode_=VoiceReduction::None;fadeIn_=0;
}

void VoiceFilter::configure(VoiceReduction mode,const AudioFormat& format){
    const size_t oldLatency=configured_?latencyFrames():0;
    mode_=mode;format_=format;
    left_=channelIndex(format,SPEAKER_FRONT_LEFT);right_=channelIndex(format,SPEAKER_FRONT_RIGHT);
    center_=channelIndex(format,SPEAKER_FRONT_CENTER);lfe_=channelIndex(format,SPEAKER_LOW_FREQUENCY);
    centerGain_=1;reduction_=0;
    std::fill(history_.begin(),history_.end(),0.0f);std::fill(ola_.begin(),ola_.end(),0.0f);
    std::fill(output_.begin(),output_.end(),0.0f);mask_.fill(0.0f);
    delay_.assign(mode==VoiceReduction::StereoSpectral?kStftSize*format.channels:0,0.0f);
    historyPos_=pending_=outputPos_=delayPos_=0;
    // A change of the fixed delay skips or repeats history: restart the
    // waveform from zero (5 ms) instead of producing a step.
    if(configured_&&oldLatency!=latencyFrames())fadeIn_=240;
    configured_=true;
}

void VoiceFilter::stereoFrame(){
    constexpr size_t n=kStftSize;
    auto& z=spectrum_;
    for(size_t i=0;i<n;++i){
        const size_t at=(historyPos_+i)%n; // historyPos_ is the oldest frame
        z[i]={history_[at*2]*window_[i],history_[at*2+1]*window_[i]};
    }
    fft_.forward(z.data());
    const float r=reduction_;
    const std::complex<float> minusHalfI{0.0f,-0.5f};
    for(size_t k=0;k<=n/2;++k){
        const auto zk=z[k],znk=std::conj(z[(n-k)%n]);
        auto l=(zk+znk)*0.5f;
        auto rr=(zk-znk)*minusHalfI;
        const float energy=std::norm(l)+std::norm(rr);
        float coherence=energy>1e-12f?std::max(0.0f,2.0f*(l*std::conj(rr)).real()/energy):0.0f;
        coherence*=coherence;
        mask_[k]=0.5f*mask_[k]+0.5f*coherence*band_[k];
        if(r>0.0f){
            const auto centre=(l+rr)*(0.5f*mask_[k]*r);
            l-=centre;rr-=centre;
        }
        const std::complex<float> i1{0.0f,1.0f};
        z[k]=l+i1*rr;
        if(k&&k<n/2)z[n-k]=std::conj(l)+i1*std::conj(rr);
    }
    fft_.inverse(z.data());
    for(size_t i=0;i<n;++i){
        const float w=window_[i]*0.5f;
        ola_[i*2]+=z[i].real()*w;ola_[i*2+1]+=z[i].imag()*w;
    }
    std::copy_n(ola_.begin(),kStftHop*2,output_.begin());
    std::copy(ola_.begin()+kStftHop*2,ola_.end(),ola_.begin());
    std::fill(ola_.end()-kStftHop*2,ola_.end(),0.0f);
}

bool VoiceFilter::process(float* pcm,size_t frames,const AudioFormat& format,const void* renderer,int64_t nowUs){
    return process(pcm,frames,format,voiceFilterSettings(),&dubVoiceQueue(),renderer,nowUs);
}

bool VoiceFilter::process(float* pcm,size_t frames,const AudioFormat& format,const VoiceFilterSettings& settings,
                          DubVoiceQueue* dub,const void* renderer,int64_t nowUs){
    const auto mode=settings.enabled?voiceReductionFor(format):VoiceReduction::None;
    if(!configured_||mode!=mode_||format!=format_)configure(mode,format);
    gLastMode.store(int(mode),std::memory_order_relaxed);
    if(!pcm||!frames)return false;
    const unsigned channels=format.channels;
    const float original=settings.enabled?originalVoiceGain(settings.balance):1.0f;
    if(mode_==VoiceReduction::CenterChannel){
        for(size_t i=0;i<frames;++i){
            centerGain_+=std::clamp(original-centerGain_,-1.0f/2400,1.0f/2400); // 50 ms full swing
            pcm[i*channels+center_]*=centerGain_;
        }
    }else if(mode_==VoiceReduction::StereoSpectral){
        targetReduction_=1.0f-original;
        for(size_t i=0;i<frames;++i){
            float* frame=pcm+i*channels;
            const float inL=frame[left_],inR=frame[right_];
            for(unsigned c=0;c<channels;++c){
                if(int(c)==left_||int(c)==right_)continue;
                float& slot=delay_[delayPos_*channels+c];
                std::swap(frame[c],slot);
            }
            delayPos_=(delayPos_+1)%kStftSize;
            frame[left_]=output_[outputPos_*2];frame[right_]=output_[outputPos_*2+1];++outputPos_;
            history_[historyPos_*2]=inL;history_[historyPos_*2+1]=inR;
            historyPos_=(historyPos_+1)%kStftSize;
            if(++pending_==kStftHop){
                reduction_+=std::clamp(targetReduction_-reduction_,-0.05f,0.05f);
                stereoFrame();pending_=0;outputPos_=0;
            }
        }
    }
    if(fadeIn_)fadePcmHead(pcm,frames,channels,fadeIn_);
    if(!settings.enabled||!dub)return false;
    const float gain=dubVoiceGain(settings.balance)*std::clamp(settings.dubGain,0.0f,kMaxDubGain);
    if(dub_.size()<frames)dub_.resize(frames); // grows once to the endpoint buffer size
    const size_t got=dub->pop(dub_.data(),frames,renderer,nowUs);
    if(!got||gain<=0.0f)return false;
    for(size_t i=0;i<got;++i){
        const float v=dub_[i]*gain;
        float* frame=pcm+i*channels;
        if(center_>=0)frame[center_]+=v;
        else if(left_>=0&&right_>=0){frame[left_]+=v*0.70710678f;frame[right_]+=v*0.70710678f;}
        else frame[0]+=v;
    }
    return true;
}

} // namespace veyra::sink
