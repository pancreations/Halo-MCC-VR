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

// F10: toggle whether the flat screen follows the head (vs. staying pinned in
// the world). Only matters while head tracking is on.
void VR_ToggleScreenFollow();

// Latest head pose in the VR "local" space (captured each frame). Orientation
// is a quaternion (x,y,z,w), position is meters (x,y,z). Returns false until a
// valid pose has been read. Thread-safe; the game camera hook (M1) reads this.
bool VR_GetHeadPose(float outQuat[4], float outPos[3]);

struct VrStatus
{
    char runtime[128];
    char sessionState[32];
    unsigned gameWidth, gameHeight;
    float fps;
};
void VR_GetStatus(VrStatus& out);
