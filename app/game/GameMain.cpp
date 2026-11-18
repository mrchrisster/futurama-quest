// fut_game: the port booted like the original (main 0x000D2A40): the App (src/game/app: the Intro, the front end, the
// Ingame context with the pause menu, the game-over screen) on the user's own disc image / extracted folder, drawn by
// the decompiled renderer (render/nixbox NiXBoxRenderer on the D3D11 backend of its D3D8 stream), the controller on
// port 0 (or the keyboard: Window's KeyboardGamepad).
//
//   fut_game <iso-or-folder> [--size WxH] [--windowed | --fullscreen] [--render-scale auto|<factor>]
//            [--filter linear|nearest] [--aa 1|2|4|8] [--anisotropic off|2|4|8|16]
//            [--aspect auto|4:3|16:9|16:10|21:9|<w>:<h>] [--saves <dir>]
//   fut_game                       (no arguments, e.g. double-clicked: platform/sdl/Launcher.h finds the disc)
//
// The picture: the game renders the Xbox's 640 x 480 frame, which the device draws into the largest 4:3 rectangle of
// the window, centred, black either side (render/xd3d/D3D11Device.h: Swap). The render scale (default auto) draws
// that frame at the rectangle's own resolution (a factor: at factor x 640 x 480, scaled to the rectangle with a
// bilinear filter, or --filter nearest for unfiltered pixels; 1 is the Xbox's picture); the game itself is unchanged
// (docs/analysis/render_scale_plan.md). The window starts borderless full screen on its monitor; Alt+Enter toggles
// a window of the windowed size (--size WxH, default 1280x960; --size or --windowed start windowed;
// platform/sdl/Window.h: SDL3 for the window, the keyboard and the controllers).
//
// --aspect (default 4:3, the original's picture; docs/analysis/widescreen_plan.md): a wider canvas, round(480 x
// aspect) x 480 (auto: the window's own shape, so a 16:9 screen is filled without bars), with more world at the
// sides (Hor+), the HUD at the edges, movies and the 4:3 pictures centred with black beside them
// (engine/ni/ScreenCanvas.h). Opt-in: the levels were built for 4:3 and may show their edges.
//
// The release build is a Windows-subsystem program (no console window when double-clicked; started from a terminal
// it writes to that terminal); its errors also go to message boxes.
//
// Saves go to %APPDATA%\FuturamaRecomp\saves (never into the game folder; --saves <dir> another directory;
// game/app/SaveStorage.h: the save games' directories with name.txt, Futurama.dat / Unlock.dat / Options.dat and the
// save images: the in-game save, the options save, the load flow). The clock is the real QueryPerformanceCounter,
// read once per App frame.
//
// Sound: the App's sound system App+0xF0 is the console's (game/sound/GameSound.h XboxSound: the ported sound layer
// over the XAudio2 backend's hardware path VP -> GP -> EP, with default.xbe's tables and xbox/dsstdfx.bin; without an
// audio device the same path renders offline and is dropped, so the game logic still sees voices end), with the
// deterministic step-based voice status (SdlSoundBackend::Options::stepStatus; docs/analysis/level_sound_port.md
// 3.4). The level's sounds play (scripts, characters, emitters, explosions, dialogue speech); see KNOWN_ISSUES for the
// sites not restored yet.
//
// Drawn: the Intro's splash screens (legal, NI), the front end's loading screen, main menu scene and its options /
// load / extras panels (the extras menus with the music list's render target), the levels with their loading screens
// and titles, the pause menu with its options and load panels, the breakdown's scene, the yes / no dialog, the Death
// screen, the movies (game/app/MoviePlayer.h on platform/sdl/MovieService.h: the Intro's fox / vug / uds / intro,
// Attract, the level cutscenes, the extras' clips; YUY2 through the D3D11 backend's YuvEnable stand-in, the ADPCM audio
// through the backend's streams).
//
#include "core/XbeImage.h"
#include "core/fs/Vfs.h"
#include "engine/ni/ScreenCanvas.h"
#include "game/app/App.h"
#include "game/app/MemoryCards.h"
#include "game/app/SaveStorage.h"
#include "game/sound/GameSound.h"
#include "game/text/TextObject.h"
#include "platform/sdl/GamepadInput.h"
#include "platform/sdl/Launcher.h"
#include "platform/sdl/MovieService.h"
#include "platform/sdl/SdlSoundBackend.h"
#include "platform/sdl/Window.h"
#include "render/nixbox/NiXBoxRenderer.h"
#ifdef FUT_USE_SDL_GPU
#include "render/xd3d/SdlGpuDevice.h"
#else
#include "render/xd3d/D3D11Device.h"
#endif
#ifdef FUT_USE_OPENXR
#include "platform/xr/OpenXrTheater.h"
#include <SDL3/SDL_timer.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#endif


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace fut;

