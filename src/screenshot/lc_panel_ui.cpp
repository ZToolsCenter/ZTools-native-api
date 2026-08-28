// 长截图子系统：侧边小地图面板 + 全屏灰蒙版 UI。
// CR-021 拆分自 long_capture_windows.cpp 的「预览面板 / 全屏蒙版」段。
// 面板分层渲染（UpdateLayeredWindow 原子提交）+ 滚轮观察目标窗口 + 蒙版取景窗。
// EnsureArgbSurface/FreeArgbSurface 是面板/工具栏/tooltip 共用的 32bpp 预乘 ARGB
// 后备 DIB 辅助，在此定义（long_capture_internal.h 声明）。
#include "internal.h"
#include "long_capture_internal.h"

// 蒙版样式（预乘 ARGB）：整屏半透明灰，采样裁剪区整透明透出实况桌面
const int LONG_MASK_GRAY = 44;

const BYTE LONG_MASK_ALPHA = 0xA0;

// 预览面板布局常量（逻辑像素，绘制时按 dpiScale 缩放）。
// 面板只保留小地图本体（帧提示与完成/取消按钮已移至选区底部的长截图工具栏窗口）。

const int LC_PANEL_W = 232;   // 面板总宽

const int LC_PANEL_PAD = 8;   // 内边距

static const int LC_PANEL_MIN_H = 48; // 预览最小高度（逻辑像素，防止无内容时塌缩成线）

#ifndef RI_MOUSE_HWHEEL
#define RI_MOUSE_HWHEEL 0x0800
#endif

// 预览图目标矩形（客户区坐标）：拼接结果等比缩放，水平居中、垂直居中于面板。
// 尺寸取「显示空间」：纵向 = physW×行数；横向 = 行数×physW（回转后宽高互换），
// 行数含裁剪窗口（裁掉的部分不进预览）。

static RECT LongCapturePanelPreviewRect(LongCaptureContext* c, int cw, int ch) {
    RECT r = {0, 0, 0, 0};
    if (!c || c->stitchH <= 0 || c->physW <= 0) return r;
    double ds = c->dpiScale;
    int pad = (int)(LC_PANEL_PAD * ds + 0.5);
    int availW = cw - pad * 2;
    int availH = ch - pad * 2;
    if (availW < 1 || availH < 1) return r;
    int rowStart = 0, rowEnd = 0;
    LongCaptureOutputRows(c, rowStart, rowEnd);
    int rows = rowEnd - rowStart;
    if (rows <= 0) return r;
    // 显示空间（物理像素）宽高：固定轴 = physW（= cropRect 的物理对应，纵向=宽/横向=高），
    // 滚动轴 = rows。输出为面板物理矩形，故用物理量；逻辑标签版见 LongCaptureOutputSizeLabel，
    // 高度生长版见 LongCapturePanelUpdate，三者同一换算口径（CR-023）。
    double dispW = c->horizontal ? (double)rows : (double)c->physW;
    double dispH = c->horizontal ? (double)c->physW : (double)rows;
    double scale = (std::min)((double)availW / dispW, (double)availH / dispH);
    int pw = (int)(dispW * scale + 0.5);
    int ph = (int)(dispH * scale + 0.5);
    r.left = (cw - pw) / 2;
    r.top = pad + (availH - ph) / 2;
    r.right = r.left + pw;
    r.bottom = r.top + ph;
    return r;
}

