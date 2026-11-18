// OpenXR 2D Theater Quad + Meta Quest Touch Plus input implementation; see OpenXrTheater.h.
#include "platform/xr/OpenXrTheater.h"

#ifdef __ANDROID__

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN

#include <jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fut::platform {

namespace {

bool HasExt(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

std::string XrErr(const char* fn, XrResult r) {
    return std::string(fn) + " failed (XrResult=" + std::to_string(static_cast<int>(r)) + ")";
}

u32 FindMemoryType(VkPhysicalDevice phys, u32 typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

// Small vector/quaternion helpers for the screen grab (local space, metres).
XrVector3f V3(float x, float y, float z) { return {x, y, z}; }
XrVector3f Add(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f Sub(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f Mul(const XrVector3f& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f Cross(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Len(const XrVector3f& a) { return std::sqrt(Dot(a, a)); }
XrVector3f Norm(const XrVector3f& a, const XrVector3f& fallback) {
    const float l = Len(a);
    return l > 1e-5f ? Mul(a, 1.0f / l) : fallback;
}
XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t = Mul(Cross(u, v), 2.0f);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
}
XrQuaternionf QuatFromTo(const XrVector3f& a, const XrVector3f& b) {   // unit vectors
    const float d = std::clamp(Dot(a, b), -1.0f, 1.0f);
    if (d > 0.99999f) return {0.0f, 0.0f, 0.0f, 1.0f};
    XrVector3f axis = Cross(a, b);
    if (Len(axis) < 1e-5f) axis = V3(0.0f, 1.0f, 0.0f);
    axis = Norm(axis, V3(0.0f, 1.0f, 0.0f));
    const float ang = std::acos(d);
    const float s = std::sin(ang * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(ang * 0.5f)};
}
// Orientation of a quad whose front (+Z) faces back along dir (head -> screen): yaw then pitch, no roll.
XrQuaternionf FacingQuat(const XrVector3f& dir) {
    const float yaw = std::atan2(-dir.x, -dir.z);
    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    const XrQuaternionf qy{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const XrQuaternionf qp{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    return {qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y,
            qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
            qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w,
            qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
}

constexpr float kDefaultDist = 2.2f;
constexpr float kDefaultHeight = 1.44f;
constexpr float kMinDist = 0.6f, kMaxDist = 6.0f;
constexpr float kMinHeight = 0.4f, kMaxHeight = 5.0f;
constexpr float kStickDeadzone = 0.3f;

// ---- VR menu panel drawing (RGBA8, rows top-down) ----
constexpr u32 kMenuW = 960, kMenuH = 720;
constexpr u32 kMenuHoldMs = 500;   // hold Menu this long to open the VR menu (a shorter tap = Start)

struct Glyph5x7 {
    char ch;
    u8 col[5];   // LSB = top row
};
constexpr Glyph5x7 kFont5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}},
    {'|', {0x00, 0x00, 0x7F, 0x00, 0x00}},
    {'=', {0x14, 0x14, 0x14, 0x14, 0x14}},
    {'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
    {')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};
const u8* FindGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const auto& g : kFont5x7)
        if (g.ch == c) return g.col;
    return kFont5x7[0].col;
}
void MenuFill(std::vector<u8>& img, int x0, int y0, int w, int h, u32 rgba) {
    for (int y = std::max(0, y0); y < std::min<int>(kMenuH, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min<int>(kMenuW, x0 + w); ++x) {
            u8* d = &img[(size_t(y) * kMenuW + x) * 4];
            d[0] = u8(rgba >> 24), d[1] = u8(rgba >> 16), d[2] = u8(rgba >> 8), d[3] = u8(rgba);
        }
}
void MenuText(std::vector<u8>& img, int x0, int y0, int scale, u32 rgba, const char* text) {
    int cx = x0;
    for (const char* p = text; *p; ++p, cx += 6 * scale) {
        const u8* cols = FindGlyph(*p);
        for (int gx = 0; gx < 5; ++gx)
            for (int gy = 0; gy < 7; ++gy)
                if (cols[gx] & (1u << gy)) MenuFill(img, cx + gx * scale, y0 + gy * scale, scale, scale, rgba);
    }
}
int MenuTextWidth(const char* text, int scale) { return int(std::strlen(text)) * 6 * scale - scale; }

enum MenuItem { kMenuResume, kMenuBackground, kMenu3D, kMenuAspect, kMenuRecenter, kMenuResetSize, kMenuExit, kMenuCount };

}  // namespace

struct OpenXrTheater::Impl {
    jobject activityGlobal = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool passthroughExt = false;
    bool colorSpaceExt = false;
    bool layerSettingsExt = false;
    bool perfSettingsExt = false;
    bool refreshRateExt = false;
    bool touchPlusExt = false;
    bool touchProExt = false;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhys = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    u32 queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkFence cmdFence = VK_NULL_HANDLE;

    int64_t swapFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // Minimal stereo clear projection layer (64x64) required by OpenXR layer validation.
    static constexpr u32 kProjDim = 64;
    VkRenderPass projRenderPass = VK_NULL_HANDLE;
    struct EyeProj {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
    } eyes[2];

    // Floating theater quad swapchain + staging upload buffer (side-by-side when stereo).
    XrSwapchain quadSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> quadImages;
    u32 quadEyeW = 0, quadEyeH = 0;
    bool quadStereo = false;
    bool initStereo = false;
    bool hasQuadFrame = false;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;

    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrPosef quadPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.2f}};
    XrExtent2Df quadSize = {2.56f, 1.44f};

    // Screen placement the player controls (both grips held = window mode), remembered in xr_view.txt.
    float viewDist = kDefaultDist;      // metres from the head
    float viewHeight = kDefaultHeight;  // quad height in metres (width follows the aspect)
    float viewElev = 0.0f;              // radians above the horizon
    bool darkMode = false;              // true = black void, false = passthrough (mixed reality)
    bool grabActive = false;
    bool viewDirty = false;
    XrVector3f grabStartDir{0.0f, 0.0f, -1.0f};
    XrVector3f grabStartOffset{0.0f, 0.0f, -kDefaultDist};
    float grabStartHand = 0.4f;
    float grabStartDist = kDefaultDist;
    bool prevBgChord = false;
    bool prevResetChord = false;
    XrAction aimPose = XR_NULL_HANDLE;
    XrSpace aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::string prefsPath;

    void UpdateQuadSize() {
        const float aspect = quadEyeH ? float(quadEyeW) / float(quadEyeH) : 16.0f / 9.0f;
        quadSize.height = viewHeight;
        quadSize.width = viewHeight * aspect;
    }
    void LoadView();
    void SaveView();
    void ApplyBackground();
    void UpdateGrab(bool active, float distStick, float sizeStick, XrTime t);
    PFN_xrPassthroughLayerPauseFB pfnPassthroughLayerPause = nullptr;

    // VR options menu
    bool menuOpen = false;
    int menuSel = 0;
    bool menuDirty = true;
    bool exitArmed = false;
    bool exitRequested = false;
    bool stereoState = true;
    bool aspect43 = false;          // saved choice
    bool startupAspect43 = false;   // what this run was started with
    bool menuHeld = false;
    bool menuLongFired = false;
    u64 menuDownMs = 0;
    int startPulse = 0;             // frames left of a Start press from a short Menu tap
    bool prevNav[6] = {};           // up, down, left, right, A, B
    XrSwapchain menuSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> menuImages;
    VkBuffer menuBuf = VK_NULL_HANDLE;
    VkDeviceMemory menuMem = VK_NULL_HANDLE;
    void* menuMapped = nullptr;
    std::vector<u8> logo;
    u32 logoW = 0, logoH = 0;

    bool CreateMenuSwapchain();
    void DrawMenu();
    void MenuInput(bool up, bool down, bool left, bool right, bool a, bool b);
    void MenuActivate(int dir);
    void SetMenuOpen(bool open) {
        menuOpen = open;
        menuDirty = true;
        exitArmed = false;
        if (open) menuSel = 0;
        for (bool& n : prevNav) n = true;   // ignore buttons already held when the menu opens
    }

    // Passthrough
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB pfnCreatePassthrough = nullptr;
    PFN_xrDestroyPassthroughFB pfnDestroyPassthrough = nullptr;
    PFN_xrPassthroughStartFB pfnPassthroughStart = nullptr;
    PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayer = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayer = nullptr;
    PFN_xrPassthroughLayerResumeFB pfnPassthroughLayerResume = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevel = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateDisplayRefreshRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB pfnRequestDisplayRefreshRate = nullptr;

    void ApplyPerfAndRefresh() {
        if (session == XR_NULL_HANDLE) return;
        if (perfSettingsExt && pfnPerfSettingsSetPerformanceLevel) {
            const XrResult cpuR = pfnPerfSettingsSetPerformanceLevel(
                session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            const XrResult gpuR = pfnPerfSettingsSetPerformanceLevel(
                session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            SDL_Log("OpenXR perf levels sustained-high (cpu=%d, gpu=%d)", static_cast<int>(cpuR),
                    static_cast<int>(gpuR));
        }
        if (refreshRateExt && pfnEnumerateDisplayRefreshRates && pfnRequestDisplayRefreshRate) {
            uint32_t n = 0;
            if (XR_SUCCEEDED(pfnEnumerateDisplayRefreshRates(session, 0, &n, nullptr)) && n > 0) {
                std::vector<float> rates(n);
                if (XR_SUCCEEDED(pfnEnumerateDisplayRefreshRates(session, n, &n, rates.data()))) {
                    rates.resize(n);
                    float target = 0.0f;
                    for (float r : rates) {
                        if (r > 59.4f && r < 60.6f) {
                            target = r;
                            break;
                        }
                        if (r >= 59.0f && (target == 0.0f || r < target)) {
                            target = r;
                        }
                    }
                    if (target > 0.0f) {
                        const XrResult rr = pfnRequestDisplayRefreshRate(session, target);
                        SDL_Log("OpenXR refresh rate request %.1f Hz (res=%d)", static_cast<double>(target),
                                static_cast<int>(rr));
                    }
                }
            }
        }
    }

    // OpenXR frame timing
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    bool frameActive = false;
    bool recenterPending = false;
    bool prevRecenterChord = false;
    bool stereoTogglePending = false;
    bool prevStereoChord = false;

    // Input actions (/interaction_profiles/oculus/touch_controller)
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrPath handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrAction leftStick = XR_NULL_HANDLE, rightStick = XR_NULL_HANDLE;
    XrAction btnA = XR_NULL_HANDLE, btnB = XR_NULL_HANDLE, btnX = XR_NULL_HANDLE, btnY = XR_NULL_HANDLE;
    XrAction leftTrigger = XR_NULL_HANDLE, rightTrigger = XR_NULL_HANDLE;
    XrAction leftGrip = XR_NULL_HANDLE, rightGrip = XR_NULL_HANDLE;
    XrAction leftStickClick = XR_NULL_HANDLE, rightStickClick = XR_NULL_HANDLE;
    XrAction menuClick = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;

    input::GamepadState gamepad{};
    u16 motorLeft = 0, motorRight = 0;
    u16 lastHapticVal[2] = {0, 0};
    u64 lastHapticApplyMs[2] = {0, 0};
    bool profileLogged = false;

    bool CreateVulkan(std::string& error);
    bool CreateSwapchains(u32 w, u32 h, std::string& error);
    bool RecreateQuadSwapchain(u32 eyeW, u32 eyeH, bool stereo);
    bool UploadRegions(XrSwapchain sc, const std::vector<XrSwapchainImageVulkanKHR>& images, VkBuffer buf,
                       const VkBufferImageCopy* regions, u32 regionCount);
    void UploadQuadRegions(const VkBufferImageCopy* regions, u32 regionCount) {
        if (UploadRegions(quadSwapchain, quadImages, stagingBuf, regions, regionCount)) hasQuadFrame = true;
    }
    bool CreateActions(std::string& error);
    void InitPassthrough();
    void SyncActions();
    void RefreshHaptics(bool forceImmediate);
    void LogActiveProfiles();
    void DoRecenter(XrTime displayTime);
    void DestroySwapchains();
    void Destroy();
};

bool OpenXrTheater::Impl::CreateVulkan(std::string& error) {
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnReqs = nullptr;
    PFN_xrCreateVulkanInstanceKHR pfnCreateInst = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR pfnGetPhys = nullptr;
    PFN_xrCreateVulkanDeviceKHR pfnCreateDev = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnReqs));
    xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateInst));
    xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetPhys));
    xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateDev));
    if (!pfnReqs || !pfnCreateInst || !pfnGetPhys || !pfnCreateDev) {
        error = "Missing XR_KHR_vulkan_enable2 entry points";
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XrResult xrRes = pfnReqs(instance, systemId, &reqs);
    if (XR_FAILED(xrRes)) {
        error = XrErr("xrGetVulkanGraphicsRequirements2KHR", xrRes);
        return false;
    }

    const u32 maxMaj = XR_VERSION_MAJOR(reqs.maxApiVersionSupported);
    const u32 maxMin = XR_VERSION_MINOR(reqs.maxApiVersionSupported);
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Futurama Recompiled";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "Futurama";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = (maxMaj > 1 || maxMin >= 1) ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;

    XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvici.systemId = systemId;
    xvici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xvici.vulkanCreateInfo = &ici;

    VkResult vkRes = VK_SUCCESS;
    xrRes = pfnCreateInst(instance, &xvici, &vkInstance, &vkRes);
    if (XR_FAILED(xrRes) || vkRes != VK_SUCCESS) {
        error = XrErr("xrCreateVulkanInstanceKHR", xrRes) + " vk=" + std::to_string(vkRes);
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdgi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdgi.systemId = systemId;
    gdgi.vulkanInstance = vkInstance;
    xrRes = pfnGetPhys(instance, &gdgi, &vkPhys);
    if (XR_FAILED(xrRes)) {
        error = XrErr("xrGetVulkanGraphicsDevice2KHR", xrRes);
        return false;
    }

    u32 qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhys, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qProps(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhys, &qCount, qProps.data());
    queueFamily = UINT32_MAX;
    for (u32 i = 0; i < qCount; ++i) {
        if (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) {
        error = "No Vulkan graphics queue family found";
        return false;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = systemId;
    xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = vkPhys;
    xdci.vulkanCreateInfo = &dci;

    xrRes = pfnCreateDev(instance, &xdci, &vkDevice, &vkRes);
    if (XR_FAILED(xrRes) || vkRes != VK_SUCCESS) {
        error = XrErr("xrCreateVulkanDeviceKHR", xrRes) + " vk=" + std::to_string(vkRes);
        return false;
    }
    vkGetDeviceQueue(vkDevice, queueFamily, 0, &vkQueue);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(vkDevice, &cpci, nullptr, &cmdPool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return false;
    }
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vkDevice, &cbai, &cmdBuf) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(vkDevice, &fci, nullptr, &cmdFence) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        return false;
    }
    return true;
}

bool OpenXrTheater::Impl::RecreateQuadSwapchain(u32 eyeW, u32 eyeH, bool stereo) {
    eyeW = std::max(eyeW, 1u);
    eyeH = std::max(eyeH, 1u);
    if (quadSwapchain && eyeW == quadEyeW && eyeH == quadEyeH && stereo == quadStereo) return true;

    const u32 texW = stereo ? eyeW * 2 : eyeW;
    const u32 texH = eyeH;

    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (stagingMapped) {
        vkUnmapMemory(vkDevice, stagingMem);
        stagingMapped = nullptr;
    }
    if (stagingBuf) {
        vkDestroyBuffer(vkDevice, stagingBuf, nullptr);
        stagingBuf = VK_NULL_HANDLE;
    }
    if (stagingMem) {
        vkFreeMemory(vkDevice, stagingMem, nullptr);
        stagingMem = VK_NULL_HANDLE;
    }
    if (quadSwapchain) {
        xrDestroySwapchain(quadSwapchain);
        quadSwapchain = XR_NULL_HANDLE;
    }
    quadImages.clear();
    hasQuadFrame = false;

    const VkDeviceSize bufSize = VkDeviceSize(texW) * texH * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkDevice, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(vkDevice, stagingBuf, &memReqs);
    const u32 memIdx = FindMemoryType(vkPhys, memReqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = memReqs.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(vkDevice, &mai, nullptr, &stagingMem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(vkDevice, stagingBuf, stagingMem, 0) != VK_SUCCESS) return false;
    if (vkMapMemory(vkDevice, stagingMem, 0, bufSize, 0, &stagingMapped) != VK_SUCCESS) return false;

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = swapFormat;
    sci.sampleCount = 1;
    sci.width = texW;
    sci.height = texH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &sci, &quadSwapchain))) return false;

    u32 imgCount = 0;
    xrEnumerateSwapchainImages(quadSwapchain, 0, &imgCount, nullptr);
    quadImages.assign(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(quadSwapchain, imgCount, &imgCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(quadImages.data()));
    quadEyeW = eyeW;
    quadEyeH = eyeH;
    quadStereo = stereo;
    UpdateQuadSize();
    return true;
}

