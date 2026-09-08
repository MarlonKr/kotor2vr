#include "scene_replay.hpp"
#include "native_stereo.hpp"
#include "probe.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <gl/GL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace k2vr::game32 {
bool AppendPersistentProbeLogLine(std::string_view message) noexcept {
    std::printf("%.*s\n",static_cast<int>(message.size()),message.data()); return true;
}
}
namespace {
using Bind=void(APIENTRY*)(GLenum,GLuint);
using Gen=void(APIENTRY*)(GLsizei,GLuint*);
using Env=void(APIENTRY*)(GLenum,GLuint,const GLfloat*);
using GetEnv=void(APIENTRY*)(GLenum,GLuint,GLfloat*);
using Source=void(APIENTRY*)(GLenum,GLenum,GLsizei,const void*);
using Attach=void(APIENTRY*)(GLenum,GLenum,GLenum,GLuint,GLint);
using Blit=void(APIENTRY*)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
using BeginQuery=void(APIENTRY*)(GLenum,GLuint);
using EndQuery=void(APIENTRY*)(GLenum);
using GetQueryResult=void(APIENTRY*)(GLuint,GLenum,GLuint*);
using MultisampleStorage=void(APIENTRY*)(GLenum,GLsizei,GLenum,GLsizei,GLsizei);
using AttachRenderbuffer=void(APIENTRY*)(GLenum,GLenum,GLenum,GLuint);
Bind mirror_bind{};
Blit mirror_blit{};
GLuint mirror_atlas{};
Env set_env{};
unsigned cpu_calls{};
bool hide_test{};
bool unsafe_test{},mirror_failure{},fixed_test{};
unsigned mirror_calls{};
bool MirrorStub() noexcept {
    ++mirror_calls;
    if (mirror_failure) return false;
    GLint old_read{},old_buffer{};
    glGetIntegerv(0x8CAA,&old_read); glGetIntegerv(GL_READ_BUFFER,&old_buffer);
    mirror_bind(0x8CA8,mirror_atlas); glReadBuffer(0x8CE0);
    // Reverse the actual atlas Y convention; the scene replay must not paint
    // its translated monitor triangle over this left-eye image afterwards.
    mirror_blit(0,64,64,0,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    mirror_bind(0x8CA8,static_cast<GLuint>(old_read)); glReadBuffer(static_cast<GLenum>(old_buffer));
    return glGetError()==GL_NO_ERROR;
}
int CpuChecks() {
    using namespace k2vr::game32;
    if (MonitorMirrorOption("") || MonitorMirrorOption("0") || MonitorMirrorOption("true") ||
        MonitorMirrorOption("01") || MonitorMirrorOption("1 ") || !MonitorMirrorOption("1")) return 40;
    for (const auto size:std::array<std::array<int,4>,7>{{{3440,1440,2040,2232},{1920,1080,2040,2232},
        {64,64,64,64},{1080,1920,2040,2232},{1,1,2040,2232},{8192,8192,8192,1},{8192,8192,1,8192}}}) {
        const auto r=FitMonitorMirror(size[0],size[1],size[2],size[3]);
        if (r.x<0 || r.y<0 || r.width<0 || r.height<0 || r.x+r.width>size[0] || r.y+r.height>size[1]) return 41;
        const auto error=static_cast<std::int64_t>(r.width)*size[3]-static_cast<std::int64_t>(r.height)*size[2];
        if (std::abs(error)>(std::max)(size[2],size[3])) return 42;
    }
    if (FitMonitorMirror(0,100,10,10).width || FitMonitorMirror(100,100,-1,10).height) return 43;
    const auto wide=FitMonitorMirror(3440,1440,2040,2232);
    if (wide.width!=1316 || wide.height!=1440 || wide.x!=1062 || wide.y!=0) return 44;
    std::puts("PASS CPU: exact opt-in/default-off, aspect fit, centering, extreme and invalid dimensions; no GL context created.");
    return 0;
}
std::uint64_t __fastcall DrawScene(void*,void*) {
    ++cpu_calls;
    if (unsafe_test) k2vr::game32::NotifySceneReplayFramebufferRead();
    // The final color, material and texture coordinate must survive discard.
    glColor4f(1,0,0,1);
    // The client memory is overwritten immediately after recording. A replay
    // must use the captured vertices with the CURRENT camera's MVP matrix.
    GLfloat vertices[]={-0.25F,-0.25F,0, 0.25F,-0.25F,0, 0,0.25F,0};
    const GLushort indices[]={0,1,2};
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3,GL_FLOAT,0,vertices);
    const bool hidden=hide_test && k2vr::game32::BeginSceneReplayHiddenDraw();
    glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,indices);
    if (hidden) k2vr::game32::EndSceneReplayHiddenDraw();
    std::memset(vertices,0xff,sizeof(vertices));
    glEnable(GL_COLOR_MATERIAL); glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);
    glColor4f(0,0.5F,0.25F,1); glDisable(GL_COLOR_MATERIAL);
    glTexCoord2f(0.25F,0.75F);
    const GLfloat green[]={0,1,0,1}; set_env(0x8620,0,green);
    return 0x123456789abcdef0ULL;
}
}
int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--cpu")==0) return CpuChecks();
    // This executable deliberately requires explicit GPU authorization to run
    // its GL branch; --cpu is safe while the game/neural workload is active.
    if (argc!=2 || std::strcmp(argv[1],"--gpu")!=0) { std::puts("Use --cpu or --gpu (real GL context)."); return 2; }
    WNDCLASSW wc{}; wc.style=CS_OWNDC; wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"K2vrSceneReplayTest";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"K2VR hidden GL test",WS_POPUP,0,0,64,64,
        nullptr,nullptr,wc.hInstance,nullptr);
    const HDC dc=GetDC(window);
    PIXELFORMATDESCRIPTOR pf{}; pf.nSize=sizeof(pf); pf.nVersion=1;
    pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pf.iPixelType=PFD_TYPE_RGBA; pf.cColorBits=32;
    if (!SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf)) return 10;
    const HGLRC context=wglCreateContext(dc);
    if (!wglMakeCurrent(dc,context)) return 11;
    auto bind=reinterpret_cast<Bind>(wglGetProcAddress("glBindFramebuffer"));
    auto gen=reinterpret_cast<Gen>(wglGetProcAddress("glGenFramebuffers"));
    auto attach=reinterpret_cast<Attach>(wglGetProcAddress("glFramebufferTexture2D"));
    mirror_bind=bind;
    mirror_blit=reinterpret_cast<Blit>(wglGetProcAddress("glBlitFramebuffer"));
    auto gen_query=reinterpret_cast<Gen>(wglGetProcAddress("glGenQueries"));
    auto begin_query=reinterpret_cast<BeginQuery>(wglGetProcAddress("glBeginQuery"));
    auto end_query=reinterpret_cast<EndQuery>(wglGetProcAddress("glEndQuery"));
    auto get_query_result=reinterpret_cast<GetQueryResult>(wglGetProcAddress("glGetQueryObjectuiv"));
    auto gen_rb=reinterpret_cast<Gen>(wglGetProcAddress("glGenRenderbuffers"));
    auto bind_rb=reinterpret_cast<Bind>(wglGetProcAddress("glBindRenderbuffer"));
    auto storage_ms=reinterpret_cast<MultisampleStorage>(wglGetProcAddress("glRenderbufferStorageMultisample"));
    auto attach_rb=reinterpret_cast<AttachRenderbuffer>(wglGetProcAddress("glFramebufferRenderbuffer"));
    auto gen_program=reinterpret_cast<Gen>(wglGetProcAddress("glGenProgramsARB"));
    auto bind_program=reinterpret_cast<Bind>(wglGetProcAddress("glBindProgramARB"));
    auto source=reinterpret_cast<Source>(wglGetProcAddress("glProgramStringARB"));
    set_env=reinterpret_cast<Env>(wglGetProcAddress("glProgramEnvParameter4fvARB"));
    auto get_env=reinterpret_cast<GetEnv>(wglGetProcAddress("glGetProgramEnvParameterfvARB"));
    if (!bind || !gen || !attach || !gen_program || !bind_program || !source || !set_env || !get_env || !mirror_blit) return 12;
    if (!gen_query || !begin_query || !end_query || !get_query_result) return 22;
    if (!gen_rb || !bind_rb || !storage_ms || !attach_rb) return 24;
    GLuint query{}; gen_query(1,&query);
    GLuint program{}; gen_program(1,&program); bind_program(0x8620,program);
    constexpr char shader[]="!!ARBvp1.0\nPARAM m[4]={state.matrix.mvp};\nDP4 result.position.x,m[0],vertex.position;\nDP4 result.position.y,m[1],vertex.position;\nDP4 result.position.z,m[2],vertex.position;\nDP4 result.position.w,m[3],vertex.position;\nMOV result.color,program.env[0];\nEND\n";
    source(0x8620,0x8875,sizeof(shader)-1,shader); glEnable(0x8620);
    GLuint fbos[6]{},textures[6]{}; gen(6,fbos); glGenTextures(6,textures);
    for (int i=0;i<6;++i) {
        glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        bind(0x8D40,fbos[i]); attach(0x8D40,0x8CE0,GL_TEXTURE_2D,textures[i],0);
        glDrawBuffer(0x8CE0); glReadBuffer(0x8CE0);
    }
    mirror_atlas=fbos[3];
    GLuint monitor_ms{}; gen_rb(1,&monitor_ms); bind_rb(0x8D41,monitor_ms);
    GLint max_samples{}; glGetIntegerv(0x8D57,&max_samples);
    if (max_samples<2) return 25;
    storage_ms(0x8D41,(std::min)(4,max_samples),GL_RGBA8,64,64);
    bind(0x8D40,fbos[4]); attach_rb(0x8D40,0x8CE0,0x8D41,monitor_ms);
    if (glGetError()!=GL_NO_ERROR) return 26;
    const auto render=reinterpret_cast<k2vr::game32::SceneRenderMethod>(&DrawScene);
    const GLfloat red[]={1,0,0,1};
    for (unsigned frame=0;frame<15;++frame) {
        const bool monitor_only=frame==6; // Repeated XR request skipped both eyes.
        const bool mirror_requested=frame>=8 && frame<=13;
        const bool active_query=frame==13;
        mirror_failure=frame==10; unsafe_test=frame==11; fixed_test=frame==9;
        const bool discarded=mirror_requested && !mirror_failure && !unsafe_test && !active_query;
        const auto prior_mirror_calls=mirror_calls;
        if (fixed_test) glDisable(0x8620); else glEnable(0x8620);
        hide_test=frame==4; // Ordinary geometry recovers on the very next frame.
        set_env(0x8620,0,red);
        GLint initial_depth{}; glGetIntegerv(GL_ATTRIB_STACK_DEPTH,&initial_depth);
        if (!k2vr::game32::BeginSceneReplayFrame()) { std::puts("begin failed"); return 13; }
        for (int view=0;view<3;++view) {
            if (monitor_only && view<2) continue;
            bind(0x8D40,fbos[view==2 && frame==12 ? 4:view]); glDrawBuffer(0x8CE0); glReadBuffer(0x8CE0);
            glViewport(0,0,view==1 ? 32:64,64);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glTranslatef(view==1 ? -0.25F:(view==2 ? 0.25F:0.0F),0,0);
            if (view==2) k2vr::game32::SetSceneReplayFinalPass(mirror_requested ? &MirrorStub:nullptr);
            if (view==2 && active_query) begin_query(0x8914,query);
            if (k2vr::game32::RenderSceneWithReplay(&cpu_calls,render)!=0x123456789abcdef0ULL) return 14;
            if (view==2 && active_query) end_query(0x8914);
            if (view==0) {
                bind(0x8CA9,mirror_atlas); glDrawBuffer(0x8CE0);
                mirror_blit(0,0,64,64,0,64,64,0,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                bind(0x8CA9,fbos[0]); glDrawBuffer(0x8CE0);
            }
        }
        k2vr::game32::FinishSceneReplayFrame();
        if (frame==12) {
            bind(0x8CA8,fbos[4]); bind(0x8CA9,fbos[5]);
            glReadBuffer(0x8CE0); glDrawBuffer(0x8CE0);
            mirror_blit(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
        // Check AFTER cleanup, so an accidental full Finish replay also fails.
        std::array<unsigned char,64*64*4> left_pixels{};
        for (int view=0;view<3;++view) {
            if (monitor_only && view<2) continue;
            bind(0x8D40,fbos[view==2 && frame==12 ? 5:view]); glReadBuffer(0x8CE0);
            std::array<unsigned char,64*64*4> pixels{};
            glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            if (view==0) left_pixels=pixels;
            if (view==2 && discarded && pixels!=left_pixels) return 21; // includes Y and alpha
            double sum{}; unsigned red_count{},green_count{};
            for (unsigned i=0;i<64*64;++i) {
                if (pixels[i*4]>200) { ++red_count; sum+=i%64+0.5; }
                if (pixels[i*4+1]>200) ++green_count;
            }
            const double center=red_count ? sum/red_count:0,expected=view==1 ? 12:(view==2 && !discarded ? 40:32);
            std::printf("frame=%u view=%d red=%u green=%u center_x=%.3f expected=%.3f\n",frame,view,red_count,green_count,center,expected);
            if (hide_test && view<2) { if (red_count || green_count) return 17; }
            else if (!red_count || green_count || std::abs(center-expected)>0.6) return 15;
            if (glIsEnabled(0x8C89)) return 18;
        }
        GLint final_depth{}; glGetIntegerv(GL_ATTRIB_STACK_DEPTH,&final_depth);
        GLfloat final_env[4]{}; get_env(0x8620,0,final_env);
        GLfloat current_color[4]{},material[4]{},texcoord[4]{};
        glGetFloatv(GL_CURRENT_COLOR,current_color); glGetMaterialfv(GL_FRONT,GL_DIFFUSE,material);
        glGetFloatv(GL_CURRENT_TEXTURE_COORDS,texcoord);
        if (current_color[1]!=0.5F || material[1]!=0.5F || texcoord[1]!=0.75F || glIsEnabled(0x8C89)) return 19;
        if (mirror_calls-prior_mirror_calls!=(mirror_requested && !unsafe_test && !active_query ? 1U:0U)) return 20;
        if (active_query) {
            GLuint samples{}; get_query_result(query,0x8866,&samples);
            if (!samples) return 23; // Query fallback actually rasterized.
        }
        const auto error=glGetError();
        if (initial_depth!=final_depth || cpu_calls!=frame+1 || final_env[1]!=1.0F || error) {
            std::printf("state failure depth=%d/%d cpu=%u env=%g error=%x\n",initial_depth,final_depth,cpu_calls,final_env[1],error); return 16;
        }
    }
    wglMakeCurrent(nullptr,nullptr); wglDeleteContext(context); ReleaseDC(window,dc); DestroyWindow(window);
    std::puts("PASS GPU: ordinary and discarded replay, fixed-function/ARB, current attributes/material, explicit discard restore, mirror failure/read fallback and following-frame recovery.");
    return 0;
}
