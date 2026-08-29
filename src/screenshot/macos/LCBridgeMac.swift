import Foundation

// MARK: - 长截图算法层 Swift 封装
//
// 职责：对 src/screenshot/lc_bridge_mac.h 的 C ABI 提供 Swift 侧声明（@_silgen_name）与
// 安全封装——C 布局结构体镜像、BGRA 帧数据拷贝与尺寸校验、七值结局/证据结构转换、
// handle 生命周期守护（deinit 统一销毁）。算法层本体为 lc_match_core.cpp /
// lc_stitch_state.cpp（构建期 clang++ 编出 .o，随 swiftc -emit-library 链入
// libZToolsNative.dylib，见 scripts/build-swift.sh）。
//
// 设计约束：本文件仅依赖 Foundation、不引用模块内其他 Swift 文件——
// scripts/test-lc-mac.sh 会把本文件与 tests/lc_algorithm_test.swift 一起编译成
// 独立测试可执行（与 dylib 共用同一批算法层 .o，对象文件分目录避免重复符号）。
// C 结构体镜像与 lc_bridge_mac.h 逐字段同序同型（全部固定宽度标量，自然对齐无隐藏
// padding 差异）；lc_bridge_mac.h 布局变更时必须同步本文件（各镜像处有标注）。

// MARK: - C ABI 枚举镜像（与 lc_bridge_mac.h 宏逐值对齐）

/// 单帧采样七值结局（镜像 LC_OUTCOME_*，对齐 internal.h 的 LCSampleOutcome）。
enum LCSampleOutcome: Int32 {
    case stitched = 0        // 已提交且新增拼接行（唯一扩展累计内容的结局）
    case repositioned = 1    // 已提交但仅视口重定位（反向回滚，无新增行）
    case noChange = 2        // 内容未滚动（全同帧 / 匹配成功 d=0）
    case weakPending = 3     // Weak 候选首次成立：只登记待复核，未提交
    case weakRejected = 4    // Weak 候选被复核否决
    case unstable = 5        // 稳定性闸门未过（仅启用闸门时可能出现）
    case failed = 6          // 硬失败；已提交状态未被触碰（状态层铁律）
}

/// 失败原因分类（镜像 LC_FAIL_*，对齐 internal.h 的 LCFailReason）。
enum LCFailReason: Int32 {
    case none = 0
    case noCandidate = 1
    case candidateWeak = 2
    case peakAmbiguous = 3
    case globalMismatch = 4
    case seamMismatch = 5
    case spatialMismatch = 6
    case continuityMismatch = 7
    case profileMismatch = 8
    case roiInconsistent = 9
    case offsetImplausible = 10
    case directionConflict = 11
    case frameUnstable = 12
}

/// 匹配档位（镜像 LC_MODE_*，对齐 LongCaptureMatchMode）。
enum LCMatchMode: Int32 {
    case normal = 0
    case weakOverlap = 1
}

/// 匹配三值结论（镜像 LC_MATCH_STATUS_*，对齐 LongMatchStatus）。
enum LCMatchStatus: Int32 {
    case failed = 0
    case lowConfidence = 1
    case success = 2
}

// MARK: - C 布局结构体镜像（字段顺序/类型与 lc_bridge_mac.h 严格一致）

/// 会话创建配置（镜像 LCSessionConfig）。
struct LCSessionConfigC {
    var interval: Int32 = 0
    var physW: Int32 = 0
    var physH: Int32 = 0
    var thumbW: Int32 = 0
    var horizontal: Int32 = 0
}

/// 匹配证据快照（镜像 LCMatchEvidence，对齐 LongMatchOutcome 字段）。
struct LCMatchEvidenceC {
    var status: Int32 = 0
    var mode: Int32 = 0
    var failReason: Int32 = 0
    var offset: Int32 = 0
    var overlap: Int32 = 0
    var overall: Float = 0
    var seam: Float = 0
    var top: Float = 0
    var middle: Float = 0
    var bottom: Float = 0
    var spatial: Float = 0
    var continuity: Float = 0
    var profileScore: Float = 0
    var edgeCorrelation: Float = 0
    var peakGap: Float = 0
    var roiWeighted: Float = 0
    var confidence: Float = 0
    var textureRatio: Float = 0
    var validBandCount: Int32 = 0
    var agreeCount: Int32 = 0
    var bandOffsets: (Int32, Int32, Int32) = (0, 0, 0)
    var bandValid: (Int32, Int32, Int32) = (0, 0, 0)
}