bool OpenXrTheater::Impl::CreateSwapchains(u32 w, u32 h, std::string& error) {
    u32 fmtCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data());
    swapFormat = 0;
    for (int64_t f : fmts) {
        if (f == VK_FORMAT_R8G8B8A8_SRGB) {
            swapFormat = f;
            break;
        }
    }
    if (!swapFormat) {
        for (int64_t f : fmts) {
            if (f == VK_FORMAT_R8G8B8A8_UNORM) {
                swapFormat = f;
                break;
            }
        }
    }
    if (!swapFormat && !fmts.empty()) swapFormat = fmts[0];
    if (!swapFormat) {
        error = "No OpenXR swapchain formats reported";
        return false;
    }

    // Minimal clear render pass for the stereo projection layer.
    VkAttachmentDescription att{};
    att.format = static_cast<VkFormat>(swapFormat);
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    if (vkCreateRenderPass(vkDevice, &rpci, nullptr, &projRenderPass) != VK_SUCCESS) {
        error = "vkCreateRenderPass failed";
        return false;
    }

    for (u32 eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sci.format = swapFormat;
        sci.sampleCount = 1;
        sci.width = kProjDim;
        sci.height = kProjDim;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        XrResult xrRes = xrCreateSwapchain(session, &sci, &eyes[eye].swapchain);
        if (XR_FAILED(xrRes)) {
            error = XrErr("xrCreateSwapchain(proj)", xrRes);
            return false;
        }
        u32 imgCount = 0;
        xrEnumerateSwapchainImages(eyes[eye].swapchain, 0, &imgCount, nullptr);
        eyes[eye].images.assign(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(eyes[eye].swapchain, imgCount, &imgCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eyes[eye].images.data()));
        eyes[eye].views.resize(imgCount, VK_NULL_HANDLE);
        eyes[eye].framebuffers.resize(imgCount, VK_NULL_HANDLE);
        for (u32 i = 0; i < imgCount; ++i) {
            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image = eyes[eye].images[i].image;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = static_cast<VkFormat>(swapFormat);
            ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(vkDevice, &ivci, nullptr, &eyes[eye].views[i]) != VK_SUCCESS) {
                error = "vkCreateImageView failed";
                return false;
            }
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = projRenderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &eyes[eye].views[i];
            fbci.width = kProjDim;
            fbci.height = kProjDim;
            fbci.layers = 1;
            if (vkCreateFramebuffer(vkDevice, &fbci, nullptr, &eyes[eye].framebuffers[i]) != VK_SUCCESS) {
                error = "vkCreateFramebuffer failed";
                return false;
            }
        }
    }

    if (!RecreateQuadSwapchain(w, h, initStereo)) {
        error = "Failed to create OpenXR quad swapchain";
        return false;
    }
    if (!CreateMenuSwapchain()) SDL_Log("OpenXR: menu swapchain unavailable (the VR menu is off)");
    return true;
}

