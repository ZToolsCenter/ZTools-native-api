// 长截图子系统：可写累计状态层（提交 / 跟踪 / 历史 / Weak 候选 / 裁剪 / 稳定性 / 管线）。
// 拆分自 long_capture_windows.cpp 的「Tentative 跟踪 / 提交阶段 / 单帧管线」段。
// 本文件是唯一允许修改累计拼接状态（body/headRev/stitchH/lastFrame/lastMatch/offsetHistory）
// 的位置；LongCaptureTryStitch 是单帧「识别→校验→提交」管线入口。
// 平台兼容（算法逻辑零改动）：本文件为纯算法，只依赖 long_capture_internal.h
// （跨平台唯一权威，双平台同一份定义）与 lc_platform.h（平台类型别名 /
// WHEEL_DELTA / GetTickCount 等价物，见其头部的依赖剥离验证结论）；
// internal.h（napi / GDI+ 依赖链）仅 Windows UI/IO 层使用，算法层不再触碰。
#include "lc_platform.h"
#include "long_capture_internal.h"

// 可条件编译的调试日志（构建加 /DLC_DEBUG_LOG 启用，经 OutputDebugStringA 输出到调试器）：
// 输出采样管线的关键决策标签与全部评分明细（模式/offset/overlap/各 ROI 候选与权重/
// overall/seam/top-middle-bottom/连续段/profile/edge/纹理占比/峰值分离度/综合置信度/
// pending 状态/失败原因分类/稳定性闸门/动态屏蔽行数/basin 择优），供真实数据驱动的
// 阈值调优；默认编译为零开销空函数。
#ifdef LC_DEBUG_LOG
#include <cstdio>
#include <cstdarg>
static void LC_LOG(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}
#else
static inline void LC_LOG(const char*, ...) {}
#endif

// 由上下文的滚轮累计增量与 px/notch 估计构造位移先验；不可用时退回成功位移中位数。
LongCaptureOffsetPrior LongCaptureBuildOffsetPrior(const LongCaptureContext* c) {
    LongCaptureOffsetPrior p;
    if (c->pixelsPerWheelNotch > 1.0f && c->wheelAccumDelta != 0) {
        int notches = (c->wheelAccumDelta > 0 ? c->wheelAccumDelta : -c->wheelAccumDelta) / WHEEL_DELTA;
        if (notches >= 1) {
            p.expectedAbsOffset = (int)(notches * c->pixelsPerWheelNotch + 0.5f);
            p.valid = p.expectedAbsOffset > 0;
        }
    }
    if (!p.valid && !c->offsetHistory.empty()) {
        std::vector<int> hist = c->offsetHistory;
        std::sort(hist.begin(), hist.end());
        p.expectedAbsOffset = hist[hist.size() / 2];
        p.valid = p.expectedAbsOffset > 0;
    }
    return p;
}

// —— Weak 时间一致性（短时序列共识）——
// 维护最近若干次弱重叠候选位移（直连 lastFrame 的参考系，与 pendingMatch 流一致），
// 多个相邻采样持续指向同一 offset 区间时给弱档置信度加分。铁律：只加分——
//   · 所有空间硬门槛（overall/seam/edge/spatial/continuity/profile/峰值分离度）
//     与动态置信度门槛数值原样不动；
//   · 任何时刻都不会因为「历史上一直是这个值」绕过 DetectPass 验收直接提交。

// 登记一次弱重叠候选位移（LOW_CONFIDENCE 弱候选 / WeakPending 首次候选均计入），
// 环形容量 LC_WEAK_TEMPORAL_MAX_HISTORY；提交成功或候选链作废时整体清空。
void LongCapturePushWeakCandidate(LongCaptureContext* c, int offset) {
    if (offset == 0) return;
    c->weakCandidateOffsets.push_back(offset);
    if ((int)c->weakCandidateOffsets.size() > LC_WEAK_TEMPORAL_MAX_HISTORY)
        c->weakCandidateOffsets.erase(c->weakCandidateOffsets.begin());
}

// 由最近弱候选序列构造时间一致性上下文：同号且与中位数偏差 ≤ LC_WEAK_TEMPORAL_TOL
// 的样本达 LC_WEAK_TEMPORAL_MIN_SAMPLES 个即共识成立；共识强度随样本数增长封顶。
LCWeakTemporal LongCaptureWeakTemporalContext(const LongCaptureContext* c) {
    LCWeakTemporal wt;
    if ((int)c->weakCandidateOffsets.size() < LC_WEAK_TEMPORAL_MIN_SAMPLES) return wt;
    std::vector<int> s = c->weakCandidateOffsets;
    std::sort(s.begin(), s.end());
    int med = s[s.size() / 2];
    int n = 0;
    for (int off : s) {
        int dd = off > med ? off - med : med - off;
        if (dd <= LC_WEAK_TEMPORAL_TOL && (off > 0) == (med > 0)) n++;
    }
    if (n < LC_WEAK_TEMPORAL_MIN_SAMPLES) return wt;
    wt.active = true;
    wt.refOffset = med;
    float ratio = (std::min)(1.0f, (float)n / 4.0f);
    wt.bonus = LC_WEAK_TEMPORAL_BONUS_MAX * ratio;
    return wt;
}

// ==================== Tentative 跟踪 / 最近帧历史 / 多跳恢复（外围状态层） ====================
// 本段全部为「视觉跟踪」基础设施，与正式拼接状态（body/headRev/stitchH/lastFrame/
// lastMatch/offsetHistory）完全隔离：FAILED / LOW_CONFIDENCE 帧只允许触碰这里的
// 跟踪字段与帧历史。正式拼接仍然只有 DetectPass SUCCESS → LongCaptureCommitStitch
// 一条路；多跳恢复产出的提交同样必须先通过与 lastFrame 的单点富验证 + 历史合理性
//（见 LongCaptureTryStitch 失败分流处的四道关卡），绝不放宽任何门槛。

// tentative 相对 committed 的最大漂移（内容坐标 px，2×视口高）：超过即钳制，

// 防止纯预测链在连续失败下无限累加误差。正常一次失败恢复窗口内的漂移远小于该值。
static int64_t LongCaptureTrackingDriftLimit(const LongCaptureContext* c) {
    return (int64_t)c->physH * 2;
}

