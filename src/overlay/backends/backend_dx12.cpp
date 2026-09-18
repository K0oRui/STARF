#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/callbacks.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d12.h>
#include <cmath>
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <vulkan/vulkan.h>
#include <cctype>
#include <ctime>
#include <algorithm>

#ifdef _WIN64
void* StarOverlay::g_dx12_captured_queue_ = nullptr;
#endif

#ifdef _WIN64
void StarOverlay::hook_dx12_ecl()
{
    if (orig_execute_command_lists_) return;

    typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) hD3D12 = LoadLibraryA("d3d12.dll");
    if (!hD3D12) { STAR_LOG("DX12 ECL: d3d12.dll not found"); return; }

    auto pfnCreate = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pfnCreate) { STAR_LOG("DX12 ECL: D3D12CreateDevice export not found"); return; }

    ID3D12Device* dummy_dev = nullptr;
    HRESULT hr = pfnCreate(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummy_dev));
    if (FAILED(hr)) { STAR_LOG("DX12 ECL: D3D12CreateDevice failed hr=0x%08x", (unsigned)hr); return; }

    D3D12_COMMAND_QUEUE_DESC cqdesc = {};
    cqdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummy_queue = nullptr;
    if (SUCCEEDED(dummy_dev->CreateCommandQueue(&cqdesc, IID_PPV_ARGS(&dummy_queue)))) {
        void** vt12 = *(void***)dummy_queue;
        MH_STATUS mh = MH_CreateHook(vt12[10], &hooked_ExecuteCommandLists, (void**)&orig_execute_command_lists_);
        if (mh == MH_OK) {
            MH_EnableHook(vt12[10]);
            STAR_LOG("DX12 ExecuteCommandLists hooked");
        } else {
            STAR_LOG("DX12 ECL: MH_CreateHook failed status=%d", (int)mh);
        }
        dummy_queue->Release();
    } else {
        STAR_LOG("DX12 ECL: CreateCommandQueue failed");
    }
    dummy_dev->Release();
}
#endif

#ifdef _WIN64
void StarOverlay::maybe_capture_dx12(IDXGISwapChain* chain)
{
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
    ID3D12Resource* readback = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) || !readback)
        return;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT ok = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(ok)) ok = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    if (FAILED(ok) || !list) {
        if (alloc) alloc->Release();
        readback->Release();
        return;
    }
    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = resource;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
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
    list->Close();
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence) {
        if (SUCCEEDED(queue->Signal(fence, 1)) && fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 3000);
                CloseHandle(ev);
            }
        }
        fence->Release();
    }
    void* mapped = nullptr;
    D3D12_RANGE range{};
    range.Begin = 0; range.End = (SIZE_T)total;
    if (SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped) {
        UINT w = (UINT)rd.Width, h = rd.Height;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++) {
            const uint8_t* s = (const uint8_t*)mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
            uint8_t* d = rgba.data() + (size_t)y * w * 4;
            if (bgra) {
                for (UINT x = 0; x < w; x++) {
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    s += 4; d += 4;
                }
            } else {
                memcpy(d, s, (size_t)w * 4);
            }
        }
        D3D12_RANGE empty{};
        empty.Begin = 0; empty.End = 0;
        readback->Unmap(0, &empty);
        std::string path = ScreenshotService::next_path();
        if (!path.empty() && ScreenshotService::save_rgba_png(path, rgba.data(), (int)w, (int)h))
            notify_screenshot(path);
    }
    list->Release();
    alloc->Release();
    readback->Release();
}
#endif