bool OpenXrTheater::Impl::CreateActions(std::string& error) {
    xrStringToPath(instance, "/user/hand/left", &handPaths[0]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[1]);

    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asci.actionSetName, sizeof asci.actionSetName, "futurama_controls");
    std::snprintf(asci.localizedActionSetName, sizeof asci.localizedActionSetName, "Futurama Controls");
    asci.priority = 0;
    XrResult r = xrCreateActionSet(instance, &asci, &actionSet);
    if (XR_FAILED(r)) {
        error = XrErr("xrCreateActionSet", r);
        return false;
    }

    auto makeAct = [&](const char* name, XrActionType type, XrAction& out, const XrPath* subs = nullptr, u32 nSub = 0) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(aci.actionName, sizeof aci.actionName, "%s", name);
        std::snprintf(aci.localizedActionName, sizeof aci.localizedActionName, "%s", name);
        aci.actionType = type;
        aci.countSubactionPaths = nSub;
        aci.subactionPaths = subs;
        return XR_SUCCEEDED(xrCreateAction(actionSet, &aci, &out));
    };

    if (!makeAct("left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, leftStick) ||
        !makeAct("right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, rightStick) ||
        !makeAct("btn_a", XR_ACTION_TYPE_BOOLEAN_INPUT, btnA) ||
        !makeAct("btn_b", XR_ACTION_TYPE_BOOLEAN_INPUT, btnB) ||
        !makeAct("btn_x", XR_ACTION_TYPE_BOOLEAN_INPUT, btnX) ||
        !makeAct("btn_y", XR_ACTION_TYPE_BOOLEAN_INPUT, btnY) ||
        !makeAct("left_trigger", XR_ACTION_TYPE_FLOAT_INPUT, leftTrigger) ||
        !makeAct("right_trigger", XR_ACTION_TYPE_FLOAT_INPUT, rightTrigger) ||
        !makeAct("left_grip", XR_ACTION_TYPE_FLOAT_INPUT, leftGrip) ||
        !makeAct("right_grip", XR_ACTION_TYPE_FLOAT_INPUT, rightGrip) ||
        !makeAct("left_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, leftStickClick) ||
        !makeAct("right_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, rightStickClick) ||
        !makeAct("menu_click", XR_ACTION_TYPE_BOOLEAN_INPUT, menuClick) ||
        !makeAct("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, haptic, handPaths, 2) ||
        !makeAct("aim_pose", XR_ACTION_TYPE_POSE_INPUT, aimPose, handPaths, 2)) {
        error = "Failed creating OpenXR actions";
        return false;
    }

    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance, s, &p);
        return p;
    };

    std::vector<XrActionSuggestedBinding> bindings = {
        {leftStick, path("/user/hand/left/input/thumbstick")},
        {rightStick, path("/user/hand/right/input/thumbstick")},
        {btnA, path("/user/hand/right/input/a/click")},
        {btnB, path("/user/hand/right/input/b/click")},
        {btnX, path("/user/hand/left/input/x/click")},
        {btnY, path("/user/hand/left/input/y/click")},
        {leftTrigger, path("/user/hand/left/input/trigger/value")},
        {rightTrigger, path("/user/hand/right/input/trigger/value")},
        {leftGrip, path("/user/hand/left/input/squeeze/value")},
        {rightGrip, path("/user/hand/right/input/squeeze/value")},
        {leftStickClick, path("/user/hand/left/input/thumbstick/click")},
        {rightStickClick, path("/user/hand/right/input/thumbstick/click")},
        {menuClick, path("/user/hand/left/input/menu/click")},
        {haptic, path("/user/hand/left/output/haptic")},
        {haptic, path("/user/hand/right/output/haptic")},
        {aimPose, path("/user/hand/left/input/aim/pose")},
        {aimPose, path("/user/hand/right/input/aim/pose")},
    };
    auto suggestProfile = [&](const char* profileName, bool required) {
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path(profileName);
        sb.suggestedBindings = bindings.data();
        sb.countSuggestedBindings = static_cast<u32>(bindings.size());
        const XrResult res = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_FAILED(res)) {
            if (required) {
                error = XrErr("xrSuggestInteractionProfileBindings", res);
                return false;
            }
            SDL_Log("OpenXR optional profile %s rejected (%d)", profileName, static_cast<int>(res));
        } else {
            SDL_Log("OpenXR bound profile %s", profileName);
        }
        return true;
    };
    if (!suggestProfile("/interaction_profiles/oculus/touch_controller", true)) return false;
    if (touchPlusExt) (void)suggestProfile("/interaction_profiles/meta/touch_controller_plus", false);
    if (touchProExt) (void)suggestProfile("/interaction_profiles/facebook/touch_controller_pro", false);

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &actionSet;
    r = xrAttachSessionActionSets(session, &ai);
    if (XR_FAILED(r)) {
        error = XrErr("xrAttachSessionActionSets", r);
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asci2{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci2.action = aimPose;
        asci2.subactionPath = handPaths[i];
        asci2.poseInActionSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        if (XR_FAILED(xrCreateActionSpace(session, &asci2, &aimSpace[i]))) aimSpace[i] = XR_NULL_HANDLE;
    }
    return true;
}

void OpenXrTheater::Impl::InitPassthrough() {
    if (!passthroughExt) return;
    xrGetInstanceProcAddr(instance, "xrCreatePassthroughFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreatePassthrough));
    xrGetInstanceProcAddr(instance, "xrDestroyPassthroughFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyPassthrough));
    xrGetInstanceProcAddr(instance, "xrPassthroughStartFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnPassthroughStart));
    xrGetInstanceProcAddr(instance, "xrCreatePassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreatePassthroughLayer));
    xrGetInstanceProcAddr(instance, "xrDestroyPassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyPassthroughLayer));
    xrGetInstanceProcAddr(instance, "xrPassthroughLayerResumeFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnPassthroughLayerResume));
    xrGetInstanceProcAddr(instance, "xrPassthroughLayerPauseFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnPassthroughLayerPause));
    if (!pfnCreatePassthrough || !pfnPassthroughStart || !pfnCreatePassthroughLayer || !pfnPassthroughLayerResume) return;

    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    if (XR_FAILED(pfnCreatePassthrough(session, &pci, &passthrough))) return;
    if (XR_FAILED(pfnPassthroughStart(passthrough))) return;

    XrPassthroughLayerCreateInfoFB plci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    plci.passthrough = passthrough;
    plci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    plci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (XR_FAILED(pfnCreatePassthroughLayer(session, &plci, &passthroughLayer))) {
        passthroughLayer = XR_NULL_HANDLE;
    }
    ApplyBackground();
}

// Dark mode pauses the passthrough layer (saves power) and leaves it out of the frame.
void OpenXrTheater::Impl::ApplyBackground() {
    if (!passthroughLayer) return;
    if (darkMode) {
        if (pfnPassthroughLayerPause) pfnPassthroughLayerPause(passthroughLayer);
    } else {
        if (pfnPassthroughLayerResume) pfnPassthroughLayerResume(passthroughLayer);
    }
    SDL_Log("OpenXR background: %s", darkMode ? "dark" : "mixed reality");
}

// xr_view.txt: "v2 <distance m> <height m> <elevation rad> <dark 0/1> <3d 0/1> <aspect 4:3 0/1>" (v1: first five)
namespace {
struct SavedView {
    float dist = kDefaultDist, height = kDefaultHeight, elev = 0.0f;
    int dark = 0, stereo = 1, aspect43 = 0;
};
std::string PrefsPath() {
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) return std::string(dir) + "/xr_view.txt";
    if (const char* idir = SDL_GetAndroidInternalStoragePath()) return std::string(idir) + "/xr_view.txt";
    return {};
}
bool ReadSavedView(const std::string& path, SavedView& v) {
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char tag[8] = {};
    const int n = std::fscanf(f, "%7s %f %f %f %d %d %d", tag, &v.dist, &v.height, &v.elev, &v.dark, &v.stereo,
                              &v.aspect43);
    std::fclose(f);
    if (std::strcmp(tag, "v2") == 0) return n == 7;
    if (std::strcmp(tag, "v1") == 0 && n >= 5) {
        v.stereo = 1, v.aspect43 = 0;
        return true;
    }
    return false;
}
}  // namespace

void OpenXrTheater::Impl::LoadView() {
    prefsPath = PrefsPath();
    SavedView v;
    if (!ReadSavedView(prefsPath, v)) return;
    viewDist = std::clamp(v.dist, kMinDist, kMaxDist);
    viewHeight = std::clamp(v.height, kMinHeight, kMaxHeight);
    viewElev = std::clamp(v.elev, -1.2f, 1.2f);
    darkMode = v.dark != 0;
    aspect43 = startupAspect43 = v.aspect43 != 0;
    SDL_Log("OpenXR view restored: dist %.2f m, height %.2f m, elev %.2f, dark %d, 3d %d, 4:3 %d", viewDist,
            viewHeight, viewElev, (int)darkMode, v.stereo, v.aspect43);
}

void OpenXrTheater::Impl::SaveView() {
    viewDirty = false;
    if (prefsPath.empty()) return;
    FILE* f = std::fopen(prefsPath.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "v2 %.3f %.3f %.3f %d %d %d\n", viewDist, viewHeight, viewElev, darkMode ? 1 : 0,
                 stereoState ? 1 : 0, aspect43 ? 1 : 0);
    std::fclose(f);
}

// Both grips held: the screen follows the hands (orbit around the head), pulling the hands apart pushes it
// away, left stick Y = distance, right stick Y = size. Released: the placement is saved.
void OpenXrTheater::Impl::UpdateGrab(bool active, float distStick, float sizeStick, XrTime t) {
    if (!active) {
        if (grabActive) {
            grabActive = false;
            viewDirty = true;
        }
        if (viewDirty) SaveView();
        return;
    }
    if (t <= 0 || !viewSpace || !aimSpace[0] || !aimSpace[1]) return;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION}, hl{XR_TYPE_SPACE_LOCATION}, hr{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, t, &head)) || (head.locationFlags & kValid) != kValid) return;
    if (XR_FAILED(xrLocateSpace(aimSpace[0], localSpace, t, &hl)) || (hl.locationFlags & kValid) != kValid) return;
    if (XR_FAILED(xrLocateSpace(aimSpace[1], localSpace, t, &hr)) || (hr.locationFlags & kValid) != kValid) return;

    const XrVector3f headPos = head.pose.position;
    const XrVector3f mid = Mul(Add(hl.pose.position, hr.pose.position), 0.5f);
    const XrVector3f aim = Norm(Add(Rotate(hl.pose.orientation, V3(0, 0, -1)), Rotate(hr.pose.orientation, V3(0, 0, -1))),
                                V3(0, 0, -1));
    const XrVector3f grabPt = Add(mid, Mul(aim, 0.38f));
    const XrVector3f dir = Norm(Sub(grabPt, headPos), V3(0, 0, -1));
    const float handDist = Len(Sub(hl.pose.position, hr.pose.position));

    if (!grabActive) {
        grabActive = true;
        grabStartDir = dir;
        grabStartHand = std::max(0.12f, handDist);
        grabStartOffset = Sub(quadPose.position, headPos);
        if (Len(grabStartOffset) < 0.2f) grabStartOffset = Mul(dir, viewDist);
        grabStartDist = std::clamp(Len(grabStartOffset), kMinDist, kMaxDist);
    }
    if (std::abs(distStick) > kStickDeadzone) {
        grabStartDist = std::clamp(grabStartDist + distStick * 0.025f, kMinDist, kMaxDist);
    }
    if (std::abs(sizeStick) > kStickDeadzone) {
        viewHeight = std::clamp(viewHeight * (1.0f + sizeStick * 0.015f), kMinHeight, kMaxHeight);
        UpdateQuadSize();
    }
    const float ratio = std::clamp(handDist / grabStartHand, 0.5f, 2.0f);
    const float newDist = std::clamp(grabStartDist * ratio, kMinDist, kMaxDist);
    const XrVector3f offDir = Norm(Rotate(QuatFromTo(grabStartDir, dir), Norm(grabStartOffset, dir)), dir);
    quadPose.position = Add(headPos, Mul(offDir, newDist));
    quadPose.orientation = FacingQuat(offDir);
    viewDist = newDist;
    viewElev = std::asin(std::clamp(offDir.y, -1.0f, 1.0f));
}

void OpenXrTheater::Impl::DoRecenter(XrTime displayTime) {
    if (displayTime <= 0 || !viewSpace || !localSpace) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, displayTime, &loc))) return;
    if ((loc.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) !=
        (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        return;
    }
    const auto& q = loc.pose.orientation;
    float fwdX = -2.0f * (q.x * q.z + q.w * q.y);
    float fwdZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float len = std::hypot(fwdX, fwdZ);
    if (len < 1e-4f) {
        fwdX = 0.0f;
        fwdZ = -1.0f;
    } else {
        fwdX /= len;
        fwdZ /= len;
    }
    const float ce = std::cos(viewElev), se = std::sin(viewElev);
    const XrVector3f dir{fwdX * ce, se, fwdZ * ce};
    quadPose.position = Add(loc.pose.position, Mul(dir, viewDist));
    quadPose.orientation = FacingQuat(dir);
    recenterPending = false;
}

void OpenXrTheater::Impl::SyncActions() {
    gamepad = {};
    if (state != XR_SESSION_STATE_FOCUSED) return;
    if (!profileLogged) LogActiveProfiles();

    XrActiveActionSet aas{actionSet, XR_NULL_PATH};
    XrActionsSyncInfo asi{XR_TYPE_ACTIONS_SYNC_INFO};
    asi.countActiveActionSets = 1;
    asi.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(session, &asi))) return;

    auto readBool = [&](XrAction a) {
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        xrGetActionStateBoolean(session, &gi, &s);
        return s.isActive && s.currentState == XR_TRUE;
    };
    auto readFloat = [&](XrAction a) {
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        xrGetActionStateFloat(session, &gi, &s);
        return s.isActive ? std::clamp(s.currentState, 0.0f, 1.0f) : 0.0f;
    };
    auto readVec2 = [&](XrAction a) {
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        xrGetActionStateVector2f(session, &gi, &s);
        return s.isActive ? s.currentState : XrVector2f{0.0f, 0.0f};
    };

    const XrVector2f ls = readVec2(leftStick);
    const XrVector2f rs = readVec2(rightStick);
    const float lt = readFloat(leftTrigger);
    const float rt = readFloat(rightTrigger);
    const float lg = readFloat(leftGrip);
    const float rg = readFloat(rightGrip);
    const bool lb = lg > 0.5f;
    const bool rb = rg > 0.5f;
    const bool l3 = readBool(leftStickClick);
    const bool r3 = readBool(rightStickClick);
    const bool menu = readBool(menuClick);

    const bool chord = l3 && r3;
    if (chord && !prevRecenterChord) recenterPending = true;
    prevRecenterChord = chord;

    // LB + RB + L3 (without R3) live-toggles stereoscopic 3D window mode.
    const bool stereoChord = lb && rb && l3 && !r3;
    if (stereoChord && !prevStereoChord) stereoTogglePending = true;
    prevStereoChord = stereoChord;

    // Window mode (both grips held): move/size the screen; LB + RB + R3 toggles dark / mixed reality,
    // LB + RB + Y resets the screen size and distance. The game gets no sticks or buttons meanwhile
    // (except Menu = Back).
    const bool windowMode = lb && rb;
    const bool yBtn = readBool(btnY);
    const bool bgChord = windowMode && r3 && !l3;
    if (bgChord && !prevBgChord) {
        darkMode = !darkMode;
        ApplyBackground();
        viewDirty = true;
    }
    prevBgChord = bgChord;
    const bool resetChord = windowMode && yBtn;
    if (resetChord && !prevResetChord) {
        viewDist = kDefaultDist;
        viewHeight = kDefaultHeight;
        viewElev = 0.0f;
        UpdateQuadSize();
        grabActive = false;            // restart the grab from the reset placement
        recenterPending = true;
        viewDirty = true;
    }
    prevResetChord = resetChord;
    if (!resetChord) UpdateGrab(windowMode, ls.y, rs.y, frameState.predictedDisplayTime);
    if (windowMode) {
        if (menu) gamepad.buttons |= 0x0020;            // Back
        return;
    }

    // Menu button: a tap is Start (sent on release, for a few frames), holding it opens / closes the VR menu.
    const u64 nowMs = SDL_GetTicks();
    bool menuTap = false;
    if (menu) {
        if (!menuHeld) {
            menuHeld = true;
            menuDownMs = nowMs;
            menuLongFired = false;
        } else if (!menuLongFired && nowMs - menuDownMs >= kMenuHoldMs) {
            menuLongFired = true;
            SetMenuOpen(!menuOpen);
        }
    } else if (menuHeld) {
        menuHeld = false;
        menuTap = !menuLongFired;
    }
    if (menuOpen) {                                     // the game gets nothing while the menu is up
        if (menuTap) {
            SetMenuOpen(false);
            return;
        }
        constexpr float kNav = 0.6f;
        MenuInput(ls.y > kNav || rs.y > kNav, ls.y < -kNav || rs.y < -kNav, ls.x < -kNav || rs.x < -kNav,
                  ls.x > kNav || rs.x > kNav, readBool(btnA), readBool(btnB));
        return;
    }
    if (menuTap) startPulse = 6;
    if (startPulse > 0) {
        gamepad.buttons |= 0x0010;                      // Start
        --startPulse;
    }
    if (l3) gamepad.buttons |= 0x0040;
    if (r3) gamepad.buttons |= 0x0080;

    gamepad.analog[0] = readBool(btnA) ? 255 : 0;       // A
    gamepad.analog[1] = readBool(btnB) ? 255 : 0;       // B
    gamepad.analog[2] = readBool(btnX) ? 255 : 0;       // X
    gamepad.analog[3] = readBool(btnY) ? 255 : 0;       // Y
    gamepad.analog[4] = rb ? 255 : 0;                   // Black (RB)
    gamepad.analog[5] = lb ? 255 : 0;                   // White (LB)
    gamepad.analog[6] = static_cast<u8>(std::lround(lt * 255.0f));
    gamepad.analog[7] = static_cast<u8>(std::lround(rt * 255.0f));

    auto axis16 = [](float v) {
        return static_cast<s16>(std::clamp(static_cast<long>(std::lround(v * 32767.0f)), -32768L, 32767L));
    };
    gamepad.thumbLX = axis16(ls.x);
    gamepad.thumbLY = axis16(ls.y);
    gamepad.thumbRX = axis16(rs.x);
    gamepad.thumbRY = axis16(rs.y);
}

void OpenXrTheater::Impl::DestroySwapchains() {
    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (stagingMapped) {
        vkUnmapMemory(vkDevice, stagingMem);
        stagingMapped = nullptr;
    }
    if (stagingBuf) {
        vkDestroyBuffer(vkDevice, stagingBuf, nullptr);
        stagingBuf = VK_NULL_HANDLE;
    }
    if (stagingMem) {
        vkFreeMemory(vkDevice, stagingMem, nullptr);
        stagingMem = VK_NULL_HANDLE;
    }
    if (quadSwapchain) {
        xrDestroySwapchain(quadSwapchain);
        quadSwapchain = XR_NULL_HANDLE;
    }
    quadImages.clear();
    if (menuMapped) {
        vkUnmapMemory(vkDevice, menuMem);
        menuMapped = nullptr;
    }
    if (menuBuf) vkDestroyBuffer(vkDevice, menuBuf, nullptr);
    if (menuMem) vkFreeMemory(vkDevice, menuMem, nullptr);
    menuBuf = VK_NULL_HANDLE;
    menuMem = VK_NULL_HANDLE;
    if (menuSwapchain) xrDestroySwapchain(menuSwapchain);
    menuSwapchain = XR_NULL_HANDLE;
    menuImages.clear();
    for (u32 e = 0; e < 2; ++e) {
        for (VkFramebuffer fb : eyes[e].framebuffers)
            if (fb) vkDestroyFramebuffer(vkDevice, fb, nullptr);
        for (VkImageView iv : eyes[e].views)
            if (iv) vkDestroyImageView(vkDevice, iv, nullptr);
        if (eyes[e].swapchain) xrDestroySwapchain(eyes[e].swapchain);
        eyes[e] = {};
    }
    if (projRenderPass) {
        vkDestroyRenderPass(vkDevice, projRenderPass, nullptr);
        projRenderPass = VK_NULL_HANDLE;
    }
}

void OpenXrTheater::Impl::Destroy() {
    if (vkDevice) vkDeviceWaitIdle(vkDevice);
    if (passthroughLayer && pfnDestroyPassthroughLayer) {
        pfnDestroyPassthroughLayer(passthroughLayer);
        passthroughLayer = XR_NULL_HANDLE;
    }
    if (passthrough && pfnDestroyPassthrough) {
        pfnDestroyPassthrough(passthrough);
        passthrough = XR_NULL_HANDLE;
    }
    DestroySwapchains();
    for (XrSpace& s : aimSpace) {
        if (s) xrDestroySpace(s);
        s = XR_NULL_HANDLE;
    }
    if (localSpace) xrDestroySpace(localSpace);
    if (viewSpace) xrDestroySpace(viewSpace);
    if (actionSet) xrDestroyActionSet(actionSet);
    if (session) xrDestroySession(session);
    if (cmdFence) vkDestroyFence(vkDevice, cmdFence, nullptr);
    if (cmdPool) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    if (vkDevice) vkDestroyDevice(vkDevice, nullptr);
    if (vkInstance) vkDestroyInstance(vkInstance, nullptr);
    if (instance) xrDestroyInstance(instance);
    if (activityGlobal) {
        if (JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
            env->DeleteGlobalRef(activityGlobal);
        }
        activityGlobal = nullptr;
    }
    localSpace = XR_NULL_HANDLE;
    viewSpace = XR_NULL_HANDLE;
    actionSet = XR_NULL_HANDLE;
    session = XR_NULL_HANDLE;
    cmdFence = VK_NULL_HANDLE;
    cmdPool = VK_NULL_HANDLE;
    vkDevice = VK_NULL_HANDLE;
    vkInstance = VK_NULL_HANDLE;
    instance = XR_NULL_HANDLE;
}

OpenXrTheater::OpenXrTheater() : impl_(std::make_unique<Impl>()) {}
OpenXrTheater::~OpenXrTheater() { Shutdown(); }

bool OpenXrTheater::Init(u32 width, u32 height, std::string& error, bool stereo) {
    Impl& d = *impl_;
    d.initStereo = stereo;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    JavaVM* vm = nullptr;
    if (!env || env->GetJavaVM(&vm) != JNI_OK || !vm) {
        error = "Failed to acquire JavaVM from SDL";
        return false;
    }
    jobject actLocal = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!actLocal) {
        error = "SDL_GetAndroidActivity returned null";
        return false;
    }
    d.activityGlobal = env->NewGlobalRef(actLocal);
    env->DeleteLocalRef(actLocal);

    PFN_xrInitializeLoaderKHR pfnInitLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnInitLoader));
    if (!pfnInitLoader) {
        error = "Missing xrInitializeLoaderKHR";
        return false;
    }
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = vm;
    loaderInfo.applicationContext = d.activityGlobal;
    XrResult r = pfnInitLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
    if (XR_FAILED(r)) {
        error = XrErr("xrInitializeLoaderKHR", r);
        return false;
    }

    u32 extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    std::vector<const char*> reqExts = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    };
    for (const char* e : reqExts) {
        if (!HasExt(exts, e)) {
            error = std::string("Missing required OpenXR extension: ") + e;
            return false;
        }
    }
    d.passthroughExt = HasExt(exts, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (d.passthroughExt) reqExts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    d.colorSpaceExt = HasExt(exts, XR_FB_COLOR_SPACE_EXTENSION_NAME);
    if (d.colorSpaceExt) reqExts.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
    d.layerSettingsExt = HasExt(exts, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (d.layerSettingsExt) reqExts.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    d.perfSettingsExt = HasExt(exts, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (d.perfSettingsExt) reqExts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    d.refreshRateExt = HasExt(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (d.refreshRateExt) reqExts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    d.touchPlusExt = HasExt(exts, "XR_META_touch_controller_plus");
    if (d.touchPlusExt) reqExts.push_back("XR_META_touch_controller_plus");
    d.touchProExt = HasExt(exts, "XR_FB_touch_controller_pro");
    if (d.touchProExt) reqExts.push_back("XR_FB_touch_controller_pro");

    XrInstanceCreateInfoAndroidKHR icia{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    icia.applicationVM = vm;
    icia.applicationActivity = d.activityGlobal;

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.next = &icia;
    std::snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "FuturamaRecomp");
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "FuturamaRecomp");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = static_cast<u32>(reqExts.size());
    ici.enabledExtensionNames = reqExts.data();
    r = xrCreateInstance(&ici, &d.instance);
    if (XR_FAILED(r)) {
        error = XrErr("xrCreateInstance", r);
        return false;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(d.instance, &sgi, &d.systemId);
    if (XR_FAILED(r)) {
        error = XrErr("xrGetSystem", r);
        d.Destroy();
        return false;
    }

    if (!d.CreateVulkan(error)) {
        d.Destroy();
        return false;
    }

    XrGraphicsBindingVulkan2KHR gbv{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gbv.instance = d.vkInstance;
    gbv.physicalDevice = d.vkPhys;
    gbv.device = d.vkDevice;
    gbv.queueFamilyIndex = d.queueFamily;
    gbv.queueIndex = 0;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gbv;
    sci.systemId = d.systemId;
    r = xrCreateSession(d.instance, &sci, &d.session);
    if (XR_FAILED(r)) {
        error = XrErr("xrCreateSession", r);
        d.Destroy();
        return false;
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(d.session, &rsci, &d.localSpace))) {
        error = "xrCreateReferenceSpace(LOCAL) failed";
        d.Destroy();
        return false;
    }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(d.session, &rsci, &d.viewSpace))) {
        error = "xrCreateReferenceSpace(VIEW) failed";
        d.Destroy();
        return false;
    }

    d.LoadView();
    if (!d.CreateSwapchains(width, height, error) || !d.CreateActions(error)) {
        d.Destroy();
        return false;
    }
    if (d.colorSpaceExt) {
        PFN_xrSetColorSpaceFB pfnSetColorSpace = nullptr;
        xrGetInstanceProcAddr(d.instance, "xrSetColorSpaceFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnSetColorSpace));
        if (pfnSetColorSpace) {
            pfnSetColorSpace(d.session, XR_COLOR_SPACE_REC709_FB);
        }
    }
    if (d.perfSettingsExt) {
        xrGetInstanceProcAddr(d.instance, "xrPerfSettingsSetPerformanceLevelEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&d.pfnPerfSettingsSetPerformanceLevel));
    }
    if (d.refreshRateExt) {
        xrGetInstanceProcAddr(d.instance, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&d.pfnEnumerateDisplayRefreshRates));
        xrGetInstanceProcAddr(d.instance, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&d.pfnRequestDisplayRefreshRate));
    }
    d.InitPassthrough();
    SDL_Log("OpenXR Theater Quad initialized (passthrough=%d, quad=%ux%u, stereo=%d, fmt=%lld, colorSpace=%d, "
            "layerSettings=%d)",
            (int)(d.passthroughLayer != XR_NULL_HANDLE), d.quadEyeW, d.quadEyeH, (int)d.quadStereo,
            (long long)d.swapFormat, (int)d.colorSpaceExt, (int)d.layerSettingsExt);
    return true;
}

void OpenXrTheater::Shutdown() { impl_->Destroy(); }

bool OpenXrTheater::PollEvents(bool& shouldQuit) {
    Impl& d = *impl_;
    if (!d.instance) return false;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(d.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            d.state = sc->state;
            if (d.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(d.session, &sbi))) {
                    d.sessionRunning = true;
                    d.recenterPending = true;
                    d.ApplyPerfAndRefresh();
                }
            } else if (d.state == XR_SESSION_STATE_STOPPING) {
                if (d.sessionRunning) xrEndSession(d.session);
                d.sessionRunning = false;
            } else if (d.state == XR_SESSION_STATE_EXITING || d.state == XR_SESSION_STATE_LOSS_PENDING) {
                d.sessionRunning = false;
                shouldQuit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            d.LogActiveProfiles();
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool OpenXrTheater::IsSessionRunning() const { return impl_->sessionRunning; }
bool OpenXrTheater::IsSessionFocused() const { return impl_->state == XR_SESSION_STATE_FOCUSED; }

bool OpenXrTheater::BeginFrame() {
    Impl& d = *impl_;
    if (!d.sessionRunning || d.frameActive) return false;

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    d.frameState = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(d.session, &fwi, &d.frameState))) return false;

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(d.session, &fbi))) return false;
    d.frameActive = true;

    d.SyncActions();
    if (d.motorLeft || d.motorRight) d.RefreshHaptics(false);
    if (d.recenterPending) d.DoRecenter(d.frameState.predictedDisplayTime);
    return true;
}

