// dear imgui: Renderer Backend for DirectX8
// Port of imgui_impl_dx9 to the DX8 fixed pipeline. Differences from DX9:
//  - No scissor rect: clipping is done by setting the viewport per draw cmd.
//  - No sampler states: filtering goes through D3DTSS_MINFILTER/MAGFILTER.
//  - No D3DRS_SEPARATEALPHABLENDENABLE / D3DRS_SCISSORTESTENABLE.
//  - State blocks are DWORD tokens (CreateStateBlock/Capture/Apply/Delete).
//  - SetIndices takes a BaseVertexIndex; DrawIndexedPrimitive has no
//    BaseVertexIndex (set it with SetIndices for each draw command).
//  - No D3DFMT_A8B8G8R8: textures use A8R8G8B8.
//  - Vertex/pixel shaders are DWORD handles (0 = none).

#include <algorithm>
#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_dx8.h"
#include "dx8/d3d8.h"

struct ImGui_ImplDX8_Data
{
    IDirect3DDevice8*           pd3dDevice;
    IDirect3DVertexBuffer8*     pVB;
    IDirect3DIndexBuffer8*      pIB;
    IDirect3DTexture8*          FontTexture;
    int                         VertexBufferSize;
    int                         IndexBufferSize;

    ImGui_ImplDX8_Data()        { memset((void*)this, 0, sizeof(*this)); VertexBufferSize = 5000; IndexBufferSize = 10000; }
};

struct CUSTOMVERTEX
{
    float    pos[3];
    D3DCOLOR col;
    float    uv[2];
};
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1)

#define IMGUI_COL_TO_DX8_ARGB(_COL)     (((_COL) & 0xFF00FF00) | (((_COL) & 0xFF0000) >> 16) | (((_COL) & 0xFF) << 16))

static ImGui_ImplDX8_Data* ImGui_ImplDX8_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplDX8_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

static void ImGui_ImplDX8_SetupRenderState(ImDrawData* draw_data)
{
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();

    // Setup viewport
    D3DVIEWPORT8 vp;
    vp.X = vp.Y = 0;
    vp.Width = (DWORD)draw_data->DisplaySize.x;
    vp.Height = (DWORD)draw_data->DisplaySize.y;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    bd->pd3dDevice->SetViewport(&vp);

    // Setup render state: fixed-pipeline, alpha-blending, no face culling, no
    // depth testing, shade mode (for gradient), bilinear sampling.
    bd->pd3dDevice->SetPixelShader(0);
    bd->pd3dDevice->SetVertexShader(D3DFVF_CUSTOMVERTEX);
    bd->pd3dDevice->SetStreamSource(0, bd->pVB, sizeof(CUSTOMVERTEX));
    bd->pd3dDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    bd->pd3dDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    bd->pd3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    bd->pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    bd->pd3dDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    bd->pd3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    bd->pd3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    bd->pd3dDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_CLIPPING, TRUE);
    bd->pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    bd->pd3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    bd->pd3dDevice->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    bd->pd3dDevice->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    bd->pd3dDevice->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    bd->pd3dDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    bd->pd3dDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    bd->pd3dDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

    // Setup orthographic projection matrix
    {
        float L = draw_data->DisplayPos.x + 0.5f;
        float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x + 0.5f;
        float T = draw_data->DisplayPos.y + 0.5f;
        float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y + 0.5f;
        D3DMATRIX mat_identity = { { { 1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f } } };
        D3DMATRIX mat_projection =
        { { {
            2.0f/(R-L),   0.0f,         0.0f,  0.0f,
            0.0f,         2.0f/(T-B),   0.0f,  0.0f,
            0.0f,         0.0f,         0.5f,  0.0f,
            (L+R)/(L-R),  (T+B)/(B-T),  0.5f,  1.0f
        } } };
        bd->pd3dDevice->SetTransform(D3DTS_WORLD, &mat_identity);
        bd->pd3dDevice->SetTransform(D3DTS_VIEW, &mat_identity);
        bd->pd3dDevice->SetTransform(D3DTS_PROJECTION, &mat_projection);
    }
}

