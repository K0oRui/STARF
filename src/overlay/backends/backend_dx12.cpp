#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <MinHook.h>
#include <d3d12.h>
#include "../../../third_party/unity/IUnityGraphicsD3D12.h"
#include "dx12_submission.h"

#ifdef _WIN64
using Microsoft::WRL::ComPtr;
namespace {
// Store the exact presenting queue on its swapchain; DXGI owns its lifetime.
const GUID queue_tag = {0x91b5d402, 0x3e47, 0x4b7e, {0xa8, 0x93, 0x23, 0xdc, 0x7b, 0xaa, 0x61, 0x04}};
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using HwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CoreFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
CreateFn original_create = nullptr;
HwndFn original_hwnd = nullptr;
CoreFn original_core = nullptr;
CompositionFn original_composition = nullptr;

void remember_queue(IUnknown* device, IDXGISwapChain* chain)
{
    ComPtr<ID3D12CommandQueue> queue;
    if (device && chain && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))) &&
        queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        chain->SetPrivateDataInterface(queue_tag, queue.Get());
}
HRESULT STDMETHODCALLTYPE create_chain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
{
    HRESULT hr = original_create(factory, device, desc, chain);
    if (SUCCEEDED(hr) && chain) remember_queue(device, *chain);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** chain)
{
    HRESULT hr = original_hwnd(factory, device, hwnd, desc, fullscreen, output, chain);
    if (SUCCEEDED(hr) && chain) remember_queue(device, *chain);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_core(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** chain)
{
    HRESULT hr = original_core(factory, device, window, desc, output, chain);
    if (SUCCEEDED(hr) && chain) remember_queue(device, *chain);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_composition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** chain)
{
    HRESULT hr = original_composition(factory, device, desc, output, chain);
    if (SUCCEEDED(hr) && chain) remember_queue(device, *chain);
    return hr;
}
}

void StarOverlay::hook_dx12_factory(IDXGIFactory* factory)
{
    auto install = [](void* target, void* hook, void** original) {
        if (*original) return;
        if (MH_CreateHook(target, hook, original) == MH_OK && MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            *original = nullptr;
        }
    };
    void** vt = *(void***)factory;
    install(vt[10], (void*)&create_chain, (void**)&original_create);
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        void** vt2 = *(void***)factory2.Get();
        install(vt2[15], (void*)&create_hwnd, (void**)&original_hwnd);
        install(vt2[16], (void*)&create_core, (void**)&original_core);
        install(vt2[24], (void*)&create_composition, (void**)&original_composition);
    }
}

void StarOverlay::update_dx12_queue(IDXGISwapChain* chain, IUnknown* const* queues)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || !desc.BufferCount) return;
    chain->SetPrivateDataInterface(queue_tag, nullptr);
    for (UINT i = 1; i < desc.BufferCount; ++i)
        if (queues[i] != queues[0]) return; // Multiple presenting queues need per-buffer ownership.
    remember_queue(queues[0], chain);
}

void StarOverlay::try_init_dx12(IDXGISwapChain* chain)
{
    ComPtr<ID3D12CommandQueue> queue;
    UINT size = sizeof(ID3D12CommandQueue*);
    chain->GetPrivateData(queue_tag, &size, queue.GetAddressOf());
    if (!queue) {
        if (auto* interfaces = unity_interfaces_.load()) {
            if (auto* unity = interfaces->Get<IUnityGraphicsD3D12v7>(); unity && unity->GetSwapChain() == chain) {
                queue = unity->GetCommandQueue();
                if (queue) remember_queue(queue.Get(), chain);
            }
        }
    }
    if (!queue) {
        if (!dx12_missing_queue_tick_) dx12_missing_queue_tick_ = GetTickCount64();
        if (GetTickCount64() - dx12_missing_queue_tick_ >= 2000 && Settings::get().overlay_mode != "hook") {
            mode_ = OverlayMode::External;
            STAR_LOG("DX12: using external overlay; presenting queue was created before hooks");
            start_external_thread();
        }
        return;
    }
    dx12_missing_queue_tick_ = 0;
    ComPtr<ID3D12Device> queue_device, chain_device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) ||
        FAILED(chain->GetDevice(IID_PPV_ARGS(&chain_device))) ||
        queue_device.Get() != chain_device.Get()) return;
    init_imgui_dx12(chain, chain_device.Get(), queue.Get());
}
#endif

