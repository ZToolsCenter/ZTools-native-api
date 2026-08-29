// 截图模块：会话线程、TSFN 桥与 NAPI 入口（公共 API 见 screenshot_windows.h）
#include "internal.h"

// 全局变量 - 区域截图

HWND g_screenshotOverlayWindow = NULL;

std::atomic<bool> g_isCapturing(false);

napi_threadsafe_function g_screenshotTsfn = nullptr;

std::thread g_screenshotThread;

// 自动确认模式：选区完成后直接出图，跳过编辑态（工具栏/标注）。
// 由 startRegionCaptureWithPrimedFrame 的 options.autoConfirm 设置，
// 会话开始时拷贝进 CaptureContext 供窗口过程读取。
std::atomic<bool> g_autoConfirm(false);

PrimedScreenshotFrame g_primedScreenshotFrame;

std::mutex g_primedScreenshotFrameMutex;

// 保护 g_longCtx 指针生命周期（声明与协议见 internal.h）

std::mutex g_longCtxMutex;

// 截图上下文指针（窗口过程使用）

CaptureContext* g_captureCtx = nullptr;

// ==================== SCGdiResources 实现（自 internal.h 下沉）====================
// 原为 internal.h 内联定义，唯一调用方在本文件（会话初始化/失败清理/退出清理），
// 下沉为外部定义以减少头文件内联实现体；签名/语义完全不变。

// 创建基础画刷/画笔/字体 + 预建固定样式 Pen/Brush（会话级缓存）。
// fontPx：小字号像素；crosshairPx：放大镜准星线宽。
void SCGdiResources::Init(int fontPx, int crosshairPx) {
    if (fontPx < 8) fontPx = 8;
    if (crosshairPx < 1) crosshairPx = 1;
    smallFontPx = fontPx;
    crosshairWidth = crosshairPx;
    bgBrush = CreateSolidBrush(RGB(52, 52, 53));
    borderPen = CreatePen(PS_SOLID, 0, RGB(102, 102, 102));
    crosshairPen = CreatePen(PS_SOLID, crosshairWidth, SC_THEME_ACCENT_BLUE);
    selectionPen = CreatePen(PS_SOLID, 1, SC_THEME_ACCENT_BLUE);
    highlightPen = CreatePen(PS_SOLID, 3, SC_THEME_ACCENT_BLUE);
    // 创建字体
    LOGFONTW lf = {};
    lf.lfHeight = -smallFontPx;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"微软雅黑");
    smallFont = CreateFontIndirectW(&lf);
    maskDC = NULL;
    maskBitmap = NULL;
    // P2：预建固定样式 Pen/Brush，会话内复用。
    toolbarSepPen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
    textSelBrush = CreateSolidBrush(RGB(51, 153, 255));
    annHoverPen = CreatePen(PS_DASH, 1, SC_THEME_ACCENT_BLUE);
    annTextSelPen = CreatePen(PS_SOLID, 2, SC_THEME_ACCENT_BLUE);
}

// 创建遮罩缓冲（纯黑位图，配合常量 alpha 实现 40%+ 半透明遮罩）
// 须在 CaptureContext 虚拟屏幕尺寸确定后调用
void SCGdiResources::InitMask(int virtualW, int virtualH) {
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return;
    maskDC = CreateCompatibleDC(screenDC);
    if (maskDC) {
        maskBitmap = CreateCompatibleBitmap(screenDC, virtualW, virtualH);
        if (maskBitmap) {
            SelectObject(maskDC, maskBitmap);
            // 填充纯黑（AlphaBlend 用常量 alpha，源颜色为黑）
            RECT rc = { 0, 0, virtualW, virtualH };
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(maskDC, &rc, black);
            DeleteObject(black);
        } else {
            DeleteDC(maskDC);
            maskDC = NULL;
        }
    }
    ReleaseDC(NULL, screenDC);
}

