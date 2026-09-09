#include "native_stereo.hpp"
#include "game_image_capture.hpp"
#include "gl_context_recovery.hpp"
#include "depth_pack.hpp"
#include "render_trace.hpp"
#include "scene_replay.hpp"
#include "stereo_request_gate.hpp"
#include "gl_ext_d3d12_bridge.hpp"
#include "nv_dx_interop_bridge.hpp"
#include "probe.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#include <gl/GL.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <new>
#include <limits>
#include <string>
#include <vector>

namespace k2vr::game32 {
namespace {
constexpr GLenum read_fbo = 0x8CA8, draw_fbo = 0x8CA9;
constexpr GLenum read_binding = 0x8CAA, draw_binding = 0x8CA6;
constexpr GLenum color_attachment = 0x8CE0, depth_stencil_attachment = 0x821A;
constexpr GLenum framebuffer_complete = 0x8CD5, renderbuffer = 0x8D41;
using Gen = void(APIENTRY*)(GLsizei, GLuint*);
using Bind = void(APIENTRY*)(GLenum, GLuint);
using Attach = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using Check = GLenum(APIENTRY*)(GLenum);
using Storage = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
using AttachDepth = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using Blit = void(APIENTRY*)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
using Perspective = void(APIENTRY*)(GLdouble,GLdouble,GLdouble,GLdouble);
using SwapInterval = BOOL(WINAPI*)(int);
using GetSwapInterval = int(WINAPI*)();
using SelectBuffer = void(APIENTRY*)(GLenum);
using Viewport = void(APIENTRY*)(GLint,GLint,GLsizei,GLsizei);
using DrawElements = void(APIENTRY*)(GLenum,GLsizei,GLenum,const void*);
using DrawArrays = void(APIENTRY*)(GLenum,GLint,GLsizei);
using BeginPrimitive = void(APIENTRY*)(GLenum);
using EndPrimitive = void(APIENTRY*)();
using GetProgram = void(APIENTRY*)(GLenum,GLenum,GLint*);
using GetProgramString = void(APIENTRY*)(GLenum,GLenum,void*);
using GetProgramEnv = void(APIENTRY*)(GLenum,GLuint,GLfloat*);
using BlendFunc = void(APIENTRY*)(GLenum,GLenum);
using BlendSeparate = void(APIENTRY*)(GLenum,GLenum,GLenum,GLenum);
using ActiveTexture = void(APIENTRY*)(GLenum);
using UseProgram = void(APIENTRY*)(GLuint);
using ColorMask = void(APIENTRY*)(GLboolean,GLboolean,GLboolean,GLboolean);
using Clear = void(APIENTRY*)(GLbitfield);
using CopyImage = void(APIENTRY*)(GLenum,GLint,GLenum,GLint,GLint,GLsizei,GLsizei,GLint);
using CopySubImage = void(APIENTRY*)(GLenum,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei);
using ReadPixels = void(APIENTRY*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
struct StereoState {
    HGLRC owner_context{};
    DWORD owner_thread{};
    bool context_deleted{},reset_history{};
    GlContextRecoveryRetry context_recovery;
    bool recovery_functions{};
    GlExtD3D12Bridge bridge;
    DepthPack depth_pack;
    ipc::StereoFrameMapping metadata;
    Gen gen_fbos{}, gen_renderbuffers{};
    Bind bind_fbo{}, bind_renderbuffer{};
    Attach attach{};
    Check check{};
    Storage storage{};
    AttachDepth attach_depth{};
    Blit blit{};
    GLuint eye_fbo{}, eye_color{}, eye_depth{}, atlas_fbo{};
    GLuint hud_fbo{},hud_color{},hud_depth{};
    GLuint mirror_fbo{},mirror_color{};
    GLint mirror_width{},mirror_height{};
    bool mirror_guard_ready{};
    GLint hud_width{},hud_height{};
    BlendSeparate blend_separate{};
    ActiveTexture active_texture{};
    UseProgram use_program{};
    bool hud_active{},awaiting_present{};
    GLboolean hud_engine_alpha_mask{GL_TRUE};
    unsigned hud_draws{};
    unsigned hud_clears{};
    bool hud_gamma_alpha_saved{};
    GLboolean hud_gamma_mask[4]{};
    std::uint64_t hud_frames{};
    GLint previous_read{}, previous_draw{};
    GLint previous_read_buffer{},previous_draw_buffer{},previous_viewport[4]{};
    unsigned matrices_logged{};
    std::uint32_t width{}, height{};
    std::uint64_t last_ready{};
    std::array<std::uint64_t, ipc::kGpuStreamSlotCount> slot_ready{};
    ipc::StereoFrameMetadata pending{};
    float depth_units_per_metre{};
    double projection_near[2]{},projection_far[2]{};
    std::size_t slot{};
    unsigned captured_mask{};
    unsigned view_mask{};
    bool pair_active{}, failed{};
    std::uint64_t attempted{},skipped_busy{},incomplete{};
    StereoRequestGate request_gate;
    StereoCaptureKey pending_key{};
    bool request_gate_enabled{};
    bool request_cadence{};
    std::uint64_t eligible_requests{},repeated_requests{},repeated_views{},skipped_requests{};
    DWORD next_log{};
    unsigned capture_failure{};
    SwapInterval swap_interval{};
    int original_swap_interval{-1};
    bool vsync_disabled{};
    std::uint64_t redirected_binds{};
    GLint rejected_draw_fbo{};
};
StereoState* state{}; // retained until process exit; never destroy GL state on an arbitrary thread
CopyImage engine_copy_image{};
CopySubImage engine_copy_sub_image{};
ReadPixels engine_read_pixels{};
void APIENTRY MirrorCopyImage(GLenum target,GLint level,GLenum format,GLint x,GLint y,GLsizei w,GLsizei h,GLint border) {
    NotifySceneReplayFramebufferRead(); engine_copy_image(target,level,format,x,y,w,h,border);
}
void APIENTRY MirrorCopySubImage(GLenum target,GLint level,GLint ox,GLint oy,GLint x,GLint y,GLsizei w,GLsizei h) {
    NotifySceneReplayFramebufferRead(); engine_copy_sub_image(target,level,ox,oy,x,y,w,h);
}
void APIENTRY MirrorReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void* pixels) {
    NotifySceneReplayFramebufferRead(); engine_read_pixels(x,y,w,h,format,type,pixels);
}
Perspective original_perspective{};
thread_local int active_eye = -1;
thread_local bool projection_seen = false;
Bind engine_bind_fbo{};
SelectBuffer engine_draw_buffer{},engine_read_buffer{};
Viewport engine_viewport{};
DrawElements engine_draw_elements{};
DrawArrays engine_draw_arrays{};
BeginPrimitive engine_begin{};
EndPrimitive engine_end{};
BlendFunc engine_blend_func{};
ColorMask engine_color_mask{};
Clear engine_clear{};
GetProgram get_arb_program{};
GetProgramString get_arb_source{};
GetProgramEnv get_arb_env{};
std::FILE* draw_trace{};
std::wstring draw_trace_directory;
unsigned draw_trace_counts[2]{};
std::array<GLuint,32> traced_programs{};
unsigned traced_program_count{};
bool ego_visibility_enabled{},ego_render_hook_ready{};
const void* ego_target_object{};
EgoMeshSelection ego_mesh_selection{};
EngineCameraPoseWxyz ego_eye{};
thread_local const void* ego_drawing_object{};
thread_local bool ego_drawing_head_attachment{};
thread_local bool ego_primitive_hidden{};
unsigned ego_hidden_draws{};
const void* ego_logged_head_target{};
unsigned ego_logged_head_mask=~0U;
std::array<const void*,16> ego_logged_head_attachments{};
unsigned ego_logged_head_attachment_count{};
using GobRender=std::uint64_t(__thiscall*)(void*,bool);
GobRender original_gob_render{};

void RefreshEgoHeadPivotsSeh(void* self) noexcept {
    ego_mesh_selection.head_pivot_mask=0;
    __try {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* vtable=*reinterpret_cast<const std::uintptr_t* const*>(self);
        const auto address=vtable[0x98/4];
        // Same exact getter validated by the first-person camera reader. These
        // optional reads are isolated: a missing/faulty head cannot change eye height.
        if (address!=base+0x00062B90U) return;
        using GetHook=int(__thiscall*)(void*,const char*,math::Vec3*,void*);
        const auto get_hook=reinterpret_cast<GetHook>(address);
        const char* names[]={"Head","Eyes"};
        const auto nan=std::numeric_limits<float>::quiet_NaN();
        for (unsigned i=0;i<2;++i) {
            math::Vec3 pivot{nan,nan,nan};
            if (get_hook(self,names[i],&pivot,nullptr) && ValidFirstPersonEye(ego_mesh_selection.feet,pivot)) {
                ego_mesh_selection.head_pivots[i]=pivot;
                ego_mesh_selection.head_pivot_mask|=1U<<i;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ego_mesh_selection.head_pivot_mask=0;
    }
}

bool IsEgoHeadAttachmentSeh(void* self) noexcept {
    if (!ego_target_object || self==ego_target_object) return false;
    __try {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::array<EgoHeadAttachmentLink,kMaxEgoHeadAttachmentDepth> chain{};
        const void* cursor=self;
        for (std::size_t i=0;i<chain.size();++i) {
            // Gob::Attach (0x461670, vtable +0x50) stores the resulting
            // behavior through +0x58 (0x461600) at Gob+0x1B4.
            const auto* behavior=*reinterpret_cast<unsigned char* const*>(
                static_cast<const unsigned char*>(cursor)+0x1B4);
            if (!behavior || *reinterpret_cast<const std::uintptr_t*>(behavior)!=
                base+0x009918D8U-0x00400000U) return false; // exact CAurBehaviorAttach
            // Constructor 0x504810 / base 0x504170: child +0x10,
            // parent reference +0x14, resolved attachment node +0x18.
            auto& link=chain[i];
            link.child=*reinterpret_cast<void* const*>(behavior+0x10);
            link.parent=*reinterpret_cast<void* const*>(behavior+0x14);
            link.node=*reinterpret_cast<void* const*>(behavior+0x18);
            link.supported=true;
            if (link.child!=cursor || !link.parent || !link.node || link.parent==cursor) return false;
            if (link.parent==ego_target_object) {
                const auto* table=*reinterpret_cast<const std::uintptr_t* const*>(ego_target_object);
                const auto address=table[0x10C/4];
                if (address!=base+0x000587F0U) return false;
                // This is the same node lookup used by Attach(HeadHook), not
                // a world-position comparison. Do not cache across model changes.
                using FindNode=void*(__thiscall*)(const void*,const char*);
                const auto head_hook=reinterpret_cast<FindNode>(address)(ego_target_object,"HeadHook");
                return ControlledHeadAttachment(ego_target_object,head_hook,self,
                    std::span<const EgoHeadAttachmentLink>(chain.data(),i+1));
            }
            cursor=link.parent;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Unsupported/faulty attachments stay visible, without altering cameras.
    }
    return false;
}

std::uint64_t __fastcall GobRenderWrapper(void* self,void*,bool cull) noexcept {
    // Track exact identity for synchronous mesh draws, including recursive
    // model nodes. A nested render of another Gob gets its own identity.
    const auto* previous=ego_drawing_object;
    const bool previous_head=ego_drawing_head_attachment;
    ego_drawing_object=self; ego_drawing_head_attachment=false;
    if (ego_mesh_selection.short_model &&
        ShouldHideFirstPersonObjectDraw(ego_visibility_enabled && ego_render_hook_ready,
            active_eye,ego_target_object,self)) {
        // Read after game/animation updates, immediately before this object's
        // synchronous mesh traversal, rather than using older camera samples.
        RefreshEgoHeadPivotsSeh(self);
        if (ego_logged_head_target!=self || ego_logged_head_mask!=ego_mesh_selection.head_pivot_mask) {
            char line[160]{};
            std::snprintf(line,sizeof(line),"ego-visibility-selection target=%p mode=short-head head_pivot_mask=0x%X",
                self,ego_mesh_selection.head_pivot_mask);
            (void)AppendPersistentProbeLogLine(line);
            ego_logged_head_target=self; ego_logged_head_mask=ego_mesh_selection.head_pivot_mask;
        }
    }
    if (!ego_mesh_selection.short_model && ego_visibility_enabled && ego_render_hook_ready && active_eye==0) {
        ego_drawing_head_attachment=IsEgoHeadAttachmentSeh(self);
        if (ego_drawing_head_attachment && ego_logged_head_attachment_count<ego_logged_head_attachments.size() &&
            std::find(ego_logged_head_attachments.begin(),
                ego_logged_head_attachments.begin()+ego_logged_head_attachment_count,self)==
                ego_logged_head_attachments.begin()+ego_logged_head_attachment_count) {
            ego_logged_head_attachments[ego_logged_head_attachment_count++]=self;
            char line[160]{};
            std::snprintf(line,sizeof(line),"ego-visibility-head-attachment target=%p head=%p verified=HeadHook",
                ego_target_object,self);
            (void)AppendPersistentProbeLogLine(line);
        }
    }
    std::uint64_t result{};
    __try { result=original_gob_render(self,cull); }
    __finally { ego_drawing_object=previous; ego_drawing_head_attachment=previous_head; }
    return result;
}
using LetterboxDraw=std::uint64_t(__thiscall*)(void*,float);
LetterboxDraw original_letterbox_draw{};
thread_local bool letterbox_drawing{},letterbox_primitive_hidden{};

std::uint64_t __fastcall LetterboxDrawWrapper(void* self,void*,float delta) noexcept {
    const bool previous=letterbox_drawing;
    letterbox_drawing=true;
    std::uint64_t result{};
    __try { result=original_letterbox_draw(self,delta); }
    __finally { letterbox_drawing=previous; }
    return result;
}
bool HideLetterboxDraw() noexcept {
    if (!letterbox_drawing || !state) return false;
    // A dialogue may begin on a frame whose eye capture was skipped. Observe
    // its actual draw even without HUD capture, forcing the next pair fresh.
    state->request_gate.Reset();
    if (!state->hud_active) return false;
    // The backdrop's actual draw marks this committed HUD as dialogue UI.
    // Carry the classification with the image, including delayed neural frames.
    state->pending.request.presentation_state=ipc::PresentationState::DialogueStereo;
    // The exact CSWGuiDialogLetterbox object contains the backdrop. Keep its
    // fade/layout updates, but omit its rasterization from the transparent UI.
    glPushAttrib(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
    glDepthMask(GL_FALSE); glStencilMask(0);
    return true;
}

bool BeginEgoDraw(GLsizei count) noexcept {
    if (!ego_visibility_enabled || !ego_render_hook_ready || active_eye!=0) return false;
    if (ego_mesh_selection.short_model) {
        // Preserve the headset-confirmed short-model Head/Eyes pivot path.
        if (!ShouldHideFirstPersonObjectDraw(true,active_eye,ego_target_object,ego_drawing_object)) return false;
        GLfloat mv[16]{}; glGetFloatv(GL_MODELVIEW_MATRIX,mv);
        const auto world=math::Rotate(EngineCameraQuaternion(ego_eye),{mv[12],mv[13],mv[14]})+
            math::Vec3{ego_eye.position_x,ego_eye.position_y,ego_eye.position_z};
        if (!NamedEgoHeadPivot(ego_mesh_selection,world)) return false;
    } else if (!ego_drawing_head_attachment) {
        // Human body skins are not heads. Keep the controlled body Gob and
        // every unrelated/hand-bound object, even when their pivots are nearby.
        return false;
    }
    GLboolean mask[4]{}; glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    if (!mask[0] && !mask[1] && !mask[2]) return false; // Preserve stencil-only shadows.
    const bool hidden=BeginSceneReplayHiddenDraw();
    if (hidden && ego_hidden_draws++<8) {
        char line[200]{};
        std::snprintf(line,sizeof(line),"ego-visibility target=%p object=%p candidate=%s count=%d draw=%u",
            ego_target_object,ego_drawing_object,ego_mesh_selection.short_model ? "named-head":
                "head-attachment",count,ego_hidden_draws);
        (void)AppendPersistentProbeLogLine(line);
    }
    return hidden;
}

void TraceNativeDraw(GLenum mode,GLsizei count,GLenum type) noexcept {
    if (state && state->hud_active) {
        if (state->hud_frames<2 && state->hud_draws<128) {
            unsigned char center[4]{}; GLint texture{},rectangle{},source{},destination{},fbo{};
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture); glGetIntegerv(0x84F6,&rectangle);
            glGetIntegerv(GL_BLEND_SRC,&source); glGetIntegerv(GL_BLEND_DST,&destination);
            glGetIntegerv(draw_binding,&fbo);
            glReadPixels(state->hud_width/2,state->hud_height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,center);
            char line[220]{};
            std::snprintf(line,sizeof(line),"hud-draw-before frame=%llu draw=%u center=%u,%u,%u,%u blend=%u factors=%x,%x tex=%d rect=%d fbo=%d mode=%u count=%d",
                static_cast<unsigned long long>(state->hud_frames),state->hud_draws,center[0],center[1],center[2],center[3],
                glIsEnabled(GL_BLEND),source,destination,texture,rectangle,fbo,mode,count);
            (void)AppendPersistentProbeLogLine(line);
        }
        ++state->hud_draws; return;
    }
    if (!state || active_eye<0 || state->attempted!=1 || draw_trace_counts[active_eye]>=1024) return;
    try {
        if (!draw_trace) {
            HMODULE module{}; wchar_t path[MAX_PATH]{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&TraceNativeDraw),&module) || !GetModuleFileNameW(module,path,MAX_PATH)) return;
            draw_trace_directory=path;
            draw_trace_directory.resize(draw_trace_directory.find_last_of(L'\\'));
            draw_trace_directory+=L"\\logs\\";
            if (_wfopen_s(&draw_trace,(draw_trace_directory+L"native-draw-trace.csv").c_str(),L"w") || !draw_trace) return;
            std::fputs("eye,draw,mode,count,type,fbo,viewport_x,viewport_y,viewport_w,viewport_h,arb_vertex,glsl_program,texture",draw_trace);
            for (unsigned i=0;i<16;++i) std::fprintf(draw_trace,",p%u",i);
            for (unsigned i=0;i<16;++i) std::fprintf(draw_trace,",v%u",i);
            std::fputs(",animation_step,bones_hash\n",draw_trace);
            get_arb_program=reinterpret_cast<GetProgram>(wglGetProcAddress("glGetProgramivARB"));
            get_arb_source=reinterpret_cast<GetProgramString>(wglGetProcAddress("glGetProgramStringARB"));
            get_arb_env=reinterpret_cast<GetProgramEnv>(wglGetProcAddress("glGetProgramEnvParameterfvARB"));
        }
        GLint fbo{},viewport[4]{},program{},glsl{},texture{};
        GLfloat p[16]{},v[16]{};
        glGetIntegerv(draw_binding,&fbo); glGetIntegerv(GL_VIEWPORT,viewport);
        glGetIntegerv(0x8B8D,&glsl); glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        glGetFloatv(GL_PROJECTION_MATRIX,p); glGetFloatv(GL_MODELVIEW_MATRIX,v);
        if (glIsEnabled(0x8620) && IsUsableWglProcAddressValue(reinterpret_cast<std::uintptr_t>(get_arb_program)))
            get_arb_program(0x8620,0x8677,&program);
        std::fprintf(draw_trace,"%d,%u,%u,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d",active_eye,
            draw_trace_counts[active_eye]++,mode,count,type,fbo,viewport[0],viewport[1],viewport[2],viewport[3],program,glsl,texture);
        for (float value:p) std::fprintf(draw_trace,",%.8g",value);
        for (float value:v) std::fprintf(draw_trace,",%.8g",value);
        std::uint64_t bones_hash=14695981039346656037ULL;
        if (program>0 && IsUsableWglProcAddressValue(reinterpret_cast<std::uintptr_t>(get_arb_env))) {
            // Actual skinned shaders use program.env[18..68]. Record only
            // once per draw in the bounded first-pair diagnostic.
            for (GLuint index=18;index<=68;++index) {
                GLfloat bone[4]{}; get_arb_env(0x8620,index,bone);
                for (unsigned char byte:std::bit_cast<std::array<unsigned char,sizeof(bone)>>(bone)) {
                    bones_hash^=byte; bones_hash*=1099511628211ULL;
                }
            }
        }
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto animation_step=*reinterpret_cast<const float*>(base+0x009F62B0U-0x00400000U);
        std::fprintf(draw_trace,",%.8g,%016llx\n",animation_step,static_cast<unsigned long long>(bones_hash));
        if (program>0 && traced_program_count<traced_programs.size() &&
            std::find(traced_programs.begin(),traced_programs.begin()+traced_program_count,static_cast<GLuint>(program))==traced_programs.begin()+traced_program_count &&
            IsUsableWglProcAddressValue(reinterpret_cast<std::uintptr_t>(get_arb_source))) {
            traced_programs[traced_program_count++]=static_cast<GLuint>(program);
            GLint length{}; get_arb_program(0x8620,0x8627,&length);
            if (length>0 && length<262144) {
                std::vector<char> source(static_cast<std::size_t>(length)+1);
                get_arb_source(0x8620,0x8628,source.data());
                std::FILE* shader{};
                if (_wfopen_s(&shader,(draw_trace_directory+L"native-vertex-"+std::to_wstring(program)+L".arb").c_str(),L"wb")==0 && shader) {
                    std::fwrite(source.data(),1,static_cast<std::size_t>(length),shader); std::fclose(shader);
                }
            }
        }
    } catch (...) { /* Optional diagnostics cannot abort the game's draw. */ }
}
void APIENTRY StereoDrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices) {
    TraceNativeDraw(mode,count,type); const bool hidden=BeginEgoDraw(count);
    const bool backdrop=HideLetterboxDraw();
    engine_draw_elements(mode,count,type,indices);
    if (hidden) EndSceneReplayHiddenDraw();
    if (backdrop) glPopAttrib();
}
void APIENTRY StereoDrawArrays(GLenum mode,GLint first,GLsizei count) {
    TraceNativeDraw(mode,count,0); const bool hidden=BeginEgoDraw(count);
    const bool backdrop=HideLetterboxDraw();
    engine_draw_arrays(mode,first,count);
    if (hidden) EndSceneReplayHiddenDraw();
    if (backdrop) glPopAttrib();
}
void APIENTRY StereoBegin(GLenum mode) {
    TraceNativeDraw(mode,0,0);
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (state && state->hud_active &&
        reinterpret_cast<std::uintptr_t>(_ReturnAddress())==base+0x00436DEEU-0x00400000U) {
        // Verified final gamma pass, shader at 0x989DB0:
        // MOV result.color.rgb, r0; MOV result.color.a, 1.0;
        // Gamma may adjust UI RGB, but must retain its existing coverage alpha.
        glGetBooleanv(GL_COLOR_WRITEMASK,state->hud_gamma_mask);
        glColorMask(state->hud_gamma_mask[0],state->hud_gamma_mask[1],state->hud_gamma_mask[2],GL_FALSE);
        state->hud_gamma_alpha_saved=true;
    }
    ego_primitive_hidden=BeginEgoDraw(0);
    letterbox_primitive_hidden=HideLetterboxDraw();
    engine_begin(mode);
}
void APIENTRY StereoEnd() {
    engine_end();
    if (letterbox_primitive_hidden) { glPopAttrib(); letterbox_primitive_hidden=false; }
    if (ego_primitive_hidden) { EndSceneReplayHiddenDraw(); ego_primitive_hidden=false; }
    if (state && state->hud_gamma_alpha_saved) {
        glColorMask(state->hud_gamma_mask[0],state->hud_gamma_mask[1],state->hud_gamma_mask[2],state->hud_gamma_mask[3]);
        state->hud_gamma_alpha_saved=false;
        if (state->hud_frames==0) (void)AppendPersistentProbeLogLine("native-hud-gamma alpha=preserved shader=0x989DB0 caller=0x436DEE");
    }
}

void APIENTRY StereoBindFramebuffer(GLenum target,GLuint framebuffer) {
    // A captured offscreen pass is not a pure monitor scene. Its resource
    // effects must retain ordinary replay, just like framebuffer copies.
    if (framebuffer!=0) NotifySceneReplayFramebufferRead();
    if (active_eye>=0 && state && state->pair_active && framebuffer==0) {
        framebuffer=state->eye_fbo; ++state->redirected_binds;
    } else if (state && state->hud_active && framebuffer==0) {
        framebuffer=state->hud_fbo;
    }
    engine_bind_fbo(target,framebuffer);
}
GLenum EyeBuffer(GLenum mode,GLenum binding) noexcept {
    if (state && (active_eye>=0 || state->hud_active) && mode>=GL_FRONT_LEFT && mode<=GL_FRONT_AND_BACK) {
        GLint bound{}; glGetIntegerv(binding,&bound);
        if (bound==static_cast<GLint>(active_eye>=0 ? state->eye_fbo:state->hud_fbo)) return color_attachment;
    }
    return mode;
}
void APIENTRY StereoDrawBuffer(GLenum mode) { engine_draw_buffer(EyeBuffer(mode,draw_binding)); }
void APIENTRY StereoReadBuffer(GLenum mode) { engine_read_buffer(EyeBuffer(mode,read_binding)); }
void APIENTRY StereoBlendFunc(GLenum source,GLenum destination) {
    if (state && state->hud_active)
        state->blend_separate(source,destination,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    else engine_blend_func(source,destination);
}
void APIENTRY StereoColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a) {
    if (state && state->hud_active) {
        state->hud_engine_alpha_mask=a;
        a=static_cast<GLboolean>(r || g || b);
    }
    engine_color_mask(r,g,b,a);
}
void APIENTRY StereoClear(GLbitfield mask) {
    if (state && state->hud_active && (mask&GL_COLOR_BUFFER_BIT)) {
        // A UI pass may clear the game's opaque backbuffer after the camera.
        // Its isolated target instead needs transparent black, including alpha.
        GLfloat color[4]{}; glGetFloatv(GL_COLOR_CLEAR_VALUE,color);
        glClearColor(0,0,0,0); engine_clear(mask);
        glClearColor(color[0],color[1],color[2],color[3]);
        ++state->hud_clears;
    } else engine_clear(mask);
}
void APIENTRY StereoViewport(GLint x,GLint y,GLsizei width,GLsizei height) {
    if (active_eye>=0 && state) {
        GLint bound{}; glGetIntegerv(draw_binding,&bound);
        if (bound==static_cast<GLint>(state->eye_fbo)) {
            x=0; y=0; width=static_cast<GLsizei>(state->width); height=static_cast<GLsizei>(state->height);
        }
    }
    engine_viewport(x,y,width,height);
}

bool ReplaceGamePointer(std::uintptr_t preferred,void* expected,void* wrapper) noexcept {
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* cell=reinterpret_cast<void* volatile*>(base+preferred-0x00400000U);
    if (*cell==wrapper) return true;
    if (!expected || *cell!=expected) {
        char line[160]{};
        std::snprintf(line,sizeof(line),"native-stereo-hook-mismatch cell=%08lx expected=%p actual=%p",
            static_cast<unsigned long>(preferred),expected,*cell);
        (void)AppendPersistentProbeLogLine(line); return false;
    }
    DWORD old{},ignored{};
    if (!VirtualProtect(const_cast<void**>(cell),sizeof(void*),PAGE_READWRITE,&old)) return false;
    const bool replaced=InterlockedCompareExchangePointer(cell,wrapper,expected)==expected;
    const bool restored=VirtualProtect(const_cast<void**>(cell),sizeof(void*),old,&ignored)!=FALSE;
    return replaced && restored;
}
bool InstallFramebufferRouting() noexcept {
    engine_bind_fbo=state->bind_fbo;
    const auto gl=GetModuleHandleW(L"opengl32.dll");
    if (NativeStereoMonitorMirrorEnabled()) {
        engine_copy_image=reinterpret_cast<CopyImage>(GetProcAddress(gl,"glCopyTexImage2D"));
        engine_copy_sub_image=reinterpret_cast<CopySubImage>(GetProcAddress(gl,"glCopyTexSubImage2D"));
        engine_read_pixels=reinterpret_cast<ReadPixels>(GetProcAddress(gl,"glReadPixels"));
        // Exact executable IAT, verified offline. A failed guard installation
        // disables ONLY this optimization; successful partial hooks are benign.
        state->mirror_guard_ready=engine_copy_image && engine_copy_sub_image && engine_read_pixels &&
            ReplaceGamePointer(0x00986358U,reinterpret_cast<void*>(engine_copy_image),reinterpret_cast<void*>(&MirrorCopyImage)) &&
            ReplaceGamePointer(0x00986354U,reinterpret_cast<void*>(engine_copy_sub_image),reinterpret_cast<void*>(&MirrorCopySubImage)) &&
            ReplaceGamePointer(0x009862C8U,reinterpret_cast<void*>(engine_read_pixels),reinterpret_cast<void*>(&MirrorReadPixels));
    }
    engine_draw_buffer=reinterpret_cast<SelectBuffer>(GetProcAddress(gl,"glDrawBuffer"));
    engine_read_buffer=reinterpret_cast<SelectBuffer>(GetProcAddress(gl,"glReadBuffer"));
    engine_viewport=reinterpret_cast<Viewport>(GetProcAddress(gl,"glViewport"));
    engine_draw_elements=reinterpret_cast<DrawElements>(GetProcAddress(gl,"glDrawElements"));
    engine_draw_arrays=reinterpret_cast<DrawArrays>(GetProcAddress(gl,"glDrawArrays"));
    engine_begin=reinterpret_cast<BeginPrimitive>(GetProcAddress(gl,"glBegin"));
    engine_end=reinterpret_cast<EndPrimitive>(GetProcAddress(gl,"glEnd"));
    engine_blend_func=reinterpret_cast<BlendFunc>(GetProcAddress(gl,"glBlendFunc"));
    engine_color_mask=reinterpret_cast<ColorMask>(GetProcAddress(gl,"glColorMask"));
    engine_clear=reinterpret_cast<Clear>(GetProcAddress(gl,"glClear"));
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Supported Gob vtable 0x98B5CC, Render(bool) slot +0x10. The scene's
    // DoGobBuckets invokes it at 0x468208. Render -> 0x4B3D80 -> 0x4B11A0
    // traverses model nodes and issues their mesh draws synchronously (RET 4).
    original_gob_render=reinterpret_cast<GobRender>(base+0x004B3FE0U-0x00400000U);
    const unsigned char render_prefix[]={0x55,0x8B,0xEC,0x6A,0xFF};
    const unsigned char render_return[]={0xC2,0x04,0x00};
    ego_render_hook_ready=std::memcmp(reinterpret_cast<const void*>(original_gob_render),
        render_prefix,sizeof(render_prefix))==0 &&
        std::memcmp(reinterpret_cast<const void*>(base+0x004B4666U-0x00400000U),
            render_return,sizeof(render_return))==0 &&
        ReplaceGamePointer(0x0098B5DCU,reinterpret_cast<void*>(original_gob_render),
            reinterpret_cast<void*>(&GobRenderWrapper));
    if (!ego_render_hook_ready)
        (void)AppendPersistentProbeLogLine("ego-visibility disabled: unsupported Gob render hook");
    original_letterbox_draw=reinterpret_cast<LetterboxDraw>(base+0x008BB260U-0x00400000U);
    // Exact-build RTTI: CSWGuiDialogLetterbox vtable 0x9A7CBC, Draw(float)
    // slot +0x34. Both function prologue and RET 4 checked offline.
    if (!ReplaceGamePointer(0x009A7CF0U,reinterpret_cast<void*>(original_letterbox_draw),
        reinterpret_cast<void*>(&LetterboxDrawWrapper))) return false;
    // 0x4329F3 loads literal "glBindFramebuffer"; 0x4329FE stores the
    // wglGetProcAddress result at 0xA32BD4. Other cells are exact PE imports.
    return ReplaceGamePointer(0x00A32BD4U,reinterpret_cast<void*>(engine_bind_fbo),reinterpret_cast<void*>(&StereoBindFramebuffer)) &&
        ReplaceGamePointer(0x00986370U,reinterpret_cast<void*>(engine_draw_buffer),reinterpret_cast<void*>(&StereoDrawBuffer)) &&
        ReplaceGamePointer(0x00986390U,reinterpret_cast<void*>(engine_read_buffer),reinterpret_cast<void*>(&StereoReadBuffer)) &&
        ReplaceGamePointer(0x0098636CU,reinterpret_cast<void*>(engine_viewport),reinterpret_cast<void*>(&StereoViewport)) &&
        ReplaceGamePointer(0x00986320U,reinterpret_cast<void*>(engine_draw_elements),reinterpret_cast<void*>(&StereoDrawElements)) &&
        ReplaceGamePointer(0x00986304U,reinterpret_cast<void*>(engine_draw_arrays),reinterpret_cast<void*>(&StereoDrawArrays)) &&
        ReplaceGamePointer(0x009862E4U,reinterpret_cast<void*>(engine_begin),reinterpret_cast<void*>(&StereoBegin)) &&
        ReplaceGamePointer(0x009862D4U,reinterpret_cast<void*>(engine_end),reinterpret_cast<void*>(&StereoEnd)) &&
        ReplaceGamePointer(0x0098626CU,reinterpret_cast<void*>(engine_blend_func),reinterpret_cast<void*>(&StereoBlendFunc)) &&
        ReplaceGamePointer(0x009863A4U,reinterpret_cast<void*>(engine_color_mask),reinterpret_cast<void*>(&StereoColorMask)) &&
        ReplaceGamePointer(0x00986274U,reinterpret_cast<void*>(engine_clear),reinterpret_cast<void*>(&StereoClear));
}

void Log(const char* stage) noexcept {
    (void)AppendPersistentProbeLogLine(stage);
}
void APIENTRY StereoPerspective(GLdouble fov, GLdouble aspect, GLdouble near_z, GLdouble far_z) {
    if (active_eye < 0 || !state || !state->pair_active) {
        original_perspective(fov,aspect,near_z,far_z); return;
    }
    const auto eye=static_cast<std::size_t>(active_eye);
    // A later projection call invalidates earlier scene depth metadata. Keep
    // color rendering usable; publish absent depth if the mapping is ambiguous.
    state->pending.depth_mask &= ~(1U<<eye);
    state->pending.depth[eye]={};
    state->projection_near[eye]=state->projection_far[eye]=0;
    const auto& f = state->pending.request.views[eye].fov;
    if (!std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0.0 || far_z <= near_z) return;
    glFrustum(std::tan(f.angle_left)*near_z, std::tan(f.angle_right)*near_z,
              std::tan(f.angle_down)*near_z, std::tan(f.angle_up)*near_z, near_z, far_z);
    state->projection_near[eye]=near_z; state->projection_far[eye]=far_z;
    projection_seen = true;
}
bool InstallProjection() noexcept {
    if (original_perspective) return true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* cell = reinterpret_cast<void* volatile*>(base + 0x00986028U - 0x00400000U);
    HMODULE glu = GetModuleHandleW(L"glu32.dll");
    if (!glu) return false;
    const auto expected = reinterpret_cast<void*>(GetProcAddress(glu,"gluPerspective"));
    if (!expected || *cell != expected) return false;
    DWORD protection{}, ignored{};
    if (!VirtualProtect(const_cast<void**>(cell), sizeof(void*), PAGE_READWRITE, &protection)) return false;
    original_perspective = reinterpret_cast<Perspective>(expected);
    const bool swapped = InterlockedCompareExchangePointer(cell,
        reinterpret_cast<void*>(&StereoPerspective), expected) == expected;
    const bool restored = VirtualProtect(const_cast<void**>(cell),sizeof(void*),protection,&ignored) != FALSE;
    if (!swapped) original_perspective = nullptr;
    return swapped && restored;
}
template<class T> bool Load(T& target, const char* name) noexcept {
    const PROC proc = wglGetProcAddress(name);
    if (!IsUsableWglProcAddressValue(reinterpret_cast<std::uintptr_t>(proc))) return false;
    target = reinterpret_cast<T>(proc); return true;
}
bool LoadStereoGlFunctions() noexcept {
    auto& s = *state;
    if (!Load(s.gen_fbos,"glGenFramebuffers") || !Load(s.bind_fbo,"glBindFramebuffer") ||
        !Load(s.attach,"glFramebufferTexture2D") || !Load(s.check,"glCheckFramebufferStatus") ||
        !Load(s.gen_renderbuffers,"glGenRenderbuffers") || !Load(s.bind_renderbuffer,"glBindRenderbuffer") ||
        !Load(s.storage,"glRenderbufferStorage") || !Load(s.attach_depth,"glFramebufferRenderbuffer") ||
        !Load(s.blit,"glBlitFramebuffer") || !Load(s.blend_separate,"glBlendFuncSeparate") ||
        !Load(s.active_texture,"glActiveTexture") || !Load(s.use_program,"glUseProgram") || !InstallProjection() ||
        !s.depth_pack.Initialize()) return false;
    (void)Load(get_arb_program,"glGetProgramivARB");
    (void)Load(get_arb_source,"glGetProgramStringARB");
    (void)Load(get_arb_env,"glGetProgramEnvParameterfvARB");
    return true;
}
bool CreateStereoGlTargets() noexcept {
    auto& s=*state;
    GLint old_read{},old_draw{},old_rb{};
    glGetIntegerv(read_binding,&old_read); glGetIntegerv(draw_binding,&old_draw);
    glGetIntegerv(0x8CA7,&old_rb);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    for (unsigned i=0;i<16 && glGetError()!=GL_NO_ERROR;++i) {}
    // Reuse partial targets on a recovery retry in this same context.
    if (!s.eye_fbo) s.gen_fbos(1,&s.eye_fbo);
    if (!s.atlas_fbo) s.gen_fbos(1,&s.atlas_fbo);
    if (!s.eye_color) glGenTextures(1,&s.eye_color);
    glBindTexture(GL_TEXTURE_2D,s.eye_color);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,static_cast<GLsizei>(s.width),
                 static_cast<GLsizei>(s.height),0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    if (!s.eye_depth) glGenTextures(1,&s.eye_depth);
    glBindTexture(GL_TEXTURE_2D,s.eye_depth);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,0x884C,GL_NONE);
    glTexImage2D(GL_TEXTURE_2D,0,0x88F0,static_cast<GLsizei>(s.width),
        static_cast<GLsizei>(s.height),0,0x84F9,0x84FA,nullptr);
    s.bind_fbo(draw_fbo,s.eye_fbo);
    s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,s.eye_color,0);
    s.attach(draw_fbo,depth_stencil_attachment,GL_TEXTURE_2D,s.eye_depth,0);
    glDrawBuffer(color_attachment);
    const bool complete=s.eye_fbo && s.atlas_fbo && s.eye_color && s.eye_depth &&
        s.check(draw_fbo)==framebuffer_complete && glGetError()==GL_NO_ERROR;
    s.bind_fbo(read_fbo,static_cast<GLuint>(old_read)); s.bind_fbo(draw_fbo,static_cast<GLuint>(old_draw));
    s.bind_renderbuffer(renderbuffer,static_cast<GLuint>(old_rb)); glPopAttrib();
    if (!complete) { Log("native-stereo-failed stage=eye-fbo-completeness"); return false; }
    if (!InstallFramebufferRouting()) { Log("native-stereo-failed stage=framebuffer-routing"); return false; }
    s.owner_context=wglGetCurrentContext(); s.owner_thread=GetCurrentThreadId();
    return true;
}
bool Initialize(const ipc::RenderRequest& request,bool request_gate_enabled,bool request_cadence) noexcept {
    state = new(std::nothrow) StereoState;
    if (!state) return false;
    state->width = request.render_width; state->height = request.render_height;
    auto& s = *state;
    if (!LoadStereoGlFunctions()) return false;
    s.request_gate_enabled=request_gate_enabled;
    s.request_cadence=request_cadence;
    char gate_log[200]{};
    std::snprintf(gate_log,sizeof(gate_log),
        "native-stereo-request-gate enabled=%u cadence=%u max_skip_age_ms=%llu clock=GetTickCount64 telemetry=1",
        (s.request_gate_enabled || s.request_cadence) ? 1U:0U,s.request_cadence ? 1U:0U,
        static_cast<unsigned long long>(s.request_cadence ? StereoRequestGate::cadence_maximum_skip_age_ms:StereoRequestGate::maximum_skip_age_ms));
    Log(gate_log);
    GlExtD3D12Diagnostic diagnostic{};
    if (s.bridge.Initialize(diagnostic) != GlExtD3D12Status::Ok) return false;
    const auto nonce = ipc::StereoStreamNonce(request.header.session_nonce);
    std::array<ipc::GpuStreamObjectName,ipc::kGpuStreamSlotCount> names{};
    std::array<std::wstring_view,ipc::kGpuStreamSlotCount> views{};
    for (std::size_t i=0;i<names.size();++i) {
        names[i] = ipc::MakeGpuStreamColorObjectName(nonce,i); views[i] = names[i].c_str();
    }
    const auto ready = ipc::MakeGpuStreamObjectName(nonce,ipc::GpuStreamObjectKind::ReadyFence);
    const auto consumed = ipc::MakeGpuStreamObjectName(nonce,ipc::GpuStreamObjectKind::ConsumedFence);
    if (s.bridge.CreateNamedStreams({s.width*2U,ipc::StereoAtlasHeight(s.height),ipc::kGpuStreamDxgiFormatRgba8Unorm},
        {views,ready.c_str(),consumed.c_str()},diagnostic) != GlExtD3D12Status::Ok ||
        !s.metadata.Open(request.header.session_nonce,true)) return false;
    if (!CreateStereoGlTargets()) return false;
    Log("native-stereo-ready mode=same-tick atlas=left-right scale=0.75 projection=asymmetric routing=game-default-fbo");
    return true;
}
}
void NotifyNativeStereoContextDeleted(void* context) noexcept {
    if (!state || state->owner_context!=context || state->owner_thread!=GetCurrentThreadId()) return;
    state->context_deleted=true;
    state->context_recovery.Reset(); state->recovery_functions=false;
    state->pair_active=state->awaiting_present=state->hud_active=false;
    state->hud_gamma_alpha_saved=false;
    active_eye=-1; projection_seen=false;
    letterbox_drawing=letterbox_primitive_hidden=false;
    ego_drawing_object=nullptr; ego_target_object=nullptr;
    ego_mesh_selection={}; ego_drawing_head_attachment=false;
    ego_logged_head_attachment_count=0;
    ego_logged_head_target=nullptr; ego_logged_head_mask=~0U;
    ego_visibility_enabled=ego_primitive_hidden=false;
    get_arb_program=nullptr; get_arb_source=nullptr; get_arb_env=nullptr;
    // Forget dead-context names once, never on a retry in a living context.
    auto& s=*state;
    s.eye_fbo=s.eye_color=s.eye_depth=s.atlas_fbo=0;
    s.mirror_fbo=s.mirror_color=0; s.mirror_width=s.mirror_height=0;
    s.hud_fbo=s.hud_color=s.hud_depth=0; s.hud_width=s.hud_height=0;
    s.depth_pack={}; s.swap_interval=nullptr; s.original_swap_interval=-1;
    s.vsync_disabled=false;
    Log("native-stereo-context-deleted");
}
void RecoverNativeStereoContext() noexcept {
    if (!state || !state->context_deleted || state->owner_thread!=GetCurrentThreadId() ||
        !wglGetCurrentContext()) return;
    auto& s=*state;
    if (!s.context_recovery.BeginAttempt(GetTickCount64(),
        s.bridge.owning_gl_context_is_current())) return;
    // Preserve the D3D ring, fence sequences and metadata mapping opened by the
    // host. Only objects owned by the deleted OpenGL context are recreated.
    GlExtD3D12Diagnostic diagnostic{};
    if (!s.context_recovery.imported() &&
        s.bridge.RebindAfterContextReplacement(diagnostic)==GlExtD3D12Status::Ok) {
        s.context_recovery.MarkImported();
        // Observe deletion of this replacement even if later target setup fails.
        s.owner_context=wglGetCurrentContext();
    }
    if (s.context_recovery.imported() && !s.recovery_functions)
        s.recovery_functions=LoadStereoGlFunctions();
    if (!s.context_recovery.imported() || !s.recovery_functions || !CreateStereoGlTargets()) {
        char line[400]{}; std::snprintf(line,sizeof(line),
            "native-stereo-context-recovery-pending stage=%s retry_ms=1000 detail=%s",
            !s.context_recovery.imported() ? "reimport":!s.recovery_functions ? "functions":"targets",
            diagnostic.detail);
        Log(line); return;
    }
    s.context_deleted=false;
    s.failed=false; s.reset_history=true; s.request_gate.Reset();
    Log("native-stereo-context-recovered eyes=2 hud=lazy histories=reset");
}
bool NativeStereoContextRecoveryPending() noexcept {
    return state && state->context_deleted;
}
void RestoreNativeStereoPacing() noexcept {
    if (state && state->owner_thread==GetCurrentThreadId()) state->request_gate.Reset();
    if (state && !state->context_deleted && state->vsync_disabled && state->swap_interval && state->bridge.owning_gl_context_is_current()) {
        if (state->swap_interval(state->original_swap_interval)) state->vsync_disabled=false;
    }
}
void ConfigureNativeStereoEgoVisibility(const void* target,const EgoMeshSelection& selection,
    const EngineCameraPoseWxyz& eye,float units,bool enabled) noexcept {
    if (target!=ego_target_object || selection.short_model!=ego_mesh_selection.short_model) {
        ego_hidden_draws=0; ego_logged_head_target=nullptr; ego_logged_head_mask=~0U;
        ego_logged_head_attachment_count=0;
    }
    ego_target_object=target; ego_mesh_selection=selection; ego_eye=eye;
    ego_visibility_enabled=enabled && target && math::IsFinite(selection.feet) &&
        std::isfinite(units) && units>0;
}

