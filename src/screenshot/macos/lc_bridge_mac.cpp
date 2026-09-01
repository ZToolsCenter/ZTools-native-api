// 长截图算法层 C ABI shim 实现（契约见 lc_bridge_mac.h）。
//
// 本文件只做参数打包/解包转发，不含业务逻辑；唯一的例外是「镜像实现段」——
// 三个纯数据函数/段原本定义在 Windows 侧 IO/UI 文件（lc_frame_io_windows.cpp /
// lc_toolbar_ui_windows.cpp）中，这些文件携带 GDI 依赖无法在 macOS 编译，而 macOS 算法层
// 链接又必需它们，故在此提供逐字等价镜像（Windows 侧不编译本文件，仍以原文件为
// 唯一权威；原文件修改时必须同步本处，各镜像处均有显著标注）。
//
// 编译归属：仅 macOS 构建链（build-swift.sh / CI / test-lc-mac.sh）编译本文件并链入
// libZToolsNative.dylib；Windows 侧 binding.gyp 不包含本文件（算法层经 internal.h
// 原生编译，行为零变化）。
#include "lc_bridge_mac.h"
#include "long_capture_internal.h"

#include <cstring>
#include <new>
#include <vector>

// C ABI 结构体与算法层结构体的维度对齐守卫（漂移即编译失败）。
// 注：重试梯常量（LC_RETRY_DELAY_* 等）为 extern 定义于 lc_match_core.cpp，本 TU
// 仅见 extern 声明、无法参与编译期断言，lc_get_algo_consts 内改用运行时钳制拷贝。
static_assert(LC_ROI_BANDS == 3, "LCMatchEvidence.bandOffsets 固定 3 桶，必须与 LC_ROI_BANDS 一致");

namespace {

// LongMatchOutcome → C 证据结构打包（LCMatchEvidence / LCDetectResult 字段同构，
// 模板统一填充；两结构任一字段更名/增删时本函数编译失败，天然防漂移）。
template <typename T>
void LcBridgeFillEvidence(T& e, const LongMatchOutcome& m) {
    e.status = (int32_t)m.status;
    e.mode = m.mode == LongCaptureMatchMode::WeakOverlap ? LC_MODE_WEAK_OVERLAP : LC_MODE_NORMAL;
    e.failReason = (int32_t)m.reason;
    e.offset = m.offset;
    e.overlap = m.overlap;
    e.overall = m.overall;
    e.seam = m.seam;
    e.top = m.top;
    e.middle = m.middle;
    e.bottom = m.bottom;
    e.spatial = m.spatial;
    e.continuity = m.continuity;
    e.profileScore = m.profileScore;
    e.edgeCorrelation = m.edgeCorrelation;
    e.peakGap = m.peakGap;
    e.roiWeighted = m.roiWeighted;
    e.confidence = m.confidence;
    e.textureRatio = m.textureRatio;
    e.validBandCount = m.validBandCount;
    e.agreeCount = m.agreeCount;
    for (int i = 0; i < LC_ROI_BANDS; i++) {
        e.bandOffsets[i] = m.bandOffsets[i];
        e.bandValid[i] = m.bandValid[i] ? 1 : 0;
    }
}

// C 证据结构清零（hasRejectEvidence=0 / 参数非法路径的确定值）。
template <typename T>
void LcBridgeZeroEvidence(T& e) {
    for (size_t i = 0; i < sizeof(T); i++) reinterpret_cast<char*>(&e)[i] = 0;
}

} // namespace

// ==================== 镜像实现段（纯数据函数；Windows 侧原文件为唯一权威） ====================
//
// 【镜像 1/4】lc_frame_io_windows.cpp 的 LongCaptureDownscaleRow：纯像素行面积平均缩列，
// 无任何平台 API。lc_stitch_state.cpp 的 CommitStitch 缩略图增量维护引用它，而其
// 唯一 Windows 侧定义所在的 lc_frame_io_windows.cpp 无法在 macOS 编译——此处以同名同签名
// （外部链接）提供逐字等价定义，闭合算法层对象的链接；Windows 侧不编译本文件，
// 不存在重复定义。

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