void SCGdiResources::Cleanup() {
    if (bgBrush) { DeleteObject(bgBrush); bgBrush = NULL; }
    if (borderPen) { DeleteObject(borderPen); borderPen = NULL; }
    if (crosshairPen) { DeleteObject(crosshairPen); crosshairPen = NULL; }
    if (selectionPen) { DeleteObject(selectionPen); selectionPen = NULL; }
    if (highlightPen) { DeleteObject(highlightPen); highlightPen = NULL; }
    if (smallFont) { DeleteObject(smallFont); smallFont = NULL; }
    if (maskBitmap) { DeleteObject(maskBitmap); maskBitmap = NULL; }
    if (maskDC) { DeleteDC(maskDC); maskDC = NULL; }
    // P2：释放缓存的固定样式 Pen/Brush。
    if (toolbarSepPen) { DeleteObject(toolbarSepPen); toolbarSepPen = NULL; }
    if (textSelBrush) { DeleteObject(textSelBrush); textSelBrush = NULL; }
    if (annHoverPen) { DeleteObject(annHoverPen); annHoverPen = NULL; }
    if (annTextSelPen) { DeleteObject(annTextSelPen); annTextSelPen = NULL; }
}

// ==================== GDI+ 会话级资源管理（性能优化） ====================
// 所有 GDI+ 调用均在 ScreenshotCaptureThread 单线程内，故可在会话开始 Startup 一次、
// 结束 Shutdown 一次，避免每个绘制/测量函数反复 Startup/Shutdown（每帧 WM_PAINT 触发 6~10 次）。

// 前置声明：InitGdipResources 在分配失败时复用 Shutdown 清理。
static void ShutdownGdipResources(CaptureContext* ctx);

// 初始化会话级 GDI+ 资源：Startup + 预建可复用的 FontFamily/StringFormat/Font。
// 必须在任何 GDI+ 调用（含 InitMosaicBrushCursors）之前调用。
// 成功返回 true；失败（Startup 失败）返回 false，调用方应中止会话。

bool InitGdipResources(CaptureContext* ctx) {
    ctx->gdipToken = 0;
    ctx->gdipInited = false;
    ctx->gdipFontFamily = nullptr;
    ctx->gdipStrFmt = nullptr;
    for (int i = 0; i < SC_FONT_COUNT; i++) ctx->gdipFonts[i] = nullptr;

    if (Gdiplus::GdiplusStartup(&ctx->gdipToken, &ctx->gdipStartupInput, NULL) != Gdiplus::Ok) {
        return false;
    }
    ctx->gdipInited = true;

    // 注意：GDI+ 类继承自 GdiplusBase，其 operator new 不接受 std::nothrow 参数，
    // 故用普通 new（GDI+ 对象通过 GetLastStatus() 而非空指针报告失败）。
    ctx->gdipFontFamily = new Gdiplus::FontFamily(SC_FONT_FACE);
    ctx->gdipStrFmt = new Gdiplus::StringFormat();
    if (!ctx->gdipFontFamily || !ctx->gdipStrFmt
        || ctx->gdipFontFamily->GetLastStatus() != Gdiplus::Ok) {
        ShutdownGdipResources(ctx); return false;
    }
    ctx->gdipStrFmt->SetAlignment(Gdiplus::StringAlignmentNear);
    ctx->gdipStrFmt->SetLineAlignment(Gdiplus::StringAlignmentNear);

    // 按文字字号预设预建 Font（与 SC_FONT_SIZES 一一对应）
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        ctx->gdipFonts[i] = new Gdiplus::Font(
            ctx->gdipFontFamily, (Gdiplus::REAL)SC_FONT_SIZES[i],
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        if (!ctx->gdipFonts[i] || ctx->gdipFonts[i]->GetLastStatus() != Gdiplus::Ok) {
            ShutdownGdipResources(ctx); return false;
        }
    }
    return true;
}

// 释放会话级 GDI+ 资源：先 delete 所有 GDI+ 对象，再 Shutdown（对象须在 Shutdown 前析构）。
// 安全可重入：重复调用无副作用。

