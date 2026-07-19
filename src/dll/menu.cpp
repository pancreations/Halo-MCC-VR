#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "menu.h"
#include "vr.h"
#include "game.h"
#include "d3d_state.h"
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
    // ImGui gets input on the game's window thread but draws on its render
    // thread; this lock keeps the two from touching ImGui at the same time.
    CRITICAL_SECTION g_cs;

    LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
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
            case VK_F1: g_open = !g_open; LOG("menu %s", g_open ? "opened" : "closed"); return 0;
            case VK_F2: Game_ToggleHeadTracking(); return 0;
            case VK_F3: Game_Recenter(); return 0;
            case VK_F4: Game_CycleReticleElement(); return 0; // find/hide the center crosshair
            case VK_F5: Game_ClearReticleElement(); return 0; // undo (show all HUD)
            case VK_F6: Game_TogglePositional(); return 0;
            case VK_F8: Game_PitchTrim(-1); return 0;
            case VK_F9: Game_PitchTrim(+1); return 0;
            case VK_F10: VR_ToggleScreenFollow(); return 0;
            case VK_F11: VR_ToggleStereo(); return 0;
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
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }

    void DrawUI()
    {
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(MENU_W - 32, MENU_H - 32), ImGuiCond_FirstUseEver);
        ImGui::Begin("Halo 3 VR — Settings (M0: virtual screen)", nullptr, ImGuiWindowFlags_NoCollapse);

        // The background-status line also lives at the top of the menu: the
        // floating toast intentionally hides while the menu is open, so this
        // is where an open-menu user sees what's happening.
        {
            char bg[160];
            if (Game_GetStatusText(bg, sizeof(bg)) > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", bg);
        }

        VrStatus st{};
        VR_GetStatus(st);
        ImGui::Text("Runtime: %s", st.runtime);
        ImGui::Text("Session: %s   |   Game: %ux%u @ %.0f fps", st.sessionState, st.gameWidth,
                    st.gameHeight, st.fps);
        ImGui::Separator();
        ImGui::Spacing();

        bool changed = false;
        changed |= ImGui::SliderFloat("Screen width (m)", &g_config.screen_width_m, 1.0f, 10.0f, "%.1f");
        changed |= ImGui::SliderFloat("Screen distance (m)", &g_config.screen_distance_m, 0.5f, 10.0f, "%.1f");

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

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Hand-held weapon");
        changed |= ImGui::SliderFloat("Weapon size", &g_config.gun_scale, 0.3f, 3.0f, "%.2fx");
        ImGui::TextDisabled("Uniform scale of hand + weapon about your grip (Home/End in-game).");
        changed |= ImGui::SliderFloat("Weapon pitch (deg)", &g_config.gun_pitch_deg, -180.0f, 180.0f, "%.0f");
        changed |= ImGui::SliderFloat("Weapon yaw (deg)", &g_config.gun_yaw_deg, -180.0f, 180.0f, "%.0f");
        changed |= ImGui::SliderFloat("Weapon roll (deg)", &g_config.gun_roll_deg, -180.0f, 180.0f, "%.0f");
        ImGui::TextDisabled("Barrel-to-cursor alignment is AUTOMATIC now. These only fine-tune");
        ImGui::TextDisabled("hand posture/roll around that line. 0/0/0 is the right default.");
        changed |= ImGui::SliderFloat("Gun forward offset (m)", &g_config.gun_forward_m, -0.3f, 0.5f, "%.2f");
        ImGui::TextDisabled("Slides gun/arms along your aim. Negative seats the gun back in your fist.");
        changed |= ImGui::Checkbox("Hide game reticle (use VR reticle only)", &g_config.kill_reticle);
        ImGui::TextDisabled("EASIEST: close this menu, have a weapon out, and tap F4 while\n"
                            "watching the center. Each tap hides a different HUD element;\n"
                            "stop when the old crosshair vanishes (F5 undoes it). Or step here:");
        if (g_config.kill_reticle)
        {
            ImGui::Indent();
            uint16_t ids[64];
            const int nids = Game_CopySeenHudIds(ids, 64);
            int cur = -1;
            for (int i = 0; i < nids; ++i)
                if ((int)ids[i] == g_config.reticle_element_id) { cur = i; break; }
            if (g_config.reticle_element_id < 0)
                ImGui::Text("Hiding: nothing picked yet  (%d HUD elements seen)", nids);
            else
                ImGui::Text("Hiding element id 0x%X  (%d of %d)",
                            g_config.reticle_element_id, cur + 1, nids);
            if (ImGui::Button("< prev##ret") && nids > 0)
            { cur = (cur <= 0) ? nids - 1 : cur - 1; g_config.reticle_element_id = ids[cur]; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Step to next element##ret") && nids > 0)
            { cur = (cur + 1 >= nids) ? 0 : cur + 1; g_config.reticle_element_id = ids[cur]; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("None##ret"))
            { g_config.reticle_element_id = -1; changed = true; }
            ImGui::TextDisabled("Have a weapon out, then click 'Step' until the centered\n"
                                "crosshair vanishes, and leave it there.");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Bone probe (diagnostic)", &g_config.weapon_probe);
        ImGui::TextDisabled("Pushes every composed skeleton (bipeds/NPCs + FP) 1m left.\n"
                            "Bodies visibly shifting = their bones are writable (VRIK stage A2).");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("HUD layout");
        changed |= ImGui::SliderFloat("HUD size", &g_config.hud_size, 0.30f, 1.00f, "%.2f");
        if (ImGui::SmallButton("Set VR preset (0.45)##sf"))
        { g_config.hud_size = 0.45f; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Back to stock (0.87)##sf"))
        { g_config.hud_size = 0.87f; changed = true; }
        ImGui::TextDisabled("Fraction of the view the HUD lays out into. 0.87 = Halo stock.\n"
                            "Lower pulls shields/radar/ammo toward the center of your eyes.\n"
                            "Small nudges (0.84) are invisible - use the preset to SEE it work.");
        {
            int sfMatches; bool sfScanning;
            Game_GetHudSafeFrameStatus(sfMatches, sfScanning);
            const bool sfStock = g_config.hud_size >= 0.8695f && g_config.hud_size <= 0.8705f;
            if (sfScanning)          ImGui::TextDisabled("status: locating HUD data (takes ~20s)...");
            else if (sfMatches > 0)  ImGui::TextDisabled("status: active (%d tag slot pair(s))", sfMatches);
            else if (sfStock)        ImGui::TextDisabled("status: stock size — nothing applied");
            else                     ImGui::TextDisabled("status: waiting for a level (auto-locates)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Rescan##sf"))
                Game_LocateHudSafeFrames();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Aim crosshair (stereo)");
        changed |= ImGui::Checkbox("Show a crosshair where the weapon shoots", &g_config.crosshair);
        if (g_config.crosshair)
        {
            changed |= ImGui::SliderFloat("Crosshair size (deg)", &g_config.crosshair_size_deg,
                                          0.3f, 5.0f, "%.1f");
            changed |= ImGui::SliderFloat("Crosshair distance (m)", &g_config.crosshair_distance_m,
                                          2.0f, 50.0f, "%.0f");
            float col[3] = {g_config.reticle_r, g_config.reticle_g, g_config.reticle_b};
            if (ImGui::ColorEdit3("Crosshair color", col))
            {
                g_config.reticle_r = col[0];
                g_config.reticle_g = col[1];
                g_config.reticle_b = col[2];
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Halo blue"))
            {
                g_config.reticle_r = 0.62f;
                g_config.reticle_g = 0.87f;
                g_config.reticle_b = 1.0f;
                changed = true;
            }
        }
        ImGui::TextDisabled("The game's own reticle follows your head, not your hand; this one is true.\n"
                            "It turns red over enemies (once target-lock is wired in).");

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
            ImGui::Unindent();
        }
        ImGui::TextDisabled("Put your left hand on the front of the gun, click/hold the LEFT GRIP.\n"
                            "Engages only when your hand is on the barrel line.");

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
        changed |= ImGui::Checkbox("Show body (VRIK stage A1)", &g_config.body_wip);
        ImGui::TextDisabled("Shows Chief's game-animated body via the engine's own director switches.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Picture");
        changed |= ImGui::SliderFloat("Game brightness", &g_config.game_brightness, 0.5f, 2.0f, "%.2f");
        ImGui::TextDisabled("Brightens/darkens the whole game. 1.0 = the game's own brightness.");
        changed |= ImGui::Checkbox("Motion blur", &g_config.motion_blur);
        ImGui::TextDisabled("Off is the VR standard. In stereo the game's blur is fed the wrong\n"
                            "previous frame and smears bright edges into repeating echoes.");

        ImGui::Spacing();
        changed |= ImGui::Checkbox("Auto-enter VR on level load", &g_config.auto_vr);
        ImGui::TextDisabled("Turns head tracking + stereo on when a level starts and off in the\n"
                            "menu - no F2/F11. F2 still toggles by hand (and vetoes auto until reload).");

        ImGui::Spacing();
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

        static bool dirty = false;
        if (changed)
            dirty = true;
        if (dirty && !ImGui::IsAnyItemActive())
        {
            ConfigSave(); // save once the slider is let go, not every frame
            dirty = false;
        }

        ImGui::Spacing();
        if (ImGui::Button("Re-center screen"))
            VR_RequestRecenter();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("F1 closes this menu. Settings save to halo3xr.cfg automatically.");
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

    g_ready = true;
    LOG("menu ready (F1 to toggle)");
    return true;
}


bool Menu_IsOpen()
{
    return g_ready && g_open;
}

// Toast: one line of background status (engine hooking, HUD data scan) shown
// in the headset while the menu is CLOSED, so waits never look like freezes.
// Text lives here so the render below can reuse it on the same thread.
static char g_toastText[160];

bool Menu_HasToast()
{
    if (!g_ready)
        return false;
    const bool has = Game_GetStatusText(g_toastText, sizeof(g_toastText)) > 0;
    // Diagnostic trail (2026-07-19: the user reported never seeing a toast):
    // log every state change so the log proves whether this path ran at all.
    // Compare only a prefix so per-second counter text doesn't spam the log.
    static char lastLogged[160] = "";
    if (has && strncmp(g_toastText, lastLogged, 24) != 0)
    {
        strncpy_s(lastLogged, g_toastText, _TRUNCATE);
        LOG("TOAST: %s", g_toastText);
    }
    else if (!has && lastLogged[0])
    {
        lastLogged[0] = 0;
        LOG("TOAST: cleared");
    }
    return has;
}

static void DrawToast()
{
    ImGui::SetNextWindowPos(ImVec2(MENU_W * 0.5f, (float)(MENU_H - 48)),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(g_toastText);
    ImGui::End();
}

ID3D11Texture2D* Menu_Render()
{
    if (!g_ready)
        return nullptr;
    EnterCriticalSection(&g_cs);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)MENU_W, (float)MENU_H); // our texture, not the window
    ImGui::NewFrame();
    if (g_open)
        DrawUI();       // full settings menu
    else
        DrawToast();    // menu closed: only the small status line
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
