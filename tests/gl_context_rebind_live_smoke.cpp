// Standalone Win32/x86 hardware test; never loads or controls the game.
// Sources: this file + src/game32/gl_ext_d3d12_bridge.cpp; C++20.
// Include: src/game32. Libraries: d3d12 dxgi opengl32 gdi32 user32.
// Uses an invisible, nonactivating window. No ShowWindow or game input.
#include "gl_ext_d3d12_bridge.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <gl/GL.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace k2vr::game32;
using Microsoft::WRL::ComPtr;
constexpr UINT kWidth = 64, kHeight = 32;
constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment = 0x8CE0;
using Pixel = std::array<unsigned char, 4>;
using Gen = void(APIENTRY*)(GLsizei, GLuint*);
using Bind = void(APIENTRY*)(GLenum, GLuint);
using Attach = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using CheckFramebuffer = GLenum(APIENTRY*)(GLenum);

void Require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
void Hr(HRESULT result, const char* detail) {
    if (FAILED(result)) {
        std::printf("HRESULT=0x%08lX: %s\n", static_cast<unsigned long>(result), detail);
        throw std::runtime_error(detail);
    }
}
void Status(GlExtD3D12Status actual, GlExtD3D12Status expected,
            const GlExtD3D12Diagnostic& diagnostic, const char* stage) {
    if (actual != expected) {
        std::printf("%s: got=%s expected=%s detail=%s gl=0x%X hr=0x%08X\n",
                    stage, ToString(actual).data(), ToString(expected).data(),
                    diagnostic.detail, diagnostic.gl_error,
                    static_cast<unsigned>(diagnostic.hresult));
        throw std::runtime_error(stage);
    }
}
template<typename Function> Function GlProc(const char* name) {
    auto address = wglGetProcAddress(name);
    Require(IsUsableGlExtD3D12ProcAddressValue(
                reinterpret_cast<std::uintptr_t>(address)), name);
    return reinterpret_cast<Function>(address);
}

struct HiddenGl {
    HWND window{};
    HDC dc{};
    HGLRC context{};
    ~HiddenGl() { Destroy(); }
    void Destroy() noexcept {
        if (context) {
            (void)wglMakeCurrent(nullptr, nullptr);
            (void)wglDeleteContext(context);
            context = nullptr;
        }
        if (dc) { ReleaseDC(window, dc); dc = nullptr; }
        if (window) { DestroyWindow(window); window = nullptr; }
    }
    void DestroyChecked() {
        glFinish();
        Require(glGetError() == GL_NO_ERROR, "finish old GL context");
        Require(wglMakeCurrent(nullptr, nullptr) != FALSE, "unbind old context");
        Require(wglDeleteContext(context) != FALSE, "destroy old context");
        context = nullptr;
        Destroy();
    }
    void Create() {
        WNDCLASSW wc{};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"K2vrContextRebindSmoke";
        Require(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "register hidden GL window class");
        window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName,
            L"K2VR hidden context rebind test", WS_POPUP, 0, 0, kWidth, kHeight,
            nullptr, nullptr, wc.hInstance, nullptr);
        Require(window != nullptr && !IsWindowVisible(window), "create invisible window");
        dc = GetDC(window);
        Require(dc != nullptr, "get hidden window DC");
        PIXELFORMATDESCRIPTOR pf{};
        pf.nSize = sizeof(pf); pf.nVersion = 1;
        pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pf.iPixelType = PFD_TYPE_RGBA; pf.cColorBits = 32;
        const int format = ChoosePixelFormat(dc, &pf);
        Require(format && SetPixelFormat(dc, format, &pf), "set GL pixel format");
        context = wglCreateContext(dc);
        Require(context && wglMakeCurrent(dc, context), "create current replacement context");
        std::printf("hidden GL context=%p renderer=%s\n", context, glGetString(GL_RENDERER));
    }
};

Pixel Expected(std::uint64_t sequence, UINT x, UINT y) {
    const auto quadrant = (x >= kWidth / 2 ? 1U : 0U) + (y >= kHeight / 2 ? 2U : 0U);
    const auto bits = static_cast<unsigned>((sequence + quadrant) % 7 + 1);
    return {static_cast<unsigned char>((bits & 1) ? 255 : 0),
            static_cast<unsigned char>((bits & 2) ? 255 : 0),
            static_cast<unsigned char>((bits & 4) ? 255 : 0), 255};
}