static void ShutdownGdipResources(CaptureContext* ctx) {
    // Font 先于 FontFamily 析构（Font 内部引用 FontFamily）
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        delete ctx->gdipFonts[i];
        ctx->gdipFonts[i] = nullptr;
    }
    delete ctx->gdipStrFmt;
    ctx->gdipStrFmt = nullptr;
    delete ctx->gdipFontFamily;
    ctx->gdipFontFamily = nullptr;

    if (ctx->gdipInited) {
        Gdiplus::GdiplusShutdown(ctx->gdipToken);
        ctx->gdipInited = false;
        ctx->gdipToken = 0;
    }
}

// 取 fontPx 对应的缓存 Font。文字标注字号仅取自 SC_FONT_SIZES，故优先查缓存表；
// 若 fontPx 不在预设表内（理论上不会发生），用 thread_local 临时对象兜底，避免泄漏。
// 返回的 Font 所有权归会话/兜底存储，调用方不得 delete。

Gdiplus::Font* GetGdipFont(CaptureContext* ctx, int fontPx) {
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        if (SC_FONT_SIZES[i] == fontPx && ctx->gdipFonts[i]) {
            return ctx->gdipFonts[i];
        }
    }
    // 兜底：thread_local 临时 Font（仅当前线程有效，覆盖极少见的非预设字号）
    static thread_local Gdiplus::Font* sFallback = nullptr;
    static thread_local int sFallbackPx = -1;
    static thread_local Gdiplus::FontFamily* sFallbackFam = nullptr;
    if (!sFallbackFam) sFallbackFam = new Gdiplus::FontFamily(SC_FONT_FACE);
    if (!sFallback || sFallbackPx != fontPx) {
        delete sFallback;
        sFallbackPx = fontPx;
        sFallback = new Gdiplus::Font(sFallbackFam, (Gdiplus::REAL)fontPx,
                                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
    return sFallback;
}

// 在主线程调用 JS 回调（截图完成）

static void CallScreenshotJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        ScreenshotResult* result = static_cast<ScreenshotResult*>(data);

        napi_value resultObj;
        napi_create_object(env, &resultObj);

        napi_value success;
        napi_get_boolean(env, result->success, &success);
        napi_set_named_property(env, resultObj, "success", success);

        if (result->success) {
            napi_value x, y, x2, y2, width, height, base64;
            napi_create_int32(env, result->x, &x);
            napi_set_named_property(env, resultObj, "x", x);
            napi_create_int32(env, result->y, &y);
            napi_set_named_property(env, resultObj, "y", y);
            napi_create_int32(env, result->x2, &x2);
            napi_set_named_property(env, resultObj, "x2", x2);
            napi_create_int32(env, result->y2, &y2);
            napi_set_named_property(env, resultObj, "y2", y2);
            napi_create_int32(env, result->width, &width);
            napi_set_named_property(env, resultObj, "width", width);
            napi_create_int32(env, result->height, &height);
            napi_set_named_property(env, resultObj, "height", height);
            napi_create_string_utf8(env, result->base64.c_str(), result->base64.size(), &base64);
            napi_set_named_property(env, resultObj, "base64", base64);
        }

        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 1, &resultObj, nullptr);
        delete result;
    }
}

// 统一 ScreenshotResult 发射口：构造结果并经截图会话 TSFN 回传 JS。
// 统一守卫：TSFN 未就绪或 napi_tsfn_nonblocking 因队列满返回非 napi_ok 时，自行
// delete 分配的 result 防泄漏（CallScreenshotJs 只在成功入队时才 delete）。
// 仅允许在截图线程内调用。各发射点（确认/取消/保存/ESC/长截图完成等）统一走此函数。
// success=false 时坐标/尺寸/base64 全置 0/空（与取消路径语义一致）。
void EmitScreenshotResult(bool success, int x, int y, int x2, int y2,
                          int width, int height, const std::string& base64) {
    ScreenshotResult* result = new ScreenshotResult();
    result->success = success;
    result->x = x; result->y = y;
    result->x2 = x2; result->y2 = y2;
    result->width = width; result->height = height;
    result->base64 = base64;
    if (g_screenshotTsfn == nullptr) {
        delete result;
        return;
    }
    if (napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking) != napi_ok) {
        // nonblocking 入队失败（队列满等）：回调不会取走所有权，自行释放防泄漏。
        delete result;
    }
}

