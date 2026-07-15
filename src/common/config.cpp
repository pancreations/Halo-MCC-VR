#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <string>
#include <algorithm>
#include "config.h"
#include "log.h"

Config g_config;
static std::wstring g_path;

static void Clamp()
{
    g_config.screen_width_m = std::clamp(g_config.screen_width_m, 0.5f, 20.0f);
    g_config.screen_distance_m = std::clamp(g_config.screen_distance_m, 0.3f, 20.0f);
    g_config.turn_snap_deg = std::clamp(g_config.turn_snap_deg, 5.0f, 90.0f);
    g_config.turn_smooth_deg_s = std::clamp(g_config.turn_smooth_deg_s, 30.0f, 360.0f);
}

void ConfigLoad(const wchar_t* path)
{
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
        if (!strcmp(key, "screen_width_m"))
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
                 !strcmp(key, "stereo_sun_shafts"))
            continue; // retired ghost-fix switches; accept old config files quietly
        else if (!strcmp(key, "dpad_hand"))
            g_config.dpad_hand = atoi(val) != 0 ? 1 : 0;
        else if (!strcmp(key, "right_eye_first"))
            g_config.right_eye_first = atoi(val) != 0;
        else if (*key)
            LOG("config: unknown key '%s' ignored", key);
    }
    fclose(f);
    Clamp();
    LOG("config: loaded (screen %.2fm wide at %.2fm)", g_config.screen_width_m, g_config.screen_distance_m);
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
    fprintf(f, "# Halo 3 VR mod settings. Edit while the game is closed, or use the\n");
    fprintf(f, "# in-headset menu (F1) which saves this file automatically.\n\n");
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
    fprintf(f, "# Ghosting diagnostic: 1 renders the right eye first.\n");
    fprintf(f, "right_eye_first = %d\n", g_config.right_eye_first ? 1 : 0);
    fclose(f);
}