void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data)
{
    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    // Create and grow buffers if needed
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    if (!bd->pVB || bd->VertexBufferSize < draw_data->TotalVtxCount)
    {
        if (bd->pVB) { bd->pVB->Release(); bd->pVB = nullptr; }
        bd->VertexBufferSize = draw_data->TotalVtxCount + 5000;
        if (bd->pd3dDevice->CreateVertexBuffer(bd->VertexBufferSize * sizeof(CUSTOMVERTEX), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &bd->pVB) < 0)
            return;
    }
    if (!bd->pIB || bd->IndexBufferSize < draw_data->TotalIdxCount)
    {
        if (bd->pIB) { bd->pIB->Release(); bd->pIB = nullptr; }
        bd->IndexBufferSize = draw_data->TotalIdxCount + 10000;
        if (bd->pd3dDevice->CreateIndexBuffer(bd->IndexBufferSize * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &bd->pIB) < 0)
            return;
    }

    // Backup the DX8 state (token-based state block)
    DWORD d3d8_state_block = 0;
    if (bd->pd3dDevice->CreateStateBlock(D3DSBT_ALL, &d3d8_state_block) < 0)
        return;
    if (bd->pd3dDevice->CaptureStateBlock(d3d8_state_block) < 0)
    {
        bd->pd3dDevice->DeleteStateBlock(d3d8_state_block);
        return;
    }

    // Backup the DX8 transform (not included in the state block)
    D3DMATRIX last_world, last_view, last_projection;
    bd->pd3dDevice->GetTransform(D3DTS_WORLD, &last_world);
    bd->pd3dDevice->GetTransform(D3DTS_VIEW, &last_view);
    bd->pd3dDevice->GetTransform(D3DTS_PROJECTION, &last_projection);

    // Allocate buffers
    CUSTOMVERTEX* vtx_dst;
    ImDrawIdx* idx_dst;
    if (bd->pVB->Lock(0, (UINT)(draw_data->TotalVtxCount * sizeof(CUSTOMVERTEX)), (BYTE**)&vtx_dst, D3DLOCK_DISCARD) < 0)
    {
        bd->pd3dDevice->DeleteStateBlock(d3d8_state_block);
        return;
    }
    if (bd->pIB->Lock(0, (UINT)(draw_data->TotalIdxCount * sizeof(ImDrawIdx)), (BYTE**)&idx_dst, D3DLOCK_DISCARD) < 0)
    {
        bd->pVB->Unlock();
        bd->pd3dDevice->DeleteStateBlock(d3d8_state_block);
        return;
    }

    // Keep indices local: SetIndices supplies the base vertex per command,
    // avoiding 16-bit overflow when the merged buffer exceeds 65535 vertices.
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_src = cmd_list->VtxBuffer.Data;
        for (int i = 0; i < cmd_list->VtxBuffer.Size; i++)
        {
            vtx_dst->pos[0] = vtx_src->pos.x;
            vtx_dst->pos[1] = vtx_src->pos.y;
            vtx_dst->pos[2] = 0.0f;
            vtx_dst->col = IMGUI_COL_TO_DX8_ARGB(vtx_src->col);
            vtx_dst->uv[0] = vtx_src->uv.x;
            vtx_dst->uv[1] = vtx_src->uv.y;
            vtx_dst++;
            vtx_src++;
        }
        memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        idx_dst += cmd_list->IdxBuffer.Size;
    }
    bd->pVB->Unlock();
    bd->pIB->Unlock();
    bd->pd3dDevice->SetStreamSource(0, bd->pVB, sizeof(CUSTOMVERTEX));
    bd->pd3dDevice->SetIndices(bd->pIB, 0);
    bd->pd3dDevice->SetVertexShader(D3DFVF_CUSTOMVERTEX);

    // Setup desired DX state
    ImGui_ImplDX8_SetupRenderState(draw_data);

    // Render command lists
    // Track offsets into the merged buffers.
    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    ImVec2 clip_off = draw_data->DisplayPos;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr)
            {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplDX8_SetupRenderState(draw_data);
                else
                    pcmd->UserCallback(cmd_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min(pcmd->ClipRect.x - clip_off.x, pcmd->ClipRect.y - clip_off.y);
                ImVec2 clip_max(pcmd->ClipRect.z - clip_off.x, pcmd->ClipRect.w - clip_off.y);
                clip_min.x = std::clamp(clip_min.x, 0.0f, draw_data->DisplaySize.x);
                clip_min.y = std::clamp(clip_min.y, 0.0f, draw_data->DisplaySize.y);
                clip_max.x = std::clamp(clip_max.x, 0.0f, draw_data->DisplaySize.x);
                clip_max.y = std::clamp(clip_max.y, 0.0f, draw_data->DisplaySize.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                // DX8 has no scissor rect: clip via the viewport.
                D3DVIEWPORT8 vp;
                vp.X = (DWORD)clip_min.x;
                vp.Y = (DWORD)clip_min.y;
                vp.Width = (DWORD)(clip_max.x - clip_min.x);
                vp.Height = (DWORD)(clip_max.y - clip_min.y);
                if (!vp.Width || !vp.Height) continue;
                vp.MinZ = 0.0f;
                vp.MaxZ = 1.0f;
                bd->pd3dDevice->SetViewport(&vp);

                // A smaller viewport also scales/translates projected vertices.
                // Match the projection to this viewport to keep screen positions
                // unchanged while hardware clipping limits the draw to it.
                float L = clip_off.x + (float)vp.X + 0.5f;
                float R = L + (float)vp.Width;
                float T = clip_off.y + (float)vp.Y + 0.5f;
                float B = T + (float)vp.Height;
                D3DMATRIX projection{};
                projection._11 = 2.0f / (R - L);
                projection._22 = 2.0f / (T - B);
                projection._33 = 0.5f;
                projection._41 = (L + R) / (L - R);
                projection._42 = (T + B) / (B - T);
                projection._43 = 0.5f;
                projection._44 = 1.0f;
                bd->pd3dDevice->SetTransform(D3DTS_PROJECTION, &projection);

                const IDirect3DTexture8* texture = (const IDirect3DTexture8*)pcmd->GetTexID();
                bd->pd3dDevice->SetTexture(0, (IDirect3DBaseTexture8*)texture);
                bd->pd3dDevice->SetIndices(bd->pIB, pcmd->VtxOffset + global_vtx_offset);
                bd->pd3dDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, (UINT)cmd_list->VtxBuffer.Size - pcmd->VtxOffset, pcmd->IdxOffset + global_idx_offset, pcmd->ElemCount / 3);
            }
        }
        global_idx_offset += cmd_list->IdxBuffer.Size;
        global_vtx_offset += cmd_list->VtxBuffer.Size;
    }

    // Restore the DX8 transform
    bd->pd3dDevice->SetTransform(D3DTS_WORLD, &last_world);
    bd->pd3dDevice->SetTransform(D3DTS_VIEW, &last_view);
    bd->pd3dDevice->SetTransform(D3DTS_PROJECTION, &last_projection);

    // Restore the DX8 state
    bd->pd3dDevice->ApplyStateBlock(d3d8_state_block);
    bd->pd3dDevice->DeleteStateBlock(d3d8_state_block);
}

