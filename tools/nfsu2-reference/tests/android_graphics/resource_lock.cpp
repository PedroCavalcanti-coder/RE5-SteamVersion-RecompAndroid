#include <d3d9.h>
#include <map>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../../android/app/src/main/cpp/guest_memory.h"
#include "../../android/app/src/main/cpp/guest_heap.h"
#include "../../android/app/src/main/cpp/dxt_decode.h"
#include "../../android/app/src/main/cpp/dxt_shadow.h"
namespace {
GuestMemory memory;
GuestHeaps heaps(memory);
void* pointer(uint32_t address){return memory.address(address,0);}
uint32_t allocateGuest(uint32_t size){return heaps.alloc(GuestHeaps::process,8,size);}
void freeGuest(uint32_t address){if(!heaps.free(GuestHeaps::process,address))throw std::runtime_error("Graphics lock double release");}
void write(uint32_t address,uint32_t value){std::memcpy(memory.address(address,4),&value,4);}
template<class T>T* nativeObject(uint32_t){throw std::runtime_error("Native device not used by memory test");}
std::map<uint32_t,CompressedView> compressedViews;
CompressedView compressionView(uint32_t address){auto found=compressedViews.find(address);return found==compressedViews.end()?CompressedView{}:found->second;}
#include "../../android/app/src/main/cpp/d3d9_locks.inc"
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void checkPixels(D3DFORMAT format,uint32_t width,uint32_t height,uint32_t pitch,uint32_t flags){
    uint32_t rows=blockCompressed(format)?(height+3)/4:height,bytes=rowBytes(format,width);
    std::vector<uint8_t> native(pitch*rows+32,0xaa);
    for(uint32_t row=0;row<rows;++row)for(uint32_t column=0;column<bytes;++column)native[row*pitch+column]=static_cast<uint8_t>(row+column);
    auto original=native;
    D3DSURFACE_DESC desc{};desc.Format=format;desc.Width=width;desc.Height=height;
    D3DLOCKED_RECT locked{static_cast<INT>(pitch),native.data()};
    uint32_t out=allocateGuest(8);require(SUCCEEDED(lockPixels(0xabc,0,desc,locked,nullptr,flags,out)),"Lock translation failed");
    uint32_t guest[2];std::memcpy(guest,pointer(out),8);
    require(guest[0]==pitch,"Guest pitch differs");require(uintptr_t(native.data())>UINT32_MAX,"Test needs a true 64-bit native pointer");
    auto temporary=static_cast<uint8_t*>(pointer(guest[1]));
    for(uint32_t row=0;row<rows;++row){require(!std::memcmp(temporary+row*pitch,native.data()+row*pitch,bytes),"Lock read copy differs");std::memset(temporary+row*pitch,0x45+row,pitch);}
    copyLockBack(0xabc,0);
    if(flags&D3DLOCK_READONLY)require(native==original,"Read-only lock uploaded changes");
    else for(uint32_t row=0;row<rows;++row)for(uint32_t column=0;column<pitch;++column)
        require(native[row*pitch+column]==(column<bytes?0x45+row:0xaa),"Upload wrote pixels or row padding incorrectly");
    for(uint32_t i=pitch*rows;i<native.size();++i)require(native[i]==0xaa,"Upload exceeded native buffer");
    releaseLock(0xabc,0);freeGuest(out);require(resourceLocks.empty(),"Lock temporary leaked");
}
void checkCompressedFallback(){
    auto shadow=std::make_shared<CompressedTexture>(D3DFMT_DXT1,5,5,1,1);compressedViews[0xdef]={shadow,0,0};
    std::vector<uint8_t> native(32*5+32,0xaa);D3DSURFACE_DESC desc{};desc.Width=5;desc.Height=5;desc.Format=D3DFMT_A8B8G8R8;
    D3DLOCKED_RECT locked{32,native.data()};uint32_t out=allocateGuest(8);
    require(SUCCEEDED(lockPixels(0xdef,0,desc,locked,nullptr,0,out)),"DXT fallback lock failed");uint32_t guest[2];std::memcpy(guest,pointer(out),8);
    require(guest[0]==16,"DXT fallback exposed RGBA pitch to the game");auto blocks=static_cast<uint8_t*>(pointer(guest[1]));
    for(unsigned i=0;i<4;++i){blocks[i*8]=blocks[i*8+1]=255;}
    copyLockBack(0xdef,0);
    for(unsigned row=0;row<5;++row)for(unsigned byte=0;byte<32;++byte)require(native[row*32+byte]==(byte<20?255:0xaa),"DXT fallback did not upload RGBA or damaged padding");
    releaseLock(0xdef,0);freeGuest(out);compressedViews.clear();
}
}
int main(){try{
    uint8_t white[32]{};for(unsigned i=0;i<4;++i){white[i*8]=white[i*8+1]=255;}
    auto rgba=decodeDXT(1,5,5,white,sizeof white);for(auto byte:rgba)require(byte==255,"DXT1 edge blocks did not decode white");
    uint8_t transparent[8]={0,0,255,255,255,255,255,255};rgba=decodeDXT(1,4,4,transparent,8);for(auto byte:rgba)require(byte==0,"DXT1 transparency differs");
    uint8_t interpolated[16]={10,20,0x88,0xc6,0xfa,0x88,0xc6,0xfa,255,255,0,0,0,0,0,0};
    const uint8_t expectedAlpha[8]={10,20,12,14,16,18,0,255};rgba=decodeDXT(5,4,4,interpolated,16);
    for(unsigned i=0;i<16;++i){require(rgba[i*4]==255&&rgba[i*4+1]==255&&rgba[i*4+2]==255,"DXT5 colors differ");require(rgba[i*4+3]==expectedAlpha[i%8],"DXT5 alpha palette differs");}
    bool truncated=false;try{decodeDXT(5,4,4,interpolated,15);}catch(const std::invalid_argument&){truncated=true;}require(truncated,"Truncated DXT data accepted");
    require(FAILED(lockTexture(0xabc,0,0,nullptr,0)),"Null texture lock output accepted");
    require(FAILED(lockCube(0xabc,0,0,0,nullptr,0)),"Null cube lock output accepted");
    require(FAILED(lockSurface(0xabc,0,nullptr,0)),"Null surface lock output accepted");
    checkPixels(D3DFMT_A8B8G8R8,3,2,32,0);
    checkPixels(D3DFMT_A8B8G8R8,3,2,32,D3DLOCK_READONLY);
    checkPixels(D3DFMT_DXT1,5,5,24,0);
    checkCompressedFallback();
    std::puts("Graphics memory and DXT decoding passed: native/guest pointers, row pitch, read-only, guard bytes, edge blocks, transparency and alpha interpolation");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
