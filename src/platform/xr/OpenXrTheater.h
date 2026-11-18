// OpenXR 2D Theater Quad + Meta Quest Touch Plus input backend (Android/Quest 3).
#pragma once

#include "core/Types.h"
#include "game/input/Pad.h"

#include <memory>
#include <string>

namespace fut::platform {

class OpenXrTheater {
public:
    OpenXrTheater();
    ~OpenXrTheater();
    OpenXrTheater(const OpenXrTheater&) = delete;
    OpenXrTheater& operator=(const OpenXrTheater&) = delete;

    bool Init(u32 width, u32 height, std::string& error, bool stereo = false);
    void Shutdown();

    bool PollEvents(bool& shouldQuit);
    bool IsSessionRunning() const;
    bool IsSessionFocused() const;

    // Must be called once per running frame before rendering/presenting.
    bool BeginFrame();
    // Uploads a mono frame (RGBA8) into the floating OpenXR quad swapchain.
    void PresentFrame(const void* rgba, u32 width, u32 height);
    // Uploads side-by-side stereo eye frames (RGBA8 each) into the double-width OpenXR quad swapchain.
    void PresentStereoFrame(const void* leftRgba, const void* rightRgba, u32 eyeWidth, u32 eyeHeight);
    // Ends the OpenXR frame, submitting passthrough + transparent projection + theater quad layer(s).
    void EndFrame();

    const input::GamepadState& Gamepad() const;
    // 3D depth level 0 = off (flat), 1 = normal, 2 = deep, 3 = extra; changed from the VR menu or LB + RB + L3.
    bool ConsumeDepthChange(int& level);
    static float DepthScale(int level);   // the stereo strength factor of a level (0 for off)
    void SetHaptics(u16 left, u16 right);
    void Recenter();

    // VR options menu (L3 + R3 opens / closes it; Menu stays Start). While it is open the game should be
    // paused: GameMain skips App::Frame and pauses the sound output.
    bool MenuOpen() const;
    bool ConsumeExitRequest();
    // The game's own logo for the menu header (RGBA8, rows top-down), taken from the player's disc at startup.
    void SetMenuLogo(const u8* rgba, u32 width, u32 height);
    // GameMain reports the live 3D state so the menu shows it and the choice is remembered.
    void SetDepthLevel(int level);
    // Settings that must be known before the renderer is made (xr_view.txt): 3D on/off and the 4:3 canvas.
    // Returns false when nothing has been saved yet.
    static bool LoadStartupPrefs(int& depthLevel, bool& aspect43);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fut::platform
