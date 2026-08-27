// 截图模块：马赛克子系统（reveal-mask 模型、画笔光标、重建/揭示）
#include "internal.h"

// ==================== 马赛克渲染 ====================

// 马赛克原理：将原始屏幕位图（srcDC = memDC，物理像素）一次缩小到按 blockPx
// 计算的低分辨率位图，再以最近邻一次放大到目标 DC，避免逐块执行大量 GDI 调用。
// 目标 DC（targetDC）为逻辑像素（backDC / finalDC），源 DC（srcDC）为物理像素（memDC），
// 二者通过 dpiScale 换算：srcX = (absX - virtualX) * dpiScale。

// 对单个矩形区域批量生成马赛克并绘制到 targetDC。
// dstX0/dstY0/dstW/dstH 是目标逻辑像素矩形；srcAbsX0/srcAbsY0 是其在虚拟屏幕中的
// 绝对逻辑坐标。低分辨率尺寸按 blockPx 向上取整，右/下不足整块的边缘由最后一格覆盖。
// 返回 true 表示缩小和最近邻放大均成功；失败时不修改缓存有效性，调用方可跳过揭示。

bool MosaicBlitRect(HDC targetDC, HDC srcDC,
                           int dstX0, int dstY0, int dstW, int dstH,
                           int srcAbsX0, int srcAbsY0,
                           int blockPx, int virtualX, int virtualY, double dpiScale) {
    if (!targetDC || !srcDC || dstW <= 0 || dstH <= 0 || blockPx < 1 || dpiScale <= 0) {
        return false;
    }

    // 每个低分辨率像素对应约一个 blockPx 逻辑像素块；向上取整覆盖非整块边缘。
    int reducedW = (dstW + blockPx - 1) / blockPx;
    int reducedH = (dstH + blockPx - 1) / blockPx;
    if (reducedW <= 0 || reducedH <= 0) return false;

    // 源位图是物理像素，目标与马赛克块大小是逻辑像素，统一在这里完成坐标换算。
    int srcX = (int)((srcAbsX0 - virtualX) * dpiScale + 0.5);
    int srcY = (int)((srcAbsY0 - virtualY) * dpiScale + 0.5);
    int srcW = (int)(dstW * dpiScale + 0.5);
    int srcH = (int)(dstH * dpiScale + 0.5);
    if (srcW < 1) srcW = 1;
    if (srcH < 1) srcH = 1;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;
    HDC reducedDC = CreateCompatibleDC(screenDC);
    HBITMAP reducedBmp = reducedDC
        ? CreateCompatibleBitmap(screenDC, reducedW, reducedH)
        : NULL;
    ReleaseDC(NULL, screenDC);
    if (!reducedDC || !reducedBmp) {
        if (reducedBmp) DeleteObject(reducedBmp);
        if (reducedDC) DeleteDC(reducedDC);
        return false;
    }

    HGDIOBJ oldReducedBmp = SelectObject(reducedDC, reducedBmp);
    if (!oldReducedBmp || oldReducedBmp == HGDI_ERROR) {
        DeleteObject(reducedBmp);
        DeleteDC(reducedDC);
        return false;
    }

    // 第一次 StretchBlt：以 HALFTONE 将整个源区域压缩到低分辨率马赛克采样图。
    int oldReducedMode = GetStretchBltMode(reducedDC);
    POINT oldBrushOrigin = {0, 0};
    bool reducedModeReady = oldReducedMode != 0
        && SetStretchBltMode(reducedDC, HALFTONE) != 0
        && SetBrushOrgEx(reducedDC, 0, 0, &oldBrushOrigin);
    bool reducedOk = reducedModeReady
        && StretchBlt(reducedDC, 0, 0, reducedW, reducedH,
                      srcDC, srcX, srcY, srcW, srcH, SRCCOPY);
    if (reducedModeReady) {
        SetBrushOrgEx(reducedDC, oldBrushOrigin.x, oldBrushOrigin.y, NULL);
    }
    if (oldReducedMode != 0) SetStretchBltMode(reducedDC, oldReducedMode);

    // 第二次 StretchBlt：最近邻放大，保持每个低分辨率采样点为纯色块。
    int oldTargetMode = GetStretchBltMode(targetDC);
    bool targetModeReady = reducedOk && oldTargetMode != 0
        && SetStretchBltMode(targetDC, COLORONCOLOR) != 0;
    bool expandedOk = targetModeReady
        && StretchBlt(targetDC, dstX0, dstY0, dstW, dstH,
                      reducedDC, 0, 0, reducedW, reducedH, SRCCOPY);
    if (oldTargetMode != 0) SetStretchBltMode(targetDC, oldTargetMode);

    SelectObject(reducedDC, oldReducedBmp);
    DeleteObject(reducedBmp);
    DeleteDC(reducedDC);
    return reducedOk && expandedOk;
}