bool OpenXrTheater::Impl::UploadRegions(XrSwapchain sc, const std::vector<XrSwapchainImageVulkanKHR>& images,
                                        VkBuffer buf, const VkBufferImageCopy* regions, u32 regionCount) {
    u32 idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc, &ai, &idx))) return false;

    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(sc, &wi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc, &ri);
        return false;
    }

    vkResetCommandBuffer(cmdBuf, 0);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &cbi);

    VkImage img = images[idx].image;
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = img;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    vkCmdCopyBufferToImage(cmdBuf, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, regions);

    VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = img;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toColor);

    vkEndCommandBuffer(cmdBuf);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuf;
    vkQueueSubmit(vkQueue, 1, &si, cmdFence);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc, &ri);
    return true;
}

bool OpenXrTheater::Impl::CreateMenuSwapchain() {
    const VkDeviceSize bufSize = VkDeviceSize(kMenuW) * kMenuH * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkDevice, &bci, nullptr, &menuBuf) != VK_SUCCESS) return false;
    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(vkDevice, menuBuf, &memReqs);
    const u32 memIdx = FindMemoryType(vkPhys, memReqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = memReqs.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(vkDevice, &mai, nullptr, &menuMem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(vkDevice, menuBuf, menuMem, 0) != VK_SUCCESS) return false;
    if (vkMapMemory(vkDevice, menuMem, 0, bufSize, 0, &menuMapped) != VK_SUCCESS) return false;

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = swapFormat;
    sci.sampleCount = 1;
    sci.width = kMenuW;
    sci.height = kMenuH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &sci, &menuSwapchain))) return false;
    u32 n = 0;
    xrEnumerateSwapchainImages(menuSwapchain, 0, &n, nullptr);
    menuImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(menuSwapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(menuImages.data()));
    return true;
}

