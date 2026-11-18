![Futurama Quest 3](https://hackertrips.wordpress.com/wp-content/uploads/2014/08/futurama-net5-e1407638267292.png?w=652)

# Futurama Quest (Meta Quest 3 Port)

**Good news, everyone!** The 2003 Xbox game **Futurama** runs natively on **Meta Quest 3** in a **stereoscopic 3D floating theater window** over passthrough.

This project is a Meta Quest 3 (OpenXR / Vulkan) and macOS (Metal) port of [hiburn8/Futurama-The-Game-The-Recomp](https://github.com/hiburn8/Futurama-The-Game-The-Recomp), the frame-accurate 1:1 C++ decompilation of the original Xbox game.

### You'll need your own copy

No game data is included in this repository or release APK. You need your own original Xbox copy of *Futurama* (NTSC-U or PAL) as a disc image (`.iso`) or extracted disc files (`default.xbe` + assets).

---

## Meta Quest 3 Features

- **Stereoscopic 3D Window:** Renders 3D gameplay with true left/right eye parallax (`clip.x += eye * strength * (clip.w - focus)`) via dual-eye command replay in `SdlGpuDevice` and side-by-side OpenXR `XrCompositionLayerQuad` layers.
- **Crisp Native Quad + Passthrough:** `1280×720` per eye (`2560×720` SBS), `2×` MSAA, `4×` anisotropic filtering, OpenXR `QUALITY_SUPER_SAMPLING_BIT_FB` + sharpening, Rec.709 color space, and full passthrough backdrop.
- **Flat 2D HUD & Menus:** HUD elements, text, FMVs, and screen-space overlays stay pinned flat at zero disparity on the window plane.
- **Touch Plus Controls & Haptics:** Native OpenXR input bindings and rumble.

---

## Installing & Playing on Quest 3

1. Download the latest APK from [Releases](../../releases) (`futurama-quest-v0.1.5.apk`) and sideload it via SideQuest, Meta Quest Developer Hub, or `adb`:
   ```sh
   adb install -r futurama-quest-v0.1.5.apk
   ```
2. Copy your Xbox Futurama disc image (`.iso`, NTSC-U or PAL) onto the headset, for example into `Download`.
3. Launch **Futurama** from **Unknown Sources** in your Quest library. On the first start a file picker opens: pick
   your `.iso` (in the picker's Recent list or under Download). The game remembers it, so this happens only once.

   The app has no storage permission, so it can only read the disc through that picker. Alternatively, put the `.iso`
   straight into the app's own folder with adb or SideQuest, and no picker is needed:
   ```sh
   adb push "Futurama (USA).iso" /sdcard/Android/data/com.futuramarecomp.app/files/
   ```

### Quest Touch Plus Controls

| Touch Plus Input | Xbox Controller / Action |
| --- | --- |
| Left Stick / Right Stick | Left Stick / Right Stick |
| A / B / X / Y | A / B / X / Y |
| Left / Right Trigger | Left / Right Trigger |
| Left / Right Grip (`LB` / `RB`) | White / Black |
| Left / Right Stick Click (`L3` / `R3`) | Left / Right Thumb |
| Left Menu Button | Start |
| **`L3 + R3` (click both sticks)** | **VR menu** (game pauses): background dark / mixed reality, 3D on/off, 4:3 / 16:9 (after a restart), recenter, reset screen size, exit |
| `LB + RB + Menu` | Back |
| **`LB + RB + L3`** | **Toggle Stereoscopic 3D / Flat 2D live** |
| **Hold `LB + RB` (both grips)** | **Window mode: move the screen with your hands, pull hands apart / together for distance** |
| `LB + RB` + Left Stick Up/Down | Screen distance |
| `LB + RB` + Right Stick Up/Down | Screen size |
| `LB + RB + R3` | Toggle dark background / mixed reality (passthrough) |
| `LB + RB + Y` | Reset screen size and distance |

Screen distance, size, height and the background choice are remembered between launches (`files/xr_view.txt`). While both grips are held the game gets no input except `Menu` (= Back).

---

## Building for Meta Quest (Android `arm64-v8a`)

Requires Android SDK (compileSdk 35, minSdk 29), NDK `28.2.13676358`, CMake `3.31.5`, JDK 17, and `glslangValidator` (only needed when regenerating SPIR-V shaders via `tools/compile_spirv_shaders.py`).

```sh
cd android
./gradlew assembleRelease
```

The signed APK is written to `android/app/build/outputs/apk/release/app-release.apk`.

<details>
<summary>Desktop Builds (macOS Metal, Windows, Linux) & CLI Options</summary>

### macOS (Apple Silicon / Metal)

Requires Xcode Command Line Tools and CMake 3.25+. Uses SDL3's GPU API with Metal (`FUT_USE_SDL_GPU=ON`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows

Requires Visual Studio 2022 ("Desktop development with C++") and CMake 3.25+:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Linux

Requires GCC 13 or Clang 17, CMake 3.25, SDL3 dependencies, and [DXVK-native](https://github.com/doitsujin/dxvk/releases) 3.1.1:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFUT_DXVK_ROOT=/path/to/dxvk/usr
cmake --build build -j
cmake --install build --prefix dist
```

### Runtime Options

| Option | Effect |
| --- | --- |
| `--windowed` / `--fullscreen` | Windowed (`1280×960`) or full screen |
| `--size WxH` | Output resolution |
| `--render-scale <auto\|0.25..16>` | Dynamic scene render scaling |
| `--filter <linear\|nearest>` | Scaling filter |
| `--aa <1\|2\|4\|8>` | MSAA sample count |
| `--anisotropic <off\|2\|4\|8\|16>` | Anisotropic filtering |
| `--aspect <auto\|4:3\|16:9\|16:10\|21:9\|w:h>` | Hor+ widescreen aspect ratio |
| `--2d` / `--3d` | Start OpenXR quad in flat 2D or stereo 3D |
| `--stereo-strength <float>` | Clip-space stereo parallax (default `0.022`) |
| `--stereo-focus <float>` | Convergence plane depth in game units (default `12.0`) |
| `--saves <dir>` | Custom save directory |

</details>

---

## Credits & Upstream

- **Upstream Decompilation:** [hiburn8/Futurama-The-Game-The-Recomp](https://github.com/hiburn8/Futurama-The-Game-The-Recomp) ([CC0 1.0](LICENSE.md))
- **Contributors:** [igno2k](https://github.com/igno2k)
- **Quest 3 / OpenXR 3D Window & macOS Metal Port:** [mrchrisster](https://github.com/mrchrisster)
