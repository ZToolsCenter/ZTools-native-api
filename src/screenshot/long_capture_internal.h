// 长截图子系统内部头：LongCaptureContext 及其子结构、子系统内共享的常量与
// 跨文件函数声明（供 lc_match_core / lc_stitch_state / lc_frame_io / lc_panel_ui /
// lc_toolbar_ui / lc_session 六个拆分块共用，不对外暴露）。
//
// 本头是 long_capture_windows.cpp 二次拆分（CR-021）的产物：原 LongCaptureContext
// ~160 字段 God-struct 按「拼接累计状态 / 裁剪状态 / 视觉跟踪状态 / 工具栏 UI 状态」
// 四职责拆为 StitchState / CropState / TrackState / LcUiState 子结构；会话级配置、
// 屏幕采样参数、控制信号与输出结果保留为 LongCaptureContext 的直接成员。
// 拆分为纯移动不改逻辑：子结构仅按字段原用途分组，所有字段类型/默认值/语义完全不变，
// 跨文件函数原 static 需跨块共享者改为非 static 并在此声明。
//
// 包含约定：本头依赖 internal.h 已定义的 LongMatchData / LongMatchOutcome /
// LongCapturePendingMatch / LongCaptureFrameHistory / LCMenuKind / CaptureContext 等
// 类型与全局声明，故各拆分块应先包含 internal.h 再包含本头。

#pragma once

#include "internal.h"

// ==================== 长截图子系统内共享类型（原 .cpp 文件内 static struct，跨块共享者）====================

// 轻量位移先验（滚轮累计增量 + 在线 px/notch 估计，仅用于候选排序加分与 tentative 预测幅度）。
struct LongCaptureOffsetPrior {
    bool valid = false;
    int expectedAbsOffset = 0;    // 期望 |d|（px）；仅用于排序加分与 tentative 预测幅度
};

// Weak 时间一致性上下文（短时序列共识，只给弱档置信度加分，不改任何验收门槛）。
struct LCWeakTemporal {
    bool active = false;
    int refOffset = 0;     // 最近弱候选簇中位数（带符号）
    float bonus = 0.0f;    // 共识加分（≤ LC_WEAK_TEMPORAL_BONUS_MAX）
};

// 全宽重叠区富验证证据：overall/seam/三段分布/连续段/profile/edge/纹理占比/动态屏蔽行数。
struct LCOverlapEvidence {
    bool valid = false;
    float overall = 0.0f;                // 全区加权匹配率
    float seam = 0.0f;                   // 接缝窗加权匹配率
    float part[3] = {0.0f, 0.0f, 0.0f};  // top/middle/bottom 匹配率（-1 = 该段无有效行）
    float spatial = 0.0f;                // 空间一致性：多段有证据取「最差段与均值折中」，单段打折
    float continuity = 0.0f;             // 连续性 = 0.6×最长连续段 + 0.4×匹配率 − 断点罚
    float longestRunRatio = 0.0f;        // 最长连续匹配段占有效行比例
    int gapCount = 0;                    // 匹配→失配跳变次数（匹配分布的碎片度）
    float profileScore = 0.0f;           // 4/8 行聚合 profile 相关度均值
    float edgeScore = 0.0f;              // 行边缘结构强度相关度
    float textureRatio = 0.0f;           // 重叠区有效纹理行占比（未屏蔽有效行 / 重叠行数）：
                                         // 大面积纯色重叠的证据量计价，进入综合置信度
    int dynamicMaskedRows = 0;           // 被识别为动态变化区而从统计中剔除的行数
                                         //（受单段行数与总量占比双重硬上限，见常量块）
};

// 单 ROI 候选（Top-N 证据之一）：只携带该列区域在此位移处的整段匹配率。
struct LCBandCandidate {
    int d = 0;               // 候选位移（含符号）
    float overall = 0.0f;    // 该 ROI 限定列区间的整段加权匹配率
    bool strict = false;     // 是否达到严格档验收（否则为宽松档）
};