/// 单帧喂入结果（镜像 LCTryStitchResult）。
struct LCTryStitchResultC {
    var outcome: Int32 = 0
    var failReason: Int32 = 0
    var addedRows: Int32 = 0
    var committedDelta: Int32 = 0
    var stitchH: Int32 = 0
    var sampleIndex: Int32 = 0
    var hasRejectEvidence: Int32 = 0
    var evidence: LCMatchEvidenceC = LCMatchEvidenceC()
    var pendingValid: Int32 = 0
    var pendingOffset: Int32 = 0
    var pendingConfidence: Float = 0
}

/// 独立识别结果（镜像 LCDetectResult，字段与 LCMatchEvidence 同构）。
struct LCDetectResultC {
    var status: Int32 = 0
    var mode: Int32 = 0
    var failReason: Int32 = 0
    var offset: Int32 = 0
    var overlap: Int32 = 0
    var overall: Float = 0
    var seam: Float = 0
    var top: Float = 0
    var middle: Float = 0
    var bottom: Float = 0
    var spatial: Float = 0
    var continuity: Float = 0
    var profileScore: Float = 0
    var edgeCorrelation: Float = 0
    var peakGap: Float = 0
    var roiWeighted: Float = 0
    var confidence: Float = 0
    var textureRatio: Float = 0
    var validBandCount: Int32 = 0
    var agreeCount: Int32 = 0
    var bandOffsets: (Int32, Int32, Int32) = (0, 0, 0)
    var bandValid: (Int32, Int32, Int32) = (0, 0, 0)
}

/// 失败帧跟踪估计（镜像 LCTrackingEstimate，对齐 LongCaptureTrackingEstimate）。
struct LCTrackingEstimateC {
    var valid: Int32 = 0
    var direction: Int32 = 0
    var predictedOffset: Double = 0
    var confidence: Double = 0
}

/// 状态快照（镜像 LCStateSnapshot；字段顺序与 lc_bridge_mac.h 严格一致）。
struct LCStateSnapshotC {
    var physW: Int32 = 0
    var physH: Int32 = 0
    var horizontal: Int32 = 0
    var stitchH: Int32 = 0
    var headRows: Int32 = 0
    var bodyRows: Int32 = 0
    var committedContentTop: Int64 = 0
    var tentativeContentTop: Int64 = 0
    var tentativeValid: Int32 = 0
    var tentativeConfidence: Float = 0
    var trackUnreliableStreak: Int32 = 0
    var trackingRevision: Int32 = 0
    var sampleIndex: Int32 = 0
    var pendingValid: Int32 = 0
    var pendingOffset: Int32 = 0
    var pendingConfidence: Float = 0
    var pendingMode: Int32 = 0
    var lastFailReason: Int32 = 0
    var offsetHistoryLen: Int32 = 0
    var wheelAccumDelta: Int32 = 0
    var pixelsPerWheelNotch: Float = 0
    var thumbW: Int32 = 0
    var thumbHeadH: Int32 = 0
    var thumbH: Int32 = 0
    var cropTopY: Int64 = 0
    var cropBottomY: Int64 = 0
    var cropPendTop: Int32 = 0
    var cropPendBottom: Int32 = 0
    var cropPendTopLo: Int64 = 0
    var cropPendTopHi: Int64 = 0
    var cropPendBottomLo: Int64 = 0
    var cropPendBottomHi: Int64 = 0
    var cropped: Int32 = 0
    var interval: Int32 = 0
}

/// 算法层常量（镜像 LCAlgoConsts：重试梯/节拍/到底确认参数）。
struct LCAlgoConstsC {
    var sampleAttempts: Int32 = 0
    var retryDelayNormal: (Int32, Int32, Int32, Int32, Int32) = (0, 0, 0, 0, 0)
    var weakRetryAttempts: Int32 = 0
    var retryDelayWeak: (Int32, Int32, Int32, Int32, Int32, Int32) = (0, 0, 0, 0, 0, 0)
    var stableMaxWaits: Int32 = 0
    var stableRetryDelay: (Int32, Int32, Int32) = (0, 0, 0)
    var quickResamples: Int32 = 0
    var resampleDelayQuick: (Int32, Int32) = (0, 0)
    var scrollSampleMaxGap: Int32 = 0
    var bottomConfirmSamples: Int32 = 0
    var weakMaxTries: Int32 = 0
    var stableRefMaxGapMs: Int32 = 0
    var cropInsetLogical: Int32 = 0
    var trackMinStep: Int32 = 0
}

