// 长截图子系统：纯匹配算法（帧间位移搜索、识别、富验证）。
// CR-021 拆分自 long_capture_windows.cpp 的「帧间全局位移搜索 / 识别阶段」段。
// 本文件为纯函数：输入 LongMatchData 输出 LongMatchOutcome，不修改任何拼接状态，
// 天然可单测。识别相关调参常量在此集中定义（extern，供 lc_stitch_state 等块共享）。
#include "internal.h"
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

// 失败分类名（LCFailReason → 日志字符串，仅可观测性用）。
const char* LcFailReasonName(LCFailReason r) {
    switch (r) {
    case LCFailReason::NoCandidate:        return "NO_CANDIDATE";
    case LCFailReason::CandidateWeak:      return "CANDIDATE_WEAK";
    case LCFailReason::PeakAmbiguous:      return "PEAK_AMBIGUOUS";
    case LCFailReason::GlobalMismatch:     return "GLOBAL_MISMATCH";
    case LCFailReason::SeamMismatch:       return "SEAM_MISMATCH";
    case LCFailReason::SpatialMismatch:    return "SPATIAL_MISMATCH";
    case LCFailReason::ContinuityMismatch: return "CONTINUITY_MISMATCH";
    case LCFailReason::ProfileMismatch:    return "PROFILE_MISMATCH";
    case LCFailReason::RoiInconsistent:    return "ROI_INCONSISTENT";
    case LCFailReason::OffsetImplausible:  return "OFFSET_IMPLAUSIBLE";
    case LCFailReason::DirectionConflict:  return "DIRECTION_CONFLICT";
    case LCFailReason::FrameUnstable:      return "FRAME_UNSTABLE";
    default:                               return "NONE";
    }
}


// 每行参与匹配的采样列上限（列方向稀疏采样；垂直对齐需行级精度，行方向保持 1:1）。

// 全位移搜索选优」，实现位于本文件靠前的采样/拼接段。

// 变化等真实噪声下几乎永远失败，导致整帧重复拼接；改为「量化灰度 + 容差行匹配 +

// 精确逐像素比对在亚像素滚动（平滑滚动/分数偏移的抗锯齿文字）、吸顶元素、滚动条

// —— 帧间模糊对齐参数（参考市面长截图工具的容差匹配方案）——

const int LONG_MATCH_MAX_COLS = 400;

// 4bit 量化灰度的单列容忍差：|Δ| 超过该值计为差异列（吸收文字边缘抗锯齿的轻微偏移）。

const int LONG_MATCH_TOL = 2;

// 扫描阶段每个位移均匀采样的探针行数（只求筛出候选峰，精度由全量验证保证）。

// —— 全位移搜索调参（对齐主流程见 LongCaptureDetectMatch / LongCaptureBestShiftInRange）——

const int LC_SCAN_PROBES = 16;

// 扫探单位移的有效权重下限：探针几乎全落空白行时该位移不可评（-1 分）。

const int LC_SCAN_MIN_WEIGHT = 6;

// 局部峰值合并窗口：±N 行内的得分类似属同一次对齐（亚像素抖动），只保留峰值。

const int LC_PEAK_WIN = 3;

// 进入全量验证的候选峰数量上限（按扫描分降序截断）。

const int LC_MAX_CANDIDATES = 8;

// 最小可信重叠行数（绝对下限）：与 LC_MIN_OVERLAP_RATIO 取较大者约束位移搜索范围，
// 重叠过小（跳变/懒加载）没有对齐依据——宁可判失败重试，也不允许“整帧兜底”式拼接。

const int LC_MIN_OVERLAP = 24;

// —— 鲁棒性约束（防单帧误识别污染累计拼接状态；全部为可调常量，勿在逻辑中散落魔法数）——

// 重叠至少占视口高的比例：位移 |d| 超过 h×(1-该值) 的候选直接不在搜索范围内。
// 旧实现仅要求 24 行重叠（2000px 视口下允许 |d|≈1976），是极端错误 offset 的主要来源。

const float LC_MIN_OVERLAP_RATIO = 0.15f;

// 跨 ROI 位移一致性容差（px）：左/中/右三个列 ROI 各自独立求得的位移，
// 组内差异 ≤ 该值视为同一次对齐（例：812/814/810 可信；812/811/3260 中 3260 为离群被否决）。

const int LC_ROI_OFFSET_TOLERANCE = 4;

// 综合置信度下限（overall/seam/ROI 一致性加权，见 LongCaptureDetectMatch）：低于该值禁止拼接。

const float LC_MIN_CONFIDENCE = 0.60f;

// |d| 超过历史成功位移中位数该倍数视为异常跳变，直接拒绝且不更新历史。

const float LC_OFFSET_JUMP_RATIO = 4.0f;

// offset 历史窗口长度（合理性校验的中位数统计样本数；滚动节奏渐变时窗口滚动自适应）。

const int LC_OFFSET_HISTORY_LEN = 5;

// 历史样本达到该数量后才启用跳变比率校验（样本过少时中位数不稳，过早启用会误杀正常滚动）。

const int LC_OFFSET_HISTORY_MIN = 3;

// 单次采样的抓取/匹配尝试总数（首次 + 重试）：滚动未生效/页面未渲染完成时等待后
// 重试，而非带着错误拼接。加大尝试数以提高「等页面渲染稳定」窗口内的判定频次，
// 提升重叠区域判定的成功率。

const int LC_SAMPLE_ATTEMPTS = 6;

// 采样重试间隔（ms）：按结局分档递增——等待 scroll 动画/懒加载/DOM 重绘稳定，
// 而非一次性永久放慢整体采样节奏。Normal 失败 5 档（数组长度 = 尝试数 - 1，首次
// 尝试不占档位）；Weak 候选复核 6 档（延迟确认需要跨多次采样观察页面稳定）。

const int LC_RETRY_DELAY_NORMAL[LC_SAMPLE_ATTEMPTS - 1] = { 90, 120, 150, 180, 210 };

const int LC_RETRY_DELAY_WEAK[6] = { 100, 130, 160, 180, 200, 220 };

// —— 帧稳定性检测（抓帧准入闸门：减少「抓到过渡帧导致识别失败」）——
// 连续两次抓帧（间隔数十至数百毫秒）的 4bit 灰度稀疏采样对比：有效权重行失配占比
// 仍高 = 页面处于滚动动画/重绘/懒加载/布局过渡中，本帧不适合作为匹配输入——短延迟
// 后重新采样，而不是带着过渡帧进 DetectMatch 白白烧掉重试预算。只判断「现在适不
// 适合匹配」：不参与 offset 评分、不修改任何累计状态、不降低任何识别门槛；每采样
// 轮等待预算耗尽后强制放行（快速连续滚动仍按既有中滚采样节拍匹配，机制不受阻塞）。

const DWORD LC_STABLE_REF_MAX_GAP = 600;   // 参考帧最大有效间隔（ms）：更早的参考
                                                  // 属于上一个滚动阶段，不具可比性（直接放行）

const float LC_STABLE_CHANGED_ROW_FRAC = 0.30f; // 有效权重行失配占比 ≥ 该值判不稳定

const int LC_STABLE_MIN_WEIGHT = 40;       // 稳定性判定的最低有效权重（更低无判据）

const int LC_STABLE_MAX_WAITS = 3;         // 每采样轮稳定性等待预算（此后强制进入匹配）

const int LC_STABLE_RETRY_DELAY[LC_STABLE_MAX_WAITS] = { 30, 45, 60 };  // ms

// Normal 失败后的短时重采样（「等待页面稳定」而非放宽条件）：无候选/全宽不可评这类
// 瞬态失败先走几十毫秒级快重采样——刚好抓在滚动/重绘过渡帧的情形在下一拍往往自愈。
// 不提交、不修改正式累计状态、不降低任何阈值，预算独立于 LC_SAMPLE_ATTEMPTS 主重试梯。

const int LC_QUICK_RESAMPLES = 2;          // 瞬态失败快重采样次数上限

const int LC_RESAMPLE_DELAY_QUICK[LC_QUICK_RESAMPLES] = { 40, 60 };  // ms

// 滚动中主动采样最大间隔（ms）：连续滚动尚未停稳时，距上次采样超过该节拍即不等
// 停稳主动抓帧拼接——相邻帧位移因此足够小、overlap 稳定落在 Normal 匹配档，重叠
// 判定首选大 overlap 的 Normal 路径，只有极端大跳变才轮到 Weak 档兜底。快速滚动
// （一次滚过大半屏）下更短的节拍直接抬高相邻帧重叠量，是比事后恢复更前置的防线；
// 该值在「重叠更大」与「采样/匹配开销更高」之间折中。用户配置的 interval 比该值
// 更小时尊重用户值（滚动中节拍取两者较小者）。

const int LC_SCROLL_SAMPLE_MAX_GAP = 150;

// 连续「内容未变化」采样数达到该值确认滚动到底（与匹配失败完全独立的事件）。

const int LC_BOTTOM_CONFIRM_SAMPLES = 3;

// —— Weak / 大跳变匹配（低重叠双模式）——
// Normal 档允许 overlap >= max(LC_MIN_OVERLAP, h×LC_MIN_OVERLAP_RATIO)；
// Weak 档把可识别范围放宽到 overlap >= max(LC_WEAK_MIN_OVERLAP, h×LC_WEAK_MIN_OVERLAP_RATIO)，
// 且只扫描 (weak 下限, normal 下限) 之间的纯弱重叠区间（两档不重叠，模式由重叠量唯一决定）。
// 原则：overlap 越小，要求的证据越强——绝不简单降低门槛放行，更不回到“失败整帧兜底”的老路。

const int LC_WEAK_MIN_OVERLAP = 32;            // 弱重叠绝对下限（px）

const float LC_WEAK_MIN_OVERLAP_RATIO = 0.05f; // 弱重叠占视口高下限（5%）

// Weak 档跨 ROI 一致性：必须 3/3 ROI 全部产出候选且极差（max−min）不超过该值。
// 任一 ROI 离群或缺失即整帧拒绝——低重叠下单 ROI 或多数派证据都不可信。

const int LC_WEAK_ROI_TOLERANCE = 3;

// Weak 档验收阈值（全部高于 Normal 严格档）：

const float LC_WEAK_ACCEPT_OVERALL = 0.65f;

const float LC_WEAK_ACCEPT_SEAM = 0.75f;

const float LC_WEAK_ACCEPT_EDGE = 0.80f;       // 行边缘结构相关度下限

