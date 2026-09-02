# C-H2-90 Halo 2 central aim-assist-off test

Status: **ready for headset test; unaccepted**.

This is one Halo-2-only optional change above C-H4-57. Halo 4 and every other
title are unchanged. The accepted cumulative pointer remains C-H4-56 until the
user accepts this exact packaged DLL; C-H4-57's separate Halo 3 regression is
still pending.

## Change

C-H2-89 is rejected and compiled dormant: its log proved the retail debug
global never resolved, so aim assist was never changed. C-H2-90 instead hooks
the verified central Halo 2 aim-assist calculation at retail RVA `0x759260`.
The binding comes from official H2EK `aim_assist.cpp`, a cross-architecture
caller match, identical ABI/neutral initialization, and a signature that is
unique in the pinned retail loaded image. Full evidence is E-H2-77.

While VR controller aim owns local user 0, the detour returns the engine's own
neutral initialization: zero camera-assist output and no acquired target.
Every other call runs the original function. It does not patch melee code,
weapon/map tags, game files, the reticle, camera, HUD, hands, muzzle effects, or
either renderer.

If the exact function cannot be proven or the hook cannot be installed, only
this feature remains stock and logs `StockFallback`. Camera, stereo, controller
aim, hands, HUD, native reticle, weapons, and OpenXR remain armed.

## Headset test

1. Launch Halo 2 and enter a combat checkpoint in Classic. Move the controller
   sight slowly past enemies without commanding camera rotation. The view
   should no longer slow, pull, or follow them.
2. Fire at targets and confirm the native reticle and projectiles still follow
   the controller sight line.
3. Try several normal-range melees and report the result. There is no separate
   melee patch in this candidate; this observes whether removing the shared
   target result also removes the unwanted melee turn/miss.
4. Switch to Anniversary and repeat briefly. Aim suppression is intentionally
   shared across Halo 2's two renderers because both consume the same engine
   player-control result; rendering, hands, HUD, reticle, and effects must not
   regress.
5. Attach `HaloMCCVR.log`. It must show:

       Halo 2 aim-assist suppression Installed (C-H2-90)
       Halo 2 C-H2-90 aim-assist calculation: N calls, M suppressed ...

   `M` must be non-zero during gameplay. `StockFallback` or zero suppressed
   calls means the intended test did not run and the headset behavior is not a
   verdict on this mechanism.

No automatic installation and no GitHub push are part of this handoff.