// MARK: - C 符号声明（实现为 lc_bridge_mac.cpp，随算法层 .o 链入 dylib/测试可执行）

@_silgen_name("lc_abi_version") func lc_abi_version() -> Int32
@_silgen_name("lc_session_create") func lc_session_create(_ config: UnsafePointer<LCSessionConfigC>) -> UnsafeMutableRawPointer?
@_silgen_name("lc_session_destroy") func lc_session_destroy(_ h: UnsafeMutableRawPointer?)
@_silgen_name("lc_init_baseline") func lc_init_baseline(_ h: UnsafeMutableRawPointer?, _ bgra: UnsafePointer<UInt32>?, _ w: Int32, _ height: Int32) -> Int32
@_silgen_name("lc_try_stitch") func lc_try_stitch(_ h: UnsafeMutableRawPointer?, _ bgra: UnsafePointer<UInt32>?, _ dir: Int32, _ allowStabilityGate: Int32, _ out: UnsafeMutablePointer<LCTryStitchResultC>?) -> Int32
@_silgen_name("lc_detect_match") func lc_detect_match(_ prevBgra: UnsafePointer<UInt32>?, _ currBgra: UnsafePointer<UInt32>?, _ w: Int32, _ height: Int32, _ dir: Int32, _ priorValid: Int32, _ priorExpectedAbsOffset: Int32, _ out: UnsafeMutablePointer<LCDetectResultC>?) -> Int32
@_silgen_name("lc_weak_required_confidence") func lc_weak_required_confidence(_ viewportH: Int32, _ overlap: Int32) -> Float
@_silgen_name("lc_fail_reason_name") func lc_fail_reason_name(_ failReason: Int32) -> UnsafePointer<CChar>?
@_silgen_name("lc_get_state") func lc_get_state(_ h: UnsafeMutableRawPointer?, _ out: UnsafeMutablePointer<LCStateSnapshotC>?) -> Int32
@_silgen_name("lc_get_offset_history") func lc_get_offset_history(_ h: UnsafeMutableRawPointer?, _ out: UnsafeMutablePointer<Int32>?, _ cap: Int32) -> Int32
@_silgen_name("lc_get_output_rows") func lc_get_output_rows(_ h: UnsafeMutableRawPointer?, _ outTop: UnsafeMutablePointer<Int64>?, _ outBottom: UnsafeMutablePointer<Int64>?) -> Int32
@_silgen_name("lc_read_rows") func lc_read_rows(_ h: UnsafeMutableRawPointer?, _ rowStart: Int64, _ rowCount: Int64, _ outBuf: UnsafeMutablePointer<UInt32>?, _ outBufRows: Int64) -> Int64
@_silgen_name("lc_read_thumb_rows") func lc_read_thumb_rows(_ h: UnsafeMutableRawPointer?, _ rowStart: Int64, _ rowCount: Int64, _ outBuf: UnsafeMutablePointer<UInt32>?, _ outBufRows: Int64) -> Int64
@_silgen_name("lc_apply_crop") func lc_apply_crop(_ h: UnsafeMutableRawPointer?, _ row: Int32) -> Int32
@_silgen_name("lc_has_crop_constraint") func lc_has_crop_constraint(_ h: UnsafeMutableRawPointer?) -> Int32
@_silgen_name("lc_update_wheel_estimate") func lc_update_wheel_estimate(_ h: UnsafeMutableRawPointer?, _ d: Int32)
@_silgen_name("lc_accumulate_wheel_delta") func lc_accumulate_wheel_delta(_ h: UnsafeMutableRawPointer?, _ delta: Int32)
@_silgen_name("lc_push_weak_candidate") func lc_push_weak_candidate(_ h: UnsafeMutableRawPointer?, _ offset: Int32)
@_silgen_name("lc_abandon_weak_chain") func lc_abandon_weak_chain(_ h: UnsafeMutableRawPointer?)
@_silgen_name("lc_offset_plausible") func lc_offset_plausible(_ h: UnsafeMutableRawPointer?, _ d: Int32, _ mode: Int32) -> Int32
@_silgen_name("lc_tracking_set_visual") func lc_tracking_set_visual(_ h: UnsafeMutableRawPointer?, _ contentY: Int64, _ confidence: Float)
@_silgen_name("lc_tracking_reset_to_committed") func lc_tracking_reset_to_committed(_ h: UnsafeMutableRawPointer?)
@_silgen_name("lc_tracking_advance_predicted") func lc_tracking_advance_predicted(_ h: UnsafeMutableRawPointer?, _ direction: Int32, _ magnitude: Double)
@_silgen_name("lc_build_tracking_estimate") func lc_build_tracking_estimate(_ h: UnsafeMutableRawPointer?, _ dir: Int32, _ out: UnsafeMutablePointer<LCTrackingEstimateC>?) -> Int32
@_silgen_name("lc_get_algo_consts") func lc_get_algo_consts(_ out: UnsafeMutablePointer<LCAlgoConstsC>?)