const float LC_WEAK_MIN_CONFIDENCE = 0.72f;    // 动态置信度门槛基线

const float LC_WEAK_CONF_EXTRA_PENALTY = 0.08f; // overlap 从 15%h 降到 5%h 的置信度加罚上限

const float LC_WEAK_OVERLAP_RATIO_LIMIT = 0.90f; // Weak offset 合理性放宽上限（×视口高）

const int LC_WEAK_CONFIRM_OFFSET_TOL = 2;      // 延迟确认：两次候选位移差上限（px）

const int LC_WEAK_RETRY_ATTEMPTS = 6;          // 单轮采样内 Weak 候选的重试次数（= LC_RETRY_DELAY_WEAK 档数）

const int LC_WEAK_MAX_TRIES = 5;               // Weak 候选独立采样轮数（耗尽即放弃候选链，从干净基准重新观察）

// 滚轮 delta 软先验：候选 |d| 落在期望值 ±max(8, 期望/4) 内时给调整分加该值。
// 仅影响同档候选的排序先后，绝不改变任何验收阈值或搜索范围。

const float LC_WHEEL_PRIOR_BONUS = 0.04f;

// —— Tentative 视觉跟踪 / 多跳恢复 / Weak 时间一致性（外围状态层，不改任何拼接验收门槛）——
// 目标：某一帧 overlap 判定失败不再切断整个滚动跟踪链——小地图在"预测位置"层面持续
// 反馈，而正式拼接始终保持严格保守（只有 SUCCESS 才能提交）。本组常量全部只作用于
// 跟踪/历史/时间证据层，绝不参与 DetectPass 的空间验收阈值。

const int LC_HISTORY_FRAMES = 8;         // 最近帧环形历史容量（规格建议 4~8；快速滚动
                                                // 失败帧多，适当加深历史给多跳恢复更多候选基准）

const int LC_HISTORY_HOPS = 4;           // 失败后最多回溯的历史基准数（最多 4 跳）

const int LC_TRACK_FREEZE_FRAMES = 3;    // 连续无视觉依据采样数达该值即冻结 tentative

const int LC_TRACK_MIN_STEP = 2;         // 小于该位移（px）不推进 tentative（噪声）

const float LC_TRACK_PREDICT_CONFIDENCE = 0.35f; // 纯预测估计的初始置信度（低）

const float LC_TRACK_CONFIDENCE_DECAY = 0.7f;   // 预测链逐帧置信度衰减系数

const int LC_WEAK_TEMPORAL_TOL = 3;          // 弱候选与共识簇中位数的容差（px）

const int LC_WEAK_TEMPORAL_MIN_SAMPLES = 2;   // 形成时间共识所需的历史样本数

const int LC_WEAK_TEMPORAL_MAX_HISTORY = 8;   // 弱候选样本环形容量

const float LC_WEAK_TEMPORAL_BONUS_MAX = 0.06f; // 时间共识给弱档置信度的加分手

// 瑕疵最明显，验收时单独要求高匹配率。

// 接缝窗行数（随重叠长度折半封顶）：新增块直接续接在接缝窗之后，此处错位的视觉

const int LC_SEAM_ROWS = 64;

// 全量验证的有效权重下限（重叠区几乎全空白时无法评判对齐质量）。

const int LC_VERIFY_MIN_WEIGHT = 20;

// 重叠区有效纹理行占比（weight>0 行 / 重叠行数）低于该值时，开始按缺失比例轻度
// 折扣综合置信度（见 DetectPass ⑥）：大面积纯色重叠在错误位移处也可能偶然拿到
// 高 overall——「匹配成功」与「匹配可信」必须区分，证据量本身要计价。

const float LC_TEXTURE_CONF_FLOOR = 0.12f;

const float LC_TEXTURE_CONF_SCALE = 0.5f;  // 折扣强度：texture=0 时最多约 −0.06

// 验收阈值：严格档覆盖常规页面；宽松档候选不再直接放行拼接，只作为 LOW_CONFIDENCE
// 的判定依据（交由重试救援动画/高噪声页面），两档都不达标即 FAILED。

const float LC_ACCEPT_STRICT_OVERALL = 0.55f;

const float LC_ACCEPT_STRICT_SEAM = 0.65f;

const float LC_ACCEPT_LOOSE_OVERALL = 0.42f;

const float LC_ACCEPT_LOOSE_SEAM = 0.50f;

// 与最近滚动方向相反的候选罚分（防误配插到错误一端；同向候选无罚分）。

const float LC_DIR_PENALTY = 0.08f;

// 不同峰之间调整分差距小于该值视为歧义（典型于周期性列表），偏向重叠更大（|d| 更小）的一峰。

const float LC_AMBIGUITY_MARGIN = 0.05f;

// —— ROI 证据融合与全宽空间一致性（「局部巧合 ≠ 真实重叠」防御的核心参数）——

// 每个 ROI 提交给跨 ROI 聚类的候选数（Top-N 证据；ROI 不再单票定生死）。

const int LC_BAND_TOP_N = 3;

// 进入全宽富验证的聚类数上限（按 ROI 加权融合分降序截断）。

const int LC_MAX_VERIFY_CANDIDATES = 5;

// 低信息量 ROI 的权重下限：空白/弱纹理 ROI 仍有最低投票权（左右留白的单栏文本页），
// 但绝不再与结构丰富的 ROI 等权（权重 = 下限 + (1-下限) × 带内细节占比）。

const float LC_ROI_MIN_INFO_WEIGHT = 0.25f;

// —— ROI 列带自适应放置（宽留白选区/空白页的匹配抗性）——
// 三列 ROI 不再固定按采样列数三分，而是按「列垂直结构量」把内容等分三段（每段
// 都携带约 1/3 内容量，见 LongCaptureBuildMatchData）：固定三分在选区左右大量留白
// 时会让边缘 ROI 整带空白（有效 ROI 只剩 1 个，Weak 档 3/3 全票永远不可达）。
// 该值为自适应列带的最小宽度：行匹配容差为 (c1-c0)/20，带宽低于它容差归零；
// 采样列总数不足 3×该值时退回固定三分（与旧行为一致）。

const int LC_BAND_MIN_COLS = 24;

// 候选综合分（不含分离度项）差达到该值视为峰值充分分离（无周期性歧义）。

const float LC_PEAK_SEP_FULL = 0.15f;

// 第一峰与次峰分差小于歧义裕度（周期性/重复内容）时的额外置信度加罚。

const float LC_AMBIGUITY_CONF_PENALTY = 0.12f;

// 匹配分布连续性的断点罚参数：gap 数达到 6 即罚满（0.15 × min(1, gaps/6)）。

const float LC_CONTINUITY_GAP_MAX = 6.0f;

// —— 动态变化区域局部降权（候选已定后的行级异常带识别，见 LongCaptureVerifyCandidate）——
// lazy-load 图片/视频帧/倒计时/sticky/局部重排会让部分行在相邻帧间真实变化，但这不
// 代表整体 offset 错误。富验证时识别「局部、连续、空间有限」的失配带并将其从统计中
// 剔除（与空白行同等对待：不计比率、不打断连续段），避免少量动态内容拖垮 overall/
// continuity。硬约束（防「挽救错误 offset」）：
//   · 单段超过 LC_DYNAMIC_MAX_ROWS、或候选屏蔽总量超过有效行占比上限 → 完全不屏蔽，
//     大面积异常是真实失配证据，绝不能被忽略；
//   · 剔除只收回负面影响，绝不产生正向证据；下游阈值（LC_VERIFY_MIN_WEIGHT 与全部
//     验收门槛）对剔除后的证据量原样生效——证据不足的候选不可能借屏蔽通过验收。

const int LC_DYNAMIC_MIN_ROWS = 3;            // 短于该行数的失配属散点噪声，正常统计

const int LC_DYNAMIC_MAX_ROWS = 48;           // 单段可屏蔽最大行数（更大 = 真实失配）

const int LC_DYNAMIC_BRIDGE = 2;              // 段内允许的空白桥接行数（动态区含留白）

const float LC_DYNAMIC_MAX_FRACTION = 0.10f;  // 可屏蔽有效行占比硬上限

// 最终候选的局部 offset basin 合并半径（±px）：胜出聚类中位数附近的 1~2px 峰漂移
// （亚像素滚动/抗锯齿/量化，如 d=417/418/419）属同一 basin，在 ±半径内做全宽富验证
// 择最优证据者作为最终位置。只做局部择优：绝不合并相距较远的周期性峰，也不改变
// 任何验收阈值（邻域点越出本档重叠区间即跳过，不越档）。

const int LC_BASIN_RADIUS = 2;

// Weak 档新增证据下限（全部为「低重叠要求更强证据」的加严项，Normal 档无对应硬门槛，
// 仅通过综合置信度起作用）：空间一致性 / 匹配连续性 / 多尺度结构 / 峰值分离度。

const float LC_WEAK_ACCEPT_SPATIAL = 0.70f;

const float LC_WEAK_ACCEPT_CONTINUITY = 0.60f;

const float LC_WEAK_ACCEPT_PROFILE = 0.50f;

const float LC_WEAK_MIN_PEAK_SEP = 0.10f;

// ==================== 长截图：帧间全局位移搜索（重叠检测） ====================

bool LongCaptureRowMatchesRange(const uint8_t* a, const uint8_t* b, int c0, int c1) {
    int bad = 0, limit = (c1 - c0) / 20;
    for (int i = c0; i < c1; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d > LONG_MATCH_TOL || -d > LONG_MATCH_TOL) {
            if (++bad > limit) return false;
        }
    }
    return true;
}

// 全宽行匹配（列区间 [0, cols) 的便捷包装）。

bool LongCaptureRowMatches(const uint8_t* a, const uint8_t* b, int cols) {
    return LongCaptureRowMatchesRange(a, b, 0, cols);
}

// 与全帧细节总量一并产出。

// 行权重（= min(4, 行内相邻采样列灰度跳变数)，空白行为 0、强纹理封顶防单行主导）

// 构建匹配数据：BGRA → 整数灰度 (29B+150G+76R)>>8 → 4bit 量化；列采样、