struct GlWriter {
    GLuint framebuffer{};
    Bind bind{};
    Attach attach{};
    CheckFramebuffer check{};
    void Recreate() {
        // The prior FBO died with its context; never delete its name here.
        framebuffer = 0;
        bind = GlProc<Bind>("glBindFramebuffer");
        attach = GlProc<Attach>("glFramebufferTexture2D");
        check = GlProc<CheckFramebuffer>("glCheckFramebufferStatus");
        GlProc<Gen>("glGenFramebuffers")(1, &framebuffer);
        Require(framebuffer != 0, "create current-context FBO");
    }
    void Draw(GLuint texture, std::uint64_t sequence) {
        bind(kFramebuffer, framebuffer);
        attach(kFramebuffer, kColorAttachment, GL_TEXTURE_2D, texture, 0);
        glDrawBuffer(kColorAttachment);
        Require(check(kFramebuffer) == 0x8CD5, "imported texture FBO complete");
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_SCISSOR_TEST);
        for (UINT y = 0; y < kHeight; y += kHeight / 2) {
            for (UINT x = 0; x < kWidth; x += kWidth / 2) {
                const auto pixel = Expected(sequence, x, y);
                glScissor(x, y, kWidth / 2, kHeight / 2);
                glClearColor(pixel[0] / 255.0F, pixel[1] / 255.0F,
                             pixel[2] / 255.0F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        glDisable(GL_SCISSOR_TEST);
        Require(glGetError() == GL_NO_ERROR, "render actual GL pattern");
    }
};

// These original host-side COM objects stay alive for the entire test. Pixel
// changes and advancing fences through them prove the backing resources survive;
// merely reopening the same object names would not prove this.
struct D3DReader {
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>, 3> colors;
    ComPtr<ID3D12Fence> ready, consumed, copied;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t copy_sequence{};
    SIZE_T readback_size{};
    HANDLE event{};
    ~D3DReader() { if (event) CloseHandle(event); }

    template<typename Interface>
    void Open(std::wstring_view name, ComPtr<Interface>& result) {
        HANDLE handle{};
        const std::wstring terminated(name);
        Hr(device->OpenSharedHandleByName(terminated.c_str(), GENERIC_ALL, &handle),
           "open original named D3D12 handle");
        const auto hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(result.GetAddressOf()));
        CloseHandle(handle);
        Hr(hr, "open original named D3D12 object");
    }
    void Initialize(GlExtD3D12AdapterLuid luid, const GlExtD3D12RingObjectNames& names) {
        ComPtr<IDXGIFactory1> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            Hr(result, "enumerate GL adapter");
            DXGI_ADAPTER_DESC1 description{};
            Hr(adapter->GetDesc1(&description), "read adapter LUID");
            if (description.AdapterLuid.LowPart != luid.low_part ||
                description.AdapterLuid.HighPart != luid.high_part) continue;
            Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device)), "create reader on same GPU");
            break;
        }
        Require(device != nullptr, "find same-LUID D3D12 adapter");
        for (std::size_t slot = 0; slot < colors.size(); ++slot) Open(names.colors[slot], colors[slot]);
        Open(names.ready_fence, ready);
        Open(names.consumed_fence, consumed);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create copy queue");
        Hr(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        Hr(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)), "create copy commands");
        Hr(commands->Close(), "close initial commands");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied)), "create copy fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event != nullptr, "create bounded fence wait event");
        const auto description = colors[0]->GetDesc();
        Require(description.Width == kWidth && description.Height == kHeight &&
                description.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "named resource dimensions/format");
        UINT64 size{};
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        readback_size = static_cast<SIZE_T>(size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size; buffer.Height = 1; buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create pixel readback");
    }
    void Wait(ID3D12Fence* fence, std::uint64_t value) {
        Require(fence->GetCompletedValue() != UINT64_MAX, "GPU device not removed");
        if (fence->GetCompletedValue() < value) {
            Hr(fence->SetEventOnCompletion(value, event), "arm fence event");
            Require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU fence completed within 5s");
        }
        const auto completed = fence->GetCompletedValue();
        Require(completed >= value && completed != UINT64_MAX, "completed fence value valid");
    }
    void CopyAndCheck(std::size_t slot, std::uint64_t value) {
        // Observe completion before queueing the wait so a failed GL signal
        // cannot leave an unbounded GPU queue wait during test cleanup.
        Wait(ready.Get(), value);
        Hr(queue->Wait(ready.Get(), value), "GPU waits for original ready fence");
        Hr(allocator->Reset(), "reset idle allocator");
        Hr(commands->Reset(allocator.Get(), nullptr), "reset copy commands");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = colors[slot].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = colors[slot].Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        commands->ResourceBarrier(1, &barrier);
        Hr(commands->Close(), "close pixel copy");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        Hr(queue->Signal(copied.Get(), ++copy_sequence), "signal copy completion");
        Wait(copied.Get(), copy_sequence);
        const D3D12_RANGE range{0, readback_size};
        void* mapped{};
        Hr(readback->Map(0, &range, &mapped), "map actual D3D pixels");
        const auto* bytes = static_cast<const unsigned char*>(mapped) + footprint.Offset;
        unsigned mismatches{};
        for (UINT y = 0; y < kHeight; ++y) for (UINT x = 0; x < kWidth; ++x) {
            const auto expected = Expected(value, x, y);
            const auto* actual = bytes + y * footprint.Footprint.RowPitch + x * 4;
            for (unsigned channel = 0; channel < 4; ++channel)
                if (actual[channel] != expected[channel]) ++mismatches;
        }
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        std::printf("pixels seq=%llu slot=%zu mismatched_channels=%u original_resource=%p\n",
                    static_cast<unsigned long long>(value), slot, mismatches, colors[slot].Get());
        Require(mismatches == 0, "actual pixels match through original D3D resource");
    }
    void Consume(std::uint64_t value) {
        Hr(queue->Signal(consumed.Get(), value), "signal original consumed fence after copy");
        Wait(consumed.Get(), value);
    }
};

