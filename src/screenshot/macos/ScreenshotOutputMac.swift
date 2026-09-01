import Foundation
import AppKit
import CoreGraphics
import UniformTypeIdentifiers

// MARK: - 输出完善（macOS；Windows 基准 output_windows.cpp）
//
// 本文件承载输出子系统——确认输出与保存共用的统一输出管线（长截图完成/保存直接复用）：
// - 统一输出辅助（对齐 EncodeHBitmapPng：单次编码同时产出 base64 与文件字节）：
//   物理尺寸位图（Retina 2x）+ 逻辑坐标 scaleBy 绘制 → 合成标注（马赛克现场重算）
//   → 圆角蒙版（radius>0）→ 单次 PNG 编码 → ScreenshotPngOutput（pngData + base64）
// - 圆角透明导出（对齐 BuildRoundedArgbFinal）：CGContext 圆角 clip 路径 + 透明外围 →
//   保留 alpha 的 PNG。macOS 原生支持 alpha，等价 Windows「预乘 ARGB + 圆角蒙版逐像素
//   alpha → PARGB PNG + CF_DIB/PNG 双格式剪贴板」的最终视觉语义（单格式即达成跨应用透明）
// - 保存对话框（对齐 PromptSaveFilePath）：NSSavePanel 默认目录 Pictures、默认名
//   Screenshot_YYYYMMDD_HHMMSS.png、仅允许 PNG、自带覆盖确认；弹出前临时降覆盖层/工具栏
//   浮层族层级（对齐 Windows 摘除 TOPMOST）；取消回编辑态（无回调）；保存成功=原子落盘
//   + base64 回调但不写剪贴板，且会话结束（对齐 Windows「无论保存成功与否均关闭截图窗口」）
//
// Windows 出处标注：output_windows.cpp（BuildRoundedArgbFinal / EncodePremulArgbPng /
// EncodeHBitmapPng / PromptSaveFilePath / MakeDefaultScreenshotName）、
// overlay_input_windows.cpp（TB_Save 分支：回调后会话状态与取消语义）。
//
// 坐标系约定：与覆盖层会话一致——CG 全局逻辑坐标（左上原点）；回调契约的
// x/y/x2/y2/width/height 均为逻辑尺寸，base64 图像为物理像素（Retina 2x，PNG 携带
// DPI 元数据按逻辑尺寸显示；Windows 端 dpiScale≈1.0 两者相等）。

// MARK: - 统一输出产物

/// 单次 PNG 编码的统一产物（对齐 Windows EncodeHBitmapPng 的「单次编码同时满足多个
/// 输出需求」：base64Out + rawOut + 落盘，避免同一张图被反复编码）。确认输出与保存
/// 路径共用；长截图「完成并复制 / 保存」直接复用。
struct ScreenshotPngOutput {
    let pngData: Data   // PNG 文件字节（剪贴板写入与原子落盘共用）
    let base64: String  // data:image/png;base64,... data URL（回调契约字段）
}

// MARK: - 保存对话框辅助（Windows 基准 output_windows.cpp）

/// 生成默认保存文件名（对齐 MakeDefaultScreenshotName：Screenshot_YYYYMMDD_HHMMSS.png，
/// wsprintfW %04d%02d%02d_%02d%02d%02d + localtime 的本地时间语义，output_windows.cpp L594-603）。
/// - Returns: 默认文件名字符串
func scMakeDefaultScreenshotName() -> String {
    let formatter = DateFormatter()
    // POSIX locale 锁定数字格式，避免用户区域设置注入本地化分隔符（等价 wsprintfW 纯数字输出）
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.dateFormat = "'Screenshot_'yyyyMMdd'_'HHmmss'.png'"
    return formatter.string(from: Date())
}

