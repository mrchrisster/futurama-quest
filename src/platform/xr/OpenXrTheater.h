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

    bool Init(u32 width, u32 height, std::string& error);
    void Shutdown();

    bool PollEvents(bool& shouldQuit);
    bool IsSessionRunning() const;
    bool IsSessionFocused() const;

    // Must be called once per running frame before rendering/presenting.
    bool BeginFrame();
    // Uploads the rendered game frame (RGBA8) into the floating OpenXR quad swapchain.
    void PresentFrame(const void* rgba, u32 width, u32 height);
    // Ends the OpenXR frame, submitting passthrough + transparent projection + theater quad layers.
    void EndFrame();

    const input::GamepadState& Gamepad() const;
    void SetHaptics(u16 left, u16 right);
    void Recenter();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fut::platform
