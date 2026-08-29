// 长截图子系统跨平台内部头：纯数据类型（匹配数据 / 枚举 / 会话上下文）、子系统内共享
// 常量与跨文件函数声明。使用方：算法层 lc_match_core / lc_stitch_state（两平台编译）、
// macOS C ABI shim lc_bridge_mac、Windows IO/UI 层 lc_frame_io_windows / lc_panel_ui_windows /
// lc_toolbar_ui_windows / lc_session_windows；不对外暴露。
//
// 本头自包含，双平台共用同一份定义（2026-08 收口：原 Windows 侧 internal.h「长截图」
// 纯数据段与 macOS 侧 lc_algo_types.h 逐字镜像合并为这里的单一权威，镜像头与「同步
// 契约」随之删除——此前两处描述的是同一个算法层状态对象，漏同步会导致 macOS 与
// Windows 的算法层行为悄然分叉）。
//   · 平台类型经 lc_platform.h 统一提供：_WIN32 → windows.h 原生类型；非 Windows →
//     依赖剥离验证后的声明级占位（见该头头部的逐符号核对结论）。
//   · 纯算法层只依赖本头 + lc_platform.h，不触碰 internal.h（napi / GDI+ 依赖链）。
//   · LongCaptureContext 含 HDC/HBITMAP 等 GDI 成员，但它们是拼接管线的帧缓冲 DIB /
//     缩略图缓冲状态：算法层仅经指针读写其纯数据字段，真实 GDI 资源的创建/释放归
//     Windows IO/UI 层；非 Windows 编译路径下这些成员为占位类型，不参与算法行为。
#pragma once

#include "lc_platform.h"

#include <algorithm>   // std::min / std::max / std::sort / std::stable_sort / std::lower_bound
#include <atomic>      // LongCaptureContext 的控制信号原子字段
#include <cmath>       // std::sqrt（LongCaptureEdgeCorrelation 等加权相关度）
#include <cstdint>     // 定宽整型 / INT64_MIN / INT64_MAX（裁剪内容坐标哨兵）
#include <cstring>     // memcpy / memcmp（提交拼接 / 全同帧判定）
#include <string>      // LongCaptureContext.base64
#include <utility>     // std::move / std::swap
#include <vector>      // 帧缓冲 / 匹配数据 / 历史环形队列

// CaptureContext（Windows 编辑态会话上下文）仅以指针出现在下方 IO/UI 函数声明中：
// 完整定义在 Windows 侧 internal.h → capture_context.h，前向声明即可。
struct CaptureContext;

// ==================== 长截图纯数据类型（跨平台唯一权威，原 internal.h「长截图」段迁入）====================

// 行细节量 = 行内相邻采样列的灰度跳变数——空白/均匀区细节为 0，其上的“匹配”不可信
// （在任何位移都能“匹配”，只会污染评分），以行权重（细节量截断到 0~4）参与评分，
// 空白行天然不计分；整帧细节总量过低则无对齐依据。
//
// 匹配用帧数据：量化灰度行（h × cols）+ 每行匹配权重。

// 多 ROI 独立验证的列带数量（左/中/右）。列带把采样列均分为三段，
// 每段独立产出 Top-N 候选后做跨 ROI 加权聚类，防单个区域误匹配决定拼接结果。
static const int LC_ROI_BANDS = 3;

// 多尺度垂直结构 profile 的列桶数：每行按采样列均分为若干桶取平均量化灰度，
// 再做 4/8 行滑动聚合，供候选位移的结构一致性辅助验证（LongCaptureProfileCorrelation）。
static const int LC_PROFILE_BUCKETS = 8;

