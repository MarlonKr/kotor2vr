#include "depth_pack.hpp"
#include <array>
#include <cmath>
#include <cstdio>

int main() {
    WNDCLASSW wc{}; wc.style=CS_OWNDC; wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"K2vrDepthPackTest";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"Hidden depth packing test",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    HDC dc=GetDC(window); PIXELFORMATDESCRIPTOR pf{}; pf.nSize=sizeof(pf); pf.nVersion=1;
    pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER; pf.iPixelType=PFD_TYPE_RGBA; pf.cColorBits=32;
    if (!SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf)) return 1;
    HGLRC context=wglCreateContext(dc); if (!wglMakeCurrent(dc,context)) return 2;
    using Gen=void(APIENTRY*)(GLsizei,GLuint*); using Bind=void(APIENTRY*)(GLenum,GLuint);
    using Attach=void(APIENTRY*)(GLenum,GLenum,GLenum,GLuint,GLint);
    auto gen=reinterpret_cast<Gen>(wglGetProcAddress("glGenFramebuffers"));
    auto bind=reinterpret_cast<Bind>(wglGetProcAddress("glBindFramebuffer"));
    auto attach=reinterpret_cast<Attach>(wglGetProcAddress("glFramebufferTexture2D"));
    if (!gen || !bind || !attach) return 3;
    constexpr unsigned width=32,height=16;
    std::array<float,width*height> source{},actual{};
    const float samples[]={0,1,0.5F,0.1F,0.999999F,0.0000001F,0.82F};
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
        source[y*width+x]=samples[(y*13+x)%7];
    GLuint depth{},color{},fbo{}; glGenTextures(1,&depth); glBindTexture(GL_TEXTURE_2D,depth);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,0x81A6,width,height,0,GL_DEPTH_COMPONENT,GL_FLOAT,source.data()); // D24
    glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT,GL_FLOAT,actual.data());
    glGenTextures(1,&color); glBindTexture(GL_TEXTURE_2D,color);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    gen(1,&fbo); bind(0x8D40,fbo); attach(0x8D40,0x8CE0,GL_TEXTURE_2D,color,0);
    glDrawBuffer(0x8CE0); glReadBuffer(0x8CE0); glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    k2vr::game32::DepthPack pack; if (!pack.Initialize()) return 4;
    glViewport(1,2,3,4); glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE);
    glEnable(GL_SCISSOR_TEST); glScissor(0,0,1,1); glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_FALSE);
    if (!pack.Draw(depth,5,9,width,height)) { std::printf("pack failed GL=%x\n",glGetError()); return 5; }
    GLint viewport[4]{}; GLboolean mask[4]{}; glGetIntegerv(GL_VIEWPORT,viewport); glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    if (viewport[0]!=1 || viewport[3]!=4 || !glIsEnabled(GL_BLEND) || !glIsEnabled(GL_SCISSOR_TEST) || mask[0] || !mask[1] || mask[3]) return 6;
    std::array<unsigned char,width*height*4> pixels{};
    glReadPixels(5,9,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    float worst=0; unsigned invalid_alpha=0;
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const auto* p=pixels.data()+(y*width+x)*4;
        const float decoded=static_cast<float>((unsigned(p[0])<<16)|(unsigned(p[1])<<8)|p[2])/16777215.F;
        const float error=std::abs(decoded-actual[(height-1-y)*width+x]);
        if (error>worst) worst=error;
        if (p[3]!=255) ++invalid_alpha;
    }
    const bool ok=worst<=1.2e-7F && !invalid_alpha && glGetError()==GL_NO_ERROR;
    std::printf("D24 GPU packing: %s samples=%u max_error=%.9g flipped_y=1 state_restored=1\n",ok ? "PASS":"FAIL",width*height,worst);
    wglMakeCurrent(nullptr,nullptr); wglDeleteContext(context); ReleaseDC(window,dc); DestroyWindow(window);
    return ok ? 0:7;
}