// 内容坐标 → 预览像素的视口框（小地图预览坐标，显示空间）：contentTop + headRows =
// 拼接图内位置，等比缩放并钳制进裁剪后的预览范围。committed（实线已确认框）与
// tentative（虚线预计框）共用本换算；横向模式的「行位置」映射为水平位置。
// 移动轴贴合预览区两侧换算；固定轴（纵向=左右、横向=上下）向外扩 1px——预览图外侧
// 一圈正是整体描边所在（见 WM_PAINT），扩出的框线恰落于该环上，重合处整像素压住
// 描边而非与之混叠透白。移动轴同理：带被钳制到输出窗口头端/尾端时该侧边界再外扩
// 1px 吞掉相邻环行，蓝框在极值处完整封边、不残留灰白线；中间位置保留环行（语义 =
// 当前视图之外还有已捕获内容）。
static RECT LongCaptureViewportRectAt(LongCaptureContext* c, const RECT& preview, int64_t contentTop) {
    RECT v = { 0, 0, 0, 0 };
    if (!c || c->stitchH <= 0 || c->physW <= 0
        || preview.right <= preview.left || preview.bottom <= preview.top) return v;
    bool horiz = !!c->horizontal;
    int rowStart = 0, rowEnd = 0;
    LongCaptureOutputRows(c, rowStart, rowEnd);
    int rows = rowEnd - rowStart;
    if (rows <= 0) return v;
    if (rows <= c->physH) {   // 未拼接滚动：整体即当前区域（四边均贴端外扩）
        v = preview;
        v.left--; v.right++; v.top--; v.bottom++;
        return v;
    }
    int64_t topStitch = contentTop + c->headRows;   // 内容坐标 → 拼接坐标
    if (topStitch < rowStart) topStitch = rowStart;
    int64_t tailLimit = (int64_t)rowEnd - c->physH;
    if (tailLimit < rowStart) tailLimit = rowStart;
    if (topStitch > tailLimit) topStitch = tailLimit;
    bool atHead = topStitch <= rowStart;   // 带抵输出窗口头端
    bool atTail = topStitch >= tailLimit;  // 带抵尾端
    if (!horiz) {
        double scale = (double)(preview.bottom - preview.top) / rows;
        int vhPx = (int)(c->physH * scale + 0.5);
        if (vhPx < 1) vhPx = 1;
        int topPx = preview.top + (int)((topStitch - rowStart) * scale + 0.5);
        v.left = preview.left - 1;
        v.right = preview.right + 1;
        v.top = topPx - (atHead ? 1 : 0);
        v.bottom = topPx + vhPx + (atTail ? 1 : 0);
    } else {
        double scale = (double)(preview.right - preview.left) / rows;
        int vwPx = (int)(c->physH * scale + 0.5);
        if (vwPx < 1) vwPx = 1;
        int leftPx = preview.left + (int)((topStitch - rowStart) * scale + 0.5);
        v.left = leftPx - (atHead ? 1 : 0);
        v.right = leftPx + vwPx + (atTail ? 1 : 0);
        v.top = preview.top - 1;
        v.bottom = preview.bottom + 1;
    }
    return v;
}

// 已确认（committed）视口框：最新一次提交帧的精确位置（替代旧的 lastDir 顶/底猜测；
// 纯向下滚 = 底部、纯向上滚 = 顶部，混合滚动方向时为真实中间位置）。

static RECT LongCaptureViewportRect(LongCaptureContext* c, const RECT& preview) {
    return LongCaptureViewportRectAt(c, preview, c ? c->committedContentTop : 0);
}


// 浮点坐标圆角矩形路径（GDI+ 抗锯齿路径基础）：int 版本见 overlay_ui_windows.cpp 的
// AddRoundedRect。描边需半像素级内缩对齐才能保证 1px 笔画完整落在表面像素带内，
// 整数坐标版本无法表达，故专设本重载。radius 自动钳制不超过短边一半。

static void AddRoundedRectF(Gdiplus::GraphicsPath& outPath, Gdiplus::REAL x, Gdiplus::REAL y,
                            Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL radius) {
    Gdiplus::REAL d = radius * 2;
    if (d > w) { radius = w / 2; d = w; }
    if (d > h) { radius = h / 2; d = h; }
    Gdiplus::RectF rect(x, y, w, h);
    if (radius < 0.5f) { outPath.AddRectangle(rect); return; }
    outPath.AddArc(rect.X, rect.Y, d, d, 180, 90);
    outPath.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    outPath.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    outPath.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    outPath.CloseFigure();
}

// 面板后备表面（与窗口等大的预乘 ARGB DIB，随窗口销毁释放）

static HDC s_lcPnSurfDC = NULL;
static HBITMAP s_lcPnSurfBmp = NULL;
static void* s_lcPnSurfBits = nullptr;
static int s_lcPnSurfW = 0, s_lcPnSurfH = 0;

// 整幅渲染小地图面板并经 UpdateLayeredWindow 原子提交：
//   清透明 → 深色圆角底 → 内缩整像素描边 → 缩略小地图（零拷贝包装增量缩略缓冲，
//   GDI+ 高质量插值）→ 视口框三层标注（半透明衬底 / 实线蓝框 / 虚线橙框）。
// 分层渲染一次性解决三个历史问题：
//   1) 滚动闪动——旧实现往屏幕 DC 分四层直绘且窗口生长时「先伸缩后绘制」，每帧中间态
//      直达合成器；现全部绘制进后备表面，ULW 单次提交，屏幕只见完整帧；
//   2) 右/底边框截断——旧实现把圆角矩形贴着窗口边界画，居中描边的外半宽越出客户区被
//      裁掉；现描边路径整体内缩半像素，1px 笔画完整落在 [0,cw-1]/[0,ch-1] 像素带内；
//   3) 圆角外黑色直角残留——旧实现为不透明方形窗口且只填圆角路径内部，方角区域从未
//      绘制（表现为黑）；现为逐像素 alpha，圆角外 alpha=0 真透明。
// panel 由调用方传入而非读 g_longControlWindow：创建时机上首帧渲染发生在全局句柄赋值前。