namespace {

std::filesystem::path SaveFolder() { return platform::PrefFolder() / "saves"; }   // (%APPDATA%\FuturamaRecomp\saves)

// Mirror internal saves out to app-external storage so users / adb pull can back up progress on non-debuggable APKs
// without moving the live SaveFolder() path.
void MirrorSavesToExternal(const std::filesystem::path& saveDir, const char* phase) {
#ifdef __ANDROID__
    std::error_code ec;
    if (!std::filesystem::exists(saveDir, ec) || !std::filesystem::is_directory(saveDir, ec)) return;
    const char* ext = SDL_GetAndroidExternalStoragePath();
    if (!ext || !*ext) return;
    const std::filesystem::path dst = std::filesystem::path(ext) / "saves_backup";
    // Recreate directories/files instead of std::filesystem::copy so internal 0700/0600 modes are not cloned onto
    // external storage (which blocks adb pull via ext_data_rw).
    std::filesystem::remove_all(dst, ec);
    ec.clear();
    std::filesystem::create_directories(dst, ec);
    if (ec) {
        SDL_Log("Save backup (%s): failed to create %s: %s", phase, dst.c_str(), ec.message().c_str());
        return;
    }
    int filesCopied = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(saveDir, ec)) {
        if (ec) break;
        const std::filesystem::path rel = std::filesystem::relative(entry.path(), saveDir, ec);
        if (ec || rel.empty()) {
            ec.clear();
            continue;
        }
        const std::filesystem::path outPath = dst / rel;
        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(outPath, ec);
        } else if (entry.is_regular_file(ec)) {
            std::filesystem::create_directories(outPath.parent_path(), ec);
            std::ifstream in(entry.path(), std::ios::binary);
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            if (in && out) {
                out << in.rdbuf();
                if (out.good()) ++filesCopied;
            }
        }
        ec.clear();
    }
    SDL_Log("Save backup (%s): mirrored %d file(s) from %s -> %s", phase, filesCopied, saveDir.c_str(), dst.c_str());
#else
    (void)saveDir;
    (void)phase;
#endif
}

// The launcher's test of a disc it found (main's own checks below, without loading the image): opens, has a
// default.xbe, and that is the NTSC-U or the PAL build.
bool CheckDisc(const std::string& source, std::string& error) {
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return false;
    std::vector<u8> data;
    if (!vfs.ReadFile("default.xbe", data)) {
        error = "default.xbe not found (the port needs a Futurama (Xbox, NTSC-U or PAL) disc image or its extracted "
                "folder)";
        return false;
    }
    xbe::XbeImage probe;
    return probe.Load(std::move(data), &error);
}

}  // namespace

