// 长截图子系统：抓帧 / DIB / 缩略图 / 位图构建 / 消息泵。
// 拆分自 long_capture_windows.cpp 的「滚轮观察 / 逐帧采样」与
// 「缩略图 / 输出行窗口 / 结果位图 / 消息泵 / 首帧初始化」段。
#include "internal.h"
#include "long_capture_internal.h"

// ==================== 长截图：滚轮观察 / 逐帧采样 / 增量拼接 ====================

bool LongCaptureRegisterWheelObserver(HWND target) {
    if (!target) return false;
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;              // Generic Desktop
    rid.usUsage = 0x02;                  // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;       // 非前台也接收（前台是选区下的底层应用）
    rid.hwndTarget = target;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
}

// 会话结束注销滚轮观察（RIDEV_REMOVE 要求 hwndTarget 为 NULL）。

void LongCaptureUnregisterWheelObserver() {
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

// 写入可复用的 DIBSection，再复制为 32bpp BGRA 缓冲（自上而下行序）。成功返回 true。

// 采样选区当前视口帧：直接用屏幕 DC BitBlt 出选区物理像素（不再整屏抓取后裁剪），

bool LongCaptureEnsureDib(LongCaptureContext* c, HDC screenDC) {
    if (c->dibBmp && c->dibW == c->capW && c->dibH == c->capH) return true;
    if (c->dibDC) { DeleteDC(c->dibDC); c->dibDC = NULL; }
    if (c->dibBmp) { DeleteObject(c->dibBmp); c->dibBmp = NULL; }
    c->dibDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = c->capW;
    bi.bmiHeader.biHeight = -c->capH;   // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->dibBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &c->dibBits, NULL, 0);
    if (!c->dibDC || !c->dibBmp) {
        // 创建失败必须复位全部 DIB 状态：旧位图已在上方删除，dibBits 若仍指向
        // 已释放内存、宽高若保留旧值，调用方据此判重入会误用脏数据参与匹配。
        if (c->dibDC) { DeleteDC(c->dibDC); c->dibDC = NULL; }
        if (c->dibBmp) { DeleteObject(c->dibBmp); c->dibBmp = NULL; }
        c->dibBits = nullptr;
        c->dibW = 0; c->dibH = 0;
        return false;
    }
    c->dibW = c->capW; c->dibH = c->capH;
    SelectObject(c->dibDC, c->dibBmp);
    return true;
}

bool LongCaptureCaptureFrameBuf(LongCaptureContext* c, std::vector<uint32_t>& out) {
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;
    if (!LongCaptureEnsureDib(c, screenDC)) { ReleaseDC(NULL, screenDC); return false; }
    // DPI 感知下屏幕 DC 坐标为物理像素：源偏移 = 虚拟屏幕物理原点 + 选区相对偏移。
    // 不带 CAPTUREBLT：该标志强制 DWM 合成所有分层窗口（含本功能自身的全屏蒙版），
    // 单帧耗时可达数十毫秒且伴随光标闪烁；选区取景内容为普通窗口桌面，无需该标志。
    // BitBlt 失败（UAC 安全桌面/独占全屏瞬断）时 dibBits 残留的是上一帧像素，
    // 绝不能当作本帧返回——否则新旧混合帧会静默错拼。直接失败，由调用方按
    // 瞬态故障重试（首帧路径则上报失败）。
    if (!BitBlt(c->dibDC, 0, 0, c->capW, c->capH, screenDC,
                c->physOriginX + c->physX, c->physOriginY + c->physY, SRCCOPY)) {
        ReleaseDC(NULL, screenDC);
        return false;
    }
    ReleaseDC(NULL, screenDC);

    size_t n = (size_t)c->capW * (size_t)c->capH;
    out.resize(n);
    // 横向模式：帧缓冲转置（capW×capH → capH×capW = physW×physH），水平滚动位移
    // 由此映射为垂直位移，匹配/拼接/缩略图整条管线与纵向完全同构，仅最终输出回转。
    // T(行 r<capW, 列 q<capH)[r*capH + q] = S(行 q, 列 r)[q*capW + r]。
    if (!c->horizontal) {
        memcpy(out.data(), c->dibBits, n * 4);
    } else {
        const uint32_t* src = (const uint32_t*)c->dibBits;
        for (int r = 0; r < c->capW; r++)
            for (int q = 0; q < c->capH; q++)
                out[(size_t)r * c->capH + q] = src[(size_t)q * c->capW + r];
    }
    return true;
}

// 将一列物理像素行缩为一行（列方向整数面积平均，AVG 通道忽略），供面板缩略图增量维护。

void LongCaptureDownscaleRow(const uint32_t* src, uint32_t* dst, int srcW, int dstW) {
    if (srcW <= 0 || dstW <= 0) return;
    if (srcW == dstW) { memcpy(dst, src, (size_t)dstW * 4); return; }
    for (int c = 0; c < dstW; c++) {
        int s0 = (int)((long long)c * srcW / dstW);
        int s1 = (int)((long long)(c + 1) * srcW / dstW);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > srcW) s1 = srcW;
        unsigned r = 0, g = 0, b = 0;
        for (int s = s0; s < s1; s++) {
            uint32_t px = src[s];
            b += px & 0xFF;
            g += (px >> 8) & 0xFF;
            r += (px >> 16) & 0xFF;
        }
        int n = s1 - s0;
        dst[c] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
}

// 重建面板绘制用合并缩略图（reverse(thumbHeadRev) + thumbBody），内容变更时调用。
// 横向模式额外生成回转后的显示缓冲（thumbMerged 为转置空间的纵向图，直接展示会被
// 旋转 90°；thumbDisplay 转置回原方向供面板绘制）。

void LongCaptureRebuildThumb(LongCaptureContext* c) {
    if (!c->thumbDirty || c->thumbW <= 0) return;
    c->thumbMerged.resize((size_t)c->thumbW * (size_t)c->thumbH);
    const size_t rowW = (size_t)c->thumbW;
    uint32_t* dst = c->thumbMerged.data() + (size_t)c->thumbHeadH * rowW;
    if (!c->thumbBody.empty())
        memcpy(dst, c->thumbBody.data(), c->thumbBody.size() * 4);
    dst = c->thumbMerged.data();
    for (int r = c->thumbHeadH - 1; r >= 0; r--)
        memcpy(dst + (size_t)(c->thumbHeadH - 1 - r) * rowW,
               c->thumbHeadRev.data() + (size_t)r * rowW, rowW * 4);
    c->thumbDirty = false;
    if (c->horizontal) c->thumbDisplayDirty = true;
}

// 横向模式：把转置空间的合并缩略图回转为原方向显示缓冲（宽高互换的纯转置）。

void LongCaptureRebuildThumbDisplay(LongCaptureContext* c) {
    if (!c->horizontal) { c->thumbDisplayDirty = false; return; }
    int w = c->thumbW, h = c->thumbH;
    if (w <= 0 || h <= 0) { c->thumbDisplayDirty = false; return; }
    c->thumbDisplay.resize((size_t)w * (size_t)h);
    for (int y = 0; y < w; y++)
        for (int x = 0; x < h; x++)
            c->thumbDisplay[(size_t)y * h + x] = c->thumbMerged[(size_t)x * w + y];
    c->thumbDisplayW = h;
    c->thumbDisplayH = w;
    c->thumbDisplayDirty = false;
}

// 当前输出行窗口（拼接图行坐标；未裁剪 = [0, stitchH)）。裁剪只约束预览/尺寸标签/输出，
// 绝不修改拼接缓冲与匹配基准，已捕获内容始终保留在缓冲内（待剔除区间见
// LongCaptureExecuteCropPurge）。裁剪锚点以内容坐标存储：拼接图行 = 内容坐标 + headRows，
// 窗口随前插自动平移、且未设置边界的一侧恒开放——继续滚动新增的拼接行始终落在
// 输出窗口内（「裁剪后继续滚动 = 以当前位置为基准继续拼图」的显示侧保证）。

void LongCaptureOutputRows(const LongCaptureContext* c, int& outTop, int& outBottom) {
    outTop = 0;
    outBottom = c->stitchH;
    if (!c->cropped) return;
    // 哨兵值显式短路：INT64_MIN/MAX 直接参与 +headRows 加法是有符号溢出（UB）；
    // 开放侧保持哨兵、由区间钳制自然落到界外，与「该侧未设边界」语义一致。
    int64_t t = c->cropTopY == INT64_MIN ? INT64_MIN : c->cropTopY + c->headRows;
    if (t > 0 && t < c->stitchH) outTop = (int)t;
    int64_t b = c->cropBottomY == INT64_MAX ? INT64_MAX : c->cropBottomY + c->headRows;
    if (b > outTop && b < c->stitchH) outBottom = (int)b;
}

// 写出 c->outWidth/outHeight，返回位图（调用方负责 DeleteObject）。
// 由拼接段（reverse(headRev) + body）构造结果位图：先应用裁剪行窗口，横向模式再
// 转置回原方向（拼接空间恒为「宽度=physW、高度=行数」的纵向图）。

HBITMAP LongCaptureBuildResultBitmap(LongCaptureContext* c) {
    if (c->stitchH <= 0 || c->physW <= 0) return NULL;
    int rowStart = 0, rowEnd = 0;
    LongCaptureOutputRows(c, rowStart, rowEnd);
    int rows = rowEnd - rowStart;
    if (rows <= 0) return NULL;
    // 段合并仅发生在结束时这一次：头部倒序段 + 主体段
    const size_t rowW = (size_t)c->physW;
    std::vector<uint32_t> merged(rowW * (size_t)rows);
    {
        // headRev/body 在 [0, headRows) / [headRows, stitchH) 两个行区间内取行
        auto copyRows = [&](int64_t from, int64_t to, int64_t dstOff) {
            for (int64_t r = from; r < to; r++) {
                const uint32_t* srcRow = r < c->headRows
                    ? c->headRev.data() + (size_t)(c->headRows - 1 - r) * rowW
                    : c->body.data() + (size_t)(r - c->headRows) * rowW;
                memcpy(merged.data() + (size_t)(dstOff + r - rowStart) * rowW, srcRow, rowW * 4);
            }
        };
        copyRows(rowStart, rowEnd, 0);
    }

    // 横向模式：转置回原方向（拼接空间 physW×rows → 显示空间 rows×physW）：
    // F(行 fy<physW, 列 fx<rows)[fy*rows + fx] = M(行 fx, 列 fy)[fx*physW + fy]。
    std::vector<uint32_t> finalBuf;
    int outW = 0, outH = 0;
    if (c->horizontal) {
        outW = rows;
        outH = c->physW;
        finalBuf.resize((size_t)outW * (size_t)outH);
        for (int fy = 0; fy < outH; fy++)
            for (int fx = 0; fx < outW; fx++)
                finalBuf[(size_t)fy * outW + fx] = merged[(size_t)fx * c->physW + fy];
    } else {
        outW = c->physW;
        outH = rows;
        finalBuf.swap(merged);
    }

    HDC screenDC = GetDC(NULL);
    HDC outDC = CreateCompatibleDC(screenDC);
    HBITMAP outBmp = CreateCompatibleBitmap(screenDC, outW, outH);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = outW;
    bi.bmiHeader.biHeight = -outH;   // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = (DWORD)((SIZE_T)outW * (SIZE_T)outH * 4);
    SetDIBits(outDC, outBmp, 0, outH, finalBuf.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(outDC);
    ReleaseDC(NULL, screenDC);
    c->outWidth = outW;
    c->outHeight = outH;
    return outBmp;
}

// 泵送线程消息：滚轮钩子回调、预览面板绘制与按钮点击均依赖本线程消息循环。

void LongCapturePumpMessages(LongCaptureContext* c) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { c->abortFlag.store(true); return; }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// 以给定帧初始化第 0 帧基准（纯数据初始化，不抓屏）：frame 即首屏内容
//（既作主体段也作重叠检测基准），调用后 frame 缓冲被交换复用。
// 由 LongCaptureInitFirstFrame（真实抓屏链路）与单元测试（合成帧注入）共用。

void LongCaptureInitBaseline(LongCaptureContext* c, std::vector<uint32_t>& frame) {
    c->lastFrame.swap(frame);       // lastFrame = 首帧；frame 复用旧缓冲
    c->body = c->lastFrame;
    c->bodyRows = c->physH;
    c->stitchH = c->physH;
    LongCaptureBuildMatchData(c->lastFrame, c->physW, c->physH, c->lastMatch);
    // 跟踪/历史基准初始化：首帧即已提交基准（内容坐标 0），tentative 与 committed 对齐；
    // 首帧同时作为第 0 条历史条目（已提交、位置精确），供失败后的多跳回溯起步。
    c->committedContentTop = 0;
    c->tentativeContentTop = 0;
    c->tentativeValid = true;
    c->tentativeConfidence = 1.0f;
    c->trackUnreliableStreak = 0;
    c->lastCommittedFrameId = 0;
    c->frameHistory.clear();
    c->weakCandidateOffsets.clear();
    LongCaptureHistoryPush(c, 0, LongMatchData(c->lastMatch), 0, true);
    if (c->thumbW > 0) {
        c->thumbBody.resize((size_t)c->physH * c->thumbW);
        uint32_t* dst = c->thumbBody.data();
        for (int r = 0; r < c->physH; r++) {
            LongCaptureDownscaleRow(c->lastFrame.data() + (size_t)r * c->physW,
                                    dst, c->physW, c->thumbW);
            dst += c->thumbW;
        }
        c->thumbH = c->physH;
        c->thumbDirty = true;
    }
}

// 抓取首帧并初始化基准。返回 false 表示首帧抓取失败。

bool LongCaptureInitFirstFrame(LongCaptureContext* c, std::vector<uint32_t>& frameBuf) {
    if (!LongCaptureCaptureFrameBuf(c, frameBuf)) return false;
    LongCaptureInitBaseline(c, frameBuf);
    return true;
}

// 构造最终输出位图：拼接合并（应用裁剪行窗口 + 横向回转）后按 DPI 缩放回逻辑尺寸，
// 写出 outWidth/outHeight（逻辑尺寸，与回调量纲一致）。失败返回 NULL。

HBITMAP LongCaptureBuildFinalBitmap(LongCaptureContext* c) {
    HBITMAP stitched = LongCaptureBuildResultBitmap(c);
    if (!stitched) return NULL;
    double ds = c->dpiScale;
    if (ds > 1.01 || ds < 0.99) {
        int lw = (int)(c->outWidth / ds + 0.5);
        int lh = (int)(c->outHeight / ds + 0.5);
        if (lw >= 1 && lh >= 1) {
            HDC screenDC = GetDC(NULL);
            HDC srcDC = CreateCompatibleDC(screenDC);
            HDC dstDC = CreateCompatibleDC(screenDC);
            HBITMAP scaledBmp = CreateCompatibleBitmap(screenDC, lw, lh);
            HGDIOBJ oldSrc = SelectObject(srcDC, stitched);
            HGDIOBJ oldDst = SelectObject(dstDC, scaledBmp);
            SetHalftoneStretchMode(dstDC);
            StretchBlt(dstDC, 0, 0, lw, lh, srcDC, 0, 0, c->outWidth, c->outHeight, SRCCOPY);
            SelectObject(srcDC, oldSrc);
            SelectObject(dstDC, oldDst);
            DeleteDC(srcDC);
            DeleteDC(dstDC);
            ReleaseDC(NULL, screenDC);
            DeleteObject(stitched);
            c->outWidth = lw;
            c->outHeight = lh;
            return scaledBmp;
        }
    }
    return stitched;
}