void LongCapturePanelRender(HWND panel, LongCaptureContext* c) {
    if (!panel || !c) return;
    RECT cr;
    if (!GetClientRect(panel, &cr)) return;
    RECT wr;
    GetWindowRect(panel, &wr);   // ULW 目标几何用窗口原点（客户区=整个 WS_POPUP）
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw < 1 || ch < 1) return;
    if (!EnsureArgbSurface(s_lcPnSurfDC, s_lcPnSurfBmp, s_lcPnSurfBits,
                           s_lcPnSurfW, s_lcPnSurfH, cw, ch))
        return;
    double ds = c->dpiScale;
    float radius = (float)(8 * ds + 0.5);
    {
        Gdiplus::Bitmap surf(cw, ch, cw * 4, PixelFormat32bppPARGB, (BYTE*)s_lcPnSurfBits);
        Gdiplus::Graphics g(&surf);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
        g.FillRectangle(&clear, 0, 0, cw, ch);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        // 圆角深色背景（半径按 DPI 缩放；旧实现固定物理 8px 与其余 UI 不同步）
        {
            Gdiplus::GraphicsPath path;
            AddRoundedRectF(path, 0, 0, (Gdiplus::REAL)cw, (Gdiplus::REAL)ch, radius);
            Gdiplus::SolidBrush bg(Gdiplus::Color(255, 52, 52, 53));
            g.FillPath(&bg, &path);
            // 边框描边：路径内缩半像素（0.5,.5)-(cw-.5,ch-.5)，居中 1px 笔画恰占满
            // 边界整像素环；半径同步内缩保持同心。
            Gdiplus::GraphicsPath border;
            AddRoundedRectF(border, 0.5f, 0.5f, (Gdiplus::REAL)(cw - 1), (Gdiplus::REAL)(ch - 1),
                            (std::max)(radius - 0.5f, 1.0f));
            Gdiplus::Pen pen(Gdiplus::Color(255, 102, 102, 102), 1.0f);
            g.DrawPath(&pen, &border);
        }
        // 缩略小地图：先按固定列宽增量缩列的缩略缓冲，这里只重采样行（避免逐帧重读拼接大缓冲）
        LongCaptureRebuildThumb(c);
        if (c->horizontal) LongCaptureRebuildThumbDisplay(c);
        RECT preview = LongCapturePanelPreviewRect(c, cw, ch);
        if (c->thumbW > 0 && c->thumbH > 0 && preview.right > preview.left
            && preview.bottom > preview.top) {
            // 裁剪行窗口 → 源矩形：纵向映射 srcY/srcH（行 = 纵向轴），
            // 横向用回转缓冲映射 srcX/srcW（行 = 回转后的水平轴）
            int rowStart = 0, rowEnd = 0;
            LongCaptureOutputRows(c, rowStart, rowEnd);
            int rows = rowEnd - rowStart;
            const uint32_t* bits = c->thumbMerged.data();
            int bw = c->thumbW, bh = c->thumbH;
            int srcX = 0, srcY = rowStart, srcW = bw, srcH = rows;
            if (c->horizontal && !c->thumbDisplay.empty()) {
                bits = c->thumbDisplay.data();
                bw = c->thumbDisplayW; bh = c->thumbDisplayH;
                srcX = rowStart; srcY = 0; srcW = rows; srcH = bh;
            }
            if (bw > 0 && bh > 0 && srcW > 0 && srcH > 0 && srcW <= bw && srcH <= bh) {
                // 零拷贝包装缩略缓冲（PixelFormat32bppRGB 视作不透明 RGBX：等宽快路径
                // memcpy 自抓屏 DIB 的 alpha 字节不可信）。先裁剪到预览矩形再 DrawImage，
                // 防高质量插值向外渗出压到描边上。
                Gdiplus::Bitmap thumb(bw, bh, bw * 4, PixelFormat32bppRGB, (BYTE*)bits);
                Gdiplus::Rect dstR(preview.left, preview.top,
                                   preview.right - preview.left, preview.bottom - preview.top);
                g.SetClip(dstR);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                g.DrawImage(&thumb, dstR, srcX, srcY, srcW, srcH, Gdiplus::UnitPixel, NULL);
                g.ResetClip();
                g.SetInterpolationMode(Gdiplus::InterpolationModeDefault);
            }
        }
        // 整体框：预览图外侧一圈 1px 灰描边（占外围相邻一像素环）= 已捕获的完整区域标记
        // 已确认区域框（实线蓝）：最新一次提交帧的精确位置，只在 Commit 后移动；固定轴
        // 恰落在整体框那圈描边上（见 LongCaptureViewportRectAt），整像素覆盖其上
        // 预计区域框（虚线橙）：本帧未提交、但已确认滚动方向/粗略位移时的 tentative 位置
        // 描边一律整数像素 FillRectangle：整像素、确定性栅格化，语义与旧版一致
        if (preview.right > preview.left && preview.bottom > preview.top) {
            auto hbar = [&](int y, int x0, int x1, const Gdiplus::Color& color) {
                Gdiplus::SolidBrush br(color);
                g.FillRectangle(&br, x0, y, x1 - x0, 1);
            };
            auto vbar = [&](int x, int y0, int y1, const Gdiplus::Color& color) {
                Gdiplus::SolidBrush br(color);
                g.FillRectangle(&br, x, y0, 1, y1 - y0);
            };
            const Gdiplus::Color kRingC(255, 190, 190, 195);    // 整体框
            const Gdiplus::Color kBlueC(255, 0x2F, 0x7E, 0xE5); // 已确认视口框
            const Gdiplus::Color kOrngC(255, 0xE8, 0xA3, 0x3C); // 预计框虚线
            RECT vp = LongCaptureViewportRect(c, preview);
            bool vpValid = vp.right > vp.left && vp.bottom > vp.top;
            RECT tp = {};
            bool tpShow = false;
            if (c->tentativeValid) {
                int64_t tdiff = c->tentativeContentTop - c->committedContentTop;
                if (tdiff < 0) tdiff = -tdiff;
                if (tdiff >= LC_TRACK_MIN_STEP) {
                    tp = LongCaptureViewportRectAt(c, preview, c->tentativeContentTop);
                    tpShow = tp.right > tp.left && tp.bottom > tp.top;
                }
            }
            // 半透明内衬只铺描边内侧、向内收 1px；先画衬底再叠不透明描边
            if (vpValid) {
                Gdiplus::SolidBrush fill(Gdiplus::Color(60, GetRValue(SC_THEME_TOOLBAR_BLUE),
                    GetGValue(SC_THEME_TOOLBAR_BLUE), GetBValue(SC_THEME_TOOLBAR_BLUE)));
                g.FillRectangle(&fill, vp.left + 1, vp.top + 1,
                                vp.right - vp.left - 2, vp.bottom - vp.top - 2);
            }
            if (tpShow) {
                Gdiplus::SolidBrush tfill(Gdiplus::Color(36, 0xE8, 0xA3, 0x3C));
                g.FillRectangle(&tfill, tp.left + 1, tp.top + 1,
                                tp.right - tp.left - 2, tp.bottom - tp.top - 2);
            }
            // 1) 整体环：预览图外沿相邻一像素（左右列 left-1/right、上下行 top-1/bottom）
            vbar(preview.left - 1, preview.top - 1, preview.bottom + 1, kRingC);
            vbar(preview.right,     preview.top - 1, preview.bottom + 1, kRingC);
            hbar(preview.top - 1,   preview.left - 1, preview.right + 1, kRingC);
            hbar(preview.bottom,    preview.left - 1, preview.right + 1, kRingC);
            // 2) 视口蓝框：四条边界条各占一整像素行/列——固定轴两条恰压住整体环，
            //    移动轴两条压住视口带首末行/列（无衬底处也不会漏出原始内容）
            if (vpValid) {
                hbar(vp.top,      vp.left, vp.right, kBlueC);
                hbar(vp.bottom-1, vp.left, vp.right, kBlueC);
                vbar(vp.left,     vp.top + 1, vp.bottom - 1, kBlueC);
                vbar(vp.right - 1, vp.top + 1, vp.bottom - 1, kBlueC);
            }
            // 3) tentative 橙色虚线框：四边按「画4空3」分段（超出已捕获范围时贴边停驻，
            //    与 committed 同一套换算，同样盖住整体环与带内首末行）
            if (tpShow) {
                auto dashH = [&](int y, int x0, int x1) {
                    for (int x = x0; x < x1; x += 7) hbar(y, x, (std::min)(x + 4, x1), kOrngC);
                };
                auto dashV = [&](int x, int y0, int y1) {
                    for (int y = y0; y < y1; y += 7) vbar(x, y, (std::min)(y + 4, y1), kOrngC);
                };
                dashH(tp.top,       tp.left, tp.right);
                dashH(tp.bottom - 1, tp.left, tp.right);
                dashV(tp.left,      tp.top + 1, tp.bottom - 1);
                dashV(tp.right - 1, tp.top + 1, tp.bottom - 1);
            }
        }
    }
    POINT dst = { wr.left, wr.top };
    SIZE sz = { cw, ch };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(panel, NULL, &dst, &sz, s_lcPnSurfDC, &src, 0, &bf, ULW_ALPHA);
}