// 以「视觉验证依据」直接设定 tentative 绝对位置（来源：多跳匹配 / 零位移对齐 /
// 与基准帧逐像素全同）。视觉依据可解除冻结并重置不可靠计数；漂移上限仍生效
//（防御性钳制，正常视觉链不会触达）。只改跟踪字段，绝不触碰拼接状态。
void LongCaptureTrackingSetVisual(LongCaptureContext* c, int64_t contentY, float confidence) {
    int64_t limit = LongCaptureTrackingDriftLimit(c);
    if (contentY < c->committedContentTop - limit) contentY = c->committedContentTop - limit;
    if (contentY > c->committedContentTop + limit) contentY = c->committedContentTop + limit;
    c->tentativeContentTop = contentY;
    c->tentativeValid = true;
    c->tentativeConfidence = confidence;
    c->trackUnreliableStreak = 0;    // 重新获得视觉依据
    c->trackingRevision++;
}

// 丢失跟踪（候选链被否决等）：tentative 回归 committed 基准并计一次不可靠样本。
// 不清除 tentativeValid——位置退回已确认处，后续采样可从干净基准重新建立预计。
void LongCaptureTrackingResetToCommitted(LongCaptureContext* c) {
    c->tentativeContentTop = c->committedContentTop;
    c->trackUnreliableStreak++;
    c->trackingRevision++;
}

// 纯预测推进（无视觉依据）：冻结状态（连续 LC_TRACK_FREEZE_FRAMES 帧无视觉依据）
// 不再推进；预测位移必须在历史位移合理范围内（有历史样本时 = 中位数×跳变比与
// 视口高一半的较大者），明显违反则只计不可靠样本、位置不动——冻结而不是继续漂移。
void LongCaptureTrackingAdvancePredicted(LongCaptureContext* c, int direction, double magnitude) {
    if (!c->tentativeValid || c->trackUnreliableStreak >= LC_TRACK_FREEZE_FRAMES) return;
    if (direction == 0 || magnitude < (double)LC_TRACK_MIN_STEP) return;
    double cap = (double)c->physH * 0.5;
    if ((int)c->offsetHistory.size() >= LC_OFFSET_HISTORY_MIN) {
        std::vector<int> hist = c->offsetHistory;
        std::sort(hist.begin(), hist.end());
        cap = (std::max)(cap, (double)hist[hist.size() / 2] * LC_OFFSET_JUMP_RATIO);
    }
    c->trackUnreliableStreak++;               // 预测帧本身无视觉依据
    if (magnitude > cap) return;              // 违反历史位移范围：冻结（streak 已累计）
    int64_t next = c->tentativeContentTop + (int64_t)(direction * magnitude);
    int64_t limit = LongCaptureTrackingDriftLimit(c);
    if (next > c->committedContentTop + limit) next = c->committedContentTop + limit;
    if (next < c->committedContentTop - limit) next = c->committedContentTop - limit;
    c->tentativeContentTop = next;
    c->tentativeConfidence *= LC_TRACK_CONFIDENCE_DECAY;
    c->trackingRevision++;
}

// 失败帧的 TrackingEstimate（注意：TrackingEstimate != StitchMatch）：只服务 tentative
// 跟踪与下一帧先验，绝不进入 LongCaptureCommitStitch。方向 = 最近滚轮方向；
// 幅度优先取最近成功视觉位移中位数（滚动节奏稳定），无历史样本时退回位移先验。

LongCaptureTrackingEstimate LongCaptureBuildTrackingEstimate(const LongCaptureContext* c, int dir,
                                                                    const LongCaptureOffsetPrior& prior) {
    LongCaptureTrackingEstimate e;
    e.direction = dir;
    if (!c->offsetHistory.empty()) {
        std::vector<int> hist = c->offsetHistory;
        std::sort(hist.begin(), hist.end());
        e.predictedOffset = hist[hist.size() / 2];
    } else if (prior.valid) {
        e.predictedOffset = prior.expectedAbsOffset;
    }
    e.confidence = LC_TRACK_PREDICT_CONFIDENCE;
    e.valid = e.direction != 0 && e.predictedOffset >= (double)LC_TRACK_MIN_STEP;
    return e;
}

// 压入一条最近帧历史（环形容量 LC_HISTORY_FRAMES，超出丢最旧）：match 为该帧完整
// 匹配数据（不保存位图）；contentY 为视口顶内容坐标（已提交=精确，未提交=tentative
// 估计）；detailSum 过低的近空白帧标记 validForMatching=false，多跳回溯直接跳过。
void LongCaptureHistoryPush(LongCaptureContext* c, int frameId, LongMatchData&& match,
                                   int64_t contentY, bool committed) {
    LongCaptureFrameHistory e;
    e.frameId = frameId;
    e.match = std::move(match);
    e.contentY = contentY;
    e.committed = committed;
    e.validForMatching = e.match.detailSum >= 16;
    c->frameHistory.push_back(std::move(e));
    if ((int)c->frameHistory.size() > LC_HISTORY_FRAMES)
        c->frameHistory.erase(c->frameHistory.begin());
}

// 多跳匹配恢复：当前帧与 lastFrame 直连失败后，回溯最近历史帧重建已验证的位移
// 关系——某一次失败不再直接切断跟踪链。基准优先级区分「可靠匹配基准」与「失败帧
// 历史」：先回溯已提交帧（位置精确、未被任何失败路径污染），再退回未提交帧（位置
// 为 tentative 估计）——失败帧仍可参与多跳回溯/tentative 跟踪，但绝不因「最新」就
// 优先于可靠基准，质量差的帧不能成为后续多帧匹配的污染源。总跳数预算不变
//（LC_HISTORY_HOPS，两段共享）。全部复用现有 LongCaptureDetectMatch 验收，不放宽
// 任何门槛；产出的 offset 只用于：
//   · tentative 跟踪推进（视觉依据，可解冻）；
//   · 当且仅当基准为「已提交帧」且 Normal 档强证据时，在 TryStitch 中推导相对
//     lastFrame 的提交位移（四道关卡全过才提交，见调用处）。
// 绝不把中间失败帧插入正式拼接（未提交基准的链只能推进跟踪，数学上无法构造
// 未经验证的提交位移）。
static LCMultihopResult LongCaptureMultihopDetect(const LongCaptureContext* c,
                                                  const LongMatchData& currMatch, int dir,
                                                  const LongCaptureOffsetPrior& prior) {
    LCMultihopResult r;
    if (currMatch.detailSum < 16) return r;      // 近空白帧：无对齐依据，跳过多跳
    int tried = 0;
    for (int pass = 0; pass < 2 && !r.matched; pass++) {
        bool wantCommitted = pass == 0;          // 第一段：已提交帧；第二段：未提交帧
        for (size_t i = c->frameHistory.size(); i-- > 0; ) {
            const LongCaptureFrameHistory& h = c->frameHistory[i];
            if (!h.validForMatching || h.committed != wantCommitted) continue;
            if (h.committed && h.frameId == c->lastCommittedFrameId)
                continue;                        // hop1（lastFrame 直连）已在主路径尝试并失败
            if (++tried > LC_HISTORY_HOPS) return r;
            LongMatchOutcome m = LongCaptureDetectMatch(h.match, currMatch, dir, prior,
                                                        LCWeakTemporal(), h.frameId);
            if (m.status != LC_MATCH_SUCCESS) continue;   // 该基准失败：继续回溯更早帧
            r.matched = true;
            r.hopCommitted = h.committed;
            r.mode = m.mode;
            r.offset = m.offset;
            r.confidence = m.confidence;
            r.hopContentY = h.contentY;
            r.hopFrameId = h.frameId;
            return r;                            // 由近及远，首个成功即止
        }
    }
    return r;
}

