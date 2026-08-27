// 长截图子系统：会话主循环 / 生命周期 / 全局状态。
// CR-021 拆分自 long_capture_windows.cpp 的「主循环 + 生命周期」段。
// 本文件定义长截图会话级全局状态（g_longCtx / 控制窗口句柄 / 参数默认值），
// 手动滚动捕获主循环 RunLongCapture，以及会话初始化 BeginLongCapture、
// JS 线程中止 LongCaptureAbort 与清理入口 DestroyLongCaptureContext。
#include "internal.h"
#include "long_capture_internal.h"

// g_longControlWindow 为侧边小地图面板（完成/取消按钮也在此）。

// g_longCtx 由 BeginLongCapture 创建、WM_LONGCAPTURE_RUN 完成后释放；
// 指针生命周期由 g_longCtxMutex 保护（JS 线程 abort 与捕获线程清理互斥，
// 读写协议见 internal.h），其余读取点均在捕获线程内，天然与释放串行。

// 长截图滚动捕获全局状态（独立于编辑态状态机）。

std::atomic<LongCaptureContext*> g_longCtx(nullptr);

HWND g_longControlWindow = NULL; // 侧边小地图预览面板（滚轮观察的 Raw Input 目标）

HWND g_longToolbarWindow = NULL; // 选区底部工具栏（宽高/方向/自动滚动/裁剪/保存/取消/完成并复制）

HWND g_longMaskWindow = NULL;    // 长截图全屏灰色蒙版（半透明 + 点击穿透）

// 长截图参数（初值即默认值；每次 start() 会先重置为默认再按 JS 覆盖，避免跨会话粘滞）：
int g_lcMaxFrames = 100;         // 默认最大拼接帧数（1~200，start() 的 longCapture.maxFrames 可配；
                                 // 滚动中主动高频采样使帧数消耗更快，默认值随之调高）

int g_lcInterval = 250;          // 默认滚轮防抖间隔 ms（50~2000，start() 的 longCapture.interval 可配）


// 拼接总像素上限：超过即自动完成，防止超大选区/超多帧下内存与编码雪崩（约 800MB BGRA）

const long long LONG_CAPTURE_MAX_PIXELS = 200000000LL;

// 构造失败结果并经截图会话回调回传 JS（success=false）。
// 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放防泄漏。

void LongCaptureEmitFailure() {
    EmitScreenshotResult(false);
}


// 实况桌面）→ PostMessage 让覆盖层窗口过程执行 RunLongCapture（手动滚动捕获主循环）。

// 覆盖层进入 CS_LongCapturing（黑色遮罩 + 选区框 + 底部工具栏，甜甜圈区域使选区内直通

// 由工具栏「长截图」按钮触发：快照选区 → 构造长截图上下文 → 创建侧边小地图面板 →

void BeginLongCapture(CaptureContext* ctx, HWND overlayHwnd) {
    LongCaptureContext* lc = new LongCaptureContext();
    lc->maxFrames = g_lcMaxFrames;
    lc->interval = g_lcInterval;
    lc->vx = ctx->virtualX; lc->vy = ctx->virtualY;
    lc->vw = ctx->virtualW; lc->vh = ctx->virtualH;
    lc->dpiScale = ctx->dpiScale;
    lc->selection = ctx->selection;

    // 采样裁剪 = 选区每边内缩（公式收口见 CalcSampleCrop 注释），换算物理像素（与 ExtractRegionResult 同式）
    lc->cropRect = CalcSampleCrop(ctx);
    RECT crop = lc->cropRect;
    int lx = crop.left - ctx->virtualX;
    int ly = crop.top - ctx->virtualY;
    double ds = ctx->dpiScale;
    lc->physX = (int)(lx * ds + 0.5);
    lc->physY = (int)(ly * ds + 0.5);
    // 屏幕采样 DIB 尺寸（未转置）；帧缓冲尺寸 physW/physH 在横向模式下为其转置
    //（进入时恒为纵向，方向切换经工具栏的 LongCaptureSwitchDirection 处理）
    lc->capW = (int)((crop.right - crop.left) * ds + 0.5);
    lc->capH = (int)((crop.bottom - crop.top) * ds + 0.5);
    if (lc->capW < 1 || lc->capH < 1) { delete lc; return; }   // 选区过小
    lc->physW = lc->horizontal ? lc->capH : lc->capW;
    lc->physH = lc->horizontal ? lc->capW : lc->capH;

    // 虚拟屏幕物理原点（屏幕 DC 坐标）：直接区域采样时作为 BitBlt 源偏移
    MonitorEnumData med = { INT_MAX, INT_MAX, INT_MIN, INT_MIN, 1.0, 0 };
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&med));
    lc->physOriginX = med.minLeft;
    lc->physOriginY = med.minTop;
    if (med.monitorCount == 0) {
        lc->physOriginX = (int)(lc->vx * ds);
        lc->physOriginY = (int)(lc->vy * ds);
    }
    // 面板两级缩略图固定列宽 = 面板预览内宽（物理像素），不超过选区宽度
    int previewPx = (int)((LC_PANEL_W - 2 * LC_PANEL_PAD) * ds + 0.5);
    lc->thumbW = (std::min)((std::max)(1, previewPx), lc->physW);

    g_longCtx.store(lc);

    // 覆盖层进入长截图态：隐藏原覆盖层（甜甜圈区域设计不再使用），
    // 由独立全屏灰蒙版接管显示与输入（半透明、点击穿透）。
    ctx->state = CS_LongCapturing;
    ShowWindow(overlayHwnd, SW_HIDE);
    EnterLongCaptureMask(ctx);

    // 侧边小地图面板（只读预览）+ 选区底部工具栏（方向/自动滚动/裁剪/保存/取消/完成）。
    // 后创建保证两者位于灰蒙版之上（蒙版点击穿透不影响其交互）。
    g_longControlWindow = LongCaptureCreatePanel(ctx, lc);
    g_longToolbarWindow = LongCaptureCreateToolbar(ctx, lc);

    // 由覆盖层窗口过程执行手动滚动捕获主循环（结束后统一清理会话）
    PostMessage(overlayHwnd, WM_LONGCAPTURE_RUN, 0, 0);
}


