#include "../android/app/src/main/cpp/audio_mixer.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
static void pcm16(PcmVoice& voice,std::initializer_list<int16_t> values){
    for(int16_t value:values){voice.samples.push_back(uint8_t(value));voice.samples.push_back(uint8_t(uint16_t(value)>>8));}
}
int main(){
    float out[16];
    {
        PcmMixer mixer;auto voice=std::make_shared<PcmVoice>();
        voice->bits=8;voice->channels=1;voice->frequency=22050;voice->samples={128,128};voice->playing=voice->loop=true;mixer.add(voice);
        mixer.render(out,8);for(float value:out)require(value==0,"unsigned 8-bit silence must not introduce noise");
        require(mixer.peakAmplitude==0&&mixer.clippedSamples==0,"silence diagnostics");
    }
    {
        PcmMixer mixer;auto voice=std::make_shared<PcmVoice>();
        pcm16(*voice,{-32768,32767,16384,-16384});voice->playing=voice->loop=true;mixer.add(voice);
        mixer.render(out,2);
        require(out[0]==-1&&out[1]==32767/32768.f&&out[2]==0.5f&&out[3]==-0.5f,"signed little-endian stereo and channel order");
        voice->left=voice->right=0;mixer.render(out,8);for(float value:out)require(value==0,"muted voice must be silent");
    }
    {
        PcmMixer mixer;auto voice=std::make_shared<PcmVoice>();
        voice->channels=1;voice->frequency=24000;pcm16(*voice,{16384,-16384});voice->playing=voice->loop=true;mixer.add(voice);
        mixer.render(out,8);float expected[]={0.5f,0,-0.5f,0,0.5f,0,-0.5f,0};
        for(unsigned i=0;i<8;++i)require(std::abs(out[2*i]-expected[i])<1e-6f&&std::abs(out[2*i+1]-expected[i])<1e-6f,"fractional resampling and wrap interpolation");
    }
    {
        PcmMixer mixer;
        for(unsigned i=0;i<3;++i){auto voice=std::make_shared<PcmVoice>();voice->channels=1;pcm16(*voice,{16384});voice->playing=voice->loop=true;mixer.add(voice);}
        mixer.render(out,8);for(float value:out)require(value==1,"mixed output must stay within PCM range");
        require(mixer.peakAmplitude==1.5f&&mixer.clippedSamples==16,"detect saturation before clamping");
    }
    {
        constexpr double pi=3.14159265358979323846;PcmMixer mixer;
        auto voice=std::make_shared<PcmVoice>();voice->channels=1;voice->rate=voice->frequency=22050;
        for(unsigned i=0;i<22050;++i){int16_t value=int16_t(std::lround(8192*std::sin(2*pi*6000*i/22050)));pcm16(*voice,{value});}
        voice->playing=voice->loop=true;mixer.add(voice);
        std::vector<float> filtered(48000*2),linear(48000*2);mixer.render(filtered.data(),48000);
        for(unsigned i=0;i<48000;++i){double pos=double(i)*22050/48000;size_t a=size_t(pos),b=(a+1)%22050;
            float f=float(pos-a),first=PcmMixer::sample(*voice,a,0),second=PcmMixer::sample(*voice,b,0);
            linear[2*i]=first+(second-first)*f;}
        auto amplitude=[&](const std::vector<float>& values,double hz){double re=0,im=0;
            for(unsigned i=0;i<48000;++i){re+=values[2*i]*std::cos(2*pi*hz*i/48000);im+=values[2*i]*std::sin(2*pi*hz*i/48000);}
            return 2*std::hypot(re,im)/48000;};
        double oldImage=amplitude(linear,16050),newImage=amplitude(filtered,16050),tone=amplitude(filtered,6000);
        require(newImage<oldImage*0.01,"22.05-to-48 kHz image rejection must improve by at least 40 dB");
        require(std::abs(tone-0.25)<0.005,"filter must preserve the original tone level");
        std::printf("Resampling image at 16.05 kHz: %.1f dB improvement, source amplitude %.4f\n",20*std::log10(oldImage/newImage),tone);
    }
    std::puts("PCM mixer passed: 8-bit silence, signed stereo, mute, fractional pitch, loop boundaries and clipping detection");
}