#ifdef _WIN64
void StarOverlay::maybe_capture_dx12(IDXGISwapChain* chain)
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !enabled_ || mode_ == OverlayMode::External || chain != dx12_chain_) return;
    if (!screenshots_.consume()) return;
    if (!enabled_) return;
    // Render is off (hostile titles): read the composed desktop instead of
    // touching the game's buffers at all.
    if (!Settings::get().overlay_dx12_render) {
        capture_desktop_duplication();
        return;
    }
    if (!imgui_initialized_ || active_api_ != GraphicsAPI::DX12) return;
    auto* dev = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    if (!dev || !queue) return;
    IDXGISwapChain3* chain3 = nullptr;
    UINT bi = 0;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&chain3))) {
        bi = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    } else {
        return;
    }
    if (bi >= dx12_resources_.size()) return;
    auto* resource = (ID3D12Resource*)dx12_resources_[bi];
    D3D12_RESOURCE_DESC rd = resource->GetDesc();
    bool bgra = (rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra) {
        STAR_LOG("Screenshot: unsupported DX12 format %u", (unsigned)rd.Format);
        return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowsize = 0, total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowsize, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) || !readback)
        return;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT ok = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(ok)) ok = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    if (FAILED(ok) || !list) return;
    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = resource;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b1 = b0;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b1);
    if (FAILED(list->Close()) ||
        !star_dx12::submit_and_wait(dev, queue, list.Get(), {alloc.Get(), readback.Get(), resource})) return;
    void* mapped = nullptr;
    D3D12_RANGE range{};
    range.Begin = 0; range.End = (SIZE_T)total;
    if (SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped) {
        UINT w = (UINT)rd.Width, h = rd.Height;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        copy_pixels32(rgba.data(), (size_t)w * 4, (const uint8_t*)mapped + fp.Offset,
                      fp.Footprint.RowPitch, w, h, bgra);
        D3D12_RANGE empty{};
        empty.Begin = 0; empty.End = 0;
        readback->Unmap(0, &empty);
        std::string path = ScreenshotService::next_path();
        screenshots_.save_async(path, std::move(rgba), (int)w, (int)h);
    }
}
#endif

#ifdef _WIN64
void StarOverlay::init_imgui_dx12(IDXGISwapChain* chain, void* device, void* command_queue)
{
    auto* dev = (ID3D12Device*)device;
    auto* queue = (ID3D12CommandQueue*)command_queue;
    DXGI_SWAP_CHAIN_DESC sd{};
    if (imgui_initialized_ || FAILED(chain->GetDesc(&sd)) || !sd.BufferCount) return;
    hook_window_for(sd.OutputWindow);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = sd.BufferCount;
    ComPtr<ID3D12DescriptorHeap> rtv, srv;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)))) return;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 257;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv)))) return;

    std::vector<ComPtr<ID3D12Resource>> resources(sd.BufferCount);
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(sd.BufferCount);
    auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
    UINT stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < sd.BufferCount; ++i) {
        if (FAILED(chain->GetBuffer(i, IID_PPV_ARGS(&resources[i]))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocators[i])))) return;
        dev->CreateRenderTargetView(resources[i].Get(), nullptr, handle);
        handle.ptr += stride;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    bool platform_ready = ImGui_ImplWin32_Init(hwnd_);
    if (!platform_ready || !ImGui_ImplDX12_Init(dev, sd.BufferCount,
            sd.BufferDesc.Format, srv.Get(), srv->GetCPUDescriptorHandleForHeapStart(),
            srv->GetGPUDescriptorHandleForHeapStart())) {
        if (platform_ready) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CloseHandle(event);
        return;
    }
    style_.setup();
    dev->AddRef(); queue->AddRef();
    dx12_device_ = dev;
    dx12_command_queue_ = queue;
    dx12_rtv_heap_ = rtv.Detach();
    dx12_srv_heap_ = srv.Detach();
    dx12_command_list_ = list.Detach();
    dx12_fence_ = fence.Detach();
    dx12_fence_event_ = event;
    dx12_fence_value_ = 0;
    dx12_buffer_count_ = sd.BufferCount;
    dx12_frame_index_ = 0;
    dx12_chain_ = chain;
    dx12_command_allocators_.resize(sd.BufferCount);
    dx12_resources_.resize(sd.BufferCount);
    for (UINT i = 0; i < sd.BufferCount; ++i) {
        dx12_command_allocators_[i] = allocators[i].Detach();
        dx12_resources_[i] = resources[i].Detach();
    }
    dx12_frame_fence_.assign(sd.BufferCount, 0);
    dx12_srv_next_slot_ = 1;
    imgui_initialized_ = true;
    active_api_ = GraphicsAPI::DX12;
    STAR_LOG("ImGui ready (DX12) buffers=%u", sd.BufferCount);
}