struct LongMatchData {
    std::vector<uint8_t> gray;      // 4bit 量化灰度，行距 cols
    std::vector<uint8_t> weight;    // 每行匹配权重 0~4（= min(4, 行细节量）)
    std::vector<uint8_t> bandWeight[LC_ROI_BANDS]; // 每行各列 ROI（左/中/右）权重 0~4
    std::vector<uint16_t> edge;     // 每行一维垂直结构强度：相邻采样列量化灰度差绝对值之和
                                    //（Weak 档匹配的边缘结构特征，见 LongCaptureEdgeCorrelation）
    std::vector<float> profile4;    // 4 行聚合垂直结构 profile：h 行 × LC_PROFILE_BUCKETS 列桶，
                                    // 桶值 = 窗口内行桶均值的平均（多尺度结构一致性验证用）
    std::vector<float> profile8;    // 8 行聚合 profile（同上，更粗尺度）
    int cols = 0;
    int bandStart[LC_ROI_BANDS] = {0, 0, 0};  // 各 ROI 起始采样列
    int bandCols[LC_ROI_BANDS] = {0, 0, 0};   // 各 ROI 采样列数
    int h = 0;
    int detailSum = 0;              // 全帧行细节量总和（整帧近乎均匀时对齐不可信）
};

// 匹配模式（双模式识别）：按候选位移的重叠量分档，overlap 越小要求证据越强。
enum class LongCaptureMatchMode {
    Normal,       // 常规匹配：overlap >= max(24, 15% 视口高)，沿用现有阈值与多数派 ROI 聚合
    WeakOverlap   // 弱重叠/大跳变匹配：overlap >= max(32, 5% 视口高)，以显著更严格的
                  // 交叉验证（3/3 ROI 全票 + 边缘结构相关 + 动态高置信度 + 延迟确认）换取可识别性
};

// 匹配状态（三值结论）：只有 SUCCESS 允许进入拼接提交。
// LOW_CONFIDENCE / FAILED 一律按“本帧被拒绝”处理（重试/计失败），
// 绝不允许用兜底 offset、上一帧推导 offset 或整帧插入的方式“继续流程”。
enum LongMatchStatus {
    LC_MATCH_FAILED = 0,        // 无可信对齐（无候选峰 / ROI 相互矛盾）
    LC_MATCH_LOW_CONFIDENCE,    // 有候选但置信度不足（宽松档/单 ROI/置信度低于阈值）
    LC_MATCH_SUCCESS            // 可信对齐（严格档 + 多 ROI 一致 + 置信度达标）
};

// 识别失败的细分类别（可观测性增强：失败不再统一记为 FAILED，归类见 DetectPass /
// TryStitch 各拒绝点）。只用于失败归因、调试日志与重试节奏选择（瞬态 vs 结构性），
// 不参与任何验收阈值判定；成功路径一律保持 None。
enum class LCFailReason {
    None = 0,
    NoCandidate,          // NO_CANDIDATE：全部 ROI 无候选 / 整帧无对齐依据
    CandidateWeak,        // CANDIDATE_WEAK：有候选但整体匹配或综合置信度不足
    PeakAmbiguous,        // PEAK_AMBIGUOUS：峰值分离度不足（周期性/重复内容歧义）
    GlobalMismatch,       // GLOBAL_MISMATCH：全宽富验证不可评（重叠区近乎全空白）
    SeamMismatch,         // SEAM_MISMATCH：接缝窗证据不足
    SpatialMismatch,      // SPATIAL_MISMATCH：top/middle/bottom 空间一致性崩塌
    ContinuityMismatch,   // CONTINUITY_MISMATCH：匹配分布碎片化（连续性不足）
    ProfileMismatch,      // PROFILE_MISMATCH：多尺度垂直结构不一致
    RoiInconsistent,      // ROI_INCONSISTENT：跨 ROI 候选冲突 / 聚类支持不足
    OffsetImplausible,    // OFFSET_IMPLAUSIBLE：offset 被历史合理性校验拒绝
    DirectionConflict,    // DIRECTION_CONFLICT：最佳候选与最近滚动方向相反
    FrameUnstable         // FRAME_UNSTABLE：稳定性检测未过（本帧未进入正式匹配）
};