// 成功提交后的外围状态收尾（调用点：Normal 提交 / Weak 延迟确认提交 / 多跳推导提交，
// 均在 LongCaptureCommitStitch 之后）：登记已提交历史条目（匹配数据取提交后的
// lastMatch 拷贝，位置精确）、标记最新已提交帧序号、清空弱候选簇（新基准下旧候选失效）。
void LongCaptureAfterCommit(LongCaptureContext* c, int frameId) {
    c->lastCommittedFrameId = frameId;
    LongCaptureHistoryPush(c, frameId, LongMatchData(c->lastMatch), c->committedContentTop, true);
    c->weakCandidateOffsets.clear();
}

// —— offset 合理性（历史趋势）校验：异常跳变直接拒绝 ——
// 为什么必须 reject 而不是“放行让流程继续”：一次误识别的极端 offset 一旦被采纳，
// 会同时污染累计画布、lastFrame 匹配基准与 offset 历史，后续所有帧都基于错误基准
// 匹配，错误滚雪球直至整张长图错位。拒绝时绝不更新任何状态，让下一帧以未受污染的
// 基准重新尝试。滚动节奏正常时相邻采样的位移量级稳定；Normal 档维持
// 「历史成功位移中位数 × LC_OFFSET_JUMP_RATIO」上限，防止误杀之外也防误放。
// Weak 档把跳变上限放宽为 max(历史上限, 视口高×LC_WEAK_OVERLAP_RATIO_LIMIT)：
// 合法的大滚动（最近都滚 120px、用户突然一次 850px）不应被历史中位数误杀；
// 该放宽只对已通过 3/3 ROI + 边缘结构 + 动态高置信度的 Weak 强证据候选开放
//（调用方仅在 DetectPass(weak) SUCCESS 之后才以 WeakOverlap 模式调用），绝不无限放宽。
// 返回 true = 合理（允许提交）；false = 异常（本帧按失败处理，不更新历史）。

bool LongCaptureOffsetPlausible(const LongCaptureContext* c, int d,
                                       LongCaptureMatchMode mode) {
    int ad = d > 0 ? d : -d;
    int h = c->physH;
    int minOv = mode == LongCaptureMatchMode::WeakOverlap
        ? (std::max)(LC_WEAK_MIN_OVERLAP, (int)(h * LC_WEAK_MIN_OVERLAP_RATIO))
        : (std::max)(LC_MIN_OVERLAP, (int)(h * LC_MIN_OVERLAP_RATIO));
    if (ad > h - minOv) return false;            // 重叠不足（防御：扫描范围理论上已排除）
    if ((int)c->offsetHistory.size() >= LC_OFFSET_HISTORY_MIN) {
        std::vector<int> hist = c->offsetHistory;
        std::sort(hist.begin(), hist.end());
        int median = hist[hist.size() / 2];
        int jumpLimit = (int)(median * LC_OFFSET_JUMP_RATIO);
        if (mode == LongCaptureMatchMode::WeakOverlap)
            jumpLimit = (std::max)(jumpLimit, (int)(h * LC_WEAK_OVERLAP_RATIO_LIMIT));
        if (ad > jumpLimit) return false;        // 相对历史异常跳变
    }
    return true;
}

//   无可信对齐（跳变/懒加载）：不拼接、走重试；失败绝不自动完成，何时结束由用户决定。

//           （头部段以倒序存储，拼接恒为 O(新增行)，向上滚动不再整段搬移大缓冲）；

//   向上滚：curr 底部与 lastFrame 顶部重叠 → 前插 curr 去重后的顶部行到 headRev 段

//   向下滚：curr 顶部与 lastFrame 底部重叠 → 追加 curr 去重后的底部行到 body 段；

// 将一次采样的视口帧增量拼接（帧间模糊对齐去相邻帧重叠行），原始段与面板缩略图段同步更新：

// ==================== 提交阶段（唯一允许修改累计拼接状态的位置） ====================

// ==================== 裁剪：内容坐标窗口 / 待剔除区间的延迟执行 ====================

// 是否存在任何生效的裁剪约束（窗口锚点或待剔除区间）；cropped 标志据此维护。
bool LongCaptureHasCropConstraint(const LongCaptureContext* c) {
    return c->cropTopY != INT64_MIN || c->cropBottomY != INT64_MAX
        || c->cropPendTop || c->cropPendBottom;
}