// ==================== C ABI：版本 / 会话生命周期 ====================

int32_t lc_abi_version(void) {
    return LC_ABI_VERSION;
}

// 对齐 lc_session_windows.cpp BeginLongCapture 构造段：new LongCaptureContext + 会话级配置
// 字段拷贝（interval/physW/physH/thumbW/horizontal；thumbW 按 BeginLongCapture
// 同式钳到 [0, physW]）。不含任何窗口/抓帧动作（归 macOS 会话层）。
lc_handle_t lc_session_create(const LCSessionConfig* config) {
    if (!config || config->physW < 1 || config->physH < 1) return nullptr;
    LongCaptureContext* c = new (std::nothrow) LongCaptureContext();
    if (!c) return nullptr;
    c->interval = config->interval;
    c->physW = config->physW;
    c->physH = config->physH;
    c->horizontal = config->horizontal != 0;
    c->thumbW = config->thumbW > 0 ? (std::min)(config->thumbW, config->physW) : 0;
    return c;
}

void lc_session_destroy(lc_handle_t h) {
    delete static_cast<LongCaptureContext*>(h);
}

// ==================== C ABI：帧管线 ====================

// 逐字镜像 lc_frame_io_windows.cpp 的 LongCaptureInitBaseline（纯数据初始化，不抓屏；
// 【镜像 2/4】Windows 侧以 lc_frame_io_windows.cpp 为唯一权威，修改需同步本处）。
int32_t lc_init_baseline(lc_handle_t h, const uint32_t* bgra, int32_t w, int32_t height) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || !bgra || w < 1 || height < 1 || w != c->physW || height != c->physH) return 0;
    std::vector<uint32_t> frame(bgra, bgra + (size_t)w * (size_t)height);
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
    return 1;
}

// 转发 LongCaptureTryStitch（单帧「识别→offset 校验→（Weak 档）延迟确认→提交」管线），
// 并打包七值结局、拒绝分类、证据快照与提交差分（committedContentTop/stitchH 前后差分，
// 等价 RunLongCapture 在 Stitched 后读取的状态变化）。
int32_t lc_try_stitch(lc_handle_t h, const uint32_t* bgra, int32_t dir,
                      int32_t allowStabilityGate, LCTryStitchResult* out) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || !bgra || !out || c->physW < 1 || c->physH < 1) return 0;
    // 帧缓冲拷贝：TryStitch 的 curr 按引用传入且提交成功时被 swap 进 lastFrame（所有权
    // 语义），输入指针为只读，故先拷入本帧缓冲（单帧 ≤ 数 MB，远低于匹配开销）。
    std::vector<uint32_t> curr(bgra, bgra + (size_t)c->physW * (size_t)c->physH);
    const int64_t committedBefore = c->committedContentTop;
    const int stitchHBefore = c->stitchH;
    LCSampleOutcome oc = LongCaptureTryStitch(c, curr, dir, allowStabilityGate != 0);
    out->outcome = (int32_t)oc;
    out->failReason = (int32_t)c->lastFailReason;
    out->addedRows = c->stitchH - stitchHBefore;       // 仅 STITCHED > 0；失败路径恒 0
    out->committedDelta = (int32_t)(c->committedContentTop - committedBefore);
    out->stitchH = c->stitchH;
    out->sampleIndex = c->sampleIndex;
    // 拒绝证据（lastReject）仅在本帧拒绝路径被写入（TryStitch 入口先重置 lastFailReason）；
    // Unstable 路径只写分类不写证据，与 Windows 侧可观测面一致。
    out->hasRejectEvidence =
        (oc == LCSampleOutcome::Failed || oc == LCSampleOutcome::WeakRejected) ? 1 : 0;
    if (out->hasRejectEvidence) {
        LcBridgeFillEvidence(out->evidence, c->lastReject);
    } else {
        LcBridgeZeroEvidence(out->evidence);
    }
    out->pendingValid = c->pendingMatch.valid ? 1 : 0;
    out->pendingOffset = c->pendingMatch.offset;
    out->pendingConfidence = c->pendingMatch.confidence;
    return 1;
}

// ==================== C ABI：纯识别查询 ====================