// 识别阶段输出（纯检测结果）：未经 offset 合理性校验前不得用于提交。
struct LongMatchOutcome {
    LongMatchStatus status = LC_MATCH_FAILED;
    LongCaptureMatchMode mode = LongCaptureMatchMode::Normal; // 本次候选的匹配档位
    int offset = 0;                  // 候选位移 d（>0 向下滚，<0 向上滚；|d|≤1 视为未滚动）
    int overlap = 0;                 // 候选位移对应的重叠行数（= h − |d|）
    float overall = 0.0f;            // 全宽重叠区加权匹配率
    float seam = 0.0f;               // 接缝窗加权匹配率
    float top = 0.0f;                // 重叠区上 1/3 加权匹配率（该段无有效行时记 0）
    float middle = 0.0f;             // 重叠区中 1/3 加权匹配率
    float bottom = 0.0f;             // 重叠区下 1/3 加权匹配率
    float spatial = 0.0f;            // top/middle/bottom 空间一致性综合分（局部假匹配在此崩塌）
    float continuity = 0.0f;         // 匹配分布连续性（最长连续段占比 + 匹配率 − 断点罚）
    float profileScore = 0.0f;       // 4/8 行聚合 profile 多尺度垂直结构一致度
    float edgeCorrelation = 0.0f;    // 重叠区行边缘结构强度的归一化相关度（Weak 档强证据）
    float peakGap = 0.0f;            // 与次优候选的综合分差（归一化；1 = 无竞争峰）
    float roiWeighted = 0.0f;        // 跨 ROI 加权证据（按 ROI 信息量加权融合的支持度）
    float confidence = 0.0f;         // 综合置信度（全宽匹配 + 空间/连续性 + 多尺度结构 +
                                     //  ROI 加权证据 + 峰值分离度的加权组合，见 DetectPass）
    LCFailReason reason = LCFailReason::None; // 拒绝原因分类（成功时保持 None；仅归因/日志/重试节奏用）
    float textureRatio = 0.0f;       // 重叠区有效纹理行占比（证据量计价，进入综合置信度）
    int bandOffsets[LC_ROI_BANDS] = {0, 0, 0};   // 各 ROI 独立求得的位移
    bool bandValid[LC_ROI_BANDS] = {false, false, false}; // 该 ROI 是否产出可信候选
    int validBandCount = 0;          // 产出候选的 ROI 数
    int agreeCount = 0;              // 最终候选所在聚类的支持 ROI 数（加权融合后）
};

// 单帧采样结论（识别→校验→提交管线的对外语义）。
enum class LCSampleOutcome {
    Stitched,    // 已安全提交拼接且新增了拼接行（唯一扩展累计拼接内容的结局）
    Repositioned,// 匹配成功但新视口完全落在已捕获内容范围内（反向回滚未越过捕获边界）：
                 // 只推进当前视口基准（lastFrame/committedContentTop）并移动小地图当前
                 // 区域标注，不新增行、不计帧数——已捕获内容的重复帧绝不再次拼接
    NoChange,    // 匹配成功但内容未滚动（d=0）；与“匹配失败”“滚动到底”均独立
    WeakPending, // Weak 候选首次成立：只登记待复核候选（pendingMatch），不提交、不改任何累计状态
    WeakRejected,// Weak 候选被复核否决（第二次候选不一致/置信度跌破门槛/offset 异常）：
                 // 与“完全无候选”的硬失败不同，不计入普通失败计数，走独立重试预算
    Unstable,    // 稳定性检测未过（页面仍处滚动/重绘/懒加载过渡）：本帧不进入正式匹配，
                 // 不提交、不修改任何累计状态（含跟踪/历史/Weak 候选），由采样主循环
                 // 短延迟后重新采样，等待页面稳定
    Failed       // 硬失败（无候选/验证崩塌/Normal offset 异常）；累计状态保持原样
};

// Weak Match 的“延迟确认提交”候选（防污染核心机制）：首次弱重叠可信候选只登记、
// 不提交；下一次稳定采样必须独立复现一致候选（|Δoffset| ≤ LC_WEAK_CONFIRM_OFFSET_TOL
// 且置信度不跌破动态门槛）才允许 Commit。候选被否决/放弃时只清除本结构，
// 绝不触碰累计拼接状态（body/headRev/lastFrame/lastMatch/offsetHistory）。
struct LongCapturePendingMatch {
    bool valid = false;
    int offset = 0;
    float confidence = 0.0f;
    LongCaptureMatchMode mode = LongCaptureMatchMode::Normal;
};

