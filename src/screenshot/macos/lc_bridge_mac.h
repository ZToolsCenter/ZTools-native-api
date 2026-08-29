// 长截图算法层 C ABI shim。
//
// 定位：把 lc_match_core.cpp / lc_stitch_state.cpp（纯算法层，仅依赖 uint32 BGRA
// 帧缓冲）的 C++ 接口打包为固定布局的 C 接口，供 macOS 侧 Swift（经 @_silgen_name）
// 链入 libZToolsNative.dylib 调用。本层不含任何业务逻辑——只做参数打包/解包转发，
// 以及少量「定义在 Windows 侧 IO/UI 文件中的纯数据函数」的逐字镜像（见 lc_bridge_mac.cpp
// 内的镜像说明段）。本文件仅在 macOS 构建链（build-swift.sh / CI / test-lc-mac.sh）
// 编译；Windows 侧不参与编译（binding.gyp win 分支不变，算法层经 internal.h 原生编译）。
//
// 跨边界纪律：
//   · 结构体全部使用固定宽度标量（int32_t/int64_t/float/double/指针），禁用 C++ STL
//     与平台句柄；字段顺序即内存布局（自然对齐），Swift 侧按同序镜像。
//   · 句柄 lc_handle_t 不透明，由 lc_session_create 产出、lc_session_destroy 销毁；
//     其余函数对 NULL 句柄一律安全返回失败值，不解引用。
//   · 所有帧缓冲按 BGRA 物理像素（与 Windows 算法层内存序一致）行主序传入。
#ifndef LC_BRIDGE_H
#define LC_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 不透明会话句柄 = 算法层 LongCaptureContext*（可写累计拼接状态对象）。
// 由 lc_session_create 产出、lc_session_destroy 销毁；其余函数对 NULL 句柄
// 一律安全返回失败值，不解引用。
typedef void* lc_handle_t;

// ==================== 枚举镜像（与 internal.h 逐值对齐，C 侧用宏保证固定宽度） ====================

// LCSampleOutcome：单帧采样七值结局（LongCaptureTryStitch 返回语义）。
#define LC_OUTCOME_STITCHED      0   // 已提交且新增拼接行
#define LC_OUTCOME_REPOSITIONED  1   // 已提交但仅视口重定位（反向回滚，无新增行）
#define LC_OUTCOME_NO_CHANGE     2   // 内容未滚动（全同帧 / 匹配成功 d=0）
#define LC_OUTCOME_WEAK_PENDING  3   // Weak 候选首次成立：只登记待复核，未提交
#define LC_OUTCOME_WEAK_REJECTED 4   // Weak 候选被复核否决（复现不一致 / 置信度跌破 / offset 异常）
#define LC_OUTCOME_UNSTABLE      5   // 稳定性闸门未过（仅 allowStabilityGate 时可能出现）
#define LC_OUTCOME_FAILED        6   // 硬失败（无候选 / 验证崩塌 / offset 异常）；状态未被触碰

// LongMatchStatus：匹配三值结论。
#define LC_MATCH_STATUS_FAILED          0
#define LC_MATCH_STATUS_LOW_CONFIDENCE  1
#define LC_MATCH_STATUS_SUCCESS         2

// LongCaptureMatchMode：匹配档位。
#define LC_MODE_NORMAL       0
#define LC_MODE_WEAK_OVERLAP 1

// LCFailReason：失败分类（顺序与 internal.h 的 enum class 逐项一致）。
#define LC_FAIL_NONE                0
#define LC_FAIL_NO_CANDIDATE        1
#define LC_FAIL_CANDIDATE_WEAK      2
#define LC_FAIL_PEAK_AMBIGUOUS      3
#define LC_FAIL_GLOBAL_MISMATCH     4
#define LC_FAIL_SEAM_MISMATCH       5
#define LC_FAIL_SPATIAL_MISMATCH    6
#define LC_FAIL_CONTINUITY_MISMATCH 7
#define LC_FAIL_PROFILE_MISMATCH    8
#define LC_FAIL_ROI_INCONSISTENT    9
#define LC_FAIL_OFFSET_IMPLAUSIBLE 10
#define LC_FAIL_DIRECTION_CONFLICT 11
#define LC_FAIL_FRAME_UNSTABLE     12

