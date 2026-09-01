// 截图模块：屏幕捕获管线（预截屏、GDI 采集、DPI/显示器/窗口枚举）
#include "internal.h"

static void ReleasePrimedScreenshotFrameLocked() {
    if (g_primedScreenshotFrame.bitmap) {
        DeleteObject(g_primedScreenshotFrame.bitmap);
        g_primedScreenshotFrame.bitmap = NULL;
    }
    g_primedScreenshotFrame.vx = 0;
    g_primedScreenshotFrame.vy = 0;
    g_primedScreenshotFrame.vw = 0;
    g_primedScreenshotFrame.vh = 0;
    g_primedScreenshotFrame.dpiScale = 1.0;
    g_primedScreenshotFrame.capturedAt = std::chrono::steady_clock::time_point{};
    g_primedScreenshotFrame.valid = false;
}

// ---- 工具函数 ----

// 获取 DPI 缩放因子
double GetDpiScaleFactor() {
    typedef UINT (WINAPI *GetDpiForSystemProc)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto proc = (GetDpiForSystemProc)GetProcAddress(user32, "GetDpiForSystem");
        if (proc) {
            UINT dpi = proc();
            double scale = dpi / 96.0;
            if (scale < 0.5) scale = 0.5;
            if (scale > 4.0) scale = 4.0;
            return scale;
        }
    }
    return 1.0;
}

