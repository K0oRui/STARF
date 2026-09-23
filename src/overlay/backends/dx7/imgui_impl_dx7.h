#pragma once
#include "imgui.h"

struct IDirect3DDevice7;
struct IDirectDrawSurface7;

bool ImGui_ImplDX7_Init(IDirect3DDevice7* device);
void ImGui_ImplDX7_Shutdown();
bool ImGui_ImplDX7_NewFrame();
long ImGui_ImplDX7_RenderDrawData(ImDrawData* draw_data);
void ImGui_ImplDX7_InvalidateDeviceObjects();
IDirectDrawSurface7* ImGui_ImplDX7_CreateTexture(const unsigned char* rgba, int w, int h);