/// 弹出系统保存对话框（对齐 PromptSaveFilePath，output_windows.cpp L611-664），返回用户
/// 选择的文件完整路径；用户取消或无有效路径返回 nil。
/// 行为对齐：
/// - 默认目录 Pictures（Windows FOLDERID_Pictures）；目录不存在回落用户主目录
///   （Windows 回落桌面 FOLDERID_Desktop；macOS 无等价语义，按任务基准回落主目录）
/// - 默认名 Screenshot_YYYYMMDD_HHMMSS.png（MakeDefaultScreenshotName）
/// - 仅允许 PNG（Windows lpstrFilter "PNG 图像 (*.png)" + lpstrDefExt "png"：非 PNG 文件
///   置灰不可选、用户未输扩展名时自动补 .png）
/// - 覆盖提示：NSSavePanel 对已存在文件自带「替换确认」弹窗，与 Windows OFN_OVERWRITEPROMPT
///   同为系统对话框自带覆盖确认（非自绘），语义一致
/// 须在主线程调用（runModal 为模态事件循环，运行在会话泵所在的主线程；模态期间会话泵
/// 暂停属预期，对齐 Windows GetSaveFileNameW 模态循环）。调用方须先临时降覆盖层/工具栏
/// 浮层族层级（Windows 弹出前摘除 TOPMOST 的等价处理，见 ScreenshotOverlaySession.
/// saveSelectionToFile 与 duckOverlayLevelsForSaveModal）。
/// - Returns: 选定文件的完整路径；取消返回 nil
func scPromptSaveFilePath() -> String? {
    let panel = NSSavePanel()
    // 默认目录：Pictures（FOLDERID_Pictures 对应的 ~/.Pictures）；目录不存在回落主目录
    let fileManager = FileManager.default
    var defaultDir: URL?
    if let pictures = fileManager.urls(for: .picturesDirectory, in: .userDomainMask).first {
        var isDir: ObjCBool = false
        if fileManager.fileExists(atPath: pictures.path, isDirectory: &isDir), isDir.boolValue {
            defaultDir = pictures
        }
    }
    panel.directoryURL = defaultDir ?? fileManager.homeDirectoryForCurrentUser

    // 默认名：Screenshot_YYYYMMDD_HHMMSS.png（对齐 MakeDefaultScreenshotName）
    panel.nameFieldStringValue = scMakeDefaultScreenshotName()
    panel.canCreateDirectories = true   // 允许新建目录（GetSaveFileNameW 亦具备，保持能力一致）
    if #available(macOS 11.0, *) {
        // 仅允许 PNG（对齐 lpstrFilter "PNG 图像 (*.png)"；UTType 框架经 #available
        // 守卫自动弱链接，10.15 目标不受影响）
        panel.allowedContentTypes = [.png]
    } else {
        // 10.15 运行时回退（旧 API，12.0 起废弃但功能等价；老系统唯一可达分支）
        panel.allowedFileTypes = ["png"]
    }

    // runModal：模态事件循环。OK=用户确认保存路径；其余（ESC/取消按钮）=用户取消。
    // 覆盖提示由面板对已存在文件自动弹出（OFN_OVERWRITEPROMPT 语义）。
    guard panel.runModal() == NSApplication.ModalResponse.OK, let url = panel.url else {
        return nil
    }
    return url.path
}

/// 构造确认输出与保存成功共用的回调 JSON（契约字段与 Windows CallScreenshotJs 一致：
/// success/x/y/x2/y2/width/height/base64；坐标为 CG 全局逻辑坐标，x2/y2 = min + size，
/// 与既有 confirmSelection 输出逐字节一致）。
/// - Parameters:
///   - sel: 选区（CG 全局逻辑坐标）
///   - base64: data URL 形式的 PNG base64
/// - Returns: 回调 JSON 字符串
func scSuccessPayloadJSON(sel: CGRect, base64: String) -> String {
    let x = Int(sel.minX.rounded())
    let y = Int(sel.minY.rounded())
    let width = Int(sel.width.rounded())
    let height = Int(sel.height.rounded())
    return "{"
        + "\"success\":true"
        + ",\"x\":\(x)"
        + ",\"y\":\(y)"
        + ",\"x2\":\(x + width)"
        + ",\"y2\":\(y + height)"
        + ",\"width\":\(width)"
        + ",\"height\":\(height)"
        + ",\"base64\":\"\(base64)\""
        + "}"
}

