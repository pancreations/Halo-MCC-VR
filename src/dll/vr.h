#pragma once

struct IDXGISwapChain;

// Called once on the DLL's background init thread. Creates the OpenXR instance
// and finds the headset (slow), so the render thread never blocks on it.
void VR_InitInstance();

// Called from the D3D11 hooks, once per game frame on the game's render thread.
void VR_OnPresent(IDXGISwapChain* swapchain);
void VR_OnResizeBuffers(IDXGISwapChain* swapchain);

// Called from the menu: re-place the virtual screen in front of where the
// user is currently looking.
void VR_RequestRecenter();

struct VrStatus
{
    char runtime[128];
    char sessionState[32];
    unsigned gameWidth, gameHeight;
    float fps;
};
void VR_GetStatus(VrStatus& out);