// 物理删除拼接图显示行区间 [r0, r1)（可横跨头部段与主体段），缩略图段同步删除。
// 仅由「待剔除裁剪区间」触发时调用：删除的内容区间与登记时刻完全一致（登记/执行都
// 用内容坐标，头部前插不改变两侧取值），因此 committedContentTop/tentative/
// frameHistory 等按内容坐标记账的字段全部无需平移——被删的只是缓冲里的行，坐标系
// 本身不动。帧历史条目刻意保留：匹配数据始终与真实屏幕对齐，用户折返被裁区域时可
// 作多跳基准把该区域重新拼回（后续内容是否越过旧裁剪线由输出窗口决定，缓冲完整性
// 不再构成约束）。
void LongCaptureEraseDisplayRows(LongCaptureContext* c, int64_t r0, int64_t r1) {
    if (!c || r1 <= r0) return;
    if (r0 < 0) r0 = 0;
    if (r1 > c->stitchH) r1 = c->stitchH;
    if (r1 <= r0) return;
    const size_t rowW = (size_t)c->physW;
    const size_t thumbRowW = (size_t)(std::max)(1, c->thumbW);
    int64_t hr = c->headRows;   // 两段均按删除前的 headRows 映射显示行 ↔ 数组下标
    // 主体段部分（显示行 [max(r0,hr), r1)）。先处理主体：其数组位置与头部删除无关。
    {
        int64_t b0 = (std::max)(r0, hr), b1 = r1;
        if (b1 > b0) {
            int64_t n = b1 - b0;
            size_t off = (size_t)(b0 - hr) * rowW;
            size_t len = (size_t)n * rowW;
            size_t end = (std::min)(c->body.size(), off + len);
            if (off < c->body.size())
                c->body.erase(c->body.begin() + (std::ptrdiff_t)off, c->body.begin() + (std::ptrdiff_t)end);
            c->bodyRows -= (int)n;
            if (c->thumbW > 0 && !c->thumbBody.empty()) {
                size_t toff = (size_t)(b0 - hr) * thumbRowW;
                size_t tend = (std::min)(c->thumbBody.size(),
                                         toff + (size_t)n * thumbRowW);
                if (toff < c->thumbBody.size())
                    c->thumbBody.erase(c->thumbBody.begin() + (std::ptrdiff_t)toff,
                                       c->thumbBody.begin() + (std::ptrdiff_t)tend);
            }
        }
    }
    // 头部段部分（显示行 [r0, min(r1, hr))）：headRev 倒序存头部，显示行 r ↔ 下标
    // (headRows-1-r)，故区间对应下标 [hr-h1, hr-h0) 的连续段；缩略图同构。
    {
        int64_t h0 = r0, h1 = (std::min)(r1, hr);
        if (h1 > h0) {
            int64_t n = h1 - h0;
            size_t off = (size_t)(hr - h1) * rowW;
            size_t len = (size_t)n * rowW;
            size_t end = (std::min)(c->headRev.size(), off + len);
            if (off < c->headRev.size())
                c->headRev.erase(c->headRev.begin() + (std::ptrdiff_t)off,
                                 c->headRev.begin() + (std::ptrdiff_t)end);
            c->headRows -= (int)n;
            if (c->thumbW > 0) {
                // 计数与主缓冲严格同步（在数据守卫之外递减），杜绝空缩略图路径下
                // thumbHeadH 与 headRows 脱钩导致 thumbH 重算漂移
                c->thumbHeadH = (std::max)(0, c->thumbHeadH - (int)n);
                if (!c->thumbHeadRev.empty()) {
                    size_t toff = (size_t)(hr - h1) * thumbRowW;
                    size_t tend = (std::min)(c->thumbHeadRev.size(),
                                             toff + (size_t)n * thumbRowW);
                    if (toff < c->thumbHeadRev.size())
                        c->thumbHeadRev.erase(c->thumbHeadRev.begin() + (std::ptrdiff_t)toff,
                                              c->thumbHeadRev.begin() + (std::ptrdiff_t)tend);
                }
            }
        }
    }
    c->stitchH = c->headRows + c->bodyRows;      // 恒等式重建，杜绝计数漂移
    if (c->thumbW > 0)
        c->thumbH = c->thumbHeadH + (int)(c->thumbBody.size() / thumbRowW);
    c->thumbDirty = true;
}

// 执行一个方向的待剔除区间（「丢弃上方/下方」延迟删除的落地点）：物理删行并把该侧
// 输出边界重新开放——此后的新增内容不再受旧裁剪线阻挡，直接从裁剪线续接拼图。
// 触发时机 = CommitStitch 入口处、位移方向与登记方向一致的成功提交（含未新增行的
// 重定位帧；提前删除只收紧已捕获范围，不影响识别与匹配基准）。
void LongCaptureExecuteCropPurge(LongCaptureContext* c, bool below) {
    if (!c || !c->cropped) return;
    if (below) {
        if (!c->cropPendBottom) return;
        c->cropPendBottom = false;
        // 内容区间 → 显示行区间：拼接图行 = 内容坐标 + headRows（当前值）
        LongCaptureEraseDisplayRows(c, c->cropPendBottomLo + c->headRows,
                                    c->cropPendBottomHi + c->headRows);
        c->cropBottomY = INT64_MAX;              // 下方重新开放（上方锚点不受影响）
    } else {
        if (!c->cropPendTop) return;
        c->cropPendTop = false;
        LongCaptureEraseDisplayRows(c, c->cropPendTopLo + c->headRows,
                                    c->cropPendTopHi + c->headRows);
        c->cropTopY = INT64_MIN;                 // 上方重新开放（下方锚点不受影响）
    }
    c->cropped = LongCaptureHasCropConstraint(c);
}


// 提交一次「已通过识别与 offset 合理性校验」的拼接。前置条件由 LongCaptureTryStitch 保证：
// 匹配 SUCCESS 且 offset 合理。识别/校验阶段绝不触碰 body/headRev/stitchH、lastFrame/lastMatch、
// offsetHistory 中的任何一个——先识别后提交，杜绝“先改状态、再发现匹配失败”的污染路径。
// 新增行数由新视口是否越出「已捕获范围 [-headRows, bodyRows)」（内容坐标）决定，而非直接取 |d|：
//   · d>0 向下滚：仅当新视口底越出 bodyRows 才追加 curr 底部越界行（≤ |d|）到 body；
//   · d<0 向上滚：仅当新视口顶越出 -headRows 才前插 curr 顶部越界行（≤ |d|）到 headRev；
//   · 完全落在已捕获范围内（反向回滚未达捕获边界）：不新增任何行，仅推进当前视口基准，
//     小地图只移动当前区域标注——已捕获内容的重复帧绝不再次拼接；
//   · 越界拼接后已捕获范围随之扩张，即「重新记录初始位置」，反向后再反向按新边界同理判定。
// 无论是否新增行，都旋转匹配基准（lastFrame/lastMatch）、写入 offset 历史并把
// committedContentTop 推进到新视口位置（tentative 同步对齐）。
// 返回本次实际新增的拼接行数（0 = 仅重定位）。

