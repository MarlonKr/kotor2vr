#include "scene_replay.hpp"
#include "gl_gpu_timing.hpp"
#include "probe.hpp"
#include "nv_dx_interop_bridge.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <gl/GL.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace k2vr::game32 {
namespace {
constexpr GLenum kReadFbo=0x8CA8, kDrawFbo=0x8CA9;
constexpr GLenum kVertexProgram=0x8620, kFragmentProgram=0x8804;
using Bind=void(APIENTRY*)(GLenum,GLuint);
using GetProgram=void(APIENTRY*)(GLenum,GLenum,GLint*);
using GetEnv=void(APIENTRY*)(GLenum,GLuint,GLfloat*);
using SetEnv=void(APIENTRY*)(GLenum,GLuint,const GLfloat*);
using GetQuery=void(APIENTRY*)(GLenum,GLenum,GLint*);
struct ProgramState {
    GLuint binding{};
    GLint count{};
    std::array<std::array<GLfloat,4>,256> env{};
};
struct ReplayState {
    HGLRC context{};
    GLuint list{};
    bool visibility_used{};
    Bind bind_fbo{},bind_program{};
    GetProgram get_program{};
    GetEnv get_env{};
    SetEnv set_env{};
    ProgramState vertex{},fragment{};
    void* scene{};
    std::uint64_t result{},frames{},replays{};
    GLint capture_read{},capture_draw{};
    bool active{},ready{},compiling{},attrib_saved{},final_pass{};
    DWORD next_log{};
    GetQuery get_query{};
    SceneReplayMirror mirror{};
    bool discard_supported{},framebuffer_read{};
    std::uint64_t mirrored{};
};
thread_local ReplayState state{};

template<class T> bool Resolve(T& output,const char* name) noexcept {
    output=reinterpret_cast<T>(wglGetProcAddress(name));
    return IsUsableWglProcAddressValue(reinterpret_cast<std::uintptr_t>(output));
}
void ReadProgram(GLenum target,ProgramState& program) noexcept {
    GLint binding{};
    state.get_program(target,0x8677,&binding); // PROGRAM_BINDING_ARB
    program.binding=static_cast<GLuint>(binding);
    for (GLint i=0;i<program.count;++i)
        state.get_env(target,static_cast<GLuint>(i),program.env[static_cast<std::size_t>(i)].data());
}
void RestoreProgram(GLenum target,const ProgramState& program) noexcept {
    state.bind_program(target,program.binding);
    for (GLint i=0;i<program.count;++i)
        state.set_env(target,static_cast<GLuint>(i),program.env[static_cast<std::size_t>(i)].data());
}

void ConfigureVisibility(bool hide) noexcept {
    glNewList(state.list+1,GL_COMPILE);
    glPushAttrib(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    if (hide) {
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
        glDepthMask(GL_FALSE); glStencilMask(0);
    }
    glEndList();
}

// GL's attribute stack does not include FBO bindings or ARB program constants.
// Restore the recording's initial server state without replacing the current
// camera matrices, render target, or viewport. The replay leaves the same final
// server state that the engine's state cache remembers from the recorded pass.
void Replay(bool keep_initial_state) noexcept {
    GLint read{},draw{},read_buffer{},draw_buffer{},viewport[4]{};
    glGetIntegerv(0x8CAA,&read); glGetIntegerv(0x8CA6,&draw);
    glGetIntegerv(GL_READ_BUFFER,&read_buffer); glGetIntegerv(GL_DRAW_BUFFER,&draw_buffer);
    glGetIntegerv(GL_VIEWPORT,viewport);
    state.bind_fbo(kReadFbo,static_cast<GLuint>(state.capture_read));
    state.bind_fbo(kDrawFbo,static_cast<GLuint>(state.capture_draw));
    glPopAttrib(); state.attrib_saved=false;
    if (keep_initial_state) { glPushAttrib(GL_ALL_ATTRIB_BITS); state.attrib_saved=true; }
    state.bind_fbo(kReadFbo,static_cast<GLuint>(read));
    state.bind_fbo(kDrawFbo,static_cast<GLuint>(draw));
    glReadBuffer(static_cast<GLenum>(read_buffer)); glDrawBuffer(static_cast<GLenum>(draw_buffer));
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    // Enable AFTER restoring initial attributes. Do not use Push/PopAttrib to
    // restore this extension state: the previous ego smoke exposed a leak.
    bool discard=false;
    if (!keep_initial_state && state.mirror && state.discard_supported && !state.framebuffer_read) {
        GLint query{},mode{};
        state.get_query(0x8914,0x8865,&query); // SAMPLES_PASSED, CURRENT_QUERY
        glGetIntegerv(GL_RENDER_MODE,&mode);
        if (!query && mode==GL_RENDER && !glIsEnabled(0x8C89)) discard=state.mirror();
    }
    if (discard) glEnable(0x8C89); // RASTERIZER_DISCARD, no transform feedback
    __try { glCallList(state.list); }
    __finally { if (discard) glDisable(0x8C89); }
    if (discard) ++state.mirrored;
    ++state.replays;
}
}

void NotifySceneReplayContextDeleted(void* context) noexcept {
    gpu_timing::ContextDeleted(context);
    // A later HGLRC may reuse the same numeric handle. Never reuse its lists.
    if (context && state.context==context) state={};
}

bool BeginSceneReplayFrame() noexcept {
    if (state.active) return false;
    const auto context=wglGetCurrentContext();
    if (!context) return false;
    if (state.context!=context) {
        // Objects belong to their context. Do not delete names in another one.
        state={}; state.context=context;
        if (!Resolve(state.bind_fbo,"glBindFramebuffer") ||
            !Resolve(state.bind_program,"glBindProgramARB") ||
            !Resolve(state.get_program,"glGetProgramivARB") ||
            !Resolve(state.get_env,"glGetProgramEnvParameterfvARB") ||
            !Resolve(state.set_env,"glProgramEnvParameter4fvARB")) return false;
        state.get_program(kVertexProgram,0x88B5,&state.vertex.count);
        state.get_program(kFragmentProgram,0x88B5,&state.fragment.count);
        if (state.vertex.count<96 || state.vertex.count>256 ||
            state.fragment.count<0 || state.fragment.count>256) return false;
        state.list=glGenLists(3);
        if (state.list) {
            glNewList(state.list+2,GL_COMPILE); glPopAttrib(); glEndList();
        }
        const auto* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        // Deliberately require desktop core 3.x+ semantics on the compatibility
        // context, rather than inferring support from a non-null extension proc.
        state.discard_supported=version && version[0]>='3' && version[0]<='9' &&
            version[1]=='.' && Resolve(state.get_query,"glGetQueryiv");
    }
    GLint compiling{}; glGetIntegerv(GL_LIST_INDEX,&compiling);
    if (!state.list || compiling) return false;
    state.active=true; state.ready=false; state.final_pass=false; state.scene=nullptr;
    state.visibility_used=false;
    state.framebuffer_read=false; state.mirror=nullptr;
    ConfigureVisibility(true);
    return true;
}

void SetSceneReplayFinalPass(SceneReplayMirror mirror) noexcept {
    state.final_pass=true;
    state.mirror=mirror;
    if (state.active && !state.compiling && state.visibility_used) ConfigureVisibility(false);
}
void NotifySceneReplayFramebufferRead() noexcept {
    if (state.active && state.compiling) state.framebuffer_read=true;
}
bool BeginSceneReplayHiddenDraw() noexcept {
    if (!state.active || !state.compiling || state.final_pass) return false;
    state.visibility_used=true; glCallList(state.list+1); return true;
}
void EndSceneReplayHiddenDraw() noexcept { glCallList(state.list+2); }

void FinishSceneReplayFrame() noexcept {
    if (!state.active) return;
    state.mirror=nullptr; // Partial-frame cleanup must retain normal replay.
    if (state.compiling) { glEndList(); state.compiling=false; }
    if (state.visibility_used) ConfigureVisibility(false);
    // Normally the monitor replay has already consumed the saved state. On a
    // partial frame, replay once to balance the stack and retain engine caches.
    if (state.attrib_saved) {
        if (state.ready) Replay(false);
        else { glPopAttrib(); state.attrib_saved=false; }
    }
    ++state.frames;
    const auto now=GetTickCount();
    if (!state.next_log || static_cast<LONG>(now-state.next_log)>=0) {
        char message[240]{};
        std::snprintf(message,sizeof(message),"native-scene-replay frames=%llu gl_replays=%llu ready=%u vertex_env=%d fragment_env=%d monitor_mirrors=%llu",
            static_cast<unsigned long long>(state.frames),static_cast<unsigned long long>(state.replays),
            state.ready ? 1U:0U,state.vertex.count,state.fragment.count,static_cast<unsigned long long>(state.mirrored));
        (void)AppendPersistentProbeLogLine(message); state.next_log=now+5000;
    }
    state.active=false; state.ready=false;
}

static std::uint64_t RenderSceneWithReplayUntimed(void* scene,SceneRenderMethod render) noexcept {
    if (!state.active || state.compiling) return render(scene);
    if (state.ready) {
        if (scene==state.scene && state.attrib_saved) {
            Replay(!state.final_pass);
            return state.result;
        }
        return render(scene);
    }
    if (state.final_pass) return render(scene);
    ReadProgram(kVertexProgram,state.vertex); ReadProgram(kFragmentProgram,state.fragment);
    glGetIntegerv(0x8CAA,&state.capture_read); glGetIntegerv(0x8CA6,&state.capture_draw);
    glPushAttrib(GL_ALL_ATTRIB_BITS); state.attrib_saved=true;
    glNewList(state.list,GL_COMPILE_AND_EXECUTE); state.compiling=true;
    __try {
        // ARB program binding/environment are absent from PushAttrib. Include
        // their initial values in the list; subsequent engine writes are also
        // recorded. Client arrays are copied by DrawElements at compilation.
        RestoreProgram(kVertexProgram,state.vertex); RestoreProgram(kFragmentProgram,state.fragment);
        state.result=render(scene);
        state.scene=scene; state.ready=true;
    } __finally {
        glEndList(); state.compiling=false;
    }
    return state.result;
}

std::uint64_t RenderSceneWithReplay(void* scene,SceneRenderMethod render) noexcept {
    // Bracket outside glNewList/glEndList. Timing commands must never become
    // part of the captured display list or be repeated by glCallList.
    // Recursive engine calls during compilation belong to the outer sample.
    if (state.compiling) return RenderSceneWithReplayUntimed(scene,render);
    auto pass=gpu_timing::Pass::BaseScene;
    if (state.active) {
        if (state.ready && scene==state.scene && state.attrib_saved)
            pass=state.final_pass ? gpu_timing::Pass::MonitorReplay:gpu_timing::Pass::StereoReplay;
        else if (!state.ready && !state.final_pass) pass=gpu_timing::Pass::StereoCapture;
    }
    const unsigned timing=gpu_timing::Begin(pass);
    std::uint64_t result{};
    __try { result=RenderSceneWithReplayUntimed(scene,render); }
    __finally { gpu_timing::End(timing,AbnormalTermination()!=FALSE); }
    return result;
}
}
