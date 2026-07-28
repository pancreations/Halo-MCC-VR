#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <imgui.h>
#include <algorithm>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "menu.h"
#include "vr.h"
#include "game.h"
#include "title_adapter.h"
#include "d3d_state.h"
#include "d3d11_hook.h"
#include "../common/log.h"
#include "../common/config.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    bool g_ready = false;
    std::atomic<bool> g_open{false};
    HWND g_hwnd = nullptr;
    WNDPROC g_origWndProc = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    ID3D11Texture2D* g_tex = nullptr;
    ID3D11RenderTargetView* g_rtv = nullptr;
    struct VrPointerInput
    {
        bool hit = false;
        bool pressed = false;
        float u = 0.0f;
        float v = 0.0f;
        float scrollY = 0.0f;
    };
    VrPointerInput g_vrPointer;
    bool g_resetArmed = false; // "reset all settings" needs a second click
    // ImGui gets input on the game's window thread but draws on its render
    // thread; this lock keeps the two from touching ImGui at the same time.
    CRITICAL_SECTION g_cs;

    // Private message we post to the game window so the fit runs on the window's
    // own (UI) thread, where touching window size/position is safe.
    constexpr UINT kFitGameWindowMsg = WM_APP + 0x37;

    // Fit the game window inside the primary monitor's work area, preserving the
    // render aspect (kNativeRenderWidth:kNativeRenderHeight, constant because
    // resolution_scale is uniform) so the downscaled desktop picture isn't
    // distorted. This shrinks only the VISIBLE window; MCC keeps drawing the
    // full-size frame into the forced full-size backbuffer (d3d11_hook.cpp), so
    // the headset picture and the gun alignment are unchanged. Runs on the UI
    // thread.
    void FitGameWindow(HWND hwnd)
    {
        // MCC's decorated window path keeps Slate in a different geometry from
        // the DXGI-stretched client when the render is larger than the monitor.
        // That is the path where native shell/pause hit-testing and controller
        // focus become unusable. Use the game's borderless-window geometry for
        // the fitted client so MCC has one client rectangle for both display and
        // native menu input.
        const LONG_PTR oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR borderlessStyle =
            (oldStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                          WS_MAXIMIZEBOX | WS_SYSMENU)) |
            WS_POPUP;
        const LONG_PTR oldExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const LONG_PTR borderlessExStyle =
            oldExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                           WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, borderlessStyle);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, borderlessExStyle);

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfo(mon, &mi))
            return;
        const int workW = mi.rcWork.right - mi.rcWork.left;
        const int workH = mi.rcWork.bottom - mi.rcWork.top;
        if (workW <= 0 || workH <= 0)
            return;
        const float aspect = (float)kNativeRenderWidth / (float)kNativeRenderHeight;
        int w = workW;
        int h = (int)((float)w / aspect + 0.5f);
        if (h > workH)
        {
            h = workH;
            w = (int)((float)h * aspect + 0.5f);
        }
        const int x = mi.rcWork.left + (workW - w) / 2;
        const int y = mi.rcWork.top + (workH - h) / 2;
        if (!SetWindowPos(hwnd, nullptr, x, y, w, h,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
        {
            LOG("fit: borderless SetWindowPos failed (%lu)",
                static_cast<unsigned long>(GetLastError()));
            return;
        }
        RECT client{};
        if (GetClientRect(hwnd, &client))
        {
            LOG("fit: native MCC window is borderless; client %ldx%ld at (%d,%d)",
                client.right - client.left, client.bottom - client.top, x, y);
        }
    }

    LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        // Fit request (posted from Menu_Init) -- run it here on the UI thread.
        if (msg == kFitGameWindowMsg)
        {
            FitGameWindow(hwnd);
            return 0;
        }
        // Keep the game rendering and processing input while the user is in the
        // headset. Looking through the headset hands desktop focus to SteamVR,
        // and MCC (like most games) stops drawing and ignores input when it
        // isn't the focused window — which showed up as a frozen VR screen. We
        // tell the game it is always the active, foreground window.
        switch (msg)
        {
        case WM_ACTIVATEAPP:
            wp = TRUE;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE)
                wp = MAKEWPARAM(WA_ACTIVE, 0);
            break;
        case WM_NCACTIVATE:
            // TRUE keeps the window drawn/treated as active.
            return CallWindowProcW(g_origWndProc, hwnd, msg, TRUE, lp);
        case WM_KILLFOCUS:
            // Don't let the game hear that it lost keyboard focus.
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_WINDOWPOSCHANGING:
            if (D3D_FitActive())
            {
                // MCC believes its client is full-size (we tell it so), so it may
                // try to size the WINDOW to match and grow it back off-screen.
                // Clamp any oversize request to a monitor fit. Genuine size
                // changes only (user drags with SWP_NOSIZE are left alone).
                WINDOWPOS* pos = (WINDOWPOS*)lp;
                if (pos && !(pos->flags & SWP_NOSIZE))
                {
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
                    MONITORINFO mi{sizeof(mi)};
                    if (GetMonitorInfo(mon, &mi))
                    {
                        const int workW = mi.rcWork.right - mi.rcWork.left;
                        const int workH = mi.rcWork.bottom - mi.rcWork.top;
                        if (workW > 0 && workH > 0 && (pos->cx > workW || pos->cy > workH))
                        {
                            const float aspect =
                                (float)kNativeRenderWidth / (float)kNativeRenderHeight;
                            int w = workW;
                            int h = (int)((float)w / aspect + 0.5f);
                            if (h > workH) { h = workH; w = (int)((float)h * aspect + 0.5f); }
                            pos->cx = w;
                            pos->cy = h;
                            pos->x = mi.rcWork.left + (workW - w) / 2;
                            pos->y = mi.rcWork.top + (workH - h) / 2;
                            pos->flags &= ~SWP_NOMOVE;
                        }
                    }
                }
            }
            break;
        case WM_SIZE:
            if (D3D_FitActive())
            {
                // The window is smaller than the render. Tell MCC its client is
                // still the full render size so it keeps drawing the full frame
                // into the (forced full-size) backbuffer instead of shrinking to
                // the corner -- the black-border crop. The GPU downscales the
                // full frame into the small window on present. Bracket the call
                // so our GetClientRect hook returns full ONLY to MCC's resize
                // code on this stack (never to DXGI's present-time query).
                unsigned fw = 0, fh = 0;
                D3D_GetForcedRenderSize(fw, fh);
                if (fw && fh)
                {
                    const LPARAM full = MAKELPARAM((WORD)fw, (WORD)fh);
                    D3D_SetForcedClientLie(true);
                    const LRESULT r = CallWindowProcW(g_origWndProc, hwnd, msg, wp, full);
                    D3D_SetForcedClientLie(false);
                    return r;
                }
            }
            break;
        }

        // Hotkeys act on plain WM_KEYDOWN only. F10 is the one exception:
        // Windows delivers it as WM_SYSKEYDOWN even without Alt held. All
        // other Alt combos must reach the game untouched — SteamVR's
        // exit/dashboard flow sends Alt+F4 (WM_SYSKEYDOWN + VK_F4), and when
        // F4 was a hotkey that phantom press flipped the yaw sign, inverting
        // head-turn and hand-aim ("controls completely broken", two sessions
        // in a row ~50 ms after the session lost focus). The F4/F5/F7
        // calibration flips now live in the F1 menu instead of on keys.
        const bool keyDown = msg == WM_KEYDOWN && !(lp & (1 << 30)); // no auto-repeat
        const bool sysKeyDown = msg == WM_SYSKEYDOWN && !(lp & (1 << 30));
        if (sysKeyDown && wp == VK_F4)
            LOG("Alt+F4 received; passing it to the game (close request)");
        if (keyDown || (sysKeyDown && wp == VK_F10))
        {
            switch (wp)
            {
            case VK_F1: Menu_Toggle(); return 0;
            case VK_F2: Game_ToggleHeadTracking(); return 0;
            case VK_F3: Game_Recenter(); return 0;
            case VK_F6: Game_TogglePositional(); return 0;
            case VK_F8: Game_PitchTrim(-1); return 0;
            case VK_F9: Game_PitchTrim(+1); return 0;
            case VK_F10: VR_ToggleScreenFollow(); return 0;
            case VK_F11:
                if (Game_CanToggleImmersiveView()) VR_ToggleStereo();
                return 0;
            case VK_PRIOR: Game_LeanScale(+1); return 0; // Page Up
            case VK_NEXT:  Game_LeanScale(-1); return 0; // Page Down
            case VK_HOME: Game_GunScale(+1); return 0; // bigger hand-held weapon
            case VK_END:  Game_GunScale(-1); return 0; // smaller hand-held weapon
            case VK_INSERT: Game_ToggleVrAim(); return 0; // controller steers aim
            }
        }
        if (g_ready && g_open)
        {
            EnterCriticalSection(&g_cs);
            if (msg == WM_MOUSEMOVE)
            {
                // The menu texture is a fixed size; map the window-space mouse
                // position onto it.
                RECT rc{};
                GetClientRect(hwnd, &rc);
                const float cw = (float)(rc.right > 0 ? rc.right : 1);
                const float ch = (float)(rc.bottom > 0 ? rc.bottom : 1);
                ImGui::GetIO().AddMousePosEvent(GET_X_LPARAM(lp) * MENU_W / cw,
                                                GET_Y_LPARAM(lp) * MENU_H / ch);
            }
            else
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            }
            LeaveCriticalSection(&g_cs);
            // Swallow mouse/keyboard so clicking the menu doesn't also fire
            // the player's weapon. (Raw input still reaches the game in M0.)
            // Never swallow Alt+F4: closing the game must always work.
            if (((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                 (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)) &&
                !(msg == WM_SYSKEYDOWN && wp == VK_F4))
                return 0;
        }
        // Keep native mouse messages in MCC's stock physical client domain. The
        // prior fit scaled only this path while its separately polled cursor stayed
        // physical. This candidate tests whether restoring one coordinate domain
        // stops mouse hover from displacing keyboard/controller menu focus.
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }

    void DrawUI()
    {
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(MENU_W - 32, MENU_H - 32), ImGuiCond_FirstUseEver);
        ImGui::Begin("HaloMCCVR Settings", nullptr, ImGuiWindowFlags_NoCollapse);

        bool changed = false;
        if (ImGui::BeginTabBar("HaloMCCVRTabs"))
        {
        if (ImGui::BeginTabItem("Status"))
        {
        VrStatus st{};
        VR_GetStatus(st);
        ImGui::Text("Runtime: %s", st.runtime);
        ImGui::Text("Session: %s   |   Game: %ux%u @ %.0f fps", st.sessionState, st.gameWidth,
                    st.gameHeight, st.fps);
        const TitleDescriptor* title = TitleAdapter_GetActive();
        ImGui::Text("Title: %s   |   Mode: %s",
                    title ? title->displayName : "MCC shell",
                    RuntimeModeName(TitleAdapter_GetRuntimeMode()));
        if (ImGui::Button("Re-center headset and screen"))
        {
            Game_Recenter();
        }
        ImGui::SameLine();
        if (ImGui::Button(VR_IsPausePresentation()
                ? "Resume game (Start)"
                : "Pause game in 2D (Start)"))
        {
            Input_RequestPauseToggle();
            Menu_Toggle();
        }
        ImGui::TextDisabled("PSVR2 fallback: press Y+B together to Pause/Resume.");
        ImGui::Separator();
        ImGui::BeginDisabled(!Game_CanToggleImmersiveView());
        if (ImGui::Button(Game_IsHeadTracking()
                ? "Turn head tracking OFF (F2)"
                : "Turn head tracking ON (F2)"))
        {
            Game_ToggleHeadTracking();
        }
        ImGui::SameLine();
        if (ImGui::Button(VR_IsStereoEnabled()
                ? "Turn stereo 3D OFF (F11)"
                : "Turn stereo 3D ON (F11)"))
        {
            VR_ToggleStereo();
        }
        ImGui::EndDisabled();
        ImGui::Text("Head tracking: %s   |   Stereo rendering: %s   |   View: %s",
                    Game_IsHeadTracking() ? "ON" : "OFF",
                    VR_IsStereoEnabled() ? "ON" : "OFF",
                    VR_IsPausePresentation() ? "head-locked 2D" : "immersive 3D");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("L3+R3 or F1 closes this menu.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Gameplay"))
        {
        changed |= ImGui::SliderFloat("Screen width (m)", &g_config.screen_width_m, 1.0f, 10.0f, "%.1f");
        changed |= ImGui::SliderFloat("Screen distance (m)", &g_config.screen_distance_m, 0.5f, 10.0f, "%.1f");
        float headsetSmoothPercent = g_config.headset_smoothing * 100.0f;
        if (ImGui::SliderFloat("Headset micro-smoothing", &headsetSmoothPercent,
                               0.0f, 10.0f, "%.0f%%", ImGuiSliderFlags_None))
        {
            g_config.headset_smoothing = headsetSmoothPercent / 100.0f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Raw (0%)##headset"))
        {
            g_config.headset_smoothing = 0.0f;
            changed = true;
        }
        ImGui::TextDisabled("Raw by default. Try 5%% only for micro-jitter; capped at 10%% for comfort.");
        changed |= ImGui::Checkbox("Auto-enter VR on level load", &g_config.auto_vr);
        ImGui::TextDisabled("Turns head tracking + stereo on when a level starts and off in the menu.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls"))
        {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("VR turning (right controller stick)");
        if (ImGui::RadioButton("Snap turn", !g_config.turn_smooth))
        {
            g_config.turn_smooth = false;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Smooth turn", g_config.turn_smooth))
        {
            g_config.turn_smooth = true;
            changed = true;
        }
        if (g_config.turn_smooth)
            changed |= ImGui::SliderFloat("Turn speed (deg/s)", &g_config.turn_smooth_deg_s, 30.0f, 360.0f, "%.0f");
        else
            changed |= ImGui::SliderFloat("Snap increment (deg)", &g_config.turn_snap_deg, 5.0f, 90.0f, "%.0f");

        ImGui::Spacing();
        ImGui::Text("D-pad gesture (hold controller next to head)");
        if (ImGui::RadioButton("Left controller", g_config.dpad_hand == 0))
        {
            g_config.dpad_hand = 0;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Right controller", g_config.dpad_hand == 1))
        {
            g_config.dpad_hand = 1;
            changed = true;
        }
        float hapticPercent = g_config.haptic_intensity * 100.0f;
        if (ImGui::SliderFloat("Controller vibration", &hapticPercent,
                               0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_None))
        {
            g_config.haptic_intensity = hapticPercent / 100.0f;
            if (g_config.haptic_intensity <= 0.0f)
                VR_SetGameHaptics(0.0f);
            changed = true;
        }
        ImGui::TextDisabled("L3+R3 toggles this menu; the right trigger clicks the VR pointer.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Aim & Weapons"))
        {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Hand-held weapon");
        const float positionWeight =
            (40.0f - g_config.weapon_position_follow) / 38.0f;
        const float rotationWeight =
            (45.0f - g_config.weapon_rotation_follow) / 43.0f;
        float weightPercent = g_config.weapon_inertia
            ? std::clamp((positionWeight + rotationWeight) * 50.0f,
                         0.0f, 100.0f)
            : 0.0f;
        if (ImGui::SliderFloat(
                "Weapon weight", &weightPercent,
                0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_None))
        {
            const float strength = weightPercent / 100.0f;
            g_config.weapon_inertia = weightPercent >= 0.5f;
            g_config.weapon_position_follow =
                40.0f - strength * 38.0f;
            g_config.weapon_rotation_follow =
                45.0f - strength * 43.0f;
            changed = true;
        }
        float catchupPercent = g_config.weapon_catchup_speed * 100.0f;
        if (ImGui::SliderFloat(
                "Fast-movement catch-up", &catchupPercent,
                0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_None))
        {
            g_config.weapon_catchup_speed = catchupPercent / 100.0f;
            changed = true;
        }
        ImGui::TextDisabled(
            "Weight controls normal handling; catch-up smoothly pulls harder as\n"
            "the real controller gets farther away. 0%% weight is exact tracking.");
        ImGui::TextDisabled("Head tracking and buttons stay unfiltered.");
        changed |= ImGui::SliderFloat("Weapon size", &g_config.gun_scale, 0.3f, 3.0f, "%.2fx");
        ImGui::TextDisabled("Uniform scale of RIGHT hand + weapon about your grip (Home/End in-game).");
        changed |= ImGui::SliderFloat("Left hand size", &g_config.left_hand_scale,
                                      0.3f, 3.0f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::SmallButton("Match weapon##lhs"))
        { g_config.left_hand_scale = g_config.gun_scale; changed = true; }
        ImGui::TextDisabled("Sizes the left hand, and the second gun when dual-wielding.\n"
                            "1.00 = authored size. Separate because the left hand is\n"
                            "usually empty; use Match weapon for identical hands.\n"
                            "Its front/back position is \"Left hand forward offset\" below.");
        changed |= ImGui::SliderFloat("Weapon pitch (deg)", &g_config.gun_pitch_deg, -180.0f, 180.0f, "%.0f");
        changed |= ImGui::SliderFloat("Weapon yaw (deg)", &g_config.gun_yaw_deg, -180.0f, 180.0f, "%.0f");
        changed |= ImGui::SliderFloat("Weapon roll (deg)", &g_config.gun_roll_deg, -180.0f, 180.0f, "%.0f");
        ImGui::TextDisabled("Pitch, yaw, and roll rotate on their matching local gun axes.");
        ImGui::TextDisabled("0/0/0 keeps the current automatic barrel alignment.");
        changed |= ImGui::SliderFloat("Gun forward offset (m)", &g_config.gun_forward_m, -0.3f, 0.5f, "%.2f");
        ImGui::TextDisabled("Slides gun/arms along your aim. Negative seats the gun back in your fist.");
        changed |= ImGui::SliderFloat("Muzzle height (m)", &g_config.muzzle_height_m, -0.3f, 0.3f, "%.2f");
        ImGui::TextDisabled("HALO REACH ONLY for now - Halo 3 and ODST support is coming soon.");
        ImGui::TextDisabled("Raises the muzzle flash / bullet spawn up the gun's own axis.");
        ImGui::TextDisabled("Where rounds LAND is unchanged. 0.11 is about four inches.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Experimental gun-mounted zoom screen");
        if (ImGui::Checkbox("Enable experimental R3 zoom screen", &g_config.scope_enabled))
        {
            changed = true;
            if (!g_config.scope_enabled)
                VR_SetScopeActive(false);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(VR_IsScopeActive() ? "[R3: visible]" : "[R3: hidden]");
        ImGui::TextDisabled("R3 toggles a fixed-magnification view while the main headset view\n"
                            "stays wide. Placement and zoom are experimental per-user tuning.");
        if (g_config.scope_enabled)
        {
            ImGui::Indent();
            changed |= ImGui::SliderFloat("Default scope zoom", &g_config.scope_zoom,
                                          6.0f, 24.0f, "%.2fx");
            changed |= ImGui::SliderFloat("Screen width (m)##scope",
                                          &g_config.scope_screen_width_m,
                                          0.04f, 0.25f, "%.3f");
            changed |= ImGui::SliderFloat("Screen right offset (m)",
                                          &g_config.scope_screen_right_m,
                                          -0.30f, 0.30f, "%.3f");
            changed |= ImGui::SliderFloat("Screen up offset (m)",
                                          &g_config.scope_screen_up_m,
                                          -0.20f, 0.30f, "%.3f");
            changed |= ImGui::SliderFloat("Screen forward offset (m)",
                                          &g_config.scope_screen_forward_m,
                                          0.05f, 0.80f, "%.3f");
            changed |= ImGui::SliderInt("Image refresh divisor",
                                        &g_config.scope_refresh_divisor, 1, 4);
            ImGui::TextDisabled("Offsets are direct gun-local meters with no hidden added distance.");
            ImGui::TextDisabled("Higher refresh divisors render the zoom image less often; the screen\n"
                                "still follows the gun every frame. Use 4 for the lowest GPU cost.");
            ImGui::Unindent();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Authored weapon crosshair (stereo)");
        changed |= ImGui::Checkbox("Show a crosshair where the weapon shoots", &g_config.crosshair);
        if (g_config.crosshair)
        {
            if (!g_config.weapon_inertia)
            {
                float crosshairSmoothPercent = g_config.aim_stabilization * 100.0f;
                if (ImGui::SliderFloat("Crosshair smoothing", &crosshairSmoothPercent,
                                       0.0f, 95.0f, "%.0f%%", ImGuiSliderFlags_None))
                {
                    g_config.aim_stabilization = crosshairSmoothPercent / 100.0f;
                    changed = true;
                }
            }
            else
                ImGui::TextDisabled("Extra crosshair-only smoothing is bypassed while weapon weight is on.");
            changed |= ImGui::SliderFloat("Crosshair size (deg)", &g_config.crosshair_size_deg,
                                          0.3f, 20.0f, "%.1f");
            changed |= ImGui::SliderFloat("Crosshair distance (m)", &g_config.crosshair_distance_m,
                                          2.0f, 50.0f, "%.0f");
            ImGui::TextDisabled("Uses the equipped weapon's authored crosshair and target colors.");
        }
        ImGui::TextDisabled("With weapon weight off, crosshair smoothing is visual only. With it on,\n"
                            "gun, bullets and crosshair share the same weighted pose.");

        ImGui::Spacing();
        changed |= ImGui::Checkbox("Two-handed aiming", &g_config.two_handed_aim);
        ImGui::SameLine();
        ImGui::TextDisabled(VR_IsTwoHandAiming() ? "[engaged]" : "[one-handed]");
        if (g_config.two_handed_aim)
        {
            ImGui::Indent();
            if (ImGui::RadioButton("Toggle (click grip)", g_config.two_hand_toggle))
            { g_config.two_hand_toggle = true; changed = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Hold grip", !g_config.two_hand_toggle))
            { g_config.two_hand_toggle = false; changed = true; }
            changed |= ImGui::SliderFloat("Left hand forward offset (m)",
                                          &g_config.left_hand_forward_m,
                                          -0.15f, 0.30f, "%.3f");
            ImGui::TextDisabled("Moves the visible support hand and the two-hand aim point together.");
            changed |= ImGui::SliderFloat("Grab zone side offset (m)",
                                          &g_config.two_hand_zone_right_m,
                                          -0.10f, 0.10f, "%.3f");
            ImGui::TextDisabled("Slides the grip-click zone sideways (+ = right) onto the visible barrel.");
            changed |= ImGui::SliderFloat("Left palm depth (m)",
                                          &g_config.left_grip_forward_m,
                                          -0.05f, 0.25f, "%.3f");
            ImGui::TextDisabled("Extends the two-hand grab line and grip-click zone to your visible palm.");
            ImGui::Unindent();
        }
        ImGui::TextDisabled("Put your left hand on the front of the gun, click/hold the LEFT GRIP.\n"
                            "Engages only when your hand is on the barrel line.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Body & Room-scale"))
        {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Body (VRIK)");
        changed |= ImGui::Checkbox("Arm IK (bend arm to controller)", &g_config.arm_ik);
        ImGui::TextDisabled("ON: shoulder stays, elbow bends, hand+gun follow your controller.\n"
                            "OFF: the whole arm rigid-parents to the controller (old behavior).");
        if (g_config.arm_ik)
        {
            changed |= ImGui::SliderFloat("Right shoulder drop", &g_config.right_shoulder_drop,
                                          0.0f, 0.3f, "%.3f");
            ImGui::TextDisabled("Lowers Chief's right arm so it doesn't clip your face.\n"
                                "Raise until the right shoulder matches your left.");
            changed |= ImGui::Checkbox("Level shoulders (don't pitch with head)",
                                       &g_config.shoulder_level);
            ImGui::TextDisabled("ON: shoulders stay level with the horizon when you look up/down.\n"
                                "OFF: shoulders ride your head pitch (old). Hand+gun unaffected.");
        }
        ImGui::Spacing();
        changed |= ImGui::Checkbox("Floating hands (hide arms)", &g_config.floating_hands);
        ImGui::TextDisabled("Shows only your hands and the guns they hold; the arms are hidden.\n"
                            "Hands still track your controllers exactly as with full arms.");
        ImGui::Spacing();
        changed |= ImGui::Checkbox("Show body (VRIK stage A1)", &g_config.body_wip);
        ImGui::TextDisabled("Shows Chief's game-animated body via the engine's own director switches.");
        ImGui::TextDisabled("Room-scale unit movement is gated until the player-biped boundary is headset-proven.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Display & HUD"))
        {
        ImGui::Text("HUD layout");
        changed |= ImGui::SliderFloat("HUD size", &g_config.hud_size, 0.30f, 1.00f, "%.2f");
        changed |= ImGui::SliderFloat("HUD width / aspect", &g_config.hud_aspect,
                                      kHudAspectMin, kHudAspectMax, "%.2f");
        changed |= ImGui::SliderFloat("HUD curvature", &g_config.hud_curvature,
                                      kHudCurvatureMin, kHudCurvatureMax, "%.2f");
        changed |= ImGui::SliderFloat("HUD height", &g_config.hud_vertical_offset,
                                      kHudHeightMin, kHudHeightMax, "%+.0f px");
        if (ImGui::SmallButton("Set VR preset (0.45)##sf"))
        { g_config.hud_size = 0.45f; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset HUD layout##sf"))
        {
            g_config.hud_size = 0.87f;
            g_config.hud_aspect = 1.0f;
            g_config.hud_curvature = 0.5f;
            g_config.hud_vertical_offset = 0.0f;
            changed = true;
        }
        ImGui::TextDisabled("0.87 is the calibrated stock layout; lower pulls HUD elements inward.");
        ImGui::TextDisabled("Width corrects squeeze separately from size; 1.00 uses automatic correction.");
        ImGui::TextDisabled("Curvature: 0.00 = flat (+0.30), 1.00 = curved (-0.30); 0.50 is authored.");
        ImGui::TextDisabled("Height: positive raises the HUD, negative lowers it; the aiming reticle stays fixed.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Picture");
        changed |= ImGui::SliderFloat("Resolution scale", &g_config.resolution_scale,
                                      kResolutionScaleMin, kResolutionScaleMax, "%.2fx");
        // Same even-rounding the launcher applies, so this is the exact render
        // size the next launch will ask MCC for.
        auto scaleEven = [](int base, float scale) {
            int value = (int)lroundf((float)base * scale);
            return (value & 1) ? value + 1 : value;
        };
        ImGui::TextDisabled("Renders %d x %d.", scaleEven(kNativeRenderWidth, g_config.resolution_scale),
                            scaleEven(kNativeRenderHeight, g_config.resolution_scale));
        struct ResolutionPreset { const char* name; float scale; };
        // Tiers span the full 0.35..2.75 range. "Keith David" is true 8K width
        // (7680, scale ~2.64); "Ultra" sits at ~5k, the heavy threshold below.
        // All uniform, so Halo's 2912:2100 VR aspect is preserved at every tier.
        static const ResolutionPreset kPresets[] = {
            {"Potato", 0.50f}, {"Low", 0.75f}, {"Medium", 1.00f},
            {"High", 1.30f}, {"Ultra", 1.80f}, {"Keith David", 2.64f}
        };
        for (int i = 0; i < 6; ++i)
        {
            if (i)
                ImGui::SameLine();
            if (ImGui::SmallButton(kPresets[i].name))
            {
                g_config.resolution_scale = kPresets[i].scale;
                changed = true;
            }
        }
        ImGui::TextDisabled("The buttons are shortcuts; the slider takes any value in between,\n"
                            "as does resolution_scale in halomccvr.cfg. Below 1.00x trades\n"
                            "sharpness for frame rate; above it supersamples. Keith David is\n"
                            "8K-class. Changing this requires a full game restart.");
        if (g_config.resolution_scale > kResolutionScaleHeavy)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                               "[!] Very heavy (~5K and up): can crash weaker GPUs. Test in\n"
                               "    short sessions and drop this if the game won't start.");
        ImGui::Spacing();
        ImGui::Text("Image quality (applies live, every title)");
        const char* upscaleItems[] = {"Linear (old)", "Sharp (strong bicubic)"};
        changed |= ImGui::Combo("Upscale filter", &g_config.upscale_filter,
                                upscaleItems, 2);
        ImGui::TextDisabled("How the game image is scaled to your headset. The game usually\n"
                            "renders BELOW your per-eye headset resolution, so this upscales it.\n"
                            "Sharp keeps edges crisp; Linear is the old soft/shimmery look.");
        changed |= ImGui::SliderFloat("Sharpening", &g_config.sharpness, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("RCAS-based 2x overdrive. 0 = off; 1 = twice the prior maximum.\n"
                            "It uses the same five taps/pass; lower it if the top rings or clips.");
        const char* aaItems[] = {
            "Off", "FXAA", "FXAA Strong", "SMAA 1x", "SMAA 1x + FXAA Strong"};
        changed |= ImGui::Combo("Anti-aliasing", &g_config.aa_mode, aaItems, 5);
        ImGui::TextDisabled("Smooths jagged edges on the finished image, so a mid/low rig doesn't\n"
                            "need a huge render resolution. SMAA 1x is the real 3-stage filter;\n"
                            "the final option adds FXAA Strong for the most aggressive cleanup.\n"
                            "SMAA costs more GPU only when one of its modes is selected.");
        ImGui::Spacing();
        changed |= ImGui::Checkbox("Fit desktop window to my monitor",
                                   &g_config.fit_desktop_window);
        ImGui::TextDisabled(
            "For monitors SMALLER than your render (e.g. a big headset resolution on\n"
            "a 1080p screen), where MCC's window overflows and you can't click the\n"
            "\"Halo 3\" tile or Quit. The headset keeps the full resolution above; only\n"
            "the desktop window shrinks to fit and the GPU downscales into it (no\n"
            "extra render pass, no measurable cost). OFF by default. Takes effect on\n"
            "the next launch -- close MCC and relaunch.");
        changed |= ImGui::SliderFloat("Game brightness", &g_config.game_brightness, 0.5f, 2.0f, "%.2f");
        ImGui::TextDisabled("Brightens/darkens the whole game. 1.0 = the game's own brightness.");
        changed |= ImGui::Checkbox("Motion blur", &g_config.motion_blur);
        ImGui::TextDisabled("Off is the VR standard. In stereo the game's blur is fed the wrong\n"
                            "previous frame and smears bright edges into repeating echoes.");
        changed |= ImGui::SliderFloat("Draw distance", &g_config.draw_distance,
                                      kDrawDistanceMin, kDrawDistanceMax, "%.2f");
        ImGui::TextDisabled("1.00 = full stock draw distance. Lower brings the far plane in toward\n"
                            "you, culling distant terrain/objects (skybox goes first). Most levels\n"
                            "only start culling below ~0.25; the lowest settings clip near geometry\n"
                            "(hard pop-in) for the most frames. Live, all three games.");

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Advanced/Diagnostics"))
        {
        changed |= ImGui::Checkbox("Bone probe (diagnostic)", &g_config.weapon_probe);
        ImGui::TextDisabled("Pushes every composed skeleton 1m left to prove writable palette boundaries.");
        changed |= ImGui::Checkbox("Render right eye first (diagnostic)",
                                   &g_config.right_eye_first);

        ImGui::Spacing();
        ImGui::Separator();
        // These flips were the F4/F5/F7 hotkeys. A phantom F4 (SteamVR sends
        // Alt+F4 on its exit path) kept inverting the controls mid-session, so
        // they are reachable only from this menu now.
        ImGui::Text("Tracking calibration  (yaw %+.0f, pitch %+.0f, up-vector %s)",
                    Game_GetYawSign(), Game_GetPitchSign(), Game_GetWriteUp() ? "on" : "off");
        if (ImGui::Button("Flip yaw"))
            Game_FlipYaw();
        ImGui::SameLine();
        if (ImGui::Button("Flip pitch"))
            Game_FlipPitch();
        ImGui::SameLine();
        if (ImGui::Button("Toggle up-vector"))
            Game_ToggleUp();
        ImGui::TextDisabled("If turning your head left turns the view right, click Flip yaw.");

        ImGui::Spacing();
        ImGui::TextDisabled("Insert toggles hand aim. Sense sticks move/turn; grips = bumpers,\ntriggers = fire/grenade, stick-click = zoom/crouch, left menu = Start.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Start over");
        // Two clicks: a stray VR pointer click here would otherwise wipe a
        // headset-tuned weapon calibration with no undo. Closing the menu
        // disarms it, so a forgotten arm can never fire on the next session.
        if (ImGui::Button(g_resetArmed ? "Click again to confirm reset"
                                       : "Reset ALL settings to defaults"))
        {
            if (g_resetArmed)
            {
                g_config = Config{};
                ConfigSave();
                LOG("config: reset to defaults from the menu");
                g_resetArmed = false;
            }
            else
            {
                g_resetArmed = true;
            }
        }
        if (g_resetArmed)
        {
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                g_resetArmed = false;
        }
        ImGui::TextDisabled("Puts every setting back to the value halomccvr.cfg lists as its\n"
                            "default, including your weapon calibration. Resolution needs a\n"
                            "game restart; everything else applies immediately.");
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }

        static bool dirty = false;
        if (changed)
            dirty = true;
        if (dirty && !ImGui::IsAnyItemActive())
        {
            ConfigSave(); // save once the slider is let go, not every frame
            dirty = false;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("F1 closes this menu. Settings save to halomccvr.cfg automatically.");
        ImGui::End();
    }
} // namespace

bool Menu_Init(HWND gameWindow, ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT rtFormat)
{
    if (g_ready)
        return true;
    g_hwnd = gameWindow;
    g_ctx = context;
    InitializeCriticalSection(&g_cs);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = MENU_W;
    td.Height = MENU_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = rtFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &g_tex)) ||
        FAILED(device->CreateRenderTargetView(g_tex, nullptr, &g_rtv)))
    {
        LOG("menu: render target creation failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;              // don't scatter imgui.ini into the game folder
    io.MouseDrawCursor = true;             // draw the cursor into our texture
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.5f); // legible at panel distance in the headset
    io.FontGlobalScale = 1.5f;

    if (!ImGui_ImplWin32_Init(gameWindow) || !ImGui_ImplDX11_Init(device, context))
    {
        LOG("menu: ImGui backend init failed");
        return false;
    }

    g_origWndProc = (WNDPROC)SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    if (!g_origWndProc)
    {
        LOG("menu: could not hook the game window procedure (%lu)", GetLastError());
        return false;
    }

    // With the fit on, shrink the desktop window to the monitor now that we can
    // catch its resizes. MCC created it at the full render size (which overflows
    // small monitors and put the menu off-screen); the shrink keeps MCC drawing
    // the full-size backbuffer while only the visible window fits. Posted so it
    // runs on the UI thread.
    if (D3D_FitActive())
        PostMessageW(gameWindow, kFitGameWindowMsg, 0, 0);

    g_ready = true;
    LOG("menu ready (F1 to toggle)");
    return true;
}


bool Menu_IsOpen()
{
    return g_ready && g_open;
}

bool Menu_Toggle()
{
    if (!g_ready)
        return false;
    const bool open = !g_open.load(std::memory_order_acquire);
    g_open.store(open, std::memory_order_release);
    g_resetArmed = false; // never leave a reset half-armed across sessions
    LOG("menu %s", open ? "opened" : "closed");
    return open;
}

void Menu_SetVrPointer(bool hit, float u, float v, bool pressed, float scrollY)
{
    if (!g_ready)
        return;
    EnterCriticalSection(&g_cs);
    g_vrPointer.hit = hit;
    g_vrPointer.pressed = pressed;
    g_vrPointer.u = u;
    g_vrPointer.v = v;
    g_vrPointer.scrollY += scrollY;
    LeaveCriticalSection(&g_cs);
}

void Menu_ClearVrPointer()
{
    Menu_SetVrPointer(false, 0.0f, 0.0f, false, 0.0f);
}

ID3D11Texture2D* Menu_Render()
{
    if (!g_ready)
        return nullptr;
    EnterCriticalSection(&g_cs);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)MENU_W, (float)MENU_H); // our texture, not the window
    // The Win32 backend updates the desktop mouse during NewFrame. Apply the
    // VR ray afterward so it is the final pointer sample ImGui consumes.
    ImGuiIO& io = ImGui::GetIO();
    if (g_vrPointer.hit)
        io.AddMousePosEvent(g_vrPointer.u * MENU_W, g_vrPointer.v * MENU_H);
    else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    static bool previousVrPressed = false;
    if (g_vrPointer.pressed != previousVrPressed)
    {
        io.AddMouseButtonEvent(0, g_vrPointer.pressed);
        previousVrPressed = g_vrPointer.pressed;
    }
    if (g_vrPointer.hit && g_vrPointer.scrollY != 0.0f)
        io.AddMouseWheelEvent(0.0f, g_vrPointer.scrollY);
    g_vrPointer.scrollY = 0.0f;
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();

    D3DStateBackup backup;
    backup.Capture(g_ctx);
    const float clear[4] = {0, 0, 0, 0}; // transparent: only the window itself shows on the panel
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    D3D11_VIEWPORT vp{0, 0, (float)MENU_W, (float)MENU_H, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    backup.Restore(g_ctx);

    LeaveCriticalSection(&g_cs);
    return g_tex;
}
