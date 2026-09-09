#include "bink_movie.hpp"
#include "bink_pixels.hpp"
#include "bink_summary_guard.hpp"
#include "probe.hpp"
#include "../common/movie_frame.hpp"
#include "../common/build_descriptor.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace k2vr::game32 {
namespace {
using CopyFunction=std::int32_t(__stdcall*)(void*,void*,std::int32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
using CloseFunction=void(__stdcall*)(void*);
using PauseFunction=std::int32_t(__stdcall*)(void*,std::int32_t);
CopyFunction original_copy{};
CloseFunction original_close{};
PauseFunction original_pause{};
bink::GetSummaryFunction original_summary{};
std::uintptr_t bink_base{};
ipc::MovieFrameChannel movie;
std::atomic<void*> active_movie{};
SRWLOCK capture_lock=SRWLOCK_INIT;
std::vector<std::uint8_t> converted;
bool installed{},format_logged{};

bool ValidateDecoder(HMODULE module) noexcept {
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(module,path,32768);
    if(!length || length>=32768) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok) ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<unsigned char,65536> buffer{};
    while(ok){DWORD read{};if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)){ok=false;break;}
        if(!read) break;ok=BCryptHashData(hash,buffer.data(),read,0)>=0;}
    builds::Sha256Digest digest{};
    if(ok) ok=BCryptFinishHash(hash,digest.bytes.data(),static_cast<ULONG>(digest.bytes.size()),0)>=0;
    if(hash) BCryptDestroyHash(hash);if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);
    const auto expected=builds::ParseSha256("D963112EA8545C8AAA6BD48A9D2D229605806AE1BBC03F90B0106DB3034466ED");
    if(!ok || !expected || digest!=*expected) return false;
    // Validate the loaded fault instruction too; file identity alone does not
    // authorize swallowing an exception in modified executable memory.
    const auto* bytes=reinterpret_cast<const unsigned char*>(module);
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(bytes+0x12296,&region,sizeof(region)) || region.AllocationBase!=module || region.State!=MEM_COMMIT) return false;
    std::array<unsigned char,2> fault{};SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),bytes+0x12296,fault.data(),fault.size(),&read) &&
        read==fault.size() && fault[0]==0xF7 && fault[1]==0xF1;
}

