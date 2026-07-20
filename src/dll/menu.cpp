#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <cfloat>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "menu.h"
#include "vr.h"
#include "game.h"
#include "title_adapter.h"
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
    struct VrPointerInput
    {
        bool hit = false;
        bool pressed = false;
        float u = 0.0f;
        float v = 0.0f;
        float scrollY = 0.0f;
    };
    VrPointerInput g_vrPointer;
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
            case VK_F1: Menu_Toggle(); return 0;
            case VK_F2: Game_ToggleHeadTracking(); return 0;
            case VK_F3: Game_Recenter(); return 0;
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
                ? "Resume Halo (Start)"
                : "Pause Halo in 2D (Start)"))
        {
            Input_RequestPauseToggle();
            Menu_Toggle();
        }
        ImGui::TextDisabled("PSVR2 fallback: press Y+B together to Pause/Resume.");
        ImGui::Separator();
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
        ImGui::TextDisabled("Hides every native CHUD crosshair class.\n"
                            "The motion-control VR reticle stays visible.");
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
            changed |= ImGui::SliderFloat("Left hand forward offset (m)",
                                          &g_config.left_hand_forward_m,
                                          -0.15f, 0.30f, "%.3f");
            ImGui::TextDisabled("Moves the visible support hand and the two-hand aim point together.");
            changed |= ImGui::SliderFloat("Grab zone side offset (m)",
                                          &g_config.two_hand_zone_right_m,
                                          -0.10f, 0.10f, "%.3f");
            ImGui::TextDisabled("Slides the grip-click zone sideways (+ = right) onto the visible barrel.");
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
        changed |= ImGui::Checkbox("Show body (VRIK stage A1)", &g_config.body_wip);
        ImGui::TextDisabled("Shows Chief's game-animated body via the engine's own director switches.");
        ImGui::TextDisabled("Room-scale unit movement is gated until the player-biped boundary is headset-proven.");
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Display & HUD"))
        {
        ImGui::Text("HUD layout");
        changed |= ImGui::SliderFloat("HUD size", &g_config.hud_size, 0.30f, 1.00f, "%.2f");
        if (ImGui::SmallButton("Set VR preset (0.45)##sf"))
        { g_config.hud_size = 0.45f; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Back to stock (0.87)##sf"))
        { g_config.hud_size = 0.87f; changed = true; }
        ImGui::TextDisabled("0.87 is Halo stock; lower values pull HUD elements toward both eyes.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Picture");
        const char* resolutionPresets[] = {
            "Potato (50%)", "Low (67%)", "Medium (80%)",
            "High (100%)", "Ultra (110%)"
        };
        const float resolutionScales[] = {0.50f, 0.67f, 0.80f, 1.00f, 1.10f};
        int resolutionPreset = 3;
        if (g_config.resolution_scale < 0.585f)
            resolutionPreset = 0;
        else if (g_config.resolution_scale < 0.735f)
            resolutionPreset = 1;
        else if (g_config.resolution_scale < 0.90f)
            resolutionPreset = 2;
        else if (g_config.resolution_scale >= 1.05f)
            resolutionPreset = 4;
        else
            resolutionPreset = 3;
        if (ImGui::Combo("Resolution preset", &resolutionPreset,
                         resolutionPresets, 5))
        {
            g_config.resolution_scale = resolutionScales[resolutionPreset];
            changed = true;
        }
        ImGui::TextDisabled("Potato/Low/Medium reduce pixel count; Ultra supersamples.\n"
                            "Changing this requires a full game restart. Close MCC and relaunch.");
        changed |= ImGui::SliderFloat("Game brightness", &g_config.game_brightness, 0.5f, 2.0f, "%.2f");
        ImGui::TextDisabled("Brightens/darkens the whole game. 1.0 = the game's own brightness.");
        changed |= ImGui::Checkbox("Motion blur", &g_config.motion_blur);
        ImGui::TextDisabled("Off is the VR standard. In stereo the game's blur is fed the wrong\n"
                            "previous frame and smears bright edges into repeating echoes.");

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