// MARK: - Swift 侧友好类型（结果结构转换）

/// 匹配证据（LongMatchOutcome 的 Swift 形态；字段语义见 internal.h）。
struct LCMatchEvidence {
    let status: LCMatchStatus
    let mode: LCMatchMode
    let failReason: LCFailReason
    let offset: Int
    let overlap: Int
    let overall: Float
    let seam: Float
    let top: Float
    let middle: Float
    let bottom: Float
    let spatial: Float
    let continuity: Float
    let profileScore: Float
    let edgeCorrelation: Float
    let peakGap: Float
    let roiWeighted: Float
    let confidence: Float
    let textureRatio: Float
    let validBandCount: Int
    let agreeCount: Int
    let bandOffsets: [Int]
    let bandValid: [Bool]

    init(_ c: LCMatchEvidenceC) {
        status = LCMatchStatus(rawValue: c.status) ?? .failed
        mode = LCMatchMode(rawValue: c.mode) ?? .normal
        failReason = LCFailReason(rawValue: c.failReason) ?? .none
        offset = Int(c.offset)
        overlap = Int(c.overlap)
        overall = c.overall
        seam = c.seam
        top = c.top
        middle = c.middle
        bottom = c.bottom
        spatial = c.spatial
        continuity = c.continuity
        profileScore = c.profileScore
        edgeCorrelation = c.edgeCorrelation
        peakGap = c.peakGap
        roiWeighted = c.roiWeighted
        confidence = c.confidence
        textureRatio = c.textureRatio
        validBandCount = Int(c.validBandCount)
        agreeCount = Int(c.agreeCount)
        bandOffsets = [Int(c.bandOffsets.0), Int(c.bandOffsets.1), Int(c.bandOffsets.2)]
        bandValid = [c.bandValid.0 != 0, c.bandValid.1 != 0, c.bandValid.2 != 0]
    }
}

/// 单帧喂入结果（lc_try_stitch 的 Swift 形态）。
struct LCTryStitchResult {
    let outcome: LCSampleOutcome
    let failReason: LCFailReason
    let addedRows: Int          // 仅 stitched 时 > 0
    let committedDelta: Int     // committedContentTop 推进量（stitched/repositioned = 本帧位移）
    let stitchH: Int            // 调用后的拼接总行数
    let sampleIndex: Int
    let hasRejectEvidence: Bool
    let evidence: LCMatchEvidence   // 拒绝证据快照（仅 FAILED/WEAK_REJECTED 有意义）
    let pendingValid: Bool
    let pendingOffset: Int
    let pendingConfidence: Float

    init(_ c: LCTryStitchResultC) {
        outcome = LCSampleOutcome(rawValue: c.outcome) ?? .failed
        failReason = LCFailReason(rawValue: c.failReason) ?? .none
        addedRows = Int(c.addedRows)
        committedDelta = Int(c.committedDelta)
        stitchH = Int(c.stitchH)
        sampleIndex = Int(c.sampleIndex)
        hasRejectEvidence = c.hasRejectEvidence != 0
        evidence = LCMatchEvidence(c.evidence)
        pendingValid = c.pendingValid != 0
        pendingOffset = Int(c.pendingOffset)
        pendingConfidence = c.pendingConfidence
    }
}