int main(int argc, char** argv) {
    platform::Window::SetDpiAware();                  // (before the launcher's dialog and the window)
    const bool boxes = platform::SetUpConsole();      // (the release build: errors in message boxes too)
    auto fail = [&](const std::string& text, int rc) {
        std::fprintf(stderr, "%s\n", text.c_str());
        if (boxes) platform::ErrorBox(text);
        return rc;
    };
    const bool hasSourceArg = argc >= 2 && std::string_view(argv[1]).rfind("--", 0) != 0;
    std::string source = hasSourceArg ? argv[1] : platform::FindDisc(CheckDisc);   // the disc image / folder
    if (!hasSourceArg && source.empty()) {            // (no argument and the dialog cancelled)
        std::fprintf(stderr, "usage: fut_game <iso-or-folder> [--size WxH] [--windowed | --fullscreen]"
                             " [--render-scale auto|<factor>] [--filter linear|nearest] [--aa 1|2|4|8]"
                             " [--anisotropic off|2|4|8|16] [--aspect auto|4:3|16:9|16:10|21:9|<w>:<h>]"
                             " [--saves <dir>]");
        std::fputs("\n", stderr);
        return 2;
    }
#ifdef FUT_USE_SDL_GPU
    using GpuDevice = render::xd3d::SdlGpuDevice;
#else
    using GpuDevice = render::xd3d::D3D11Device;
#endif
#ifdef FUT_USE_OPENXR
    u32 width = 1280, height = 720;                   // per-eye OpenXR 3D theater quad texture size (2560x720 SBS)
    bool fullscreen = false;
    GpuDevice::Filter filter = GpuDevice::Filter::Linear;   // (--filter)
    double renderScale = 0;                           // (--render-scale: 0 = auto)
    double aspect = 16.0 / 9.0;                       // 16:9 widescreen quad
    u32 aaSamples = 2, anisotropy = 4;                // balanced 2x MSAA + 4x AF for smooth 60 fps on Quest 3
    bool stereo = true;                               // stereoscopic 3D window by default (--2d disables)
    f32 stereoStrength = 0.022f;                      // clip-space shift at infinity (~5.6 cm on 2.56 m quad)
    f32 stereoFocus = 12.0f;                          // convergence depth in game units (just in front of Fry)
#elif defined(__ANDROID__)
    u32 width = 1280, height = 960;                   // the windowed size (--size WxH)
    bool fullscreen = false;                          // Android/Quest OS manages the 2D panel surface
    GpuDevice::Filter filter = GpuDevice::Filter::Linear;   // (--filter)
    double renderScale = 0;                           // (--render-scale: 0 = auto)
    double aspect = 0;                                // fill the Horizon OS 2D panel automatically
    u32 aaSamples = 1, anisotropy = 1;                // (--aa / --anisotropic: the original sampling)
#else
    u32 width = 1280, height = 960;                   // the windowed size (--size WxH)
    bool fullscreen = true;                           // (--size / --windowed: a window; --fullscreen: full screen)
    GpuDevice::Filter filter = GpuDevice::Filter::Linear;   // (--filter)
    double renderScale = 0;                           // (--render-scale: 0 = auto)
    double aspect = 4.0 / 3.0;                        // (--aspect: the canvas's shape; 0 = auto, the window's)
    u32 aaSamples = 1, anisotropy = 1;                // (--aa / --anisotropic: the original sampling)
#endif
    std::filesystem::path saveDir = SaveFolder();     // (--saves <dir>)
#ifdef FUT_USE_OPENXR
    {   // the VR menu's saved choices that shape the renderer: 3D on/off and the 4:3 canvas (a 960 x 720 eye)
        bool savedStereo = stereo, saved43 = true;   // (nothing saved yet: 3D on, 4:3, the menu's defaults)
        if (platform::OpenXrTheater::LoadStartupPrefs(savedStereo, saved43)) stereo = savedStereo;
        if (saved43) aspect = 4.0 / 3.0, width = 960;
    }
#endif
    for (int i = hasSourceArg ? 2 : 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &width, &height), fullscreen = false;
        else if (a == "--windowed") fullscreen = false;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--render-scale" && i + 1 < argc) {
            const std::string s = argv[++i];
            if (s == "auto") renderScale = 0;
            else if (renderScale = std::atof(s.c_str()); !(renderScale >= 0.25 && renderScale <= 16))
                return fail("--render-scale takes auto or a factor from 0.25 to 16", 2);
        } else if (a == "--filter" && i + 1 < argc) {
            const std::string f = argv[++i];
            if (f == "nearest") filter = GpuDevice::Filter::Nearest;
            else if (f == "linear") filter = GpuDevice::Filter::Linear;
            else return fail("--filter takes linear or nearest", 2);
        } else if (a == "--aa" && i + 1 < argc) {
            const std::string s = argv[++i];
            if (s == "1" || s == "2" || s == "4" || s == "8") aaSamples = std::stoul(s);
            else return fail("--aa takes 1, 2, 4 or 8", 2);
        } else if (a == "--anisotropic" && i + 1 < argc) {
            const std::string s = argv[++i];
            if (s == "off") anisotropy = 1;
            else if (s == "2" || s == "4" || s == "8" || s == "16") anisotropy = std::stoul(s);
            else return fail("--anisotropic takes off, 2, 4, 8 or 16", 2);
        } else if (a == "--aspect" && i + 1 < argc) {
            const std::string s = argv[++i];
            unsigned w = 0, h = 0;
            if (s == "auto") aspect = 0;
            else if (std::sscanf(s.c_str(), "%u:%u", &w, &h) == 2 && w > 0 && h > 0) aspect = double(w) / h;
            else return fail("--aspect takes auto, 4:3, 16:9, 16:10, 21:9 or <w>:<h>", 2);
        } else if (a == "--saves" && i + 1 < argc) saveDir = argv[++i];
