#include "depth_pack.hpp"
#include <cstdint>

namespace k2vr::game32 {
namespace {
template<class T> bool Load(T& pointer,const char* name) {
    const auto proc=wglGetProcAddress(name);
    const auto address=reinterpret_cast<std::uintptr_t>(proc);
    if (address<=3 || address==UINTPTR_MAX) return false;
    pointer=reinterpret_cast<T>(proc); return true;
}
}
bool DepthPack::Initialize() noexcept {
    if (program_) return true;
    GLuint (APIENTRY* create_shader)(GLenum){};
    void (APIENTRY* shader_source)(GLuint,GLsizei,const char* const*,const GLint*){};
    void (APIENTRY* compile_shader)(GLuint){};
    void (APIENTRY* get_shader)(GLuint,GLenum,GLint*){};
    void (APIENTRY* delete_shader)(GLuint){};
    GLuint (APIENTRY* create_program)(){};
    void (APIENTRY* attach_shader)(GLuint,GLuint){};
    void (APIENTRY* link_program)(GLuint){};
    void (APIENTRY* get_program)(GLuint,GLenum,GLint*){};
    void (APIENTRY* delete_program)(GLuint){};
    GLint (APIENTRY* uniform_location)(GLuint,const char*){};
    void (APIENTRY* uniform1i)(GLint,GLint){};
    if (!Load(create_shader,"glCreateShader") || !Load(shader_source,"glShaderSource") ||
        !Load(compile_shader,"glCompileShader") || !Load(get_shader,"glGetShaderiv") ||
        !Load(delete_shader,"glDeleteShader") || !Load(create_program,"glCreateProgram") ||
        !Load(attach_shader,"glAttachShader") || !Load(link_program,"glLinkProgram") ||
        !Load(get_program,"glGetProgramiv") || !Load(delete_program,"glDeleteProgram") ||
        !Load(uniform_location,"glGetUniformLocation") || !Load(uniform1i,"glUniform1i") ||
        !Load(use_program_,"glUseProgram") || !Load(active_texture_,"glActiveTexture") ||
        !Load(uniform4i_,"glUniform4i") || !Load(bind_sampler_,"glBindSampler")) return false;
    const char* sources[]={
        "#version 330 compatibility\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}",
        "#version 330 compatibility\nuniform sampler2D depthImage; uniform ivec4 rect; out vec4 packedDepth;\n"
        "void main(){ivec2 p=ivec2(gl_FragCoord.xy)-rect.xy;p.y=rect.w-1-p.y;"
        "float d=texelFetch(depthImage,p,0).r;uint v=uint(round(clamp(d,0.0,1.0)*16777215.0));"
        "packedDepth=vec4(float((v>>16)&255u),float((v>>8)&255u),float(v&255u),255.0)/255.0;}"};
    GLuint shaders[2]{}; bool ok=true;
    for (unsigned i=0;i<2;++i) {
        shaders[i]=create_shader(i ? 0x8B30:0x8B31);
        shader_source(shaders[i],1,&sources[i],nullptr); compile_shader(shaders[i]);
        GLint compiled{}; get_shader(shaders[i],0x8B81,&compiled); ok=ok && compiled;
    }
    if (ok) {
        program_=create_program();
        for (auto shader:shaders) attach_shader(program_,shader);
        link_program(program_); GLint linked{}; get_program(program_,0x8B82,&linked); ok=linked!=0;
    }
    for (auto shader:shaders) if (shader) delete_shader(shader);
    if (!ok) { if (program_) delete_program(program_); program_=0; return false; }
    rectangle_=uniform_location(program_,"rect");
    GLint previous{}; glGetIntegerv(0x8B8D,&previous);
    use_program_(program_); uniform1i(uniform_location(program_,"depthImage"),0);
    use_program_(static_cast<GLuint>(previous));
    return rectangle_>=0;
}
bool DepthPack::Draw(GLuint texture,GLint x,GLint y,GLsizei width,GLsizei height) noexcept {
    if (!program_ || !texture || width<=0 || height<=0) return false;
    GLint previous_program{},previous_active{},previous_sampler{};
    glGetIntegerv(0x8B8D,&previous_program); glGetIntegerv(0x84E0,&previous_active);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    active_texture_(0x84C0); glGetIntegerv(0x8919,&previous_sampler); bind_sampler_(0,0);
    glBindTexture(GL_TEXTURE_2D,texture);
    const GLenum caps[]={GL_BLEND,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_ALPHA_TEST,GL_CULL_FACE,
         GL_SCISSOR_TEST,GL_DITHER,GL_COLOR_LOGIC_OP,GL_FOG,0x8DB9,0x8620,0x8804,0x8C89};
    for (GLenum cap:caps) glDisable(cap);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glViewport(x,y,width,height); use_program_(program_); uniform4i_(rectangle_,x,y,width,height);
    glDrawArrays(GL_TRIANGLES,0,3);
    const bool ok=glGetError()==GL_NO_ERROR;
    use_program_(static_cast<GLuint>(previous_program)); bind_sampler_(0,static_cast<GLuint>(previous_sampler));
    glPopAttrib(); active_texture_(static_cast<GLenum>(previous_active));
    return ok;
}
}
