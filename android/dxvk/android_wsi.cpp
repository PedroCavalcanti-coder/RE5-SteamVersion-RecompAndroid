// WSI da DXVK native 1.9.2b no Android para o re5: a "HWND" que a ponte D3D9
// do runtime entrega (runtime/src/gfx/d3d9_host.c) é o ANativeWindow* da janela
// SDL2, tirado de SDL_GetWindowWMInfo. Assim a libdxvk_d3d9.so não depende do
// SDL (que está estático dentro da libre5.so) e a superfície Vulkan sai direto
// de vkCreateAndroidSurfaceKHR.
//
// Baseado em tools/nfsu2-reference/ports/dxvk_android/android_wsi.cpp; lá a
// janela era registrada à parte (dxvkAndroidSetWindow), aqui é o próprio HWND.
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <mutex>

#include "src/wsi/wsi_presenter.h"
#include "src/wsi/wsi_window.h"
#include "src/wsi/wsi_monitor.h"
#include "src/dxvk/dxvk_platform_exts.h"

namespace {
std::mutex screenMutex;
// Tamanho da tela (orientação atual), dado pelo runtime antes do Direct3DCreate9.
uint32_t screenWidth = 1280, screenHeight = 720;
HMONITOR monitor() { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }
ANativeWindow* nativeWindow(HWND window) { return reinterpret_cast<ANativeWindow*>(window); }
// O driver Vulkan que a DXVK usa (o patch do vulkan_loader.cpp vem do porte do
// NFSU2, que permite trocar pelo adrenotools); aqui é sempre o do sistema.
std::atomic<PFN_vkGetInstanceProcAddr> instanceProcAddr{nullptr};
}

extern "C" __attribute__((visibility("default"))) void re5_dxvk_set_screen(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> guard(screenMutex);
    if (width && height) {
        if (width < height) std::swap(width, height);
        screenWidth = width;
        screenHeight = height;
    }
}
extern "C" void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr entry) { instanceProcAddr = entry; }
extern "C" PFN_vkGetInstanceProcAddr dxvkAndroidGetInstanceProcAddr() {
    auto entry = instanceProcAddr.load();
    return entry ? entry : vkGetInstanceProcAddr;
}

namespace dxvk {
DxvkPlatformExts DxvkPlatformExts::s_instance;
std::string_view DxvkPlatformExts::getName() { return "Android WSI (re5)"; }
DxvkNameSet DxvkPlatformExts::getInstanceExtensions() {
    DxvkNameSet names;
    names.add(VK_KHR_SURFACE_EXTENSION_NAME);
    names.add(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    return names;
}
DxvkNameSet DxvkPlatformExts::getDeviceExtensions(uint32_t) { return {}; }
void DxvkPlatformExts::initInstanceExtensions() {}
void DxvkPlatformExts::initDeviceExtensions(const DxvkInstance*) {}
}

namespace dxvk::wsi {
VkResult createSurface(HWND window, const Rc<vk::InstanceFn>& instance, VkSurfaceKHR* surface) {
    if (!window) return VK_ERROR_SURFACE_LOST_KHR;
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        dxvkAndroidGetInstanceProcAddr()(instance->instance(), "vkCreateAndroidSurfaceKHR"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = nativeWindow(window);
    VkResult vr = create(instance->instance(), &info, nullptr, surface);
    if (vr != VK_SUCCESS)
        Logger::err(str::format("Android WSI: vkCreateAndroidSurfaceKHR(", window, ") = ", vr));
    return vr;
}

HMONITOR getDefaultMonitor() { return monitor(); }
HMONITOR enumMonitors(uint32_t index) { return index ? nullptr : monitor(); }
bool getDisplayName(HMONITOR handle, WCHAR (&name)[32]) {
    if (handle != monitor()) return false;
    const wchar_t display[] = L"\\\\.\\DISPLAY1";
    std::fill(std::begin(name), std::end(name), 0);
    std::copy(std::begin(display), std::end(display), name);
    return true;
}
bool getDesktopCoordinates(HMONITOR handle, RECT* rect) {
    if (handle != monitor() || !rect) return false;
    std::lock_guard<std::mutex> guard(screenMutex);
    *rect = {0, 0, static_cast<LONG>(screenWidth), static_cast<LONG>(screenHeight)};
    return true;
}
bool getCurrentDisplayMode(HMONITOR handle, WsiMode* mode) {
    if (handle != monitor() || !mode) return false;
    std::lock_guard<std::mutex> guard(screenMutex);
    *mode = {screenWidth, screenHeight, {60, 1}, 32, false};
    return true;
}
bool getDesktopDisplayMode(HMONITOR handle, WsiMode* mode) { return getCurrentDisplayMode(handle, mode); }
// Modos de PC até o tamanho da tela, e por último a própria tela: o jogo escolhe
// a resolução do config.ini dentre os que o adaptador lista.
bool getDisplayMode(HMONITOR handle, uint32_t index, WsiMode* mode) {
    if (handle != monitor() || !mode) return false;
    static constexpr uint32_t pcModes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720},
                                              {1280, 960}, {1366, 768}, {1600, 900}, {1920, 1080}};
    std::lock_guard<std::mutex> guard(screenMutex);
    uint32_t n = 0;
    for (auto& m : pcModes) {
        if (m[0] > screenWidth || m[1] > screenHeight || (m[0] == screenWidth && m[1] == screenHeight)) continue;
        if (n++ == index) { *mode = {m[0], m[1], {60, 1}, 32, false}; return true; }
    }
    if (n != index) return false;
    *mode = {screenWidth, screenHeight, {60, 1}, 32, false};
    return true;
}

void getWindowSize(HWND window, uint32_t* width, uint32_t* height) {
    ANativeWindow* w = nativeWindow(window);
    uint32_t rw = w ? static_cast<uint32_t>(ANativeWindow_getWidth(w)) : 0;
    uint32_t rh = w ? static_cast<uint32_t>(ANativeWindow_getHeight(w)) : 0;
    if (rw && rh && rw < rh) std::swap(rw, rh);
    if (width) *width = rw;
    if (height) *height = rh;
}
// A janela do Android tem o tamanho da tela; o back buffer é escalado no Present.
void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) {}
bool isWindow(HWND window) { return window != nullptr; }
HMONITOR getWindowMonitor(HWND window) { return isWindow(window) ? monitor() : nullptr; }
bool setWindowMode(HMONITOR handle, HWND window, const WsiMode*, bool) { return handle == monitor() && isWindow(window); }
bool enterFullscreenMode(HMONITOR handle, HWND window, DxvkWindowState*, bool) { return handle == monitor() && isWindow(window); }
bool leaveFullscreenMode(HWND window, DxvkWindowState*) { return isWindow(window); }
bool restoreDisplayMode(HMONITOR handle) { return handle == monitor(); }
}
