#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2vr::game32 {
// Bink 1.5 surface IDs, verified against the supported decoder's dispatch.
// Buffer access hints and COPYALL do not change the copied pixel layout.
// Keep scaling/interlacing and unknown flags intact so the format switch rejects them.
constexpr std::uint32_t BinkPixelSurface(std::uint32_t flags) noexcept {
    constexpr std::uint32_t layout_neutral_flags=0x08000000U | 0x04000000U | 0x80000000U;
    return flags & ~layout_neutral_flags;
}
constexpr unsigned BinkBytesPerPixel(std::uint32_t surface) noexcept {
    surface=BinkPixelSurface(surface);
    switch(surface){case 1:case 2:return 3;case 3:case 4:case 5:case 6:return 4;
        case 9:case 10:return 2;default:return 0;}
}
inline bool ConvertBinkPixels(std::span<const std::uint8_t> source,
    std::uint32_t width,std::uint32_t height,std::uint32_t pitch,std::uint32_t surface,
    std::span<std::uint8_t> output) noexcept {
    const auto bpp=BinkBytesPerPixel(surface);
    surface=BinkPixelSurface(surface);
    if(!bpp || !width || !height || std::uint64_t(width)*bpp>pitch ||
       std::uint64_t(pitch)*(height-1)+std::uint64_t(width)*bpp>source.size() ||
       std::uint64_t(width)*height*4!=output.size()) return false;
    for(std::uint32_t y=0;y<height;++y) for(std::uint32_t x=0;x<width;++x){
        const auto* p=source.data()+std::size_t(y)*pitch+std::size_t(x)*bpp;
        auto* q=output.data()+(std::size_t(y)*width+x)*4;
        if(bpp==2){
            const auto v=std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8);
            const auto b=v&31U,r=(v>>(surface==10 ? 11:10))&31U;
            const auto g=(v>>5)&(surface==10 ? 63U:31U);
            q[0]=static_cast<std::uint8_t>((b<<3)|(b>>2));
            q[1]=static_cast<std::uint8_t>(surface==10 ? (g<<2)|(g>>4):(g<<3)|(g>>2));
            q[2]=static_cast<std::uint8_t>((r<<3)|(r>>2));
        }else{
            const bool reversed=surface==2 || surface==4 || surface==6;
            q[0]=p[reversed ? 2:0];q[1]=p[1];q[2]=p[reversed ? 0:2];
        }
        q[3]=255; // a movie is opaque, including decoders leaving X bytes unset
    }
    return true;
}
}
