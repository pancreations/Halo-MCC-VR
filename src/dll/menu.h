#pragma once
#include <windows.h>
#include <d3d11.h>

// Dear ImGui settings menu, drawn into an offscreen texture that vr.cpp
// shows on a floating panel in the headset. Toggled with F1; while open,
// mouse and keyboard go to the menu instead of the game.

inline constexpr int MENU_W = 1024;
inline constexpr int MENU_H = 768;

bool Menu_Init(HWND gameWindow, ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT rtFormat);
bool Menu_IsOpen();
bool Menu_Toggle();
void Menu_SetVrPointer(bool hit, float u, float v, bool pressed, float scrollY);
void Menu_ClearVrPointer();
ID3D11Texture2D* Menu_Render(); // draws the current frame of UI; nullptr on failure