int LongCaptureCommitStitch(LongCaptureContext* c, std::vector<uint32_t>& curr,
                                   LongMatchData& currMatch, int d) {
    int w = c->physW, h = c->physH;
    // —— 裁剪延迟剔除（LongCaptureExecuteCropPurge）：朝被裁方向的成功提交在此触发
    // 真删除；随后本帧 addRows 按删除后的新外沿计算，新增行自然从裁剪线续接，
    // 「裁剪后继续滚动 = 继续从当前位置拼图」由此成立。必须在计算已捕获范围前执行。
    if (d > 0 && c->cropPendBottom)
        LongCaptureExecuteCropPurge(c, true);
    else if (d < 0 && c->cropPendTop)
        LongCaptureExecuteCropPurge(c, false);
    // 新视口顶（内容坐标）与已捕获范围外沿；范围恒包含 lastFrame 整帧
    // [committedContentTop, +h)，故单帧只可能越出一个方向的外沿（由 d 的符号决定）。
    int64_t newTop = c->committedContentTop + d;
    int64_t capTop = -(int64_t)c->headRows;      // 已捕获上外沿（“初始位置”）
    int64_t capBottom = (int64_t)c->bodyRows;    // 已捕获下外沿
    int addRows = 0;
    if (d > 0) {
        int64_t newBottom = newTop + h;
        if (newBottom > capBottom) addRows = (int)(newBottom - capBottom);
    } else if (d < 0) {
        if (newTop < capTop) addRows = (int)(capTop - newTop);
    }
    bool toHead = d < 0;
    const uint32_t* src = d > 0 ? curr.data() + (size_t)(h - addRows) * w : curr.data();

    // 原始拼接段：headRev 尾部追加「新增行块倒序」，body 尾部直接追加（回滚重定位无新增行）
    if (addRows > 0) {
        if (toHead) {
            size_t old = c->headRev.size();
            c->headRev.resize(old + (size_t)addRows * w);
            for (int k = 0; k < addRows; k++)
                memcpy(c->headRev.data() + old + (size_t)k * w,
                       src + (size_t)(addRows - 1 - k) * w, (size_t)w * 4);
            c->headRows += addRows;
        } else {
            c->body.insert(c->body.end(), src, src + (size_t)addRows * w);
            c->bodyRows += addRows;
        }
        c->stitchH += addRows;
    }

    // 面板两级缩略图段：行数与新增段一一对应，先按固定列宽缩列
    if (addRows > 0 && c->thumbW > 0) {
        if (toHead) {
            size_t old = c->thumbHeadRev.size();
            c->thumbHeadRev.resize(old + (size_t)addRows * c->thumbW);
            for (int k = 0; k < addRows; k++)
                LongCaptureDownscaleRow(src + (size_t)(addRows - 1 - k) * w,
                                        c->thumbHeadRev.data() + old + (size_t)k * c->thumbW,
                                        w, c->thumbW);
            c->thumbHeadH += addRows;
        } else {
            size_t old = c->thumbBody.size();
            c->thumbBody.resize(old + (size_t)addRows * c->thumbW);
            uint32_t* dst = c->thumbBody.data() + old;
            for (int k = 0; k < addRows; k++) {
                LongCaptureDownscaleRow(src + (size_t)k * w, dst, w, c->thumbW);
                dst += c->thumbW;
            }
        }
        c->thumbH = c->thumbHeadH + (int)(c->thumbBody.size() / (size_t)c->thumbW);
        c->thumbDirty = true;
    }

    // 成功提交后才更新匹配基准与 offset 历史（被拒绝的帧完全不经过这里，
    // 因此单次失败不可能污染 lastFrame/lastMatch/offsetHistory）。
    // offset 历史记录完整视觉位移 |d|（回滚重定位同样是已验证的真实滚动量）。
    c->lastFrame.swap(curr);
    std::swap(c->lastMatch, currMatch);
    c->offsetHistory.push_back(d > 0 ? d : -d);
    if ((int)c->offsetHistory.size() > LC_OFFSET_HISTORY_LEN)
        c->offsetHistory.erase(c->offsetHistory.begin());

    // SUCCESS 后 tentative 与 committed 对齐重置（视觉跟踪重新以本帧为基准）。
    // 内容坐标不随头部前插平移，恒等式：lastFrame 视口顶 += d（d>0 下移 / d<0 上移）。
    // 这是「tentative → committed」的唯一同步点，位于唯一提交入口内部。
    // 回滚重定位同样走到这里：committedContentTop 前进即小地图当前区域标注移动。
    c->committedContentTop += d;
    c->tentativeContentTop = c->committedContentTop;
    c->tentativeValid = true;
    c->tentativeConfidence = 1.0f;
    c->trackUnreliableStreak = 0;
    c->trackingRevision++;
    return addRows;
}

// 滚轮 delta ↔ 成功像素位移的在线关系估计（软先验数据源）：成功提交后把自上次提交
// 累计的滚轮增量折算成 px/notch 并 EMA 平滑；方向不一致或幅度离谱的样本直接丢弃。
// 失败的采样绝不更新估计（误识别不能污染先验）；累计增量随后清零重新观察。
// 该估计只通过 LongCaptureBuildOffsetPrior 影响候选排序，不影响任何验收阈值。
void LongCaptureUpdateWheelEstimate(LongCaptureContext* c, int d) {
    int accum = c->wheelAccumDelta;
    c->wheelAccumDelta = 0;
    if (accum == 0 || d == 0) return;
    int notches = (accum > 0 ? accum : -accum) / WHEEL_DELTA;
    if (notches < 1) return;                     // 不足一个 notch（触控板微滚动）：无可比性
    if ((accum > 0) != (d < 0)) return;          // 正 delta=向上滚 ↔ d<0，方向不符丢弃
    float pxPerNotch = (float)(d > 0 ? d : -d) / (float)notches;
    if (pxPerNotch < 1.0f || pxPerNotch > 20000.0f) return;   // 离谱样本丢弃
    c->pixelsPerWheelNotch = c->pixelsPerWheelNotch > 0.0f
        ? 0.7f * c->pixelsPerWheelNotch + 0.3f * pxPerNotch   // EMA 平滑（新样本权重 0.3）
        : pxPerNotch;
}

// —— 轻量帧稳定性检测（进入正式 DetectMatch 前的准入闸门）——
// 用连续两次抓帧的 4bit 灰度稀疏采样对比（直接复用 LongCaptureBuildMatchData 的现成
// 产物，不做完整图像比较）：间隔数十至数百毫秒的两帧在大量有效权重行上仍失配，
// 说明页面仍处于滚动动画/重绘/懒加载/布局过渡中——本帧不适合作为匹配输入，应短延迟
// 后重新采样（RunLongCapture 重试环按 LC_STABLE_RETRY_DELAY 节奏推进）。
// 只回答「现在适不适合匹配」：不参与 offset 评分、不进入任何验收阈值、未过闸也
// 不算匹配失败（跟踪/历史/Weak 候选全部不动）。参考帧超过 LC_STABLE_REF_MAX_GAP
// 视为上一滚动阶段的残影，不具可比性（直接放行，由正式匹配裁决）。
bool LongCaptureFrameUnstable(LongCaptureContext* c, const LongMatchData& curr) {
    if (!c->stableRefValid || c->stableRefCols != curr.cols || c->stableRefH != curr.h)
        return false;
    if (GetTickCount() - c->stableRefTick > LC_STABLE_REF_MAX_GAP) return false;
    const uint8_t* ref = c->stableRefGray.data();
    const uint8_t* gray = curr.gray.data();
    int wTotal = 0, wChanged = 0;
    for (int r = 0; r < curr.h; r++) {
        int wt = curr.weight[r];
        if (wt <= 0) continue;                   // 空白行不具判据
        wTotal += wt;
        if (!LongCaptureRowMatchesRange(ref + (size_t)r * curr.cols,
                                        gray + (size_t)r * curr.cols, 0, curr.cols))
            wChanged += wt;
    }
    if (wTotal < LC_STABLE_MIN_WEIGHT) return false;   // 有效行过少：无稳定性判据
    return (float)wChanged / (float)wTotal >= LC_STABLE_CHANGED_ROW_FRAC;
}