// 多跳匹配恢复结果：matched 为真时 offset 是基准帧 H → 当前帧的已验证位移。
struct LCMultihopResult {
    bool matched = false;
    bool hopCommitted = false;                    // 基准为已提交帧（位置精确，可推导提交位移）
    LongCaptureMatchMode mode = LongCaptureMatchMode::Normal;
    int offset = 0;                               // 已验证的 H→curr 位移（带符号）
    float confidence = 0.0f;                      // 该匹配的综合置信度
    int64_t hopContentY = 0;                      // 基准帧视口顶（内容坐标）
    int hopFrameId = -1;                          // 基准帧采样序号（日志关联）
};

// 失败帧的 TrackingEstimate（只服务 tentative 跟踪与下一帧先验，绝不进入提交）。
struct LongCaptureTrackingEstimate {
    bool valid = false;
    int direction = 0;                // +1 向下滚 / -1 向上滚
    double predictedOffset = 0.0;     // 预计 |位移|（px）
    double confidence = 0.0;          // 低置信度（纯预测无视觉验证）
};

// ==================== 长截图子系统内共享常量（原 long_capture_windows.cpp 文件级 static）====================
// 这些常量原本为单文件 static，拆分后需跨 lc_* 块共享，改为非 static 在此集中声明，
// 定义保留在对应块（算法调参 → lc_match_core.cpp）。

// 全位移搜索的采样列上限与单列容忍差（量化灰度容差行匹配）。
extern const int LONG_MATCH_MAX_COLS;
extern const int LONG_MATCH_TOL;

// 全位移扫描（探针粗筛→候选峰收集）参数。
extern const int LC_SCAN_PROBES;
extern const int LC_SCAN_MIN_WEIGHT;
extern const int LC_PEAK_WIN;
extern const int LC_MAX_CANDIDATES;

// 最小可信重叠（绝对下限 + 视口高比例）。
extern const int LC_MIN_OVERLAP;
extern const float LC_MIN_OVERLAP_RATIO;

// 跨 ROI 位移一致性容差。
extern const int LC_ROI_OFFSET_TOLERANCE;

// 综合置信度下限与 offset 跳变比率校验参数。
extern const float LC_MIN_CONFIDENCE;
extern const float LC_OFFSET_JUMP_RATIO;
extern const int LC_OFFSET_HISTORY_LEN;
extern const int LC_OFFSET_HISTORY_MIN;

// 采样尝试总数与重试间隔档。
extern const int LC_SAMPLE_ATTEMPTS;
extern const int LC_RETRY_DELAY_NORMAL[];   // 长度 = LC_SAMPLE_ATTEMPTS - 1
extern const int LC_RETRY_DELAY_WEAK[];     // 长度 = LC_WEAK_RETRY_ATTEMPTS

// 帧稳定性检测参数。
extern const DWORD LC_STABLE_REF_MAX_GAP;
extern const float LC_STABLE_CHANGED_ROW_FRAC;
extern const int LC_STABLE_MIN_WEIGHT;
extern const int LC_STABLE_MAX_WAITS;
extern const int LC_STABLE_RETRY_DELAY[];  // 长度 = LC_STABLE_MAX_WAITS

// 瞬态快重采样参数。
extern const int LC_QUICK_RESAMPLES;
extern const int LC_RESAMPLE_DELAY_QUICK[]; // 长度 = LC_QUICK_RESAMPLES

// 滚动中主动采样最大间隔与到底确认采样数。
extern const int LC_SCROLL_SAMPLE_MAX_GAP;
extern const int LC_BOTTOM_CONFIRM_SAMPLES;

