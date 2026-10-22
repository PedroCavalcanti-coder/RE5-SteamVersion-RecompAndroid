#include "audio_mixer.h"
#include <cstdio>
#include <stdexcept>
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){
    PcmMixer mixer;auto voice=std::make_shared<PcmVoice>();voice->channels=1;voice->bits=16;voice->frequency=mixer.rate;
    voice->samples={0,0x40,0,0xc0};voice->playing=true;require(mixer.add(voice),"voice allocation");
    float output[8];mixer.render(output,4);
    require(output[0]==0.5f&&output[1]==0.5f&&output[2]==-0.5f&&output[3]==-0.5f,"signed PCM16 mono expansion");
    require(output[4]==0&&output[7]==0&&!voice->playing,"one-shot stops and silence follows");
    voice->playing=true;voice->loop=true;voice->frequency=mixer.rate/2;mixer.render(output,4);
    require(output[0]==0.5f&&output[2]==0&&output[4]==-0.5f&&output[6]==0,"fractional resampling and loop interpolation");
    mixer.remove(voice);auto stereo=std::make_shared<PcmVoice>();stereo->channels=2;stereo->bits=8;stereo->samples={255,0,128,128};stereo->playing=true;stereo->loop=true;
    require(mixer.add(stereo),"stereo allocation");mixer.render(output,2);
    require(output[0]==127/128.f&&output[1]==-1.f&&output[2]==0&&output[3]==0,"unsigned PCM8 stereo channels");
    auto second=std::make_shared<PcmVoice>(*stereo);second->position=0;require(mixer.add(second),"second voice");stereo->position=0;mixer.render(output,1);
    require(output[0]==1.f&&output[1]==-1.f,"mixed clipping");
    second->playing=false;stereo->position=0;stereo->left=0.25f;stereo->right=0;mixer.render(output,1);
    require(output[0]==127/512.f&&output[1]==0,"volume and pan gains");
    std::puts("PCM mixer passed: signed PCM16, unsigned PCM8, stereo, resampling, loops, stop, clipping and gains");
}