void OpenXrTheater::Impl::DrawMenu() {
    std::vector<u8> img(size_t(kMenuW) * kMenuH * 4, 0);
    constexpr int kBand = 190;
    MenuFill(img, 0, 0, kMenuW, kMenuH, 0xF5C842FFu);                    // yellow border
    MenuFill(img, 6, 6, kMenuW - 12, kMenuH - 12, 0x0B1620FFu);          // dark panel
    MenuFill(img, 6, 6, kMenuW - 12, kBand, 0x0E3A48FFu);                // header band (the logo's teal)
    if (logoW && logoH) {   // bilinear fit into the band
        const float sc = std::min(float(kMenuW - 40) / logoW, float(kBand - 16) / logoH);
        const int dw = int(logoW * sc), dh = int(logoH * sc);
        const int ox = (int(kMenuW) - dw) / 2, oy = 6 + (kBand - dh) / 2;
        for (int y = 0; y < dh; ++y) {
            const float fy = std::clamp((y + 0.5f) / sc - 0.5f, 0.0f, float(logoH - 1));
            const int y0 = int(fy), y1 = std::min<int>(y0 + 1, logoH - 1);
            const float ty = fy - y0;
            for (int x = 0; x < dw; ++x) {
                const float fx = std::clamp((x + 0.5f) / sc - 0.5f, 0.0f, float(logoW - 1));
                const int x0 = int(fx), x1 = std::min<int>(x0 + 1, logoW - 1);
                const float tx = fx - x0;
                u8* d = &img[(size_t(oy + y) * kMenuW + ox + x) * 4];
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int xx, int yy) { return float(logo[(size_t(yy) * logoW + xx) * 4 + c]); };
                    const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
                                    (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                    d[c] = u8(std::clamp(v + 0.5f, 0.0f, 255.0f));
                }
                d[3] = 255;
            }
        }
    } else {
        MenuText(img, (kMenuW - MenuTextWidth("FUTURAMA", 12)) / 2, 6 + (kBand - 84) / 2, 12, 0xF5C842FFu, "FUTURAMA");
    }

    const char* labels[kMenuCount] = {"RESUME", "BACKGROUND", "3D", "ASPECT", "RECENTER SCREEN",
                                      "RESET SCREEN SIZE", "EXIT GAME"};
    const char* values[kMenuCount] = {"", darkMode ? "DARK ROOM" : "MIXED REALITY", stereoState ? "ON" : "OFF",
                                      aspect43 ? "4:3" : "16:9", "", "", exitArmed ? "PRESS A AGAIN" : ""};
    if (!passthroughLayer) values[kMenuBackground] = "DARK ROOM";
    const int top = kBand + 6 + 22;
    for (int i = 0; i < kMenuCount; ++i) {
        const int y = top + i * 54;
        const bool sel = i == menuSel;
        if (sel) MenuFill(img, 20, y - 8, kMenuW - 40, 50, 0xF5C842FFu);
        const u32 fg = sel ? 0x0B1620FFu : 0xFFFFFFFFu;
        MenuText(img, 44, y, 5, fg, labels[i]);
        if (values[i][0]) {
            char v[64];
            if (i == kMenuExit) std::snprintf(v, sizeof v, "%s", values[i]);
            else std::snprintf(v, sizeof v, "- %s +", values[i]);
            MenuText(img, kMenuW - 44 - MenuTextWidth(v, 4), y + 4, 4, sel ? 0x0B1620FFu : 0x8FE3F0FFu, v);
        }
    }
    if (aspect43 != startupAspect43)
        MenuText(img, 44, kMenuH - 92, 4, 0xF5C842FFu, "ASPECT CHANGES AFTER A RESTART");
    MenuText(img, 44, kMenuH - 44, 3, 0xB8C4E0FFu, "STICK: CHANGE  A: OK  B: CLOSE  GRIPS: MOVE");
    std::memcpy(menuMapped, img.data(), img.size());
}

