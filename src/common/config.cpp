#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <algorithm>
#include "config.h"
#include "log.h"

Config g_config;
static std::wstring g_path;

static bool ParseFloatSetting(const char* key, const char* text, float& destination)
{
    char* end = nullptr;
    errno = 0;
    const float parsed = strtof(text, &end);
    while (end && isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (end == text || !end || *end != 0 || errno == ERANGE || !std::isfinite(parsed))
    {
        LOG("config: malformed value for '%s' ignored; keeping %.3f", key, destination);
        return false;
    }
    destination = parsed;
    return true;
}

static bool FileExists(const wchar_t* path)
{
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void Clamp()
{
    g_config.config_version = 4;
    g_config.haptic_intensity = std::clamp(g_config.haptic_intensity, 0.0f, 1.0f);
    g_config.headset_smoothing = std::clamp(g_config.headset_smoothing, 0.0f, 0.10f);
    g_config.aim_stabilization = std::clamp(g_config.aim_stabilization, 0.0f, 0.95f);
    g_config.screen_width_m = std::clamp(g_config.screen_width_m, 0.5f, 20.0f);
    g_config.screen_distance_m = std::clamp(g_config.screen_distance_m, 0.3f, 20.0f);
    g_config.turn_snap_deg = std::clamp(g_config.turn_snap_deg, 5.0f, 90.0f);
    g_config.turn_smooth_deg_s = std::clamp(g_config.turn_smooth_deg_s, 30.0f, 360.0f);
    g_config.crosshair_distance_m = std::clamp(g_config.crosshair_distance_m, 2.0f, 50.0f);
    g_config.crosshair_size_deg = std::clamp(g_config.crosshair_size_deg, 0.3f, 20.0f);
    g_config.reticle_r = std::clamp(g_config.reticle_r, 0.0f, 1.0f);
    g_config.reticle_g = std::clamp(g_config.reticle_g, 0.0f, 1.0f);
    g_config.reticle_b = std::clamp(g_config.reticle_b, 0.0f, 1.0f);
    g_config.gun_scale = std::clamp(g_config.gun_scale, 0.3f, 3.0f);
    g_config.left_hand_scale = std::clamp(g_config.left_hand_scale, 0.3f, 3.0f);
    g_config.game_brightness = std::clamp(g_config.game_brightness, 0.5f, 2.0f);
    // Free-form: a hand-typed 0.90 stays 0.90. (Until 2026-07-20 this snapped
    // to the six installer tiers, so any custom value was silently rounded.)
    g_config.resolution_scale = std::clamp(g_config.resolution_scale,
                                           kResolutionScaleMin, kResolutionScaleMax);
    g_config.hud_size = std::clamp(g_config.hud_size, 0.30f, 1.00f);
    g_config.hud_aspect = std::clamp(g_config.hud_aspect, kHudAspectMin, kHudAspectMax);
    g_config.hud_curvature = std::clamp(g_config.hud_curvature,
                                        kHudCurvatureMin, kHudCurvatureMax);
    g_config.hud_vertical_offset = std::clamp(g_config.hud_vertical_offset,
                                              kHudHeightMin, kHudHeightMax);
    g_config.left_hand_forward_m = std::clamp(g_config.left_hand_forward_m, -0.15f, 0.30f);
    g_config.two_hand_zone_right_m = std::clamp(g_config.two_hand_zone_right_m, -0.10f, 0.10f);
    g_config.left_grip_forward_m = std::clamp(g_config.left_grip_forward_m, -0.05f, 0.25f);
    g_config.right_shoulder_drop = std::clamp(g_config.right_shoulder_drop, 0.0f, 0.3f);
    g_config.shoulder_back_m = std::clamp(g_config.shoulder_back_m, -0.3f, 0.3f);
    g_config.gun_pitch_deg = std::clamp(g_config.gun_pitch_deg, -180.0f, 180.0f);
    g_config.gun_yaw_deg = std::clamp(g_config.gun_yaw_deg, -180.0f, 180.0f);
    g_config.gun_roll_deg = std::clamp(g_config.gun_roll_deg, -180.0f, 180.0f);
    g_config.gun_forward_m = std::clamp(g_config.gun_forward_m, -0.3f, 0.5f);
    g_config.scope_zoom = std::clamp(g_config.scope_zoom, 6.0f, 24.0f);
    g_config.scope_screen_width_m = std::clamp(g_config.scope_screen_width_m, 0.04f, 0.25f);
    g_config.scope_screen_right_m = std::clamp(g_config.scope_screen_right_m, -0.30f, 0.30f);
    g_config.scope_screen_up_m = std::clamp(g_config.scope_screen_up_m, -0.20f, 0.30f);
    g_config.scope_screen_forward_m = std::clamp(g_config.scope_screen_forward_m, 0.05f, 0.80f);
    g_config.scope_refresh_divisor = std::clamp(g_config.scope_refresh_divisor, 1, 4);
}

void ConfigLoad(const wchar_t* path)
{
    g_config = Config{};
    g_path = path;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rt");
    if (!f)
    {
        LOG("config: no file yet, writing defaults");
        ConfigSave();
        return;
    }
    char line[512];
    int loadedConfigVersion = 1;
    bool loadedLegacyCurvature = false;
    bool loadedScopeZoom = false;
    while (fgets(line, sizeof(line), f))
    {
        if (char* hash = strchr(line, '#'))
            *hash = 0;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        auto trim = [](char* s) -> char* {
            while (isspace((unsigned char)*s)) s++;
            char* e = s + strlen(s);
            while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
            return s;
        };
        const char* key = trim(line);
        const char* val = trim(eq + 1);
        if (!strcmp(key, "config_version"))
        {
            char* end = nullptr;
            const long parsed = strtol(val, &end, 10);
            while (end && isspace(static_cast<unsigned char>(*end)))
                ++end;
            if (end == val || !end || *end != 0 || parsed < 1)
                LOG("config: malformed value for 'config_version' ignored; using version 1");
            else
            {
                loadedConfigVersion = static_cast<int>(parsed);
                if (parsed > 4)
                    LOG("config: version %ld is newer than supported version 4; known keys will be loaded", parsed);
            }
        }
        else if (!strcmp(key, "haptic_intensity"))
            ParseFloatSetting(key, val, g_config.haptic_intensity);
        else if (!strcmp(key, "headset_smoothing"))
            ParseFloatSetting(key, val, g_config.headset_smoothing);
        else if (!strcmp(key, "aim_stabilization"))
            ParseFloatSetting(key, val, g_config.aim_stabilization);
        else if (!strcmp(key, "screen_width_m"))
            g_config.screen_width_m = (float)atof(val);
        else if (!strcmp(key, "screen_distance_m"))
            g_config.screen_distance_m = (float)atof(val);
        else if (!strcmp(key, "turn_smooth"))
            g_config.turn_smooth = atoi(val) != 0;
        else if (!strcmp(key, "turn_snap_deg"))
            g_config.turn_snap_deg = (float)atof(val);
        else if (!strcmp(key, "turn_smooth_deg_s"))
            g_config.turn_smooth_deg_s = (float)atof(val);
        else if (!strcmp(key, "ghost_fix") || !strcmp(key, "stereo_alternate_order") ||
                 !strcmp(key, "stereo_warmup_pass") || !strcmp(key, "per_eye_history") ||
                 !strcmp(key, "stereo_sun_shafts") || !strcmp(key, "gun_length_scale"))
            continue; // retired switches; accept old config files quietly
        else if (!strcmp(key, "dpad_hand"))
            g_config.dpad_hand = atoi(val) != 0 ? 1 : 0;
        else if (!strcmp(key, "crosshair"))
            g_config.crosshair = atoi(val) != 0;
        else if (!strcmp(key, "crosshair_distance_m"))
            g_config.crosshair_distance_m = (float)atof(val);
        else if (!strcmp(key, "crosshair_size_deg"))
            g_config.crosshair_size_deg = (float)atof(val);
        else if (!strcmp(key, "reticle_r"))
            g_config.reticle_r = (float)atof(val);
        else if (!strcmp(key, "reticle_g"))
            g_config.reticle_g = (float)atof(val);
        else if (!strcmp(key, "reticle_b"))
            g_config.reticle_b = (float)atof(val);
        else if (!strcmp(key, "gun_scale"))
            g_config.gun_scale = (float)atof(val);
        else if (!strcmp(key, "left_hand_scale"))
            g_config.left_hand_scale = (float)atof(val);
        else if (!strcmp(key, "bullet_snap"))
            continue; // retired: the composed-wrist snap was reverted (hand spin); accept old cfgs quietly
        else if (!strcmp(key, "hud_probe"))
            g_config.hud_probe = atoi(val) != 0;
        else if (!strcmp(key, "bullet_probe"))
            g_config.bullet_probe = atoi(val) != 0;
        else if (!strcmp(key, "weapon_probe"))
            g_config.weapon_probe = atoi(val) != 0;
        else if (!strcmp(key, "gun_pitch_deg"))
            g_config.gun_pitch_deg = (float)atof(val);
        else if (!strcmp(key, "gun_yaw_deg"))
            g_config.gun_yaw_deg = (float)atof(val);
        else if (!strcmp(key, "gun_roll_deg"))
            g_config.gun_roll_deg = (float)atof(val);
        else if (!strcmp(key, "gun_forward_m"))
            g_config.gun_forward_m = (float)atof(val);
        else if (!strcmp(key, "scope_enabled"))
            g_config.scope_enabled = atoi(val) != 0;
        else if (!strcmp(key, "scope_zoom"))
        {
            g_config.scope_zoom = (float)atof(val);
            loadedScopeZoom = true;
        }
        else if (!strcmp(key, "scope_screen_width_m"))
            g_config.scope_screen_width_m = (float)atof(val);
        else if (!strcmp(key, "scope_screen_right_m"))
            g_config.scope_screen_right_m = (float)atof(val);
        else if (!strcmp(key, "scope_screen_up_m"))
            g_config.scope_screen_up_m = (float)atof(val);
        else if (!strcmp(key, "scope_screen_forward_m"))
            g_config.scope_screen_forward_m = (float)atof(val);
        else if (!strcmp(key, "scope_refresh_divisor"))
            g_config.scope_refresh_divisor = atoi(val);
        else if (!strcmp(key, "scope_magnification") ||
                 !strcmp(key, "scope_size_deg") ||
                 !strcmp(key, "scope_up_m") ||
                 !strcmp(key, "scope_forward_m"))
            continue; // retire the failed diagnostic-scope coordinates quietly
        else if (!strcmp(key, "show_hud") || !strcmp(key, "hud_ammo") ||
                 !strcmp(key, "hud_health") || !strcmp(key, "hud_motion") ||
                 !strcmp(key, "hud_grenades"))
            continue; // retired: chud byte writes used a disproven offset map; accept old cfgs quietly
        else if (!strcmp(key, "kill_reticle"))
            g_config.kill_reticle = atoi(val) != 0;
        else if (!strcmp(key, "reticle_element_id"))
            continue; // retired runtime tag-index picker; accept old configs quietly
        else if (!strcmp(key, "game_brightness"))
            g_config.game_brightness = (float)atof(val);
        else if (!strcmp(key, "resolution_scale"))
            g_config.resolution_scale = (float)atof(val);
        else if (!strcmp(key, "hud_size"))
            g_config.hud_size = (float)atof(val);
        else if (!strcmp(key, "hud_aspect"))
            ParseFloatSetting(key, val, g_config.hud_aspect);
        else if (!strcmp(key, "hud_curvature") || !strcmp(key, "hud_height"))
        {
            ParseFloatSetting(key, val, g_config.hud_curvature);
            loadedLegacyCurvature = loadedConfigVersion < 2 || !strcmp(key, "hud_height");
        }
        else if (!strcmp(key, "hud_vertical_offset"))
            ParseFloatSetting(key, val, g_config.hud_vertical_offset);
        else if (!strcmp(key, "hud_offset_x") || !strcmp(key, "hud_offset_y") ||
                 !strcmp(key, "hud_elem_scale"))
            continue; // retired placement-experiment keys; accept old cfgs quietly
        else if (!strcmp(key, "hud_scale") || !strcmp(key, "hud_forward") ||
                 !strcmp(key, "hud_fov_scale") || !strcmp(key, "hud_zoom") ||
                 !strcmp(key, "hud_panel") || !strcmp(key, "hud_panel_size_m") ||
                 !strcmp(key, "hud_panel_dist_m"))
            continue; // retired levers (hud_scale was brightness; hud_zoom + the capture-diff panel both disproven); accept old cfgs quietly
        else if (!strcmp(key, "auto_vr"))
            g_config.auto_vr = atoi(val) != 0;
        else if (!strcmp(key, "two_handed_aim"))
            g_config.two_handed_aim = atoi(val) != 0;
        else if (!strcmp(key, "two_hand_toggle"))
            g_config.two_hand_toggle = atoi(val) != 0;
        else if (!strcmp(key, "left_hand_forward_m"))
            g_config.left_hand_forward_m = (float)atof(val);
        else if (!strcmp(key, "two_hand_zone_right_m"))
            g_config.two_hand_zone_right_m = (float)atof(val);
        else if (!strcmp(key, "left_grip_forward_m"))
            g_config.left_grip_forward_m = (float)atof(val);
        else if (!strcmp(key, "crouch_by_height") || !strcmp(key, "crouch_threshold_m"))
            continue; // removed feature; accept old config files quietly
        else if (!strcmp(key, "body_wip"))
            g_config.body_wip = atoi(val) != 0;
        else if (!strcmp(key, "arm_ik"))
            g_config.arm_ik = atoi(val) != 0;
        else if (!strcmp(key, "floating_hands"))
            g_config.floating_hands = atoi(val) != 0;
        else if (!strcmp(key, "right_shoulder_drop"))
            g_config.right_shoulder_drop = (float)atof(val);
        else if (!strcmp(key, "shoulder_back_m"))
            g_config.shoulder_back_m = (float)atof(val);
        else if (!strcmp(key, "shoulder_level"))
            g_config.shoulder_level = atoi(val) != 0;
        else if (!strcmp(key, "motion_blur"))
            g_config.motion_blur = atoi(val) != 0;
        else if (!strcmp(key, "right_eye_first"))
            g_config.right_eye_first = atoi(val) != 0;
        else if (*key)
            LOG("config: unknown key '%s' ignored", key);
    }
    fclose(f);
    if (loadedConfigVersion < 3 && loadedScopeZoom)
    {
        // Version 3 moved the scope to the gameplay/bullet origin. The former
        // crosshair-origin calibration is too wide there, so preserve the
        // user's relative setting while doubling the available lens strength.
        g_config.scope_zoom *= 2.0f;
        LOG("config: migrated scope zoom to the stronger gameplay-origin lens");
    }
    if (loadedConfigVersion < 4 && loadedScopeZoom)
    {
        // The first gameplay-origin headset pass was still much too wide. Keep
        // the prior relative setting but move it into the tighter 6x..24x lens.
        g_config.scope_zoom *= 1.75f;
        LOG("config: migrated scope zoom to the tighter world-only lens");
    }
    if (loadedLegacyCurvature)
    {
        // Version 1 stored a signed value whose physical delta was value*0.1.
        // Preserve that exact curve when migrating to normalized 0(flat)..1(curved).
        const float legacyDelta = g_config.hud_curvature * 0.1f;
        g_config.hud_curvature = (0.30f - legacyDelta) / 0.60f;
    }
    Clamp();
    LOG("config: loaded (screen %.2fm wide at %.2fm)", g_config.screen_width_m, g_config.screen_distance_m);
}

void ConfigLoadMigrating(const wchar_t* primaryPath, const wchar_t* legacyPath)
{
    if (FileExists(primaryPath))
    {
        ConfigLoad(primaryPath);
        return;
    }
    if (legacyPath && FileExists(legacyPath))
    {
        ConfigLoad(legacyPath);
        g_path = primaryPath;
        ConfigSave();
        LOG("config: imported legacy %ls into %ls (legacy file retained)",
            legacyPath, primaryPath);
        return;
    }
    ConfigLoad(primaryPath);
}

void ConfigSave()
{
    if (g_path.empty())
        return;
    Clamp();
    FILE* f = nullptr;
    _wfopen_s(&f, g_path.c_str(), L"wt");
    if (!f)
    {
        LOG("config: FAILED to write %ls", g_path.c_str());
        return;
    }
    const Config d{}; // struct defaults, printed beside each setting

    fprintf(f, "# ===================================================================\n");
    fprintf(f, "#  Halo MCC VR settings\n");
    fprintf(f, "# ===================================================================\n");
    fprintf(f, "#  Edit this file in Notepad with MCC CLOSED, or press F1 in game.\n");
    fprintf(f, "#  Every setting below lists what it does and its default value, so\n");
    fprintf(f, "#  you can always put one back the way it was.\n");
    fprintf(f, "#  This ONE file is shared by every supported MCC game. Your comfort,\n");
    fprintf(f, "#  control, aiming, and presentation preferences follow you between\n");
    fprintf(f, "#  titles; each game keeps its own internal engine calibration.\n");
    fprintf(f, "#\n");
    fprintf(f, "#  * Lost? Close MCC, DELETE this file, and start the game. A fresh\n");
    fprintf(f, "#    one with all the defaults is written for you. Nothing else in\n");
    fprintf(f, "#    the mod folder is affected.\n");
    fprintf(f, "#  * The F1 menu rewrites this whole file when it saves, so notes you\n");
    fprintf(f, "#    type in yourself will disappear. Your VALUES are always kept.\n");
    fprintf(f, "#  * resolution_scale needs a full game restart. Everything else\n");
    fprintf(f, "#    takes effect the next time you launch, or live in the F1 menu.\n");
    fprintf(f, "#  * A line the mod does not recognize, or a value that is not a\n");
    fprintf(f, "#    number, is ignored and noted in halo3xr.log. It cannot break\n");
    fprintf(f, "#    the mod, and out-of-range numbers are pulled back into range.\n");
    fprintf(f, "# ===================================================================\n\n");
    fprintf(f, "config_version = %d\n\n", g_config.config_version);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  OPENXR & COMFORT\n");
    fprintf(f, "#  Headset, controller feedback, and the shared 2D menu screen.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# OpenXR controller vibration strength, 0 = off and 1 = full.\n");
    fprintf(f, "# (default %.2f, range 0 to 1)\n", d.haptic_intensity);
    fprintf(f, "haptic_intensity = %.2f\n\n", g_config.haptic_intensity);
    fprintf(f, "# Headset micro-smoothing, 0 = raw. Try 0.05 only for micro-jitter.\n");
    fprintf(f, "# (default %.2f, range 0 to 0.10)\n", d.headset_smoothing);
    fprintf(f, "headset_smoothing = %.2f\n\n", g_config.headset_smoothing);
    fprintf(f, "# Width of the virtual screen in meters (menus / 2D mode).\n");
    fprintf(f, "# (default %.2f, range 0.5 to 20)\n", d.screen_width_m);
    fprintf(f, "screen_width_m = %.2f\n\n", g_config.screen_width_m);
    fprintf(f, "# Distance from your head to that screen, in meters.\n");
    fprintf(f, "# (default %.2f, range 0.3 to 20)\n", d.screen_distance_m);
    fprintf(f, "screen_distance_m = %.2f\n\n", g_config.screen_distance_m);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  CONTROLS & TURNING\n");
    fprintf(f, "#  Universal controller choices used in every supported title.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# VR turning with the right controller stick: 0 = snap, 1 = smooth.\n");
    fprintf(f, "# (default %d)\n", d.turn_smooth ? 1 : 0);
    fprintf(f, "turn_smooth = %d\n\n", g_config.turn_smooth ? 1 : 0);
    fprintf(f, "# Degrees per snap turn.\n");
    fprintf(f, "# (default %.0f, range 5 to 90)\n", d.turn_snap_deg);
    fprintf(f, "turn_snap_deg = %.0f\n\n", g_config.turn_snap_deg);
    fprintf(f, "# Smooth turn speed in degrees per second.\n");
    fprintf(f, "# (default %.0f, range 30 to 360)\n", d.turn_smooth_deg_s);
    fprintf(f, "turn_smooth_deg_s = %.0f\n\n", g_config.turn_smooth_deg_s);
    fprintf(f, "# Hold this controller next to your head to use the left stick as D-pad:\n");
    fprintf(f, "# 0 = left controller, 1 = right controller.\n");
    fprintf(f, "# (default %d)\n", d.dpad_hand);
    fprintf(f, "dpad_hand = %d\n\n", g_config.dpad_hand);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  RETICLE & AIMING\n");
    fprintf(f, "#  Portable aiming preferences; title adapters supply engine offsets.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Floating VR-crosshair smoothing only; bullets stay raw.\n");
    fprintf(f, "# (default %.2f, range 0 to 0.95)\n", d.aim_stabilization);
    fprintf(f, "aim_stabilization = %.2f\n\n", g_config.aim_stabilization);
    fprintf(f, "# Aim crosshair in stereo: a floating reticle where the weapon actually\n");
    fprintf(f, "# shoots (the game's own reticle follows your head, not your hand).\n");
    fprintf(f, "# (default %d)\n", d.crosshair ? 1 : 0);
    fprintf(f, "crosshair = %d\n\n", g_config.crosshair ? 1 : 0);
    fprintf(f, "# How far away that crosshair floats, in meters.\n");
    fprintf(f, "# (default %.1f, range 2 to 50)\n", d.crosshair_distance_m);
    fprintf(f, "crosshair_distance_m = %.1f\n\n", g_config.crosshair_distance_m);
    fprintf(f, "# Apparent size of the crosshair, in degrees.\n");
    fprintf(f, "# (default %.2f, range 0.3 to 20)\n", d.crosshair_size_deg);
    fprintf(f, "crosshair_size_deg = %.2f\n\n", g_config.crosshair_size_deg);
    fprintf(f, "# Crosshair color, 0-1 per channel. Not in the F1 menu; this file only.\n");
    fprintf(f, "# (defaults %.3f / %.3f / %.3f = light blue, range 0 to 1)\n",
             d.reticle_r, d.reticle_g, d.reticle_b);
    fprintf(f, "reticle_r = %.3f\n", g_config.reticle_r);
    fprintf(f, "reticle_g = %.3f\n", g_config.reticle_g);
    fprintf(f, "reticle_b = %.3f\n\n", g_config.reticle_b);
    fprintf(f, "# Hide the native head-centered crosshair after the floating\n");
    fprintf(f, "# motion-control reticle is ready. Set 0 for an emergency native fallback.\n");
    fprintf(f, "# (default %d)\n", d.kill_reticle ? 1 : 0);
    fprintf(f, "kill_reticle = %d\n\n", g_config.kill_reticle ? 1 : 0);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  WEAPON CALIBRATION\n");
    fprintf(f, "#  Personal trims applied over the active title's verified base pose.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Size of the right hand and the weapon it holds. Home/End adjust it\n");
    fprintf(f, "# in-game. 1.00 = the size the active game authored the model at.\n");
    fprintf(f, "# (default %.2f, range 0.3 to 3)\n", d.gun_scale);
    fprintf(f, "gun_scale = %.2f\n\n", g_config.gun_scale);
    fprintf(f, "# Size of the LEFT hand, and of the second gun when dual-wielding.\n");
    fprintf(f, "# Separate from gun_scale because the left hand is usually empty.\n");
    fprintf(f, "# Set it to the same number as gun_scale for matching hands.\n");
    fprintf(f, "# (default %.2f, range 0.3 to 3)\n", d.left_hand_scale);
    fprintf(f, "left_hand_scale = %.2f\n\n", g_config.left_hand_scale);
    fprintf(f, "# Weapon mounting rotation on the controller, in degrees. Rotates only\n");
    fprintf(f, "# the visible gun; the cursor/bullet ray stays fixed on the controller.\n");
    fprintf(f, "# (defaults %.0f / %.0f / %.0f, range -180 to 180)\n",
            d.gun_pitch_deg, d.gun_yaw_deg, d.gun_roll_deg);
    fprintf(f, "gun_pitch_deg = %.0f\n", g_config.gun_pitch_deg);
    fprintf(f, "gun_yaw_deg = %.0f\n", g_config.gun_yaw_deg);
    fprintf(f, "gun_roll_deg = %.0f\n\n", g_config.gun_roll_deg);
    fprintf(f, "# Push the gun forward of the controller, in meters.\n");
    fprintf(f, "# Negative seats the gun back into your fist.\n");
    fprintf(f, "# (default %.2f, range -0.3 to 0.5)\n", d.gun_forward_m);
    fprintf(f, "gun_forward_m = %.2f\n\n", g_config.gun_forward_m);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  EXPERIMENTAL SCOPE\n");
    fprintf(f, "#  Universal physical placement and performance preferences.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Gun-mounted zoom screen. R3 toggles it without hiding the VR body.\n");
    fprintf(f, "# The main VR view stays wide while a fixed-magnification image appears. 1 = on.\n");
    fprintf(f, "# (default %d)\n", d.scope_enabled ? 1 : 0);
    fprintf(f, "scope_enabled = %d\n\n", g_config.scope_enabled ? 1 : 0);
    fprintf(f, "# Default experimental magnification used for every weapon.\n");
    fprintf(f, "# Initial zoom restored whenever R3 opens the scope; right-stick Y adjusts it.\n");
    fprintf(f, "# (default %.2f, range 6.0 to 24.0)\n", d.scope_zoom);
    fprintf(f, "scope_zoom = %.2f\n\n", g_config.scope_zoom);
    fprintf(f, "# Fixed physical screen width in meters; height is always 3/4 of width.\n");
    fprintf(f, "# (default %.3f, range 0.04 to 0.25)\n", d.scope_screen_width_m);
    fprintf(f, "scope_screen_width_m = %.3f\n\n", g_config.scope_screen_width_m);
    fprintf(f, "# Direct controller-local screen offsets in meters: right, up, forward.\n");
    fprintf(f, "# (defaults %.3f / %.3f / %.3f)\n",
            d.scope_screen_right_m, d.scope_screen_up_m, d.scope_screen_forward_m);
    fprintf(f, "scope_screen_right_m = %.3f\n", g_config.scope_screen_right_m);
    fprintf(f, "scope_screen_up_m = %.3f\n", g_config.scope_screen_up_m);
    fprintf(f, "scope_screen_forward_m = %.3f\n\n", g_config.scope_screen_forward_m);
    fprintf(f, "# Refresh the final zoom picture every Nth frame. 1 = full rate.\n");
    fprintf(f, "# (default %d, range 1 to 4)\n", d.scope_refresh_divisor);
    fprintf(f, "scope_refresh_divisor = %d\n\n", g_config.scope_refresh_divisor);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  HUD, PRESENTATION & PERFORMANCE\n");
    fprintf(f, "#  Shared intent; each title adapter maps it to that game's renderer.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Game brightness / gamma. 1.0 = the game's own; higher = brighter.\n");
    fprintf(f, "# (default %.2f, range 0.5 to 2)\n", d.game_brightness);
    fprintf(f, "game_brightness = %.2f\n\n", g_config.game_brightness);
    fprintf(f, "# How sharp the game renders inside the headset. ANY value in range\n");
    fprintf(f, "# works, so pick your own: 0.90 really means 90%%, not \"rounded to 80\".\n");
    fprintf(f, "# The named tiers are only shortcuts in the F1 menu:\n");
    fprintf(f, "#   0.50 potato   0.67 low     0.80 medium\n");
    fprintf(f, "#   1.00 high     1.10 ultra   1.50 keith david\n");
    fprintf(f, "# %d x %d is 1.00; your value scales both numbers together.\n",
            kNativeRenderWidth, kNativeRenderHeight);
    fprintf(f, "#\n");
    fprintf(f, "# YES, YOU CAN GO OVER 100%%. Above 1.00 is supersampling: the game\n");
    fprintf(f, "# renders bigger than the headset needs and the extra detail is\n");
    fprintf(f, "# squeezed down, which is the cleanest image available. The ceiling\n");
    fprintf(f, "# is %.2f (%d x %d), well past keith david, and it needs a top-end\n",
            kResolutionScaleMax,
            (int)(kNativeRenderWidth * kResolutionScaleMax),
            (int)(kNativeRenderHeight * kResolutionScaleMax));
    fprintf(f, "# card. A bigger number is pulled back to %.2f instead of accepted,\n",
            kResolutionScaleMax);
    fprintf(f, "# so a typo (20 instead of 2.0) cannot leave you unable to start.\n");
    fprintf(f, "#\n");
    fprintf(f, "# CLOSE MCC COMPLETELY and relaunch after changing this one.\n");
    fprintf(f, "# The headset projection stays full-size; the complete eye is upscaled.\n");
    fprintf(f, "# (default %.2f, range %.2f to %.2f)\n",
            d.resolution_scale, kResolutionScaleMin, kResolutionScaleMax);
    fprintf(f, "resolution_scale = %.2f\n\n", g_config.resolution_scale);
    fprintf(f, "# HUD size: fraction of the view the HUD lays out into. Smaller pulls\n");
    fprintf(f, "# shields/radar/ammo toward the center so both VR eyes see them.\n");
    fprintf(f, "# (default %.2f = calibrated stock layout, range 0.30 to 1.00)\n", d.hud_size);
    fprintf(f, "hud_size = %.2f\n\n", g_config.hud_size);
    fprintf(f, "# HUD width/aspect trim after automatic headset correction.\n");
    fprintf(f, "# 1 = automatic, lower = narrower, higher = wider.\n");
    fprintf(f, "# (default %.2f, range %.2f to %.2f)\n",
            d.hud_aspect, kHudAspectMin, kHudAspectMax);
    fprintf(f, "hud_aspect = %.2f\n\n", g_config.hud_aspect);
    fprintf(f, "# HUD curvature: 0 = flat (+0.30), 1 = fully curved (-0.30).\n");
    fprintf(f, "# 0.50 keeps the active game's authored curvature.\n");
    fprintf(f, "# (default %.2f, range %.2f to %.2f)\n",
            d.hud_curvature, kHudCurvatureMin, kHudCurvatureMax);
    fprintf(f, "hud_curvature = %.2f\n\n", g_config.hud_curvature);
    fprintf(f, "# HUD height in virtual-screen pixels. Positive = higher, negative = lower.\n");
    fprintf(f, "# (default %+.0f, range %+.0f to %+.0f)\n",
             d.hud_vertical_offset, kHudHeightMin, kHudHeightMax);
    fprintf(f, "hud_vertical_offset = %+.0f\n\n", g_config.hud_vertical_offset);
    fprintf(f, "# Game camera motion blur: 0 = off (VR default; also removes\n");
    fprintf(f, "# repeating stereo echo artifacts), 1 = the game's normal blur.\n");
    fprintf(f, "# (default %d)\n", d.motion_blur ? 1 : 0);
    fprintf(f, "motion_blur = %d\n\n", g_config.motion_blur ? 1 : 0);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  GAMEPLAY, HANDS & IK\n");
    fprintf(f, "#  Shared behavior with title-specific skeleton calibration underneath.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Automatically enter VR when a level loads (no F2/F11 needed).\n");
    fprintf(f, "# (default %d)\n", d.auto_vr ? 1 : 0);
    fprintf(f, "auto_vr = %d\n\n", g_config.auto_vr ? 1 : 0);
    fprintf(f, "# Two-handed aiming: put your left hand on the gun front and use the\n");
    fprintf(f, "# left grip to steady aim along the two-hand line. 1 = on.\n");
    fprintf(f, "# (default %d)\n", d.two_handed_aim ? 1 : 0);
    fprintf(f, "two_handed_aim = %d\n\n", g_config.two_handed_aim ? 1 : 0);
    fprintf(f, "# Engage style: 1 = toggle (click grip on/off), 0 = hold.\n");
    fprintf(f, "# (default %d)\n", d.two_hand_toggle ? 1 : 0);
    fprintf(f, "two_hand_toggle = %d\n\n", g_config.two_hand_toggle ? 1 : 0);
    fprintf(f, "# Left controller wrist-to-palm correction, shared by support-hand IK\n");
    fprintf(f, "# and the two-hand aim point, in meters.\n");
    fprintf(f, "# (default %.3f, range -0.15 to 0.30)\n", d.left_hand_forward_m);
    fprintf(f, "left_hand_forward_m = %.3f\n\n", g_config.left_hand_forward_m);
    fprintf(f, "# Sideways nudge of the two-hand grab zone (+ = player's right), so the\n");
    fprintf(f, "# grab line sits on the visible barrel, in meters.\n");
    fprintf(f, "# (default %.3f, range -0.10 to 0.10)\n", d.two_hand_zone_right_m);
    fprintf(f, "two_hand_zone_right_m = %.3f\n\n", g_config.two_hand_zone_right_m);
    fprintf(f, "# Rendered left hand wrist-to-palm distance, in meters. Seats the\n");
    fprintf(f, "# dual-wield gun in the palm and the grab line through it.\n");
    fprintf(f, "# (default %.3f, range -0.05 to 0.25)\n", d.left_grip_forward_m);
    fprintf(f, "left_grip_forward_m = %.3f\n\n", g_config.left_grip_forward_m);
    fprintf(f, "# VRIK arm IK: 1 = bend the arm to your controller (shoulder planted,\n");
    fprintf(f, "# elbow solved); 0 = rigid-parent the whole arm assembly.\n");
    fprintf(f, "# (default %d)\n", d.arm_ik ? 1 : 0);
    fprintf(f, "arm_ik = %d\n\n", g_config.arm_ik ? 1 : 0);
    fprintf(f, "# Floating hands: 1 = show only the hands and the guns they hold\n");
    fprintf(f, "# (arms hidden); 0 = full arms. Pure render filter over VRIK.\n");
    fprintf(f, "# (default %d)\n", d.floating_hands ? 1 : 0);
    fprintf(f, "floating_hands = %d\n\n", g_config.floating_hands ? 1 : 0);
    fprintf(f, "# Lower the right (weapon) shoulder so the arm doesn't clip your face\n");
    fprintf(f, "# (0 = authored, higher = lower; ~world units).\n");
    fprintf(f, "# (default %.3f, range 0 to 0.3)\n", d.right_shoulder_drop);
    fprintf(f, "right_shoulder_drop = %.3f\n\n", g_config.right_shoulder_drop);
    fprintf(f, "# Push BOTH shoulders back toward your torso, along your heading.\n");
    fprintf(f, "# Some titles (e.g. ODST) plant the shoulders in front of you;\n");
    fprintf(f, "# raise this until they sit at your body. Negative = forward.\n");
    fprintf(f, "# (default %.3f, range -0.3 to 0.3; ~world units)\n", d.shoulder_back_m);
    fprintf(f, "shoulder_back_m = %.3f\n\n", g_config.shoulder_back_m);
    fprintf(f, "# Keep the IK shoulders level with the horizon instead of pitching\n");
    fprintf(f, "# with your head. 1 = level torso (shoulders stay put); 0 = old.\n");
    fprintf(f, "# (default %d)\n", d.shoulder_level ? 1 : 0);
    fprintf(f, "shoulder_level = %d\n\n", g_config.shoulder_level ? 1 : 0);
    fprintf(f, "# VRIK stage A1: show the player's game-animated body (experimental).\n");
    fprintf(f, "# (default %d)\n", d.body_wip ? 1 : 0);
    fprintf(f, "body_wip = %d\n\n", g_config.body_wip ? 1 : 0);
    fprintf(f, "# -------------------------------------------------------------------\n");
    fprintf(f, "#  DEVELOPMENT DIAGNOSTICS\n");
    fprintf(f, "#  Leave these off unless a developer asks you to enable one.\n");
    fprintf(f, "# -------------------------------------------------------------------\n\n");
    fprintf(f, "# Diagnostic: 1 ignores the controller and pushes the weapon a fixed\n");
    fprintf(f, "# distance left, to test whether the gun mesh reads our matrices.\n");
    fprintf(f, "# (default %d)\n", d.weapon_probe ? 1 : 0);
    fprintf(f, "weapon_probe = %d\n\n", g_config.weapon_probe ? 1 : 0);
    fprintf(f, "# Diagnostic: 1 logs the CHUD state-byte window on change (finds the\n");
    fprintf(f, "# enemy-red reticle state and per-element HUD flags). Log-only.\n");
    fprintf(f, "# Not in the F1 menu; this file only. (default %d)\n", d.hud_probe ? 1 : 0);
    fprintf(f, "hud_probe = %d\n\n", g_config.hud_probe ? 1 : 0);
    fprintf(f, "# Diagnostic: log camera-vs-muzzle offset on each shot (bullet origin).\n");
    fprintf(f, "# Not in the F1 menu; this file only. (default %d)\n", d.bullet_probe ? 1 : 0);
    fprintf(f, "bullet_probe = %d\n\n", g_config.bullet_probe ? 1 : 0);
    fprintf(f, "# Ghosting diagnostic: 1 renders the right eye first.\n");
    fprintf(f, "# (default %d)\n", d.right_eye_first ? 1 : 0);
    fprintf(f, "right_eye_first = %d\n", g_config.right_eye_first ? 1 : 0);
    fclose(f);
}
