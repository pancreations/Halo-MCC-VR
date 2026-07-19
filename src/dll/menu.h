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
ID3D11Texture2D* Menu_Render(); // draws the current frame of UI; nullptr on failure

// True when a one-line background-status toast should be shown even though
// the menu is closed (engine hooking, HUD data scan...). Refreshes the toast
// text from Game_GetStatusText; when it returns true, vr.cpp renders and
// submits the menu panel, which then shows only the small status line.
bool Menu_HasToast();