// Up/down select, left/right (or A) change, B or a Menu tap closes. Edge-triggered.
void OpenXrTheater::Impl::MenuInput(bool up, bool down, bool left, bool right, bool a, bool b) {
    const bool now[6] = {up, down, left, right, a, b};
    bool edge[6];
    for (int i = 0; i < 6; ++i) edge[i] = now[i] && !prevNav[i], prevNav[i] = now[i];
    if (edge[0]) menuSel = (menuSel + kMenuCount - 1) % kMenuCount, exitArmed = false, menuDirty = true;
    if (edge[1]) menuSel = (menuSel + 1) % kMenuCount, exitArmed = false, menuDirty = true;
    if (edge[2]) MenuActivate(-1);
    if (edge[3]) MenuActivate(+1);
    if (edge[4]) MenuActivate(0);
    if (edge[5]) SetMenuOpen(false);
}

void OpenXrTheater::Impl::MenuActivate(int dir) {
    menuDirty = true;
    switch (menuSel) {
    case kMenuResume:
        if (dir == 0) SetMenuOpen(false);
        break;
    case kMenuBackground:
        if (!passthroughLayer) break;
        darkMode = !darkMode;
        ApplyBackground();
        SaveView();
        break;
    case kMenu3D:
        stereoTogglePending = true;   // GameMain flips it and reports back through SetStereoState
        break;
    case kMenuAspect:
        aspect43 = !aspect43;
        SaveView();
        break;
    case kMenuRecenter:
        if (dir == 0) recenterPending = true;
        break;
    case kMenuResetSize:
        if (dir == 0) {
            viewDist = kDefaultDist;
            viewHeight = kDefaultHeight;
            viewElev = 0.0f;
            UpdateQuadSize();
            recenterPending = true;
            SaveView();
        }
        break;
    case kMenuExit:
        if (dir != 0) break;
        if (exitArmed) exitRequested = true;
        else exitArmed = true;
        break;
    }
}

