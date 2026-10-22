#include "../../android/app/src/main/cpp/guest_memory.h"
#include "../../android/app/src/main/cpp/guest_heap.h"
#include "../../android/app/src/main/cpp/android_resolution.h"
#include "../../android/app/src/main/cpp/d3d9_bridge.cpp"

extern "C" IDirect3D9* Direct3DCreate9(UINT){return nullptr;}
extern "C" void dxvkAndroidSetWindow(ANativeWindow*){}
extern "C" HWND dxvkAndroidGetWindowHandle(){return nullptr;}
extern "C" void dxvkAndroidSetRenderSize(uint32_t,uint32_t){}
#ifndef __ANDROID__
extern "C" ANativeWindow* dxvkCreateTestWindow(){return nullptr;}
extern "C" void dxvkDestroyTestWindow(ANativeWindow*){}
#endif
namespace {
GuestMemory testMemory;
GuestHeaps testHeaps(testMemory);
uint32_t testAllocate(uint32_t size){return testHeaps.alloc(GuestHeaps::process,8,size);}
void testFree(uint32_t address){if(!testHeaps.free(GuestHeaps::process,address))throw std::runtime_error("COM wrapper double free");}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct ParentCountedSurface: IUnknown {
    ULONG refs=2; // One parent reference and one acquired surface reference.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** output) override {*output=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {require(refs>0,"Native reference underflow");return --refs;}
};
struct DelegatedSurface: IUnknown {
    ParentCountedSurface& owner;
    explicit DelegatedSurface(ParentCountedSurface& value):owner(value){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** output) override {*output=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return owner.AddRef();}
    ULONG STDMETHODCALLTYPE Release() override {return owner.Release();}
};
uint32_t invoke(uint32_t surface,uint32_t method){uint32_t args[]={surface},count=0;
    uint32_t result=dispatchGuestD3D9(methodBase+5*4096+method*16,args,&count);
    require(count==1,"COM method stack cleanup differs");return result;
}
}
int main(){try {
    configureD3D9Bridge(testMemory.base(),testAllocate,testFree);
    ParentCountedSurface native;
    auto surface=wrap(&native,Kind::Surface);
    require(invoke(surface,2)==0,"Final guest Release exposed parent's native count");
    require(native.refs==1&&objects.empty()&&addresses.empty(),"Guest wrapper or parent lifetime differs");
    native.AddRef();surface=wrap(&native,Kind::Surface);
    native.AddRef();require(wrap(&native,Kind::Surface)==surface,"Repeated acquisition lost COM identity");
    require(invoke(surface,1)==3&&native.refs==4,"Guest AddRef exposed parent's native count");
    require(invoke(surface,2)==2&&invoke(surface,2)==1&&invoke(surface,2)==0,"Guest Release sequence differs");
    require(native.refs==1&&objects.empty()&&addresses.empty(),"Surface consumed parent reference or leaked wrapper");
    native.AddRef();surface=wrap(&native,Kind::Surface);
    retainInitialDepthDescriptor(0x870854,surface);
    require(native.refs==3&&invoke(surface,2)==1&&invoke(surface,2)==0,
        "Engine's aliased depth descriptor did not survive its first Release");
    require(native.refs==1&&objects.empty()&&addresses.empty(),"Depth descriptor references leaked");
    ParentCountedSurface cube;cube.refs=1;
    auto cubeHandle=wrap(&cube,Kind::Cube);
    cube.AddRef();DelegatedSurface face(cube);
    auto faceHandle=wrap(&face,Kind::Surface);attachTextureSurface(cubeHandle,faceHandle);
    {uint32_t arguments[]={cubeHandle},count=0;
        auto result=dispatchGuestD3D9(methodBase+4*4096+2*16,arguments,&count);
        require(result==1&&cube.refs==1&&objects.count(cubeHandle),"Parent wrapper vanished while a cube face remains");
        result=dispatchGuestD3D9(methodBase+4*4096+2*16,arguments,&count);
        require(result==0&&cube.refs==1,"Repeated detail rebuild Release consumed an owned cube face");
    }
    require(invoke(faceHandle,2)==0&&cube.refs==0&&objects.empty()&&addresses.empty(),
        "Cube wrapper or delegated face remained after final release");
    auto modes=androidRenderModes(2340,1080,1170,540);
    require(modes[0].width==584&&modes[0].height==270&&modes[1].width==1170&&modes[1].height==540,"Scaled Android modes differ");
    require(modes[2].width==1754&&modes[2].height==810&&modes[3].width==2340&&modes[3].height==1080,"Native Android modes differ");
    require(modes[4].width==1280&&modes[4].height==720&&modes[5].width==1170&&modes[5].height==540,"Fallback/launch Android modes differ");
    std::puts("COM lifetime and Android modes passed: parent counts, alias identity, balanced native ownership, cleanup and scaled/native/launch sizes");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
