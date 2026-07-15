#pragma once

// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers process-wide.
// Present is the "here's a finished frame" call every D3D11 game makes each
// frame — our hook is where all VR work happens.

bool InstallD3D11Hooks();
// The lightweight SRV hook is needed only long enough to identify Halo's
// read-before-write post-process history resource. It is disabled afterward
// so the steady-state 120 FPS render path has no shader-resource hook.
void D3D11_SetHistoryDiscovery(bool enabled);