// 最近帧历史条目（多跳匹配恢复的基准池）。内容坐标 = 以首帧视口顶为原点、不随头部
// 前插平移的滚动空间坐标（拼接图内位置 = contentY + headRows）。
// 只保存匹配所需数据（量化灰度/行权重/ROI 权重/profile 等，约 1MB 级/帧），
// 不保存完整位图；环形容量见 LC_HISTORY_FRAMES（lc_match_core.cpp）。
// contentY 语义：已提交帧精确（可作跨帧提交链锚点）；未提交帧为 tentative 估计。
struct LongCaptureFrameHistory {
    int frameId = -1;                // 采样序号（识别 lastFrame 直连条目 / 日志关联）
    LongMatchData match;             // 该帧匹配数据（LongCaptureBuildMatchData 全量产物）
    int64_t contentY = 0;            // 视口顶内容坐标（已提交=精确；未提交=估计）
    bool committed = false;          // 已提交帧：位置精确、可作提交链锚点
    bool validForMatching = true;    // 近乎空白的帧不可作多跳匹配基准
};

// 长截图工具栏二级菜单类型：方向与裁剪均为图标 popover（同一套展开/绘制/命中机制，
// lc_toolbar_ui_windows.cpp），popover 悬停各 cell 有 title 式 tooltip
enum LCMenuKind {
    LCM_None = 0,     // 无展开菜单
    LCM_Direction,    // 方向 popover：纵向/横向图标 cell，悬停/点击方向按钮展开（已拼接多帧后锁定）
    LCM_Crop          // 裁剪 popover：悬停/点击裁剪按钮展开的图标浮层（丢弃上方/下方内容，随方向切换左右变体；已裁剪时含重置）
};

struct LongCaptureContext {
    // 选项（由 start() 的 options.longCapture 注入，会话开始时拷贝）
    int interval = 250;     // 滚轮停止后等待内容稳定的毫秒数（采样防抖）

    // 虚拟屏幕（逻辑坐标）与 DPI
    int vx = 0, vy = 0, vw = 0, vh = 0;
    double dpiScale = 1.0;   // 单一 scale 模型的已知限制见 CaptureContext.dpiScale 注释

    // 选区（逻辑虚拟屏幕坐标，已规范化）
    RECT selection = {};

    // 采样裁剪矩形（逻辑虚拟屏幕坐标）：选区每边内缩 LC_CROP_INSET_LOGI，
    // 避开选区框描边与边缘抗锯齿像素污染拼接结果；滚轮过滤与结果 rect 均以此为准。
    RECT cropRect = {};

    // 选区物理像素裁剪参数（相对虚拟屏幕原点的物理偏移 + 尺寸）。
    // 纵向模式 physW/physH 即帧尺寸；横向模式（horizontal=true）帧缓冲为屏幕采样的
    // 转置（physW=capH、physH=capW），下游匹配/拼接/缩略图管线与纵向完全同构。
    int physX = 0, physY = 0, physW = 0, physH = 0;
    // 屏幕采样 DIB 尺寸（未转置的物理宽高；方向切换只交换 physW/physH，DIB 不变）
    int capW = 0, capH = 0;
    // 虚拟屏幕物理原点（屏幕 DC 坐标，DPI 感知下为物理像素），直接区域采样时作 BitBlt 源偏移
    int physOriginX = 0, physOriginY = 0;
    // 专用 DIB 段：每帧采样的可复用目标（避免反复新建位图 + GetDIBits 回读）
    HDC dibDC = NULL;
    HBITMAP dibBmp = NULL;
    void* dibBits = nullptr;
    int dibW = 0, dibH = 0;

    // 预览面板放置在选区上方（空间兜底时的退化形态）：向下生长受选区顶边约束，
    // 避免面板长大后重新覆盖选区导致自身入画
    bool panelAbove = false;