// 预览面板窗口过程：仅承载拼接缩略小地图与视口框的分层渲染提交时机（WM_PAINT/
// InvalidateRect 仍作为串行化重绘入口），并作为滚轮观察（Raw Input）的目标窗口。

LRESULT CALLBACK LongCapturePanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        // 内容由 LongCapturePanelRender 经 ULW 提交：这里只验证更新区并触发整幅渲染。
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        LongCapturePanelRender(hwnd, g_longCtx.load());
        return 0;
    }
    case WM_INPUT: {
        // 滚轮观察（Raw Input INPUTSINK，注册见 LongCaptureRegisterWheelObserver）：
        // 只被动解析滚轮方向与时机供防抖采样，不拦截、不影响任何输入送达底层应用。
        // 纵向模式消费纵滚轮；横向模式消费横滚轮（RI_MOUSE_HWHEEL）与 Shift+纵滚轮
        // （多数应用把 Shift+滚轮翻译为水平滚动）。wheelAccumDelta 的符号约定与
        // LongCaptureUpdateWheelEstimate 对齐：正累计 ↔ 位移 d<0（内容向头部方向滚）。
        LongCaptureContext* c = g_longCtx.load();
        if (c) {
            BYTE buf[sizeof(RAWINPUT) + 64];
            UINT size = sizeof(buf);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                const RAWINPUT* raw = (const RAWINPUT*)buf;
                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    USHORT flags = raw->data.mouse.usButtonFlags;
                    short delta = 0;
                    bool isHWheel = false;
                    if (flags & RI_MOUSE_HWHEEL) {
                        delta = (short)raw->data.mouse.usButtonData;
                        isHWheel = true;
                    } else if (flags & RI_MOUSE_WHEEL) {
                        delta = (short)raw->data.mouse.usButtonData;
                    }
                    if (delta != 0) {
                        int dir = 0, accum = 0;
                        bool consume = false;
                        if (!c->horizontal) {
                            if (!isHWheel) {          // 纵向模式忽略横滚轮
                                dir = delta > 0 ? -1 : 1;   // 正 delta = 向上滚
                                accum = delta;
                                consume = true;
                            }
                        } else if (isHWheel) {        // 横滚轮：正 delta = 向右滚 = 追加尾部
                            dir = delta > 0 ? 1 : -1;
                            accum = -delta;
                            consume = true;
                        } else if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                            dir = delta < 0 ? 1 : -1;  // Shift+滚轮向下 = 向右滚
                            accum = delta;
                            consume = true;
                        }
                        if (consume) {
                            c->wheelPending = true;
                            c->lastDir = dir;
                            c->lastWheelTick = GetTickCount();
                            // 软先验：累计待消化的滚轮增量（带符号），成功提交时折算 px/notch；
                            // 只经 LongCaptureBuildOffsetPrior 参与候选排序，绝不约束搜索范围。
                            c->wheelAccumDelta += accum;
                        }
                    }
                }
            }
        }
        DefWindowProc(hwnd, msg, wp, lp);   // 让系统释放本条原始输入缓冲
        return 0;
    }
    case WM_SETCURSOR: { SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE; }
    case WM_NCHITTEST: return HTCLIENT;
    case WM_ERASEBKGND: return 1;  // 分层窗口自绘，无系统擦底
    case WM_DESTROY:
        FreeArgbSurface(s_lcPnSurfDC, s_lcPnSurfBmp, s_lcPnSurfBits,
                        s_lcPnSurfW, s_lcPnSurfH);
        return 0;     // 不 PostQuitMessage：截图线程拥有消息循环
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// 依据拼接内容重算面板尺寸（左上角锚定、向下生长，只增不减）并触发重绘。
// 预览高等比换算按「显示空间」（横向模式宽高互换）；生长上限额外避让选区底部
// 工具栏（两者水平范围重叠时面板不得越过工具栏顶边）。