ImTextureID StarOverlay::upload_icon_dx12(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* dev   = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* heap  = (ID3D12DescriptorHeap*)dx12_srv_heap_;
    if (!dev || !queue || !heap || (dx12_srv_next_slot_ >= 257 && dx12_free_icon_slots_.empty())) return nullptr;

    UINT row_pitch     = (UINT)(w * 4);
    UINT aligned_pitch = (row_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 upload_size = (UINT64)aligned_pitch * h;

    D3D12_HEAP_PROPERTIES upload_props = {};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension  = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width      = upload_size;
    buf_desc.Height     = buf_desc.DepthOrArraySize = buf_desc.MipLevels = 1;
    buf_desc.Format     = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout     = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload_buf;
    if (FAILED(dev->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE,
                                            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&upload_buf))))
        return nullptr;

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped))) return nullptr;
    for (int row = 0; row < h; row++)
        memcpy((uint8_t*)mapped + (size_t)row * aligned_pitch, rgba.data() + (size_t)row * row_pitch, row_pitch);
    upload_buf->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES default_props = {};
    default_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = (UINT64)w;
    tex_desc.Height             = (UINT)h;
    tex_desc.DepthOrArraySize   = tex_desc.MipLevels = 1;
    tex_desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> texture;
    if (FAILED(dev->CreateCommittedResource(&default_props, D3D12_HEAP_FLAG_NONE,
                                            &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&texture)))) return nullptr;

    ComPtr<ID3D12CommandAllocator> tmp_alloc;
    ComPtr<ID3D12GraphicsCommandList> tmp_list;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmp_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmp_alloc.Get(), nullptr, IID_PPV_ARGS(&tmp_list)))) return nullptr;

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource        = texture.Get();
    dst_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource                            = upload_buf.Get();
    src_loc.Type                                 = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Footprint.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width      = (UINT)w;
    src_loc.PlacedFootprint.Footprint.Height     = (UINT)h;
    src_loc.PlacedFootprint.Footprint.Depth      = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch   = aligned_pitch;

    tmp_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tmp_list->ResourceBarrier(1, &barrier);
    bool uploaded = SUCCEEDED(tmp_list->Close()) &&
        star_dx12::submit_and_wait(dev, queue, tmp_list.Get(), {tmp_alloc.Get(), upload_buf.Get(), texture.Get()}, false);
    if (!uploaded) return nullptr;

    UINT desc_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    UINT slot = dx12_free_icon_slots_.empty() ? dx12_srv_next_slot_++ : dx12_free_icon_slots_.back();
    if (!dx12_free_icon_slots_.empty()) dx12_free_icon_slots_.pop_back();
    cpu.ptr += (UINT64)slot * desc_inc;
    gpu.ptr += (UINT64)slot * desc_inc;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels       = 1;
    dev->CreateShaderResourceView(texture.Get(), &srv_desc, cpu);

    if (dx12_icon_resources_.size() <= slot) dx12_icon_resources_.resize(slot + 1);
    dx12_icon_resources_[slot] = texture.Detach();
    return (ImTextureID)(void*)(UINT64)gpu.ptr;
}

void StarOverlay::release_icon_dx12(ImTextureID texture)
{
    auto* device = (ID3D12Device*)dx12_device_;
    auto* heap = (ID3D12DescriptorHeap*)dx12_srv_heap_;
    if (!device || !heap) return;
    auto base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    size_t slot = ((UINT64)(uintptr_t)texture - base) / step;
    if (slot >= dx12_icon_resources_.size() || !dx12_icon_resources_[slot]) return;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* resource = (ID3D12Resource*)dx12_icon_resources_[slot];
    dx12_icon_resources_[slot] = nullptr;
    dx12_free_icon_slots_.push_back((UINT)slot);
    if (!queue || FAILED(device->GetDeviceRemovedReason())) { resource->Release(); return; }
    // Retire without blocking: the queue signals this fence after all prior
    // work (including the draws that referenced the icon); reap_submissions
    // releases the resource once it completes.
    auto& pending = star_dx12::pending_submissions();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.reap();
    auto submission = std::make_unique<star_dx12::Submission>();
    submission->device = device;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission->fence)))) {
        resource->Release();
        return;
    }
    submission->objects.emplace_back(resource);
    queue->Signal(submission->fence.Get(), 1);
    pending.items.push_back(std::move(submission));
}