// 会话初始化失败快速回传：构造 {success:false} 结果并经 EmitScreenshotResult 回传 JS，
// 唤醒 await 方避免永久挂起（早退路径统一收口点）。
void FailFast() {
    EmitScreenshotResult(false);
}

// 会话出口统一收口：释放截图会话的 threadsafe function 并置空。
// 前提（本文件与 wndproc/overlay_input/long_capture 各 Emit 点共同保证）：所有
// napi_call_threadsafe_function 调用端都运行在截图线程内，且都在会话结束前完成；
// 故整个进程内唯一的释放点就是本函数，各出口汇聚到此处释放一次即可把
// initial_thread_count 从 1 归 0，不再阻碍 Node 进程优雅退出。置空防止释放后误用。
static void ReleaseScreenshotTsfn() {
    if (g_screenshotTsfn != nullptr) {
        napi_release_threadsafe_function(g_screenshotTsfn, napi_tsfn_release);
        g_screenshotTsfn = nullptr;
    }
}

// 截图线程（预截屏 + 双缓冲架构）

void ScreenshotCaptureThread() {
    // 设置 DPI 感知
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setDpiProc = (SetThreadDpiAwarenessContextProc)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (setDpiProc) {
            setDpiProc(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    double uiScale = GetDpiScaleFactor();
    double dpiScale = uiScale;

    // 预截屏整个虚拟屏幕
    HDC memDC = NULL;
    HBITMAP screenBitmap = NULL;
    int vx, vy, vw, vh;
    if (!AcquireScreenshotBase(memDC, screenBitmap, vx, vy, vw, vh, dpiScale)) {
        FailFast();
        ReleaseScreenshotTsfn();
        g_isCapturing = false;
        return;
    }

    // 创建双缓冲
    HDC backDC = NULL;
    HBITMAP backBmp = NULL;
    if (!CreateBackBuffer(backDC, backBmp, vw, vh)) {
        DeleteDC(memDC);
        DeleteObject(screenBitmap);
        FailFast();
        ReleaseScreenshotTsfn();
        g_isCapturing = false;
        return;
    }

    // 枚举窗口
    std::vector<SCWindowInfo> windows = EnumWindowsForCapture();

    // 初始化 GDI 资源
    SCPanelMetrics panelMetrics = CalcPanelMetrics(uiScale);
    SCGdiResources gdi;
    gdi.Init(panelMetrics.fontPx, panelMetrics.crosshair);
    // 创建选区外遮罩缓冲（需虚拟屏幕尺寸）
    gdi.InitMask(vw, vh);

    // 初始化上下文
    CaptureContext ctx = {};
    ctx.state = CS_Idle;
    ctx.autoConfirm = g_autoConfirm.load();
    ctx.virtualX = vx; ctx.virtualY = vy;
    ctx.virtualW = vw; ctx.virtualH = vh;
    ctx.startX = 0; ctx.startY = 0;
    ctx.endX = 0; ctx.endY = 0;
    ctx.hoveredWindow = -1;
    ctx.screenBitmap = screenBitmap;
    ctx.memDC = memDC;
    ctx.backDC = backDC;
    ctx.backBitmap = backBmp;
    ctx.lastPanelRect = {0,0,0,0};
    ctx.lastSelectionRect = {0,0,0,0};
    ctx.lastLabelRect = {0,0,0,0};
    ctx.lastHighlightRect = {0,0,0,0};
    ctx.lastToolbarRect = {0,0,0,0};
    ctx.lastPopupRect = {0,0,0,0};
    ctx.lastCaretRect = {0,0,0,0};
    ctx.hasLastCaret = false;
    ctx.lastAnnotationBox = {0,0,0,0};
    ctx.hasLastAnnotationBox = false;
    ctx.lastDrawingBox = {0,0,0,0};
    ctx.hasLastDrawingBox = false;
    ctx.selection = {0,0,0,0};
    ctx.resizeHandle = RH_None;
    ctx.dragStartX = 0; ctx.dragStartY = 0;
    ctx.dragStartSelection = {0,0,0,0};
    ctx.toolbarRect = {0,0,0,0};
    ctx.hoverToolbarBtn = -1;
    ctx.activeTool = -1;
    ctx.popupTool = -1;
    ctx.needFullRedraw = true;
    ctx.dpiScale = dpiScale;
    ctx.gdi = gdi;
    ctx.panelMetrics = panelMetrics;
    ctx.windows = std::move(windows);

    // 工具栏几何（按 DPI 缩放）+ 图标位图缓存（按 DPI 预渲染）
    ctx.toolbarMetrics = CalcToolbarMetrics(uiScale);
    ctx.handleMetrics = CalcHandleMetrics(uiScale);
    ctx.iconCache.Init(ctx.toolbarMetrics.iconSize);
    // GDI+ 会话级初始化（必须在 InitMosaicBrushCursors 及任何 GDI+ 调用之前）：
    // 会话内单次 Startup，避免每帧反复初始化导致拖拽卡顿。
    if (!InitGdipResources(&ctx)) {
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        FailFast();
        ReleaseScreenshotTsfn();
        g_isCapturing = false;
        return;
    }
    // 涂抹光标缓存（按半径预生成，DPI 缩放半径）
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) ctx.mosaicBrushCursors[i] = NULL;
    ctx.mosaicBrushCursorsInited = false;
    InitMosaicBrushCursors(&ctx);
    // 子菜单几何（按 DPI 缩放）+ 标注绘制默认值
    ctx.popupMetrics = CalcPopupMetrics(uiScale);
    ctx.popupOpen = false;
    ctx.popupRect = {0,0,0,0};
    ctx.drawColorIdx = SC_DEFAULT_COLOR_IDX;
    ctx.drawThickIdx = SC_DEFAULT_THICK_IDX;
    ctx.fontSizeIdx = SC_DEFAULT_FONT_IDX;
    ctx.mosaicSizeIdx = SC_DEFAULT_MOSAIC_IDX;
    ctx.mosaicRadiusIdx = SC_DEFAULT_MOSAIC_RADIUS_IDX;
    ctx.mosaicRectMode = false;  // 默认涂抹模式
    ctx.mosaicBaseDC = NULL;
    ctx.mosaicBaseBitmap = NULL;
    ctx.mosaicBaseW = 0;
    ctx.mosaicBaseH = 0;
    ctx.mosaicBaseBlockPx = 0;
    ctx.mosaicDrawLastIdx = 0;
    ctx.hasCurDrawing = false;
    // 文字编辑初始化
    ctx.textBuf.clear();
    ctx.textAnchorX = 0;
    ctx.textAnchorY = 0;
    ctx.textCaretPos = 0;
    ctx.textCaretVisible = true;
    ctx.textCaretLastBlink = GetTickCount();
    ctx.textSelStart = -1;
    ctx.textSelEnd = -1;
    ctx.textDraggingSelection = false;
    ctx.hoveredTextAnnotation = -1;
    ctx.selectedTextAnnotation = -1;
    ctx.draggingTextAnnotation = -1;
    ctx.textDragStartX = 0;
    ctx.textDragStartY = 0;
    // 非文字标注选中/拖拽/缩放状态初始化（与文字机制互斥）
    ctx.hoveredAnnotation = -1;
    ctx.selectedAnnotation = -1;
    ctx.draggingAnnotation = -1;
    ctx.resizingAnnotation = -1;
    ctx.annotationResizeHandle = RH_None;
    ctx.annotationDragStartX = 0;
    ctx.annotationDragStartY = 0;
    ctx.annotationOpHistoryPushed = false;
    ctx.dragStartAnnotation = {};
    ctx.annotationResizeStartBox = { 0, 0, 0, 0 };

    // 获取初始鼠标位置和颜色
    POINT pt;
    GetCursorPos(&pt);
    ctx.mouseX = pt.x;
    ctx.mouseY = pt.y;
    ctx.currentColor = GetPixelColorFromBitmap(memDC, pt.x, pt.y, vx, vy, dpiScale);

    g_captureCtx = &ctx;

    // 注册窗口类
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    // 必须含 CS_DBLCLKS,否则系统不合成 WM_LBUTTONDBLCLK,
    // wndproc 中「确认态双击选区 → 确认截图」分支永远不会触发。
    wc.style |= CS_DBLCLKS;
    wc.lpfnWndProc = ScreenshotOverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);  // 默认鼠标样式
    wc.lpszClassName = L"ZToolsScreenshotOverlay";

    if (!RegisterClassExW(&wc)) {
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        FailFast();
        ReleaseScreenshotTsfn();
        g_isCapturing = false;
        return;
    }

    // 创建普通 WS_POPUP 窗口（非分层窗口）
    g_screenshotOverlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ZToolsScreenshotOverlay",
        L"Screenshot Overlay",
        WS_POPUP,
        vx, vy, vw, vh,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (g_screenshotOverlayWindow == NULL) {
        UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        FailFast();
        ReleaseScreenshotTsfn();
        g_isCapturing = false;
        return;
    }

    ShowWindow(g_screenshotOverlayWindow, SW_SHOW);
    SetForegroundWindow(g_screenshotOverlayWindow);

    // 消息循环
    MSG msg;
    while (true) {
        if (ctx.state == CS_Done || ctx.state == CS_Cancelled) break;

        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // 工具栏 title 式 tooltip 停顿轮询（悬停 ~0.5s 出气泡；内部按状态门控）
            TickToolbarTooltip(&ctx, g_screenshotOverlayWindow);

            // 文字编辑态：光标闪烁（每 500ms 切换）
            if (ctx.state == CS_TextEditing) {
                DWORD now = GetTickCount();
                if (now - ctx.textCaretLastBlink >= 500) {
                    ctx.textCaretVisible = !ctx.textCaretVisible;
                    ctx.textCaretLastBlink = now;
                    // 仅刷新光标区域（光标位置不变，只切换可见性）。首次无缓存时全屏。
                    if (ctx.hasLastCaret) {
                        RECT r = InflateRectBy(ctx.lastCaretRect, 2);
                        InvalidateRect(g_screenshotOverlayWindow, &r, FALSE);
                    } else {
                        InvalidateRect(g_screenshotOverlayWindow, NULL, FALSE);
                    }
                }
            }

            // 检查 ESC 键（窗口可能没有焦点）
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                if (ctx.state != CS_Done && ctx.state != CS_Cancelled) {
                    ctx.state = CS_Cancelled;
                    // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放防泄漏。
                    EmitScreenshotResult(false);
                    DestroyWindow(g_screenshotOverlayWindow);
                    break;
                }
            }
            Sleep(1);
        }
    }

    // 清理
    g_captureCtx = nullptr;
    gdi.Cleanup();
    ctx.iconCache.Cleanup();
    FreeMosaicBase(&ctx);
    FreeMosaicBrushCursors(&ctx);
    DeleteDC(backDC); DeleteObject(backBmp);
    DeleteDC(memDC); DeleteObject(screenBitmap);
    // GDI+ 会话级资源最后释放（所有 GDI+ 调用均已结束后才可 Shutdown）
    ShutdownGdipResources(&ctx);
    UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));

    // 会话出口统一收口：正常出口均已在确认/取消分支各回调一次
    // （ESC 取消、右键取消、确认/保存完成、长截图 RunLongCapture 各自的结果发射）。
    // 这里兜底：若循环未经任何确认/取消分支退出（如外部投递 WM_QUIT），补发一次
    // 失败结果，保证 JS 端 await 必然被唤醒；随后释放 TSFN，防止每次 start() 的
    // 线程计数滞留阻碍 Node 进程优雅退出。g_isCapturing 置 false 必须最后执行，
    // 使下一次 start() 观察到 false 时旧 TSFN 已必然释放完毕。
    if (ctx.state != CS_Done && ctx.state != CS_Cancelled) {
        FailFast();
    }
    ReleaseScreenshotTsfn();
    g_isCapturing = false;
}