// 滚动更新稳定性参考帧（每次抓帧构建匹配数据后调用，无论本帧走向哪条路径：
// 闸门判定前后、匹配成败与否，参考系始终 = 最近一次抓帧）。
void LongCaptureUpdateStabilityRef(LongCaptureContext* c, const LongMatchData& curr) {
    c->stableRefGray = curr.gray;                // h×cols 字节（≤ ~400KB），拷贝成本可忽略
    c->stableRefCols = curr.cols;
    c->stableRefH = curr.h;
    c->stableRefTick = GetTickCount();
    c->stableRefValid = true;
}

// 单帧「识别 → offset 校验 →（Weak 档）延迟确认 → 提交」管线入口（RunLongCapture
// 采样与单元测试共用）。返回值语义见 LCSampleOutcome：
//   NoChange      —— 两帧完全相同（滚动未生效/尚未渲染，调用方应等待重试），
//                    或匹配成功但位移为 0（≠失败，≠到底）；
//   Failed        —— Normal 档识别失败/置信度不足/offset 异常跳变：本帧被拒绝，
//                    body/headRev/stitchH、lastFrame/lastMatch、offsetHistory 全部保持原样
//                   （已登记的 pendingMatch 保留——硬失败并不否定候选，等下一帧复核）；
//   WeakPending   —— Weak 档首次可信候选：只登记 pendingMatch，不提交、不改任何累计状态；
//   WeakRejected  —— Weak 候选被复核否决（第二次候选不一致/置信度跌破动态门槛/
//                    Weak offset 异常）：清除候选，仍不污染任何累计状态；
//   Unstable      —— 稳定性闸门未过（页面仍处滚动/重绘/懒加载过渡，仅当调用方启用
//                    闸门时可能出现）：本帧不进入正式匹配，不修改任何状态（含跟踪/
//                    历史/Weak 候选），调用方短延迟后重新采样；
//   Stitched      —— 已安全提交且新增拼接行（唯一扩展累计拼接内容的结局；Normal 直接提交，
//                    Weak 需连续两次稳定采样独立复现一致候选，多跳推导提交需过四道关卡）；
//   Repositioned  —— 匹配成功但新视口未越出已捕获范围（反向回滚）：CommitStitch 只推进
//                    当前视口基准并移动小地图当前区域标注，无新增行/帧数。
// 失败帧的跟踪分流（与本函数返回语义正交，只影响外围状态层）：
//   · 与 lastFrame 直连失败 → 多跳回溯最近历史帧重建已验证位移（视觉依据）；
//   · 无多跳依据 → 轻量预测（历史中位数/位移先验）推进 tentative；
//   · 本帧匹配数据登记进最近帧历史（未提交，位置 = 当前 tentative 估计），
//     供后续帧多跳恢复——一次失败不再切断跟踪链，小地图仍能在预计位置层面反馈。
// 注意：旧实现的“匹配失败按滚动方向整帧插入（整帧兜底）”已移除——那是单帧误识别
// 把整个当前视口拼接到长图末尾（后续全部错位）的直接根因；失败现在只能重试或终止。

