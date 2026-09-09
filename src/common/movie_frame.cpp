#include "movie_frame.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace k2vr::ipc {
namespace {
constexpr std::uint32_t kMagic=0x564D324B; // K2MV, ABI 1
struct alignas(8) Shared {
    std::uint32_t magic,version,width,height;
    std::uint64_t sequence,tick_ms;
    std::uint32_t active,reserved;
    std::uint8_t pixels[kMovieMaximumBytes];
};
static_assert(offsetof(Shared,pixels)==40);
bool ValidDimensions(std::uint32_t w,std::uint32_t h) noexcept {
    return w && h && w<=kMovieMaximumWidth && h<=kMovieMaximumHeight;
}
struct Lock {
    HANDLE mutex; bool acquired{},abandoned{};
    explicit Lock(void* value,DWORD timeout=0):mutex(value) {
        const auto result=WaitForSingleObject(mutex,timeout);
        acquired=result==WAIT_OBJECT_0 || result==WAIT_ABANDONED;
        abandoned=result==WAIT_ABANDONED;
    }
    ~Lock(){if(acquired) ReleaseMutex(mutex);}
};
}
MovieFrameChannel::~MovieFrameChannel(){Close();}
void MovieFrameChannel::Close() noexcept {
    if(view_) UnmapViewOfFile(view_);
    if(mapping_) CloseHandle(mapping_);
    if(mutex_) CloseHandle(mutex_);
    view_=mapping_=mutex_=nullptr; nonce_={}; writer_=false;
}
bool MovieFrameChannel::Open(SessionNonce nonce,bool writer) noexcept {
    if(view_) return nonce.low==nonce_.low && nonce.high==nonce_.high && writer==writer_;
    if(!IsValid(nonce)) return false;
    wchar_t name[112]{},mutex_name[120]{};
    std::swprintf(name,112,L"Local\\Kotor2VR-Movie-v1-%016llX%016llX",
        static_cast<unsigned long long>(nonce.high),static_cast<unsigned long long>(nonce.low));
    std::swprintf(mutex_name,120,L"%ls-lock",name);
    HANDLE mutex=writer ? CreateMutexW(nullptr,FALSE,mutex_name):
        OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,mutex_name);
    if(!mutex) return false;
    HANDLE mapping=writer ? CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name):
        OpenFileMappingW(FILE_MAP_READ,FALSE,name);
    const bool existing=writer && GetLastError()==ERROR_ALREADY_EXISTS;
    if(!mapping || existing){if(mapping) CloseHandle(mapping);CloseHandle(mutex);return false;}
    void* view=MapViewOfFile(mapping,writer ? FILE_MAP_ALL_ACCESS:FILE_MAP_READ,0,0,sizeof(Shared));
    if(!view){CloseHandle(mapping);CloseHandle(mutex);return false;}
    mapping_=mapping;mutex_=mutex;view_=view;nonce_=nonce;writer_=writer;
    if(writer){
        Lock lock(mutex_);
        if(!lock.acquired){Close();return false;}
        auto& shared=*static_cast<Shared*>(view_);
        shared.version=1;shared.magic=kMagic;
    }
    return true;
}
bool MovieFrameChannel::Publish(std::uint32_t width,std::uint32_t height,
    std::span<const std::uint8_t> pixels) noexcept {
    if(!view_ || !writer_ || !ValidDimensions(width,height) || pixels.size()!=std::size_t(width)*height*4) return false;
    Lock lock(mutex_);if(!lock.acquired) return false;
    auto& s=*static_cast<Shared*>(view_);
    s.active=0;
    std::memcpy(s.pixels,pixels.data(),pixels.size());
    s.width=width;s.height=height;s.tick_ms=GetTickCount64();
    if(++s.sequence==0) ++s.sequence;
    s.active=1;return true;
}
void MovieFrameChannel::EndMovie() noexcept {
    if(!view_ || !writer_) return;
    Lock lock(mutex_,50);if(!lock.acquired) return; // bounded teardown; timestamp also expires
    static_cast<Shared*>(view_)->active=0;
}
MovieRead MovieFrameChannel::ReadLatest(MovieFrame& frame) noexcept {
    if(!view_ || writer_) return MovieRead::Unavailable;
    Lock lock(mutex_);if(!lock.acquired) return MovieRead::Unavailable;
    if(lock.abandoned) return MovieRead::Inactive;
    const auto& s=*static_cast<const Shared*>(view_);
    const auto now=GetTickCount64();
    if(s.magic!=kMagic || s.version!=1 || s.active!=1 || !s.sequence ||
       now<s.tick_ms || now-s.tick_ms>kMovieFrameTimeoutMs || !ValidDimensions(s.width,s.height)) return MovieRead::Inactive;
    if(frame.sequence==s.sequence) return MovieRead::Unchanged;
    try {frame.pixels.resize(std::size_t(s.width)*s.height*4);}catch(...){return MovieRead::Unavailable;}
    std::memcpy(frame.pixels.data(),s.pixels,frame.pixels.size());
    frame.sequence=s.sequence;frame.tick_ms=s.tick_ms;frame.width=s.width;frame.height=s.height;
    return MovieRead::Fresh;
}
}
