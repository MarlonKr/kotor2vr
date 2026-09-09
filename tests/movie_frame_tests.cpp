#include "movie_frame.hpp"
#include "../src/game32/bink_pixels.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <iostream>

int main(){
    unsigned checks=0,failures=0;
    const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<label<<'\n';}};
    using namespace k2vr::ipc;using k2vr::game32::ConvertBinkPixels;
    std::array<std::uint8_t,16> pixels{};
    const std::array<std::uint8_t,12> bgr={0,0,255,0,255,0,255,0,0,255,255,255};
    check(ConvertBinkPixels(bgr,2,2,6,1,pixels),"BGR24 accepted");
    check(pixels==std::array<std::uint8_t,16>{0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255},"color order, opaque alpha and top-down rows");
    std::array<std::uint8_t,4> single{};
    const std::array<std::uint8_t,4> rgbx={240,100,20,0};
    check(ConvertBinkPixels(rgbx,1,1,4,4,single) && single==std::array<std::uint8_t,4>{20,100,240,255},"RGBX32 channel swap");
    const std::array<std::uint8_t,2> green565={0xE0,0x07},white555={0xFF,0x7F};
    check(ConvertBinkPixels(green565,1,1,2,10,single) && single==std::array<std::uint8_t,4>{0,255,0,255},"RGB565 green six bits");
    check(ConvertBinkPixels(white555,1,1,2,9,single) && single==std::array<std::uint8_t,4>{255,255,255,255},"RGB555 full range");
    const std::array<std::uint8_t,8> padded={1,2,3,77,4,5,6,88};
    std::array<std::uint8_t,8> pair{};
    check(ConvertBinkPixels(padded,1,2,4,1,pair) && pair==std::array<std::uint8_t,8>{1,2,3,255,4,5,6,255},"row padding excluded");
    check(!ConvertBinkPixels(bgr,2,2,5,1,pixels),"short pitch rejected");
    check(!ConvertBinkPixels(std::span(bgr).first(11),2,2,6,1,pixels),"truncated buffer rejected");
    check(!ConvertBinkPixels(bgr,2,2,6,0,pixels),"unknown format rejected");
    const std::array<std::uint8_t,4> bgrx={20,100,240,0};
    check(ConvertBinkPixels(bgrx,1,1,4,0x08000003U,single) &&
        single==std::array<std::uint8_t,4>{20,100,240,255},"observed slow BGRX32 movie buffer accepted");
    check(ConvertBinkPixels(rgbx,1,1,4,0x8C000004U,single) &&
        single==std::array<std::uint8_t,4>{20,100,240,255},"layout-neutral flags retain reversed channel order");
    check(ConvertBinkPixels(green565,1,1,2,0x0800000AU,single) &&
        single==std::array<std::uint8_t,4>{0,255,0,255},"slow RGB565 retains six green bits");
    check(!ConvertBinkPixels(bgrx,1,1,4,0x10000003U,single),"scaled copy rejected");
    check(!ConvertBinkPixels(bgrx,1,1,4,0x20000003U,single),"interlaced copy rejected");
    check(!ConvertBinkPixels(bgrx,1,1,4,0x00000103U,single),"unknown copy modifier rejected");

    const SessionNonce nonce{GetTickCount64(),0x4D4F564900000000ULL | GetCurrentProcessId()};
    MovieFrameChannel writer,reader,duplicate;MovieFrame frame;
    check(!reader.Open({},false),"invalid nonce rejected");
    check(writer.Open(nonce,true),"create movie channel");
    check(!duplicate.Open(nonce,true),"duplicate producer rejected");
    check(reader.Open(nonce,false),"open movie reader");
    check(reader.ReadLatest(frame)==MovieRead::Inactive,"no unwritten frame published");
    check(!writer.Publish(0,1,pixels),"empty frame rejected");
    check(!writer.Publish(2,2,std::span(pixels).first(15)),"partial frame rejected");
    check(writer.Publish(2,2,pixels),"publish movie");
    check(reader.ReadLatest(frame)==MovieRead::Fresh && frame.width==2 && frame.height==2 &&
        std::equal(frame.pixels.begin(),frame.pixels.end(),pixels.begin(),pixels.end()),"complete consistent snapshot");
    const auto sequence=frame.sequence;
    check(reader.ReadLatest(frame)==MovieRead::Unchanged,"unchanged frame retained");
    writer.EndMovie();check(reader.ReadLatest(frame)==MovieRead::Inactive,"close immediately ends movie");
    check(writer.Publish(1,1,single) && reader.ReadLatest(frame)==MovieRead::Fresh && frame.sequence>sequence &&
        frame.pixels.size()==4,"next movie may change dimensions");
    check(!reader.Publish(1,1,single),"reader cannot publish");
    check(writer.ReadLatest(frame)==MovieRead::Unavailable,"writer cannot read as consumer");
    reader.Close();writer.Close();
    std::cout<<checks-failures<<'/'<<checks<<" movie transport and pixel checks passed\n";
    return failures ? 1:0;
}
