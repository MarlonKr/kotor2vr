#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <gl/GL.h>

namespace k2vr::game32 {
// Store raw D24 depth in RGB bytes in the same fenced RGBA8 atlas as color.
// RGB is most/medium/least significant byte; alpha=255. No CPU readback.
class DepthPack final {
public:
    bool Initialize() noexcept;
    bool Draw(GLuint depth_texture, GLint x, GLint y, GLsizei width, GLsizei height) noexcept;
private:
    GLuint program_{};
    GLint rectangle_{};
    void (APIENTRY* use_program_)(GLuint){};
    void (APIENTRY* active_texture_)(GLenum){};
    void (APIENTRY* uniform4i_)(GLint,GLint,GLint,GLint,GLint){};
    void (APIENTRY* bind_sampler_)(GLuint,GLuint){};
};
}
