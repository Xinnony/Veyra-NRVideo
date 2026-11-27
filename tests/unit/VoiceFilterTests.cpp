#include "veyra/sink/VoiceFilter.h"

#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using namespace veyra::sink;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool near(float a,float b,float tolerance=1e-5f){return std::abs(a-b)<=tolerance;}
constexpr AudioFormat kStereo{2,SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT};
constexpr AudioFormat k51{6,SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT};

float tone(double hz,size_t i){return float(0.5*std::sin(2.0*std::numbers::pi*hz*double(i)/48000.0));}
double rms(const std::vector<float>& pcm,unsigned channels,unsigned channel,size_t from,size_t to){
    double sum=0;for(size_t i=from;i<to;++i){const double v=pcm[i*channels+channel];sum+=v*v;}
    return std::sqrt(sum/double(to-from));
}
// Feed in uneven blocks, as the WASAPI endpoint does.
void run(VoiceFilter& filter,std::vector<float>& pcm,const AudioFormat& format,const VoiceFilterSettings& s,DubVoiceQueue* dub=nullptr){
    const size_t frames=pcm.size()/format.channels;
    const size_t blocks[]={480,17,1024,333,96};size_t at=0,b=0;
    while(at<frames){
        const size_t n=std::min(blocks[b++%5],frames-at);
        filter.process(pcm.data()+at*format.channels,n,format,s,dub,&filter,int64_t(at));
        at+=n;
    }
}

void gains(){
    require(originalVoiceGain(0)==1&&dubVoiceGain(0)==0,"balance 0 keeps the original voice only");
    require(originalVoiceGain(0.5f)==1&&dubVoiceGain(0.5f)==1,"balance 0.5 keeps both at full level");
    require(originalVoiceGain(1)==0&&dubVoiceGain(1)==1,"balance 1 keeps the translated voice only");
    require(originalVoiceGain(std::nanf(""))==1,"NaN balance falls back to the original voice");
    require(voiceReductionFor(kStereo)==VoiceReduction::StereoSpectral,"stereo uses the spectral estimate");
    require(voiceReductionFor(k51)==VoiceReduction::CenterChannel,"5.1 uses the centre channel");
    require(voiceReductionFor({1,SPEAKER_FRONT_CENTER})==VoiceReduction::None,"mono cannot be separated");
}

void fft(){
    Fft f(1024);std::vector<std::complex<float>> data(1024),copy;
    for(size_t i=0;i<data.size();++i)data[i]={tone(440,i),tone(3000,i)};
    copy=data;f.forward(data.data());f.inverse(data.data());
    for(size_t i=0;i<data.size();++i)require(std::abs(data[i]-copy[i])<1e-4f,"FFT round trip");
}

void transparentWhenNotReducing(){
    // Enabled with balance <= 0.5: the STFT path must reconstruct the input,
    // delayed by exactly kStftSize frames, on both channels.
    VoiceFilter filter;std::vector<float> pcm(48000*2),in;
    for(size_t i=0;i<48000;++i){pcm[i*2]=tone(440,i)+0.2f*tone(7000,i);pcm[i*2+1]=tone(1250,i);}
    in=pcm;run(filter,pcm,kStereo,{true,0.5f,1});
    require(filter.latencyFrames()==VoiceFilter::kStftSize,"stereo latency is reported");
    for(size_t i=VoiceFilter::kStftSize*2;i<48000;++i)for(unsigned c=0;c<2;++c)
        require(near(pcm[i*2+c],in[(i-VoiceFilter::kStftSize)*2+c],2e-4f),"perfect reconstruction with fixed delay");
}