// 转发 lc_match_core.cpp LongCaptureDetectMatch（双档识别主入口，纯函数不触碰状态）。
int32_t lc_detect_match(const uint32_t* prevBgra, const uint32_t* currBgra,
                        int32_t w, int32_t height, int32_t dir,
                        int32_t priorValid, int32_t priorExpectedAbsOffset,
                        LCDetectResult* out) {
    if (!prevBgra || !currBgra || !out || w < 1 || height < 2) return 0;
    std::vector<uint32_t> prev(prevBgra, prevBgra + (size_t)w * (size_t)height);
    std::vector<uint32_t> curr(currBgra, currBgra + (size_t)w * (size_t)height);
    LongMatchData prevM, currM;
    LongCaptureBuildMatchData(prev, w, height, prevM);
    LongCaptureBuildMatchData(curr, w, height, currM);
    LongCaptureOffsetPrior prior;    // 先验由参数打包（对齐 LongCaptureBuildOffsetPrior 产物）
    if (priorValid) {
        prior.valid = true;
        prior.expectedAbsOffset = priorExpectedAbsOffset;
    }
    LongMatchOutcome m = LongCaptureDetectMatch(prevM, currM, dir, prior, LCWeakTemporal(), 0);
    LcBridgeFillEvidence(*out, m);
    return 1;
}

float lc_weak_required_confidence(int32_t viewportH, int32_t overlap) {
    return LongCaptureWeakRequiredConfidence(viewportH, overlap);
}

const char* lc_fail_reason_name(int32_t failReason) {
    return LcFailReasonName(static_cast<LCFailReason>(failReason));
}

// ==================== C ABI：状态读取 ====================

int32_t lc_get_state(lc_handle_t h, LCStateSnapshot* out) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || !out) return 0;
    out->physW = c->physW;
    out->physH = c->physH;
    out->horizontal = c->horizontal ? 1 : 0;
    out->stitchH = c->stitchH;
    out->headRows = c->headRows;
    out->bodyRows = c->bodyRows;
    out->committedContentTop = c->committedContentTop;
    out->tentativeContentTop = c->tentativeContentTop;
    out->tentativeValid = c->tentativeValid ? 1 : 0;
    out->tentativeConfidence = c->tentativeConfidence;
    out->trackUnreliableStreak = c->trackUnreliableStreak;
    out->trackingRevision = c->trackingRevision;
    out->sampleIndex = c->sampleIndex;
    out->pendingValid = c->pendingMatch.valid ? 1 : 0;
    out->pendingOffset = c->pendingMatch.offset;
    out->pendingConfidence = c->pendingMatch.confidence;
    out->pendingMode = c->pendingMatch.mode == LongCaptureMatchMode::WeakOverlap
        ? LC_MODE_WEAK_OVERLAP : LC_MODE_NORMAL;
    out->lastFailReason = (int32_t)c->lastFailReason;
    out->offsetHistoryLen = (int32_t)c->offsetHistory.size();
    out->wheelAccumDelta = c->wheelAccumDelta;
    out->pixelsPerWheelNotch = c->pixelsPerWheelNotch;
    out->thumbW = c->thumbW;
    out->thumbHeadH = c->thumbHeadH;
    out->thumbH = c->thumbH;
    out->cropTopY = c->cropTopY;
    out->cropBottomY = c->cropBottomY;
    out->cropPendTop = c->cropPendTop ? 1 : 0;
    out->cropPendBottom = c->cropPendBottom ? 1 : 0;
    out->cropPendTopLo = c->cropPendTopLo;
    out->cropPendTopHi = c->cropPendTopHi;
    out->cropPendBottomLo = c->cropPendBottomLo;
    out->cropPendBottomHi = c->cropPendBottomHi;
    out->cropped = c->cropped ? 1 : 0;
    out->interval = c->interval;
    return 1;
}

int32_t lc_get_offset_history(lc_handle_t h, int32_t* out, int32_t cap) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || !out || cap <= 0) return 0;
    const int32_t n = (int32_t)c->offsetHistory.size();
    const int32_t take = n < cap ? n : cap;
    for (int32_t i = 0; i < take; i++) out[i] = c->offsetHistory[(size_t)i];
    return take;
}