#ifdef FUT_USE_OPENXR
        else if (a == "--2d") stereo = false;
        else if (a == "--3d") stereo = true;
        else if (a == "--stereo-strength" && i + 1 < argc) stereoStrength = std::strtof(argv[++i], nullptr);
        else if (a == "--stereo-focus" && i + 1 < argc) stereoFocus = std::strtof(argv[++i], nullptr);
#endif
        else {
            return fail(std::string("unknown option (or its value missing): ") + argv[i], 2);
        }
    }
    std::string error;
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return fail(source + ": " + error, 1);
    {   // the original's data tables (core/XbeImage.h): the user's default.xbe, NTSC-U or PAL
        std::vector<u8> data;
        if (!vfs.ReadFile("default.xbe", data))
            return fail(source + ": default.xbe not found (the port needs a Futurama (Xbox, NTSC-U or PAL) disc image "
                                 "or its extracted folder)", 1);
        if (!xbe::LoadGameImage(std::move(data), &error)) return fail(source + ": " + error, 1);
    }
    const std::vector<u8>& xbeFile = xbe::GameImage().Bytes();
    platform::Window window;
#ifdef FUT_USE_OPENXR
    if (!window.Create("Futurama", 64, 64, false, false)) return fail("window creation failed", 1);
    platform::OpenXrTheater xr;
    if (!xr.Init(width, height, error, stereo)) return fail(error, 1);
    xr.SetStereoState(stereo);
    {   // the menu header: the FUTURAMA logo from the disc's loading screen (640 x 480 RGB565 in a 1024 x 512
        // texture, the last 1 MB of the .nif)
        std::vector<u8> nif;
        if (vfs.ReadFile("images/english/loadmenu.nif", nif) && nif.size() >= 1024 * 512 * 2) {
            const u8* px = nif.data() + nif.size() - 1024 * 512 * 2;
            constexpr u32 kX = 92, kY = 60, kW = 456, kH = 126;   // the logo's box (with the light rays behind it)
            std::vector<u8> rgba(size_t(kW) * kH * 4);
            for (u32 y = 0; y < kH; ++y)
                for (u32 x = 0; x < kW; ++x) {
                    const u32 o = ((kY + y) * 1024 + kX + x) * 2;
                    const u32 v = u32(px[o]) | (u32(px[o + 1]) << 8);
                    u8* d = &rgba[(size_t(y) * kW + x) * 4];
                    d[0] = u8(((v >> 11) & 31) * 255 / 31);
                    d[1] = u8(((v >> 5) & 63) * 255 / 63);
                    d[2] = u8((v & 31) * 255 / 31);
                    d[3] = 255;
                }
            xr.SetMenuLogo(rgba.data(), kW, kH);
        }
    }
#else
    if (!window.Create("Futurama", width, height, false, fullscreen)) return fail("window creation failed", 1);
#endif
#if !defined(_WIN32) && !defined(FUT_USE_SDL_GPU)
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);             // (DXVK-native: its window system is SDL3's; read at device creation)
#endif
    GpuDevice device;                                 // the decompiled renderer's D3D8 stream, executed
    {   // the canvas (engine/ni/ScreenCanvas.h): 640 x 480, or round(480 x aspect) rounded to even, 4:3 .. 32:9
#ifdef FUT_USE_OPENXR
        if (aspect <= 0) aspect = double(width) / std::max(height, 1u);
#else
        if (aspect <= 0) aspect = double(window.Width()) / std::max(window.Height(), 1u);
#endif
        aspect = std::clamp(aspect, 4.0 / 3.0, 32.0 / 9.0);
        const u32 w = u32(std::lround(480.0 * aspect / 2.0) * 2);
        ni::ScreenCanvas::width = w;
        device.SetCanvasSize(w, ni::ScreenCanvas::kHeight);   // (before Init: the frame buffers are made at the size)
    }
    device.SetRenderScale(renderScale);               // (before Init: the targets are made at the scale)