void LongCapturePanelUpdate(LongCaptureContext* c) {
    HWND panel = g_longControlWindow;
    if (!panel || c->stitchH <= 0 || c->physW <= 0) return;
    double ds = c->dpiScale;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    RECT wr;
    GetWindowRect(panel, &wr);
    int winW = wr.right - wr.left;
    int pad = sc(LC_PANEL_PAD);
    int availW = winW - pad * 2;
    // 显示空间逻辑尺寸：宽度撑满 availW 时的等比预览高
    int rowStart = 0, rowEnd = 0;
    LongCaptureOutputRows(c, rowStart, rowEnd);
    int rows = rowEnd - rowStart;
    if (rows <= 0) return;
    // 显示空间逻辑尺寸统一公式（CR-023）：固定轴取 cropRect 逻辑尺寸（纵向=宽/横向=高，
    // 无 /ds 舍入往返误差），滚动轴取 rows / ds。与 LongCaptureOutputSizeLabel、
    // LongCapturePanelPreviewRect 同一换算口径。
    double dispWLogical = c->horizontal ? rows / ds : (double)(c->cropRect.right - c->cropRect.left);
    double dispHLogical = c->horizontal ? (double)(c->cropRect.bottom - c->cropRect.top)
                                        : rows / ds;
    if (dispWLogical < 1.0) dispWLogical = 1.0;
    int prevH = (int)(dispHLogical * ((double)availW / dispWLogical) + 0.5);
    // 屏幕下沿约束：预览不超过面板顶部以下剩余空间与屏高 45%
    int roomH = (c->vy + c->vh) - sc(4) - (int)wr.top - pad * 2;
    // 选区底部工具栏避让：水平范围与面板重叠时，面板生长不得越过工具栏顶边
    if (g_longToolbarWindow) {
        RECT tb;
        if (GetWindowRect(g_longToolbarWindow, &tb)
            && tb.left < wr.right + sc(8) && tb.right > wr.left - sc(8)
            && tb.top > wr.top) {
            int byTb = tb.top - sc(8) - (int)wr.top - pad * 2;
            if (byTb < roomH) roomH = byTb;
        }
    }
    // 面板在选区上方时：生长不得越过选区顶边（否则面板入画）
    if (c->panelAbove) {
        int bySel = c->selection.top - sc(12) - (int)wr.top - pad * 2;
        if (bySel < roomH) roomH = bySel;
    }
    int capH = (int)(c->vh * 0.45);
    if (roomH < capH) capH = roomH;
    if (capH < sc(LC_PANEL_MIN_H)) capH = sc(LC_PANEL_MIN_H);
    if (prevH > capH) prevH = capH;
    int newH = pad * 2 + prevH;
    int oldH = wr.bottom - wr.top;
    if (newH > oldH)
        SetWindowPos(panel, NULL, 0, 0, winW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(panel, NULL, FALSE);
}

// 创建预览面板窗口：停靠选区右侧（空间不足退左侧，再退化到选区下方/上方），
// 初始高度按选区首帧等比，之后由 LongCapturePanelUpdate 随拼接内容向下生长。

HWND LongCaptureCreatePanel(CaptureContext* ctx, LongCaptureContext* c) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LongCapturePanelWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZToolsLongCapturePanel";
        RegisterClassExW(&wc);
        registered = true;
    }
    double ds = ctx->dpiScale;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    int winW = sc(LC_PANEL_W);
    int pad = sc(LC_PANEL_PAD);
    int margin = sc(12);
    // 首帧即整个选区：按采样裁剪等比计算初始预览高，避免面板先闪空再放大
    int selW = (std::max)(1, (int)(c->cropRect.right - c->cropRect.left));
    int selH = (std::max)(1, (int)(c->cropRect.bottom - c->cropRect.top));
    double scale0 = (double)(winW - pad * 2) / selW;
    int prevH0 = (int)(selH * scale0 + 0.5);
    int capH = (int)(ctx->virtualH * 0.45);
    if (prevH0 > capH) prevH0 = capH;
    int winH = pad * 2 + prevH0;

    int x = c->selection.right + margin;
    int y = c->selection.top;
    if (x + winW > ctx->virtualX + ctx->virtualW) x = c->selection.left - winW - margin;
    if (x < ctx->virtualX) {
        // 水平无空间（选区接近全屏宽）：退化为选区下方/上方，避免面板覆盖选区入画
        x = (std::min)((std::max)(ctx->virtualX + sc(4),
            (int)((c->selection.left + c->selection.right) / 2) - winW / 2),
            ctx->virtualX + ctx->virtualW - winW - sc(4));
        y = c->selection.bottom + margin;
        if (y + winH > ctx->virtualY + ctx->virtualH) {
            y = c->selection.top - winH - margin;
            c->panelAbove = true;
        }
    }
    if (y + winH > ctx->virtualY + ctx->virtualH) y = ctx->virtualY + ctx->virtualH - winH - sc(4);
    if (y < ctx->virtualY) y = ctx->virtualY + sc(4);
    // WS_EX_LAYERED：面板内容全部走 LongCapturePanelRender 的逐像素 alpha ULW 提交。
    // 不透明方形窗口会让圆角外四角从未绘制（表现为黑色直角残留），分层后圆角外 alpha=0。
    HWND panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
                                     | WS_EX_LAYERED,
        L"ZToolsLongCapturePanel", L"LongCapturePanel", WS_POPUP, x, y, winW, winH,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!panel) return NULL;
    // Win11 默认给无框架弹窗叠加系统圆角，与自绘圆角双重裁剪会在角部产生残缺；
    // 显式声明禁止系统圆角（属性仅 Win11 存在，Win10 返回错误码忽略即可）。
    {
        DWORD pref = 1;   // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(panel, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                              &pref, sizeof(pref));
    }
    // 分层窗口 ShowWindow 前没有可显示的内容（首次 ULW 前 DWM 不合成），
    // 先整幅渲染一次空面板（深色圆角占位框），随后再显示——无「先透明后闪现」中间帧。
    LongCapturePanelRender(panel, g_longCtx.load());
    ShowWindow(panel, SW_SHOWNOACTIVATE);
    return panel;
}

