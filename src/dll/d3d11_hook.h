#pragma once

#include <cstdint>

// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers process-wide.
// Present is the "here's a finished frame" call every D3D11 game makes each
// frame — our hook is where all VR work happens.

bool InstallD3D11Hooks();

// True only after Halo 2's identified native crosshair shader has completed a
// capture for the currently loaded halo2.dll generation. Until then the shared
// compositor retains its procedural fallback, so a shader mismatch cannot
// leave the player without any aiming marker.
bool D3D_Halo2NativeCrosshairCaptured();
bool D3D_Halo2HudShaderPathAvailable();
// Stage 3BH/3BR's Draw/DrawIndexed framing pin is required before Halo 4's
// optional authored-reticle hooks may hide the native flat CUI copy.
bool D3D_Halo4AuthoredReticleDrawPathAvailable();
// Exact known-good V6 pixel-shader path for Halo 4's visor/framing overlay.
// The feature remains stock unless both shader creation and PS binding hooks
// are live; the default-visible path forwards every binding byte-for-byte.
bool D3D_Halo4HelmetShaderPathAvailable();
void D3D_GetHalo4HelmetTelemetry(
    uint64_t& shadersRegistered, uint64_t& suppressions);
void D3D_GetHalo2HudTelemetry(
    uint64_t& gameplayShadersRegistered,
    uint64_t& crosshairShadersRegistered,
    uint64_t& rasterDraws,
    uint64_t& nativeCrosshairDraws,
    uint64_t& stateFailures);

// --- Desktop-window fit (config.fit_desktop_window) -------------------------
// The forced full-render backbuffer size (0,0 when the fit is off or not yet
// initialized). menu.cpp uses it while rewriting MCC's WM_SIZE transaction so
// the engine keeps drawing the full headset render into the fitted window.
void D3D_GetForcedRenderSize(unsigned& width, unsigned& height);

// True only when the desktop fit is on AND its hooks installed at startup. All
// the window-shrink behavior in menu.cpp is gated on this (not the live config
// flag), so ticking the restart-required checkbox mid-session can't half-engage
// the fit without the backbuffer force behind it.
bool D3D_FitActive();

// menu.cpp brackets the game's own WM_SIZE handling with this. While set, our
// GetClientRect hook returns the full render size to the game's resize code on
// THAT call stack only (it is thread-local), so MCC keeps sizing its render to
// the full backbuffer. DXGI/DWM present-time client queries -- on the render
// thread -- never see it, so they still downscale the full frame into the real,
// smaller window instead of clipping it to a corner.
void D3D_SetForcedClientLie(bool on);

// --- Dormant co-op drop probe (config.coop_probe) ---------------------------
// The lifecycle call remains wired for targeted diagnostic rebuilds. Production
// builds compile out both Present sampling and this dump; reason names the mode
// a diagnostic rebuild fell out of.
void CoopProbe_DumpRunUp(const char* reason);