#ifdef _WIN64
void STDMETHODCALLTYPE StarOverlay::hooked_ExecuteCommandLists(void* queue, UINT count, void* const* lists)
{
    // Only the DIRECT (graphics) queue can do our render-target work. Games
    // routinely execute copy/compute queues first (uploads during loading);
    // capturing one of those and issuing graphics barriers on it faults.
    if (!g_dx12_captured_queue_) {
        auto* q = (ID3D12CommandQueue*)queue;
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_dx12_captured_queue_ = queue;
            STAR_LOG("DX12 command queue captured");
        }
    }
    if (g_overlay && g_overlay->orig_execute_command_lists_)
        g_overlay->orig_execute_command_lists_(queue, count, lists);
}
#endif
#ifdef _WIN64
void StarOverlay::init_imgui_dx12(IDXGISwapChain* chain, void* device, void* command_queue)
{
    auto* dev = (ID3D12Device*)device;
    auto* queue = (ID3D12CommandQueue*)command_queue;

    DXGI_SWAP_CHAIN_DESC sd{};
    chain->GetDesc(&sd);
    hook_window_for(sd.OutputWindow);
    STAR_LOG("init_imgui_dx12: buffers=%u fmt=%u hwnd=%p", sd.BufferCount, sd.BufferDesc.Format, hwnd_);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = sd.BufferCount;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ID3D12DescriptorHeap* rtv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) { STAR_LOG("init_imgui_dx12: rtv_heap failed"); return; }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 257;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ID3D12DescriptorHeap* srv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)))) {
        STAR_LOG("init_imgui_dx12: srv_heap failed");
        rtv_heap->Release();
        return;
    }

    std::vector<ID3D12Resource*> resources(sd.BufferCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    UINT rtv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < sd.BufferCount; i++) {
        if (SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&resources[i])))) {
            dev->CreateRenderTargetView(resources[i], nullptr, rtv_handle);
            rtv_handle.ptr += rtv_descriptor_size;
        }
    }

    std::vector<ID3D12CommandAllocator*> allocators(sd.BufferCount);
    for (UINT i = 0; i < sd.BufferCount; i++) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])))) { STAR_LOG("init_imgui_dx12: allocator[%u] failed", i); return; }
    }

    ID3D12GraphicsCommandList* cmd_list = nullptr;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&cmd_list)))) { STAR_LOG("init_imgui_dx12: cmd_list failed"); return; }
    cmd_list->Close();

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd_);
    hook_window();

    IMGUI_CHECKVERSION();
    STAR_LOG("init_imgui_dx12: calling ImGui_ImplDX12_Init");
    if (ImGui_ImplDX12_Init(dev, sd.BufferCount, sd.BufferDesc.Format, srv_heap,
                            srv_heap->GetCPUDescriptorHandleForHeapStart(),
                            srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        style_.setup();
        dev->AddRef();
        queue->AddRef();
        dx12_device_ = dev;
        dx12_command_queue_ = queue;
        dx12_rtv_heap_ = rtv_heap;
        dx12_srv_heap_ = srv_heap;
        dx12_command_list_ = cmd_list;
        dx12_buffer_count_ = sd.BufferCount;

        dx12_command_allocators_.resize(sd.BufferCount);
        for (UINT i = 0; i < sd.BufferCount; i++) dx12_command_allocators_[i] = allocators[i];

        dx12_resources_.resize(sd.BufferCount);
        for (UINT i = 0; i < sd.BufferCount; i++) dx12_resources_[i] = resources[i];

        ID3D12Fence* frame_fence = nullptr;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence))) || !frame_fence) {
            STAR_LOG("init_imgui_dx12: frame fence failed, cleaning up");
            cleanup_dx12();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            STAR_LOG("init_imgui_dx12: fence event failed, cleaning up");
            frame_fence->Release();
            cleanup_dx12();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        dx12_fence_ = frame_fence;
        dx12_fence_value_ = 0;
        dx12_frame_fence_.assign(sd.BufferCount, 0);
        dx12_fence_event_ = fence_event;

        dx12_srv_next_slot_ = 1;
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::DX12;
        STAR_LOG("ImGui ready (DX12)");
    } else {
        STAR_LOG("init_imgui_dx12: ImGui_ImplDX12_Init FAILED - BackendRendererUserData=%p",
                 ImGui::GetIO().BackendRendererUserData);
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

ImTextureID StarOverlay::upload_icon_dx12(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* dev   = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* heap  = (ID3D12DescriptorHeap*)dx12_srv_heap_;
    if (!dev || !queue || !heap || dx12_srv_next_slot_ >= 257) return nullptr;

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

    ID3D12Resource* upload_buf = nullptr;
    if (FAILED(dev->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE,
                                            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&upload_buf))))
        return nullptr;

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped))) { upload_buf->Release(); return nullptr; }
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

    ID3D12Resource* texture = nullptr;
    if (FAILED(dev->CreateCommittedResource(&default_props, D3D12_HEAP_FLAG_NONE,
                                            &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&texture)))) {
        upload_buf->Release(); return nullptr;
    }

    ID3D12CommandAllocator*    tmp_alloc = nullptr;
    ID3D12GraphicsCommandList* tmp_list  = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmp_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmp_alloc, nullptr, IID_PPV_ARGS(&tmp_list)))) {
        if (tmp_alloc) tmp_alloc->Release();
        texture->Release(); upload_buf->Release(); return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource        = texture;
    dst_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource                            = upload_buf;
    src_loc.Type                                 = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Footprint.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width      = (UINT)w;
    src_loc.PlacedFootprint.Footprint.Height     = (UINT)h;
    src_loc.PlacedFootprint.Footprint.Depth      = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch   = aligned_pitch;

    tmp_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tmp_list->ResourceBarrier(1, &barrier);
    tmp_list->Close();

    ID3D12CommandList* lists[] = { tmp_list };
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        tmp_list->Release(); tmp_alloc->Release(); upload_buf->Release();
        texture->Release(); return nullptr;
    }
    if (FAILED(queue->Signal(fence, 1))) {
        fence->Release();
        tmp_list->Release(); tmp_alloc->Release(); upload_buf->Release();
        texture->Release(); return nullptr;
    }
    if (fence->GetCompletedValue() < 1) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
    fence->Release();
    tmp_list->Release();
    tmp_alloc->Release();
    upload_buf->Release();

    UINT desc_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (UINT64)dx12_srv_next_slot_ * desc_inc;
    gpu.ptr += (UINT64)dx12_srv_next_slot_ * desc_inc;
    dx12_srv_next_slot_++;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels       = 1;
    dev->CreateShaderResourceView(texture, &srv_desc, cpu);

    dx12_icon_resources_.push_back(texture);
    return (ImTextureID)(void*)(UINT64)gpu.ptr;
}

