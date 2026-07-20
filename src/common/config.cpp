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
    g_config.config_version = 1;
    g_config.haptic_intensity = std::clamp(g_config.haptic_intensity, 0.0f, 1.0f);
    g_config.headset_smoothing = std::clamp(g_config.headset_smoothing, 0.0f, 0.10f);
    g_config.aim_stabilization = std::clamp(g_config.aim_stabilization, 0.0f, 0.95f);
    g_config.screen_width_m = std::clamp(g_config.screen_width_m, 0.5f, 20.0f);
    g_config.screen_distance_m = std::clamp(g_config.screen_distance_m, 0.3f, 20.0f);
    g_config.turn_snap_deg = std::clamp(g_config.turn_snap_deg, 5.0f, 90.0f);
    g_config.turn_smooth_deg_s = std::clamp(g_config.turn_smooth_deg_s, 30.0f, 360.0f);
    g_config.crosshair_distance_m = std::clamp(g_config.crosshair_distance_m, 2.0f, 50.0f);
    g_config.crosshair_size_deg = std::clamp(g_config.crosshair_size_deg, 0.3f, 5.0f);
    g_config.reticle_r = std::clamp(g_config.reticle_r, 0.0f, 1.0f);
    g_config.reticle_g = std::clamp(g_config.reticle_g, 0.0f, 1.0f);
    g_config.reticle_b = std::clamp(g_config.reticle_b, 0.0f, 1.0f);
    g_config.gun_scale = std::clamp(g_config.gun_scale, 0.3f, 3.0f);
    g_config.game_brightness = std::clamp(g_config.game_brightness, 0.5f, 2.0f);
    if (g_config.resolution_scale < 0.585f)
        g_config.resolution_scale = 0.50f;
    else if (g_config.resolution_scale < 0.735f)
        g_config.resolution_scale = 0.67f;
    else if (g_config.resolution_scale < 0.90f)
        g_config.resolution_scale = 0.80f;
    else if (g_config.resolution_scale < 1.05f)
        g_config.resolution_scale = 1.00f;
    else if (g_config.resolution_scale < 1.30f)
        g_config.resolution_scale = 1.10f;
    else
        g_config.resolution_scale = 1.50f;
    g_config.hud_size = std::clamp(g_config.hud_size, 0.30f, 1.00f);
    g_config.left_hand_forward_m = std::clamp(g_config.left_hand_forward_m, -0.15f, 0.30f);
    g_config.two_hand_zone_right_m = std::clamp(g_config.two_hand_zone_right_m, -0.10f, 0.10f);
    g_config.left_grip_forward_m = std::clamp(g_config.left_grip_forward_m, -0.05f, 0.25f);
    g_config.right_shoulder_drop = std::clamp(g_config.right_shoulder_drop, 0.0f, 0.3f);
    g_config.gun_pitch_deg = std::clamp(g_config.gun_pitch_deg, -180.0f, 180.0f);
    g_config.gun_yaw_deg = std::clamp(g_config.gun_yaw_deg, -180.0f, 180.0f);
    g_config.gun_roll_deg = std::clamp(g_config.gun_roll_deg, -180.0f, 180.0f);
    g_config.gun_forward_m = std::clamp(g_config.gun_forward_m, -0.3f, 0.5f);
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
            else if (parsed > 1)
                LOG("config: version %ld is newer than supported version 1; known keys will be loaded", parsed);
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
    fprintf(f, "# HaloMCCVR settings. Edit while the game is closed, or use the\n");
    fprintf(f, "# in-headset menu (F1) which saves this file automatically.\n\n");
    fprintf(f, "config_version = %d\n\n", g_config.config_version);
    fprintf(f, "# OpenXR controller vibration strength, 0 = off and 1 = full.\n");
    fprintf(f, "haptic_intensity = %.2f\n\n", g_config.haptic_intensity);
    fprintf(f, "# Headset micro-smoothing, 0 = raw and 0.10 = maximum. Default 0.\n");
    fprintf(f, "headset_smoothing = %.2f\n\n", g_config.headset_smoothing);
    fprintf(f, "# Floating VR-crosshair smoothing only; bullets stay raw (0-0.95).\n");
    fprintf(f, "aim_stabilization = %.2f\n\n", g_config.aim_stabilization);
    fprintf(f, "# Width of the virtual screen in meters.\n");
    fprintf(f, "screen_width_m = %.2f\n\n", g_config.screen_width_m);
    fprintf(f, "# Distance from your head to the screen in meters.\n");
    fprintf(f, "screen_distance_m = %.2f\n\n", g_config.screen_distance_m);
    fprintf(f, "# VR turning with the right controller stick: 0 = snap, 1 = smooth.\n");
    fprintf(f, "turn_smooth = %d\n\n", g_config.turn_smooth ? 1 : 0);
    fprintf(f, "# Degrees per snap turn (5-90).\n");
    fprintf(f, "turn_snap_deg = %.0f\n\n", g_config.turn_snap_deg);
    fprintf(f, "# Smooth turn speed in degrees per second (30-360).\n");
    fprintf(f, "turn_smooth_deg_s = %.0f\n\n", g_config.turn_smooth_deg_s);
    fprintf(f, "# Hold this controller next to your head to use the left stick as D-pad:\n");
    fprintf(f, "# 0 = left controller, 1 = right controller.\n");
    fprintf(f, "dpad_hand = %d\n\n", g_config.dpad_hand);
    fprintf(f, "# Aim crosshair in stereo: a floating reticle where the weapon actually\n");
    fprintf(f, "# shoots (the game's own reticle follows your head, not your hand).\n");
    fprintf(f, "crosshair = %d\n\n", g_config.crosshair ? 1 : 0);
    fprintf(f, "# How far away the crosshair floats, in meters (2-50).\n");
    fprintf(f, "crosshair_distance_m = %.1f\n\n", g_config.crosshair_distance_m);
    fprintf(f, "# Apparent size of the crosshair, in degrees (0.3-5).\n");
    fprintf(f, "crosshair_size_deg = %.1f\n\n", g_config.crosshair_size_deg);
    fprintf(f, "# Crosshair color, 0-1 per channel (default = Halo 3 light blue).\n");
    fprintf(f, "reticle_r = %.3f\n", g_config.reticle_r);
    fprintf(f, "reticle_g = %.3f\n", g_config.reticle_g);
    fprintf(f, "reticle_b = %.3f\n\n", g_config.reticle_b);
    fprintf(f, "# Size of the hand-held weapon and arms (0.3-3). 1 = authored size;\n");
    fprintf(f, "# Home/End adjust this in-game.\n");
    fprintf(f, "gun_scale = %.2f\n\n", g_config.gun_scale);
    fprintf(f, "# Weapon mounting rotation on the controller, in degrees. Rotates only\n");
    fprintf(f, "# the visible gun; the cursor/bullet ray stays fixed on the controller.\n");
    fprintf(f, "gun_pitch_deg = %.0f\n", g_config.gun_pitch_deg);
    fprintf(f, "gun_yaw_deg = %.0f\n", g_config.gun_yaw_deg);
    fprintf(f, "gun_roll_deg = %.0f\n\n", g_config.gun_roll_deg);
    fprintf(f, "# Push the gun forward of the controller, in meters (-0.3 to 0.5).\n");
    fprintf(f, "# Negative seats the gun back into your fist.\n");
    fprintf(f, "gun_forward_m = %.2f\n\n", g_config.gun_forward_m);
    fprintf(f, "# Hide every native CHUD crosshair class.\n");
    fprintf(f, "# The floating motion-control reticle remains visible.\n");
    fprintf(f, "kill_reticle = %d\n\n", g_config.kill_reticle ? 1 : 0);
    fprintf(f, "# Game brightness / gamma. 1.0 = the game's own; higher = brighter.\n");
    fprintf(f, "game_brightness = %.2f\n\n", g_config.game_brightness);
    fprintf(f, "# Internal render preset: 0.50 potato, 0.67 low, 0.80 medium,\n");
    fprintf(f, "# 1.00 high, 1.10 ultra, or 1.50 keith david.\n");
    fprintf(f, "# Restart the game after changing this value.\n");
    fprintf(f, "# The headset projection stays full-size; the complete eye is upscaled.\n");
    fprintf(f, "resolution_scale = %.2f\n\n", g_config.resolution_scale);
    fprintf(f, "# HUD size: fraction of the view the HUD lays out into (0.30-1.00).\n");
    fprintf(f, "# 0.87 = Halo's stock value; smaller pulls shields/radar/ammo toward\n");
    fprintf(f, "# the center so both VR eyes see them. Tune live in the F1 menu.\n");
    fprintf(f, "hud_size = %.2f\n\n", g_config.hud_size);
    fprintf(f, "# Automatically enter VR when a level loads (no F2/F11 needed).\n");
    fprintf(f, "auto_vr = %d\n\n", g_config.auto_vr ? 1 : 0);
    fprintf(f, "# Two-handed aiming: put your left hand on the gun front and use the\n");
    fprintf(f, "# left grip to steady aim along the two-hand line. 1 = on.\n");
    fprintf(f, "two_handed_aim = %d\n", g_config.two_handed_aim ? 1 : 0);
    fprintf(f, "# Engage style: 1 = toggle (click grip on/off), 0 = hold.\n");
    fprintf(f, "two_hand_toggle = %d\n", g_config.two_hand_toggle ? 1 : 0);
    fprintf(f, "# Left controller wrist-to-palm correction, shared by support-hand IK\n");
    fprintf(f, "# and the two-hand aim point. Tune live in F1. Range -0.15 to 0.30 m.\n");
    fprintf(f, "left_hand_forward_m = %.3f\n", g_config.left_hand_forward_m);
    fprintf(f, "# Sideways nudge of the two-hand grab zone (+ = player's right),\n");
    fprintf(f, "# so the grab line sits on the visible barrel. Range -0.10 to 0.10 m.\n");
    fprintf(f, "two_hand_zone_right_m = %.3f\n", g_config.two_hand_zone_right_m);
    fprintf(f, "# Rendered left hand wrist-to-palm distance. Seats the dual-wield gun\n");
    fprintf(f, "# in the palm and the two-hand grab line through it. -0.05 to 0.25 m.\n");
    fprintf(f, "left_grip_forward_m = %.3f\n\n", g_config.left_grip_forward_m);
    fprintf(f, "# VRIK arm IK: 1 = bend the arm to your controller (shoulder planted,\n");
    fprintf(f, "# elbow solved); 0 = rigid-parent the whole arm assembly.\n");
    fprintf(f, "arm_ik = %d\n", g_config.arm_ik ? 1 : 0);
    fprintf(f, "# Floating hands: 1 = show only the hands and the guns they hold\n");
    fprintf(f, "# (arms hidden); 0 = full arms. Pure render filter over VRIK.\n");
    fprintf(f, "floating_hands = %d\n", g_config.floating_hands ? 1 : 0);
    fprintf(f, "# Lower the right (weapon) shoulder so the arm doesn't clip your face\n");
    fprintf(f, "# (0 = authored, higher = lower; ~world units). Range 0-0.3.\n");
    fprintf(f, "right_shoulder_drop = %.3f\n", g_config.right_shoulder_drop);
    fprintf(f, "# Keep the IK shoulders level with the horizon instead of pitching\n");
    fprintf(f, "# with your head. 1 = level torso (shoulders stay put); 0 = old.\n");
    fprintf(f, "shoulder_level = %d\n\n", g_config.shoulder_level ? 1 : 0);
    fprintf(f, "# VRIK stage A1: show the player's game-animated body (experimental).\n");
    fprintf(f, "body_wip = %d\n\n", g_config.body_wip ? 1 : 0);
    fprintf(f, "# Halo's camera motion blur: 0 = off (VR default; also removes the\n");
    fprintf(f, "# repeating echo artifacts in stereo), 1 = the game's normal blur.\n");
    fprintf(f, "motion_blur = %d\n\n", g_config.motion_blur ? 1 : 0);
    fprintf(f, "# Diagnostic: 1 ignores the controller and pushes the weapon a fixed\n");
    fprintf(f, "# distance left, to test whether the gun mesh reads our matrices.\n");
    fprintf(f, "weapon_probe = %d\n\n", g_config.weapon_probe ? 1 : 0);
    fprintf(f, "# Diagnostic: 1 logs the CHUD state-byte window on change (finds the\n");
    fprintf(f, "# enemy-red reticle state and per-element HUD flags). Log-only.\n");
    fprintf(f, "hud_probe = %d\n\n", g_config.hud_probe ? 1 : 0);
    fprintf(f, "# Diagnostic: log camera-vs-muzzle offset on each shot (bullet origin).\n");
    fprintf(f, "bullet_probe = %d\n\n", g_config.bullet_probe ? 1 : 0);
    fprintf(f, "# Ghosting diagnostic: 1 renders the right eye first.\n");
    fprintf(f, "right_eye_first = %d\n", g_config.right_eye_first ? 1 : 0);
    fclose(f);
}