/// 独立识别结果（lc_detect_match 的 Swift 形态）。
struct LCDetectResult {
    let status: LCMatchStatus
    let mode: LCMatchMode
    let failReason: LCFailReason
    let offset: Int
    let overlap: Int
    let overall: Float
    let seam: Float
    let spatial: Float
    let continuity: Float
    let profileScore: Float
    let edgeCorrelation: Float
    let peakGap: Float
    let roiWeighted: Float
    let confidence: Float

    init(_ c: LCDetectResultC) {
        status = LCMatchStatus(rawValue: c.status) ?? .failed
        mode = LCMatchMode(rawValue: c.mode) ?? .normal
        failReason = LCFailReason(rawValue: c.failReason) ?? .none
        offset = Int(c.offset)
        overlap = Int(c.overlap)
        overall = c.overall
        seam = c.seam
        spatial = c.spatial
        continuity = c.continuity
        profileScore = c.profileScore
        edgeCorrelation = c.edgeCorrelation
        peakGap = c.peakGap
        roiWeighted = c.roiWeighted
        confidence = c.confidence
    }
}

/// 算法层状态快照（LCStateSnapshot 的 Swift 形态；会话侧簿记字段不在此，
/// 见 lc_bridge_mac.h 的 LCStateSnapshot 注释）。
struct LCState {
    let physW: Int
    let physH: Int
    let horizontal: Bool
    let stitchH: Int
    let headRows: Int
    let bodyRows: Int
    let committedContentTop: Int64
    let tentativeContentTop: Int64
    let tentativeValid: Bool
    let tentativeConfidence: Float
    let trackUnreliableStreak: Int
    let trackingRevision: Int
    let sampleIndex: Int
    let pendingValid: Bool
    let pendingOffset: Int
    let pendingConfidence: Float
    let pendingMode: LCMatchMode
    let lastFailReason: LCFailReason
    let offsetHistoryLen: Int
    let wheelAccumDelta: Int
    let pixelsPerWheelNotch: Float
    let thumbW: Int
    let thumbHeadH: Int
    let thumbH: Int
    let cropTopY: Int64
    let cropBottomY: Int64
    let cropPendTop: Bool
    let cropPendBottom: Bool
    let cropPendTopLo: Int64
    let cropPendTopHi: Int64
    let cropPendBottomLo: Int64
    let cropPendBottomHi: Int64
    let cropped: Bool
    let interval: Int

    init(_ c: LCStateSnapshotC) {
        physW = Int(c.physW)
        physH = Int(c.physH)
        horizontal = c.horizontal != 0
        stitchH = Int(c.stitchH)
        headRows = Int(c.headRows)
        bodyRows = Int(c.bodyRows)
        committedContentTop = c.committedContentTop
        tentativeContentTop = c.tentativeContentTop
        tentativeValid = c.tentativeValid != 0
        tentativeConfidence = c.tentativeConfidence
        trackUnreliableStreak = Int(c.trackUnreliableStreak)
        trackingRevision = Int(c.trackingRevision)
        sampleIndex = Int(c.sampleIndex)
        pendingValid = c.pendingValid != 0
        pendingOffset = Int(c.pendingOffset)
        pendingConfidence = c.pendingConfidence
        pendingMode = LCMatchMode(rawValue: c.pendingMode) ?? .normal
        lastFailReason = LCFailReason(rawValue: c.lastFailReason) ?? .none
        offsetHistoryLen = Int(c.offsetHistoryLen)
        wheelAccumDelta = Int(c.wheelAccumDelta)
        pixelsPerWheelNotch = c.pixelsPerWheelNotch
        thumbW = Int(c.thumbW)
        thumbHeadH = Int(c.thumbHeadH)
        thumbH = Int(c.thumbH)
        cropTopY = c.cropTopY
        cropBottomY = c.cropBottomY
        cropPendTop = c.cropPendTop != 0
        cropPendBottom = c.cropPendBottom != 0
        cropPendTopLo = c.cropPendTopLo
        cropPendTopHi = c.cropPendTopHi
        cropPendBottomLo = c.cropPendBottomLo
        cropPendBottomHi = c.cropPendBottomHi
        cropped = c.cropped != 0
        interval = Int(c.interval)
    }
}