LRESULT CALLBACK LongCaptureMaskWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// 采样裁剪矩形 = 选区每边内缩 LC_CROP_INSET_LOGI（避开选区描边/抗锯齿边缘，
// 防止污染拼接内容），内缩后钳制保证宽高至少 1px。蒙版取景窗（EnterLongCaptureMask）
// 与长截图抓屏（BeginLongCapture 的 lc->cropRect）共用同一算法，原先两处手抄易漂移。

RECT CalcSampleCrop(const CaptureContext* ctx) {
    RECT crop = ctx->selection;
    int inset = LC_CROP_INSET_LOGI;
    crop.left   = (std::min)(crop.right - 1, crop.left + inset);
    crop.top    = (std::min)(crop.bottom - 1, crop.top + inset);
    crop.right  = (std::max)(crop.left + 1, crop.right - inset);
    crop.bottom = (std::max)(crop.top + 1, crop.bottom - inset);
    return crop;
}

// （整窗点击穿透），因此滚轮/点击在任意位置都直达底层窗口——整个背景均可滚动操作。

// 蒙版为 WS_EX_LAYERED（逐像素预乘 ARGB，UpdateLayeredWindow 一次性提交）+ WS_EX_TRANSPARENT

// 进入长截图蒙版：创建全屏半透明灰色蒙版窗口，覆盖所有屏幕（不限于选区）。