void StarOverlay::render_frame_dx12(IDXGISwapChain* chain)
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    star_dx12::reap_submissions();
    if (!enabled_ || mode_ == OverlayMode::External || !imgui_initialized_ || chain != dx12_chain_) return;
    if (!Settings::get().overlay_dx12_render || active_api_ != GraphicsAPI::DX12) return;

    IDXGISwapChain3* chain3 = nullptr;
    UINT backbuffer_index = 0;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&chain3))) {
        backbuffer_index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    } else {
        return;
    }

    auto* dev = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* cmd_list = (ID3D12GraphicsCommandList*)dx12_command_list_;
    auto* fence = (ID3D12Fence*)dx12_fence_;
    auto fence_event = (HANDLE)dx12_fence_event_;
    if (backbuffer_index >= dx12_command_allocators_.size() ||
        backbuffer_index >= dx12_resources_.size() ||
        backbuffer_index >= dx12_frame_fence_.size() ||
        !fence || !fence_event) {
        return;
    }
    UINT frame_index = dx12_frame_index_;
    auto* allocator = (ID3D12CommandAllocator*)dx12_command_allocators_[frame_index];
    auto* resource = (ID3D12Resource*)dx12_resources_[backbuffer_index];
    auto* rtv_heap = (ID3D12DescriptorHeap*)dx12_rtv_heap_;
    auto* srv_heap = (ID3D12DescriptorHeap*)dx12_srv_heap_;

    // Wait until the GPU finished the previous frame recorded with this
    // buffer's allocator. Timeout+skip (instead of hang) on device loss.
    uint64_t wait_value = dx12_frame_fence_[frame_index];
    if (wait_value != 0 && fence->GetCompletedValue() < wait_value) {
        if (FAILED(fence->SetEventOnCompletion(wait_value, fence_event)) ||
            WaitForSingleObject(fence_event, 1000) != WAIT_OBJECT_0) {
            static bool logged_timeout = false;
            if (!logged_timeout) {
                logged_timeout = true;
                STAR_LOG("DX12 frame fence timeout, skipping frames until GPU recovers");
            }
            if (++dx12_timeout_streak_ >= 30) {
                dx12_timeout_streak_ = 0;
                switch_to_external("DX12 frame fence never completes");
                return;
            }
            return;
        }
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();
    if (ImGui::GetDrawData()->DisplaySize.x <= 0 || ImGui::GetDrawData()->DisplaySize.y <= 0) return;

    // NOTE: on engines with unexpected backbuffer state (e.g. ACEVO), ANY
    // transition of their buffer faults the GPU, so there is a dx12_render
    // escape hatch in overlay.star. When enabled we assume stock flip-model
    // PRESENT state here, like the stock ImGui DX12 example does.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    {
        HRESULT r1 = allocator->Reset();
        if (FAILED(r1)) {
            STAR_LOG("DX12 allocator Reset failed hr=0x%08x removed=0x%08x",
                (unsigned)r1, (unsigned)dev->GetDeviceRemovedReason());
            return;
        }
        HRESULT r2 = cmd_list->Reset(allocator, nullptr);
        if (FAILED(r2)) {
            STAR_LOG("DX12 list Reset failed hr=0x%08x removed=0x%08x",
                (unsigned)r2, (unsigned)dev->GetDeviceRemovedReason());
            return;
        }
    }
    cmd_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    UINT rtv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_handle.ptr += backbuffer_index * rtv_descriptor_size;
    cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    cmd_list->SetDescriptorHeaps(1, &srv_heap);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_list->ResourceBarrier(1, &barrier);

    HRESULT close_hr = cmd_list->Close();
    if (FAILED(close_hr)) {
        STAR_LOG("DX12 list Close failed hr=0x%08x removed=0x%08x",
            (unsigned)close_hr, (unsigned)dev->GetDeviceRemovedReason());
        return;
    }

    ID3D12CommandList* lists[] = { cmd_list };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence, ++dx12_fence_value_))) {
        STAR_LOG("DX12 frame fence signal failed");
        return;
    }
    dx12_frame_fence_[frame_index] = dx12_fence_value_;
    dx12_frame_index_ = (frame_index + 1) % dx12_buffer_count_;
    dx12_timeout_streak_ = 0;
    {
        static bool logged_first = false;
        if (!logged_first) {
            logged_first = true;
            HRESULT removed = dev->GetDeviceRemovedReason();
            STAR_LOG("DX12 first frame submitted (removed=0x%08x)", (unsigned)removed);
            if (FAILED(removed))
                switch_to_external("DX12 device removed after first submit");
        }
    }
}

