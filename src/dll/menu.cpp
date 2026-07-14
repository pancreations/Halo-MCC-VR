#include <windows.h>
#include <windowsx.h>
#include <atomic>
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

        if (msg == WM_KEYDOWN && !(lp & (1 << 30))) // ignore key auto-repeat
        {
            switch (wp)
            {
            case VK_F1: g_open = !g_open; LOG("menu %s", g_open ? "opened" : "closed"); return 0;
            case VK_F2: Game_ToggleHeadTracking(); return 0;
            case VK_F3: Game_Recenter(); return 0;
            case VK_F4: Game_FlipYaw(); return 0;
            case VK_F5: Game_FlipPitch(); return 0;
            case VK_F6: Game_CycleTarget(); return 0;
            case VK_F7: Game_ToggleUp(); return 0;
            case VK_F8: Game_PitchTrim(-1); return 0;
            case VK_F9: Game_PitchTrim(+1); return 0;
            case VK_F10: VR_ToggleScreenFollow(); return 0;
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
            if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
                return 0;
        }
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }

    void DrawUI()
    {
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(MENU_W - 32, MENU_H - 32), ImGuiCond_FirstUseEver);
        ImGui::Begin("Halo 3 VR — Settings (M0: virtual screen)", nullptr, ImGuiWindowFlags_NoCollapse);

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

ID3D11Texture2D* Menu_Render()
{
    if (!g_ready)
        return nullptr;
    EnterCriticalSection(&g_cs);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)MENU_W, (float)MENU_H); // our texture, not the window
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