// 启动区域截图

Napi::Value StartRegionCapture(const Napi::CallbackInfo& info) {
    return StartRegionCaptureWithPrimedFrame(info);
}

// 供 JS 主动触发首帧预抓取。

Napi::Value PrimeScreenshotFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const bool success = PrimeScreenshotFrameNow();
    return Napi::Boolean::New(env, success);
}

Napi::Value StartRegionCaptureWithPrimedFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_isCapturing) {
        Napi::Error::New(env, "Screenshot already in progress").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 解析可选参数（顺序无关，便于后续扩展）：
    //   - 回调函数：截图完成后回调
    //   - 选项对象：目前支持 { autoConfirm: boolean, longCapture: {...} }
    //              默认 autoConfirm=true，选区确定后直接出图，跳过编辑态；
    //              传 false 才进入编辑态（工具栏/标注）
    //              longCapture 为编辑态工具栏「长截图」按钮的手动滚动捕获参数：
    //              { interval?: number }
    // 每次会话显式解析长截图参数到默认值后再按 JS 覆盖，不再依赖跨会话残留的粘滞全局
    // （旧实现用文件级全局 g_lcInterval 承载，第二次 start 不传 longCapture
    //  时会沿用上一次的值，属跨会话状态泄漏）。
    bool autoConfirm = true;
    int lcInterval = 250;    // 默认滚轮防抖间隔 ms
    for (int i = 0; i < (int)info.Length(); i++) {
        if (info[i].IsFunction()) {
            Napi::Function callback = info[i].As<Napi::Function>();
            napi_value resource_name;
            napi_create_string_utf8(env, "ScreenshotCallback", NAPI_AUTO_LENGTH, &resource_name);

            napi_status status = napi_create_threadsafe_function(
                env, callback, nullptr, resource_name,
                0, 1, nullptr, nullptr, nullptr,
                CallScreenshotJs, &g_screenshotTsfn
            );

            if (status != napi_ok) {
                Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
                return env.Undefined();
            }
        } else if (info[i].IsObject()) {
            Napi::Object opts = info[i].As<Napi::Object>();
            if (opts.Has("autoConfirm")) {
                Napi::Value v = opts.Get("autoConfirm");
                if (v.IsBoolean()) {
                    autoConfirm = v.As<Napi::Boolean>().Value();
                }
            }
            if (opts.Has("longCapture")) {
                Napi::Value v = opts.Get("longCapture");
                if (v.IsObject()) {
                    Napi::Object lc = v.As<Napi::Object>();
                    if (lc.Has("interval")) {
                        Napi::Value t = lc.Get("interval");
                        if (t.IsNumber()) {
                            int iv = t.As<Napi::Number>().Int32Value();
                            if (iv >= 50 && iv <= 2000) lcInterval = iv;
                        }
                    }
                }
            }
        }
    }
    g_autoConfirm = autoConfirm;
    // 每次会话重置为本次解析值（默认值或 JS 覆盖），消除跨会话粘滞
    g_lcInterval = lcInterval;

    g_isCapturing = true;

    g_screenshotThread = std::thread(ScreenshotCaptureThread);
    g_screenshotThread.detach();

    return env.Undefined();
}

// 中止进行中的长截图滚动捕获（注册为 "abortLongCapture" 导出）。
// 仅设置中止标记：由滚动循环在下一帧检查点退出，并按失败结果回调 JS。

Napi::Value AbortLongCapture(const Napi::CallbackInfo& info) {
    LongCaptureAbort();
    return info.Env().Undefined();
}