    // 拼接结果：物理像素 BGRA，宽度恒为 physW；高度 stitchH 增长。
    // 分两段存储以让拼接保持 O(新增行)：向上滚新增行存 headRev（倒序，reverse 后为显示顺序），
    // 向下滚新增行存 body（正序）；实际图像 = reverse(headRev) + body。
    std::vector<uint32_t> headRev;
    std::vector<uint32_t> body;
    int headRows = 0, bodyRows = 0;
    int stitchH = 0;
    // 最近一次采样的视口帧（相邻帧重叠检测基准，缓冲随拼接迭代复用）
    std::vector<uint32_t> lastFrame;
    // lastFrame 的模糊对齐数据（两帧采样之间 lastFrame 不变，重叠检测直接复用，
    // 只在拼接成功后随 lastFrame 旋转更新）
    LongMatchData lastMatch;

    // 面板两级缩略图：先按固定列宽（面板预览宽）增量缩列，绘制时再合并缩行。
    // 分段与拼接缓冲同构（thumbHeadRev 倒序 / thumbBody 正序），thumbMerged 为绘制用临时合并缓冲。
    std::vector<uint32_t> thumbHeadRev;
    std::vector<uint32_t> thumbBody;
    std::vector<uint32_t> thumbMerged;
    int thumbW = 0;        // 缩略图列宽（≤ physW）
    int thumbHeadH = 0;    // 头部段行数（合并后显示于顶部）
    int thumbH = 0;        // 缩略图总行数
    bool thumbDirty = false;

    // 滚轮观察状态（面板 WM_INPUT 经本线程消息泵分发，与主循环同线程，无需原子）
    bool wheelPending = false;
    DWORD lastWheelTick = 0;
    DWORD lastSampleTick = 0;   // 最近一次采样轮触发时刻（滚动中主动采样节拍用；首帧后初始化）
    int lastDir = 0;        // +1 最近一次向下滚（追加底部）；-1 向上滚（前插头部）

    // —— 拼接失败恢复与 offset 合理性校验状态 ——
    // 这些字段只在「成功提交拼接」后更新；识别失败/被拒绝的帧绝不写入，
    // 保证单帧误识别无法通过污染匹配基准或位移历史把错误扩散到后续帧。
    std::vector<int> offsetHistory;   // 最近若干次成功拼接的 |d|（px，长度上限见 LC_OFFSET_HISTORY_LEN）
    int noChangeCount = 0;            // 连续「内容未变化」采样数（仅匹配成功且 d=0 时递增；与失败完全独立）
    bool reachedBottom = false;       // 滚动到底（由连续多次内容未变化确认；匹配失败绝不置位）

    // —— Weak（低重叠大跳变）延迟确认与独立重试状态 ——
    // pendingMatch 只在本结构内暂存候选，任何未确认路径都不修改累计拼接状态；
    // weakTries 计连续未决的 Weak 采样轮数，达 LC_WEAK_MAX_TRIES 即放弃当前候选链重新观察。
    LongCapturePendingMatch pendingMatch; // 待复核的 Weak 候选（有效时驱动无滚轮的继续采样）
    int weakTries = 0;                    // 连续未决的 Weak 候选采样轮数
    int sampleIndex = 0;                  // 采样序号（LC_DEBUG_LOG 调试日志用）