/// 算法层常量（重试梯/节拍/到底确认；macOS 会话主循环按 lc_session_windows.cpp 同式取用）。
struct LCAlgoConsts {
    let sampleAttempts: Int
    let retryDelayNormal: [Int]      // 长度 = sampleAttempts - 1
    let weakRetryAttempts: Int
    let retryDelayWeak: [Int]
    let stableMaxWaits: Int
    let stableRetryDelay: [Int]
    let quickResamples: Int
    let resampleDelayQuick: [Int]
    let scrollSampleMaxGap: Int
    let bottomConfirmSamples: Int
    let weakMaxTries: Int
    let stableRefMaxGapMs: Int
    let cropInsetLogical: Int
    let trackMinStep: Int

    /// 进程内只取一次（常量为编译期定值）。
    static let shared: LCAlgoConsts = {
        var c = LCAlgoConstsC()
        lc_get_algo_consts(&c)
        return LCAlgoConsts(
            sampleAttempts: Int(c.sampleAttempts),
            retryDelayNormal: [Int(c.retryDelayNormal.0), Int(c.retryDelayNormal.1),
                               Int(c.retryDelayNormal.2), Int(c.retryDelayNormal.3),
                               Int(c.retryDelayNormal.4)],
            weakRetryAttempts: Int(c.weakRetryAttempts),
            retryDelayWeak: [Int(c.retryDelayWeak.0), Int(c.retryDelayWeak.1),
                             Int(c.retryDelayWeak.2), Int(c.retryDelayWeak.3),
                             Int(c.retryDelayWeak.4), Int(c.retryDelayWeak.5)],
            stableMaxWaits: Int(c.stableMaxWaits),
            stableRetryDelay: [Int(c.stableRetryDelay.0), Int(c.stableRetryDelay.1),
                               Int(c.stableRetryDelay.2)],
            quickResamples: Int(c.quickResamples),
            resampleDelayQuick: [Int(c.resampleDelayQuick.0), Int(c.resampleDelayQuick.1)],
            scrollSampleMaxGap: Int(c.scrollSampleMaxGap),
            bottomConfirmSamples: Int(c.bottomConfirmSamples),
            weakMaxTries: Int(c.weakMaxTries),
            stableRefMaxGapMs: Int(c.stableRefMaxGapMs),
            cropInsetLogical: Int(c.cropInsetLogical),
            trackMinStep: Int(c.trackMinStep))
    }()
}

/// 封装层错误（参数校验失败时绝不把非法输入透传给 C ABI）。
enum LCAlgorithmError: Error, Equatable {
    case abiVersionMismatch(actual: Int32)      // dylib 与 Swift 封装版本不一致
    case invalidConfiguration                    // 创建参数非法（宽高 < 1 等）
    case frameSizeMismatch(expected: Int, actual: Int)
    case callFailed(String)                      // C 函数返回失败（句柄/参数被拒）
}

// MARK: - 算法层会话封装（handle 生命周期守护）

/// 长截图算法层会话：守护 lc_handle_t 生命周期（init 创建 / deinit 销毁，杜绝泄漏），
/// 帧数据按 [UInt32] 拷贝传递并在跨界前做尺寸校验。对应 lc_stitch_state 的
/// LongCaptureContext 状态对象；纯 Swift 值语义出口，不暴露任何指针。
/// 线程约定：对同一实例的调用须串行（对齐 Windows 侧捕获线程单线程驱动算法层）。
final class LCAlgorithmSession {
    /// 帧缓冲宽（物理像素）。
    let physWidth: Int
    /// 帧缓冲高（物理像素）。
    let physHeight: Int
    private let handle: UnsafeMutableRawPointer

    /// 会话配置（对应 LCSessionConfig；语义见 lc_bridge_mac.h）。
    struct Config {
        var interval: Int = 250
        var thumbW: Int = 0      // 缩略图列宽（0 = 关闭；> physW 时由 bridge 钳到 physW）
        var horizontal: Bool = false
    }

    /// 创建算法层会话（对齐 lc_session_windows.cpp BeginLongCapture 的上下文构造段）。
    init(width: Int, height: Int, config: Config = Config()) throws {
        guard width >= 1, height >= 1 else { throw LCAlgorithmError.invalidConfiguration }
        let abi = lc_abi_version()
        guard abi == 2 else { throw LCAlgorithmError.abiVersionMismatch(actual: abi) }
        var cfg = LCSessionConfigC(
            interval: Int32(config.interval),
            physW: Int32(width),
            physH: Int32(height),
            thumbW: Int32(config.thumbW),
            horizontal: config.horizontal ? 1 : 0)
        guard let h = lc_session_create(&cfg) else { throw LCAlgorithmError.invalidConfiguration }
        handle = h
        physWidth = width
        physHeight = height
    }