bool BeginNativeStereoPair(const ipc::RenderRequest& request, std::uint64_t camera_frame,
    bool disable_monitor_vsync,const EngineCameraPoseWxyz* eyes,const void* camera,bool request_gate_enabled,
    float engine_units_per_metre,bool request_cadence) noexcept {
    if (!ipc::ValidStereoRequest(request) || !eyes || !camera) return false;
    // Graphics settings can replace the renderer without reaching our old
    // SwapBuffers import. The verified world-camera path is still running on
    // the game's render thread and must be able to revive both retained rings.
    RecoverGameImageContextsForScene();
    if (!state && !Initialize(request,request_gate_enabled,request_cadence)) {
        if (state) state->failed = true;
        Log("native-stereo-failed stage=initialization"); return false;
    }
    if (!state || state->failed || state->context_deleted || state->pair_active || state->awaiting_present || !state->bridge.owning_gl_context_is_current() ||
        request.render_width != state->width || request.render_height != state->height) return false;
    auto& s = *state;
    if (disable_monitor_vsync && !s.vsync_disabled) {
        GetSwapInterval get{};
        if (Load(s.swap_interval,"wglSwapIntervalEXT") && Load(get,"wglGetSwapIntervalEXT")) {
            if (s.original_swap_interval<0) s.original_swap_interval=get();
            s.vsync_disabled=s.swap_interval(0)!=FALSE;
            if (s.vsync_disabled) Log("native-stereo-monitor-vsync disabled=1 scope=current-game-window");
        }
    }
    const auto consumed = s.bridge.consumed_value();
    s.metadata.MarkWorldRendered();
    if (consumed == UINT64_MAX || s.last_ready >= UINT64_MAX-1U) { s.failed=true; return false; }
    StereoCaptureKey key{};
    key.request=request; key.eyes={eyes[0],eyes[1]}; key.camera=reinterpret_cast<std::uintptr_t>(camera);
    ++s.eligible_requests;
    if (s.request_gate.SameRequest(key)) ++s.repeated_requests;
    const bool reusable=!s.reset_history && s.request_gate.CanReuseView(key,GetTickCount64(),s.request_cadence);
    if (reusable) ++s.repeated_views;
    // Retain the existing no-eye-capture path: the monitor evaluates the CPU
    // scene once and balances replay state. No slot/fence/pending state changes.
    if ((s.request_gate_enabled || s.request_cadence) && reusable) { ++s.skipped_requests; return false; }
    s.slot = ipc::GpuStreamSlotForSequence(s.last_ready+1U);
    if (!ipc::CanReuseGpuStreamSlot(s.slot_ready[s.slot],consumed)) { ++s.skipped_busy; return false; }
    GlExtD3D12Diagnostic diagnostic{};
    if (s.slot_ready[s.slot] && s.bridge.WaitConsumed(s.slot,s.slot_ready[s.slot],diagnostic) != GlExtD3D12Status::Ok) {
        s.failed=true; Log("native-stereo-failed stage=wait-consumed"); return false;
    }
    s.pending = {}; s.pending.ready_value=s.last_ready+1U;
    s.depth_units_per_metre=engine_units_per_metre;
    for (unsigned eye=0;eye<2;++eye) s.projection_near[eye]=s.projection_far[eye]=0;
    s.pending_key=key;
    s.pending.camera_frame_id=camera_frame; s.pending.request=request;
    if (s.reset_history) s.pending.request.history_reset_reasons |=
        static_cast<std::uint32_t>(ipc::ResetReason::DeviceReset);
    ++s.attempted; s.capture_failure=0;
    s.captured_mask=0; s.view_mask=0; s.pair_active=true; return true;
}
bool BeginNativeStereoEye(std::size_t eye) noexcept {
    if (!state || !state->pair_active || active_eye >= 0 || eye >= 2) return false;
    auto& s = *state;
    glGetIntegerv(read_binding,&s.previous_read); glGetIntegerv(draw_binding,&s.previous_draw);
    glGetIntegerv(GL_READ_BUFFER,&s.previous_read_buffer); glGetIntegerv(GL_DRAW_BUFFER,&s.previous_draw_buffer);
    glGetIntegerv(GL_VIEWPORT,s.previous_viewport);
    // Camera::RenderScene is an ENGINE pass: retain its texture, shader and
    // enable-state changes, which its own caches also retain. An ALL_ATTRIB
    // push/pop around the call would restore GL alone and desynchronize caches.
    active_eye=static_cast<int>(eye); projection_seen=false;
    s.bind_fbo(draw_fbo,s.eye_fbo); s.bind_fbo(read_fbo,s.eye_fbo);
    glDrawBuffer(color_attachment); glReadBuffer(color_attachment);
    glViewport(0,0,static_cast<GLsizei>(s.width),static_cast<GLsizei>(s.height));
    glPushAttrib(GL_SCISSOR_BIT|GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE); glStencilMask(~0U); glClearColor(0,0,0,1); glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT); glPopAttrib();
    return true;
}
bool NativeStereoEyeActive() noexcept { return active_eye >= 0; }
bool NativeStereoHudActive() noexcept { return state && state->hud_active; }
void TraceNativeStereoSceneMatrices() noexcept {
    if (!state || active_eye<0 || (state->view_mask & (1U<<active_eye))) return;
    state->view_mask|=1U<<active_eye;
    GLfloat projection[16]{},view[16]{};
    glGetFloatv(GL_PROJECTION_MATRIX,projection); glGetFloatv(GL_MODELVIEW_MATRIX,view);
    // Sample the real window-depth mapping immediately before the scene, not
    // after rendering (when the engine may already have restored other state).
    GLdouble window_depth[2]{}; glGetDoublev(GL_DEPTH_RANGE,window_depth);
    math::Matrix4x4 captured_projection{};
    std::copy_n(projection,16,captured_projection.value.begin());
    const auto depth=NativeStereoDepthRange(captured_projection,
        state->pending.request.views[active_eye].fov,
        state->projection_near[active_eye],state->projection_far[active_eye],
        state->depth_units_per_metre,window_depth[0],window_depth[1]);
    // Existing guide/reprojection consumers assume canonical GL window depth.
    // Only advertise compositor metrics for that captured range; the generic
    // conversion helper's partial/reversed cases are not capture support.
    if (depth && window_depth[0]==0.0 && window_depth[1]==1.0) {
        state->pending.depth[active_eye]=*depth;
        state->pending.depth_mask |= 1U<<static_cast<unsigned>(active_eye);
    }
    auto& vp=state->pending.view_projection[active_eye].value;
    for (unsigned col=0;col<4;++col) for (unsigned row=0;row<4;++row) {
        vp[col*4+row]=0;
        for (unsigned k=0;k<4;++k) vp[col*4+row]+=projection[k*4+row]*view[col*4+k];
    }
    if (state->matrices_logged & (1U<<active_eye)) return;
    state->matrices_logged|=1U<<active_eye;
    for (unsigned kind=0;kind<2;++kind) {
        const auto* m=kind ? view:projection;
        char line[700]{};
        std::snprintf(line,sizeof(line),"native-stereo-matrix eye=%d kind=%s column_major=[%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g]",
            active_eye,kind ? "view":"projection",m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);
        Log(line);
    }
}
void CaptureNativeStereoEye() noexcept {
    if (!state || !state->pair_active || active_eye < 0) return;
    if (!projection_seen) { state->capture_failure=1; return; }
    auto& s = *state;
    GLint draw{}; glGetIntegerv(draw_binding,&draw);
    if (draw != static_cast<GLint>(s.eye_fbo)) { s.capture_failure=2; s.rejected_draw_fbo=draw; return; }
    const GLuint eye=static_cast<GLuint>(active_eye);
    if (s.captured_mask & (1U<<eye)) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    for (unsigned i=0;i<16 && glGetError()!=GL_NO_ERROR;++i) {}
    glDisable(GL_SCISSOR_TEST); glDisable(0x8DB9); // GL_FRAMEBUFFER_SRGB
    s.bind_fbo(read_fbo,s.eye_fbo); glReadBuffer(color_attachment);
    s.bind_fbo(draw_fbo,s.atlas_fbo);
    s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,s.bridge.gl_texture_name(s.slot),0);
    glDrawBuffer(color_attachment);
    if (s.check(draw_fbo) == framebuffer_complete) {
        const auto w=static_cast<GLint>(s.width),h=static_cast<GLint>(s.height);
        const auto x=static_cast<GLint>(eye*s.width);
        s.blit(0,0,w,h,x,h,x+w,0,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        if (glGetError() == GL_NO_ERROR &&
            s.depth_pack.Draw(s.eye_depth,x,static_cast<GLint>(ipc::StereoDepthOffset(s.height)),w,h)) s.captured_mask |= 1U<<eye;
        else s.capture_failure=3;
    } else {
        s.capture_failure=4;
    }
    s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,0,0);
    s.bind_fbo(draw_fbo,s.eye_fbo); s.bind_fbo(read_fbo,s.eye_fbo); glPopAttrib();
}
void EndNativeStereoEye() noexcept {
    if (!state || active_eye < 0) return;
    auto& s=*state;
    active_eye=-1; projection_seen=false;
    s.bind_fbo(read_fbo,static_cast<GLuint>(s.previous_read));
    s.bind_fbo(draw_fbo,static_cast<GLuint>(s.previous_draw));
    glReadBuffer(static_cast<GLenum>(s.previous_read_buffer));
    glDrawBuffer(static_cast<GLenum>(s.previous_draw_buffer));
    glViewport(s.previous_viewport[0],s.previous_viewport[1],s.previous_viewport[2],s.previous_viewport[3]);
}
bool EndNativeStereoPair(bool success) noexcept {
    if (!state || !state->pair_active) return false;
    auto& s=*state;
    EndNativeStereoEye(); s.pair_active=false;
    if (draw_trace) { std::fclose(draw_trace); draw_trace=nullptr;
        Log("native-stereo-draw-trace saved=logs/native-draw-trace.csv scope=first-pair"); }
    if (!success || s.captured_mask != 3) ++s.incomplete;
    const DWORD now=GetTickCount();
    if (!s.next_log || static_cast<LONG>(now-s.next_log)>=0) {
        char line[700]{};
        std::snprintf(line,sizeof(line),"native-stereo-counters attempted=%llu published=%llu incomplete=%llu busy=%llu eye_mask=%u capture_failure=%u camera_frame=%llu xr_frame=%llu redirected_binds=%llu rejected_fbo=%ld eye_fbo=%u eligible_requests=%llu repeated_requests=%llu repeated_views=%llu skipped_requests=%llu request_gate=%u request_cadence=%u",
            static_cast<unsigned long long>(s.attempted),static_cast<unsigned long long>(s.last_ready),
            static_cast<unsigned long long>(s.incomplete),static_cast<unsigned long long>(s.skipped_busy),
            s.captured_mask,s.capture_failure,static_cast<unsigned long long>(s.pending.camera_frame_id),
            static_cast<unsigned long long>(s.pending.request.frame_id),
            static_cast<unsigned long long>(s.redirected_binds),static_cast<long>(s.rejected_draw_fbo),s.eye_fbo,
            static_cast<unsigned long long>(s.eligible_requests),static_cast<unsigned long long>(s.repeated_requests),
            static_cast<unsigned long long>(s.repeated_views),static_cast<unsigned long long>(s.skipped_requests),s.request_gate_enabled ? 1U:0U,s.request_cadence ? 1U:0U);
        Log(line); s.next_log=now+5000;
    }
    if (!success || s.captured_mask != 3 || s.view_mask!=3) return false;
    s.pending.guide_mask=3;
    if (s.pending.depth_mask==3) s.pending.engine_units_per_metre=s.depth_units_per_metre;
    else {
        // Optional extension: unknown projection/range on either eye disables
        // depth for the whole pair, without dropping otherwise valid color.
        s.pending.depth_mask=0; s.pending.engine_units_per_metre=0;
        for (auto& depth:s.pending.depth) depth={};
    }
    if (!ipc::ValidStereoGuides(s.pending)) return false;
    // Commit eyes and UI together at SwapBuffers. Until then this slot remains
    // producer-owned and cannot be read by the XR consumer.
    s.awaiting_present=true;
    return true;
}