// 有界等待（采样重试间隔用）：持续泵消息保持面板/工具栏绘制与按钮响应，
// 超时或用户中止/完成/保存信号到达即返回，绝不无限阻塞。

void LongCaptureWaitMessages(LongCaptureContext* c, DWORD ms) {
    DWORD until = GetTickCount() + ms;
    while (GetTickCount() < until) {
        if (c->abortFlag.load() || c->finishFlag.load() || c->saveFlag.load()) return;
        LongCapturePumpMessages(c);
        if (c->abortFlag.load() || c->finishFlag.load() || c->saveFlag.load()) return;
        MsgWaitForMultipleObjectsEx(0, NULL, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}


// 返回 true 表示成功产出（已发回调），false 表示失败/取消（已发回调）。

// 滚轮停稳后采样一视口帧 → 识别/校验/提交（LongCaptureTryStitch）→ 面板实时刷新。

// 手动滚动捕获主循环：安装滚轮观察钩子 → 泵消息（面板/按钮响应）→

bool RunLongCapture(LongCaptureContext* c) {
    // 第 0 帧：进入长截图时选区内的当前内容即首屏（既作主体段也作重叠检测基准）
    std::vector<uint32_t> frameBuf;   // 单帧读取缓冲（跨迭代复用，避免每帧分配）
    if (!LongCaptureInitFirstFrame(c, frameBuf)) { LongCaptureEmitFailure(); return false; }
    c->frameCount.store(1);
    c->lastSampleTick = GetTickCount();   // 滚动中主动采样节拍从首帧起算
    LongCapturePanelUpdate(c);
    LongCaptureToolbarRepaint();   // 初始宽×高标签

    // 注册滚轮观察（Raw Input INPUTSINK → 面板 WM_INPUT）：滚动照常送达选区下的目标窗口，
    // 这里只被动接收广播解析滚轮方向与时机；不安装 WH_MOUSE_LL 之类的全局同步钩子，
    // 避免钩子回调依赖本线程泵消息而拖慢全系统鼠标输入。
    if (!LongCaptureRegisterWheelObserver(g_longControlWindow)) {
        LongCaptureEmitFailure();
        return false;
    }

    bool savedToFile = false;   // 「保存到本地」已成功落盘（回调成功但不进剪贴板）

    while (true) {
        if (c->abortFlag.load()) break;
        if (c->finishFlag.load()) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { c->abortFlag.store(true); break; }

        // 保存到本地：主循环内处理（取消保存对话框则回到捕获继续）。确认路径时单次
        // 编码同时产出 PNG 文件与 base64，直接走成功回调（不进剪贴板）。
        if (c->saveFlag.load()) {
            c->saveFlag.store(false);
            LongCaptureSetAutoScroll(c, false);   // 模态对话框期间不得继续注入滚轮
            LongCaptureSetMenu(c, LCM_None);      // 收起展开中的二级菜单
            LongCaptureSetTopmost(false);         // 系统保存对话框需要真正置顶
            std::wstring path = PromptSaveFilePath(NULL);
            LongCaptureSetTopmost(true);
            if (!path.empty()) {
                HBITMAP bmp = LongCaptureBuildFinalBitmap(c);
                if (bmp) {
                    std::string b64;
                    bool ok = EncodeHBitmapPng(bmp, &b64, nullptr, path.c_str());
                    DeleteObject(bmp);
                    if (ok) {
                        c->base64 = b64;
                        savedToFile = true;
                        break;
                    }
                }
            }
        }

        // 泵消息：滚轮观察（面板 WM_INPUT）、面板绘制与按钮点击均依赖本线程消息循环
        LongCapturePumpMessages(c);
        if (c->abortFlag.load()) break;

        // 触发一次采样轮（三条件任一满足；采样 = 抓取 → 识别 → 校验 →（Weak 档延迟
        // 确认）→（全部通过才）提交，见 LongCaptureTryStitch）：
        //   · 停稳采样：滚轮 interval 内无新事件——捕获滚动尾段，消费 wheelPending；
        //   · 滚动中主动采样：滚动尚未停稳、但距上次采样已达节拍（min(interval,
        //     LC_SCROLL_SAMPLE_MAX_GAP)）即不等停稳主动抓帧拼接。相邻帧位移因此足够
        //     小、overlap 稳定落在 Normal 档，重叠判定成功率从源头提高；不消费
        //     wheelPending，停稳后仍补一次收尾采样；
        //   · Weak 待复核：存在 pendingMatch 时即使没有新滚轮也继续采样（大跳变后的
        //     平滑滚动/懒加载往往要跨多次采样才能稳定复现候选，由 weakTries 预算
        //     保证该自主采样有界）。
        // 轮内重试（语义 =「等待页面稳定」，绝不放宽匹配条件）：
        //   · 稳定性闸门：抓帧后先做轻量稳定性检测（TryStitch ⓪），页面仍在快速变化
        //     时本帧不进入正式匹配，按 LC_STABLE_RETRY_DELAY（30~60ms）短延迟重新采样，
        //     预算 LC_STABLE_MAX_WAITS 耗尽后闸门关闭、强制进入正式匹配——手动连续
        //     滚动下的中滚采样节拍不受阻塞；自动滚动开启时持续注入使页面恒处微滚动
        //     状态，闸门注定不放行、只会白耗每轮重试预算（最终仍是被强制匹配），直接跳过；
        //   · 瞬态快重采样：Normal 失败且分类为瞬态（无候选/全宽不可评，多为抓在滚动/
        //     重绘过渡帧）时先走 LC_RESAMPLE_DELAY_QUICK（40~60ms）短延迟重采样，
        //     预算独立、不提交、不改任何累计状态、不降低任何阈值；
        //   · 常规重试：Normal 失败/内容未变按 LC_SAMPLE_ATTEMPTS 次预算、间隔 90~210ms
        //     递增等待渲染稳定；Weak 候选走独立 6 次重试 + 更长间隔。
        // 重试耗尽仍硬失败 → 本轮放弃：不拼接、不污染任何累计状态、也绝不自动完成——
        // 保留已拼接的有效内容，等用户继续滚动（后续采样仍从干净基准恢复）或手动结束。
        // 采样频次由 interval 与滚动节拍共同限频（默认 ~4 次/秒），每次成功拼接立即刷新面板。
        DWORD now = GetTickCount();
        DWORD scrollGap = c->interval < LC_SCROLL_SAMPLE_MAX_GAP
                        ? (DWORD)c->interval : (DWORD)LC_SCROLL_SAMPLE_MAX_GAP;
        bool settleSample = c->wheelPending && now - c->lastWheelTick >= (DWORD)c->interval;
        bool midScrollSample = c->wheelPending && !settleSample
                            && now - c->lastSampleTick >= scrollGap;
        if (settleSample || midScrollSample || c->pendingMatch.valid) {
            if (settleSample) c->wheelPending = false;
            int trackRevBefore = c->trackingRevision;   // 本轮 tentative 是否变化的观察点
            LCSampleOutcome oc = LCSampleOutcome::Failed;
            int normalTries = 0, weakRetries = 0;
            int stableWaits = 0, quickResamples = 0;    // 稳定性等待 / 瞬态快重采样预算
            while (true) {
                if (c->abortFlag.load() || c->finishFlag.load()) break;
                if (LongCaptureCaptureFrameBuf(c, frameBuf))
                    // 稳定性闸门在等待预算内启用（预算耗尽后强制进入正式匹配，
                    // 保证中滚采样节拍不被阻塞）；自动滚动持续注入时页面恒处微滚动，
                    // 闸门必然不放行，直接跳过以免白耗重试预算
                    oc = LongCaptureTryStitch(c, frameBuf, c->lastDir,
                                              !c->autoScroll && stableWaits < LC_STABLE_MAX_WAITS);
                // 已提交（新增行）或已重定位（回滚到已捕获范围内）：本次采样即告结束，
                // 重试只会对着新基准匹配到相同内容，没有意义
                if (oc == LCSampleOutcome::Stitched
                    || oc == LCSampleOutcome::Repositioned) break;
                if (oc == LCSampleOutcome::WeakPending || oc == LCSampleOutcome::WeakRejected) {
                    // Weak 候选：延迟确认需要更多次稳定采样，间隔走 weak 档
                    if (weakRetries >= LC_WEAK_RETRY_ATTEMPTS) break;
                    LongCaptureWaitMessages(c, LC_RETRY_DELAY_WEAK[weakRetries++]);
                } else if (oc == LCSampleOutcome::Unstable) {
                    // 稳定性未过：短延迟后重新采样（等待页面稳定），预算独立于
                    // Normal/Weak 重试梯，也不消耗它们
                    if (stableWaits >= LC_STABLE_MAX_WAITS) break;
                    LongCaptureWaitMessages(c, LC_STABLE_RETRY_DELAY[stableWaits++]);
                } else if (oc == LCSampleOutcome::Failed && quickResamples < LC_QUICK_RESAMPLES
                           && (c->lastFailReason == LCFailReason::NoCandidate
                               || c->lastFailReason == LCFailReason::GlobalMismatch)) {
                    // 瞬态失败（无候选/全宽不可评——多为抓在滚动/重绘过渡帧）：几十毫秒级
                    // 快重采样等页面稳定，而不是放宽匹配条件；不消耗主重试预算
                    LongCaptureWaitMessages(c, LC_RESAMPLE_DELAY_QUICK[quickResamples++]);
                } else {
                    // Normal 失败或内容未变：等待渲染稳定后重试（现有采样预算）
                    if (normalTries >= LC_SAMPLE_ATTEMPTS - 1) break;
                    LongCaptureWaitMessages(c, LC_RETRY_DELAY_NORMAL[normalTries++]);
                }
            }
            c->lastSampleTick = GetTickCount();   // 滚动中主动采样节拍以采样轮结束时刻起算
            if (oc == LCSampleOutcome::Stitched) {
                c->noChangeCount = 0;
                c->weakTries = 0;
                c->frameCount.fetch_add(1);
                LongCapturePanelUpdate(c);
                LongCaptureToolbarRepaint();   // 宽×高标签刷新
                if (c->frameCount.load() >= c->maxFrames) break;   // 帧数上限：自动完成
                if ((long long)c->physW * c->stitchH >= LONG_CAPTURE_MAX_PIXELS)
                    break;                                         // 内存上限：自动完成
            } else if (oc == LCSampleOutcome::Repositioned) {
                // 反向回滚未越出已捕获边界：视口基准已确认推进（小地图当前区域标注随
                // committedContentTop 移动），无新增行——不计帧数、不重算面板尺寸；
                // 面板重绘由下方 trackingRevision 变化判定触发（CommitStitch 内自增）。
                c->noChangeCount = 0;
                c->weakTries = 0;
            } else if (oc == LCSampleOutcome::NoChange) {
                // 内容未变化（匹配成功且 d=0）：与“匹配失败”完全独立的事件。
                // 连续多次未变化才确认滚动到底；匹配失败绝不计入该计数。
                c->weakTries = 0;
                if (++c->noChangeCount >= LC_BOTTOM_CONFIRM_SAMPLES) c->reachedBottom = true;
            } else if (oc == LCSampleOutcome::WeakPending || oc == LCSampleOutcome::WeakRejected) {
                // 「发现了候选，但 Weak 验证/复核不足」：与“完全没有候选”是两种失败，
                // 不计入任何终止条件；独立预算（LC_WEAK_MAX_TRIES 轮）耗尽即放弃当前
                // 候选链，从干净基准重新观察。
                c->noChangeCount = 0;
                if (++c->weakTries >= LC_WEAK_MAX_TRIES) {
                    c->weakTries = 0;
                    c->pendingMatch.valid = false;   // 放弃当前候选链，从干净基准重新观察
                    c->weakCandidateOffsets.clear(); // 候选链作废：时间一致性样本一并过期
                }
            } else {
                // 硬失败（重试耗尽仍无可信对齐）：不拼接、不污染基准、也绝不自动完成
                // ——会话由用户经「完成/取消/ESC」结束，基准未被污染，后续采样仍可
                // 从当前基准继续拼接。
                c->noChangeCount = 0;
            }
            // 自动滚动的自动停止：确认滚动到底，或连续多次硬失败（内容可能已滚出可
            // 匹配范围，继续注入滚轮只会丢内容）时停止注入——会话保留，由用户处置。
            if (c->autoScroll) {
                if (oc == LCSampleOutcome::Failed) {
                    if (++c->autoFailStreak >= LC_AUTOSCROLL_STOP_FAILS)
                        LongCaptureSetAutoScroll(c, false);
                } else if (oc != LCSampleOutcome::Unstable && oc != LCSampleOutcome::WeakPending) {
                    c->autoFailStreak = 0;
                }
                if (c->reachedBottom) LongCaptureSetAutoScroll(c, false);
            }
            // tentative 跟踪状态在本轮发生变化（多跳恢复/预测推进/候选否决回退）：
            // 小地图按「预计位置」刷新——正式拼接未变，不触发面板尺寸重算
            //（LongCapturePanelUpdate 仍只在 Stitched 后调用）。
            if (c->trackingRevision != trackRevBefore && g_longControlWindow)
                InvalidateRect(g_longControlWindow, NULL, FALSE);
        }
        // 等待消息到达或 15ms 超时：有滚轮/绘制/点击消息立即唤醒（替代固定 Sleep，
        // 防抖计时与面板重绘不被固定睡眠拖延）。
        MsgWaitForMultipleObjectsEx(0, NULL, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    LongCaptureUnregisterWheelObserver();

    if (c->abortFlag.load()) { LongCaptureEmitFailure(); return false; }

    // 拼接 + 输出（复用截图输出管线）：
    //   · 完成并复制：拼接结果缩放回逻辑尺寸 → base64 + 剪贴板；
    //   · 保存到本地（savedToFile）：文件已落盘、base64 已生成，直接回调成功（不进剪贴板）。
    bool ok = false;
    if (savedToFile) {
        ok = true;
    } else {
        HBITMAP finalBmp = LongCaptureBuildFinalBitmap(c);
        if (finalBmp) {
            c->base64 = BitmapToBase64Png(finalBmp);
            ok = SaveBitmapToClipboard(finalBmp);
            DeleteObject(finalBmp);
        }
    }
    c->success = ok;

    // 回调（统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放防泄漏；
    // 原代码先 new 后判 TSFN 且不查 nonblocking 返回值，TSFN 为空或队列满时 result 泄漏
    // —— 此处一并修复）。
    if (ok) {
        EmitScreenshotResult(true, c->cropRect.left, c->cropRect.top,
            c->cropRect.right, c->cropRect.bottom, c->outWidth, c->outHeight, c->base64);
    } else {
        EmitScreenshotResult(false);
    }
    return ok;
}


// JS 线程主动中止进行中的长截图。

void LongCaptureAbort() {
    // g_longCtxMutex 保证「load 到指针 → 写 abortFlag」期间对象不被捕获线程
    // 的清理段（WM_LONGCAPTURE_RUN 内 store(nullptr)+delete）释放：两端都以该锁
    // 为边界，读到非空即意味着 delete 尚未发生。锁内只有原子读写，无消息调用。
    std::lock_guard<std::mutex> lock(g_longCtxMutex);
    LongCaptureContext* c = g_longCtx.load();
    if (c) c->abortFlag.store(true);
}


// 释放长截图上下文的采样 DIB 段（dibDC/dibBmp/dibBits）。
// CR-021：原 wndproc_windows.cpp 的 WM_LONGCAPTURE_RUN 清理段手动释放 lc->dibDC/dibBmp
// 的跨界所有权收进此处统一释放——调用方（wndproc 清理段）改为调用本函数，
// 释放语义、顺序、守卫（if (lc->dibDC) DeleteDC ...）与原实现逐字一致。
void DestroyLongCaptureContext(LongCaptureContext* lc) {
    if (!lc) return;
    if (lc->dibDC) DeleteDC(lc->dibDC);
    if (lc->dibBmp) DeleteObject(lc->dibBmp);
}