    deinit {
        lc_session_destroy(handle)
    }

    // —— 帧管线 ——

    /// 以首帧初始化基准（对齐 lc_frame_io_windows.cpp LongCaptureInitBaseline：纯数据初始化，
    /// 不抓屏）。`bgra.count` 必须等于 physWidth * physHeight。
    func initBaseline(bgra: [UInt32]) throws {
        try validateFrame(bgra)
        guard lc_init_baseline(handle, bgra, Int32(physWidth), Int32(physHeight)) == 1 else {
            throw LCAlgorithmError.callFailed("lc_init_baseline")
        }
    }

    /// 单帧「识别 → offset 校验 →（Weak 档）延迟确认 → 提交」管线（转发
    /// LongCaptureTryStitch）。direction：+1 向下滚 / -1 向上滚 / 0 未知；
    /// allowStabilityGate 对齐 RunLongCapture 的传参（采样主循环启用，合成帧注入默认关）。
    func tryStitch(bgra: [UInt32], direction: Int32, allowStabilityGate: Bool = false) throws -> LCTryStitchResult {
        try validateFrame(bgra)
        var out = LCTryStitchResultC()
        guard lc_try_stitch(handle, bgra, direction, allowStabilityGate ? 1 : 0, &out) == 1 else {
            throw LCAlgorithmError.callFailed("lc_try_stitch")
        }
        return LCTryStitchResult(out)
    }

    /// 独立识别查询（转发 LongCaptureDetectMatch，纯函数不触碰累计状态；诊断/单测用）。
    /// priorExpectedOffset：位移先验（对齐 LongCaptureBuildOffsetPrior 的产物语义）。
    func detectMatch(prev: [UInt32], curr: [UInt32], direction: Int32,
                     priorExpectedOffset: Int? = nil) throws -> LCDetectResult {
        try validateFrame(prev)
        try validateFrame(curr)
        var out = LCDetectResultC()
        guard lc_detect_match(prev, curr, Int32(physWidth), Int32(physHeight),
                              direction,
                              priorExpectedOffset != nil ? 1 : 0,
                              Int32(priorExpectedOffset ?? 0),
                              &out) == 1 else {
            throw LCAlgorithmError.callFailed("lc_detect_match")
        }
        return LCDetectResult(out)
    }

    // —— 状态读取 ——

    /// 会话状态快照（LongCaptureContext 会话可读字段的只读打包）。
    var state: LCState {
        var out = LCStateSnapshotC()
        guard lc_get_state(handle, &out) == 1 else {
            // 句柄恒非空（生命周期守护），此路径仅防御性兜底
            return LCState(LCStateSnapshotC())
        }
        return LCState(out)
    }

    /// 成功位移历史（|d| 列表，对齐 lc->offsetHistory）。
    func offsetHistory() -> [Int] {
        let n = state.offsetHistoryLen
        guard n > 0 else { return [] }
        var buf = [Int32](repeating: 0, count: n)
        let written = lc_get_offset_history(handle, &buf, Int32(n))
        return buf.prefix(Int(written)).map { Int($0) }
    }

    /// 当前输出行窗口（裁剪后的 [top, bottom)；未裁剪 = [0, stitchH)）。
    func outputRows() throws -> (top: Int64, bottom: Int64) {
        var top: Int64 = 0, bottom: Int64 = 0
        guard lc_get_output_rows(handle, &top, &bottom) == 1 else {
            throw LCAlgorithmError.callFailed("lc_get_output_rows")
        }
        return (top, bottom)
    }

    /// 读取拼接缓冲的显示行区间 [start, start+count)（headRev 倒序头部段 + body 正序
    /// 主体段的双段映射已在此解出；完整合并与横向回转由输出层组装）。
    func readRows(start: Int, count: Int) throws -> [UInt32] {
        try readSegment(start: start, count: count, thumb: false)
    }

    /// 读取缩略图双段的显示行区间（供小地图面板两级缩略列绘制）。
    func readThumbRows(start: Int, count: Int) throws -> [UInt32] {
        try readSegment(start: start, count: count, thumb: true)
    }