// Weak（低重叠大跳变）匹配参数。
extern const int LC_WEAK_MIN_OVERLAP;
extern const float LC_WEAK_MIN_OVERLAP_RATIO;
extern const int LC_WEAK_ROI_TOLERANCE;
extern const float LC_WEAK_ACCEPT_OVERALL;
extern const float LC_WEAK_ACCEPT_SEAM;
extern const float LC_WEAK_ACCEPT_EDGE;
extern const float LC_WEAK_MIN_CONFIDENCE;
extern const float LC_WEAK_CONF_EXTRA_PENALTY;
extern const float LC_WEAK_OVERLAP_RATIO_LIMIT;
extern const int LC_WEAK_CONFIRM_OFFSET_TOL;
extern const int LC_WEAK_RETRY_ATTEMPTS;
extern const int LC_WEAK_MAX_TRIES;

// 滚轮 delta 软先验加分。
extern const float LC_WHEEL_PRIOR_BONUS;

// Tentative 视觉跟踪 / 多跳恢复 / Weak 时间一致性参数。
extern const int LC_HISTORY_FRAMES;
extern const int LC_HISTORY_HOPS;
extern const int LC_TRACK_FREEZE_FRAMES;
extern const int LC_TRACK_MIN_STEP;
extern const float LC_TRACK_PREDICT_CONFIDENCE;
extern const float LC_TRACK_CONFIDENCE_DECAY;
extern const int LC_WEAK_TEMPORAL_TOL;
extern const int LC_WEAK_TEMPORAL_MIN_SAMPLES;
extern const int LC_WEAK_TEMPORAL_MAX_HISTORY;
extern const float LC_WEAK_TEMPORAL_BONUS_MAX;

// 接缝窗行数与全量验证有效权重下限。
extern const int LC_SEAM_ROWS;
extern const int LC_VERIFY_MIN_WEIGHT;

// 纹理置信度折扣参数。
extern const float LC_TEXTURE_CONF_FLOOR;
extern const float LC_TEXTURE_CONF_SCALE;

// 验收阈值（严格档 / 宽松档）。
extern const float LC_ACCEPT_STRICT_OVERALL;
extern const float LC_ACCEPT_STRICT_SEAM;
extern const float LC_ACCEPT_LOOSE_OVERALL;
extern const float LC_ACCEPT_LOOSE_SEAM;

// 方向罚与歧义裕度。
extern const float LC_DIR_PENALTY;
extern const float LC_AMBIGUITY_MARGIN;

// ROI 证据融合参数。
extern const int LC_BAND_TOP_N;
extern const int LC_MAX_VERIFY_CANDIDATES;
extern const float LC_ROI_MIN_INFO_WEIGHT;
extern const int LC_BAND_MIN_COLS;
extern const float LC_PEAK_SEP_FULL;
extern const float LC_AMBIGUITY_CONF_PENALTY;
extern const float LC_CONTINUITY_GAP_MAX;

// 动态变化区局部降权参数。
extern const int LC_DYNAMIC_MIN_ROWS;
extern const int LC_DYNAMIC_MAX_ROWS;
extern const int LC_DYNAMIC_BRIDGE;
extern const float LC_DYNAMIC_MAX_FRACTION;

// offset basin 合并半径。
extern const int LC_BASIN_RADIUS;

// Weak 档额外证据下限。
extern const float LC_WEAK_ACCEPT_SPATIAL;
extern const float LC_WEAK_ACCEPT_CONTINUITY;
extern const float LC_WEAK_ACCEPT_PROFILE;
extern const float LC_WEAK_MIN_PEAK_SEP;

// 跨文件的常量：原定义归属不同块但被另一块使用，故在此声明为 extern。
// LC_TRACK_MIN_STEP 定义于 lc_stitch_state.cpp（跟踪逻辑用），panel UI 也读它判定 tentative 框显示。
extern const int LC_TRACK_MIN_STEP;
// LC_AUTOSCROLL_STOP_FAILS 定义于 lc_toolbar_ui.cpp（自动滚动参数），session 主循环读它决定自动停止。
extern const int LC_AUTOSCROLL_STOP_FAILS;

// ==================== 二级常量（原文件级 static，归各使用块）====================

