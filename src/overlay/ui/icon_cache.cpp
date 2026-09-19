#include "overlay/ui/icon_cache.h"

ImTextureID IconCache::get_or_create(const std::string& key, const UploadFn& upload)
{
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;

    ImTextureID tex = upload ? upload() : nullptr;
    // Notifications arrive before their asynchronous icon decode completes.
    // Cache only successful uploads so an empty first frame (or a transient
    // upload failure) cannot permanently hide the icon under this key.
    if (tex) textures_[key] = tex;
    return tex;
}

bool IconCache::contains(const std::string& key) const
{
    return textures_.find(key) != textures_.end();
}

ImTextureID IconCache::find(const std::string& key) const
{
    auto it = textures_.find(key);
    return it != textures_.end() ? it->second : nullptr;
}

ImTextureID IconCache::take(const std::string& key)
{
    auto it = textures_.find(key);
    if (it == textures_.end()) return nullptr;
    ImTextureID tex = it->second;
    textures_.erase(it);
    return tex;
}

void IconCache::add_gl_texture(unsigned int tex) { gl_textures_.push_back(tex); }
void IconCache::clear_gl() { gl_textures_.clear(); }
void IconCache::clear() { textures_.clear(); }
