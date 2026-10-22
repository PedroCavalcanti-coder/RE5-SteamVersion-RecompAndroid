#include "guest_memory.h"
#include <filesystem>
#include <memory>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <initializer_list>
using recomp_func_t=void(*)();
using GuestStop=std::runtime_error;
#define NFS_RUNTIME_LOG(...) ((void)0)
std::unique_ptr<GuestMemory> memory;
std::string hostGameRoot;uint32_t lastError=0,result=0,cleaned=0;
std::vector<uint32_t> arguments;
void* ptr(uint32_t va,uint64_t bytes){return memory->address(va,bytes);}
uint32_t read32(uint32_t va){uint32_t value;std::memcpy(&value,ptr(va,4),4);return value;}
void write32(uint32_t va,uint32_t value){std::memcpy(ptr(va,4),&value,4);}
uint32_t arg(unsigned index){return arguments.at(index);}
void ret(uint32_t value,unsigned count){result=value;cleaned=count;}
std::string guestString(uint32_t va){return va?std::string(static_cast<char*>(ptr(va,1))):std::string();}
std::string lower(std::string value){for(auto& c:value)c=char(std::tolower(static_cast<unsigned char>(c)));return value;}
#include "guest_files.inc"
void require(bool value){if(!value)throw std::runtime_error("Guest filesystem check failed");}
uint32_t invoke(recomp_func_t fn,std::initializer_list<uint32_t> args){arguments=args;fn();require(cleaned==args.size());return result;}
uint32_t text(const char* value){std::strcpy(static_cast<char*>(ptr(0x401000,4096)),value);return 0x401000;}
int main(){
    memory=std::make_unique<GuestMemory>();memory->commit(0x400000,0x4000);
    char temporary[]="/data/local/tmp/nfsu2-files-XXXXXX";auto root=mkdtemp(temporary);require(root);hostGameRoot=root;
    try{
        require(invoke(shellFolderPath,{0,0x801c,0,0,0x400000})==0);
        require(guestString(0x400000)=="C:\\nfsu2\\AppData\\Local");
        require(std::filesystem::is_directory(hostGameRoot+"/AppData/Local"));
        require(invoke(shellFolderPath,{0,0xffff,0,0,0x400000})==0x80070057u&&guestString(0x400000).empty());
        require(invoke(createDirectory,{text("C:\\nfsu2\\AppData\\Local\\NFS Underground 2"),0})==1);
        require(invoke(createDirectory,{0x401000,0})==0&&lastError==183);
        std::ofstream(hostGameRoot+"/AppData/Local/NFS Underground 2/Player.Dat")<<"abcd";
        std::ofstream(hostGameRoot+"/AppData/Local/NFS Underground 2/README")<<"x";
        auto handle=invoke(findFirstFile,{text("C:\\nfsu2\\appdata\\LOCAL\\NFS Underground 2\\*.dAt"),0x402000});
        require(handle!=UINT32_MAX&&guestString(0x402000+44)=="Player.Dat"&&read32(0x402000+32)==4);
        require(invoke(findNextFile,{handle,0x402000})==0&&lastError==18);
        require(invoke(findClose,{handle})==1&&invoke(findClose,{handle})==0&&lastError==6);
        handle=invoke(findFirstFile,{text("C:\\nfsu2\\AppData\\Local\\NFS Underground 2\\*.*"),0x402000});
        unsigned count=1;while(invoke(findNextFile,{handle,0x402000}))++count;
        require(count==4&&lastError==18);require(invoke(findClose,{handle})==1);
        require(invoke(findFirstFile,{text("C:\\nfsu2\\AppData\\Local\\missing\\*"),0x402000})==UINT32_MAX&&lastError==3);
        auto name=text("C:\\nfsu2\\AppData\\Local\\NFS Underground 2\\Save.bin");
        auto length=invoke(fullPathName,{name,0,0,0});require(length==guestString(name).size()+1);
        require(invoke(fullPathName,{name,260,0x402000,0x403000})==length-1);
        require(guestString(read32(0x403000))=="Save.bin");
        handle=invoke(createFile,{name,0xc0000000u,0,0,1,0,0});require(handle!=UINT32_MAX);
        require(invoke(fileType,{handle})==1);
        require(invoke(fileType,{0xf0000200u})==2);
        require(invoke(fileType,{0xdeadbeefu})==0&&lastError==6);
        require(invoke(writeFile,{handle,text("abcdef"),6,0x403000,0})==1&&read32(0x403000)==6);
        require(invoke(writeFile,{0,0,1,0x403000,0})==0&&lastError==6&&read32(0x403000)==0);
        require(invoke(setFilePointer,{handle,3,0,0})==3&&invoke(setEndOfFile,{handle})==1);
        require(invoke(flushFileBuffers,{handle})==1&&invoke(fileSize,{handle,0})==3);
        require(closeGuestFile(handle));require(invoke(fileType,{handle})==0&&lastError==6);
        std::ifstream saved(hostGameRoot+"/AppData/Local/NFS Underground 2/Save.bin");std::string contents; saved>>contents;require(contents=="abc");
        bool rejected=false;try{gamePath("C:\\nfsu2\\..\\escape");}catch(const GuestStop&){rejected=true;}require(rejected);
        std::filesystem::create_symlink("/",hostGameRoot+"/outside");rejected=false;
        try{gamePath("C:\\nfsu2\\outside\\etc");}catch(const GuestStop&){rejected=true;}require(rejected);
        std::filesystem::remove_all(hostGameRoot);
        std::puts("Guest files passed: profile folders, case-insensitive paths, wildcard enumeration, WIN32_FIND_DATA layout, search errors, full paths/file-part pointers, write/seek/truncate/flush persistence and escape rejection");
    }catch(...){std::filesystem::remove_all(hostGameRoot);throw;}
}