// 裁剪登记动作（lc_apply_crop 的 row 参数，语义对齐 lc_toolbar_ui_windows.cpp 的裁剪 popover 行）。
#define LC_CROP_DISCARD_TOP    0   // 丢弃上方（横向模式 = 丢弃左侧）
#define LC_CROP_DISCARD_BOTTOM 1   // 丢弃下方（横向模式 = 丢弃右侧）
#define LC_CROP_RESET          2   // 重置裁剪（仅已裁剪时存在该行；未裁剪时调用返回 0）

// ABI 版本（结构体布局或语义变更时递增；Swift 侧启动时可校验）。
// 2：LCSessionConfig / LCStateSnapshot 移除 maxFrames 字段（长截图取消拼接帧数上限）。
#define LC_ABI_VERSION 2

// ==================== C 布局结构体 ====================

// 会话创建配置（对应 LongCaptureContext 的构造参数中「会话级配置」子集，
// 对齐 lc_session_windows.cpp BeginLongCapture 的赋值面；虚拟屏/选区/DPI 等纯 IO 簿记字段
// 算法层不读取，不进 C ABI，由 macOS 会话层自行持有）。
typedef struct LCSessionConfig {
    int32_t interval;     // 滚轮停稳防抖 ms（对齐 lc->interval；算法层不读取，随快照回读）
    int32_t physW;        // 帧缓冲宽（物理像素；横向模式为转置后宽度，对齐 lc->physW）
    int32_t physH;        // 帧缓冲高（物理像素，对齐 lc->physH）
    int32_t thumbW;       // 缩略图列宽（对齐 lc->thumbW；0 = 关闭缩略图；> physW 时被钳到 physW）
    int32_t horizontal;   // 0 = 纵向（默认）；1 = 横向（帧缓冲转置复用纵向管线）
} LCSessionConfig;

// 匹配证据快照（对齐 internal.h LongMatchOutcome 字段；来自拒绝帧的 lastReject
// 或独立识别调用 lc_detect_match 的返回）。
typedef struct LCMatchEvidence {
    int32_t status;               // LC_MATCH_STATUS_*
    int32_t mode;                 // LC_MODE_*
    int32_t failReason;           // LC_FAIL_*
    int32_t offset;               // 候选位移 d（>0 向下滚，<0 向上滚）
    int32_t overlap;              // 候选位移对应重叠行数 = h − |d|
    float   overall;              // 全宽重叠区加权匹配率
    float   seam;                 // 接缝窗加权匹配率
    float   top;                  // 重叠区上 1/3 匹配率
    float   middle;               // 重叠区中 1/3 匹配率
    float   bottom;               // 重叠区下 1/3 匹配率
    float   spatial;              // 三段空间一致性综合分
    float   continuity;           // 匹配分布连续性
    float   profileScore;         // 4/8 行聚合 profile 多尺度结构一致度
    float   edgeCorrelation;      // 行边缘结构相关度（Weak 档强证据）
    float   peakGap;              // 峰值分离度（归一化）
    float   roiWeighted;          // 跨 ROI 加权证据
    float   confidence;           // 综合置信度
    float   textureRatio;         // 重叠区有效纹理行占比
    int32_t validBandCount;       // 产出候选的 ROI 数
    int32_t agreeCount;           // 最终候选聚类支持 ROI 数
    int32_t bandOffsets[3];       // 各 ROI 独立求得的位移
    int32_t bandValid[3];         // 各 ROI 是否产出候选
} LCMatchEvidence;