void OpenXrTheater::PresentFrame(const void* rgba, u32 width, u32 height) {
    Impl& d = *impl_;
    if (!d.frameActive || !rgba || !width || !height) return;
    if (!d.RecreateQuadSwapchain(width, height, false)) return;

    vkWaitForFences(d.vkDevice, 1, &d.cmdFence, VK_TRUE, UINT64_MAX);
    vkResetFences(d.vkDevice, 1, &d.cmdFence);
    std::memcpy(d.stagingMapped, rgba, size_t(width) * height * 4);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    d.UploadQuadRegions(&region, 1);
}

void OpenXrTheater::PresentStereoFrame(const void* leftRgba, const void* rightRgba, u32 eyeWidth, u32 eyeHeight) {
    Impl& d = *impl_;
    if (!d.frameActive || !leftRgba || !rightRgba || !eyeWidth || !eyeHeight) return;
    if (!d.RecreateQuadSwapchain(eyeWidth, eyeHeight, true)) return;

    vkWaitForFences(d.vkDevice, 1, &d.cmdFence, VK_TRUE, UINT64_MAX);
    vkResetFences(d.vkDevice, 1, &d.cmdFence);

    const size_t eyeBytes = size_t(eyeWidth) * eyeHeight * 4;
    u8* mapped = static_cast<u8*>(d.stagingMapped);
    std::memcpy(mapped, leftRgba, eyeBytes);
    std::memcpy(mapped + eyeBytes, rightRgba, eyeBytes);

    VkBufferImageCopy regions[2]{};
    regions[0].bufferOffset = 0;
    regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    regions[0].imageOffset = {0, 0, 0};
    regions[0].imageExtent = {eyeWidth, eyeHeight, 1};

    regions[1].bufferOffset = eyeBytes;
    regions[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    regions[1].imageOffset = {static_cast<int32_t>(eyeWidth), 0, 0};
    regions[1].imageExtent = {eyeWidth, eyeHeight, 1};
    d.UploadQuadRegions(regions, 2);
}

void OpenXrTheater::EndFrame() {
    Impl& d = *impl_;
    if (!d.frameActive) return;
    d.frameActive = false;

    const bool showMenu = d.menuOpen && d.menuSwapchain && d.frameState.shouldRender;
    if (showMenu && d.menuDirty) {
        d.menuDirty = false;
        vkWaitForFences(d.vkDevice, 1, &d.cmdFence, VK_TRUE, UINT64_MAX);
        vkResetFences(d.vkDevice, 1, &d.cmdFence);
        d.DrawMenu();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kMenuW, kMenuH, 1};
        if (!d.UploadRegions(d.menuSwapchain, d.menuImages, d.menuBuf, &region, 1)) {
            d.menuDirty = true;
            // (no submit happened: re-signal the fence so the next wait does not hang)
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            vkQueueSubmit(d.vkQueue, 1, &si, d.cmdFence);
        }
    }

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerPassthroughFB passthroughComp{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (d.passthroughLayer && !d.darkMode) {
        passthroughComp.layerHandle = d.passthroughLayer;
        passthroughComp.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughComp));
    }

    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrCompositionLayerProjectionView projViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    if (d.frameState.shouldRender) {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = d.frameState.predictedDisplayTime;
        vli.space = d.localSpace;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        u32 viewCount = 0;
        if (XR_SUCCEEDED(xrLocateViews(d.session, &vli, &vs, 2, &viewCount, views)) && viewCount == 2 &&
            (vs.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) ==
                (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            u32 indices[2] = {};
            for (u32 eye = 0; eye < 2; ++eye) {
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(d.eyes[eye].swapchain, &ai, &indices[eye]);
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(d.eyes[eye].swapchain, &wi);
            }

            vkWaitForFences(d.vkDevice, 1, &d.cmdFence, VK_TRUE, UINT64_MAX);
            vkResetFences(d.vkDevice, 1, &d.cmdFence);
            vkResetCommandBuffer(d.cmdBuf, 0);
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(d.cmdBuf, &cbi);

            VkClearValue clear{};
            const bool mr = d.passthroughLayer && !d.darkMode;
            if (mr) clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            else clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            for (u32 eye = 0; eye < 2; ++eye) {
                VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                rpbi.renderPass = d.projRenderPass;
                rpbi.framebuffer = d.eyes[eye].framebuffers[indices[eye]];
                rpbi.renderArea = {{0, 0}, {Impl::kProjDim, Impl::kProjDim}};
                rpbi.clearValueCount = 1;
                rpbi.pClearValues = &clear;
                vkCmdBeginRenderPass(d.cmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(d.cmdBuf);
            }
            vkEndCommandBuffer(d.cmdBuf);

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &d.cmdBuf;
            vkQueueSubmit(d.vkQueue, 1, &si, d.cmdFence);

            for (u32 eye = 0; eye < 2; ++eye) {
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(d.eyes[eye].swapchain, &ri);

                projViews[eye].pose = views[eye].pose;
                projViews[eye].fov = views[eye].fov;
                projViews[eye].subImage.swapchain = d.eyes[eye].swapchain;
                projViews[eye].subImage.imageRect = {{0, 0}, {Impl::kProjDim, Impl::kProjDim}};
                projViews[eye].subImage.imageArrayIndex = 0;
            }
            projLayer.layerFlags = mr ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            projLayer.space = d.localSpace;
            projLayer.viewCount = 2;
            projLayer.views = projViews;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer));
        }
    }

    XrCompositionLayerQuad quadLayers[2] = {
        {XR_TYPE_COMPOSITION_LAYER_QUAD},
        {XR_TYPE_COMPOSITION_LAYER_QUAD},
    };
    XrCompositionLayerSettingsFB quadSettings[2] = {
        {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB},
        {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB},
    };
    if (d.hasQuadFrame && d.frameState.shouldRender) {
        const u32 eyesCount = d.quadStereo ? 2u : 1u;
        const int32_t eyeW = static_cast<int32_t>(d.quadEyeW);
        const int32_t eyeH = static_cast<int32_t>(d.quadEyeH);
        for (u32 eye = 0; eye < eyesCount; ++eye) {
            XrCompositionLayerQuad& ql = quadLayers[eye];
            if (d.layerSettingsExt) {
                quadSettings[eye].layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
                                               XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
                ql.next = &quadSettings[eye];
            }
            ql.layerFlags = 0;
            ql.space = d.localSpace;
            ql.eyeVisibility = !d.quadStereo ? XR_EYE_VISIBILITY_BOTH
                             : eye == 0      ? XR_EYE_VISIBILITY_LEFT
                                             : XR_EYE_VISIBILITY_RIGHT;
            ql.subImage.swapchain = d.quadSwapchain;
            ql.subImage.imageRect = {{static_cast<int32_t>(eye) * eyeW, 0}, {eyeW, eyeH}};
            ql.subImage.imageArrayIndex = 0;
            ql.pose = d.quadPose;
            ql.size = d.quadSize;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ql));
        }
    }

    XrCompositionLayerQuad menuLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (showMenu) {
        const XrVector3f normal = Rotate(d.quadPose.orientation, V3(0.0f, 0.0f, 1.0f));
        menuLayer.layerFlags = 0;
        menuLayer.space = d.localSpace;
        menuLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        menuLayer.subImage.swapchain = d.menuSwapchain;
        menuLayer.subImage.imageRect = {{0, 0}, {int32_t(kMenuW), int32_t(kMenuH)}};
        menuLayer.pose = {d.quadPose.orientation, Add(d.quadPose.position, Mul(normal, 0.25f))};
        const float mh = std::clamp(d.viewHeight * 0.75f, 0.45f, 2.4f);
        menuLayer.size = {mh * float(kMenuW) / float(kMenuH), mh};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuLayer));
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = d.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = static_cast<u32>(layers.size());
    fei.layers = layers.data();
    xrEndFrame(d.session, &fei);
}

