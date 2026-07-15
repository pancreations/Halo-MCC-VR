#pragma once

#include <d3d11.h>

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

// F11: development stereo proof. Alternates left/right game renders and
// submits the two retained images as an OpenXR projection layer.
void VR_ToggleStereo();
bool VR_IsStereoEnabled();

// Called by the M2 game render hook immediately after each eye's scene pass,
// before the next eye overwrites the game backbuffer.
void VR_CaptureRenderedEye(int eye);
void VR_BeginRasterEye(int eye);
void VR_EndRasterEye();
// Redirects the final scene-color RTV to the active eye's target and, while
// stereo_sun_shafts is off, neutralizes the sun-shaft occlusion pass (its
// radial blur uses a single per-frame sun position, streaking the other eye).
// The context is the one the game is binding on, so the neutralizing clear
// lands in the right command order.
bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output);
// During initial stereo frames, identifies render targets sampled before they
// are overwritten. Those are the shared post-process history that needs an
// independent copy for each eye; discovery disables itself once identified.
void VR_RecordShaderResourceReads(UINT count, ID3D11ShaderResourceView* const* srvs);
// Records render-target binds OUTSIDE the eye passes (frame-level post).
// A texture written out of pass but sampled inside an eye pass is per-frame
// shared history that per-eye copies cannot fix; it gets blanked in stereo.
void VR_RecordFrameRtv(UINT count, ID3D11RenderTargetView* const* rtvs);
// Ghost hunt, log-only: GPU-side copies were never observable by any earlier
// census. Logs unique texture copy pairs while stereo runs, tagged with the
// active eye (-1 = between passes, where the poison is suspected to move).
void VR_RecordCopy(ID3D11Resource* dst, ID3D11Resource* src, const char* what);
// THE GHOST FIX at the shader-constant level: during an eye's render, any
// constant upload containing a 64-byte block that exactly matches the OTHER
// eye's camera/derived matrices (current or previous frame) gets that block
// swapped for THIS eye's version. Exact-match only, so it cannot misfire;
// does nothing when no block matches. The per-eye reference blocks are
// stored by the render hook each pass via VR_StoreEyeDerived.
void VR_StoreEyeDerived(int eye, const void* data, size_t len);
// Returns `data`, or `scratch` holding a patched copy (UpdateSubresource
// gives const data, so patching needs a copy).
const void* VR_FilterParamUpload(ID3D11Resource* dst, const void* data,
                                 void* scratch, unsigned scratchSize);
void VR_NoteMappedBuffer(ID3D11Resource* res, void* pData);
void VR_RecordUnmap(ID3D11Resource* res, void* caller); // patches mapped bytes in place
// Ghosting hunt: logs each unique UAV-bound resource seen during an eye pass
// (the temporal history writer is compute/UAV-based and invisible to the RTV hook).
void VR_RecordUavCensus(UINT count, ID3D11UnorderedAccessView* const* uavs);
// Read-only M3 diagnostic: records the tail of one eye's D3D draw stream so
// weapon/CHUD shaders can be isolated without changing render state.
void VR_RecordDraw(unsigned kind, unsigned count, const void* vertexShader,
                   const void* pixelShader);

// Latest head pose in the VR "local" space (captured each frame). Orientation
// is a quaternion (x,y,z,w), position is meters (x,y,z). Returns false until a
// valid pose has been read. Thread-safe; the game camera hook (M1) reads this.
bool VR_GetHeadPose(float outQuat[4], float outPos[3]);
// Latest right-controller aim pose in the same OpenXR local space as the head.
// This is tracking only; weapon/projectile application is performed by M3 game hooks.
bool VR_GetRightControllerPose(float outQuat[4], float outPos[3]);
// Left controller pose (used by the D-pad gesture; false until tracked).
bool VR_GetLeftControllerPose(float outQuat[4], float outPos[3]);
// Latest OpenXR per-eye FOV angles: left, right, up, down (radians).
bool VR_GetEyeFov(int eye, float outFov[4]);
// M3: snapshot of the VR controllers' gamepad-like inputs, read once per frame
// from the OpenXR action set. The XInput hook merges this into (or fabricates)
// the gamepad state MCC reads, so the Sense controllers drive menus and game.
struct VrPadState
{
    bool valid = false;      // controllers tracked and actions synced
    float moveX = 0, moveY = 0;  // left thumbstick
    float turnX = 0, turnY = 0;  // right thumbstick
    float trigL = 0, trigR = 0;  // triggers 0..1
    float gripL = 0, gripR = 0;  // grips 0..1
    bool a = false, b = false, x = false, y = false;
    bool clickL = false, clickR = false, menu = false;
};
void VR_GetPadState(VrPadState& out);

// Rotation of one eye relative to the midpoint of both eyes, as a quaternion
// (x,y,z,w) in OpenXR view-local axes (+X right, +Y up, -Z forward). Canted
// headsets like PSVR2 mount each display angled outward a few degrees; the
// per-eye FOV above is measured around that canted axis, so the raster camera
// must be turned by this rotation for the image to cover the whole lens.
bool VR_GetEyeCantQuat(int eye, float outQuat[4]);

struct VrStatus
{
    char runtime[128];
    char sessionState[32];
    unsigned gameWidth, gameHeight;
    float fps;
};
void VR_GetStatus(VrStatus& out);
