#include "overlay/overlay_internal.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include <d3d9.h>
#include <d3d10.h>

ImTextureID StarOverlay::get_or_create_icon(
    const std::string& key, const std::vector<uint8_t>& rgba, int w, int h)
{
    return icons_.get_or_create(key, [&]() -> ImTextureID {
        if (rgba.empty() || w <= 0 || h <= 0) return nullptr;
#ifdef _WIN64
        if (active_api_ == GraphicsAPI::DX12) {
            return upload_icon_dx12(rgba, w, h);
        }
#endif
        if (active_api_ == GraphicsAPI::Vulkan) {
            return upload_icon_vulkan(rgba, w, h);
        } else if (active_api_ == GraphicsAPI::DX9) {
            return upload_icon_dx9(rgba, w, h);
        } else if (active_api_ == GraphicsAPI::DX10) {
            return upload_icon_dx10(rgba, w, h);
        } else if (active_api_ == GraphicsAPI::OpenGL) {
            return upload_icon_opengl(rgba, w, h);
        } else if (device_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{ rgba.data(), (UINT)(w*4), 0 };
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&td, &srd, &tex))) {
                ID3D11ShaderResourceView* srv = nullptr;
                device_->CreateShaderResourceView(tex, nullptr, &srv);
                tex->Release();
                return (ImTextureID)(void*)srv;
            }
        }
        return nullptr;
    });
}

ImTextureID StarOverlay::upload_icon_dx9(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!dx9_device_) return nullptr;
    // MANAGED pool: survives Reset, no invalidate needed. ImGui DX9 backend
    // draws IDirect3DTexture9* directly.
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dx9_device_->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
        return nullptr;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0))) {
        // WIC hands us RGBA bytes; D3DFMT_A8R8G8B8 stores BGRA in memory,
        // so swizzle R<->B (otherwise browns render blue/purple).
        const uint8_t* src = rgba.data();
        uint8_t* dst = (uint8_t*)locked.pBits;
        UINT row_bytes = (UINT)w * 4;
        for (int y = 0; y < h; y++) {
            const uint8_t* s = src + (size_t)y * row_bytes;
            uint8_t* d = dst + (size_t)y * locked.Pitch;
            for (int x = 0; x < w; x++) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                s += 4; d += 4;
            }
        }
        tex->UnlockRect(0);
        return (ImTextureID)(void*)tex;
    }
    tex->Release();
    return nullptr;
}

ImTextureID StarOverlay::upload_icon_dx10(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!dx10_device_) return nullptr;
    D3D10_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT; td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    D3D10_SUBRESOURCE_DATA srd{ rgba.data(), (UINT)(w * 4), 0 };
    ID3D10Texture2D* tex = nullptr;
    if (FAILED(dx10_device_->CreateTexture2D(&td, &srd, &tex)) || !tex) return nullptr;
    ID3D10ShaderResourceView* srv = nullptr;
    dx10_device_->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return (ImTextureID)(void*)srv;
}

ImTextureID StarOverlay::upload_icon_opengl(const std::vector<uint8_t>& rgba, int w, int h)
{
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) return nullptr;
    typedef void(WINAPI* glGenTexturesFn)(int, unsigned int*);
    typedef void(WINAPI* glBindTextureFn)(unsigned int, unsigned int);
    typedef void(WINAPI* glTexParameteriFn)(unsigned int, unsigned int, int);
    typedef void(WINAPI* glTexImage2DFn)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
    auto glGenTextures = (glGenTexturesFn)GetProcAddress(opengl_dll, "glGenTextures");
    auto glBindTexture = (glBindTextureFn)GetProcAddress(opengl_dll, "glBindTexture");
    auto glTexParameteri = (glTexParameteriFn)GetProcAddress(opengl_dll, "glTexParameteri");
    auto glTexImage2D = (glTexImage2DFn)GetProcAddress(opengl_dll, "glTexImage2D");
    if (!glGenTextures || !glBindTexture || !glTexParameteri || !glTexImage2D) return nullptr;

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return nullptr;
    const unsigned int GL_TEXTURE_2D = 0x0DE1;
    const unsigned int GL_RGBA = 0x1908;
    const unsigned int GL_UNSIGNED_BYTE = 0x1401;
    const unsigned int GL_TEXTURE_MIN_FILTER = 0x2801;
    const unsigned int GL_TEXTURE_MAG_FILTER = 0x2800;
    const int GL_LINEAR = 0x2601;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, (int)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    icons_.add_gl_texture(tex);
    return (ImTextureID)(void*)(uintptr_t)tex;
}

void StarOverlay::enqueue_icon_decode(const std::string& path, const std::string& key,
                                      const std::string& toast_title)
{
    if (path.empty() || key.empty()) return;
    {
        std::lock_guard<std::mutex> lock(icon_decode_mutex_);
        if (!icon_decode_pending_.insert(key).second) return;
        icon_decode_queue_.push_back({path, key, toast_title});
    }
    icon_decode_cv_.notify_one();
}

void StarOverlay::icon_decode_worker()
{
    for (;;) {
        IconDecodeRequest req;
        {
            std::unique_lock<std::mutex> lock(icon_decode_mutex_);
            icon_decode_cv_.wait(lock, [&] {
                return icon_decode_stop_ || !icon_decode_queue_.empty();
            });
            if (icon_decode_stop_ && icon_decode_queue_.empty()) break;
            req = std::move(icon_decode_queue_.front());
            icon_decode_queue_.pop_front();
        }
        IconDecodeResult res;
        res.key = req.key;
        res.toast_title = req.toast_title;
        StarSteamUtils::get().LoadIconFile(req.path, res.rgba, res.w, res.h);
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_ready_.push_back(std::move(res));
        }
    }
}

void StarOverlay::drain_icon_decodes()
{
    std::vector<IconDecodeResult> ready;
    {
        std::lock_guard<std::mutex> lock(icon_decode_mutex_);
        ready.swap(icon_decode_ready_);
    }
    for (auto& r : ready) {
        if (r.w > 0 && r.h > 0 && !r.rgba.empty())
            get_or_create_icon(r.key, r.rgba, r.w, r.h);
        if (!r.toast_title.empty())
            notifications_.attach_icon(r.toast_title, std::move(r.rgba), r.w, r.h);
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_pending_.erase(r.key);
        }
    }
}