// 单帧喂入结果（lc_try_stitch 的返回打包）。
typedef struct LCTryStitchResult {
    int32_t outcome;            // LC_OUTCOME_*（七值结局）
    int32_t failReason;         // LC_FAIL_*（本帧拒绝分类；成功/NoChange/WeakPending 为 NONE）
    int32_t addedRows;          // 新增拼接行数（仅 STITCHED 时 > 0；由 stitchH 前后差分得出）
    int32_t committedDelta;     // committedContentTop 推进量（STITCHED/REPOSITIONED = 本帧位移 d，其余 0）
    int32_t stitchH;            // 调用后的拼接总行数
    int32_t sampleIndex;        // 本帧采样序号（对齐 lc->sampleIndex）
    int32_t hasRejectEvidence;  // 1 = evidence 为本次拒绝的 lastReject 快照（FAILED/WEAK_REJECTED）
    LCMatchEvidence evidence;   // 拒绝证据快照（hasRejectEvidence=0 时内容无效）
    int32_t pendingValid;       // 调用后待复核 Weak 候选是否有效（WEAK_PENDING 时为 1）
    int32_t pendingOffset;      // 待复核候选位移
    float   pendingConfidence;  // 待复核候选置信度
} LCTryStitchResult;

// 独立识别结果（lc_detect_match 的返回打包 = LongMatchOutcome 全量）。
typedef struct LCDetectResult {
    int32_t status;             // LC_MATCH_STATUS_*
    int32_t mode;               // LC_MODE_*
    int32_t failReason;         // LC_FAIL_*
    int32_t offset;             // 候选位移 d
    int32_t overlap;            // 重叠行数
    float   overall;
    float   seam;
    float   top;
    float   middle;
    float   bottom;
    float   spatial;
    float   continuity;
    float   profileScore;
    float   edgeCorrelation;
    float   peakGap;
    float   roiWeighted;
    float   confidence;
    float   textureRatio;
    int32_t validBandCount;
    int32_t agreeCount;
    int32_t bandOffsets[3];
    int32_t bandValid[3];
} LCDetectResult;

// 失败帧的跟踪估计（lc_build_tracking_estimate 的返回，对齐 LongCaptureTrackingEstimate：
// 只服务 tentative 跟踪与下一帧先验，绝不进入提交）。
typedef struct LCTrackingEstimate {
    int32_t valid;              // 1 = 可用于预测推进
    int32_t direction;          // +1 向下滚 / -1 向上滚
    double  predictedOffset;    // 预计 |位移|（px）
    double  confidence;         // 纯预测置信度（低，LC_TRACK_PREDICT_CONFIDENCE）
} LCTrackingEstimate;

// 状态快照（LongCaptureContext 会话可读字段的只读打包；会话侧簿记字段——
// noChangeCount / reachedBottom / weakTries / lastDir / wheelPending / 各 tick /
// frameCount / autoFailStreak / abort / finish / save 标志——按 lc_session_windows.cpp 的
// 归属属会话层，macOS 由 Swift 侧自持，不进快照）。
typedef struct LCStateSnapshot {
    int32_t physW;              // 帧缓冲宽（物理像素）
    int32_t physH;              // 帧缓冲高（物理像素）
    int32_t horizontal;         // 0/1
    int32_t stitchH;            // 拼接总行数（headRows + bodyRows）
    int32_t headRows;           // 头部段行数（向上滚前插，倒序存储）
    int32_t bodyRows;           // 主体段行数（向下滚追加，正序存储）
    int64_t committedContentTop;// 正式拼接基准视口顶（内容坐标）
    int64_t tentativeContentTop;// 预计当前视口顶（内容坐标，小地图虚线框）
    int32_t tentativeValid;     // 0/1
    float   tentativeConfidence;
    int32_t trackUnreliableStreak;
    int32_t trackingRevision;   // 跟踪状态变更计数（面板按变化刷新）
    int32_t sampleIndex;
    int32_t pendingValid;       // 待复核 Weak 候选 0/1
    int32_t pendingOffset;
    float   pendingConfidence;
    int32_t pendingMode;        // LC_MODE_*
    int32_t lastFailReason;     // LC_FAIL_*（最近一次拒绝分类）
    int32_t offsetHistoryLen;   // 成功位移历史长度（内容见 lc_get_offset_history）
    int32_t wheelAccumDelta;    // 自上次成功提交累计的滚轮增量（session 侧累计的只读回读）
    float   pixelsPerWheelNotch;// px/notch 在线估计（0 = 尚无估计）
    int32_t thumbW;             // 缩略图列宽（0 = 关闭）
    int32_t thumbHeadH;         // 缩略图头部段行数
    int32_t thumbH;             // 缩略图总行数
    int64_t cropTopY;           // 输出窗口上界（内容坐标；INT64_MIN = 开放）
    int64_t cropBottomY;        // 输出窗口下界（内容坐标；INT64_MAX = 开放）
    int32_t cropPendTop;        // 待剔除上方区间 0/1
    int32_t cropPendBottom;     // 待剔除下方区间 0/1
    int64_t cropPendTopLo;      // 待删内容区间 [lo, hi)
    int64_t cropPendTopHi;
    int64_t cropPendBottomLo;
    int64_t cropPendBottomHi;
    int32_t cropped;            // 是否存在任何生效裁剪约束 0/1
    int32_t interval;
} LCStateSnapshot;