void LongCaptureBuildMatchData(const std::vector<uint32_t>& frame, int w, int h,
                                      LongMatchData& m) {
    int left = (std::max)(8, w / 50);            // 左侧剔除 ~2%（边缘抗锯齿）
    int right = w - (std::max)(24, w / 20);      // 右侧剔除 ~5%（覆盖滚动条宽度）
    if (right - left < 8) { left = 0; right = w; }   // 过窄选区兜底：全宽参与
    int span = right - left;
    int step = (std::max)(1, (span + LONG_MATCH_MAX_COLS - 1) / LONG_MATCH_MAX_COLS);
    m.cols = (span + step - 1) / step;
    m.h = h;
    m.gray.assign((size_t)h * (size_t)m.cols, 0);
    m.weight.assign((size_t)h, 0);
    m.edge.assign((size_t)h, 0);
    m.detailSum = 0;
    // 三列 ROI（左/中/右）边界在灰度数据构建完成后按内容自适应放置（见下方
    // 「列垂直结构量等分」段），不再固定按列数三分。列区间来自已剔除选区描边/
    // 抗锯齿/滚动条的采样范围，选区 Overlay 与固定 UI 不会进入任何 ROI。
    for (int r = 0; r < h; r++) {
        const uint32_t* src = frame.data() + (size_t)r * w + left;
        uint8_t* dst = m.gray.data() + (size_t)r * m.cols;
        int trans = 0, edgeSum = 0;
        for (int k = 0, cx = 0; k < m.cols; k++, cx += step) {
            uint32_t px = src[cx];   // 内存序 BGRA
            int g = ((int)(px & 0xFF) * 29 + (int)((px >> 8) & 0xFF) * 150
                   + (int)((px >> 16) & 0xFF) * 76) >> 8;
            g >>= 4;
            if (k > 0) {
                int dG = g - (int)dst[k - 1];
                if (dG < 0) dG = -dG;
                if (dG > 0) trans++;
                edgeSum += dG;       // 行结构强度：相邻采样列量化灰度差绝对值之和
            }
            dst[k] = (uint8_t)g;
        }
        m.weight[r] = (uint8_t)((std::min)(trans, 4));
        m.edge[r] = (uint16_t)edgeSum;   // 上限 cols(≤400)×15，uint16 足够
        m.detailSum += trans;
    }

    // —— 三列 ROI 自适应放置（宽留白选区/空白页匹配抗性的核心）——
    // 按列垂直结构量（该列相邻行量化灰度跳变总数：纯色留白列为 0、文字/图片列很高）
    // 把全宽内容等分为三段——每段都携带约 1/3 的内容量。固定列数三分在「选区左右
    // 大量留白」时会让边缘 ROI 整带空白（有效 ROI 只剩 1 个，Weak 档 3/3 全票永远
    // 不可达），内容等分后三个 ROI 各自落在有效内容上，「多个有效局部区域分别
    // 匹配 + 跨 ROI 聚类取中位数」的既有机制（BestShiftInRange → DetectPass ③）
    // 才真正成立。带宽钳制保证任一带 ≥ LC_BAND_MIN_COLS（行匹配容差 (c1-c0)/20
    // 不归零）；全帧无结构或采样列过少时退回固定三分（与旧行为一致）。
    bool bandsByContent = false;
    if (m.cols >= LC_BAND_MIN_COLS * LC_ROI_BANDS) {
        std::vector<int> colDetail((size_t)m.cols, 0);
        for (int r = 1; r < h; r++) {
            const uint8_t* row = m.gray.data() + (size_t)r * m.cols;
            const uint8_t* prev = row - m.cols;
            for (int k = 0; k < m.cols; k++)
                if (row[k] != prev[k]) colDetail[k]++;
        }
        int total = 0;
        for (int k = 0; k < m.cols; k++) total += colDetail[k];
        if (total > 0) {
            std::vector<int> prefix((size_t)m.cols + 1, 0);
            for (int k = 0; k < m.cols; k++)
                prefix[(size_t)k + 1] = prefix[k] + colDetail[k];
            int b1 = 0, b2 = 0;
            while (b1 < m.cols && prefix[b1] * 3 < total) b1++;        // 首个内容量 ≥ 1/3 的边界
            while (b2 < m.cols && prefix[b2] * 3 < total * 2) b2++;    // 首个内容量 ≥ 2/3 的边界
            int lo1 = LC_BAND_MIN_COLS, hi1 = m.cols - 2 * LC_BAND_MIN_COLS;
            if (b1 < lo1) b1 = lo1; else if (b1 > hi1) b1 = hi1;       // 最小带宽钳制
            int lo2 = b1 + LC_BAND_MIN_COLS, hi2 = m.cols - LC_BAND_MIN_COLS;
            if (b2 < lo2) b2 = lo2; else if (b2 > hi2) b2 = hi2;
            m.bandStart[0] = 0;       m.bandCols[0] = b1;
            m.bandStart[1] = b1;      m.bandCols[1] = b2 - b1;
            m.bandStart[2] = b2;      m.bandCols[2] = m.cols - b2;
            bandsByContent = true;
        }
    }
    if (!bandsByContent) {            // 固定三分兜底：全帧无结构 / 采样列过少
        for (int b = 0; b < LC_ROI_BANDS; b++) {
            m.bandStart[b] = m.cols * b / LC_ROI_BANDS;
            m.bandCols[b] = m.cols * (b + 1) / LC_ROI_BANDS - m.bandStart[b];
        }
    }
    // 带内行权重：该带列区间内相邻采样列的灰度跳变数（空白行为 0），
    // 供各 ROI 独立扫描/验证（LongCaptureBestShiftInRange）使用。
    for (int b = 0; b < LC_ROI_BANDS; b++) {
        m.bandWeight[b].assign((size_t)h, 0);
        int bs = m.bandStart[b], be = bs + m.bandCols[b];
        for (int r = 0; r < h; r++) {
            const uint8_t* row = m.gray.data() + (size_t)r * m.cols;
            int bt = 0;
            for (int k = bs + 1; k < be; k++)
                if (row[k] != row[k - 1]) bt++;
            m.bandWeight[b][r] = (uint8_t)((std::min)(bt, 4));
        }
    }

    // 多尺度垂直结构 profile：每行按采样列均分为 LC_PROFILE_BUCKETS 桶取平均量化灰度
    //（行桶均值对光标/局部动画等水平噪声鲁棒），再对每个桶做 4 行 / 8 行滑动窗口平均。
    // 逐行灰度匹配可被「局部巧合、大面积纯色」偶然通过，而聚合 profile 刻画整段的
    // 明暗起伏——真对齐处两个尺度的相关度都应高，用于区分「局部像素相似」与
    // 「整段垂直结构一致」（仅作辅助证据，见 LongCaptureProfileCorrelation）。
    // 帧尾不足一个窗口时自动截短（两帧同式处理，真对齐处两侧窗口仍逐桶相等）。
    m.profile4.assign((size_t)h * LC_PROFILE_BUCKETS, 0.0f);
    m.profile8.assign((size_t)h * LC_PROFILE_BUCKETS, 0.0f);
    {
        std::vector<double> prefix((size_t)(h + 1) * LC_PROFILE_BUCKETS, 0.0);
        for (int r = 0; r < h; r++) {
            const uint8_t* row = m.gray.data() + (size_t)r * m.cols;
            for (int b = 0; b < LC_PROFILE_BUCKETS; b++) {
                int bc0 = m.cols * b / LC_PROFILE_BUCKETS;
                int bc1 = m.cols * (b + 1) / LC_PROFILE_BUCKETS;
                double s = 0;
                for (int k = bc0; k < bc1; k++) s += row[k];
                prefix[(size_t)(r + 1) * LC_PROFILE_BUCKETS + b] =
                    prefix[(size_t)r * LC_PROFILE_BUCKETS + b] + s / (std::max)(1, bc1 - bc0);
            }
        }
        for (int r = 0; r < h; r++) {
            for (int s = 0; s < 2; s++) {
                int win = s == 0 ? 4 : 8;
                std::vector<float>& prof = s == 0 ? m.profile4 : m.profile8;
                int r1 = (std::min)(h, r + win);
                for (int b = 0; b < LC_PROFILE_BUCKETS; b++)
                    prof[(size_t)r * LC_PROFILE_BUCKETS + b] =
                        (float)((prefix[(size_t)r1 * LC_PROFILE_BUCKETS + b]
                               - prefix[(size_t)r * LC_PROFILE_BUCKETS + b])
                               / (std::max)(1, r1 - r));
            }
        }
    }
}

// 位移记 -1 分（大片空白无法评判）。out 返回候选位移列表（含符号）。

// （空白行权重 0 不计分；失配权重过半即早退），有效权重不足 LC_SCAN_MIN_WEIGHT 的

// 单位移评分：重叠区均匀取 LC_SCAN_PROBES 个探针行按 curr 行权重累积匹配/失配权重

// 相邻位移属同一次对齐的亚像素抖动），按分数降序保留至多 LC_MAX_CANDIDATES 个。

// 全位移扫描：对每个可行位移打快速分，收集局部峰值（±LC_PEAK_WIN 邻域最大者，

// 全位移扫描（限定列区间 [c0,c1) 与行权重 weight）：对每个可行位移打快速分，
// 收集局部峰值（±LC_PEAK_WIN 邻域最大者，相邻位移属同一次对齐的亚像素抖动），
// 按分数降序保留至多 LC_MAX_CANDIDATES 个。out 返回候选位移列表（含符号）。
// 探针行只落在重叠区内的有效行（weight>0）上——空白行无论布到多少针都不产生
// 证据，优先把针位花在有纹理的行上（见函数内「有效行索引」注释）。
//
// 可行位移范围由重叠区间 [ovMin, ovMax] 参数化（双模式识别的档位边界）：
//   · Normal 档：ovMin = max(LC_MIN_OVERLAP, h×15%)、ovMax = h（含 d=0），行为与原实现一致；
//   · Weak 档：只扫 (max(32, h×5%), Normal 下限) 的纯弱重叠区间，与 Normal 档不重叠。
// 先粗后精的两段式（探针粗筛→候选全量验证）只在给定范围内进行，不做全图无界搜索；
// 过小重叠（跳变/懒加载）没有对齐依据，直接产不出候选（判定失败走重试）。
// 向下滚时重叠关系即「上一帧底部 ↔ 当前帧顶部」（curr[c] ↔ prev[d+c]），
// 向上滚对称，天然聚焦在上下帧重叠区域而非盲搜。