    // —— Tentative 视觉跟踪状态（与正式拼接状态完全解耦的"预计当前位置"层）——
    // 正式拼接位置（committed）= committedContentTop，仅 LongCaptureCommitStitch 推进；
    // 预计当前位置（tentative）= tentativeContentTop，由视觉验证（多跳匹配/零位移对齐/
    // 全同帧）直接设定或轻量预测推进。FAILED / LOW_CONFIDENCE 帧只允许触碰本组字段，
    // 绝不修改 body/headRev/stitchH、lastFrame/lastMatch、offsetHistory 与拼接缓冲。
    // 防漂移约束：相对 committed 漂移上限（2×视口高）+ 连续无视觉依据冻结
    //（见 LongCaptureTrackingSetVisual / LongCaptureTrackingAdvancePredicted）。
    int64_t committedContentTop = 0;   // lastFrame 视口顶（内容坐标；CommitStitch 内 += d）
    int64_t tentativeContentTop = 0;   // 预计当前视口顶（内容坐标；SUCCESS 后与 committed 对齐）
    bool tentativeValid = false;       // 是否已建立可信跟踪（小地图据此显示虚线预计框）
    float tentativeConfidence = 0.0f;  // tentative 置信度（视觉验证来源高，预测链逐帧衰减）
    int trackUnreliableStreak = 0;     // 连续无视觉依据采样数（达阈值冻结预测推进）
    int trackingRevision = 0;          // 跟踪状态变更计数（RunLongCapture 据此刷新小地图）
    int lastCommittedFrameId = 0;      // 最新已提交帧序号（多跳回溯时跳过 hop1 直连基准）
    std::vector<LongCaptureFrameHistory> frameHistory; // 最近帧环形历史（容量 LC_HISTORY_FRAMES）
    std::vector<int> weakCandidateOffsets;             // 最近弱重叠候选位移（Weak 时间一致性样本）

    // —— 帧稳定性检测（进入正式 DetectMatch 前的准入闸门，纯诊断层）——
    // stableRef* = 上一次抓帧的 4bit 灰度稀疏采样，供与本次抓帧对比判断页面是否仍在
    // 快速变化（滚动动画/重绘/懒加载过渡）。每次抓帧后滚动更新；只在稳定性闸门启用
    // 且参考帧新鲜（间隔 ≤ LC_STABLE_REF_MAX_GAP）时参与判定。绝不参与 offset 评分、
    // 不触碰任何累计拼接状态（含跟踪/历史/Weak 候选）。
    std::vector<uint8_t> stableRefGray;
    int stableRefCols = 0;
    int stableRefH = 0;
    DWORD stableRefTick = 0;
    bool stableRefValid = false;

    // —— 失败可观测性：最近一次拒绝的原因分类与最佳候选证据快照 ——
    // 仅调试日志与采样主循环的重试节奏选择（瞬态 vs 结构性）使用，不参与任何判定。
    LCFailReason lastFailReason = LCFailReason::None;
    LongMatchOutcome lastReject;

    // —— 滚轮 delta 软先验（仅用于候选排序加分，绝不作为期望区间硬约束） ——
    // wheelAccumDelta 在面板 WM_INPUT 累计，成功提交时折算 px/notch 并 EMA 平滑；
    // 失败采样绝不更新估计。Windows/浏览器/平滑滚动/触控板都使位移与 notch 非线性相关，
    // 因此先验只轻微影响同档候选的排序先后（LC_WHEEL_PRIOR_BONUS），不改变任何阈值。
    int wheelAccumDelta = 0;          // 自上次成功提交以来累计的滚轮增量（带符号，WHEEL_DELTA=120）
    float pixelsPerWheelNotch = 0.0f; // 「成功像素位移 ↔ 滚轮 notch」在线估计（0 = 尚无估计）

    // 控制信号（跨线程：abortLongCapture 由 JS 线程设置）
    std::atomic<bool> abortFlag{false};
    std::atomic<bool> finishFlag{false};   // 用户点「完成并复制」
    std::atomic<int> frameCount{0};