// 算法层常量导出（lc_session_windows.cpp 采样主循环所需的重试梯 / 节拍 / 到底确认参数；
// 数组维度由算法层常量决定，见 lc_bridge_mac.cpp 的 static_assert）。
typedef struct LCAlgoConsts {
    int32_t sampleAttempts;         // LC_SAMPLE_ATTEMPTS（单次采样轮总尝试数）
    int32_t retryDelayNormal[5];    // LC_RETRY_DELAY_NORMAL（长度 = LC_SAMPLE_ATTEMPTS - 1）
    int32_t weakRetryAttempts;      // LC_WEAK_RETRY_ATTEMPTS
    int32_t retryDelayWeak[6];      // LC_RETRY_DELAY_WEAK
    int32_t stableMaxWaits;         // LC_STABLE_MAX_WAITS（稳定性等待预算）
    int32_t stableRetryDelay[3];    // LC_STABLE_RETRY_DELAY
    int32_t quickResamples;         // LC_QUICK_RESAMPLES（瞬态快重采样次数上限）
    int32_t resampleDelayQuick[2];  // LC_RESAMPLE_DELAY_QUICK
    int32_t scrollSampleMaxGap;     // LC_SCROLL_SAMPLE_MAX_GAP（滚动中主动采样节拍上限 ms）
    int32_t bottomConfirmSamples;   // LC_BOTTOM_CONFIRM_SAMPLES（连续 NoChange 确认到底）
    int32_t weakMaxTries;           // LC_WEAK_MAX_TRIES（Weak 候选独立采样轮数上限）
    int32_t stableRefMaxGapMs;      // LC_STABLE_REF_MAX_GAP（稳定性参考帧最大有效间隔 ms）
    int32_t cropInsetLogical;       // LC_CROP_INSET_LOGI（采样裁剪每边内缩，逻辑像素）
    int32_t trackMinStep;           // LC_TRACK_MIN_STEP（tentative 推进最小位移 px；小地图
                                    //  虚线框显示判定用，对齐 lc_panel_ui_windows.cpp 的引用点）
} LCAlgoConsts;

// ==================== C ABI 函数（注释标注对应算法层 API） ====================

// ABI 版本查询（返回 LC_ABI_VERSION；无句柄依赖，供 Swift 侧启动校验）。
int32_t lc_abi_version(void);

// —— 会话生命周期 ——
// 对齐 lc_session_windows.cpp BeginLongCapture 的 LongCaptureContext 构造段（new + 配置字段
// 拷贝；不含任何窗口/抓帧动作）。失败（参数非法）返回 NULL。
lc_handle_t lc_session_create(const LCSessionConfig* config);
// 销毁会话（对齐 Windows 侧 delete lc；DIB 段归 IO 层所有，macOS 不涉及）。
void lc_session_destroy(lc_handle_t h);