static void LongCaptureScanCandidatesRange(const LongMatchData& prevM, const LongMatchData& currM,
                                           int c0, int c1, const uint8_t* weight,
                                           int ovMin, int ovMax, std::vector<int>& out) {
    out.clear();
    int h = currM.h;
    if (ovMin < 2 || ovMax < ovMin || prevM.cols != currM.cols || prevM.h != h) return;
    int cols = currM.cols;
    int lo = -(h - ovMin), hi = h - ovMin;
    if (lo > hi) return;
    // 有效行索引（weight>0，升序）：探针只在重叠区内的有效行上均匀取样。原实现
    // 按重叠区均匀布针，重叠区大部分为空白（宽留白选区/页面留白段）时探针大量
    // 落在空白行上，goodW+badW 凑不满 LC_SCAN_MIN_WEIGHT → 所有位移不可评 →
    // 整帧无候选。内容导向布针后空白占比不再影响「可评性」，只影响证据量（有效
    // 行更少 → 权重更低，下游 LC_VERIFY_MIN_WEIGHT 与验收门槛自然兜底）。
    std::vector<int> informative;
    for (int r = 0; r < h; r++)
        if (weight[r] > 0) informative.push_back(r);
    std::vector<float> score((size_t)hi - lo + 1, -1.0f);
    for (int d = lo; d <= hi; d++) {
        int ad = d > 0 ? d : -d;
        int o = h - ad;
        if (o > ovMax) continue;                   // 超出本档重叠上限（Weak 档排除 Normal 区间）
        int cStart = d < 0 ? ad : 0;               // 重叠区在 curr 内的行区间 [cStart, cEnd)
        int cEnd = cStart + o;
        int goodW = 0, badW = 0;
        if (!informative.empty()) {
            int f = (int)(std::lower_bound(informative.begin(), informative.end(), cStart)
                          - informative.begin());
            int l = (int)(std::lower_bound(informative.begin(), informative.end(), cEnd)
                          - informative.begin());
            int m = l - f;                         // 重叠区内有效行数
            int nProbes = m < LC_SCAN_PROBES ? m : LC_SCAN_PROBES;
            for (int i = 0; i < nProbes; i++) {
                int c = informative[f + (int)((long long)m * (i * 2 + 1) / (nProbes * 2))];
                int wt = weight[c];
                if (LongCaptureRowMatchesRange(
                        prevM.gray.data() + (size_t)(d + c) * cols,
                        currM.gray.data() + (size_t)c * cols, c0, c1))
                    goodW += wt;
                else
                    badW += wt;
                if (badW >= 4 && badW * 2 > goodW + badW) break;
            }
        }
        if (goodW + badW >= LC_SCAN_MIN_WEIGHT)
            score[d - lo] = (float)goodW / (float)(goodW + badW);
    }
    for (int d = lo; d <= hi; d++) {
        float s = score[d - lo];
        if (s <= 0) continue;
        bool peak = true;
        for (int k = -LC_PEAK_WIN; k <= LC_PEAK_WIN && peak; k++) {
            int dd = d + k;
            if (dd == d || dd < lo || dd > hi) continue;
            if (score[dd - lo] > s) peak = false;
        }
        if (peak) out.push_back(d);
    }
    if (out.size() > 1) {
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
            return score[a - lo] > score[b - lo];
        });
        if (out.size() > (size_t)LC_MAX_CANDIDATES) out.resize((size_t)LC_MAX_CANDIDATES);
    }
}

// 返回 false = 有效权重不足（重叠区几乎全空白），对齐不可信。

//   重新暴露的内容失配集中在另一端，不影响 seam 判定。

//   向下滚接缝在重叠区尾端、向上滚在头端；吸顶/吸底栏两帧同位必匹配，被其遮挡后

//            新增块直接续接在接缝窗之后，此处错位的视觉瑕疵最明显，需单独达标；

//   seam   ：接缝窗（靠近新增块一端 LC_SEAM_ROWS 行，随重叠折半封顶）匹配权重占比——

//   overall：全区匹配权重占比——衡量对齐的整体可信度；

// 候选位移全量验证：遍历整个重叠区逐行加权匹配。

static bool LongCaptureVerifyShiftRange(const LongMatchData& prevM, const LongMatchData& currM,
                                        int c0, int c1, const uint8_t* weight,
                                        int d, float& overall, float& seam) {
    int ad = d > 0 ? d : -d;
    int cols = currM.cols;
    int o = currM.h - ad;
    int cStart = d < 0 ? ad : 0;
    int win = (std::min)(LC_SEAM_ROWS, o / 2);
    int seamLo = d > 0 ? cStart + o - win : cStart;   // 接缝窗在 curr 内的行区间 [seamLo, seamHi)
    int seamHi = seamLo + win;
    int goodAll = 0, wAll = 0, goodSeam = 0, wSeam = 0;
    for (int c = cStart; c < cStart + o; c++) {
        int wt = weight[c];
        if (wt <= 0) continue;
        bool ok = LongCaptureRowMatchesRange(
            prevM.gray.data() + (size_t)(d + c) * cols,
            currM.gray.data() + (size_t)c * cols, c0, c1);
        goodAll += ok ? wt : 0;
        wAll += wt;
        if (c >= seamLo && c < seamHi) {
            goodSeam += ok ? wt : 0;
            wSeam += wt;
        }
    }
    if (wAll < LC_VERIFY_MIN_WEIGHT) return false;
    overall = (float)goodAll / (float)wAll;
    // 接缝窗内无有效行（连接处为纯色/空白）时不再一票否决：空白接缝在视觉上
    // 不可能错位，行稀疏内容页的接缝窗常常全空白（旧实现在此整帾拒绝）；
    // seam 指标退化为继承整段 overall 作为最可得的质量代理，下游 seam 阈值对
    // 「有证据的接缝」约束力不变。
    seam = wSeam > 0 ? (float)goodSeam / (float)wSeam : overall;
    return true;
}

// 峰邻 ±1 精搜（粗搜→精搜的收尾一级）：扫描探针分数只负责筛峰，亚像素滚动/
// 探针行稀疏时峰值本身可能偏 1px。对峰 d 及 d±1 各做一次整段验证（同
// LongCaptureVerifyShiftRange 的 overall/seam 双指标），取 overall 最优（并列时
// seam 更高）者；邻域点越出本档重叠区间 [ovMin, ovMax] 时跳过（不越档）。
// 返回 false = 邻域内无可评位移。bestD/overall/seam 输出精搜后的最优位移与指标。
static bool LongCaptureVerifyShiftRefined(const LongMatchData& prevM, const LongMatchData& currM,
                                          int c0, int c1, const uint8_t* weight,
                                          int d, int ovMin, int ovMax,
                                          int& bestD, float& overall, float& seam) {
    bestD = d;
    bool any = false;
    float bestOverall = 0.0f, bestSeam = 0.0f;
    for (int dd = d - 1; dd <= d + 1; dd++) {
        int ad = dd > 0 ? dd : -dd;
        int o = currM.h - ad;
        if (o < ovMin || o > ovMax) continue;
        float ov = 0.0f, sm = 0.0f;
        if (!LongCaptureVerifyShiftRange(prevM, currM, c0, c1, weight, dd, ov, sm)) continue;
        if (!any || ov > bestOverall + 0.001f
            || (ov > bestOverall - 0.001f && sm > bestSeam)) {
            any = true;
            bestD = dd;
            bestOverall = ov;
            bestSeam = sm;
        }
    }
    if (!any) return false;
    overall = bestOverall;
    seam = bestSeam;
    return true;
}

// ==================== 识别阶段（纯函数：不修改任何拼接状态） ====================

// 轻量位移先验（明确不引入 ECC / dense optical flow / phase correlation 等重型配准）：
// 候选幅度综合两个低成本来源——
//   ① 滚轮累计增量 × 在线 px/notch 估计（估计可用且有待消化增量时）；
//   ② 最近成功视觉位移中位数（估计尚不可用时的退路——滚动节奏通常稳定）。
// 只作为同档候选的排序加分（LC_WHEEL_PRIOR_BONUS）与失败帧的预测幅度来源，
// 绝不变质为 expectedOffset±fixedRange 硬约束——Windows/浏览器/页面平滑滚动/触控板