#ifdef FUT_USE_OPENXR
    device.SetStereo(stereo, stereoStrength, stereoFocus);
    if (!device.Init(nullptr, width, height, error)) return fail(error, 1);
    device.SetPresentCallback([&](const void* rgba, u32 w, u32 h) { xr.PresentFrame(rgba, w, h); });
    device.SetStereoPresentCallback([&](const void* leftRgba, const void* rightRgba, u32 w, u32 h) {
        xr.PresentStereoFrame(leftRgba, rightRgba, w, h);
    });
#elif defined(FUT_USE_SDL_GPU)
    if (!device.Init(window.Handle(), window.Width(), window.Height(), error)) return fail(error, 1);
#else
    if (!device.Init(window.NativeHandle(), window.Width(), window.Height(), error)) return fail(error, 1);
#endif
#ifdef FUT_USE_OPENXR
    while (aaSamples > 1 && !device.SetAntiAliasing(aaSamples, error)) aaSamples /= 2;
    if (aaSamples == 1 && !device.SetAntiAliasing(1, error)) return fail(error, 1);
#else
    if (!device.SetAntiAliasing(aaSamples, error)) return fail(error, 1);
#endif
    if (!device.SetAnisotropy(anisotropy)) return fail("invalid anisotropic filtering value", 1);
    device.SetFilter(filter);
    render::nixbox::NiXBoxRenderer renderer(device);  // the factory 0x0014C200 (App+0x104)
    platform::Gamepads gamepads;
    input::GamepadState pad0{};
    MirrorSavesToExternal(saveDir, "startup");
    game::app::HostSaveStorage saves(saveDir);        // (one directory per save game, as the U:\ model)
    saves.readDisc = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    // orig: App::Init's XboxSoundSystem 0x000CCD90 (DirectSoundCreate, the effects image): the backend first, its
    // hardware data before the sound system's constructor downloads the effects image.
    platform::SdlSoundBackend::Options audioOptions;
    audioOptions.stepStatus = true;
    platform::SdlSoundBackend soundBackend(audioOptions);
    bool soundOffline = false;
    if (!soundOffline && !soundBackend.Init()) {
        std::fprintf(stderr, "audio output unavailable: the sound layer runs offline (silent)\n");
        soundOffline = true;
    }
    if (soundOffline) soundBackend.InitOffline();
    std::vector<s32> soundDrop;
    auto finish = [&](int rc) {
        MirrorSavesToExternal(saveDir, "shutdown");
#ifdef FUT_USE_OPENXR
        device.SetStereoPresentCallback(nullptr);
        device.SetPresentCallback(nullptr);
        xr.Shutdown();
#endif
        return rc;
    };
    {
        std::vector<u8> data;
        if (vfs.ReadFile("xbox/dsstdfx.bin", data)) soundBackend.SetEffectsImage(data);
        soundBackend.LoadXbeTables(xbeFile);
    }
    // the movie player's platform: the renderer's device, the disc's Video\*.xmv, the backend's streams, the XMV
    // library's tables from default.xbe
    platform::MovieService movies(vfs, &soundBackend, xbeFile);
    sound::XboxSound sound(&soundBackend, [&](const std::string& path, std::vector<u8>& out) {
        return vfs.ReadFile(path, out);
    });
    game::app::AppServices sv;
    sv.readFile = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    sv.streamFile = sv.readFile;                      // (the sound-request loader's NiUDSStream job: synchronous, L)
    sv.sound = &sound;
    sv.movies = &movies;
    sv.readPad = [&](u32 port) -> const input::GamepadState* {
        if (port != 0) return gamepads.State(port);
#ifdef FUT_USE_OPENXR
        pad0 = xr.Gamepad();
        if (const input::GamepadState* ext = gamepads.State(0)) {
            pad0.buttons |= ext->buttons;
            for (int i = 0; i < 8; ++i) pad0.analog[i] = std::max(pad0.analog[i], ext->analog[i]);
            if (std::abs(s32(ext->thumbLX)) > std::abs(s32(pad0.thumbLX))) pad0.thumbLX = ext->thumbLX;
            if (std::abs(s32(ext->thumbLY)) > std::abs(s32(pad0.thumbLY))) pad0.thumbLY = ext->thumbLY;
            if (std::abs(s32(ext->thumbRX)) > std::abs(s32(pad0.thumbRX))) pad0.thumbRX = ext->thumbRX;
            if (std::abs(s32(ext->thumbRY)) > std::abs(s32(pad0.thumbRY))) pad0.thumbRY = ext->thumbRY;
        }
#else
        pad0 = gamepads.State(0) ? *gamepads.State(0) : platform::KeyboardGamepad(window);
#endif
        return &pad0;
    };
    sv.deviceChanges = [&](u32& ins, u32& rem) { return gamepads.TakeChanges(ins, rem); };   // (XGetDeviceChanges)
    sv.renderer = &renderer;
    sv.saves = &saves;
    sv.qpf = platform::Window::Frequency();         // (QueryPerformanceFrequency)
    sv.pal = vfs.IsPal();                             // (the disc's archives: sysPAL.img / FutPAL##.img)
    // (XGetLanguage: unset = English, the default; the PAL game's language choice comes later)
    text::CreateAppFont(sv.readFile);                 // App::Init's error overlay: font 0 (tex2)
    game::app::App app(sv);
    u64 base = platform::Window::Counter();
    app.qpc = 0;                                      // (the clock counts from the start: QPC - base)
