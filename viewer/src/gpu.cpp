#include "gpu.h"

#include <dxgi1_5.h>

#include <imgui_impl_dx12.h>

namespace erv {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

D3D12_CPU_DESCRIPTOR_HANDLE Gpu::Rtv(UINT i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * rtv_stride_;
    return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE Gpu::SrvCpu(UINT i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * srv_stride_;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Gpu::SrvGpu(UINT i) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * srv_stride_;
    return h;
}

bool Gpu::Init(HWND hwnd, ID3D12Device* dev, ID3D12CommandQueue* q) {
    hwnd_ = hwnd, dev_ = dev, q_ = q;
    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = kFrames + 1;
    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sd.NumDescriptors = kSrvFirstTexture + kMaxTextures;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap_))) || FAILED(dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&srv_heap_))))
        return false;
    rtv_stride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_stride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (Frame& f : frames_)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
        FAILED(list_->Close()) || FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return false;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IDXGIFactory5> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    tearing_ = tearing != FALSE;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.BufferCount = kFrames;
    desc.Format = kBackBufferFormat;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(q, hwnd, &desc, nullptr, nullptr, &sc1)) || FAILED(sc1.As(&sc_))) return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc_->SetMaximumFrameLatency(kFrames);
    waitable_ = sc_->GetFrameLatencyWaitableObject();
    RECT rc;
    GetClientRect(hwnd, &rc);
    width = UINT(rc.right - rc.left), height = UINT(rc.bottom - rc.top);
    MakeBackBuffers();
    return ImGui_ImplDX12_Init(dev, kFrames, kBackBufferFormat, srv_heap_.Get(), SrvCpu(kSrvFont), SrvGpu(kSrvFont));
}

void Gpu::MakeBackBuffers() {
    for (UINT i = 0; i < kFrames; ++i) {
        sc_->GetBuffer(i, IID_PPV_ARGS(&back_[i]));
        dev_->CreateRenderTargetView(back_[i].Get(), nullptr, Rtv(i));
    }
}

void Gpu::WaitIdle() {
    if (!fence_) return;
    const UINT64 v = ++fence_value_;
    q_->Signal(fence_.Get(), v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void Gpu::Resize(UINT w, UINT h) {
    if (!sc_ || w == 0 || h == 0 || (w == width && h == height)) return;
    WaitIdle();
    for (auto& b : back_) b.Reset();
    sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u));
    width = w, height = h;
    MakeBackBuffers();
}

void Gpu::Shutdown() {
    WaitIdle();
    ImGui_ImplDX12_Shutdown();
    if (fence_event_) CloseHandle(fence_event_);
}

bool Gpu::EnsureViewport(UINT w, UINT h) {
    if (viewport_ && w == vw_ && h == vh_) return true;
    if (w == 0 || h == 0) return false;
    WaitIdle();
    viewport_.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = echo::render::kViewportRtvFormat;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                             IID_PPV_ARGS(&viewport_))))
        return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = echo::render::kViewportRtvFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev_->CreateRenderTargetView(viewport_.Get(), &rtv, Rtv(kRtvViewport));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = echo::render::kViewportTextureFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(viewport_.Get(), &srv, SrvCpu(kSrvViewport));
    vw_ = w, vh_ = h;
    viewport_id_ = ImTextureID(SrvGpu(kSrvViewport).ptr);
    return true;
}

echo::render::RenderTargetView Gpu::ViewportTarget() const { return {viewport_.Get(), Rtv(kRtvViewport), vw_, vh_}; }

bool Gpu::Upload(const uint8_t* rgba, uint32_t w, uint32_t h, Texture& out) {
    if (next_texture_ >= kMaxTextures) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w, d.Height = h, d.DepthOrArraySize = 1, d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out.res))))
        return false;
    const UINT pitch = (w * 4 + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = UINT64(pitch) * h, b.Height = 1, b.DepthOrArraySize = 1, b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    if (FAILED(dev_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))))
        return false;
    uint8_t* p = nullptr;
    buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (uint32_t y = 0; y < h; ++y) memcpy(p + size_t(y) * pitch, rgba + size_t(y) * w * 4, size_t(w) * 4);
    buf->Unmap(0, nullptr);

    WaitIdle();
    Frame& fr = frames_[frame_];
    fr.alloc->Reset();
    list_->Reset(fr.alloc.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = out.res.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = buf.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = out.res.Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list_->ResourceBarrier(1, &bar);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    q_->ExecuteCommandLists(1, lists);
    WaitIdle();

    const UINT slot = kSrvFirstTexture + next_texture_++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(out.res.Get(), &srv, SrvCpu(slot));
    out.id = ImTextureID(SrvGpu(slot).ptr);
    out.w = w, out.h = h;
    return true;
}

void Gpu::Present(ImDrawData* dd) {
    WaitForSingleObjectEx(waitable_, 1000, TRUE);
    frame_ = sc_->GetCurrentBackBufferIndex();
    Frame& fr = frames_[frame_];
    if (fence_->GetCompletedValue() < fr.fence) {
        fence_->SetEventOnCompletion(fr.fence, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    fr.alloc->Reset();
    list_->Reset(fr.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = back_[frame_].Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list_->ResourceBarrier(1, &bar);
    const float clear[4] = {0.02f, 0.025f, 0.035f, 1.f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(frame_);
    list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(dd, list_.Get());
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    list_->ResourceBarrier(1, &bar);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    q_->ExecuteCommandLists(1, lists);
    sc_->Present(1, 0);
    fr.fence = ++fence_value_;
    q_->Signal(fence_.Get(), fr.fence);
}

}  // namespace erv