// 显示器枚举回调

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(dwData);

    // 获取显示器的物理尺寸
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        // 在 DPI 感知模式下，rcMonitor 已经是物理像素坐标
        data->minLeft = (std::min)(data->minLeft, mi.rcMonitor.left);
        data->minTop = (std::min)(data->minTop, mi.rcMonitor.top);
        data->maxRight = (std::max)(data->maxRight, mi.rcMonitor.right);
        data->maxBottom = (std::max)(data->maxBottom, mi.rcMonitor.bottom);
        data->monitorCount++;

        // 获取显示器 DPI（shcore.dll 由外层 CaptureVirtualScreen 一次性 LoadLibrary，
        // 此处只 GetProcAddress+调用，避免每次回调重复 Load/Free）
        if (data->shcore) {
            typedef HRESULT(WINAPI* GetDpiForMonitorProc)(HMONITOR, int, UINT*, UINT*);
            auto getDpiForMonitor = (GetDpiForMonitorProc)GetProcAddress(data->shcore, "GetDpiForMonitor");
            if (getDpiForMonitor) {
                UINT dpiX, dpiY;
                if (SUCCEEDED(getDpiForMonitor(hMonitor, 0/*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY))) {
                    data->totalDpiScale = (std::max)(data->totalDpiScale, dpiX / 96.0);
                }
            }
        }
    }
    return TRUE;
}

// 截取整个虚拟屏幕到物理尺寸位图

static bool CaptureVirtualScreen(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    // 获取逻辑坐标的虚拟屏幕尺寸
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // 枚举所有显示器获取物理像素边界（shcore.dll 一次性 Load，回调内复用避免重复 Load）
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    MonitorEnumData enumData = { INT_MAX, INT_MAX, INT_MIN, INT_MIN, 1.0, 0, shcore };
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&enumData));
    if (shcore) FreeLibrary(shcore);

    // 计算物理尺寸（使用枚举得到的实际物理像素边界）
    int physVx = enumData.minLeft;
    int physVy = enumData.minTop;
    int physVw = enumData.maxRight - enumData.minLeft;
    int physVh = enumData.maxBottom - enumData.minTop;

    // 如果枚举失败，回退到 DPI 缩放计算
    if (physVw <= 0 || physVh <= 0 || enumData.monitorCount == 0) {
        physVx = (int)(vx * dpiScale);
        physVy = (int)(vy * dpiScale);
        physVw = (int)(vw * dpiScale + 0.5);
        physVh = (int)(vh * dpiScale + 0.5);
    }

    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;

    outMemDC = CreateCompatibleDC(screenDC);
    if (!outMemDC) { ReleaseDC(NULL, screenDC); return false; }

    outBitmap = CreateCompatibleBitmap(screenDC, physVw, physVh);
    if (!outBitmap) { DeleteDC(outMemDC); ReleaseDC(NULL, screenDC); return false; }

    SelectObject(outMemDC, outBitmap);

    // 直接 BitBlt 物理像素（在 DPI 感知模式下，屏幕 DC 和坐标都是物理像素级别）。
    // UAC 安全桌面激活/锁屏瞬断等场景该调用会失败，CompatibleBitmap 内容此时未初始化
    // （GDI 不保证清零），照常返回成功会把整幅黑图报给上层；失败时清理全部 GDI 对象并
    // 返回 false，由调用方回退链承接（AcquireScreenshotBase 失败分支 / 预抓取降级重采）。
    if (!BitBlt(outMemDC, 0, 0, physVw, physVh, screenDC, physVx, physVy, SRCCOPY | CAPTUREBLT)) {
        DeleteDC(outMemDC);      // 先解除位图选中，再删除位图
        outMemDC = NULL;
        DeleteObject(outBitmap);
        outBitmap = NULL;
        ReleaseDC(NULL, screenDC);
        return false;
    }

    // 更新返回的 dpiScale 为实际的物理/逻辑比例
    // 这样后续的坐标转换才能正确
    if (vw > 0 && vh > 0) {
        dpiScale = (double)physVw / vw;
    }

    ReleaseDC(NULL, screenDC);
    return true;
}

// 创建双缓冲

bool CreateBackBuffer(HDC& outDC, HBITMAP& outBmp, int w, int h) {
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;
    outDC = CreateCompatibleDC(screenDC);
    if (!outDC) { ReleaseDC(NULL, screenDC); return false; }
    outBmp = CreateCompatibleBitmap(screenDC, w, h);
    if (!outBmp) { DeleteDC(outDC); ReleaseDC(NULL, screenDC); return false; }
    SelectObject(outDC, outBmp);
    ReleaseDC(NULL, screenDC);
    return true;
}

// 立即抓取当前虚拟屏幕首帧，供后续截图流程复用。

bool PrimeScreenshotFrameNow() {
    HDC memDC = NULL;
    HBITMAP bitmap = NULL;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    double dpiScale = 1.0;
    if (!CaptureVirtualScreen(memDC, bitmap, vx, vy, vw, vh, dpiScale)) {
        return false;
    }

    if (memDC) {
        DeleteDC(memDC);
    }

    std::lock_guard<std::mutex> lock(g_primedScreenshotFrameMutex);
    ReleasePrimedScreenshotFrameLocked();
    g_primedScreenshotFrame.bitmap = bitmap;
    g_primedScreenshotFrame.vx = vx;
    g_primedScreenshotFrame.vy = vy;
    g_primedScreenshotFrame.vw = vw;
    g_primedScreenshotFrame.vh = vh;
    g_primedScreenshotFrame.dpiScale = dpiScale;
    g_primedScreenshotFrame.capturedAt = std::chrono::steady_clock::now();
    g_primedScreenshotFrame.valid = true;
    return true;
}

static bool ConsumePrimedScreenshotFrame(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    std::lock_guard<std::mutex> lock(g_primedScreenshotFrameMutex);
    if (!g_primedScreenshotFrame.valid || !g_primedScreenshotFrame.bitmap) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_primedScreenshotFrame.capturedAt > SC_PRIMED_FRAME_TTL) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    outMemDC = CreateCompatibleDC(screenDC);
    ReleaseDC(NULL, screenDC);
    if (!outMemDC) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    SelectObject(outMemDC, g_primedScreenshotFrame.bitmap);
    outBitmap = g_primedScreenshotFrame.bitmap;
    vx = g_primedScreenshotFrame.vx;
    vy = g_primedScreenshotFrame.vy;
    vw = g_primedScreenshotFrame.vw;
    vh = g_primedScreenshotFrame.vh;
    dpiScale = g_primedScreenshotFrame.dpiScale;
    g_primedScreenshotFrame.bitmap = NULL;
    g_primedScreenshotFrame.valid = false;
    return true;
}

bool AcquireScreenshotBase(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    if (ConsumePrimedScreenshotFrame(outMemDC, outBitmap, vx, vy, vw, vh, dpiScale)) {
        return true;
    }
    return CaptureVirtualScreen(outMemDC, outBitmap, vx, vy, vw, vh, dpiScale);
}

// 从预截屏位图读取像素颜色（逻辑坐标）

COLORREF GetPixelColorFromBitmap(HDC memDC, int x, int y, int vx, int vy, double dpiScale) {
    int lx = x - vx;
    int ly = y - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    return GetPixel(memDC, px, py);
}

// COLORREF 转 HEX/RGB 字符串

void ColorrefToStrings(COLORREF color, char* hexBuf, char* rgbBuf) {
    int r = color & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = (color >> 16) & 0xFF;
    sprintf_s(hexBuf, 32, "#%02X%02X%02X", r, g, b);
    sprintf_s(rgbBuf, 32, "%d, %d, %d", r, g, b);
}

// 枚举窗口回调

static BOOL CALLBACK SCEnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<SCWindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style == 0) return TRUE;

    // 检查是否为幽灵窗口（cloaked window）
    // 幽灵窗口虽然 IsWindowVisible 返回 true，但实际上不可见
    BOOL isCloaked = FALSE;
    HRESULT hrCloaked = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (SUCCEEDED(hrCloaked) && isCloaked) {
        return TRUE; // 跳过幽灵窗口
    }

    // 获取窗口类名以进行额外过滤
    const int MAX_CLASS_NAME = 256;
    WCHAR className[MAX_CLASS_NAME] = {0};
    int classNameLen = GetClassNameW(hwnd, className, MAX_CLASS_NAME);

    // 过滤某些特殊的系统窗口类
    if (classNameLen > 0) {
        // Windows 输入法相关窗口（如 Microsoft Text Input Application）
        if (wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
            // 对于 CoreWindow，再次确认是否真的可见（通过检查是否有有效的可视化区域）
            RECT clientRect;
            if (!GetClientRect(hwnd, &clientRect)) return TRUE;

            // 如果客户区太小，很可能是输入法等后台窗口
            int clientW = clientRect.right - clientRect.left;
            int clientH = clientRect.bottom - clientRect.top;
            if (clientW < 100 || clientH < 100) return TRUE;
        }

        // 过滤 ApplicationFrameWindow 的空壳窗口
        // UWP 应用在未激活时可能留下空的 ApplicationFrameWindow
        if (wcscmp(className, L"ApplicationFrameWindow") == 0) {
            // 检查窗口是否被最小化或隐藏
            if (IsIconic(hwnd)) return TRUE;

            // 检查是否真的有内容（通过检查窗口透明度或其他属性）
            BYTE opacity = 255;
            DWORD cloakedReason = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloakedReason, sizeof(cloakedReason));
            if (cloakedReason != 0) return TRUE;
        }
    }

    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen == 0) return TRUE;

    std::wstring title(titleLen + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], titleLen + 1);
    title.resize(titleLen);

    if (hwnd == GetDesktopWindow()) return TRUE;

    // 使用 DWM 获取精确边界
    RECT rect = {};
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(hr)) {
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
    }

    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w < 50 || h < 50) return TRUE;

    SCWindowInfo info;
    info.hwnd = hwnd;
    info.rect = rect;
    info.title = title;
    windows->push_back(info);
    return TRUE;
}

// 枚举窗口

std::vector<SCWindowInfo> EnumWindowsForCapture() {
    std::vector<SCWindowInfo> windows;
    EnumWindows(SCEnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// 查找鼠标下方的窗口

int FindWindowAtPoint(const std::vector<SCWindowInfo>& windows, int x, int y) {
    for (size_t i = 0; i < windows.size(); i++) {
        const RECT& r = windows[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return (int)i;
    }
    return -1;
}