// —— 帧管线（喂帧 → 识别/校验/提交一步）——
// 以首帧初始化基准：镜像 lc_frame_io_windows.cpp LongCaptureInitBaseline（纯数据初始化，
// 不抓屏；lastFrame/body/lastMatch/跟踪基准/第 0 条历史/缩略图首段）。调用后 frame
// 内容即首屏。bgra 长度必须为 w*h 且 w/h 与创建配置一致。返回 1 成功 / 0 参数非法。
int32_t lc_init_baseline(lc_handle_t h, const uint32_t* bgra, int32_t w, int32_t height);
// 单帧「识别 → offset 校验 →（Weak 档）延迟确认 → 提交」管线：转发
// LongCaptureTryStitch（internal.h 声明，lc_stitch_state.cpp 定义）。dir = 滚动方向
//（+1 下 / -1 上 / 0 未知）；allowStabilityGate = 启用稳定性闸门（对齐 RunLongCapture
// 传参 !autoScroll && stableWaits < LC_STABLE_MAX_WAITS）。结果含七值结局、拒绝分类、
// 证据快照与提交差分。返回 1 = 已调用算法层（out 有效）/ 0 = 参数非法（out 不变）。
int32_t lc_try_stitch(lc_handle_t h, const uint32_t* bgra, int32_t dir,
                      int32_t allowStabilityGate, LCTryStitchResult* out);

// —— 纯识别查询（不触碰任何累计状态；诊断与单测用）——
// 对齐 lc_match_core.cpp LongCaptureDetectMatch（双档识别主入口）：两帧 BGRA 各建
// LongMatchData 后识别；prior 由参数打包（对齐 LongCaptureBuildOffsetPrior 的产物；
// WeakTemporal 用空值，与无候选历史场景一致）。返回 1 成功 / 0 参数非法。
int32_t lc_detect_match(const uint32_t* prevBgra, const uint32_t* currBgra,
                        int32_t w, int32_t height, int32_t dir,
                        int32_t priorValid, int32_t priorExpectedAbsOffset,
                        LCDetectResult* out);
// 对齐 lc_match_core.cpp LongCaptureWeakRequiredConfidence（Weak 档动态置信度门槛）。
float lc_weak_required_confidence(int32_t viewportH, int32_t overlap);
// 对齐 lc_match_core.cpp LcFailReasonName（失败分类 → 日志字符串；指针常驻有效）。
const char* lc_fail_reason_name(int32_t failReason);

// —— 状态读取 ——
// LongCaptureContext 会话可读字段只读快照（见 LCStateSnapshot 注释）。
int32_t lc_get_state(lc_handle_t h, LCStateSnapshot* out);
// 成功位移历史（|d| 列表，对齐 lc->offsetHistory；cap 不足时截断，返回实际写入数）。
int32_t lc_get_offset_history(lc_handle_t h, int32_t* out, int32_t cap);
// 当前输出行窗口（内容坐标 → 拼接行坐标）：镜像 lc_frame_io_windows.cpp
// LongCaptureOutputRows（未裁剪 = [0, stitchH)；裁剪只收紧输出窗口，不动拼接缓冲）。
// 返回 1 成功 / 0 参数非法。
int32_t lc_get_output_rows(lc_handle_t h, int64_t* outTop, int64_t* outBottom);
// 读取拼接缓冲的显示行区间 [rowStart, rowStart+rowCount)：headRev（倒序头部段）+
// body（正序主体段）的双段映射（对齐 LongCaptureBuildResultBitmap 的合并取行序）。
// outBuf 容量须 ≥ rowCount*physW（按行写入）。返回写入行数 / 0 参数非法。
int64_t lc_read_rows(lc_handle_t h, int64_t rowStart, int64_t rowCount,
                     uint32_t* outBuf, int64_t outBufRows);