bool NativeStereoMonitorMirrorEnabled() noexcept {
    static const bool enabled=[]() noexcept {
        char value[3]{};
        const auto count=GetEnvironmentVariableA("KOTOR2VR_MONITOR_EYE_MIRROR",value,sizeof(value));
        return count<sizeof(value) && MonitorMirrorOption(std::string_view(value,count));
    }();
    return enabled;
}

bool TryNativeStereoMonitorMirror() noexcept {
    if (!state || !state->mirror_guard_ready || !state->awaiting_present || state->pair_active ||
        state->hud_active || state->context_deleted || !state->bridge.owning_gl_context_is_current()) return false;
    auto& s=*state;
    GLint read{},draw{},read_buffer{},draw_buffer{},vp[4]{};
    glGetIntegerv(read_binding,&read); glGetIntegerv(draw_binding,&draw);
    glGetIntegerv(GL_READ_BUFFER,&read_buffer); glGetIntegerv(GL_DRAW_BUFFER,&draw_buffer);
    glGetIntegerv(GL_VIEWPORT,vp);
    // Only the ordinary monitor back buffer, never an engine offscreen pass.
    if (draw!=0 || draw_buffer!=GL_BACK || vp[2]<=0 || vp[3]<=0 || vp[2]>8192 || vp[3]>8192) return false;
    const auto rect=FitMonitorMirror(vp[2],vp[3],static_cast<int>(s.width),static_cast<int>(s.height));
    if (!rect.width || !rect.height || glGetError()!=GL_NO_ERROR) return false;
    const GLboolean srgb=glIsEnabled(0x8DB9);
    bool success=false;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    __try {
        glDisable(GL_SCISSOR_TEST); glDisable(0x8DB9); // raw eye color, no extra sRGB conversion
        if (!s.mirror_fbo) s.gen_fbos(1,&s.mirror_fbo);
        if (!s.mirror_color) glGenTextures(1,&s.mirror_color);
        s.bind_fbo(draw_fbo,s.mirror_fbo);
        glBindTexture(GL_TEXTURE_2D,s.mirror_color);
        if (s.mirror_width!=vp[2] || s.mirror_height!=vp[3]) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,vp[2],vp[3],0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            if (glGetError()==GL_NO_ERROR) { s.mirror_width=vp[2]; s.mirror_height=vp[3]; }
        }
        s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,s.mirror_color,0);
        glDrawBuffer(color_attachment);
        s.bind_fbo(read_fbo,s.atlas_fbo);
        s.attach(read_fbo,color_attachment,GL_TEXTURE_2D,s.bridge.gl_texture_name(s.slot),0);
        glReadBuffer(color_attachment);
        if (s.mirror_fbo && s.mirror_color && s.mirror_width==vp[2] && s.mirror_height==vp[3] &&
            s.check(read_fbo)==framebuffer_complete && s.check(draw_fbo)==framebuffer_complete) {
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glClearColor(0,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT);
            // Atlas eyes are Y-flipped. NEAREST cannot bleed the adjacent right
            // eye or packed-depth row into this rectangle when downsampling.
            s.blit(0,static_cast<GLint>(s.height),static_cast<GLint>(s.width),0,
                rect.x,rect.y,rect.x+rect.width,rect.y+rect.height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            if (glGetError()==GL_NO_ERROR) {
                s.attach(read_fbo,color_attachment,GL_TEXTURE_2D,0,0);
                s.bind_fbo(read_fbo,s.mirror_fbo); glReadBuffer(color_attachment);
                s.bind_fbo(draw_fbo,static_cast<GLuint>(draw)); glDrawBuffer(static_cast<GLenum>(draw_buffer));
                // A same-size second blit supports a multisampled monitor:
                // scaling directly into its MSAA back buffer is invalid.
                s.blit(0,0,vp[2],vp[3],vp[0],vp[1],vp[0]+vp[2],vp[1]+vp[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
                success=glGetError()==GL_NO_ERROR;
            }
        }
    } __finally {
        s.bind_fbo(read_fbo,s.atlas_fbo); s.attach(read_fbo,color_attachment,GL_TEXTURE_2D,0,0);
        s.bind_fbo(read_fbo,static_cast<GLuint>(read)); s.bind_fbo(draw_fbo,static_cast<GLuint>(draw));
        glPopAttrib();
        if (srgb) glEnable(0x8DB9); else glDisable(0x8DB9);
        glReadBuffer(static_cast<GLenum>(read_buffer)); glDrawBuffer(static_cast<GLenum>(draw_buffer));
        glViewport(vp[0],vp[1],vp[2],vp[3]);
    }
    return success;
}

void BeginNativeStereoHud() noexcept {
    if (!state || !state->awaiting_present || state->hud_active) return;
    auto& s=*state;
    glGetIntegerv(read_binding,&s.previous_read); glGetIntegerv(draw_binding,&s.previous_draw);
    glGetIntegerv(GL_READ_BUFFER,&s.previous_read_buffer); glGetIntegerv(GL_DRAW_BUFFER,&s.previous_draw_buffer);
    glGetIntegerv(GL_VIEWPORT,s.previous_viewport);
    const GLint w=s.previous_viewport[2],h=s.previous_viewport[3];
    if (w<=0 || h<=0 || w>8192 || h>8192) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    GLint old_rb{}; glGetIntegerv(0x8CA7,&old_rb);
    if (!s.hud_fbo) { s.gen_fbos(1,&s.hud_fbo); glGenTextures(1,&s.hud_color); s.gen_renderbuffers(1,&s.hud_depth); }
    s.bind_fbo(draw_fbo,s.hud_fbo); s.bind_fbo(read_fbo,s.hud_fbo);
    if (s.hud_width!=w || s.hud_height!=h) {
        glBindTexture(GL_TEXTURE_2D,s.hud_color);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,s.hud_color,0);
        s.bind_renderbuffer(renderbuffer,s.hud_depth);
        s.storage(renderbuffer,0x88F0,w,h);
        s.attach_depth(draw_fbo,depth_stencil_attachment,renderbuffer,s.hud_depth);
        s.hud_width=w; s.hud_height=h;
    }
    glDrawBuffer(color_attachment); glReadBuffer(color_attachment);
    const bool complete=s.check(draw_fbo)==framebuffer_complete;
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE); glStencilMask(~0U); glClearColor(0,0,0,0); glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    s.bind_renderbuffer(renderbuffer,static_cast<GLuint>(old_rb));
    // Pop while the original FBO is bound (its draw/read-buffer enums are saved).
    s.bind_fbo(read_fbo,static_cast<GLuint>(s.previous_read));
    s.bind_fbo(draw_fbo,static_cast<GLuint>(s.previous_draw)); glPopAttrib();
    if (!complete) { Log("native-hud-failed stage=framebuffer"); return; }
    s.hud_active=true; s.hud_draws=0; s.hud_clears=0;
    GLboolean mask[4]{}; glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    s.hud_engine_alpha_mask=mask[3];
    glColorMask(mask[0],mask[1],mask[2],static_cast<GLboolean>(mask[0] || mask[1] || mask[2]));
    s.bind_fbo(read_fbo,s.hud_fbo); s.bind_fbo(draw_fbo,s.hud_fbo);
    glReadBuffer(color_attachment); glDrawBuffer(color_attachment);
    glViewport(0,0,w,h);
    GLint source{},destination{}; glGetIntegerv(GL_BLEND_SRC,&source); glGetIntegerv(GL_BLEND_DST,&destination);
    s.blend_separate(static_cast<GLenum>(source),static_cast<GLenum>(destination),GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
}