/// 将 PNG 图像写入系统剪贴板（对齐 Windows 确认路径 SaveBitmapToClipboard：成功出图必进
/// 剪贴板）。圆角透明图直接以 PNG 类型写入，NSPasteboard 保留 alpha 通道——跨应用透明
/// 对齐 Windows 圆角路径 CF_DIB+PNG 双格式中 PNG 的角色（透明度最可靠载体），macOS 单
/// PNG 格式即达成等价语义，无需双格式。NSPasteboard 官方文档标注线程安全。
/// （自 ScreenshotMac.swift 迁入：输出辅助统一收口本文件。）
/// - Parameter pngData: PNG 字节
/// - Returns: 写入成功返回 true
func writePngToPasteboard(_ pngData: Data) -> Bool {
    let pasteboard = NSPasteboard.general
    pasteboard.clearContents()
    return pasteboard.setData(pngData, forType: .png)
}

// MARK: - 会话输出扩展（统一输出管线 + 保存流）

extension ScreenshotOverlaySession {
    // MARK: 统一输出管线（确认输出 / 保存共用；长截图复用）

    /// 按选区从常驻底图裁剪物理像素图（自 confirmSelection 抽出的公共步骤，对齐
    /// ExtractRegionResult / ComposeSelectedBitmap 的区域提取：
    /// 物理 = (逻辑 − 虚拟屏原点) × dpiScale，四边钳制在位图内）。
    /// - Parameter sel: 选区（CG 全局逻辑坐标）
    /// - Returns: 物理像素 CGImage；选区为空或裁剪失败返回 nil（失败不输出黑图）
    func cropSelectionPhysical(_ sel: CGRect) -> CGImage? {
        let scale = baseFrame.scale
        let imgW = CGFloat(baseFrame.image.width)
        let imgH = CGFloat(baseFrame.image.height)
        let px0 = max(0, ((sel.minX - baseFrame.origin.x) * scale).rounded(.down))
        let py0 = max(0, ((sel.minY - baseFrame.origin.y) * scale).rounded(.down))
        let px1 = min(imgW, ((sel.maxX - baseFrame.origin.x) * scale).rounded(.down))
        let py1 = min(imgH, ((sel.maxY - baseFrame.origin.y) * scale).rounded(.down))
        guard px1 > px0, py1 > py0 else { return nil }
        return baseFrame.image.cropping(to: CGRect(x: px0, y: py0, width: px1 - px0, height: py1 - py0))
    }