// 缩略图双段读取（同 lc_read_rows，行宽 = thumbW，头部段行数 = thumbHeadH；
// 供小地图面板两级缩略列绘制，对齐 LongCaptureRebuildThumb 的合并序）。
int64_t lc_read_thumb_rows(lc_handle_t h, int64_t rowStart, int64_t rowCount,
                           uint32_t* outBuf, int64_t outBufRows);

// —— 裁剪（#44 延迟剔除语义）——
// 裁剪登记：镜像 lc_toolbar_ui_windows.cpp LongCaptureApplyCrop 的纯状态段（UI 刷新调用除外）。
// row = LC_CROP_DISCARD_TOP / LC_CROP_DISCARD_BOTTOM / LC_CROP_RESET。登记只收紧输出
// 窗口并记录「待剔除区间」；物理删行由下一次朝该方向的成功提交触发（CommitStitch 入口
// 的 LongCaptureExecuteCropPurge）。返回 1 已应用 / 0 未应用（参数或行号非法）。
int32_t lc_apply_crop(lc_handle_t h, int32_t row);
// 对齐 lc_stitch_state.cpp LongCaptureHasCropConstraint。
int32_t lc_has_crop_constraint(lc_handle_t h);

// —— 滚轮先验 / Weak 时间一致性 ——
// 对齐 lc_stitch_state.cpp LongCaptureUpdateWheelEstimate（成功提交后折算 px/notch）。
void lc_update_wheel_estimate(lc_handle_t h, int32_t d);
// 滚轮增量累计（对齐 lc_session_windows.cpp 面板 WM_INPUT 的 wheelAccumDelta += delta；
// 成功提交时由算法层经 lc_update_wheel_estimate 消化清零）。
void lc_accumulate_wheel_delta(lc_handle_t h, int32_t delta);
// 对齐 lc_stitch_state.cpp LongCapturePushWeakCandidate（登记弱候选时间一致性样本）。
void lc_push_weak_candidate(lc_handle_t h, int32_t offset);
// 放弃当前 Weak 候选链：镜像 lc_session_windows.cpp 主循环 weakTries 耗尽分支的状态段
//（pendingMatch.valid = false + weakCandidateOffsets.clear()；不计失败、不触碰拼接）。
void lc_abandon_weak_chain(lc_handle_t h);
// 对齐 lc_stitch_state.cpp LongCaptureOffsetPlausible（历史跳变合理性校验；
// mode = LC_MODE_*）。返回 1 合理 / 0 异常 / -1 参数非法。
int32_t lc_offset_plausible(lc_handle_t h, int32_t d, int32_t mode);

// —— Tentative 视觉跟踪（外围状态层入口，绝不触碰正式拼接状态）——
// 对齐 lc_stitch_state.cpp LongCaptureTrackingSetVisual（视觉依据直接设定，可解冻）。
void lc_tracking_set_visual(lc_handle_t h, int64_t contentY, float confidence);
// 对齐 lc_stitch_state.cpp LongCaptureTrackingResetToCommitted（候选否决后回退基准）。
void lc_tracking_reset_to_committed(lc_handle_t h);
// 对齐 lc_stitch_state.cpp LongCaptureTrackingAdvancePredicted（纯预测推进：
// 冻结计数 / 漂移上限 / 历史范围约束由算法层内部保证）。
void lc_tracking_advance_predicted(lc_handle_t h, int32_t direction, double magnitude);
// 对齐 lc_stitch_state.cpp LongCaptureBuildTrackingEstimate（失败帧的跟踪估计）。
int32_t lc_build_tracking_estimate(lc_handle_t h, int32_t dir, LCTrackingEstimate* out);

// —— 常量导出 ——
// 算法层重试梯 / 节拍 / 到底确认常量（lc_session_windows.cpp 主循环所需；Windows 侧这些
// 常量定义于 lc_match_core.cpp，会话直接引用，macOS 会话经本函数取用避免硬编码漂移）。
void lc_get_algo_consts(LCAlgoConsts* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LC_BRIDGE_H