void stereoRemovesCentredSpeechBand(){
    VoiceFilter centred;std::vector<float> pcm(48000*2);
    for(size_t i=0;i<48000;++i)pcm[i*2]=pcm[i*2+1]=tone(1000,i);
    const double before=rms(pcm,2,0,24000,48000);
    run(centred,pcm,kStereo,{true,1.0f,1});
    require(rms(pcm,2,0,24000,48000)<0.1*before,"centred 1 kHz voice band is removed at balance 1");

    VoiceFilter panned;std::vector<float> side(48000*2,0.0f);
    for(size_t i=0;i<48000;++i)side[i*2]=tone(1000,i);
    run(panned,side,kStereo,{true,1.0f,1});
    require(rms(side,2,0,24000,48000)>0.95*before,"hard-panned effects are kept");

    VoiceFilter bass;std::vector<float> low(48000*2);
    for(size_t i=0;i<48000;++i)low[i*2]=low[i*2+1]=tone(45,i);
    run(bass,low,kStereo,{true,1.0f,1});
    require(rms(low,2,1,24000,48000)>0.95*before,"centred engine rumble below the speech band is kept");
}

void centreChannel(){
    VoiceFilter filter;std::vector<float> pcm(9600*6),in;
    for(size_t i=0;i<9600;++i)for(unsigned c=0;c<6;++c)pcm[i*6+c]=tone(300+100*c,i);
    in=pcm;run(filter,pcm,k51,{true,1.0f,1});
    require(filter.latencyFrames()==0,"centre mode adds no delay");
    for(size_t i=0;i<9600;++i)for(unsigned c:{0u,1u,3u,4u,5u})require(pcm[i*6+c]==in[i*6+c],"other channels untouched");
    for(size_t i=4800;i<9600;++i)require(pcm[i*6+2]==0.0f,"centre fully reduced at balance 1");
    require(near(pcm[6+2],in[6+2]*(1.0f-2.0f/2400),1e-6f),"centre gain ramps instead of stepping");
}

void disabledPassThrough(){
    VoiceFilter filter;DubVoiceQueue dub(4800);std::vector<float> voice(480,0.25f),pcm(4800*2),in;
    require(dub.push(voice.data(),voice.size()),"clip queued");
    for(size_t i=0;i<4800;++i)pcm[i*2]=pcm[i*2+1]=tone(1000,i);
    in=pcm;run(filter,pcm,kStereo,{false,1.0f,1},&dub);
    require(pcm==in,"disabled filter is bit exact");
    require(dub.queuedSeconds()>0,"disabled filter does not consume the translated voice");
}

void queueAndMix(){
    DubVoiceQueue q(1000);std::vector<float> clip(600,0.5f),out(1000);
    require(q.push(clip.data(),600),"first clip fits");
    require(!q.push(clip.data(),600)&&q.rejectedClips()==1,"a clip that does not fit is rejected whole");
    int owner1=0,owner2=0;
    require(q.pop(out.data(),100,&owner1,0)==100,"owner pops");
    require(q.pop(out.data(),100,&owner2,100000)==0,"a second renderer is ignored while the first is live");
    require(q.pop(out.data(),100,&owner2,500000)==100,"ownership moves after 300 ms");
    q.clear();require(!q.push(clip.data(),10),"push rejected while clear is pending");
    require(q.pop(out.data(),1000,&owner2,500001)==0&&q.queuedSeconds()==0,"clear drops queued voice");
    require(q.push(clip.data(),600),"push accepted after clear");

    VoiceFilter filter;DubVoiceQueue dub(48000);std::vector<float> voice(480,0.5f),pcm(480*6,0.0f);
    dub.push(voice.data(),voice.size());
    filter.process(pcm.data(),480,k51,{true,0.75f,1.0f},&dub,&filter,0);
    require(near(pcm[2],0.5f)&&pcm[0]==0&&pcm[3]==0,"5.1: translated voice goes to the centre only");
    VoiceFilter stereo;std::vector<float> two(480*2,0.0f);
    dub.push(voice.data(),voice.size());
    // A different renderer: it takes the queue over once the first one has been idle 300 ms.
    stereo.process(two.data(),480,{1,SPEAKER_FRONT_CENTER},{true,1.0f,2.0f},&dub,&stereo,1000000);
    require(near(two[0],1.0f)&&near(two[479],1.0f),"mono: translated voice at dub gain");
}
} // namespace

int main()try{
    gains();fft();transparentWhenNotReducing();stereoRemovesCentredSpeechBand();centreChannel();disabledPassThrough();queueAndMix();
    std::cout<<"voice filter tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\n";return 1;}
