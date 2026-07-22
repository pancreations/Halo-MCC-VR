#pragma once

#include <d3d11.h>

struct IDXGISwapChain;

// Called once on the DLL's background init thread. Creates the OpenXR instance
// and finds the headset (slow), so the render thread never blocks on it.
void VR_InitInstance();

// Called around the game's real DXGI Present. The completed OpenXR frame is
// submitted before Present; after Present returns, OpenXR supplies the exact
// predicted display time that Halo will use while rendering its next frame.
void VR_BeforePresent(IDXGISwapChain* swapchain);
void VR_AfterPresent(IDXGISwapChain* swapchain);
void VR_OnResizeBuffers(IDXGISwapChain* swapchain);

// Timing beacon from Halo's camera-copy hook. The first call after
// VR_AfterPresent proves how quickly the freshly predicted pose reaches the
// render camera; subsequent calls prove camera transforms keep up with FPS.
void VR_NotifyCameraTransform();

// HUD panel capture: called from the FP driver hook (render thread, left-eye
// pass, before the HUD elements draw). Snapshots the left-eye scene-only image;
// VR_OnPresent diffs it against the finished frame to extract Halo's HUD pixels
// for the floating HUD panel.

// One-shot frame trace: logs the in-frame ORDER of render events (eye begin,
// RTV redirect, FP driver runs, HUD element submits, eye end) for ~one frame,
// a few seconds into a level. Names the correct pre-HUD snapshot anchor from
// the game's own timeline instead of an assumption.
void VR_TraceEvent(const char* tag, int a, int b);

// Called from the menu: re-place the virtual screen in front of where the
// user is currently looking.
void VR_RequestRecenter();

// Switch between immersive stereo gameplay and Halo's stable head-locked
// pause/menu screen. Requests are comfort-faded on the render thread.
void VR_RequestPausePresentation(bool paused);
bool VR_IsPausePresentation();
bool VR_IsPausePresentationTarget();

// F10: toggle whether the flat screen follows the head (vs. staying pinned in
// the world). Only matters while head tracking is on.
void VR_ToggleScreenFollow();

// F11: development stereo proof. Alternates left/right game renders and
// submits the two retained images as an OpenXR projection layer.
void VR_ToggleStereo();
bool VR_IsStereoEnabled();
// True only for the begun OpenXR frame the runtime asked us to render. A
// synchronized/unfocused session commonly publishes false while the headset is
// idle; game hooks must not treat the resulting absent eye raster as failure.
bool VR_ShouldRenderPreparedFrame();
// Called on the render thread when Halo stops driving its level camera. Makes
// every 3D path inactive immediately and drops references to Halo's scene
// target before MCC switches to its shell or another resident game engine.
void VR_DetachGamePresentation();

// Called by the M2 game render hook immediately after each eye's scene pass,
// before the next eye overwrites the game backbuffer.
bool VR_CaptureRenderedEye(int eye);
// ODST's third-person death renderer bypasses the internal scene-color RTV and
// draws directly into the current swapchain buffer. Copy that completed draw
// into the eye cache without doing COM discovery in the game render hook.
bool VR_CaptureBackbufferEye(int eye);
void VR_BeginRasterEye(int eye);
void VR_EndRasterEye();
// ODST's native CHUD phases are part of the same logical per-eye render as
// Halo 3, but ODST can rebind the flat output target while those phases run.
// Keep that title-specific phase on the active eye cache and restore every
// output-merger reference afterward. Called at CHUD-phase granularity, never
// per widget.
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
bool VR_BeginNativeHudEyeDraw(int eye);
void VR_EndNativeHudEyeDraw();
void VR_BeginNativeHudTargetCopy();
void VR_EndNativeHudTargetCopy();
ID3D11Resource* VR_RedirectNativeHudCopySource(ID3D11Resource* source);
void VR_GetNativeHudRouteStats(unsigned& completedPhaseScopes,
                               unsigned& provenOmMatches,
                               unsigned& exactCopyScopes,
                               unsigned& copySubstitutions);
#endif

// Universal scope: a refresh-limited third world render redirected into a
// private cache. The physical OpenXR quad continues tracking every frame.
bool VR_ScopeShouldRenderThisFrame();
bool VR_BeginScopeRaster();
void VR_CaptureScope();
void VR_EndScopeRaster();
bool VR_GetScopeRenderAspect(float& outAspect);
// Current non-persistent scope magnification. It resets to scope_zoom whenever
// R3 opens the scope and is adjusted by right-stick Y while active.
float VR_GetScopeZoom();

// Redirects the final scene-color RTV to the active eye's target and, while
// stereo_sun_shafts is off, neutralizes the sun-shaft occlusion pass (its
// radial blur uses a single per-frame sun position, streaking the other eye).
// The context is the one the game is binding on, so the neutralizing clear
// lands in the right command order.
bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output);

// Latest head pose in the VR "local" space (captured each frame). Orientation
// is a quaternion (x,y,z,w), position is meters (x,y,z). Returns false until a
// valid pose has been read. Thread-safe; the game camera hook (M1) reads this.
bool VR_GetHeadPose(float outQuat[4], float outPos[3]);
// Latest right-controller aim pose in the same OpenXR local space as the head.
// This is tracking only; weapon/projectile application is performed by M3 game hooks.
bool VR_GetRightControllerPose(float outQuat[4], float outPos[3]);
// Left controller pose (used by the D-pad gesture; false until tracked).
bool VR_GetLeftControllerPose(float outQuat[4], float outPos[3]);
// Called only from Halo's already-validated class-2 CHUD path. The active
// weapon reticle is redirected into the controller-ray quad texture instead
// of being drawn at the center of either VR eye.
bool VR_BeginAuthoredReticleCapture();
void VR_EndAuthoredReticleCapture();
// M3: the game layer sets this when the crosshair is over an enemy (engine
// target-lock). While true, the floating reticle repaints red like the OG HUD.
void VR_SetReticleEnemy(bool enemy);
// Weapon-hand aim pose shared by bullet steering, the reticle, and the visible
// barrel. Position = right hand; orientation = right controller, or the
// right->left two-hand line when two-handed aim engages. False until tracked.
bool VR_GetAimPose(float outQuat[4], float outPos[3]);
bool VR_IsTwoHandAiming();
// Latest OpenXR per-eye FOV angles: left, right, up, down (radians).
bool VR_GetEyeFov(int eye, float outFov[4]);
// Pixel aspect of Halo's main render surface. Halo lays native 2D HUD geometry
// out for this shape, which can differ substantially from the headset's
// tangent-space view aspect.
bool VR_GetGameRenderAspect(float& outAspect);
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
// Universal scope state is owned by the VR controller input path and consumed
// by the render/compositor path. It is independent of Halo's native zoom.
void VR_SetScopeActive(bool active);
bool VR_IsScopeActive();
void VR_RequestScopeToggle();
// Receives Halo's blended XInput rumble level (0..1). The render thread maps
// it to portable OpenXR feedback on both hands and owns all stop conditions.
void VR_SetGameHaptics(float amplitude);

// Position and rotation of one eye relative to the midpoint of both OpenXR
// views. Position is in meters and both outputs use OpenXR view-local axes
// (+X right, +Y up, -Z forward). Runtimes may change these offsets when the
// headset's lens spacing changes, so stereo rendering must not assume one
// headset's fixed IPD or eye cant.
bool VR_GetEyeViewOffset(int eye, float outPosition[3], float outQuat[4]);

struct VrStatus
{
    char runtime[128];
    char sessionState[32];
    unsigned gameWidth, gameHeight;
    float fps;
};
void VR_GetStatus(VrStatus& out);
