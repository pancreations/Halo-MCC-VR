#pragma once

// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers process-wide.
// Present is the "here's a finished frame" call every D3D11 game makes each
// frame — our hook is where all VR work happens.

bool InstallD3D11Hooks();
