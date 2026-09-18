#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"

// Owns uploaded ImGui texture ids keyed by a string, plus the OpenGL texture
// handles owned by the game's GL context. Upload/creation is deferred to a
// caller-supplied callback so the cache stays independent of any graphics API.
class IconCache {
public:
    using UploadFn = std::function<ImTextureID()>;

    ImTextureID get_or_create(const std::string& key, const UploadFn& upload);
    bool        contains(const std::string& key) const;
    ImTextureID find(const std::string& key) const;
    ImTextureID take(const std::string& key);
    void        add_gl_texture(unsigned int tex);
    void        clear_gl();
    void        clear();

    template <class ReleaseFn>
    void release_all(ReleaseFn release) {
        for (auto& kv : textures_) if (kv.second) release(kv.second);
        textures_.clear();
    }

private:
    std::unordered_map<std::string, ImTextureID> textures_;
    std::vector<unsigned int>                    gl_textures_;
};