void StarOverlay::render_frame_dx12(IDXGISwapChain* chain)
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!imgui_initialized_) return;
    // Escape hatch: API emulation without any DX12 drawing.    if (!Settings::get().overlay_dx12_render) return;

    IDXGISwapChain3* chain3 = nullptr;
    UINT backbuffer_index = 0;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&chain3))) {
        backbuffer_index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    } else {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();

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
    auto* allocator = (ID3D12CommandAllocator*)dx12_command_allocators_[backbuffer_index];
    auto* resource = (ID3D12Resource*)dx12_resources_[backbuffer_index];
    auto* rtv_heap = (ID3D12DescriptorHeap*)dx12_rtv_heap_;
    auto* srv_heap = (ID3D12DescriptorHeap*)dx12_srv_heap_;

    // Wait until the GPU finished the previous frame recorded with this
    // buffer's allocator. Timeout+skip (instead of hang) on device loss.
    uint64_t wait_value = dx12_frame_fence_[backbuffer_index];
    if (wait_value != 0 && fence->GetCompletedValue() < wait_value) {
        fence->SetEventOnCompletion(wait_value, fence_event);
        if (WaitForSingleObject(fence_event, 1000) != WAIT_OBJECT_0) {
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
        // First-frames diagnostics: pinpoints which D3D call dies, if any.
        static int dx12_dbg = 0;
        bool dbg = dx12_dbg < 5;
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
        if (dbg) STAR_LOG("DX12 frame %d: reset ok (bi=%u)", dx12_dbg, backbuffer_index);
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
    queue->Signal(fence, ++dx12_fence_value_);
    dx12_frame_fence_[backbuffer_index] = dx12_fence_value_;
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

void StarOverlay::cleanup_dx12()
{
    for (auto* res : dx12_icon_resources_) if (res) ((ID3D12Resource*)res)->Release();
    dx12_icon_resources_.clear();
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
