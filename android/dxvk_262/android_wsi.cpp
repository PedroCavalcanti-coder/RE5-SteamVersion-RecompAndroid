#include <android/native_window.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

#include "src/wsi/wsi_platform.h"
#include "src/wsi/wsi_monitor.h"
#include "src/wsi/wsi_window.h"
#include "src/util/log/log.h"
#include "src/util/util_string.h"

namespace {
std::mutex s_screenMutex;
uint32_t s_screenWidth = 1280, s_screenHeight = 720;
HMONITOR s_monitor() { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }
ANativeWindow* s_nativeWindow(HWND window) { return reinterpret_cast<ANativeWindow*>(window); }
std::atomic<PFN_vkGetInstanceProcAddr> s_instanceProcAddr{nullptr};
}

extern "C" __attribute__((visibility("default"))) void re5_dxvk_set_screen(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> guard(s_screenMutex);
    if (width && height) {
        if (width < height) std::swap(width, height);
        s_screenWidth = width;
        s_screenHeight = height;
    }
}

extern "C" __attribute__((visibility("default"))) void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr entry) {
    s_instanceProcAddr = entry;
}

extern "C" __attribute__((visibility("default"))) PFN_vkGetInstanceProcAddr dxvkAndroidGetInstanceProcAddr() {
    auto entry = s_instanceProcAddr.load();
    return entry ? entry : nullptr;
}

namespace dxvk::wsi {

  class AndroidWsiDriver : public WsiDriver {
  public:
    AndroidWsiDriver() = default;
    ~AndroidWsiDriver() override = default;

    std::vector<const char *> getInstanceExtensions() override {
      return {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
      };
    }

    HMONITOR getDefaultMonitor() override {
      return s_monitor();
    }

    HMONITOR enumMonitors(uint32_t index) override {
      return index == 0 ? s_monitor() : nullptr;
    }

    HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t index) override {
      return index == 0 ? s_monitor() : nullptr;
    }

    bool getDisplayName(HMONITOR hMonitor, WCHAR (&name)[32]) override {
      if (hMonitor != s_monitor()) return false;
      const wchar_t display[] = L"\\\\.\\DISPLAY1";
      std::fill(std::begin(name), std::end(name), 0);
      std::copy(std::begin(display), std::end(display), name);
      return true;
    }

    bool getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) override {
      if (hMonitor != s_monitor() || !pRect) return false;
      std::lock_guard<std::mutex> guard(s_screenMutex);
      *pRect = {0, 0, static_cast<LONG>(s_screenWidth), static_cast<LONG>(s_screenHeight)};
      return true;
    }

    bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) override {
      if (hMonitor != s_monitor() || !pMode) return false;
      static const struct { uint32_t w, h; } k_modes[] = {
        {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1920, 1080},
      };
      std::lock_guard<std::mutex> guard(s_screenMutex);
      if (modeNumber < sizeof(k_modes) / sizeof(k_modes[0])) {
        if (k_modes[modeNumber].w <= s_screenWidth && k_modes[modeNumber].h <= s_screenHeight) {
          *pMode = {k_modes[modeNumber].w, k_modes[modeNumber].h, {60, 1}, 32, false};
          return true;
        }
      }
      if (modeNumber == sizeof(k_modes) / sizeof(k_modes[0])) {
        *pMode = {s_screenWidth, s_screenHeight, {60, 1}, 32, false};
        return true;
      }
      return false;
    }

    bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override {
      if (hMonitor != s_monitor() || !pMode) return false;
      std::lock_guard<std::mutex> guard(s_screenMutex);
      *pMode = {s_screenWidth, s_screenHeight, {60, 1}, 32, false};
      return true;
    }

    bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override {
      return getCurrentDisplayMode(hMonitor, pMode);
    }

    WsiEdidData getMonitorEdid(HMONITOR) override {
      return {};
    }

    void getWindowSize(HWND, uint32_t* pWidth, uint32_t* pHeight) override {
      std::lock_guard<std::mutex> guard(s_screenMutex);
      if (pWidth) *pWidth = s_screenWidth;
      if (pHeight) *pHeight = s_screenHeight;
    }

    void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) override {}

    bool setWindowMode(HMONITOR, HWND, DxvkWindowState*, const WsiMode&) override {
      return true;
    }

    bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) override {
      return true;
    }

    bool leaveFullscreenMode(HWND, DxvkWindowState*, bool) override {
      return true;
    }

    bool restoreDisplayMode() override {
      return true;
    }

    HMONITOR getWindowMonitor(HWND) override {
      return s_monitor();
    }

    bool isWindow(HWND hWindow) override {
      return hWindow != nullptr;
    }

    bool isMinimized(HWND) override {
      return false;
    }

    bool isOccluded(HWND) override {
      return false;
    }

    void updateFullscreenWindow(HMONITOR, HWND, bool) override {}

    VkResult createSurface(HWND hWindow, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkSurfaceKHR* pSurface) override {
      if (!hWindow) return VK_ERROR_SURFACE_LOST_KHR;
      auto customGipa = dxvkAndroidGetInstanceProcAddr();
      auto effGipa = customGipa ? customGipa : gipa;
      auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(effGipa(instance, "vkCreateAndroidSurfaceKHR"));
      if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
      VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
      info.window = s_nativeWindow(hWindow);
      VkResult vr = create(instance, &info, nullptr, pSurface);
      if (vr != VK_SUCCESS)
        Logger::err(str::format("Android WSI: vkCreateAndroidSurfaceKHR = ", vr));
      return vr;
    }
  };

  static bool createAndroidWsiDriver(WsiDriver **driver) {
    *driver = new AndroidWsiDriver();
    return true;
  }

  WsiBootstrap AndroidWSI = {
    "SDL2", // Matched by DXVK_WSI_DRIVER="SDL2"
    createAndroidWsiDriver
  };

  std::optional<WsiDisplayMetadata> parseColorimetryInfo(const WsiEdidData&) {
    return std::nullopt;
  }

}