// 重叠区行边缘结构强度的归一化相关度（加权 Pearson，权重 = curr 行权重，空白行不计）：
// Weak Match 的辅助强证据——量化灰度逐行匹配可被大面积纯色区“偶然通过”，
// 而行结构强度曲线在真对齐处应逐行同起伏。有效权重不足或方差为零（曲线平坦）
// 时无可判依据，返回 0（无法作为强证据）。
static float LongCaptureEdgeCorrelation(const LongMatchData& prevM, const LongMatchData& currM,
                                        const uint8_t* weight, int d) {
    int h = currM.h;
    int ad = d > 0 ? d : -d;
    int o = h - ad;
    int cStart = d < 0 ? ad : 0;
    double wSum = 0, sa = 0, sb = 0;
    for (int c = cStart; c < cStart + o; c++) {
        double w = weight[c];
        if (w <= 0) continue;
        wSum += w;
        sa += w * prevM.edge[(size_t)(d + c)];
        sb += w * currM.edge[(size_t)c];
    }
    if (wSum < LC_VERIFY_MIN_WEIGHT) return 0.0f;
    double ma = sa / wSum, mb = sb / wSum;
    double cov = 0, va = 0, vb = 0;
    for (int c = cStart; c < cStart + o; c++) {
        double w = weight[c];
        if (w <= 0) continue;
        double da = (double)prevM.edge[(size_t)(d + c)] - ma;
        double db = (double)currM.edge[(size_t)c] - mb;
        cov += w * da * db;
        va += w * da * da;
        vb += w * db * db;
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0f;
    float corr = (float)(cov / std::sqrt(va * vb));
    if (corr > 1.0f) corr = 1.0f;
    if (corr < -1.0f) corr = -1.0f;
    return corr;
}

// 单尺度聚合 profile 的加权 Pearson 相关度（权重 = curr 行权重，空白行不计）：
// 把重叠区每个有效行的 LC_PROFILE_BUCKETS 个桶值视作联合序列，衡量「整段垂直
// 明暗结构」在候选位移处是否同起伏。有效样本不足或方差为零（结构平坦）时
// 无可判依据，返回 0；负相关按零证据处理（结构反向不可能来自同一内容段）。
static float LongCaptureProfileCorrOne(const LongMatchData& prevM, const LongMatchData& currM,
                                       const uint8_t* weight, int d,
                                       const std::vector<float>& profPrev,
                                       const std::vector<float>& profCurr, int win) {
    int h = currM.h;
    int ad = d > 0 ? d : -d;
    int o = h - ad;
    if (o <= win || profPrev.empty() || profCurr.empty()) return 0.0f;
    int cStart = d < 0 ? ad : 0;
    int maxIdx = (std::max)(0, h - win);   // 帧尾不足一个聚合窗口的行共用末窗口
    double wSum = 0, sa = 0, sb = 0;
    for (int c = cStart; c < cStart + o; c++) {
        double w = weight[c];
        if (w <= 0) continue;
        int pa = (std::min)((std::max)(d + c, 0), maxIdx);
        int pb = (std::min)(c, maxIdx);
        for (int b = 0; b < LC_PROFILE_BUCKETS; b++) {
            wSum += w;
            sa += w * profPrev[(size_t)pa * LC_PROFILE_BUCKETS + b];
            sb += w * profCurr[(size_t)pb * LC_PROFILE_BUCKETS + b];
        }
    }
    if (wSum < (double)LC_VERIFY_MIN_WEIGHT * LC_PROFILE_BUCKETS) return 0.0f;
    double ma = sa / wSum, mb = sb / wSum;
    double cov = 0, va = 0, vb = 0;
    for (int c = cStart; c < cStart + o; c++) {
        double w = weight[c];
        if (w <= 0) continue;
        int pa = (std::min)((std::max)(d + c, 0), maxIdx);
        int pb = (std::min)(c, maxIdx);
        for (int b = 0; b < LC_PROFILE_BUCKETS; b++) {
            double da = (double)profPrev[(size_t)pa * LC_PROFILE_BUCKETS + b] - ma;
            double db = (double)profCurr[(size_t)pb * LC_PROFILE_BUCKETS + b] - mb;
            cov += w * da * db;
            va += w * da * da;
            vb += w * db * db;
        }
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0f;
    float corr = (float)(cov / std::sqrt(va * vb));
    if (corr > 1.0f) corr = 1.0f;
    if (corr < 0.0f) corr = 0.0f;
    return corr;
}

// 多尺度垂直结构一致度 = 4 行与 8 行聚合 profile 相关度的均值（辅助证据，不替换
// 灰度匹配）：局部像素巧合在粗尺度上通常失去结构对应，真对齐则两个尺度都一致。
static float LongCaptureProfileCorrelation(const LongMatchData& prevM, const LongMatchData& currM,
                                           const uint8_t* weight, int d) {
    float c4 = LongCaptureProfileCorrOne(prevM, currM, weight, d,
                                         prevM.profile4, currM.profile4, 4);
    float c8 = LongCaptureProfileCorrOne(prevM, currM, weight, d,
                                         prevM.profile8, currM.profile8, 8);
    return 0.5f * (c4 + c8);
}

// 候选位移的全宽富验证（识别阶段最终裁决的证据来源）：遍历整个重叠区逐行加权匹配，
// 一次扫描同时产出 overall/seam/三段分布/连续段统计。空白行（权重 0）不计入任何
// 比率、也不打断连续段——它既不能证明也不能证伪对齐。动态变化区行（局部连续失配
// 带，识别规则见常量块）与空白行同等对待：不参与比率、不打断连续段；屏蔽受单段
// 行数与总量占比双重硬上限约束，大面积异常绝不忽略，剔除也绝不产生正向证据。
// 返回 false = 有效权重不足（重叠区几乎全空白），该候选不可评。
bool LongCaptureVerifyCandidate(const LongMatchData& prevM, const LongMatchData& currM,
                                       int d, LCOverlapEvidence& ev) {
    int h = currM.h;
    int cols = currM.cols;
    int ad = d > 0 ? d : -d;
    int o = h - ad;
    if (o < 2 || prevM.cols != cols || prevM.h != h) return false;
    int cStart = d < 0 ? ad : 0;
    const uint8_t* weight = currM.weight.data();
    int win = (std::min)(LC_SEAM_ROWS, o / 2);
    int seamLo = d > 0 ? cStart + o - win : cStart;   // 接缝窗在 curr 内的行区间
    // 第一遍：逐行匹配，行状态落盘（0=空白无证据 1=有效失配 2=有效匹配；3=动态区
    // 屏蔽由第二遍回填）。单字节 × 帧高的行结果缓冲，成本可忽略。
    std::vector<uint8_t> state((size_t)h, 0);
    int informative = 0;
    for (int c = cStart; c < cStart + o; c++) {
        if (weight[c] <= 0) continue;              // 空白行：无证据
        bool ok = LongCaptureRowMatchesRange(
            prevM.gray.data() + (size_t)(d + c) * cols,
            currM.gray.data() + (size_t)c * cols, 0, cols);
        state[c] = ok ? 2 : 1;
        informative++;
    }
    // 第二遍：动态变化区识别（局部连续失配带，允许少量空白桥接——动态区内常夹留白）。
    // 候选屏蔽段必须同时满足：段内失配行数 ∈ [LC_DYNAMIC_MIN_ROWS, LC_DYNAMIC_MAX_ROWS]，
    // 且全部候选段失配行总数 ≤ 有效行数 × LC_DYNAMIC_MAX_FRACTION——任一不满足即完全
    // 不屏蔽（宁可保守计入失配，也不给错误 offset 借局部屏蔽通过验收的空间）。
    int maskedRows = 0;
    {
        int maskBudget = (int)((float)informative * LC_DYNAMIC_MAX_FRACTION);
        struct LCMismatchRun { int lo, hi, len; };   // [lo,hi] 行闭区间；len = 段内失配行数
        std::vector<LCMismatchRun> runs;
        int i = cStart;
        while (i < cStart + o) {
            if (state[i] != 1) { i++; continue; }
            int j = i, len = 1, k = i + 1;
            while (k < cStart + o) {
                if (state[k] == 1) {                // 连续失配：段延伸
                    j = k; len++; k++; continue;
                }
                if (state[k] == 0) {                // 空白桥接：段内小片留白不断开动态区
                    int b = k;
                    while (b < cStart + o && state[b] == 0) b++;
                    if (b < cStart + o && state[b] == 1 && b - k <= LC_DYNAMIC_BRIDGE) {
                        j = b; k = b + 1;
                        continue;
                    }
                }
                break;                              // 匹配行 / 超长空白：段终止
            }
            if (len >= LC_DYNAMIC_MIN_ROWS && len <= LC_DYNAMIC_MAX_ROWS)
                runs.push_back({i, j, len});
            i = j + 1;
        }
        int total = 0;
        for (const LCMismatchRun& r : runs) total += r.len;
        if (total > 0 && total <= maskBudget) {
            for (const LCMismatchRun& r : runs)
                for (int c = r.lo; c <= r.hi; c++)
                    if (state[c] == 1) { state[c] = 3; maskedRows++; }
        }
    }
    // 第三遍：聚合统计（空白行与动态屏蔽行同为「无证据」：不参与比率、不打断连续段）。
    long long goodAll = 0, wAll = 0, goodSeam = 0, wSeam = 0;
    long long partGood[3] = {0, 0, 0}, partW[3] = {0, 0, 0};
    int used = 0, run = 0, bestRun = 0, gaps = 0;
    bool lastOk = false;
    for (int c = cStart; c < cStart + o; c++) {
        int wt = weight[c];
        if (wt <= 0 || state[c] == 3) continue;
        bool ok = state[c] == 2;
        used++;
        if (ok) {
            run++;
            if (run > bestRun) bestRun = run;
        } else {
            run = 0;
            if (lastOk) gaps++;             // 匹配带被切断：连续匹配分布出现断点
        }
        lastOk = ok;
        goodAll += ok ? wt : 0;
        wAll += wt;
        if (c >= seamLo && c < seamLo + win) {
            goodSeam += ok ? wt : 0;
            wSeam += wt;
        }
        int third = (c - cStart) * 3 / (std::max)(1, o);
        if (third > 2) third = 2;
        partGood[third] += ok ? wt : 0;
        partW[third] += wt;
    }
    if (wAll < LC_VERIFY_MIN_WEIGHT) return false;
    ev.valid = true;
    ev.textureRatio = (float)used / (float)o;   // 评分基准（剔除动态行后）的有效行占比
    ev.dynamicMaskedRows = maskedRows;
    ev.overall = (float)((double)goodAll / (double)wAll);
    // 同 LongCaptureVerifyShiftRange：接缝窗全空白（无有效行）时 seam 继承 overall，
    // 不再一票否决——空白连接处无错位可言，行稀疏内容页不应因此整帾被拒。
    ev.seam = wSeam > 0 ? (float)((double)goodSeam / (double)wSeam) : ev.overall;
    int have = 0;
    float minv = 1.0f, sum = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (partW[i] > 0) {
            ev.part[i] = (float)((double)partGood[i] / (double)partW[i]);
            if (ev.part[i] < minv) minv = ev.part[i];
            sum += ev.part[i];
            have++;
        } else {
            ev.part[i] = -1.0f;             // 该段几乎全空白：无证据（中性，不参与统计）
        }
    }
    // 空间一致性：多个位置都有稳定证据才得高分（min 与均值折中，弱段拖累总分）；
    // 只有一段有证据时对折（覆盖面不足），top=.92 middle=.31 bottom=.26 型的
    // 局部假匹配在这里崩塌，而 .88/.91/.87 型的强候选不受影响。
    ev.spatial = have >= 2 ? 0.5f * (minv + sum / (float)have)
                           : (have == 1 ? 0.5f * sum : 0.0f);
    ev.longestRunRatio = used > 0 ? (float)bestRun / (float)used : 0.0f;
    ev.gapCount = gaps;
    float gapNorm = (std::min)(1.0f, (float)gaps / LC_CONTINUITY_GAP_MAX);
    ev.continuity = 0.6f * ev.longestRunRatio + 0.4f * ev.overall - 0.15f * gapNorm;
    if (ev.continuity < 0.0f) ev.continuity = 0.0f;
    if (ev.continuity > 1.0f) ev.continuity = 1.0f;
    ev.profileScore = LongCaptureProfileCorrelation(prevM, currM, weight, d);
    ev.edgeScore = LongCaptureEdgeCorrelation(prevM, currM, weight, d);
    return true;
}

// Weak 档动态置信度门槛：overlap 越小要求越强——基线 LC_WEAK_MIN_CONFIDENCE，
// overlap 从 Normal 下限降至 Weak 下限线性加罚至 LC_WEAK_CONF_EXTRA_PENALTY。
// 视口过小导致区间退化时取最大加罚（最保守）。
float LongCaptureWeakRequiredConfidence(int viewportH, int overlap) {
    float lo = (float)((std::max)(LC_WEAK_MIN_OVERLAP, (int)(viewportH * LC_WEAK_MIN_OVERLAP_RATIO)));
    float hi = (float)((std::max)(LC_MIN_OVERLAP, (int)(viewportH * LC_MIN_OVERLAP_RATIO)));
    if (hi <= lo) return LC_WEAK_MIN_CONFIDENCE + LC_WEAK_CONF_EXTRA_PENALTY;
    float t = ((float)overlap - lo) / (hi - lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return LC_WEAK_MIN_CONFIDENCE + (1.0f - t) * LC_WEAK_CONF_EXTRA_PENALTY;
}

// 在限定列区间 [c0,c1) 与重叠区间 [ovMin,ovMax] 内求解两帧间的候选集（单 ROI
// 证据入口）：扫描收集候选峰（粗搜）→ 峰邻 ±1 整段验证取最优（精搜）→ 严格档
// 优先、按 overall 降序保留至多 LC_BAND_TOP_N 个彼此距离 > LC_PEAK_WIN 的候选。
// 方向罚/滚轮先验/歧义取舍统一移到最终选优（LongCaptureDetectPass）：
//   · 单 ROI 的局部歧义（相似纹理/重复代码行/头像列等造成的假峰）没有资格在
//     这里被否决或放大——它只是证据之一，交给跨 ROI 聚类与全宽空间一致性裁决；
//   · 周期性内容的多峰接近也不再在此预选，由峰值分离度在最终置信度中统一处理。
static void LongCaptureBestShiftInRange(const LongMatchData& prevM, const LongMatchData& currM,
                                        int c0, int c1, const uint8_t* weight,
                                        int ovMin, int ovMax,
                                        std::vector<LCBandCandidate>& out) {
    out.clear();
    std::vector<int> cands;
    LongCaptureScanCandidatesRange(prevM, currM, c0, c1, weight, ovMin, ovMax, cands);
    std::vector<LCBandCandidate> tiers[2];          // [0]=严格档验收 [1]=宽松档验收
    for (int d : cands) {
        float overall = 0.0f, seam = 0.0f;
        int dr = d;
        if (!LongCaptureVerifyShiftRefined(prevM, currM, c0, c1, weight, d, ovMin, ovMax,
                                           dr, overall, seam))
            continue;
        if (overall >= LC_ACCEPT_STRICT_OVERALL && seam >= LC_ACCEPT_STRICT_SEAM)
            tiers[0].push_back({dr, overall, true});
        else if (overall >= LC_ACCEPT_LOOSE_OVERALL && seam >= LC_ACCEPT_LOOSE_SEAM)
            tiers[1].push_back({dr, overall, false});
    }
    for (int tier = 0; tier < 2 && (int)out.size() < LC_BAND_TOP_N; tier++) {
        std::vector<LCBandCandidate>& list = tiers[tier];
        std::stable_sort(list.begin(), list.end(),
                         [](const LCBandCandidate& a, const LCBandCandidate& b) {
                             return a.overall > b.overall;
                         });
        for (const LCBandCandidate& c : list) {
            if ((int)out.size() >= LC_BAND_TOP_N) break;
            bool dup = false;                       // ±LC_PEAK_WIN 内视为同一次对齐的亚像素抖动
            for (const LCBandCandidate& e : out) {
                int diff = c.d > e.d ? c.d - e.d : e.d - c.d;
                if (diff <= LC_PEAK_WIN) { dup = true; break; }
            }
            if (!dup) out.push_back(c);
        }
    }
}

// 单档识别（Normal 或 WeakOverlap），六段式流程。核心原则：
//   局部区域匹配得很好 ≠ 重叠区域匹配正确。
// ROI 只提供证据，最终结论由「全宽空间一致性优先」的综合置信度决定：
//   ① 各 ROI 产出 Top-N 候选（LongCaptureBestShiftInRange），不再单 ROI 定生死，
//      也不再要求「多数派一致，否则直接 FAIL」；
//   ② ROI 信息量加权：结构丰富（带内有效行细节多）的 ROI 高权重，大量空白/
//      低信息量的 ROI 只有保底权重（LC_ROI_MIN_INFO_WEIGHT），没有等权投票权；
//   ③ 候选按 offset 聚类（容差 = ROI 一致性容差，Weak 用更紧的全票容差），相近
//      offset 的 ROI 证据加权融合为「聚类支持度」；
//   ④ Top 聚类做全宽富验证（LongCaptureVerifyCandidate）：overall/seam 之外，
//      top/middle/bottom 三段空间一致性 + 匹配分布连续性（最长连续段/断点数）+
//      4/8 行聚合 profile 多尺度结构一致度 + 行边缘结构相关度全部进入评分；
//   ⑤ 最终置信度 = 全宽匹配 + 空间一致性 + 连续性 + 多尺度结构 + ROI 加权证据 +
//      候选峰值分离度的加权组合 − 有效纹理占比折扣（大面积空白重叠证据量打折）；
//      第一/第二峰接近（周期性/重复内容歧义）时加罚，
//      并在分差 < 歧义裕度的远距峰之间偏向重叠更大的候选；
//   ⑤+ 局部 offset basin 择优：胜出候选 ±LC_BASIN_RADIUS 内全宽富验证取最优证据，
//      吸收亚像素滚动/抗锯齿/量化的 1~2px 峰漂移（只做局部择优，绝不合并远距峰，
//      验收阈值不变——跨采样候选因此更稳定，Weak 延迟确认的复现一致性同步受益）；
//   ⑥ Weak 档证据链全面收紧：3/3 ROI 全票聚类 + 更高 overall/seam/edge 阈值 +
//      spatial/continuity/profile 下限 + 峰值分离度下限 + 随 overlap 缩小动态
//      升高的置信度门槛。
// 任何一关不过都不是 SUCCESS——LOW_CONFIDENCE / FAILED 一律按 LCFailReason 细分类
// 记录（NO_CANDIDATE / SEAM_MISMATCH / ...，见枚举定义）后走重试，
// 绝不拿候选 offset 或兜底值拼接（更不允许回到「失败整帧兜底」的老路）。
// offset==0 且 SUCCESS 表示内容实际未滚动（仅光标闪烁等微差 |d|≤1），无需追加。

LongMatchOutcome LongCaptureDetectPass(const LongMatchData& prevM,
                                              const LongMatchData& currM, int dir,
                                              const LongCaptureOffsetPrior& prior, bool weak,
                                              const LCWeakTemporal& wt, int logId) {
    LongMatchOutcome m;
    m.mode = weak ? LongCaptureMatchMode::WeakOverlap : LongCaptureMatchMode::Normal;
    int h = currM.h;
    if (h < 2 || prevM.cols != currM.cols || prevM.h != h) {
        m.reason = LCFailReason::NoCandidate;      // 帧尺寸不一致：无可对齐依据
        return m;
    }
    if (currM.detailSum < 16) {
        m.reason = LCFailReason::NoCandidate;      // 整帧近乎均匀（空白）：无对齐依据
        return m;
    }

    // 本档重叠区间：overlap ∈ [ovMin, ovMax]。Normal 覆盖 [Normal 下限, h]；
    // Weak 只覆盖 [Weak 下限, Normal 下限)——两档不重叠，模式由重叠量唯一决定。
    int ovMin = weak ? (std::max)(LC_WEAK_MIN_OVERLAP, (int)(h * LC_WEAK_MIN_OVERLAP_RATIO))
                     : (std::max)(LC_MIN_OVERLAP, (int)(h * LC_MIN_OVERLAP_RATIO));
    int ovMax = weak ? (std::max)(LC_MIN_OVERLAP, (int)(h * LC_MIN_OVERLAP_RATIO)) - 1 : h;
    if (ovMax < ovMin) {                          // 视口过小：弱重叠区间不存在（Weak 档无候选）
        m.reason = LCFailReason::NoCandidate;
        return m;
    }

    // ① 各 ROI 独立产出 Top-N 候选（证据收集，不做硬否决）
    std::vector<LCBandCandidate> bandCands[LC_ROI_BANDS];
    for (int b = 0; b < LC_ROI_BANDS; b++) {
        LongCaptureBestShiftInRange(prevM, currM,
                                    currM.bandStart[b],
                                    currM.bandStart[b] + currM.bandCols[b],
                                    currM.bandWeight[b].data(), ovMin, ovMax, bandCands[b]);
        if (!bandCands[b].empty()) {
            m.bandValid[b] = true;
            m.bandOffsets[b] = bandCands[b][0].d;
            m.validBandCount++;
        }
    }
    if (m.validBandCount == 0) {                  // 全部 ROI 无候选：FAILED
        m.reason = LCFailReason::NoCandidate;
        return m;
    }

    // ② ROI 信息量权重：带内有效行权重总和越高（结构越丰富、有效行越多）权重越大；
    //    空白/重复纹理等低信息 ROI 只有保底权重——不再与高信息 ROI 等权投票。
    float bandW[LC_ROI_BANDS];
    {
        double raw[LC_ROI_BANDS];
        double maxRaw = 0;
        for (int b = 0; b < LC_ROI_BANDS; b++) {
            double s = 0;
            const uint8_t* bw = currM.bandWeight[b].data();
            for (int r = 0; r < h; r++) s += bw[r];
            raw[b] = s;
            if (s > maxRaw) maxRaw = s;
        }
        for (int b = 0; b < LC_ROI_BANDS; b++)
            bandW[b] = maxRaw > 0
                ? LC_ROI_MIN_INFO_WEIGHT
                  + (1.0f - LC_ROI_MIN_INFO_WEIGHT) * (float)(raw[b] / maxRaw)
                : 1.0f;
    }
#ifdef LC_DEBUG_LOG
    for (int b = 0; b < LC_ROI_BANDS; b++) {
        char cb[128];
        int off = 0;
        for (size_t k = 0; k < bandCands[b].size() && off < (int)sizeof(cb) - 32; k++)
            off += snprintf(cb + off, sizeof(cb) - off, " d=%d(%.2f%s)",
                            bandCands[b][k].d, bandCands[b][k].overall,
                            bandCands[b][k].strict ? ",s" : ",l");
        LC_LOG("[LC#%d] %s band%d w=%.2f cands:%s", logId, weak ? "WEAK" : "NORM",
               b, bandW[b], bandCands[b].empty() ? " none" : cb);
    }
#endif

    // ③ 跨 ROI 候选聚类：相近 offset 的证据融合为一簇（亚像素抖动归并），
    //    聚类位移取成员中位数（鲁棒统计，离群单票不牵动中心），
    //    支持度 = Σ(ROI 权重 × 带内最高匹配率) / Σ权重。
    struct ClusterEntry { int d; int band; float overall; bool strict; };
    std::vector<ClusterEntry> all;
    for (int b = 0; b < LC_ROI_BANDS; b++)
        for (const LCBandCandidate& c : bandCands[b])
            all.push_back({c.d, b, c.overall, c.strict});
    std::sort(all.begin(), all.end(),
              [](const ClusterEntry& a, const ClusterEntry& b) { return a.d < b.d; });
    struct LCCluster {
        int offset = 0;
        int support = 0;                          // 支持 ROI 数（有无候选，不含权重）
        float roiWeighted = 0.0f;                 // ROI 加权证据 [0,1]
        float supportScore[LC_ROI_BANDS] = {0, 0, 0};
        bool strictMember = false;                // 是否有成员达到严格档
    };
    int clusterTol = weak ? LC_WEAK_ROI_TOLERANCE : LC_ROI_OFFSET_TOLERANCE;
    std::vector<LCCluster> clusters;
    for (size_t i = 0; i < all.size();) {
        LCCluster cl;
        std::vector<int> ds;
        size_t j = i;
        while (j < all.size() && (ds.empty() || all[j].d - ds.back() <= clusterTol)) {
            ds.push_back(all[j].d);
            if (all[j].overall > cl.supportScore[all[j].band])
                cl.supportScore[all[j].band] = all[j].overall;
            if (all[j].strict) cl.strictMember = true;
            j++;
        }
        std::sort(ds.begin(), ds.end());
        cl.offset = ds[ds.size() / 2];            // 组内中位数（鲁棒统计）
        float num = 0, den = 0;
        for (int b = 0; b < LC_ROI_BANDS; b++) {
            den += bandW[b];
            if (cl.supportScore[b] > 0) {
                num += bandW[b] * (std::min)(1.0f, cl.supportScore[b]);
                cl.support++;
            }
        }
        cl.roiWeighted = den > 0 ? num / den : 0;
        clusters.push_back(cl);
        i = j;
    }
    std::stable_sort(clusters.begin(), clusters.end(),
                     [](const LCCluster& a, const LCCluster& b) {
                         if (a.roiWeighted != b.roiWeighted) return a.roiWeighted > b.roiWeighted;
                         if (a.support != b.support) return a.support > b.support;
                         int aa = a.offset > 0 ? a.offset : -a.offset;
                         int ab = b.offset > 0 ? b.offset : -b.offset;
                         return aa < ab;           // 融合分并列时偏向重叠更大（|d| 更小）
                     });

    // ④ Top 聚类全宽富验证 + 综合分（不含分离度项）。全宽空间一致性是主要指标：
    //    半幅各配不同内容、只有局部巧合匹配的候选，spatial/continuity/profile
    //    会全面崩塌，即使 overall 因高分峰集中而不低也拿不到综合分。
    //    排序分 = 综合分 + 滚轮先验加分 − 反向候选罚（只影响排序，不影响验收阈值）。
    //    Weak 档：仅全票（3/3 ROI）聚类可胜出，其余只作竞争峰基线参与分离度计算。
    struct FinalCand {
        int d = 0;
        LCOverlapEvidence ev{};
        float roiWeighted = 0;
        int support = 0;
        bool strictMember = false;
        bool eligible = true;
        float baseConf = 0;
        float rank = 0;
    };
    std::vector<FinalCand> finals;
    std::vector<char> attempted(clusters.size(), 0);   // 已进入富验证的聚类（含验证失败者）
    // 综合分公式（不含分离度项）：候选富验证证据 + ROI 加权证据的加权组合，
    // verifyAndPush 主路径与下方 basin 择优后的重算共用同一份权重。
    auto baseConfOf = [weak](const LCOverlapEvidence& ev, float roiW) {
        return weak
            ? 0.22f * ev.overall + 0.11f * ev.seam + 0.15f * ev.spatial
              + 0.13f * ev.continuity + 0.15f * ev.profileScore + 0.14f * roiW
            : 0.28f * ev.overall + 0.14f * ev.seam + 0.16f * ev.spatial
              + 0.12f * ev.continuity + 0.10f * ev.profileScore + 0.12f * roiW;
    };
    // 单聚类 → 富验证候选的统一入口（下方 Top-N 主路径与先验补位共用）。
    auto verifyAndPush = [&](size_t ci) {
        FinalCand f;
        if (!LongCaptureVerifyCandidate(prevM, currM, clusters[ci].offset, f.ev)) return;
        f.d = clusters[ci].offset;
        f.roiWeighted = clusters[ci].roiWeighted;
        f.support = clusters[ci].support;
        f.strictMember = clusters[ci].strictMember;
        f.eligible = !weak || f.support == LC_ROI_BANDS;
        f.baseConf = baseConfOf(f.ev, f.roiWeighted);
        f.rank = f.baseConf;
        bool opposite = (dir != 0) && ((f.d > 0) == (dir < 0));
        if (opposite) f.rank -= LC_DIR_PENALTY;
        if (prior.valid) {
            int ad = f.d > 0 ? f.d : -f.d;
            int tol = (std::max)(8, prior.expectedAbsOffset / 4);
            int ed = ad > prior.expectedAbsOffset ? ad - prior.expectedAbsOffset
                                                  : prior.expectedAbsOffset - ad;
            if (ed <= tol) f.rank += LC_WHEEL_PRIOR_BONUS;
        }
        finals.push_back(f);
        LC_LOG("[LC#%d] %s cand d=%d ov=%d overall=%.2f seam=%.2f tmb=%.2f/%.2f/%.2f "
               "run=%.2f gaps=%d cont=%.2f prof=%.2f edge=%.2f tex=%.2f dyn=%d "
               "roiW=%.2f sup=%d base=%.2f",
               logId, weak ? "WEAK" : "NORM", f.d, h - (f.d > 0 ? f.d : -f.d),
               f.ev.overall, f.ev.seam,
               (std::max)(0.0f, f.ev.part[0]), (std::max)(0.0f, f.ev.part[1]),
               (std::max)(0.0f, f.ev.part[2]),
               f.ev.longestRunRatio, f.ev.gapCount, f.ev.continuity, f.ev.profileScore,
               f.ev.edgeScore, f.ev.textureRatio, f.ev.dynamicMaskedRows,
               f.roiWeighted, f.support, f.baseConf);
    };
    for (size_t ci = 0; ci < clusters.size() && (int)finals.size() < LC_MAX_VERIFY_CANDIDATES; ci++) {
        attempted[ci] = 1;
        verifyAndPush(ci);
    }
    // 位移预测指引的验证补位（快速滚动支持）：重复内容页上真峰的 ROI 融合分可能
    // 排在 Top-N 之外而从未进入富验证。把与预测位移（滚轮增量估计 / 历史成功位移
    // 中位数，见 LongCaptureBuildOffsetPrior）最接近且尚未验证的最高分聚类补验
    // 一个——只扩大验证覆盖（至多多验一个聚类），不放宽任何验收阈值；先验仍然
    // 只是排序/覆盖提示，绝不是搜索范围硬约束。
    if (prior.valid) {
        int tol = (std::max)(8, prior.expectedAbsOffset / 4);
        int pick = -1;
        for (size_t ci = 0; ci < clusters.size(); ci++) {
            if (attempted[ci]) continue;
            int ad = clusters[ci].offset > 0 ? clusters[ci].offset : -clusters[ci].offset;
            int ed = ad > prior.expectedAbsOffset ? ad - prior.expectedAbsOffset
                                                  : prior.expectedAbsOffset - ad;
            if (ed <= tol && (pick < 0 || clusters[ci].roiWeighted > clusters[pick].roiWeighted))
                pick = (int)ci;
        }
        if (pick >= 0) verifyAndPush((size_t)pick);
    }
    if (finals.empty()) {                         // 全宽不可评（重叠区几乎全空白）：FAILED
        m.reason = LCFailReason::GlobalMismatch;
        return m;
    }
    bool anyEligible = false;
    for (const FinalCand& f : finals)
        if (f.eligible) { anyEligible = true; break; }
    if (!anyEligible) {                           // Weak 全票不成立：直接拒绝（无多数派概念）
        m.reason = LCFailReason::RoiInconsistent;
        return m;
    }

    // ⑤ 最终选优：排序分最高者胜出；分差 < 歧义裕度的远距峰之间偏向重叠更大者
    //   （周期性列表多峰得分接近，取大重叠少漏拼——与全宽复核共同裁决）。
    int best = -1;
    for (int i = 0; i < (int)finals.size(); i++) {
        if (!finals[i].eligible) continue;
        if (best < 0 || finals[i].rank > finals[best].rank) best = i;
    }
    for (int i = 0; i < (int)finals.size(); i++) {
        if (!finals[i].eligible || i == best) continue;
        int diff = finals[i].d > finals[best].d ? finals[i].d - finals[best].d
                                                : finals[best].d - finals[i].d;
        int ai = finals[i].d < 0 ? -finals[i].d : finals[i].d;
        int ab = finals[best].d < 0 ? -finals[best].d : finals[best].d;
        if (diff > LC_PEAK_WIN && finals[best].rank - finals[i].rank < LC_AMBIGUITY_MARGIN
            && ai < ab)
            best = i;                            // 歧义：改选重叠更大的峰
    }

    // ⑤+ 局部 offset basin 确认：胜出聚类中位数 ±LC_BASIN_RADIUS 内逐一做全宽富验证，
    // 取最优证据（overall 优先、seam 决胜）者作为最终位置——吸收亚像素滚动/抗锯齿/
    // 量化造成的 1~2px 峰漂移（d=417/418/419 属同一 basin，不应是三个独立候选）。
    // 邻域点越出本档重叠区间即跳过（不越档）；半径 ≤ 2~3px，绝不合并远距周期峰；
    // 验收阈值原样不动（择优后的证据照常走 ⑥ 验收，只会更难不会更易）。
    {
        FinalCand& win = finals[best];
        int d0 = win.d;
        float oldOverall = win.ev.overall, oldSeam = win.ev.seam;
        for (int dd = d0 - LC_BASIN_RADIUS; dd <= d0 + LC_BASIN_RADIUS; dd++) {
            if (dd == d0) continue;
            int ad2 = dd < 0 ? -dd : dd;
            int o2 = h - ad2;
            if (o2 < ovMin || o2 > ovMax) continue;
            LCOverlapEvidence ev2;
            if (!LongCaptureVerifyCandidate(prevM, currM, dd, ev2)) continue;
            // 切换必须是「严格更优」：overall 明显提升且 seam 不实质受损，或 overall
            // 持平下 seam 提升——绝不允许用 seam 换 overall（否则可能把本可通过验收
            // 的中位数候选换成不通过的邻点，引入回归）。
            bool better = (ev2.overall > win.ev.overall + 0.001f
                           && ev2.seam >= win.ev.seam - 0.02f)
                       || (ev2.overall > win.ev.overall - 0.001f
                           && ev2.seam > win.ev.seam + 0.001f);
            if (better) {
                win.d = dd;
                win.ev = ev2;
            }
        }
        if (win.d != d0) {                       // basin 内择优命中：综合分按新证据重算
            win.baseConf = baseConfOf(win.ev, win.roiWeighted);
            LC_LOG("[LC#%d] %s basin d=%d->%d overall=%.2f->%.2f seam=%.2f->%.2f",
                   logId, weak ? "WEAK" : "NORM", d0, win.d,
                   oldOverall, win.ev.overall, oldSeam, win.ev.seam);
        }
    }

    // ⑥ 峰值分离度：与最优相距 > LC_PEAK_WIN 的其余可胜出候选中最高综合分之差。
    //    分离度进入置信度（加分项），分差过小（周期性/重复内容歧义）额外加罚——
    //    这种情况绝不立即高置信放行，宁可降置信度走重试。
    float secondBase = -1.0f;
    for (int i = 0; i < (int)finals.size(); i++) {
        if (!finals[i].eligible || i == best) continue;
        int diff = finals[i].d > finals[best].d ? finals[i].d - finals[best].d
                                                : finals[best].d - finals[i].d;
        if (diff <= LC_PEAK_WIN) continue;
        if (finals[i].baseConf > secondBase) secondBase = finals[i].baseConf;
    }
    float sepRaw = secondBase < 0 ? LC_PEAK_SEP_FULL : finals[best].baseConf - secondBase;
    if (sepRaw < 0.0f) sepRaw = 0.0f;
    if (sepRaw > LC_PEAK_SEP_FULL) sepRaw = LC_PEAK_SEP_FULL;
    float sepNorm = sepRaw / LC_PEAK_SEP_FULL;
    float conf = finals[best].baseConf + (weak ? 0.10f : 0.08f) * sepNorm;
    if (sepRaw < LC_AMBIGUITY_MARGIN) conf -= LC_AMBIGUITY_CONF_PENALTY;
    // 有效纹理占比折扣（「匹配成功 ≠ 匹配可信」的最后一环）：重叠区大部分为空白
    // 时 overall/spatial 只由少数有效行支撑——大面积纯色区在错误位移处也可能偶然
    // 高分，证据量本身要计价。按缺失比例轻度衰减（texture→0 时最多约 −0.06）。
    if (finals[best].ev.textureRatio < LC_TEXTURE_CONF_FLOOR)
        conf -= (LC_TEXTURE_CONF_FLOOR - finals[best].ev.textureRatio) * LC_TEXTURE_CONF_SCALE;
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;

    const FinalCand& w = finals[best];
    const LCOverlapEvidence& ev = w.ev;
    m.offset = w.d;
    m.overlap = h - (w.d > 0 ? w.d : -w.d);
    m.overall = ev.overall;
    m.seam = ev.seam;
    m.top = (std::max)(0.0f, ev.part[0]);
    m.middle = (std::max)(0.0f, ev.part[1]);
    m.bottom = (std::max)(0.0f, ev.part[2]);
    m.spatial = ev.spatial;
    m.continuity = ev.continuity;
    m.profileScore = ev.profileScore;
    m.edgeCorrelation = ev.edgeScore;
    m.textureRatio = ev.textureRatio;
    m.peakGap = sepNorm;
    m.roiWeighted = w.roiWeighted;
    m.agreeCount = w.support;
    m.confidence = conf;
    LC_LOG("[LC#%d] %s pick d=%d sep=%.2f conf=%.2f%s", logId, weak ? "WEAK" : "NORM",
           w.d, sepNorm, conf, sepRaw < LC_AMBIGUITY_MARGIN ? " (ambiguous peaks)" : "");

    if (weak) {
        // ⑥-Weak：全票 + 高阈值 + 空间/连续性/多尺度结构/分离度下限 + 动态置信度门槛。
        float need = LongCaptureWeakRequiredConfidence(h, m.overlap);
        // Weak 时间一致性（短时序列共识）：多个相邻采样持续指向同一 offset 区间时给
        // 置信度加分。只加分——空间硬门槛与动态门槛数值原样不动，也绝不因
        // 「历史上一直是这个值」绕过下方验收直接提交。
        if (wt.active) {
            int tdiff = m.offset > wt.refOffset ? m.offset - wt.refOffset : wt.refOffset - m.offset;
            if (tdiff <= LC_WEAK_TEMPORAL_TOL && m.confidence < 1.0f) {
                m.confidence += wt.bonus;
                if (m.confidence > 1.0f) m.confidence = 1.0f;
            }
        }
        if (m.overall >= LC_WEAK_ACCEPT_OVERALL && m.seam >= LC_WEAK_ACCEPT_SEAM
            && m.edgeCorrelation >= LC_WEAK_ACCEPT_EDGE
            && m.spatial >= LC_WEAK_ACCEPT_SPATIAL && m.continuity >= LC_WEAK_ACCEPT_CONTINUITY
            && m.profileScore >= LC_WEAK_ACCEPT_PROFILE
            && sepRaw >= LC_WEAK_MIN_PEAK_SEP && conf >= need) {
            m.status = LC_MATCH_SUCCESS;
        } else {
            m.status = LC_MATCH_LOW_CONFIDENCE;  // 有候选但证据不足：交由重试，绝不拼接
            // 失败分类（首个未达标指标定类）：整体匹配/边缘结构/综合置信度不足 →
            // CANDIDATE_WEAK，其余指标各有专属类别，便于区分失败来源。
            if (m.overall < LC_WEAK_ACCEPT_OVERALL)
                m.reason = LCFailReason::CandidateWeak;
            else if (m.seam < LC_WEAK_ACCEPT_SEAM)
                m.reason = LCFailReason::SeamMismatch;
            else if (m.edgeCorrelation < LC_WEAK_ACCEPT_EDGE)
                m.reason = LCFailReason::CandidateWeak;
            else if (m.spatial < LC_WEAK_ACCEPT_SPATIAL)
                m.reason = LCFailReason::SpatialMismatch;
            else if (m.continuity < LC_WEAK_ACCEPT_CONTINUITY)
                m.reason = LCFailReason::ContinuityMismatch;
            else if (m.profileScore < LC_WEAK_ACCEPT_PROFILE)
                m.reason = LCFailReason::ProfileMismatch;
            else if (sepRaw < LC_WEAK_MIN_PEAK_SEP)
                m.reason = LCFailReason::PeakAmbiguous;
            else
                m.reason = LCFailReason::CandidateWeak;   // 剩余唯一失败原因：置信度不足
        }
        return m;
    }

    // ⑥-Normal：严格档 overall/seam + 可信来源 + 综合置信度三重把关。
    // 单 ROI 支持的聚类也可由全宽富复核裁决，但要求该 ROI 达到严格档（沿用
    // 「左右留白单栏文本页」的逃生通道）；多 ROI 加权支持本身就是强证据。
    bool strictPass = m.overall >= LC_ACCEPT_STRICT_OVERALL && m.seam >= LC_ACCEPT_STRICT_SEAM;
    bool credible = w.support >= 2 || w.strictMember;
    if (strictPass && credible && conf >= LC_MIN_CONFIDENCE) {
        m.status = LC_MATCH_SUCCESS;
        m.offset = (w.d < 0 ? -w.d : w.d) > 1 ? w.d : 0;
    } else {
        m.status = LC_MATCH_LOW_CONFIDENCE;      // 有候选但证据不足：交由重试，绝不拼接
        // 失败分类：整体匹配不足 → CANDIDATE_WEAK / 接缝不足 → SEAM_MISMATCH /
        // ROI 支持不足 → ROI_INCONSISTENT / 峰值歧义压低置信度 → PEAK_AMBIGUOUS。
        if (!strictPass)
            m.reason = m.overall < LC_ACCEPT_STRICT_OVERALL
                ? LCFailReason::CandidateWeak : LCFailReason::SeamMismatch;
        else if (!credible)
            m.reason = LCFailReason::RoiInconsistent;
        else
            m.reason = sepRaw < LC_AMBIGUITY_MARGIN
                ? LCFailReason::PeakAmbiguous : LCFailReason::CandidateWeak;
    }
    return m;
}

// 识别主入口（双档扫描，纯函数）：先 Normal 档，未 SUCCESS 再试 WeakOverlap 档
//（纯弱重叠区间 + 严格交叉验证 + 延迟确认 + 时间一致性加分）。Weak 只有在强证据下
// 才产出 SUCCESS；否则保留信息量更大的诊断结论返回——弱档候选置信度更高时返回弱档
// 结论（LOW_CONFIDENCE 比 FAILED / Normal 诊断信息量更大，供 Weak 时间一致性登记与
// 失败归因使用；两者对调用方同样只是「本帧被拒绝」）。logId 仅用于调试日志关联采样
// 序号（不参与任何判定）。
LongMatchOutcome LongCaptureDetectMatch(const LongMatchData& prevM,
                                               const LongMatchData& currM, int dir,
                                               const LongCaptureOffsetPrior& prior,
                                               const LCWeakTemporal& wt, int logId) {
    LongMatchOutcome m = LongCaptureDetectPass(prevM, currM, dir, prior, false, wt, logId);
    if (m.status == LC_MATCH_SUCCESS) return m;
    LongMatchOutcome w = LongCaptureDetectPass(prevM, currM, dir, prior, true, wt, logId);
    if (w.status == LC_MATCH_SUCCESS) return w;
    return w.confidence > m.confidence ? w : m;
}

// CR-018: 防常量漂移。LC_RETRY_DELAY_WEAK 按弱候选重试档分档，调用方以
// LC_WEAK_RETRY_ATTEMPTS 为循环上界索引 LC_RETRY_DELAY_WEAK[weakRetries++]，
// 档数不一致将越界读或漏掉档位。
static_assert(sizeof(LC_RETRY_DELAY_WEAK) / sizeof(LC_RETRY_DELAY_WEAK[0]) == LC_WEAK_RETRY_ATTEMPTS,
    "LC_RETRY_DELAY_WEAK dimension must equal LC_WEAK_RETRY_ATTEMPTS");