// 逐字镜像 lc_frame_io_windows.cpp 的 LongCaptureOutputRows（当前输出行窗口；
// 【镜像 3/4】纯数据段，Windows 侧以 lc_frame_io_windows.cpp 为唯一权威，修改需同步本处）。
int32_t lc_get_output_rows(lc_handle_t h, int64_t* outTop, int64_t* outBottom) {
    const LongCaptureContext* c = static_cast<const LongCaptureContext*>(h);
    if (!c || !outTop || !outBottom) return 0;
    int outTopVal = 0;
    int outBottomVal = c->stitchH;
    if (c->cropped) {
        // 哨兵值显式短路：INT64_MIN/MAX 直接参与 +headRows 加法是有符号溢出（UB）；
        // 开放侧保持哨兵、由区间钳制自然落到界外，与「该侧未设边界」语义一致。
        int64_t t = c->cropTopY == INT64_MIN ? INT64_MIN : c->cropTopY + c->headRows;
        if (t > 0 && t < c->stitchH) outTopVal = (int)t;
        int64_t b = c->cropBottomY == INT64_MAX ? INT64_MAX : c->cropBottomY + c->headRows;
        if (b > outTopVal && b < c->stitchH) outBottomVal = (int)b;
    }
    *outTop = outTopVal;
    *outBottom = outBottomVal;
    return 1;
}

// 拼接缓冲显示行读取：headRev（倒序头部段）+ body（正序主体段）的双段映射，
// 取行序逐字对齐 lc_frame_io_windows.cpp LongCaptureBuildResultBitmap 内的 copyRows 合并
//（显示行 r < headRows ↔ headRev[headRows-1-r]；r ≥ headRows ↔ body[r-headRows]）。
// 完整合并/横向回转由 Swift 侧组装（对齐 Windows 侧 BuildResultBitmap 的缓冲段职责）。
int64_t lc_read_rows(lc_handle_t h, int64_t rowStart, int64_t rowCount,
                     uint32_t* outBuf, int64_t outBufRows) {
    const LongCaptureContext* c = static_cast<const LongCaptureContext*>(h);
    if (!c || !outBuf || rowCount <= 0 || outBufRows < rowCount || c->physW < 1) return 0;
    if (rowStart < 0 || rowStart + rowCount > c->stitchH) return 0;
    const size_t rowW = (size_t)c->physW;
    int64_t written = 0;
    for (int64_t r = rowStart; r < rowStart + rowCount; r++) {
        const uint32_t* srcRow = r < c->headRows
            ? c->headRev.data() + (size_t)(c->headRows - 1 - r) * rowW
            : c->body.data() + (size_t)(r - c->headRows) * rowW;
        memcpy(outBuf + (size_t)written * rowW, srcRow, rowW * 4);
        written++;
    }
    return written;
}

// 缩略图双段读取（thumbHeadRev 倒序头部段 + thumbBody 正序主体段），合并序对齐
// lc_frame_io_windows.cpp LongCaptureRebuildThumb；供小地图面板两级缩略列绘制。
int64_t lc_read_thumb_rows(lc_handle_t h, int64_t rowStart, int64_t rowCount,
                           uint32_t* outBuf, int64_t outBufRows) {
    const LongCaptureContext* c = static_cast<const LongCaptureContext*>(h);
    if (!c || !outBuf || rowCount <= 0 || outBufRows < rowCount || c->thumbW < 1) return 0;
    if (rowStart < 0 || rowStart + rowCount > c->thumbH) return 0;
    const size_t rowW = (size_t)c->thumbW;
    int64_t written = 0;
    for (int64_t r = rowStart; r < rowStart + rowCount; r++) {
        const uint32_t* srcRow = r < c->thumbHeadH
            ? c->thumbHeadRev.data() + (size_t)(c->thumbHeadH - 1 - r) * rowW
            : c->thumbBody.data() + (size_t)(r - c->thumbHeadH) * rowW;
        memcpy(outBuf + (size_t)written * rowW, srcRow, rowW * 4);
        written++;
    }
    return written;
}