bool StarOverlay::wait_dx12_idle()
{
    auto* fence = (ID3D12Fence*)dx12_fence_;
    auto* dev = (ID3D12Device*)dx12_device_;
    if (!fence || !dev || FAILED(dev->GetDeviceRemovedReason()) ||
        fence->GetCompletedValue() >= dx12_fence_value_) return true;
    return dx12_fence_event_ &&
        SUCCEEDED(fence->SetEventOnCompletion(dx12_fence_value_, (HANDLE)dx12_fence_event_)) &&
        WaitForSingleObject((HANDLE)dx12_fence_event_, 1000) == WAIT_OBJECT_0 &&
        fence->GetCompletedValue() >= dx12_fence_value_;
}

void StarOverlay::retire_dx12_renderer()
{
    // A timeout does not mean the GPU stopped using the renderer. Retain it
    // until its fence completes/device is removed while external mode proceeds.
    auto retired = std::make_unique<star_dx12::Submission>();
    retired->device = (ID3D12Device*)dx12_device_;
    retired->fence = (ID3D12Fence*)dx12_fence_;
    retired->completion_value = dx12_fence_value_;
    retired->event = (HANDLE)dx12_fence_event_;
    dx12_fence_event_ = nullptr;
    auto retain = [&](void*& object) {
        if (object) {
            ComPtr<IUnknown> owner;
            owner.Attach((IUnknown*)object);
            retired->objects.push_back(std::move(owner));
            object = nullptr;
        }
    };
    for (auto*& object : dx12_resources_) retain(object);
    for (auto*& object : dx12_command_allocators_) retain(object);
    for (auto*& object : dx12_icon_resources_) retain(object);
    retain(dx12_device_); retain(dx12_command_queue_); retain(dx12_command_list_);
    retain(dx12_rtv_heap_); retain(dx12_srv_heap_); retain(dx12_fence_);
    ImGuiContext* context = ImGui::GetCurrentContext();
    retired->on_complete = [context] {
        ImGuiContext* current = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(current);
    };
    ImGui::SetCurrentContext(nullptr);
    imgui_initialized_ = false;
    icons_.clear();
    auto& pending = star_dx12::pending_submissions();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.items.push_back(std::move(retired));
    STAR_LOG("DX12 renderer retained until pending GPU work completes");
}

void StarOverlay::cleanup_dx12()
{
    star_dx12::reap_submissions();
    dx12_chain_ = nullptr;
    dx12_frame_index_ = 0;
    for (auto* res : dx12_icon_resources_) if (res) ((ID3D12Resource*)res)->Release();
    dx12_icon_resources_.clear();
    dx12_free_icon_slots_.clear();
    dx12_srv_next_slot_ = 1;
    dx12_frame_fence_.clear();
    dx12_fence_value_ = 0;
    if (dx12_fence_event_) { CloseHandle((HANDLE)dx12_fence_event_); dx12_fence_event_ = nullptr; }
    if (dx12_fence_) { ((ID3D12Fence*)dx12_fence_)->Release(); dx12_fence_ = nullptr; }

    for (auto* res : dx12_resources_) if (res) ((ID3D12Resource*)res)->Release();
    dx12_resources_.clear();
    for (auto* alloc : dx12_command_allocators_) if (alloc) ((ID3D12CommandAllocator*)alloc)->Release();
    dx12_command_allocators_.clear();
    if (dx12_command_list_) { ((ID3D12GraphicsCommandList*)dx12_command_list_)->Release(); dx12_command_list_ = nullptr; }
    if (dx12_rtv_heap_) { ((ID3D12DescriptorHeap*)dx12_rtv_heap_)->Release(); dx12_rtv_heap_ = nullptr; }
    if (dx12_srv_heap_) { ((ID3D12DescriptorHeap*)dx12_srv_heap_)->Release(); dx12_srv_heap_ = nullptr; }
    if (dx12_command_queue_) { ((ID3D12CommandQueue*)dx12_command_queue_)->Release(); dx12_command_queue_ = nullptr; }
    if (dx12_device_) { ((ID3D12Device*)dx12_device_)->Release(); dx12_device_ = nullptr; }
}
#endif
