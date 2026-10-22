#include "../android/app/src/main/cpp/frame_pacer.h"
#include <cstdio>
#include <cstdlib>
using namespace std::chrono;
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    using Clock=FramePacer::Clock;
    auto origin=Clock::time_point(seconds(100));
    for(unsigned limit:{30u,60u,90u,120u}){
        FramePacer pacer;auto time=origin;auto period=nanoseconds(1000000000ull/limit);
        require(pacer.deadline(time,limit)==time,"first frame must not sleep");
        for(unsigned frame=1;frame<=1000;++frame){
            auto ready=time+period/3;auto target=pacer.deadline(ready,limit);
            require(target==time+period,"fast frames obey the selected limit");time=target;
        }
        for(unsigned frame=0;frame<1000;++frame){
            time+=period+milliseconds(7);
            require(pacer.deadline(time,limit)==time,"CPU-limited frames must not gain periodic extra sleeps");
        }
        time+=seconds(5);
        require(pacer.deadline(time,limit)==time,"resume must not add a full frame of delay");
        require(pacer.deadline(time+milliseconds(1),0)==time+milliseconds(1),"uncapped mode never sleeps");
        require(pacer.deadline(time+milliseconds(2),limit)==time+milliseconds(2),"enabling a cap starts a fresh schedule");
        require(pacer.deadline(time+milliseconds(3),limit==30?60:30)==time+milliseconds(3),"changing caps starts a fresh schedule");
    }
    std::puts("Frame pacing passed: 30/60/90/120, 8000 fast/slow frames, pause, uncapped and cap changes");
}