    /// 构建最终输出图并单次编码 PNG（输出路径复用核心；对齐 Windows
    /// EncodeHBitmapPng + EncodePremulArgbPng + BuildRoundedArgbFinal 的合并语义）：
    /// 物理尺寸位图（Retina 下 2x）+ 逻辑坐标 scaleBy 绘制 → 合成标注（马赛克现场重算）
    /// → 圆角蒙版（radius>0）→ 单次编码同时产出 base64 data URL 与 PNG 文件字节。
    ///
    /// 输出分辨率：位图按「逻辑尺寸 × dpiScale」建，底图物理像素
    /// 1:1 落位、标注按矢量在高分辨率下重渲染——Retina 下不再缩回逻辑尺寸（缩回会丢一半
    /// 分辨率导致发虚；Windows 在 dpiScale≈1.0 时本就不缩放，macOS 主流 2x 屏必须保留
    /// 物理像素才能与屏幕所见一致）。PNG 写入 DPI 元数据（pixels/size×72，Retina=144dpi），
    /// 看图应用按逻辑尺寸显示，与系统截图（Cmd+Shift+4）行为一致；回调契约的
    /// width/height 仍为逻辑尺寸，跨端语义不变。
    ///
    /// 圆角蒙版实现（对齐 BuildRoundedArgbFinal 的逐像素 alpha 预乘）：
    /// 上下文为 premultipliedLast（预乘 RGBA，等价 PARGB 语义），内容落笔前先经圆角
    /// 路径裁剪，路径外保持透明——CGContext clip 的抗锯齿 coverage 即逐像素 alpha
    /// （内部不透明、弧边预乘、外部全透明），PNG 编码器（NSBitmapImageRep）写回非预乘
    /// 字节，视觉正确。半径钳制与圆弧几何对齐 Windows：r = min(radius, min(w,h)/2)
    /// （BuildRoundedArgbFinal L407），CGPath(roundedRect:) 与覆盖层选区圆角绘制同款
    /// （AddRoundedRect 均为圆弧构造），导出与所见一致。
    ///
    /// - Parameters:
    ///   - cropped: 按选区裁剪的物理像素底图（cropSelectionPhysical 产物）
    ///   - sel: 选区（CG 全局逻辑坐标；输出尺寸与标注偏移基准）
    ///   - cornerRadius: 圆角半径（逻辑点；0 = 直角输出，与直角路径行为一致）
    /// - Returns: 统一输出产物；任一步失败返回 nil（失败路径不输出黑图，对齐 Windows
    ///   「拷贝/缩放/编码失败不输出黑图」）
    func buildFinalPngOutput(cropped: CGImage, sel: CGRect, cornerRadius: CGFloat) -> ScreenshotPngOutput? {
        let width = Int(sel.width.rounded())
        let height = Int(sel.height.rounded())
        let scale = baseFrame.scale
        // 位图按物理尺寸建（Retina 2x），逻辑坐标经 scaleBy 映射——底图 1:1 落位不重采样，
        // 标注/文字/圆角在高分辨率下重渲染（修复缩回逻辑尺寸导致的输出模糊）
        let physW = Int((sel.width * scale).rounded())
        let physH = Int((sel.height * scale).rounded())
        guard physW > 0, physH > 0, scale > 0,
              let ctx = CGContext(
                data: nil, width: physW, height: physH,
                bitsPerComponent: 8, bytesPerRow: physW * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
              ) else { return nil }
        ctx.scaleBy(x: scale, y: scale)

        // 圆角蒙版（radius>0）：内容先经圆角路径裁剪再落笔，路径外保持透明。
        // radius==0 不加 clip，直角输出几何不变；路径按逻辑坐标构建，clip 抗锯齿经
        // scaleBy 在物理分辨率上生成，弧边平滑度随输出分辨率提升。
        let radius = min(cornerRadius, min(CGFloat(width), CGFloat(height)) / 2)
        if radius >= 1 {
            ctx.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: width, height: height),
                               cornerWidth: radius, cornerHeight: radius, transform: nil))
            ctx.clip()
        }

        // 底图：物理像素 1:1 落位（缩放后上下文中逻辑选区矩形即物理尺寸；混合 DPI 等
        // scale 失配场景仍由插值兜底）
        ctx.interpolationQuality = .high
        ctx.draw(cropped, in: CGRect(x: 0, y: 0, width: CGFloat(width), height: CGFloat(height)))

        // 标注合成（原 compositeAnnotationsToPng 并入统一管线；无标注时跳过，保持
        // 直角空标注路径行为不变）：raw CGContext 原点在左下，翻转为左上原点后复用
        // 覆盖层同款绘制函数（final 位图原点 = 选区左上角，ox/oy 对齐 CompositeAnnotations）。
        // 马赛克先从裁剪底图现场重算，矢量/文字标注清晰覆盖其上（对齐 CompositeAnnotations
        // 的马赛克先行揭示 + 标注覆盖次序）。
        if !annotations.isEmpty {
            ctx.saveGState()
            ctx.translateBy(x: 0, y: CGFloat(height))
            ctx.scaleBy(x: 1, y: -1)
            compositeMosaicLayer(ctx, cropped: cropped, region: sel)
            for a in annotations {
                scDrawAnnotation(ctx, a, ox: -sel.minX, oy: -sel.minY)
            }
            ctx.restoreGState()
        }

        // 单次 PNG 编码同时产出 base64 与文件字节（EncodeHBitmapPng 等价）。
        // rep.size 声明逻辑尺寸 → PNG 携带 DPI 元数据（Retina=144dpi），看图应用按
        // 逻辑尺寸显示（等价系统截图行为）；像素数据不变，仅元数据。
        guard let final = ctx.makeImage() else { return nil }
        let rep = NSBitmapImageRep(cgImage: final)
        rep.size = NSSize(width: CGFloat(width), height: CGFloat(height))
        guard let pngData = rep.representation(using: .png, properties: [:]), !pngData.isEmpty else {
            return nil
        }
        return ScreenshotPngOutput(
            pngData: pngData,
            base64: "data:image/png;base64," + pngData.base64EncodedString())
    }

    // MARK: 保存流（工具栏「保存」；对齐 overlay_input_windows.cpp TB_Save 分支）

    /// 保存选区为 PNG 文件（保存对话框主流程；对齐 Windows TB_Save 分支 L337-359）：
    /// 1) 弹出前临时下调覆盖层/工具栏浮层族窗口层级 + event tap 放行模态期间按键
    ///   （对齐 Windows 弹出前摘除 TOPMOST：PromptSaveFilePath 注释「覆盖层是 WS_EX_TOPMOST
    ///   全屏窗口，通用对话框可能被遮挡」，L608-609/L632-636）
    /// 2) NSSavePanel 模态（runModal 在主线程=会话泵线程执行；模态期间会话泵暂停属预期，
    ///   对齐 GetSaveFileNameW 模态循环；面板自成模态，键盘/鼠标不会误触覆盖层）
    /// 3) 取消对话框：层级恢复（defer）、回到编辑态继续会话、无回调（对齐 Windows
    ///   L358「用户取消保存对话框：不关闭，留在编辑态」）
    /// 4) 选择路径：统一输出 → 原子落盘 → 回调（成功 success:true 含坐标与 base64 /
    ///   失败 success:false）→ 会话结束（对齐 Windows L343「无论保存成功与否，均关闭
    ///   截图窗口」——state = CS_Done 后 DestroyWindow）。
    ///   不写剪贴板（对齐 Windows 保存路径语义：SaveRegionToPngFile 落盘 + 回调即收口，
    ///   不经 SaveBitmapToClipboard/ExtractRegionResult 的剪贴板写入）。
    func saveSelectionToFile() {
        guard isRunning, state == .confirmed else { return }
        let sel = selection.standardized
        guard sel.width >= 1, sel.height >= 1 else { return }   // 防御：确认态选区恒有效

        // 弹出前临时降层级（Windows 摘除 TOPMOST 等价）+ event tap 放行（模态期间
        // ESC/右键交给保存面板自消费，不触发会话取消）
        duckOverlayLevelsForSaveModal(true)
        saveModalFlag.set()
        defer {
            // 对话框关闭后恢复层级（对齐 PromptSaveFilePath 关闭后恢复 TOPMOST）；
            // 会话已结束（保存成功/失败路径）时窗口已销毁，恢复为 no-op 安全
            saveModalFlag.reset()
            duckOverlayLevelsForSaveModal(false)
        }

        // 弹出保存对话框；取消 → 回编辑态（无回调、会话继续，编辑态完好）
        guard let path = scPromptSaveFilePath() else { return }

        // 统一输出（物理裁剪 → 逻辑尺寸 → 标注 → 圆角蒙版 → 单次编码；与确认输出同管线）
        guard let cropped = cropSelectionPhysical(sel),
              let output = buildFinalPngOutput(cropped: cropped, sel: sel,
                                               cornerRadius: selectionCornerRadius) else {
            // 用户已选择保存路径：编码失败同样结束会话并回调失败（对齐 Windows
            // L352-354「无论保存成功与否均关闭截图窗口」+ EmitScreenshotResult(false)）
            state = .done
            finish(failurePayload("failed to encode screenshot"))
            return
        }

        // 原子落盘（对齐 EncodeHBitmapPng 的「同目录临时文件 + MOVEFILE_REPLACE_EXISTING
        // 原子替换」语义：macOS 用 writeOptions .atomic——先写临时文件全部成功后再原子
        // 替换目标，磁盘满/权限中断不会留下截断 PNG 或覆盖旧图）
        do {
            try output.pngData.write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            state = .done
            finish(failurePayload("failed to save screenshot"))
            return
        }

        // 保存成功：回调（success:true 含坐标与 base64，走既有 TSFN 出口）后结束会话；
        // 不写剪贴板（对齐 Windows 保存路径语义）
        state = .done
        finish(scSuccessPayloadJSON(sel: sel, base64: output.base64))
    }
}
