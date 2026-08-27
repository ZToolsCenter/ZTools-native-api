// 截图模块：覆盖层窗口过程与选区状态机
#include "internal.h"
#include "long_capture_internal.h"

// 钳制选区圆角半径到合理范围：[0, min(w,h)/2]。
// 选区尺寸变化（确认/调整/移动）后调用，避免半径越界导致手柄命中与渲染不一致。

void ClampCornerRadius(CaptureContext* ctx) {
    int w = ctx->selection.right - ctx->selection.left;
    int h = ctx->selection.bottom - ctx->selection.top;
    int maxR = (std::min)(w, h) / 2;
    if (maxR < 0) maxR = 0;
    if (ctx->selectionCornerRadius > maxR) ctx->selectionCornerRadius = maxR;
    if (ctx->selectionCornerRadius < 0) ctx->selectionCornerRadius = 0;
}

// 进入确认态：规范化选区并切换状态

void EnterConfirmed(CaptureContext* ctx, const RECT& sel) {
    RECT n = NormalizeRect(sel);
    // 约束到虚拟屏幕内
    if (n.left < ctx->virtualX) { n.right += ctx->virtualX - n.left; n.left = ctx->virtualX; }
    if (n.top < ctx->virtualY) { n.bottom += ctx->virtualY - n.top; n.top = ctx->virtualY; }
    if (n.right > ctx->virtualX + ctx->virtualW) n.right = ctx->virtualX + ctx->virtualW;
    if (n.bottom > ctx->virtualY + ctx->virtualH) n.bottom = ctx->virtualY + ctx->virtualH;
    // 最小尺寸保护
    if (n.right - n.left < SC_MIN_SELECTION) n.right = n.left + SC_MIN_SELECTION;
    if (n.bottom - n.top < SC_MIN_SELECTION) n.bottom = n.top + SC_MIN_SELECTION;
    ctx->selection = n;
    ctx->resizeHandle = RH_None;
    ctx->hoveredCornerHandle = RH_None;  // 进入确认态先置无，由后续 MOUSEMOVE 重新靠近探测
    ClampCornerRadius(ctx);
    if (ctx->activeTool < 0) ctx->activeTool = TB_Drag;
    if (ctx->popupTool < 0) ctx->popupTool = -1;
    ctx->state = CS_Confirmed;
    ctx->needFullRedraw = true;
}

// 约束单轴 resize 的活动端坐标，固定端始终保持按下时的位置。
// rawActive/originalActive 分别是当前候选值和按下时的活动端；screenMin/screenMax 是
// 虚拟屏幕在该轴上的绝对边界。已有标注时优先保证内容不被裁掉；松开时可按活动端
// 当前所在侧补足最小尺寸，整个过程只移动活动端，因此穿越后不会带动固定端漂移。

static int ConstrainResizeActiveCoordinate(int rawActive, int originalActive, int fixed,
                                           int screenMin, int screenMax,
                                           bool hasContent, int contentMin, int contentMax,
                                           bool enforceMinSize) {
    int active = (std::max)(screenMin, (std::min)(rawActive, screenMax));

    if (hasContent) {
        if (fixed <= contentMin) {
            // 内容位于固定端高侧：活动端必须覆盖内容高边，不能穿越后把内容留在选区外。
            active = (std::max)(active, contentMax);
        } else if (fixed >= contentMax) {
            // 内容位于固定端低侧：活动端必须覆盖内容低边。
            active = (std::min)(active, contentMin);
        } else {
            // 固定端落在内容内部时不存在可完整覆盖内容的单侧区间，保留按下时活动端。
            active = originalActive;
        }
        active = (std::max)(screenMin, (std::min)(active, screenMax));
    }

    if (enforceMinSize) {
        // 活动端恰好落在固定端时沿按下时的方向补足，避免释放后方向不确定。
        bool onLowSide = active < fixed || (active == fixed && originalActive < fixed);
        if (onLowSide) {
            active = (std::min)(active, fixed - SC_MIN_SELECTION);
        } else {
            active = (std::max)(active, fixed + SC_MIN_SELECTION);
        }
        // 固定端靠近虚拟屏幕边缘时，目标侧可能不足最小尺寸；此时边界优先且固定端不动。
        active = (std::max)(screenMin, (std::min)(active, screenMax));
    }

    return active;
}

// 从鼠标按下时的选区快照计算本帧 resize 结果（绝对虚拟屏幕坐标）。
// 每个活动轴都只更新对应手柄端，固定边/固定对角点始终取 startSelection；活动端可穿过
// 固定端，最后仅为绘制和导出调用一次 NormalizeRect。contentBounds 在 hasContent=true 时
// 限制活动端以保留已有标注；enforceMinSize 仅用于松开时沿最终方向补足最小尺寸。