void EnterLongCaptureMask(const CaptureContext* ctx) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LongCaptureMaskWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZToolsLongCaptureMask";
        RegisterClassExW(&wc);
        registered = true;
    }
    if (g_longMaskWindow) { DestroyWindow(g_longMaskWindow); g_longMaskWindow = NULL; }

    int w = ctx->virtualW, h = ctx->virtualH;
    HWND mask = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"ZToolsLongCaptureMask", L"LongCaptureMask", WS_POPUP,
        ctx->virtualX, ctx->virtualY, w, h,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!mask) return;
    g_longMaskWindow = mask;

    // 采样裁剪矩形 = 选区每边内缩（公式收口见 CalcSampleCrop 注释）
    RECT crop = CalcSampleCrop(ctx);
    int cropW = crop.right - crop.left, cropH = crop.bottom - crop.top;

    // 32bpp 预乘 ARGB 表面：整屏半透明灰 → 选区边缘蓝色描边（外偏采样区）→ 采样裁剪区清透明
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(memDC, bmp);
        Gdiplus::Bitmap gbmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        {
            // 1) 整屏半透明灰
            Gdiplus::Graphics g(&gbmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush gray(Gdiplus::Color(
                LONG_MASK_ALPHA, LONG_MASK_GRAY, LONG_MASK_GRAY, LONG_MASK_GRAY));
            g.FillRectangle(&gray, (Gdiplus::REAL)0, (Gdiplus::REAL)0, (Gdiplus::REAL)w, (Gdiplus::REAL)h);
        }
        {
            // 2) 选区边缘蓝色描边（落在半透明灰上，清晰可辨）。
            //    路径 = 采样裁剪矩形向外偏移 LC_CROP_INSET_LOGI 的圆角矩形（半径同步外扩）：
            //    GDI+ 居中描边的内半宽（1.25px）+ 抗锯齿（~0.5px）会越过路径内侧，若直接以
            //    crop 为路径，内半圈描边恰好落进采样区（crop = 每帧 BitBlt 取样范围 = 最终
            //    拼接画面），滚动拼接后画面边缘与接缝处会混入蓝色边框线。外偏 2px 使直边段
            //    描边整体留在采样区外；圆角弧段在大半径时仍会斜向内凹越过采样区直角角落，
            //    由紧随其后的第 3 步清透明兜底擦除。
            Gdiplus::Graphics g(&gbmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(ScOpaqueColor(SC_THEME_ACCENT_BLUE), 2.5f);
            Gdiplus::GraphicsPath path;
            int outX = crop.left - ctx->virtualX - LC_CROP_INSET_LOGI;
            int outY = crop.top - ctx->virtualY - LC_CROP_INSET_LOGI;
            int outW = cropW + LC_CROP_INSET_LOGI * 2;
            int outH = cropH + LC_CROP_INSET_LOGI * 2;
            int radius = (std::min)(ctx->selectionCornerRadius, (std::min)(cropW, cropH) / 2)
                       + LC_CROP_INSET_LOGI;
            AddRoundedRect(path, outX, outY, outW, outH, radius);
            g.DrawPath(&pen, &path);
        }
        {
            // 3) 清空采样裁剪区为全透明（SourceCopy 直接写回，透出实况桌面）。
            //    放在描边之后：即使描边圆角弧段越界进入采样区，也在此被整体擦除，
            //    保证抓屏取样范围内绝无蒙版自身像素。
            Gdiplus::Graphics g(&gbmp);
            Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.FillRectangle(&clear,
                (Gdiplus::REAL)(crop.left - ctx->virtualX), (Gdiplus::REAL)(crop.top - ctx->virtualY),
                (Gdiplus::REAL)cropW, (Gdiplus::REAL)cropH);
        }
        SIZE sz = { w, h };
        POINT dst = { ctx->virtualX, ctx->virtualY };
        POINT src = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(mask, NULL, &dst, &sz, memDC, &src, 0, &bf, ULW_ALPHA);
        SelectObject(memDC, old);
    }
    if (memDC) DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    if (bmp) DeleteObject(bmp);
    ShowWindow(mask, SW_SHOWNOACTIVATE);
}

// 确保 32bpp 预乘 ARGB 后备 DIB 就绪：尺寸不变时复用，变化时释放重建。
// dc/bmp/bits/w/h 由调用方持有（工具栏/tooltip 各一份静态），成功返回 true。
bool EnsureArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h,
                              int wantW, int wantH) {
    if (bmp && w == wantW && h == wantH) return true;
    if (dc) { DeleteDC(dc); dc = NULL; }
    if (bmp) { DeleteObject(bmp); bmp = NULL; }
    bits = nullptr;
    w = h = 0;
    if (wantW <= 0 || wantH <= 0) return false;
    HDC screen = GetDC(NULL);
    if (!screen) return false;
    dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = wantW;
    bi.bmiHeader.biHeight = -wantH;   // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (dc) bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!dc || !bmp || !bits) return false;
    SelectObject(dc, bmp);
    w = wantW;
    h = wantH;
    return true;
}

// 释放后备 DIB（窗口销毁时调用）
void FreeArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h) {
    if (dc) { DeleteDC(dc); dc = NULL; }
    if (bmp) { DeleteObject(bmp); bmp = NULL; }
    bits = nullptr;
    w = h = 0;
}

