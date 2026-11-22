#pragma once
// The game's own settings (RDR.exe build 0x673783F3), as its parser lays them out: rage::LoadedOptions (options.xml:
// gameplay, display, controller, keyboard / mouse, audio) and rage::GraphicsOptions (graphicsOptions.xml). Offsets come
// from the parser's member descriptors (each one's name hash sits 8 bytes before its offset; re/NOTES.md "Game
// settings"). rdmp.dll reads and applies the raw structures through the game's own code (game/gamesettings.cpp); the
// client DLL turns them into the main menu's settings pages with this table.
#include <cstddef>
#include <cstdint>

namespace rdmp::opt {

constexpr size_t kLoadedSize = 0x70;
constexpr size_t kGraphicsSize = 0x5c;

enum class Type : uint8_t { Bool, U8, I32, F32 };
enum class Block : uint8_t { Loaded, Graphics };

struct Field {
    const char* key;    // name in the game's XML (prefixed with its group for LoadedOptions)
    Block block;
    uint16_t offset;
    Type type;
};

// clang-format off
inline constexpr Field kFields[] = {
    // gameplayOptions @ 0x00
    {"gameplay.bGoldenGuns", Block::Loaded, 0x00, Type::Bool},
    {"gameplay.bAutoSaveDisabled", Block::Loaded, 0x01, Type::Bool},
    // displayOptions @ 0x04
    {"display.fBrightness", Block::Loaded, 0x04, Type::F32},
    {"display.fContrast", Block::Loaded, 0x08, Type::F32},
    {"display.fSaturation", Block::Loaded, 0x0c, Type::F32},
    {"display.fSubtitleScale", Block::Loaded, 0x10, Type::F32},
    {"display.fObjectiveScale", Block::Loaded, 0x14, Type::F32},
    {"display.Subtitles", Block::Loaded, 0x18, Type::I32},
    {"display.fHelpScale", Block::Loaded, 0x1c, Type::F32},
    {"display.bRadarOn", Block::Loaded, 0x20, Type::Bool},
    {"display.bRouteOn", Block::Loaded, 0x21, Type::Bool},
    // controllerOptions @ 0x24
    {"controller.bRumble", Block::Loaded, 0x24, Type::Bool},
    {"controller.iHorseCameraRelativity", Block::Loaded, 0x28, Type::I32},
    {"controller.bSwapButtons", Block::Loaded, 0x2c, Type::Bool},
    {"controller.bSouthPaw", Block::Loaded, 0x2d, Type::Bool},
    {"controller.iCombatMode", Block::Loaded, 0x30, Type::I32},
    {"controller.bInvertedY", Block::Loaded, 0x34, Type::Bool},
    {"controller.bSwapCamX", Block::Loaded, 0x35, Type::Bool},
    {"controller.bAutoCenter", Block::Loaded, 0x36, Type::Bool},
    {"controller.fScalarSensitivity", Block::Loaded, 0x38, Type::F32},
    {"controller.fLeftStickDeadzone", Block::Loaded, 0x3c, Type::F32},
    {"controller.fRightStickDeadzone", Block::Loaded, 0x40, Type::F32},
    // kbMouseOptions @ 0x44
    {"kbMouse.fMouseScalarSensitivity", Block::Loaded, 0x44, Type::F32},
    {"kbMouse.fADSScalarSensitivity", Block::Loaded, 0x48, Type::F32},
    {"kbMouse.iHorseCameraRelativity", Block::Loaded, 0x4c, Type::I32},
    {"kbMouse.bInvertedY", Block::Loaded, 0x50, Type::Bool},
    {"kbMouse.bSwapCamX", Block::Loaded, 0x51, Type::Bool},
    {"kbMouse.bHoldSprint", Block::Loaded, 0x52, Type::Bool},
    {"kbMouse.bHoldSprintVehicle", Block::Loaded, 0x53, Type::Bool},
    // audioOptions @ 0x54
    {"audio.fSFXVolume", Block::Loaded, 0x54, Type::F32},
    {"audio.fMusicVolume", Block::Loaded, 0x58, Type::F32},
    {"audio.bHeadphones", Block::Loaded, 0x5c, Type::Bool},
    // rage::GraphicsOptions
    {"Fullscreen", Block::Graphics, 0x00, Type::Bool},
    {"Monitor", Block::Graphics, 0x01, Type::U8},
    {"ResolutionX", Block::Graphics, 0x04, Type::I32},
    {"ResolutionY", Block::Graphics, 0x08, Type::I32},
    {"RefreshRate", Block::Graphics, 0x0c, Type::F32},
    {"FrameRateLimit", Block::Graphics, 0x10, Type::I32},
    {"Vsync", Block::Graphics, 0x14, Type::U8},
    {"TripleBuffer", Block::Graphics, 0x15, Type::Bool},
    {"DynamicResolution", Block::Graphics, 0x16, Type::Bool},
    {"DynamicResolutionTargetFramerate", Block::Graphics, 0x18, Type::I32},
    {"DynamicResolutionMinScaleIndex", Block::Graphics, 0x1c, Type::I32},
    {"MinAnisotropicFiltering", Block::Graphics, 0x20, Type::U8},
    {"MotionBlurStyle", Block::Graphics, 0x21, Type::U8},
    {"MotionBlurStrength", Block::Graphics, 0x24, Type::F32},
    {"ShadowQuality", Block::Graphics, 0x28, Type::U8},
    {"ShadowSoftness", Block::Graphics, 0x29, Type::U8},
    {"ShadowBlend", Block::Graphics, 0x2a, Type::U8},
    {"WorldStreamingRadius", Block::Graphics, 0x2c, Type::F32},
    {"TerrainStreamingFactor", Block::Graphics, 0x30, Type::F32},
    {"TreeLevelOfDetail", Block::Graphics, 0x34, Type::F32},
    {"TreeImposterHighLodStreamingDistance", Block::Graphics, 0x38, Type::F32},
    {"GrassStreamingDistance", Block::Graphics, 0x3c, Type::F32},
    {"bFocusPaused", Block::Graphics, 0x40, Type::Bool},
    {"bConstrainMouse", Block::Graphics, 0x41, Type::Bool},
    {"HDR", Block::Graphics, 0x42, Type::Bool},
    {"PeakBrightness", Block::Graphics, 0x44, Type::F32},
    {"PaperWhite", Block::Graphics, 0x48, Type::F32},
    {"aaAntiAliasing", Block::Graphics, 0x4c, Type::I32},
    {"FSR3UpscalingQuality", Block::Graphics, 0x50, Type::I32},
    {"FSR3AdditionalSharpness", Block::Graphics, 0x54, Type::F32},
    {"NVIDIAReflex", Block::Graphics, 0x58, Type::U8},
    {"DLSSUpscalingQuality", Block::Graphics, 0x59, Type::U8},
    {"DLSSFrameGeneration", Block::Graphics, 0x5a, Type::U8},
};
// clang-format on

}  // namespace rdmp::opt