    // —— 选区底部工具栏（宽高标签 / 方向 / 自动滚动 / 裁剪 / 保存 / 取消 / 完成并复制）——
    bool horizontal = false;     // 长截图方向：false=纵向（默认）；true=横向（帧缓冲转置复用纵向管线）
    bool autoScroll = false;     // 自动滚动（默认关闭）：开启时光标一次性移到选区中心，此后定时器只注入滚轮
                                  //（SendInput 按光标下方窗口路由；单拍不动鼠标，见 LongCaptureAutoScrollTick）
    std::atomic<bool> saveFlag{false};  // 用户点「保存到本地」：主循环内弹保存对话框并直接落盘
    // —— 裁剪状态（全部用「内容坐标」表达：拼接图行 = 内容坐标 + headRows）——
    // 内容坐标不随头部前插/主体追加平移，裁剪线因此永远锚定在页面的同一位置，
    // 不会因继续滚动导致窗口错位（旧的绝对拼接行号在前插后会整体漂移）。
    // 裁剪动作只收紧输出行窗口并登记「待剔除区间」，绝不立即修改拼接缓冲与匹配基准；
    // 待剔除区间在第一次朝该方向的成功提交时物理删除（见 CommitStitch 入口的延迟剔除），
    // 删除后该侧边界重新开放——继续滚动的新增内容直接续接在裁剪线之后继续拼图。
    int64_t cropTopY = INT64_MIN;      // 输出窗口上界（内容坐标）；INT64_MIN = 该侧开放（未裁剪）
    int64_t cropBottomY = INT64_MAX;   // 输出窗口下界（内容坐标）；INT64_MAX = 该侧开放（未裁剪）
    bool cropPendTop = false;          // 待剔除「上方已捕获内容」（丢弃上方登记）：下一次 d<0 提交时删除
    int64_t cropPendTopLo = 0, cropPendTopHi = 0;     // 待删内容区间 [lo, hi)
    bool cropPendBottom = false;       // 待剔除「下方已捕获内容」（丢弃下方登记）：下一次 d>0 提交时删除
    int64_t cropPendBottomLo = 0, cropPendBottomHi = 0;
    bool cropped = false;              // 是否存在任何生效的裁剪约束（重置菜单项/图标徽标显示用；
                                       // 含尚未触发的待剔除区间，触发或重置后自动回收）
    int tbHover = -1;            // 工具栏 hover 项（LongToolbarItem，-1=无）
    int tbPressItem = -1;        // 工具栏按下项（WM_LBUTTONDOWN 命中，-1=无）：UP 必须命中与
                                 // DOWN 相同的目标才触发点击——编辑工具栏「长截图」按钮在按下
                                 // 瞬间创建本工具栏，残留的松开事件绝不能误触恰好同位的按钮
    int tbPressMenuRow = -1;     // 二级菜单 popover 按下的 cell（-1=无；语义同 tbPressItem）
    bool tbDragging = false;     // 正在拖动工具栏窗口（按住最左 6 点把手，鼠标捕获中）
    int tbDragGrabDX = 0, tbDragGrabDY = 0;  // 按下点相对工具栏窗口左上角的偏移（物理像素）
    LCMenuKind menuKind = LCM_None;  // 展开中的二级菜单类型（方向/裁剪均为图标 popover）
    int menuHover = -1;          // 二级菜单 popover hover 的图标 cell（-1=无）
    bool menuBelow = false;      // 二级菜单绘制在工具栏下方（朝避让选区的一侧展开，见 LongCaptureSetMenu）
    LCMenuKind popHoverDisarm = LCM_None;  // 「悬停展开」被解除武装的菜单锚点（LCM_None=全部武装）：
                                           // 点击收起其 popover 后置为该菜单，光标移出锚点前不再
                                           // 因悬停重开，防止点击收起与悬停展开互相打架（方向/裁剪通用）
    std::vector<uint32_t> thumbDisplay;  // 横向模式显示用回转缩略图（纵向直接用 thumbMerged）
    int thumbDisplayW = 0, thumbDisplayH = 0;
    bool thumbDisplayDirty = false;
    int autoFailStreak = 0;      // 自动滚动中连续硬失败采样轮数（达上限自动停止，防丢内容）

    // 输出结果（物理像素）
    int outWidth = 0, outHeight = 0;
    std::string base64;
    bool success = false;
};

// 采样裁剪相对选区每边内缩（逻辑像素）：避开选区框描边与边缘抗锯齿，防止污染拼接内容。

static const int LC_CROP_INSET_LOGI = 2;

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
// LC_AUTOSCROLL_STOP_FAILS 定义于 lc_toolbar_ui_windows.cpp（自动滚动参数），session 主循环读它决定自动停止。
extern const int LC_AUTOSCROLL_STOP_FAILS;

// ==================== 二级常量（原文件级 static，归各使用块）====================

// 蒙版样式（预乘 ARGB）：整屏半透明灰，采样裁剪区整透明透出实况桌面。
// 定义于 lc_panel_ui_windows.cpp（蒙版绘制唯一使用方）。
extern const int LONG_MASK_GRAY;
extern const BYTE LONG_MASK_ALPHA;