// ==================== C ABI：裁剪（#44 延迟剔除语义） ====================

// 逐字镜像 lc_toolbar_ui_windows.cpp LongCaptureApplyCrop 的纯状态段（【镜像 4/4】，
// 与上方 LongCaptureOutputRows / LongCaptureInitBaseline / LongCaptureDownscaleRow
// 同性质——
// 尾部 LongCapturePanelUpdate / InvalidateRect / LongCaptureToolbarRepaint 三个
// Windows UI 刷新调用不在镜像内，归 macOS 会话层；Windows 侧以 lc_toolbar_ui_windows.cpp
// 为唯一权威，修改需同步本处）。物理删行仍由算法层 CommitStitch 入口的
// LongCaptureExecuteCropPurge 在下一次朝该方向成功提交时触发，本函数只登记。
int32_t lc_apply_crop(lc_handle_t h, int32_t row) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || c->stitchH <= 0) return 0;
    int n = c->cropped ? 3 : 2;   // 裁剪菜单行数（末行=重置，仅已裁剪时存在）
    if (row < 0 || row >= n) return 0;
    if (row == n - 1 && c->cropped) {
        // 重置裁剪：清空全部锚点与待剔除区间（尚未执行的剔除随之作废、内容完整恢复；
        // 已触发执行的删除不可逆）
        c->cropPendTop = false;
        c->cropPendBottom = false;
        c->cropTopY = INT64_MIN;
        c->cropBottomY = INT64_MAX;
        c->cropped = false;
    } else {
        // 裁剪线锚定在当前视口边沿（内容坐标）；反向有效边界 = 已设置的锚点或捕获
        // 外沿中的更紧者，保证窗口至少保留一行
        int64_t vpTopY = c->committedContentTop;
        int64_t vpBottomY = vpTopY + c->physH;
        int64_t capTopY = -(int64_t)c->headRows;
        int64_t capBottomY = (int64_t)c->bodyRows;
        if (row == 0) {
            // 丢弃上方（横向 = 左侧）：窗口顶收到当前视口顶（越靠下 = 收得越紧）
            int64_t botEff = (std::min)(c->cropBottomY, capBottomY);
            int64_t cut = vpTopY < botEff - 1 ? vpTopY : botEff - 1;
            if (c->cropTopY == INT64_MIN || cut > c->cropTopY) c->cropTopY = cut;
            int64_t pendHi = c->cropTopY;
            c->cropPendTop = pendHi > capTopY;
            if (c->cropPendTop) {
                c->cropPendTopLo = capTopY;
                c->cropPendTopHi = pendHi;
            }
        } else {
            // 丢弃下方（横向 = 右侧）：窗口底收到当前视口底（越靠上 = 收得越紧）
            int64_t topEff = (std::max)(c->cropTopY, capTopY);
            int64_t cut = vpBottomY > topEff + 1 ? vpBottomY : topEff + 1;
            if (c->cropBottomY == INT64_MAX || cut < c->cropBottomY) c->cropBottomY = cut;
            int64_t pendLo = c->cropBottomY;
            c->cropPendBottom = capBottomY > pendLo;
            if (c->cropPendBottom) {
                c->cropPendBottomLo = pendLo;
                c->cropPendBottomHi = capBottomY;
            }
        }
        c->cropped = true;
    }
    return 1;
}

int32_t lc_has_crop_constraint(lc_handle_t h) {
    const LongCaptureContext* c = static_cast<const LongCaptureContext*>(h);
    if (!c) return 0;
    return LongCaptureHasCropConstraint(c) ? 1 : 0;
}

// ==================== C ABI：滚轮先验 / Weak 时间一致性 ====================

void lc_update_wheel_estimate(lc_handle_t h, int32_t d) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    LongCaptureUpdateWheelEstimate(c, d);
}

// 对齐 lc_session_windows.cpp 面板 WM_INPUT 的滚轮累计（wheelAccumDelta += delta）。
void lc_accumulate_wheel_delta(lc_handle_t h, int32_t delta) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    c->wheelAccumDelta += delta;
}

void lc_push_weak_candidate(lc_handle_t h, int32_t offset) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    LongCapturePushWeakCandidate(c, offset);
}

