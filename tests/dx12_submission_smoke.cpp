#ifdef NDEBUG
#undef NDEBUG
#endif
#include "overlay/backends/dx12_submission.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <cassert>
#include <cstdio>
#include <memory>
using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 77;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    assert(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload, readback;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    assert(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
    void* mapped = nullptr;
    assert(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
    *(unsigned*)mapped = 0x12345678;
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 4);
    assert(SUCCEEDED(list->Close()));
    ComPtr<ID3D12Fence> gate;
    assert(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    assert(SUCCEEDED(queue->Wait(gate.Get(), 1))); // Hold the GPU while the caller submits.
    assert(star_dx12::submit_and_wait(device.Get(), queue.Get(), list.Get(),
        {allocator.Get(), upload.Get(), readback.Get()}, false));
    upload.Reset(); allocator.Reset(); list.Reset();
    star_dx12::reap_submissions();
    auto& pending = star_dx12::pending_submissions();
    assert(pending.items.size() == 1); // Submitted objects survive the caller releasing them.
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    assert(event);
    assert(SUCCEEDED(pending.items[0]->fence->SetEventOnCompletion(1, event)));
    assert(SUCCEEDED(gate->Signal(1)));
    assert(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    CloseHandle(event);
    star_dx12::reap_submissions();
    assert(pending.items.empty());
    assert(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    assert(*(unsigned*)mapped == 0x12345678);
    readback->Unmap(0, nullptr);
    // Retiring a renderer must wait for its actual last frame, not fence 1.
    auto retired = std::make_unique<star_dx12::Submission>();
    retired->device = device;
    retired->fence = gate;
    retired->completion_value = 7;
    bool cleaned = false;
    retired->on_complete = [&] { cleaned = true; };
    pending.items.push_back(std::move(retired));
    assert(SUCCEEDED(gate->Signal(3)));
    star_dx12::reap_submissions();
    assert(!cleaned && pending.items.size() == 1);
    assert(SUCCEEDED(gate->Signal(7)));
    star_dx12::reap_submissions();
    assert(cleaned && pending.items.empty());
    std::puts("DX12 asynchronous submission smoke passed");
}
