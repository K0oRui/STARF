// dear imgui: Renderer Backend for DirectX8
// Port of imgui_impl_dx9 to the DX8 fixed pipeline. DX8 has no scissor rect
// (clipping is done via viewport), no sampler states (D3DTSS_* instead), no
// separate alpha blend, and token-based state blocks.
#pragma once
#include "imgui.h"

struct IDirect3DDevice8;

IMGUI_IMPL_API bool ImGui_ImplDX8_Init(IDirect3DDevice8* device);
IMGUI_IMPL_API void ImGui_ImplDX8_Shutdown();
IMGUI_IMPL_API void ImGui_ImplDX8_NewFrame();
IMGUI_IMPL_API void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data);
IMGUI_IMPL_API bool ImGui_ImplDX8_CreateDeviceObjects();
IMGUI_IMPL_API void ImGui_ImplDX8_InvalidateDeviceObjects();