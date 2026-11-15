#pragma once
// The window's D3D12 side: a swap chain on the renderer's device and queue, ImGui, the viewport texture the
// renderer draws the level into, and textures for images (the menu's map backgrounds).

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <stdint.h>

#include <imgui.h>

#include <echo/renderer/render_target_view.hpp>

namespace erv {

using Microsoft::WRL::ComPtr;

struct Texture {
    ComPtr<ID3D12Resource> res;
    ImTextureID id = 0;
    uint32_t w = 0, h = 0;
};

class Gpu {
public:
    static constexpr UINT kFrames = 2;
    bool Init(HWND, ID3D12Device*, ID3D12CommandQueue*);
    void Resize(UINT w, UINT h);
    void Shutdown();

    // The level's frame: a texture the size of the window, resting in PIXEL_SHADER_RESOURCE.
    bool EnsureViewport(UINT w, UINT h);
    echo::render::RenderTargetView ViewportTarget() const;
    ImTextureID ViewportId() const { return viewport_id_; }

    // Uploads an RGBA8 image (waits for the copy). Up to kMaxTextures.
    bool Upload(const uint8_t* rgba, uint32_t w, uint32_t h, Texture& out);

    // One frame: Begin before ImGui::Render, Present after.
    void Present(ImDrawData*);
    void WaitIdle();

    UINT width = 0, height = 0;

private:
    static constexpr UINT kMaxTextures = 64;
    enum { kSrvFont = 0, kSrvViewport = 1, kSrvFirstTexture = 2 };
    enum { kRtvViewport = kFrames };
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT i) const;
    void MakeBackBuffers();

    HWND hwnd_ = nullptr;
    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* q_ = nullptr;
    ComPtr<IDXGISwapChain3> sc_;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    UINT rtv_stride_ = 0, srv_stride_ = 0;
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        UINT64 fence = 0;
    } frames_[kFrames];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;
    ComPtr<ID3D12Resource> back_[kFrames];
    UINT frame_ = 0;
    ComPtr<ID3D12Resource> viewport_;
    UINT vw_ = 0, vh_ = 0;
    ImTextureID viewport_id_ = 0;
    UINT next_texture_ = 0;
    bool tearing_ = false;
};

}  // namespace erv