// 蒙版样式（预乘 ARGB）：整屏半透明灰，采样裁剪区整透明透出实况桌面。
// 定义于 lc_panel_ui.cpp（蒙版绘制唯一使用方）。
extern const int LONG_MASK_GRAY;
extern const BYTE LONG_MASK_ALPHA;

// 面板布局常量（逻辑像素）：面板总宽与内边距被 toolbar（缩略图列宽）与 session（缩略图列宽）
// 共用，故在此声明为 extern；定义于 lc_panel_ui.cpp。
extern const int LC_PANEL_W;
extern const int LC_PANEL_PAD;

// 拼接总像素上限（超过即自动完成，防内存雪崩）。定义于 lc_session.cpp。
extern const long long LONG_CAPTURE_MAX_PIXELS;

// ==================== 长截图子系统内跨文件函数声明 ====================

// 失败分类名（LCFailReason → 日志字符串，仅可观测性用）。定义于 lc_match_core.cpp。
const char* LcFailReasonName(LCFailReason r);

// —— 纯算法（lc_match_core.cpp）——
void LongCaptureBuildMatchData(const std::vector<uint32_t>& frame, int w, int h, LongMatchData& m);
bool LongCaptureRowMatchesRange(const uint8_t* a, const uint8_t* b, int c0, int c1);
bool LongCaptureRowMatches(const uint8_t* a, const uint8_t* b, int cols);
LongMatchOutcome LongCaptureDetectMatch(const LongMatchData& prevM, const LongMatchData& currM,
                                        int dir, const LongCaptureOffsetPrior& prior,
                                        const LCWeakTemporal& wt, int logId);
LongMatchOutcome LongCaptureDetectPass(const LongMatchData& prevM, const LongMatchData& currM,
                                       int dir, const LongCaptureOffsetPrior& prior, bool weak,
                                       const LCWeakTemporal& wt, int logId);
bool LongCaptureVerifyCandidate(const LongMatchData& prevM, const LongMatchData& currM,
                                int d, LCOverlapEvidence& ev);
float LongCaptureWeakRequiredConfidence(int viewportH, int overlap);

// —— 可写累计状态层（lc_stitch_state.cpp）——
LongCaptureOffsetPrior LongCaptureBuildOffsetPrior(const LongCaptureContext* c);
LCWeakTemporal LongCaptureWeakTemporalContext(const LongCaptureContext* c);
void LongCapturePushWeakCandidate(LongCaptureContext* c, int offset);
bool LongCaptureOffsetPlausible(const LongCaptureContext* c, int d,
                                LongCaptureMatchMode mode = LongCaptureMatchMode::Normal);
LCMultihopResult LongCaptureMultihopDetect(const LongCaptureContext* c,
                                           const LongMatchData& currMatch, int dir,
                                           const LongCaptureOffsetPrior& prior);
LongCaptureTrackingEstimate LongCaptureBuildTrackingEstimate(const LongCaptureContext* c, int dir,
                                                              const LongCaptureOffsetPrior& prior);
int64_t LongCaptureTrackingDriftLimit(const LongCaptureContext* c);
void LongCaptureTrackingSetVisual(LongCaptureContext* c, int64_t contentY, float confidence);
void LongCaptureTrackingResetToCommitted(LongCaptureContext* c);
void LongCaptureTrackingAdvancePredicted(LongCaptureContext* c, int direction, double magnitude);
void LongCaptureHistoryPush(LongCaptureContext* c, int frameId, LongMatchData&& match,
                             int64_t contentY, bool committed);
void LongCaptureAfterCommit(LongCaptureContext* c, int frameId);
int LongCaptureCommitStitch(LongCaptureContext* c, std::vector<uint32_t>& curr,
                            LongMatchData& currMatch, int d);
