#include "guest_memory.h"
#include <cstdio>
void require(bool value){if(!value)throw std::runtime_error("Guest protection check failed");}
int main(){
    testGuestMemory();GuestMemory mem;uint32_t page=uint32_t(sysconf(_SC_PAGESIZE)),base=0x400000,previous=0xdeadbeef;
    mem.commit(base,3*page);std::memset(mem.address(base,3*page),0xa5,3*page);
    require(mem.protect(base+page-1,2,2,previous)&&previous==4);
    require(mem.readable(base,2*page)&&!mem.writable(base,2*page)&&mem.writable(base+2*page,page));
    mem.commit(base+16,16);require(!mem.writable(base,1));
    mem.commit(base+page,3*page);require(!mem.writable(base+page,1)&&mem.writable(base+3*page,page));
    require(!mem.protect(base+4*page-1,2,1,previous)&&mem.writable(base+3*page,page));
    require(!mem.protect(base,0,4,previous)&&!mem.protect(0xffffffff,2,4,previous));
    require(!mem.protect(base,page,0x104,previous)&&mem.readable(base,page));
    require(mem.protect(base,page,0x40,previous)&&previous==2&&mem.writable(base,page));
    require(*static_cast<uint8_t*>(mem.address(base,1))==0xa5);
    require(mem.protect(base+page,page,1,previous)&&previous==2&&!mem.readable(base+page,1));
    require(mem.protect(base+page,page,4,previous)&&previous==1&&mem.writable(base+page,page));
    std::puts("Guest protections passed: page crossings, old flags, RO/RW/no-access, data preservation, invalid and uncommitted ranges; guest execute remains non-native");
}