RECT ResizeSelectionFromHandle(const RECT& startSelection, int handle, int dx, int dy,
                                      const RECT& virtualBounds,
                                      bool hasContent, const RECT& contentBounds,
                                      bool enforceMinSize) {
    RECT resized = startSelection;

    bool movesLeft = handle == RH_Left || handle == RH_TopLeft || handle == RH_BottomLeft;
    bool movesRight = handle == RH_Right || handle == RH_TopRight || handle == RH_BottomRight;
    bool movesTop = handle == RH_Top || handle == RH_TopLeft || handle == RH_TopRight;
    bool movesBottom = handle == RH_Bottom || handle == RH_BottomLeft || handle == RH_BottomRight;

    if (movesLeft) {
        resized.left = ConstrainResizeActiveCoordinate(
            startSelection.left + dx, startSelection.left, startSelection.right,
            virtualBounds.left, virtualBounds.right,
            hasContent, contentBounds.left, contentBounds.right, enforceMinSize);
    } else if (movesRight) {
        resized.right = ConstrainResizeActiveCoordinate(
            startSelection.right + dx, startSelection.right, startSelection.left,
            virtualBounds.left, virtualBounds.right,
            hasContent, contentBounds.left, contentBounds.right, enforceMinSize);
    }

    if (movesTop) {
        resized.top = ConstrainResizeActiveCoordinate(
            startSelection.top + dy, startSelection.top, startSelection.bottom,
            virtualBounds.top, virtualBounds.bottom,
            hasContent, contentBounds.top, contentBounds.bottom, enforceMinSize);
    } else if (movesBottom) {
        resized.bottom = ConstrainResizeActiveCoordinate(
            startSelection.bottom + dy, startSelection.bottom, startSelection.top,
            virtualBounds.top, virtualBounds.bottom,
            hasContent, contentBounds.top, contentBounds.bottom, enforceMinSize);
    }

    return NormalizeRect(resized);
}

// 取调整手柄在选区上的锚点（绝对虚拟屏幕坐标），作为放大镜焦点。
// 锚点 = 手柄所在边/角的端点：左右手柄取边中点，顶/底取中点，四角取角点。

void GetResizeHandleAnchor(int handle, const RECT& sel, int& ax, int& ay) {
    int cx = (sel.left + sel.right) / 2, cy = (sel.top + sel.bottom) / 2;
    switch (handle) {
        case RH_Left:        ax = sel.left;  ay = cy;        break;
        case RH_Right:       ax = sel.right; ay = cy;        break;
        case RH_Top:         ax = cx;        ay = sel.top;    break;
        case RH_Bottom:      ax = cx;        ay = sel.bottom; break;
        case RH_TopLeft:     ax = sel.left;  ay = sel.top;    break;
        case RH_TopRight:    ax = sel.right; ay = sel.top;    break;
        case RH_BottomLeft:  ax = sel.left;  ay = sel.bottom; break;
        case RH_BottomRight: ax = sel.right; ay = sel.bottom; break;
        default:             ax = cx;        ay = cy;        break;
    }
}

// 从按下快照 + (鼠标位移 + 键盘微调) 实时重算选区，并刷新放大镜焦点像素色。
// MOUSEMOVE 与 KEYDOWN 共用，保证两者一致。拖拽过程不强制最小尺寸（释放时再补）。

void ApplyResizeSelection(HWND hwnd, CaptureContext* ctx) {
    const RECT& start = ctx->dragStartSelection;
    int dx = (ctx->mouseX - ctx->dragStartX) + ctx->kbDX;
    int dy = (ctx->mouseY - ctx->dragStartY) + ctx->kbDY;
    RECT vb = { ctx->virtualX, ctx->virtualY,
                ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH };
    RECT cb = {0, 0, 0, 0};
    bool hasContent = CalcAnnotationsBounds(ctx->annotations, cb, ctx->backDC);
    ctx->selection = ResizeSelectionFromHandle(start, ctx->resizeHandle, dx, dy,
                                               vb, hasContent, cb, false);
    // 放大镜焦点取活动手柄锚点（随活动边移动，键盘微调时鼠标不动也能跟随）
    int ax, ay;
    GetResizeHandleAnchor(ctx->resizeHandle, ctx->selection, ax, ay);
    ctx->currentColor = GetPixelColorFromBitmap(ctx->memDC, ax, ay,
        ctx->virtualX, ctx->virtualY, ctx->dpiScale);
    InvalidateRect(hwnd, NULL, FALSE);
}

// 方向键微调选区：CS_Resizing 微调活动边（累加 kbDX/kbDY）；CS_Confirmed 整体平移 1px。
// Shift 加速到 10px。返回是否已处理（已处理则调用方 return 0）。