// 镜像 lc_session_windows.cpp 主循环 Weak 预算耗尽分支的状态段（weakTries 重置归会话层，
// 此处只作废候选链：pendingMatch.valid = false + weakCandidateOffsets.clear()）。
void lc_abandon_weak_chain(lc_handle_t h) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    c->pendingMatch.valid = false;   // 放弃当前候选链，从干净基准重新观察
    c->weakCandidateOffsets.clear(); // 候选链作废：时间一致性样本一并过期
}

int32_t lc_offset_plausible(lc_handle_t h, int32_t d, int32_t mode) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return -1;
    return LongCaptureOffsetPlausible(c, d,
        mode == LC_MODE_WEAK_OVERLAP ? LongCaptureMatchMode::WeakOverlap
                                     : LongCaptureMatchMode::Normal) ? 1 : 0;
}

// ==================== C ABI：Tentative 视觉跟踪 ====================

void lc_tracking_set_visual(lc_handle_t h, int64_t contentY, float confidence) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    LongCaptureTrackingSetVisual(c, contentY, confidence);
}

void lc_tracking_reset_to_committed(lc_handle_t h) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    LongCaptureTrackingResetToCommitted(c);
}

void lc_tracking_advance_predicted(lc_handle_t h, int32_t direction, double magnitude) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c) return;
    LongCaptureTrackingAdvancePredicted(c, direction, magnitude);
}

int32_t lc_build_tracking_estimate(lc_handle_t h, int32_t dir, LCTrackingEstimate* out) {
    LongCaptureContext* c = static_cast<LongCaptureContext*>(h);
    if (!c || !out) return 0;
    LongCaptureTrackingEstimate e = LongCaptureBuildTrackingEstimate(c, dir,
                                                                     LongCaptureOffsetPrior());
    out->valid = e.valid ? 1 : 0;
    out->direction = e.direction;
    out->predictedOffset = e.predictedOffset;
    out->confidence = e.confidence;
    return 1;
}

// ==================== C ABI：常量导出 ====================

// lc_session_windows.cpp 采样主循环所需常量（重试梯/节拍/到底确认/采样内缩）；
// macOS 会话经此取用，禁止在 Swift 侧硬编码同名数值造成漂移。
// 数组拷贝按「语义数量与 C 结构体维度取较小者」钳制，杜绝常量定义变化时的越界读。
void lc_get_algo_consts(LCAlgoConsts* out) {
    if (!out) return;
    out->sampleAttempts = LC_SAMPLE_ATTEMPTS;
    for (int i = 0; i < 5; i++)
        out->retryDelayNormal[i] = i < LC_SAMPLE_ATTEMPTS - 1 ? LC_RETRY_DELAY_NORMAL[i] : 0;
    out->weakRetryAttempts = LC_WEAK_RETRY_ATTEMPTS;
    for (int i = 0; i < 6; i++)
        out->retryDelayWeak[i] = i < LC_WEAK_RETRY_ATTEMPTS ? LC_RETRY_DELAY_WEAK[i] : 0;
    out->stableMaxWaits = LC_STABLE_MAX_WAITS;
    for (int i = 0; i < 3; i++)
        out->stableRetryDelay[i] = i < LC_STABLE_MAX_WAITS ? LC_STABLE_RETRY_DELAY[i] : 0;
    out->quickResamples = LC_QUICK_RESAMPLES;
    for (int i = 0; i < 2; i++)
        out->resampleDelayQuick[i] = i < LC_QUICK_RESAMPLES ? LC_RESAMPLE_DELAY_QUICK[i] : 0;
    out->scrollSampleMaxGap = LC_SCROLL_SAMPLE_MAX_GAP;
    out->bottomConfirmSamples = LC_BOTTOM_CONFIRM_SAMPLES;
    out->weakMaxTries = LC_WEAK_MAX_TRIES;
    out->stableRefMaxGapMs = (int32_t)LC_STABLE_REF_MAX_GAP;
    out->cropInsetLogical = LC_CROP_INSET_LOGI;
    out->trackMinStep = LC_TRACK_MIN_STEP;
}