void LongCaptureUpdateWheelEstimate(LongCaptureContext* c, int d);
bool LongCaptureFrameUnstable(LongCaptureContext* c, const LongMatchData& curr);
void LongCaptureUpdateStabilityRef(LongCaptureContext* c, const LongMatchData& curr);
bool LongCaptureHasCropConstraint(const LongCaptureContext* c);
void LongCaptureEraseDisplayRows(LongCaptureContext* c, int64_t r0, int64_t r1);
void LongCaptureExecuteCropPurge(LongCaptureContext* c, bool below);
// LongCaptureTryStitch 已在 internal.h 声明（供单元测试共用），此处不重复。

// —— 抓帧 / DIB / 缩略图 / 位图构建 / 消息泵（lc_frame_io.cpp）——
void LongCaptureDownscaleRow(const uint32_t* src, uint32_t* dst, int srcW, int dstW);
void LongCaptureOutputRows(const LongCaptureContext* c, int& outTop, int& outBottom);
bool LongCaptureRegisterWheelObserver(HWND target);
void LongCaptureUnregisterWheelObserver();
bool LongCaptureEnsureDib(LongCaptureContext* c, HDC screenDC);
bool LongCaptureCaptureFrameBuf(LongCaptureContext* c, std::vector<uint32_t>& out);
void LongCaptureRebuildThumb(LongCaptureContext* c);
void LongCaptureRebuildThumbDisplay(LongCaptureContext* c);
HBITMAP LongCaptureBuildResultBitmap(LongCaptureContext* c);
void LongCapturePumpMessages(LongCaptureContext* c);
bool LongCaptureInitFirstFrame(LongCaptureContext* c, std::vector<uint32_t>& frameBuf);
void LongCaptureInitBaseline(LongCaptureContext* c, std::vector<uint32_t>& frame);
HBITMAP LongCaptureBuildFinalBitmap(LongCaptureContext* c);

// —— 面板 / 蒙版 UI（lc_panel_ui.cpp）——
bool EnsureArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h, int wantW, int wantH);
void FreeArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h);
void LongCapturePanelRender(HWND panel, LongCaptureContext* c);
LRESULT CALLBACK LongCapturePanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void LongCapturePanelUpdate(LongCaptureContext* c);
HWND LongCaptureCreatePanel(CaptureContext* ctx, LongCaptureContext* c);
LRESULT CALLBACK LongCaptureMaskWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
RECT CalcSampleCrop(const CaptureContext* ctx);
void EnterLongCaptureMask(const CaptureContext* ctx);

// —— 工具栏 UI（lc_toolbar_ui.cpp）——
void LongCaptureToolbarRender(LongCaptureContext* c, int dstX, int dstY, int w, int h);
void LongCaptureToolbarRepaint();
LRESULT CALLBACK LongCaptureToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
HWND LongCaptureCreateToolbar(CaptureContext* ctx, LongCaptureContext* c);
void LongCaptureSetMenu(LongCaptureContext* c, LCMenuKind kind);
void LongCaptureResetSession(LongCaptureContext* c);
void LongCaptureSwitchDirection(LongCaptureContext* c);
void LongCaptureSetAutoScroll(LongCaptureContext* c, bool on);
void LongCaptureApplyCrop(LongCaptureContext* c, int row);
// 临时摘除/恢复长截图窗口组的置顶（保存对话框等系统弹窗需要真正置顶）。
// 定义于 lc_toolbar_ui.cpp，session 主循环弹保存对话框前后调用。
void LongCaptureSetTopmost(bool topmost);

// —— 会话主循环 / 生命周期（lc_session.cpp）——
void LongCaptureEmitFailure();
void LongCaptureWaitMessages(LongCaptureContext* c, DWORD ms);
// 释放采样 DIB 段：把原 wndproc_windows.cpp 里手动释放 lc->dibDC/dibBmp 的跨界所有权
// 收进此处，由 DestroyLongCaptureContext 统一释放（CR-021）。
// 注：会话级窗口（panel/toolbar/mask）销毁仍由捕获线程清理段调用，
// 本函数只负责 DIB 资源的归口释放。
void DestroyLongCaptureContext(LongCaptureContext* lc);