// ==================== 马赛克渲染（reveal-mask 模型） ====================
// 核心：仅在存在已提交马赛克或正在绘制马赛克时，才把整张虚拟屏幕按当前块大小
// 生成 mosaicBase。马赛克标注只是「蒙版」——涂抹=路径圆形区域、框选=矩形区域——
// 揭示其背后的 mosaicBase。任意区域、任意顺序叠加都连续无缝；切换块大小时延迟到
// 下一次实际需要马赛克的绘制再重建，避免普通截图确认态承担无用的整屏预处理。

// 释放马赛克 base 资源

void FreeMosaicBase(CaptureContext* ctx) {
    if (ctx->mosaicBaseDC) { DeleteDC(ctx->mosaicBaseDC); ctx->mosaicBaseDC = NULL; }
    if (ctx->mosaicBaseBitmap) { DeleteObject(ctx->mosaicBaseBitmap); ctx->mosaicBaseBitmap = NULL; }
    ctx->mosaicBaseW = 0;
    ctx->mosaicBaseH = 0;
    ctx->mosaicBaseBlockPx = 0;
}

// 用 GDI+ 把单色位图转为带透明通道的 32bpp HBITMAP（用于光标）。

static HBITMAP ColorBitmapFromBitmap(Gdiplus::Bitmap& bmp) {
    HBITMAP hBmp = NULL;
    Gdiplus::Color bg(0, 0, 0, 0);  // 透明背景
    bmp.GetHBITMAP(bg, &hBmp);
    return hBmp;
}

// 生成单个涂抹光标：半径圆（白底 + 深色描边 + 中心十字）。
// size 为光标位图边长（逻辑像素）；hotspot 在中心。

static HCURSOR CreateMosaicBrushCursor(int radius) {
    int pad = 3;
    int size = (radius + pad) * 2;
    if (size < 16) size = 16;

    HCURSOR result = NULL;
    // GDI+ 已由会话级 InitGdipResources 启动（在 InitMosaicBrushCursors 调用前完成）。
    {
        Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            int cx = size / 2;
            int cy = size / 2;
            // 外圈：白色描边底（保证暗背景可见）
            Gdiplus::Pen whitePen(Gdiplus::Color(255, 255, 255, 255), 3.0f);
            g.DrawEllipse(&whitePen, cx - radius, cy - radius, radius * 2, radius * 2);
            // 内圈：深色虚线描边
            Gdiplus::Pen darkPen(Gdiplus::Color(255, 30, 30, 30), 1.5f);
            darkPen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawEllipse(&darkPen, cx - radius, cy - radius, radius * 2, radius * 2);
            // 中心十字（准星）
            Gdiplus::Pen crossPen(Gdiplus::Color(255, 30, 30, 30), 1.0f);
            int cl = (std::min)(6, radius);
            g.DrawLine(&crossPen, cx - cl, cy, cx + cl, cy);
            g.DrawLine(&crossPen, cx, cy - cl, cx, cy + cl);
        }
        HBITMAP hColor = ColorBitmapFromBitmap(bmp);

        // 掩码位图（全黑，使用彩色光标时掩码可忽略，但 CreateIcon 要求非空）
        HDC screenDC = GetDC(NULL);
        HDC maskDC = CreateCompatibleDC(screenDC);
        HBITMAP hMask = CreateCompatibleBitmap(screenDC, size, size);
        HGDIOBJ oldMask = SelectObject(maskDC, hMask);
        PatBlt(maskDC, 0, 0, size, size, BLACKNESS);
        SelectObject(maskDC, oldMask);
        DeleteDC(maskDC);
        ReleaseDC(NULL, screenDC);

        ICONINFO ii = {0};
        ii.fIcon = FALSE;
        ii.xHotspot = size / 2;
        ii.yHotspot = size / 2;
        ii.hbmMask = hMask;
        ii.hbmColor = hColor;
        result = CreateIconIndirect(&ii);
        DeleteObject(hMask);
        DeleteObject(hColor);
    }
    return result;
}

// 初始化涂抹光标缓存（按 DPI 缩放半径）。

void InitMosaicBrushCursors(CaptureContext* ctx) {
    if (ctx->mosaicBrushCursorsInited) return;
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
        ctx->mosaicBrushCursors[i] = CreateMosaicBrushCursor(SC_MOSAIC_RADIUS[i]);
    }
    ctx->mosaicBrushCursorsInited = true;
}