void FinishNativeStereoPresent() noexcept {
    if (!state || !state->awaiting_present || !state->bridge.owning_gl_context_is_current()) return;
    auto& s=*state;
    if (s.hud_active) {
        s.hud_active=false;
        GLboolean mask[4]{}; glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        glColorMask(mask[0],mask[1],mask[2],s.hud_engine_alpha_mask);
        s.bind_fbo(read_fbo,static_cast<GLuint>(s.previous_read));
        s.bind_fbo(draw_fbo,static_cast<GLuint>(s.previous_draw));
        glReadBuffer(static_cast<GLenum>(s.previous_read_buffer));
        glDrawBuffer(static_cast<GLenum>(s.previous_draw_buffer));
        glViewport(s.previous_viewport[0],s.previous_viewport[1],s.previous_viewport[2],s.previous_viewport[3]);
        GLint source{},destination{},matrix_mode{},program{};
        glGetIntegerv(GL_BLEND_SRC,&source); glGetIntegerv(GL_BLEND_DST,&destination);
        glGetIntegerv(GL_MATRIX_MODE,&matrix_mode); glGetIntegerv(0x8B8D,&program);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_SCISSOR_TEST); glDisable(0x8DB9);
        s.bind_fbo(read_fbo,s.hud_fbo); glReadBuffer(color_attachment);
        s.bind_fbo(draw_fbo,s.atlas_fbo);
        s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,s.bridge.gl_texture_name(s.slot),0);
        glDrawBuffer(color_attachment);
        const auto width=(std::min)(static_cast<GLuint>(s.hud_width),s.width*2U);
        const auto height=(std::min)(static_cast<GLuint>(s.hud_height),ipc::kStereoHudMaximumHeight);
        for (unsigned i=0;i<16 && glGetError()!=GL_NO_ERROR;++i) {}
        s.blit(0,0,s.hud_width,s.hud_height,0,static_cast<GLint>(s.height+height),
            static_cast<GLint>(width),static_cast<GLint>(s.height),GL_COLOR_BUFFER_BIT,GL_LINEAR);
        if (glGetError()==GL_NO_ERROR && s.hud_draws) {
            s.pending.hud_width=width; s.pending.hud_height=height;
            s.pending.hud_source_aspect=static_cast<float>(s.hud_width)/static_cast<float>(s.hud_height);
        }
        s.attach(draw_fbo,color_attachment,GL_TEXTURE_2D,0,0);
        s.bind_fbo(read_fbo,static_cast<GLuint>(s.previous_read));
        s.bind_fbo(draw_fbo,static_cast<GLuint>(s.previous_draw));
        glReadBuffer(static_cast<GLenum>(s.previous_read_buffer)); glDrawBuffer(static_cast<GLenum>(s.previous_draw_buffer));
        // The normal game has drawn UI once into a transparent target. Composite
        // that premultiplied result onto its untouched monitor scene.
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_STENCIL_TEST);
        glDisable(GL_ALPHA_TEST); glDisable(GL_FOG); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
        glDisable(0x8620); glDisable(0x8804); s.use_program(0);
        GLint units{}; glGetIntegerv(0x84E2,&units);
        for (GLint i=0;i<units;++i) {
            s.active_texture(0x84C0U+static_cast<GLuint>(i));
            glDisable(GL_TEXTURE_1D); glDisable(GL_TEXTURE_2D); glDisable(0x806F); glDisable(0x8513); glDisable(0x84F5);
        }
        s.active_texture(0x84C0); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,s.hud_color);
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
        glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glColor4f(1,1,1,1);
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2f(-1,-1); glTexCoord2f(1,0); glVertex2f(1,-1);
        glTexCoord2f(1,1); glVertex2f(1,1); glTexCoord2f(0,1); glVertex2f(-1,1);
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_TEXTURE); glPopMatrix();
        glMatrixMode(static_cast<GLenum>(matrix_mode)); s.use_program(static_cast<GLuint>(program));
        glPopAttrib(); glBlendFunc(static_cast<GLenum>(source),static_cast<GLenum>(destination));
        if (++s.hud_frames==1 || s.hud_frames%300==0) {
            char line[180]{}; std::snprintf(line,sizeof(line),"native-hud frame=%llu draws=%u clears=%u source=%dx%d captured=%ux%u",
                static_cast<unsigned long long>(s.hud_frames),s.hud_draws,s.hud_clears,s.hud_width,s.hud_height,s.pending.hud_width,s.pending.hud_height);
            Log(line);
        }
    }
    s.awaiting_present=false;
    if (!s.metadata.Write(s.pending)) return;
    GlExtD3D12Diagnostic diagnostic{};
    if (s.bridge.SignalReady(s.slot,s.pending.ready_value,diagnostic) != GlExtD3D12Status::Ok) {
        s.failed=true; Log("native-stereo-failed stage=signal-ready"); return;
    }
    s.last_ready=s.pending.ready_value; s.slot_ready[s.slot]=s.last_ready;
    s.request_gate.Published(s.pending_key,GetTickCount64(),
        s.pending.request.presentation_state==ipc::PresentationState::DialogueStereo);
    s.reset_history=false;
    if (s.last_ready==1) Log("native-stereo-first-pair eyes=2 same-camera-tick=1");
}
}