bool HandleSelectionNudgeKey(HWND hwnd, CaptureContext* ctx, WPARAM vk) {
    int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
    int ddx = 0, ddy = 0;
    switch (vk) {
        case VK_LEFT:  ddx = -step; break;
        case VK_RIGHT: ddx =  step; break;
        case VK_UP:    ddy = -step; break;
        case VK_DOWN:  ddy =  step; break;
        default: return false;
    }
    if (ctx->state == CS_Resizing && !IsCornerRadiusHandle(ctx->resizeHandle)) {
        ctx->kbDX += ddx;
        ctx->kbDY += ddy;
        ApplyResizeSelection(hwnd, ctx);
        return true;
    }
    if (ctx->state == CS_Confirmed && !ctx->popupOpen) {
        RECT& s = ctx->selection;
        int w = s.right - s.left, h = s.bottom - s.top;
        int nl = s.left + ddx, nt = s.top + ddy;
        if (nl < ctx->virtualX) nl = ctx->virtualX;
        if (nt < ctx->virtualY) nt = ctx->virtualY;
        if (nl + w > ctx->virtualX + ctx->virtualW) nl = ctx->virtualX + ctx->virtualW - w;
        if (nt + h > ctx->virtualY + ctx->virtualH) nt = ctx->virtualY + ctx->virtualH - h;
        s.left = nl; s.right = nl + w; s.top = nt; s.bottom = nt + h;
        ctx->needFullRedraw = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return true;
    }
    return false;
}

// 截图覆盖层窗口过程（双缓冲渲染）

LRESULT CALLBACK ScreenshotOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CaptureContext* ctx = g_captureCtx;
    if (!ctx) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LONGCAPTURE_RUN: {
        // BeginLongCapture 已切到 CS_LongCapturing（黑色遮罩 + 选区框 + 底部工具栏），
        // 此处执行手动滚动捕获主循环。RunLongCapture 内部自行泵消息（面板可响应），
        // 结束后统一清理会话（销毁小地图面板、释放上下文、结束截图会话）。
        LongCaptureContext* lc = g_longCtx.load();
        if (lc) {
            RunLongCapture(lc);
            // 清理：释放采样 DIB 段（dibDC/dibBmp 跨界所有权收进 DestroyLongCaptureContext，
            // CR-021）、销毁灰蒙版/小地图面板、释放上下文、结束截图会话
            DestroyLongCaptureContext(lc);
            if (g_longMaskWindow) {
                DestroyWindow(g_longMaskWindow);
                g_longMaskWindow = NULL;
            }
            if (g_longToolbarWindow) {
                DestroyWindow(g_longToolbarWindow);
                g_longToolbarWindow = NULL;
            }
            if (g_longControlWindow) {
                DestroyWindow(g_longControlWindow);
                g_longControlWindow = NULL;
            }
            {
                // 释放指针与销毁对象必须在同一临界区内完成（协议见 internal.h）：
                // JS 线程 LongCaptureAbort 可能刚 load 到 lc 还没写 abortFlag，
                // 若先释放锁再 delete 会复现 use-after-free；锁内只有指针写与
                // 内存释放，不触碰窗口消息 API。
                std::lock_guard<std::mutex> lock(g_longCtxMutex);
                g_longCtx.store(nullptr);
                delete lc;
            }
        }
        ctx->state = CS_Done;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_PAINT: return OnPaint(hwnd, ctx);

    case WM_LBUTTONDOWN: return OnLButtonDown(hwnd, ctx);

    case WM_MOUSEMOVE: return OnMouseMove(hwnd, ctx);

    case WM_LBUTTONUP: return OnLButtonUp(hwnd, ctx);

    case WM_LBUTTONDBLCLK: {
        // 确认态下双击选区内部 -> 确认截图
        if ((ctx->state == CS_Confirmed) && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations, ctx->selectionCornerRadius,
                ctx->mosaicSizeIdx);
            // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放 result 防泄漏。
            EmitScreenshotResult(result->success, result->x, result->y, result->x2, result->y2,
                                 result->width, result->height, result->base64);
            delete result;
            ctx->state = CS_Done;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        ctx->state = CS_Cancelled;
        // 回调失败结果（统一走 EmitScreenshotResult）
        EmitScreenshotResult(false);
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN: return OnKeyDown(hwnd, wParam, ctx);

    case WM_IME_COMPOSITION: return OnImeComposition(hwnd, msg, wParam, lParam, ctx);

    case WM_CHAR: return OnChar(hwnd, wParam, ctx);

    case WM_SETCURSOR: return OnSetCursor(hwnd, msg, wParam, lParam, ctx);

    case WM_DESTROY: {
        g_screenshotOverlayWindow = NULL;
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