#ifdef FUT_USE_OPENXR
    app.input.onRumble = [&](const input::Pad& p) {
        if (p.Port() == 0) xr.SetHaptics(p.motorLeft, p.motorRight);
        gamepads.SetMotors(p.Port(), p.motorLeft, p.motorRight);
    };
#else
    app.input.onRumble = [&](const input::Pad& p) { gamepads.SetMotors(p.Port(), p.motorLeft, p.motorRight); };
#endif
    const char* startLevel = nullptr;                 // (null = the boot)
    if (!app.Init(startLevel)) return fail("App::Init failed", 1);
#ifdef FUT_USE_OPENXR
    bool menuPaused = false;
    u64 pauseStart = 0;
    for (s32 f = 0; window.Pump();) {
        bool xrQuit = false;
        xr.PollEvents(xrQuit);
        if (xrQuit) break;
        if (!xr.IsSessionRunning()) {
            SDL_Delay(16);
            continue;
        }
        if (!xr.BeginFrame()) {
            SDL_Delay(8);
            continue;
        }
        if (xr.ConsumeStereoToggle()) {
            stereo = !stereo;
            device.SetStereo(stereo, stereoStrength, stereoFocus);
            xr.SetStereoState(stereo);
            std::fprintf(stderr, "OpenXR 3D Window: %s (strength=%.3f, focus=%.1f)\n", stereo ? "ON" : "OFF",
                         stereoStrength, stereoFocus);
        }
        if (xr.ConsumeExitRequest()) {
            xr.EndFrame();
            break;
        }
        if (const bool paused = xr.MenuOpen(); paused != menuPaused) {   // the VR menu pauses the game
            menuPaused = paused;
            if (!soundOffline) soundBackend.SetPaused(paused);
            if (paused) pauseStart = platform::Window::Counter();
            else base += platform::Window::Counter() - pauseStart;        // (the game's clock skips the pause)
        }
        if (menuPaused) {
            xr.EndFrame();                            // (the last game frame stays on the screen)
            continue;
        }
        gamepads.Poll();                              // (keep BT pads working alongside Quest Touch Plus)
        app.frame = f++;
        app.qpc = platform::Window::Counter() - base;
        const bool keepGoing = app.Frame();
        xr.EndFrame();
        if (!keepGoing) break;                        // (the App quits: no context requested)
        if (soundOffline) {                           // (offline: the published state rendered and dropped)
            soundDrop.clear();
            soundBackend.RenderFrames(25 * app.lastSteps, soundDrop);   // 800 samples per 1/60 s step
        }
    }
#else
    for (s32 f = 0; window.Pump(); ++f) {
        if (window.Resized()) device.Resize(window.Width(), window.Height());   // (the swap chain follows the window)
        gamepads.Poll();                              // (Input_Update reads the pads through readPad every step)
        app.frame = f;
        app.qpc = platform::Window::Counter() - base;
        if (!app.Frame()) break;                      // (the App quits: no context requested)
        if (f == 0) window.Show();                    // (the first frame is presented: no empty window before it)
        if (soundOffline) {                           // (offline: the published state rendered and dropped)
            soundDrop.clear();
            soundBackend.RenderFrames(25 * app.lastSteps, soundDrop);   // 800 samples per 1/60 s step
        }
    }
#endif
    return finish(0);
}