// Isolate optional pixel reads from C++ cleanup. Only this additional capture
// is suppressed on bad buffer metadata; original decoder failures propagate.
bool ReadPixels(void* handle,void* destination,std::int32_t pitch,std::uint32_t destination_height,
    std::uint32_t x,std::uint32_t y,std::uint32_t flags,std::uint32_t& width,std::uint32_t& height) noexcept {
    __try {
        if(!handle || !destination || pitch<=0 || pitch>1024*1024) return false;
        const auto* dimensions=static_cast<const std::uint32_t*>(handle);
        width=dimensions[0];height=dimensions[1];
        const auto bpp=BinkBytesPerPixel(flags);
        if(!bpp || !width || !height || width>ipc::kMovieMaximumWidth || height>ipc::kMovieMaximumHeight ||
            std::uint64_t(x+std::uint64_t(width))*bpp>static_cast<std::uint32_t>(pitch) ||
            std::uint64_t(y)+height>destination_height) return false;
        if(converted.size()!=std::size_t(width)*height*4) return false;
        const auto offset=std::uint64_t(y)*static_cast<std::uint32_t>(pitch)+std::uint64_t(x)*bpp;
        const auto size=std::uint64_t(height-1)*static_cast<std::uint32_t>(pitch)+std::uint64_t(width)*bpp;
        if(offset+size>UINT32_MAX || reinterpret_cast<std::uintptr_t>(destination)>UINT32_MAX-offset-size) return false;
        return ConvertBinkPixels({static_cast<const std::uint8_t*>(destination)+static_cast<std::size_t>(offset),
            static_cast<std::size_t>(size)},width,height,static_cast<std::uint32_t>(pitch),flags,converted);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReadDimensions(void* handle,std::uint32_t& width,std::uint32_t& height) noexcept {
    __try {if(!handle) return false;const auto* p=static_cast<const std::uint32_t*>(handle);width=p[0];height=p[1];
        return width && height && width<=ipc::kMovieMaximumWidth && height<=ipc::kMovieMaximumHeight;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void CaptureMovie(void* handle,void* destination,std::int32_t pitch,std::uint32_t destination_height,
    std::uint32_t x,std::uint32_t y,std::uint32_t flags) noexcept {
    if(!TryAcquireSRWLockExclusive(&capture_lock)) return;
    std::uint32_t width{},height{};
    bool captured=false;
    if(ReadDimensions(handle,width,height)){
        try {converted.resize(std::size_t(width)*height*4);
            captured=ReadPixels(handle,destination,pitch,destination_height,x,y,flags,width,height) &&
                movie.Publish(width,height,converted);
        }catch(...){captured=false;}
    }
    if(captured){
        if(active_movie.exchange(handle)!=handle){
            char line[160]{};std::snprintf(line,sizeof(line),"bink-movie-frame width=%u height=%u surface=%u path=direct-buffer",width,height,flags);
            (void)AppendPersistentProbeLogLine(line);
        }
    }else if(!format_logged){
        char line[160]{};std::snprintf(line,sizeof(line),"bink-movie-capture-unavailable width=%u height=%u surface=%u pitch=%d",width,height,flags,pitch);
        (void)AppendPersistentProbeLogLine(line);format_logged=true;
    }
    ReleaseSRWLockExclusive(&capture_lock);
}
std::int32_t __stdcall Copy(void* handle,void* destination,std::int32_t pitch,std::uint32_t height,
    std::uint32_t x,std::uint32_t y,std::uint32_t flags) {
    const auto result=original_copy(handle,destination,pitch,height,x,y,flags);
    const auto error=GetLastError();CaptureMovie(handle,destination,pitch,height,x,y,flags);SetLastError(error);return result;
}
void End(void* handle) noexcept {
    if(active_movie.compare_exchange_strong(handle,nullptr)) movie.EndMovie();
}
void __stdcall Close(void* handle){const auto error=GetLastError();End(handle);SetLastError(error);original_close(handle);}
std::int32_t __stdcall Pause(void* handle,std::int32_t paused){
    const auto result=original_pause(handle,paused);const auto error=GetLastError();
    if(paused) End(handle);SetLastError(error);return result;
}
void __stdcall Summary(void* handle,void* summary){
    if(bink::GetSummaryGuarded(original_summary,handle,summary,bink_base))
        (void)AppendPersistentProbeLogLine("bink-summary-overflow-contained rva=0x12296 statistics=unavailable");
}
}

bool InstallBinkMovieHooks(ipc::SessionNonce nonce) noexcept {
    if(installed) return movie.Open(nonce,true);
    const auto module=GetModuleHandleW(L"binkw32.dll");
    if(!module || !ValidateDecoder(module)){
        (void)AppendPersistentProbeLogLine("bink-hook-validation-failed reason=unsupported-decoder");return false;
    }
    struct Hook {const char* name;void* replacement;void* original;void** cell;};
    Hook hooks[]={
        {"_BinkCopyToBuffer@28",reinterpret_cast<void*>(&Copy),nullptr,nullptr},
        {"_BinkClose@4",reinterpret_cast<void*>(&Close),nullptr,nullptr},
        {"_BinkPause@8",reinterpret_cast<void*>(&Pause),nullptr,nullptr},
        {"_BinkGetSummary@8",reinterpret_cast<void*>(&Summary),nullptr,nullptr}};
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;++imports){
        if(_stricmp(reinterpret_cast<const char*>(base+imports->Name),"binkw32.dll") || !imports->OriginalFirstThunk) continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->OriginalFirstThunk);
        auto* cells=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;++names,++cells){
            if(IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            const auto* name=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            for(auto& hook:hooks) if(std::strcmp(reinterpret_cast<const char*>(name->Name),hook.name)==0){
                hook.original=reinterpret_cast<void*>(GetProcAddress(module,hook.name));
                hook.cell=reinterpret_cast<void**>(&cells->u1.Function);
                if(!hook.original || *hook.cell!=hook.original) return false;
            }
        }
    }
    for(const auto& hook:hooks) if(!hook.cell) return false;
    if(!movie.Open(nonce,true)) return false;
    original_copy=reinterpret_cast<CopyFunction>(hooks[0].original);
    original_close=reinterpret_cast<CloseFunction>(hooks[1].original);
    original_pause=reinterpret_cast<PauseFunction>(hooks[2].original);
    original_summary=reinterpret_cast<bink::GetSummaryFunction>(hooks[3].original);
    bink_base=reinterpret_cast<std::uintptr_t>(module);
    // All four supported-game IAT cells occupy one page. Change its protection
    // once and roll back our own cells if installation cannot be completed.
    auto first=reinterpret_cast<std::uintptr_t>(hooks[0].cell),last=first;
    for(const auto& hook:hooks){const auto address=reinterpret_cast<std::uintptr_t>(hook.cell);
        first=std::min(first,address);last=std::max(last,address);}
    SYSTEM_INFO system{};GetSystemInfo(&system);
    if(first/system.dwPageSize!=last/system.dwPageSize) return false;
    DWORD old{},ignored{};
    auto* region=reinterpret_cast<void*>(first);const auto region_size=last-first+sizeof(void*);
    if(!VirtualProtect(region,region_size,PAGE_READWRITE,&old)) return false;
    unsigned changed=0;
    for(auto& hook:hooks){
        if(InterlockedCompareExchangePointer(hook.cell,hook.replacement,hook.original)!=hook.original) break;
        ++changed;
    }
    if(changed!=std::size(hooks)){
        for(unsigned i=0;i<changed;++i) InterlockedCompareExchangePointer(hooks[i].cell,hooks[i].original,hooks[i].replacement);
        (void)VirtualProtect(region,region_size,old,&ignored);return false;
    }
    if(!VirtualProtect(region,region_size,old,&ignored)){
        for(auto& hook:hooks) InterlockedCompareExchangePointer(hook.cell,hook.original,hook.replacement);
        (void)VirtualProtect(region,region_size,old,&ignored);return false;
    }
    installed=true;(void)AppendPersistentProbeLogLine("bink-hooks-installed movie=direct-buffer summary=exact-overflow-guard");return true;
}
}
