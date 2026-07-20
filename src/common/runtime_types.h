#pragma once

#include <cstdint>

enum class GameTitle : uint8_t
{
    None = 0,
    Halo3,
    Halo3ODST,
    HaloReach,
    Halo4,
    HaloCE,
    Halo2,
    Unknown,
};

enum class RuntimeMode : uint8_t
{
    Shell = 0,
    Loading,
    Gameplay,
    Paused,
    Cutscene,
    Vehicle,
    Turret,
    Dead,
    Unsupported,
};

enum class VrHand : uint8_t
{
    Left = 0,
    Right,
};

enum TitleCapability : uint32_t
{
    TitleCapability_None = 0,
    TitleCapability_Stereo = 1u << 0,
    TitleCapability_ControllerAim = 1u << 1,
    TitleCapability_Hud = 1u << 2,
    TitleCapability_ArmIk = 1u << 3,
    TitleCapability_RuntimeModes = 1u << 4,
    TitleCapability_RoomScale = 1u << 5,
};