    private func readSegment(start: Int, count: Int, thumb: Bool) throws -> [UInt32] {
        let rowW = thumb ? state.thumbW : physWidth
        guard count > 0, rowW > 0 else { return [] }
        var buf = [UInt32](repeating: 0, count: count * rowW)
        let written = thumb
            ? lc_read_thumb_rows(handle, Int64(start), Int64(count), &buf, Int64(count))
            : lc_read_rows(handle, Int64(start), Int64(count), &buf, Int64(count))
        guard written == count else {
            throw LCAlgorithmError.callFailed(thumb ? "lc_read_thumb_rows" : "lc_read_rows")
        }
        return buf
    }

    // —— 裁剪（#44 延迟剔除语义） ——

    /// 裁剪登记（镜像 lc_toolbar_ui_windows.cpp LongCaptureApplyCrop 的纯状态段）。
    /// row：0 = 丢弃上方（横向=左侧）、1 = 丢弃下方（横向=右侧）、2 = 重置（仅已裁剪时）。
    /// 返回是否已应用；物理删行由下一次朝该方向的成功提交自动触发（CommitStitch 入口）。
    @discardableResult
    func applyCrop(row: Int32) throws -> Bool {
        guard lc_apply_crop(handle, row) == 1 else { return false }
        return true
    }

    /// 是否存在任何生效的裁剪约束（转发 LongCaptureHasCropConstraint）。
    var hasCropConstraint: Bool { lc_has_crop_constraint(handle) != 0 }

    // —— 滚轮先验 / Weak 时间一致性 ——

    /// 成功提交后折算 px/notch 先验（转发 LongCaptureUpdateWheelEstimate）。
    func updateWheelEstimate(d: Int32) { lc_update_wheel_estimate(handle, d) }

    /// 滚轮增量累计（对齐 lc_session_windows.cpp 面板 WM_INPUT 的 wheelAccumDelta 累计）。
    func accumulateWheelDelta(delta: Int32) { lc_accumulate_wheel_delta(handle, delta) }

    /// 登记弱候选时间一致性样本（转发 LongCapturePushWeakCandidate）。
    func pushWeakCandidate(offset: Int32) { lc_push_weak_candidate(handle, offset) }

    /// 放弃当前 Weak 候选链（镜像 lc_session_windows.cpp 的 weakTries 耗尽状态段；
    /// weakTries 计数本身归会话层自持）。
    func abandonWeakChain() { lc_abandon_weak_chain(handle) }

    /// 历史跳变合理性校验（转发 LongCaptureOffsetPlausible；mode 见 LCMatchMode）。
    func offsetPlausible(d: Int32, mode: LCMatchMode) -> Bool {
        lc_offset_plausible(handle, d, mode.rawValue) == 1
    }

    // —— Tentative 视觉跟踪 ——

    /// 视觉依据直接设定 tentative 位置（转发 LongCaptureTrackingSetVisual，可解冻）。
    func trackingSetVisual(contentY: Int64, confidence: Float) {
        lc_tracking_set_visual(handle, contentY, confidence)
    }

    /// tentative 回退到 committed 基准（转发 LongCaptureTrackingResetToCommitted）。
    func trackingResetToCommitted() { lc_tracking_reset_to_committed(handle) }

    /// 纯预测推进（转发 LongCaptureTrackingAdvancePredicted；冻结/漂移上限由算法层保证）。
    func trackingAdvancePredicted(direction: Int32, magnitude: Double) {
        lc_tracking_advance_predicted(handle, direction, magnitude)
    }

    /// 失败帧的跟踪估计（转发 LongCaptureBuildTrackingEstimate；只服务 tentative，
    /// 绝不进入提交）。
    func buildTrackingEstimate(direction: Int32) throws -> (valid: Bool, direction: Int,
                                                            predictedOffset: Double, confidence: Double) {
        var out = LCTrackingEstimateC()
        guard lc_build_tracking_estimate(handle, direction, &out) == 1 else {
            throw LCAlgorithmError.callFailed("lc_build_tracking_estimate")
        }
        return (out.valid != 0, Int(out.direction), out.predictedOffset, out.confidence)
    }

    // —— 私有 ——

    /// 帧尺寸校验（跨界前拦截，避免把错误长度的缓冲交给算法层）。
    private func validateFrame(_ bgra: [UInt32]) throws {
        let expected = physWidth * physHeight
        guard bgra.count == expected else {
            throw LCAlgorithmError.frameSizeMismatch(expected: expected, actual: bgra.count)
        }
    }
}