void FreeMosaicBrushCursors(CaptureContext* ctx) {
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
        if (ctx->mosaicBrushCursors[i]) { DestroyIcon(ctx->mosaicBrushCursors[i]); ctx->mosaicBrushCursors[i] = NULL; }
    }
    ctx->mosaicBrushCursorsInited = false;
}

// 生成会话级马赛克底图：将整个虚拟屏幕批量缩小后最近邻放大到离屏位图。
// 缓存坐标与 backDC 一致（原点 = 虚拟屏幕左上角），尺寸 = virtualW×virtualH，
// 与选区无关：选区移动/缩放不需要重建，仅在虚拟屏幕尺寸或块大小变化时重建。
// 先在临时 GDI 对象中完整生成，成功后再替换旧缓存；失败会保留“无有效缓存”状态。

bool RebuildMosaicBase(CaptureContext* ctx) {
    int w = ctx->virtualW;
    int h = ctx->virtualH;
    if (w <= 0 || h <= 0) {
        FreeMosaicBase(ctx);
        return false;
    }

    int blockPx = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
    if (blockPx < 2) blockPx = 2;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        FreeMosaicBase(ctx);
        return false;
    }
    HDC newDC = CreateCompatibleDC(screenDC);
    HBITMAP newBitmap = newDC ? CreateCompatibleBitmap(screenDC, w, h) : NULL;
    ReleaseDC(NULL, screenDC);
    if (!newDC || !newBitmap) {
        if (newBitmap) DeleteObject(newBitmap);
        if (newDC) DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    HGDIOBJ oldNewBitmap = SelectObject(newDC, newBitmap);
    if (!oldNewBitmap || oldNewBitmap == HGDI_ERROR) {
        DeleteObject(newBitmap);
        DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    // 整虚拟屏幕按 blockPx 马赛克化：base 原点 = 虚拟屏幕左上角，源绝对坐标 = virtualX/virtualY。
    bool generated = MosaicBlitRect(newDC, ctx->memDC, 0, 0, w, h,
                                    ctx->virtualX, ctx->virtualY, blockPx,
                                    ctx->virtualX, ctx->virtualY, ctx->dpiScale);
    if (!generated) {
        SelectObject(newDC, oldNewBitmap);
        DeleteObject(newBitmap);
        DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    FreeMosaicBase(ctx);
    ctx->mosaicBaseDC = newDC;
    ctx->mosaicBaseBitmap = newBitmap;
    ctx->mosaicBaseW = w;
    ctx->mosaicBaseH = h;
    ctx->mosaicBaseBlockPx = blockPx;
    return true;
}

// 判断现有马赛克缓存是否缺失，或尺寸/块大小已与当前会话不一致。
// 此函数只检查缓存键；调用方必须先通过 HasMosaicToRender 确认当前帧确实需要马赛克。

bool MosaicBaseNeedsRebuild(const CaptureContext* ctx) {
    int blockPx = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
    if (blockPx < 2) blockPx = 2;
    bool blockChanged = (blockPx != ctx->mosaicBaseBlockPx);
    bool sizeChanged = (ctx->mosaicBaseW != ctx->virtualW || ctx->mosaicBaseH != ctx->virtualH);
    return blockChanged || sizeChanged || !ctx->mosaicBaseDC;
}

bool HasMosaicToRender(const std::vector<Annotation>& annotations, const Annotation* curDrawing) {
    if (curDrawing && curDrawing->type == AT_Mosaic) return true;
    for (const Annotation& a : annotations) {
        if (a.type == AT_Mosaic) return true;
    }
    return false;
}

// 把单条马赛克标注对应的蒙版构建为目标 DC 局部坐标下的 HRGN。
// ox/oy 将标注的绝对屏幕坐标转换到目标坐标系：覆盖层使用 -virtualX/-virtualY，
// 导出位图使用 -selection.left/-selection.top。

static HRGN BuildMosaicMaskRegion(const Annotation& a, float ox, float oy) {
    if (a.mosaicRect) {
        int absL = (std::min)(a.x1, a.x2);
        int absT = (std::min)(a.y1, a.y2);
        int absR = (std::max)(a.x1, a.x2);
        int absB = (std::max)(a.y1, a.y2);
        return CreateRectRgn((int)(absL + ox + 0.5f), (int)(absT + oy + 0.5f),
                             (int)(absR + ox + 0.5f), (int)(absB + oy + 0.5f));
    } else {
        // 涂抹：把整条路径变为连续的「胶囊」区域（保证快速移动时不留空隙）。
        // 做法：沿相邻点之间的线段以不超过 radius/2 的步长插值取点，每个点画一个圆并并入区域，
        // 相邻圆重叠从而形成无缝的粗笔触轨迹。
        int radius = a.brushRadius;
        if (radius < 1) radius = 1;
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        if (a.pts.empty()) return rgn;
        // 步长：半径的一半，保证相邻圆重叠 ≥50%，无视觉缝隙
        double step = (std::max)(1.0, radius * 0.5);

        auto addCircle = [&](double cx, double cy) {
            int ix = (int)(cx + 0.5);
            int iy = (int)(cy + 0.5);
            HRGN circle = CreateEllipticRgn(ix - radius, iy - radius,
                                            ix + radius, iy + radius);
            CombineRgn(rgn, rgn, circle, RGN_OR);
            DeleteObject(circle);
        };

        // 第一个点
        addCircle(a.pts[0].x + ox, a.pts[0].y + oy);
        for (size_t i = 1; i < a.pts.size(); i++) {
            double x0 = a.pts[i - 1].x + ox;
            double y0 = a.pts[i - 1].y + oy;
            double x1 = a.pts[i].x + ox;
            double y1 = a.pts[i].y + oy;
            double dx = x1 - x0, dy = y1 - y0;
            double segLen = std::sqrt(dx * dx + dy * dy);
            if (segLen < 0.5) {
                addCircle(x1, y1);
                continue;
            }
            int n = (int)(segLen / step + 0.5);
            if (n < 1) n = 1;
            for (int k = 1; k <= n; k++) {
                double t = (double)k / n;
                addCircle(x0 + dx * t, y0 + dy * t);
            }
        }
        return rgn;
    }
}

// 揭示马赛克：把 mosaicBase 中由 masks（已提交标注）+ curDrawing（正在绘制）覆盖的区域
// BitBlt 到 targetDC。mosaicBase 与 targetDC 使用相同的目标坐标系：覆盖层以虚拟屏幕
// 左上角为原点，导出位图以截图选区左上角为原点。
// contentBounds 是目标 DC 中允许显示马赛克的内容矩形；最终蒙版会与其求交，确保笔刷半径
// 或历史标注不会越过截图选区。ox/oy 用于把标注绝对坐标换算到目标局部坐标。

void RevealMosaicToTarget(HDC targetDC, HDC mosaicBase,
                                 const std::vector<Annotation>& annotations,
                                 const Annotation* curDrawing,
                                 const RECT& contentBounds,
                                 float ox, float oy) {
    if (!targetDC || !mosaicBase
        || contentBounds.right <= contentBounds.left
        || contentBounds.bottom <= contentBounds.top) {
        return;
    }

    // 合并所有马赛克标注的蒙版区域（目标局部坐标）
    HRGN mask = CreateRectRgn(0, 0, 0, 0);
    bool any = false;
    for (const Annotation& a : annotations) {
        if (a.type != AT_Mosaic) continue;
        HRGN r = BuildMosaicMaskRegion(a, ox, oy);
        CombineRgn(mask, mask, r, RGN_OR);
        DeleteObject(r);
        any = true;
    }
    if (curDrawing && curDrawing->type == AT_Mosaic) {
        HRGN r = BuildMosaicMaskRegion(*curDrawing, ox, oy);
        CombineRgn(mask, mask, r, RGN_OR);
        DeleteObject(r);
        any = true;
    }
    if (any) {
        // 先把马赛克蒙版裁到截图内容矩形，再与调用方已有的 dirtyRect 裁剪区求交。
        // contentBounds 采用半开区间；因此边框所在的矩形外沿不会被马赛克写入。
        HRGN contentRgn = CreateRectRgn(contentBounds.left, contentBounds.top,
                                        contentBounds.right, contentBounds.bottom);
        CombineRgn(mask, mask, contentRgn, RGN_AND);
        DeleteObject(contentRgn);

        // 用 SaveDC 保护调用方的裁剪区（P1 局部帧时为 dirtyRect），揭示结束后完整恢复。
        int saved = SaveDC(targetDC);
        // mask 与现有裁剪区（dirtyRect）求交，揭示只发生在 dirtyRect∩选区∩蒙版。
        ExtSelectClipRgn(targetDC, mask, RGN_AND);
        // base 与 targetDC 同坐标系，1:1 拷贝。
        BitBlt(targetDC, contentBounds.left, contentBounds.top,
               contentBounds.right - contentBounds.left,
               contentBounds.bottom - contentBounds.top,
               mosaicBase, contentBounds.left, contentBounds.top, SRCCOPY);
        if (saved) RestoreDC(targetDC, saved);
    }
    DeleteObject(mask);
}
