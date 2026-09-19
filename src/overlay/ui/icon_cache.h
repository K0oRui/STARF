#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include "imgui.h"

// Owns uploaded ImGui texture ids keyed by a string. Upload/creation is deferred to a
// caller-supplied callback so the cache stays independent of any graphics API.
class IconCache {
public:
    using UploadFn = std::function<ImTextureID()>;

    ImTextureID get_or_create(const std::string& key, const UploadFn& upload);
    bool        contains(const std::string& key) const;
    ImTextureID find(const std::string& key) const;
    ImTextureID take(const std::string& key);
    void        clear();

    template <class ReleaseFn>
    void release_all(ReleaseFn release) {
        for (auto& kv : textures_) if (kv.second) release(kv.second);
        textures_.clear();
    }

private:
    std::unordered_map<std::string, ImTextureID> textures_;
};