LCSampleOutcome LongCaptureTryStitch(LongCaptureContext* c, std::vector<uint32_t>& curr, int dir,
                                     bool allowStabilityGate) {
    int w = c->physW, h = c->physH;
    c->sampleIndex++;
    c->lastFailReason = LCFailReason::None;       // 每次调用重置，各结局路径按需覆写
    if (curr.size() != (size_t)w * h || c->lastFrame.size() != (size_t)w * h) {
        c->lastFailReason = LCFailReason::NoCandidate;
        return LCSampleOutcome::Failed;
    }
    if (memcmp(c->lastFrame.data(), curr.data(), (size_t)w * h * 4) == 0) {
        c->pendingMatch.valid = false;           // 内容回到基准：待复核候选已过期
        c->weakCandidateOffsets.clear();         // 弱候选簇同样以基准为参考系，一并过期
        // 与基准帧逐像素全同 = 精确视觉依据：当前视口就在 committed 位置
        //（tentative 同步回归，纠正此前失败帧的预测漂移）。
        LongCaptureTrackingSetVisual(c, c->committedContentTop, 1.0f);
        LC_LOG("[LC#%d] NO_CHANGE h=%d (identical frame, pending cleared)", c->sampleIndex, h);
        return LCSampleOutcome::NoChange;
    }

    // ① 识别（纯函数，不修改状态）：Normal 档未 SUCCESS 时自动尝试 Weak 档
    //（低重叠大跳变，需 3/3 ROI + 边缘结构 + 动态高置信度强证据）。
    // lastFrame 对齐数据两帧采样间不变直接复用，curr 本轮计算，仅提交成功后旋转。
    if (c->lastMatch.h != h || c->lastMatch.cols == 0)
        LongCaptureBuildMatchData(c->lastFrame, w, h, c->lastMatch);
    LongMatchData currMatch;
    LongCaptureBuildMatchData(curr, w, h, currMatch);
    // ⓪ 轻量稳定性检测（准入闸门，默认仅采样主循环启用）：页面仍在快速变化时本帧
    //    不进入正式匹配——短延迟后重新采样，而不是带着过渡帧烧掉正式匹配/重试预算。
    //    本路径不修改任何累计状态（含跟踪/历史/Weak 候选）、不降低任何识别门槛、
    //    也不参与 offset 评分；等待预算由调用方的 stableWaits 控制，耗尽后闸门关闭，
    //    快速连续滚动仍按既有中滚采样节拍进入正式匹配。
    if (allowStabilityGate && LongCaptureFrameUnstable(c, currMatch)) {
        LongCaptureUpdateStabilityRef(c, currMatch);   // 参考帧随抓帧滚动前进
        c->lastFailReason = LCFailReason::FrameUnstable;
        LC_LOG("[LC#%d] decision=FRAME_UNSTABLE (gate: page still changing, wait & resample)",
               c->sampleIndex);
        return LCSampleOutcome::Unstable;
    }
    LongCaptureUpdateStabilityRef(c, currMatch);
    LongCaptureOffsetPrior prior = LongCaptureBuildOffsetPrior(c);
    LCWeakTemporal wt = LongCaptureWeakTemporalContext(c);
    LongMatchOutcome m = LongCaptureDetectMatch(c->lastMatch, currMatch, dir, prior, wt, c->sampleIndex);
    LC_LOG("[LC#%d] detect: status=%d mode=%s d=%d overlap=%d roi=%d/%d/%d(valid=%d) agree=%d "
           "overall=%.2f seam=%.2f tmb=%.2f/%.2f/%.2f spatial=%.2f cont=%.2f prof=%.2f "
           "edge=%.2f sep=%.2f roiW=%.2f tex=%.2f conf=%.2f reason=%s dir=%d prior=%d/%d "
           "pending=%d(%d)",
           c->sampleIndex, (int)m.status,
           m.mode == LongCaptureMatchMode::WeakOverlap ? "WEAK" : "NORMAL",
           m.offset, m.overlap, m.bandOffsets[0], m.bandOffsets[1], m.bandOffsets[2],
           m.validBandCount, m.agreeCount, m.overall, m.seam,
           m.top, m.middle, m.bottom, m.spatial, m.continuity, m.profileScore,
           m.edgeCorrelation, m.peakGap, m.roiWeighted, m.textureRatio, m.confidence,
           LcFailReasonName(m.reason),
           dir, prior.valid ? 1 : 0, prior.expectedAbsOffset,
           c->pendingMatch.valid ? 1 : 0, c->pendingMatch.offset);
    if (m.status != LC_MATCH_SUCCESS) {
        // 方向冲突归因：有候选但与最近滚动方向相反（反向候选已被排序罚分压制、仍无
        // 可信正向对齐）——覆写泛化的「证据不足」分类，便于定位方向先验问题。
        if ((m.reason == LCFailReason::CandidateWeak || m.reason == LCFailReason::None)
            && m.validBandCount > 0 && m.offset != 0 && dir != 0
            && ((m.offset > 0) != (dir > 0)))
            m.reason = LCFailReason::DirectionConflict;
        // —— 失败帧分流：正式拼接保持原样（绝不拼接、绝不污染基准/历史），
        //    先尝试多跳恢复跟踪，再退回轻量预测；本帧登记进最近帧历史。——
        LCMultihopResult mh = LongCaptureMultihopDetect(c, currMatch, dir, prior);
        if (mh.matched) {
            // 已验证的多跳关系：tentative 直接落到链推导位置（视觉依据，可解冻）。
            LongCaptureTrackingSetVisual(c, mh.hopContentY + mh.offset, mh.confidence);
            LC_LOG("[LC#%d] multihop: base=%s frame=%d d=%d conf=%.2f trackY=%lld",
                   c->sampleIndex, mh.hopCommitted ? "committed" : "tentative",
                   mh.hopFrameId, mh.offset, mh.confidence,
                   (long long)(mh.hopContentY + mh.offset));
            // 已提交基准链 + Normal 档强证据：可推导相对 lastFrame 的提交位移。
            // 四道关卡（全过才提交，任一不过 → 只推进 tentative 跟踪）：
            //   ① 推导位移与最近滚动方向同号，且与 lastFrame 重叠满足 Normal 档下限
            //     （重叠不足意味着可能跳过未捕获内容——绝不提交）；
            //   ② 历史跳变合理性（LongCaptureOffsetPlausible）；
            //   ③ 在推导位移处与 lastFrame 做单点富验证并达严格档（防 H 链误配入图）；
            //   ④ 多跳匹配自身为 Normal 档 SUCCESS（弱档链只推进跟踪，不推导提交）。
            if (mh.hopCommitted && mh.mode == LongCaptureMatchMode::Normal) {
                int64_t currContentY = mh.hopContentY + mh.offset;
                int dCommit = (int)(currContentY - c->committedContentTop);
                int ad = dCommit > 0 ? dCommit : -dCommit;
                int minOv = (std::max)(LC_MIN_OVERLAP, (int)(h * LC_MIN_OVERLAP_RATIO));
                bool dirOk = dir == 0 || dCommit == 0 || ((dCommit > 0) == (dir > 0));
                if (dCommit != 0 && dirOk && ad <= h - minOv
                    && LongCaptureOffsetPlausible(c, dCommit, LongCaptureMatchMode::Normal)) {
                    LCOverlapEvidence ev;
                    if (LongCaptureVerifyCandidate(c->lastMatch, currMatch, dCommit, ev)
                        && ev.overall >= LC_ACCEPT_STRICT_OVERALL
                        && ev.seam >= LC_ACCEPT_STRICT_SEAM) {
                        c->pendingMatch.valid = false;   // 多跳强证据覆盖未决候选
                        int added = LongCaptureCommitStitch(c, curr, currMatch, dCommit);
                        LongCaptureUpdateWheelEstimate(c, dCommit);
                        LongCaptureAfterCommit(c, c->sampleIndex);
                        LC_LOG("[LC#%d] decision=MULTIHOP_COMMIT d=%d added=%d via frame=%d "
                               "conf=%.2f overall=%.2f seam=%.2f",
                               c->sampleIndex, dCommit, added, mh.hopFrameId, mh.confidence,
                               ev.overall, ev.seam);
                        return added > 0 ? LCSampleOutcome::Stitched
                                         : LCSampleOutcome::Repositioned;
                    }
                }
            }
        } else {
            // 无多跳依据：轻量预测（TrackingEstimate != StitchMatch，只驱动 tentative，
            // 绝不进入提交）。完全无法判断方向/位移时 tentative 不更新（冻结计数推进）。
            LongCaptureTrackingEstimate est = LongCaptureBuildTrackingEstimate(c, dir, prior);
            if (est.valid)
                LongCaptureTrackingAdvancePredicted(c, est.direction, est.predictedOffset);
            else
                c->trackUnreliableStreak++;
        }
        // 弱档候选（未达标）登记为时间一致性样本（参考系 = lastFrame，与 pending 流一致）。
        if (m.mode == LongCaptureMatchMode::WeakOverlap && m.offset != 0)
            LongCapturePushWeakCandidate(c, m.offset);
        // 本帧进入最近帧历史（未提交，位置 = 当前 tentative 估计），供后续帧多跳回溯。
        LongCaptureHistoryPush(c, c->sampleIndex, std::move(currMatch),
                               c->tentativeContentTop, false);
        // 失败可观测性：原因分类 + 本次最佳候选及主要证据留档（仅日志/重试节奏用，
        // 不参与任何判定），用于区分「没找到候选 / 候选证据不足 / offset 被历史拒绝 /
        // ROI 冲突 / 方向冲突」等失败来源。
        c->lastFailReason = m.reason;
        c->lastReject = m;
        LC_LOG("[LC#%d] decision=REJECTED reason=%s d=%d overlap=%d overall=%.2f seam=%.2f "
               "spatial=%.2f cont=%.2f prof=%.2f edge=%.2f sep=%.2f roiW=%.2f sup=%d "
               "tex=%.2f conf=%.2f (state untouched, tracking only)",
               c->sampleIndex, LcFailReasonName(m.reason), m.offset, m.overlap,
               m.overall, m.seam, m.spatial, m.continuity, m.profileScore,
               m.edgeCorrelation, m.peakGap, m.roiWeighted, m.agreeCount,
               m.textureRatio, m.confidence);
        return LCSampleOutcome::Failed;          // LOW_CONFIDENCE / FAILED：绝不拼接，交给重试
    }
    if (m.offset == 0) {
        c->pendingMatch.valid = false;           // 对齐回零位移：此前候选系误识
        c->weakCandidateOffsets.clear();
        // 零位移对齐 = 视觉依据：当前视口与 committed 基准同位（tentative 回归）。
        LongCaptureTrackingSetVisual(c, c->committedContentTop, m.confidence);
        LC_LOG("[LC#%d] NO_CHANGE (zero-offset success)", c->sampleIndex);
        return LCSampleOutcome::NoChange;        // 匹配成功但内容未滚动：不是失败，也不判断到底
    }

    if (m.mode == LongCaptureMatchMode::Normal) {
        // ②-Normal offset 合理性（历史趋势）→ ③ 提交（与原流程一致）。
        if (!LongCaptureOffsetPlausible(c, m.offset, LongCaptureMatchMode::Normal)) {
            m.reason = LCFailReason::OffsetImplausible;
            c->lastFailReason = m.reason;
            c->lastReject = m;
            LC_LOG("[LC#%d] decision=OFFSET_IMPLAUSIBLE d=%d conf=%.2f (normal)",
                   c->sampleIndex, m.offset, m.confidence);
            return LCSampleOutcome::Failed;
        }
        c->pendingMatch.valid = false;           // Normal 强证据直接覆盖未决的 Weak 候选
        int added = LongCaptureCommitStitch(c, curr, currMatch, m.offset);
        LongCaptureUpdateWheelEstimate(c, m.offset);
        LongCaptureAfterCommit(c, c->sampleIndex);   // 已提交历史登记 + 弱候选簇清空
        LC_LOG("[LC#%d] decision=NORMAL_SUCCESS d=%d added=%d overlap=%d conf=%.2f",
               c->sampleIndex, m.offset, added, m.overlap, m.confidence);
        // added=0：反向回滚未越出已捕获范围，仅重定位（小地图标注移动，无新增行）
        return added > 0 ? LCSampleOutcome::Stitched : LCSampleOutcome::Repositioned;
    }

    // —— WeakOverlap：延迟确认提交（本次修改最重要的防污染机制） ——
    if (!LongCaptureOffsetPlausible(c, m.offset, LongCaptureMatchMode::WeakOverlap)) {
        c->pendingMatch.valid = false;           // 候选幅度不可信：整条候选链作废
        LongCaptureTrackingResetToCommitted(c);// 候选不可信：跟踪退回已确认基准
        LongCaptureHistoryPush(c, c->sampleIndex, std::move(currMatch),
                               c->tentativeContentTop, false);
        m.reason = LCFailReason::OffsetImplausible;
        c->lastFailReason = m.reason;
        c->lastReject = m;
        LC_LOG("[LC#%d] decision=OFFSET_IMPLAUSIBLE d=%d conf=%.2f (weak)",
               c->sampleIndex, m.offset, m.confidence);
        return LCSampleOutcome::WeakRejected;
    }
    if (!c->pendingMatch.valid) {
        c->pendingMatch.valid = true;            // 只登记候选，不触碰任何累计拼接状态
        c->pendingMatch.offset = m.offset;
        c->pendingMatch.confidence = m.confidence;
        c->pendingMatch.mode = LongCaptureMatchMode::WeakOverlap;
        // 弱档候选已过全部空间硬门槛：tentative 按候选位移做视觉级推进（若延迟确认
        // 被否决会退回 committed）；候选同时登记为时间一致性样本；本帧进历史
        //（位置 = committed + 候选位移），作为后续帧的就近多跳基准。
        LongCaptureTrackingSetVisual(c, c->committedContentTop + m.offset, m.confidence);
        LongCapturePushWeakCandidate(c, m.offset);
        LongCaptureHistoryPush(c, c->sampleIndex, std::move(currMatch),
                               c->tentativeContentTop, false);
        LC_LOG("[LC#%d] decision=WEAK_PENDING d=%d overlap=%d conf=%.2f(need %.2f)",
               c->sampleIndex, m.offset, m.overlap, m.confidence,
               LongCaptureWeakRequiredConfidence(h, m.overlap));
        return LCSampleOutcome::WeakPending;
    }
    int pendOff = c->pendingMatch.offset;        // 先留档供日志，再决定去留
    int diff = m.offset > pendOff ? m.offset - pendOff : pendOff - m.offset;
    float need = LongCaptureWeakRequiredConfidence(h, m.overlap);
    if (diff <= LC_WEAK_CONFIRM_OFFSET_TOL && m.confidence >= need) {
        c->pendingMatch.valid = false;           // 连续两次稳定采样独立复现：确认提交
        int added = LongCaptureCommitStitch(c, curr, currMatch, m.offset);
        LongCaptureUpdateWheelEstimate(c, m.offset);
        LongCaptureAfterCommit(c, c->sampleIndex);   // 已提交历史登记 + 弱候选簇清空
        LC_LOG("[LC#%d] decision=WEAK_CONFIRMED d=%d added=%d overlap=%d conf=%.2f",
               c->sampleIndex, m.offset, added, m.overlap, m.confidence);
        return added > 0 ? LCSampleOutcome::Stitched : LCSampleOutcome::Repositioned;
    }
    c->pendingMatch.valid = false;               // 第二次候选不一致/置信度不足：清除，不污染
    LongCapturePushWeakCandidate(c, m.offset);   // 仍是有效的弱候选目击样本
    LongCaptureTrackingResetToCommitted(c);      // 候选链被否决：跟踪退回已确认基准
    LongCaptureHistoryPush(c, c->sampleIndex, std::move(currMatch),
                           c->committedContentTop + m.offset, false);
    m.reason = LCFailReason::CandidateWeak;      // 复核未复现一致候选（证据链断裂）
    c->lastFailReason = m.reason;
    c->lastReject = m;
    LC_LOG("[LC#%d] decision=WEAK_REJECTED d=%d vs pending=%d (diff=%d) conf=%.2f(need %.2f)",
           c->sampleIndex, m.offset, pendOff, diff, m.confidence, need);
    return LCSampleOutcome::WeakRejected;
}

