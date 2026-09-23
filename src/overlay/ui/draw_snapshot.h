#pragma once
#include "imgui.h"
#include <cstring>
#include <vector>

// Compare small draw streams before doing a full-window GPU readback.
class DrawSnapshot {
public:
    void clear() { previous_.clear(); }
    bool changed(const ImDrawData* data) {
        next_.clear();
        append(&data->DisplayPos, sizeof(data->DisplayPos));
        append(&data->DisplaySize, sizeof(data->DisplaySize));
        append(&data->FramebufferScale, sizeof(data->FramebufferScale));
        bool callback = false;
        for (int i = 0; i < data->CmdListsCount; ++i) {
            const auto* list = data->CmdLists[i];
            append(&list->CmdBuffer.Size, sizeof(int));
            append(&list->VtxBuffer.Size, sizeof(int));
            append(&list->IdxBuffer.Size, sizeof(int));
            append(list->CmdBuffer.Data, list->CmdBuffer.size_in_bytes());
            append(list->VtxBuffer.Data, list->VtxBuffer.size_in_bytes());
            append(list->IdxBuffer.Data, list->IdxBuffer.size_in_bytes());
            for (const auto& cmd : list->CmdBuffer) callback |= cmd.UserCallback != nullptr;
        }
        bool different = callback || next_ != previous_;
        previous_.swap(next_);
        return different;
    }
private:
    void append(const void* data, size_t size) {
        if (!size) return;
        auto* bytes = static_cast<const unsigned char*>(data);
        next_.insert(next_.end(), bytes, bytes + size);
    }
    std::vector<unsigned char> previous_, next_;
};
