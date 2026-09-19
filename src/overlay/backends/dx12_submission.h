#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>
#include <memory>

namespace star_dx12 {
using Microsoft::WRL::ComPtr;

// Keep submitted objects alive even when a driver fails to signal its fence.
// Reap timed-out submissions on subsequent uploads/captures.
struct Submission {
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Device> device;
    std::vector<ComPtr<IUnknown>> objects;
    HANDLE event = nullptr;
    ~Submission() { if (event) CloseHandle(event); }
};
inline bool submit_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* list, std::initializer_list<IUnknown*> objects)
{
    static auto* pending = new std::vector<std::unique_ptr<Submission>>;
    static auto* mutex = new std::mutex;
    std::lock_guard<std::mutex> lock(*mutex);
    for (auto it = pending->begin(); it != pending->end();) {
        if ((*it)->fence->GetCompletedValue() >= 1 ||
            FAILED((*it)->device->GetDeviceRemovedReason())) it = pending->erase(it);
        else ++it;
    }
    auto submission = std::make_unique<Submission>();
    submission->device = device;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&submission->fence)))) return false;
    submission->objects.emplace_back(list);
    submission->objects.emplace_back(queue);
    for (auto* object : objects) submission->objects.emplace_back(object);
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    HRESULT hr = queue->Signal(submission->fence.Get(), 1);
    HANDLE event = submission->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && event) {
        hr = submission->fence->SetEventOnCompletion(1, event);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, 3000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    } else if (!event) hr = E_OUTOFMEMORY;
    if (FAILED(hr) || FAILED(device->GetDeviceRemovedReason())) {
        // Signal failure does not prove that execution has stopped.
        pending->push_back(std::move(submission));
        return false;
    }
    return true;
}
}