const input::GamepadState& OpenXrTheater::Gamepad() const { return impl_->gamepad; }

bool OpenXrTheater::ConsumeStereoToggle() {
    const bool t = impl_->stereoTogglePending;
    impl_->stereoTogglePending = false;
    return t;
}

void OpenXrTheater::Impl::LogActiveProfiles() {
    profileLogged = true;
    for (int i = 0; i < 2; ++i) {
        XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult r = xrGetCurrentInteractionProfile(session, handPaths[i], &ips);
        char buf[XR_MAX_PATH_LENGTH] = "<none>";
        if (XR_SUCCEEDED(r) && ips.interactionProfile != XR_NULL_PATH) {
            uint32_t len = 0;
            xrPathToString(instance, ips.interactionProfile, sizeof(buf), &len, buf);
        }
        SDL_Log("OpenXR hand[%d] active profile: %s (res=%d, touchPlusExt=%d, touchProExt=%d)", i, buf,
                static_cast<int>(r), static_cast<int>(touchPlusExt), static_cast<int>(touchProExt));
    }
}

void OpenXrTheater::Impl::RefreshHaptics(bool forceImmediate) {
    if (!sessionRunning || state != XR_SESSION_STATE_FOCUSED || !haptic) return;
    const u64 nowMs = SDL_GetTicks();
    const u16 vals[2] = {motorLeft, motorRight};
    for (int i = 0; i < 2; ++i) {
        XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
        hai.action = haptic;
        hai.subactionPath = handPaths[i];
        if (vals[i] == 0) {
            if (lastHapticVal[i] != 0 || forceImmediate) {
                const XrResult r = xrStopHapticFeedback(session, &hai);
                if (XR_FAILED(r)) {
                    SDL_Log("OpenXR xrStopHapticFeedback hand[%d] failed: %d", i, static_cast<int>(r));
                }
            }
            lastHapticVal[i] = 0;
            lastHapticApplyMs[i] = 0;
            continue;
        }
        const int diff = std::abs(static_cast<int>(vals[i]) - static_cast<int>(lastHapticVal[i]));
        const bool due = (nowMs - lastHapticApplyMs[i]) >= 65;
        if (lastHapticVal[i] != 0 && !due && (!forceImmediate || diff < 6554)) continue;

        XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
        vib.duration = 100'000'000LL;  // 100ms, refreshed every ~65ms while motor active
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        vib.amplitude = std::clamp(float(vals[i]) / 65535.0f, 0.0f, 1.0f);
        const XrResult r = xrApplyHapticFeedback(session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
        if (XR_FAILED(r)) {
            SDL_Log("OpenXR xrApplyHapticFeedback hand[%d] failed: %d", i, static_cast<int>(r));
        }
        lastHapticVal[i] = vals[i];
        lastHapticApplyMs[i] = nowMs;
    }
}

void OpenXrTheater::SetHaptics(u16 left, u16 right) {
    Impl& d = *impl_;
    if (left == d.motorLeft && right == d.motorRight) return;
    d.motorLeft = left;
    d.motorRight = right;
    d.RefreshHaptics(true);
}

void OpenXrTheater::Recenter() { impl_->recenterPending = true; }

bool OpenXrTheater::MenuOpen() const { return impl_->menuOpen; }

bool OpenXrTheater::ConsumeExitRequest() {
    const bool r = impl_->exitRequested;
    impl_->exitRequested = false;
    return r;
}

void OpenXrTheater::SetMenuLogo(const u8* rgba, u32 width, u32 height) {
    Impl& d = *impl_;
    if (!rgba || !width || !height) return;
    d.logo.assign(rgba, rgba + size_t(width) * height * 4);
    d.logoW = width;
    d.logoH = height;
    d.menuDirty = true;
}

void OpenXrTheater::SetStereoState(bool stereo) {
    Impl& d = *impl_;
    if (d.stereoState == stereo) return;
    d.stereoState = stereo;
    d.menuDirty = true;
    d.SaveView();
}

bool OpenXrTheater::LoadStartupPrefs(bool& stereo, bool& aspect43) {
    SavedView v;
    if (!ReadSavedView(PrefsPath(), v)) return false;
    stereo = v.stereo != 0;
    aspect43 = v.aspect43 != 0;
    return true;
}

}  // namespace fut::platform

#endif  // __ANDROID__
