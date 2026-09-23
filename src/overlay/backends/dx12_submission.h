#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

namespace star_dx12 {
using Microsoft::WRL::ComPtr;

// Keep submitted objects alive even when a driver fails to signal its fence.
// Reap timed-out submissions on subsequent uploads/captures.
struct Submission {
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Device> device;
    std::vector<ComPtr<IUnknown>> objects;
    HANDLE event = nullptr;
    uint64_t completion_value = 1;
    std::function<void()> on_complete;
    ~Submission() { if (on_complete) on_complete(); if (event) CloseHandle(event); }
};
struct PendingSubmissions {
    std::vector<std::unique_ptr<Submission>> items;
    std::mutex mutex;
    void reap() {
        for (auto it = items.begin(); it != items.end();) {
            if ((*it)->fence->GetCompletedValue() >= (*it)->completion_value ||
                FAILED((*it)->device->GetDeviceRemovedReason())) it = items.erase(it);
            else ++it;
        }
    }
};
inline PendingSubmissions& pending_submissions() {
    static auto* pending = new PendingSubmissions;
    return *pending;
}
inline void reap_submissions() {
    auto& pending = pending_submissions();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.reap();
}
inline bool submit_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* list, std::initializer_list<IUnknown*> objects, bool wait = true)
{
    auto& pending = pending_submissions();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.reap();
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
    if (!wait && SUCCEEDED(hr)) {
        pending.items.push_back(std::move(submission));
        return true; // Queue ordering makes the texture available to the next draw.
    }
    HANDLE event = submission->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && event) {
        hr = submission->fence->SetEventOnCompletion(1, event);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, 3000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    } else if (!event) hr = E_OUTOFMEMORY;
    if (FAILED(hr) || FAILED(device->GetDeviceRemovedReason())) {
        // Signal failure does not prove that execution has stopped.
        pending.items.push_back(std::move(submission));
        return false;
    }
    return true;
}
}