void Run() {
    HiddenGl gl;
    gl.Create();
    GlExtD3D12Bridge bridge;
    GlExtD3D12Diagnostic diagnostic;
    Status(bridge.Initialize(diagnostic), GlExtD3D12Status::Ok, diagnostic, "initialize bridge");
    const auto luid = bridge.adapter_luid();
    const auto prefix = L"Local\\K2VR-RebindSmoke-" + std::to_wstring(GetCurrentProcessId()) +
                        L"-" + std::to_wstring(GetTickCount64());
    const std::array<std::wstring, 3> color_names{prefix + L"-0", prefix + L"-1", prefix + L"-2"};
    const auto ready_name = prefix + L"-ready", consumed_name = prefix + L"-consumed";
    const GlExtD3D12RingObjectNames names{
        {color_names[0], color_names[1], color_names[2]}, ready_name, consumed_name};
    Status(bridge.CreateNamedStreams({kWidth, kHeight}, names, diagnostic),
           GlExtD3D12Status::Ok, diagnostic, "create named ring");
    D3DReader reader;
    reader.Initialize(luid, names);
    GlWriter writer;
    writer.Recreate();
    for (std::uint64_t generation = 0; generation < 3; ++generation) {
        const auto base = generation * 3;
        for (std::size_t slot = 0; slot < 3; ++slot) {
            const auto value = base + slot + 1;
            writer.Draw(bridge.gl_texture_name(slot), value);
            Status(bridge.SignalReady(slot, value, diagnostic), GlExtD3D12Status::Ok,
                   diagnostic, "signal rendered frame");
            reader.CopyAndCheck(slot, value);
            // Two slots have already acquired GL ownership in the old context;
            // the third is deliberately not acknowledged until after rebind.
            if (slot < 2) {
                reader.Consume(value);
                Status(bridge.WaitConsumed(slot, value, diagnostic), GlExtD3D12Status::Ok,
                       diagnostic, "prime old imported wait bookkeeping");
            }
        }
        if (generation == 2) {
            reader.Consume(base + 3);
            Status(bridge.WaitConsumed(2, base + 3, diagnostic), GlExtD3D12Status::Ok,
                   diagnostic, "final reacquire");
            glFinish();
            break;
        }
        const std::array<GLuint, 3> old_names{bridge.gl_texture_name(0),
            bridge.gl_texture_name(1), bridge.gl_texture_name(2)};
        const auto old_context = gl.context;
        gl.DestroyChecked();
        Status(bridge.RebindAfterContextReplacement(diagnostic), GlExtD3D12Status::NoCurrentGlContext,
               diagnostic, "recoverable no-context rebind");
        Require(bridge.initialized() && !bridge.stream_created() &&
                bridge.active_slot_count() == 3 && bridge.ready_value() == base + 3 &&
                bridge.consumed_value() == base + 2 && SameGlExtD3D12AdapterLuid(luid, bridge.adapter_luid()),
                "failed rebind retains D3D resources, adapter and fence values");
        gl.Create();
        std::printf("replacement=%llu numeric_HGLRC_reused=%u\n",
            static_cast<unsigned long long>(generation + 1), old_context == gl.context ? 1U : 0U);
        // Populate colliding names in the NEW namespace. A wrong old-name
        // deletion would destroy these unrelated textures, which is observable.
        constexpr Pixel sentinel{255, 0, 255, 255};
        for (const auto name : old_names) {
            glBindTexture(GL_TEXTURE_2D, name);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, sentinel.data());
        }
        Status(bridge.RebindAfterContextReplacement(diagnostic), GlExtD3D12Status::Ok,
               diagnostic, "reimport original resources into replacement context");
        for (const auto name : old_names) {
            Require(glIsTexture(name) == GL_TRUE, "new-context colliding texture not deleted");
            glBindTexture(GL_TEXTURE_2D, name);
            GLint width{}, height{};
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            // Fail before the bounded readback if a bad rebind deleted this
            // sentinel and reused its name for a full-sized imported texture.
            Require(width == 1 && height == 1, "colliding texture dimensions preserved");
            Pixel actual{};
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
            Require(actual == sentinel, "new-context colliding texture pixels preserved");
        }
        Require(glGetError() == GL_NO_ERROR && bridge.stream_created() &&
                bridge.owning_gl_context_is_current() && bridge.active_slot_count() == 3 &&
                SameGlExtD3D12AdapterLuid(luid, bridge.adapter_luid()) &&
                reader.ready->GetCompletedValue() == base + 3 &&
                reader.consumed->GetCompletedValue() == base + 2,
                "successful rebind retains original host fences and adapter");
        Status(bridge.SignalReady(0, base + 3, diagnostic), GlExtD3D12Status::InvalidFenceValue,
               diagnostic, "ready sequence cannot restart");
        Status(bridge.SignalReady(0, base + 4, diagnostic), GlExtD3D12Status::StreamSlotNotConsumed,
               diagnostic, "old imported GL waits cannot authorize reuse");
        Status(bridge.WaitConsumed(2, base + 3, diagnostic), GlExtD3D12Status::GlSemaphoreWaitFailure,
               diagnostic, "cannot reacquire before D3D completion");
        reader.Consume(base + 3);
        // Older slots must work after a newer-slot wait: the GL barriers are per
        // texture even though the D3D12 completion fence is a single timeline.
        for (const std::size_t slot : {2U, 0U, 1U}) {
            Status(bridge.WaitConsumed(slot, base + slot + 1, diagnostic), GlExtD3D12Status::Ok,
                   diagnostic, "reacquire each reused slot in replacement context");
        }
        writer.Recreate();
    }
    Require(bridge.ready_value() == 9 && bridge.consumed_value() == 9,
            "original fences advance continuously through both replacements");
    Status(bridge.Shutdown(diagnostic), GlExtD3D12Status::Ok, diagnostic, "clean bridge shutdown");
    std::puts("PASS: two window/context replacements; nine GL-to-D3D pixel frames; original ring/fences retained; failed-rebind retry; per-slot waits; colliding GL names preserved.");
}
} // namespace

int main() {
    try { Run(); return 0; }
    catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