bool ImGui_ImplDX8_Init(IDirect3DDevice8* device)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

    ImGui_ImplDX8_Data* bd = IM_NEW(ImGui_ImplDX8_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_dx8";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    bd->pd3dDevice = device;
    bd->pd3dDevice->AddRef();

    return true;
}

void ImGui_ImplDX8_Shutdown()
{
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplDX8_InvalidateDeviceObjects();
    if (bd->pd3dDevice) { bd->pd3dDevice->Release(); }
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    IM_DELETE(bd);
}

static bool ImGui_ImplDX8_CreateFontsTexture()
{
    // Build texture atlas
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    unsigned char* pixels;
    int width, height, bytes_per_pixel;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bytes_per_pixel);

    // DX8 has no A8B8G8R8: always store BGRA (A8R8G8B8) and convert colors.
    ImU32* dst_start = (ImU32*)ImGui::MemAlloc((size_t)width * height * bytes_per_pixel);
    for (ImU32* src = (ImU32*)pixels, *dst = dst_start, *dst_end = dst_start + (size_t)width * height; dst < dst_end; src++, dst++)
        *dst = IMGUI_COL_TO_DX8_ARGB(*src);
    pixels = (unsigned char*)dst_start;

    // Upload texture to graphics system
    bd->FontTexture = nullptr;
    if (bd->pd3dDevice->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &bd->FontTexture) < 0)
    {
        ImGui::MemFree(dst_start);
        return false;
    }
    D3DLOCKED_RECT tex_locked_rect;
    if (bd->FontTexture->LockRect(0, &tex_locked_rect, nullptr, 0) != D3D_OK)
    {
        bd->FontTexture->Release();
        bd->FontTexture = nullptr;
        ImGui::MemFree(dst_start);
        return false;
    }
    for (int y = 0; y < height; y++)
        memcpy((unsigned char*)tex_locked_rect.pBits + (size_t)tex_locked_rect.Pitch * y, pixels + (size_t)width * bytes_per_pixel * y, (size_t)width * bytes_per_pixel);
    bd->FontTexture->UnlockRect(0);
    ImGui::MemFree(dst_start);

    // Store our identifier
    io.Fonts->SetTexID((ImTextureID)bd->FontTexture);

    return true;
}

bool ImGui_ImplDX8_CreateDeviceObjects()
{
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    if (!bd || !bd->pd3dDevice)
        return false;
    if (!ImGui_ImplDX8_CreateFontsTexture())
        return false;
    return true;
}

void ImGui_ImplDX8_InvalidateDeviceObjects()
{
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    if (!bd || !bd->pd3dDevice)
        return;
    if (bd->pVB) { bd->pVB->Release(); bd->pVB = nullptr; }
    if (bd->pIB) { bd->pIB->Release(); bd->pIB = nullptr; }
    if (bd->FontTexture) { bd->FontTexture->Release(); bd->FontTexture = nullptr; ImGui::GetIO().Fonts->SetTexID(0); }
}

bool ImGui_ImplDX8_NewFrame()
{
    ImGui_ImplDX8_Data* bd = ImGui_ImplDX8_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplDX8_Init()?");

    return bd->FontTexture || ImGui_ImplDX8_CreateDeviceObjects();
}

#endif // #ifndef IMGUI_DISABLE