// 面板布局常量（逻辑像素）：面板总宽与内边距被 toolbar（缩略图列宽）与 session（缩略图列宽）
// 共用，故在此声明为 extern；定义于 lc_panel_ui_windows.cpp。
extern const int LC_PANEL_W;
extern const int LC_PANEL_PAD;

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
// 以下两个函数实际定义在 lc_stitch_state.cpp 内为 static（仅本文件内部使用）：
// MSVC 容忍「头文件非 static 声明 + static 定义」的非标准扩展（Windows 现状，声明
// 保留不动）；clang 按 IL 严格拒绝，非 Windows 编译路径（lc_bridge 复用链）不声明。
#ifdef _MSC_VER
LCMultihopResult LongCaptureMultihopDetect(const LongCaptureContext* c,
                                           const LongMatchData& currMatch, int dir,
                                           const LongCaptureOffsetPrior& prior);
#endif
LongCaptureTrackingEstimate LongCaptureBuildTrackingEstimate(const LongCaptureContext* c, int dir,
                                                              const LongCaptureOffsetPrior& prior);
#ifdef _MSC_VER
int64_t LongCaptureTrackingDriftLimit(const LongCaptureContext* c);
#endif
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
// 单帧「识别→offset 校验→（Weak 档）延迟确认→提交」管线（RunLongCapture 采样流程与单元测试共用入口）：
// Normal 档识别 SUCCESS 且 offset 通过历史合理性校验即提交；Weak 档（低重叠大跳变）首次可信候选
// 只登记 pendingMatch（WeakPending），下一次稳定采样独立复现一致候选才提交（Stitched）。
// 任何拒绝（Failed/WeakPending/WeakRejected/Unstable）都不修改累计拼接状态。
// allowStabilityGate：进入正式 DetectMatch 前启用轻量帧稳定性闸门（采样主循环传入
// true；默认 false 保持单元测试/合成帧直连注入的旧行为完全不变）。闸门未过时返回
// Unstable——本帧不匹配、不提交、不改任何状态，调用方短延迟后重新采样。
LCSampleOutcome LongCaptureTryStitch(LongCaptureContext* c, std::vector<uint32_t>& curr, int dir,
                                     bool allowStabilityGate = false);

// —— 抓帧 / DIB / 缩略图 / 位图构建 / 消息泵（lc_frame_io_windows.cpp）——
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

// —— 面板 / 蒙版 UI（lc_panel_ui_windows.cpp）——
bool EnsureArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h, int wantW, int wantH);
void FreeArgbSurface(HDC& dc, HBITMAP& bmp, void*& bits, int& w, int& h);
void LongCapturePanelRender(HWND panel, LongCaptureContext* c);
LRESULT CALLBACK LongCapturePanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void LongCapturePanelUpdate(LongCaptureContext* c);
HWND LongCaptureCreatePanel(CaptureContext* ctx, LongCaptureContext* c);
LRESULT CALLBACK LongCaptureMaskWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
RECT CalcSampleCrop(const CaptureContext* ctx);
void EnterLongCaptureMask(const CaptureContext* ctx);

// —— 工具栏 UI（lc_toolbar_ui_windows.cpp）——
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
// 定义于 lc_toolbar_ui_windows.cpp，session 主循环弹保存对话框前后调用。
void LongCaptureSetTopmost(bool topmost);

// —— 会话主循环 / 生命周期（lc_session_windows.cpp）——
void LongCaptureEmitFailure();
void LongCaptureWaitMessages(LongCaptureContext* c, DWORD ms);
// 释放采样 DIB 段：把原 wndproc_windows.cpp 里手动释放 lc->dibDC/dibBmp 的跨界所有权
// 收进此处，由 DestroyLongCaptureContext 统一释放。
// 注：会话级窗口（panel/toolbar/mask）销毁仍由捕获线程清理段调用，
// 本函数只负责 DIB 资源的归口释放。
void DestroyLongCaptureContext(LongCaptureContext* lc);
