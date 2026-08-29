// 截图模块共享内部头（Windows 侧唯一总入口）：系统包含、拆分类型头伞包含、
// 长截图 Windows 会话机制（WM_LONGCAPTURE_RUN）、可变全局变量与跨文件函数声明。
// 由 screenshot_windows.cpp 拆分而来；2026-08 二次拆分：类型定义下沉至本目录
// sc_types.h / sc_annotations.h / sc_theme.h / capture_context.h，长截图跨平台纯数据
// 类型迁至 ../algo/long_capture_internal.h（双平台唯一权威）。既有 .cpp 只包含本头的
// 用法保持不变。
#pragma once

#include <napi.h>
#include <node_api.h>
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
// 冷门头 <imm.h>/<commdlg.h>/<shlobj.h> 不在此集中包含（本头类型声明均不依赖其符号），
// 仅由唯一使用方 .cpp 自行引入：imm.h → overlay_input_windows.cpp（IME 输入法）、
// commdlg.h → output_windows.cpp（GetSaveFileNameW 保存对话框）、
// shlobj.h → output_windows.cpp（SHGetKnownFolderPath 已知文件夹路径）。
#include <thread>
#include <atomic>
#include <algorithm>   // For std::min, std::max
#include <vector>
#include <deque>
#include <string>
#include <cmath>      // For std::sqrt, std::fabs
#include <mutex>
#include <chrono>
#include <cstdint>    // For INT64_MIN / INT64_MAX（长截图裁剪内容坐标哨兵）
#include <utility>     // For std::move（最近帧历史的匹配数据转移）

// DWMWA_CLOAKED 在较新的 Windows SDK 中定义，为了兼容性手动定义
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// GDI+ 需要 min/max
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>


#include "screenshot_windows.h"

// ---- 拆分类型头（各自自包含；此处伞包含使既有 .cpp 的单头用法不变）----
#include "sc_types.h"               // 基础类型与常量（状态枚举 / 几何 / metrics / GDI 资源 / 结果结构）
#include "sc_annotations.h"         // 标注类型与绘制参数预设
#include "sc_theme.h"               // 主题色常量
#include "capture_context.h"        // CaptureContext 会话上下文
#include "long_capture_internal.h"  // 长截图跨平台类型与子系统声明（../algo/，经构建 include_dirs 检索）

// ==================== 长截图（手动滚动捕获） ====================

// 进入方式：在编辑态（CS_Confirmed）点工具栏「长截图」按钮 → BeginLongCapture
// 隐式切换到 CS_LongCapturing，通过 PostMessage(WM_LONGCAPTURE_RUN) 在覆盖层
// 窗口过程中执行 RunLongCapture。
//
// 快照选区 → 隐藏全屏覆盖层 → 创建侧边预览面板（缩略小地图 + 完成/取消）→
// 滚轮停稳后采样一帧并增量拼接到小地图；向下滚追加到底部，向上滚前插到头部。
//
// 交互形态为手动模式：用户自己在选区上滚动鼠标滚轮（Raw Input 被动观察，不拦截输入），
//
// 独立于 CaptureContext 编辑态状态机的轻量上下文。复用截图会话的线程与
// threadsafe 回调（g_screenshotTsfn），但滚动/拼接逻辑全部在此处实现，
// 不触碰 CaptureContext 的标注/工具栏机制。

#define WM_LONGCAPTURE_RUN (WM_APP + 200)

// ---- 可变全局变量（定义见各归属 .cpp）----
// 访问规则（此前线程归属散落在各字段注释，现集中声明）：
//   线程模型——本模块仅两条线程访问这些全局：JS 线程（NAPI 导出 start/abort 等）与
//   截图捕获线程（ScreenshotCaptureThread，串行创建所有覆盖层窗口并派发其消息，
//   各窗口 wndproc 与绘制函数均在该线程内执行，互斥天然串行）。

// 覆盖层主窗口句柄：捕获线程独占。start() 时在捕获线程创建，消息循环内读写，
// WM_DESTROY 末尾由捕获线程置 NULL（wndproc_windows.cpp）。JS 线程不直接访问。
extern HWND g_screenshotOverlayWindow;

// 截图进行中标志（atomic）：跨线程。捕获线程 start 入口置 true、各退出路径置 false；
// JS 线程 start() 入口读它拒绝重入（已进行中则直接返回）。无锁，依赖 atomic 可见性。
extern std::atomic<bool> g_isCapturing;

// 截图结果回传 JS 的 threadsafe function：跨线程但写时序确定。JS 线程在
// StartRegionCaptureWithPrimedFrame 创建并初始化引用计数；此后由捕获线程独占
// 调用/释放（EmitScreenshotResult 唯一调用点 + ReleaseScreenshotTsfn 唯一释放点，
// 均在捕获线程内），JS 线程不再触碰。收口后访问点唯一。
extern napi_threadsafe_function g_screenshotTsfn;

// 截图捕获线程对象：JS 线程独占。start() 创建后立即 detach，此后不再访问
// （依赖 g_isCapturing 而非 join 判定会话状态，故无需 join；线程自然退出）。
extern std::thread g_screenshotThread;

// 自动确认模式（atomic）：跨线程。JS 线程 start() 写入；捕获线程在会话开始时
// 一次性 load 进 CaptureContext.autoConfirm，会话期内不再读此全局。
extern std::atomic<bool> g_autoConfirm;

// 预截首帧（及保护其读写的互斥锁）：跨线程，受 g_primedScreenshotFrameMutex 保护。
// JS 线程（PrimeScreenshotFrameNow/PrimeScreenshotFrame）与捕获线程（会话开始消费）
// 均在锁内读写 bitmap 与 valid 等字段；bitmap 句柄所有权在锁内转移（写方创建、
// 读方消费后置 invalid）。锁外不得解引用 bitmap。
extern PrimedScreenshotFrame g_primedScreenshotFrame;
extern std::mutex g_primedScreenshotFrameMutex;

// 截图上下文指针：捕获线程独占。会话开始置 &ctx（栈上局部变量）、结束置 nullptr；
// 窗口过程/绘制函数读它取 GDI+ 会话级资源（仅捕获线程内执行）。会话期外恒为 nullptr，
// 各读点均带 nullptr 守卫。JS 线程不访问。
extern CaptureContext* g_captureCtx;

// 长截图上下文指针（atomic）：跨线程，受 g_longCtxMutex 保护指针生命周期。
extern std::atomic<LongCaptureContext*> g_longCtx;
// 保护 g_longCtx 指针的生命周期：JS 线程 LongCaptureAbort 的 load/abortFlag 写，
// 与捕获线程 WM_LONGCAPTURE_RUN 清理段的 store(nullptr)/delete 互斥（定义见
// session_windows.cpp）。锁内只做指针读写与原子标志写，禁止 SendMessage 等
// 可能死锁的调用；abortFlag 本身保持 atomic，由锁保证写入时对象仍存活。
extern std::mutex g_longCtxMutex;

// 长截图会话窗口句柄（控制面板/工具栏/蒙版）：捕获线程独占。BeginLongCapture
// 时由捕获线程创建，wndproc 清理段（捕获线程）销毁并置 NULL。JS 线程不访问。
extern HWND g_longControlWindow;
extern HWND g_longToolbarWindow;
extern HWND g_longMaskWindow;

// 长截图参数：跨线程，但时序天然串行——JS 线程 start() 写入（先按默认重置再按 JS
// 覆盖，消除跨会话粘滞），捕获线程 BeginLongCapture 一次性读入 LongCaptureContext。
// start() 写入发生在 g_screenshotThread 创建之前，捕获线程读在创建之后，happens-before
// 由线程创建建立，无需额外同步。
extern int g_lcInterval;

// ---- 跨文件函数声明 ----
bool IsCornerRadiusHandle(int h);
bool IsVectorTool(int btn);
bool IsDragTool(int btn);
bool CanShowStylePopupTool(int btn);
AnnotationType ToolToAnnotationType(int btn);
int AnnotationTypeToTool(AnnotationType t);
SCPanelMetrics CalcPanelMetrics(double dpiScale);
SCToolbarMetrics CalcToolbarMetrics(double dpiScale);
SCHandleMetrics CalcHandleMetrics(double dpiScale);
HBITMAP RenderSvgToBitmap(const char* svgText, COLORREF color, int px);
int CalcToolbarWidth(const SCToolbarMetrics& m);
SCPopupMetrics CalcPopupMetrics(double dpiScale);
// 多屏异分辨率布局基准：取包含参考矩形（绝对坐标）的显示器物理边界。
// 工具栏/子菜单的上下翻转判定必须用所在显示器自身的边界——整个虚拟屏幕包围盒
// 会被高分屏拉大，低分屏上选区已触本屏底边仍会被误判为"下方放得下"而不翻转。
bool GetMonitorBoundsForRect(const RECT& refAbs, RECT& out);
void CalcPopupPlacement(const RECT& toolbarRect, int virtualX, int virtualY,
                        int virtualW, int virtualH, const SCPopupMetrics& m, int pw, int ph, RECT& out);
void CalcPopupPosition(const RECT& toolbarRect, int virtualX, int virtualY,
                       int virtualW, int virtualH, const SCPopupMetrics& m, RECT& out);
void CalcMosaicPopupSize(const SCPopupMetrics& m, int& outW, int& outH);
int HitTestMosaicPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m);
void DrawMosaicPopup(HDC hdc, const RECT& popupRect, int modeIdx, int sizeIdx, int radiusIdx, const SCPopupMetrics& m);
int HitTestPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m);
void DrawPopup(HDC hdc, const RECT& popupRect, int colorIdx, int firstIdx, bool isTextTool, const SCPopupMetrics& m);
void PushAnnotationHistory(CaptureContext* ctx);
bool UndoAnnotations(CaptureContext* ctx);
bool RedoAnnotations(CaptureContext* ctx);
bool InitGdipResources(CaptureContext* ctx);
Gdiplus::Font* GetGdipFont(CaptureContext* ctx, int fontPx);
double GetDpiScaleFactor();
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
bool CreateBackBuffer(HDC& outDC, HBITMAP& outBmp, int w, int h);
bool PrimeScreenshotFrameNow();
bool AcquireScreenshotBase(HDC& outMemDC, HBITMAP& outBitmap, int& vx, int& vy, int& vw, int& vh, double& dpiScale);
COLORREF GetPixelColorFromBitmap(HDC memDC, int x, int y, int vx, int vy, double dpiScale);
void ColorrefToStrings(COLORREF color, char* hexBuf, char* rgbBuf);
std::vector<SCWindowInfo> EnumWindowsForCapture();
int FindWindowAtPoint(const std::vector<SCWindowInfo>& windows, int x, int y);
void CalcPanelPosition(int mx, int my, int vx, int vy, int vw, int vh, const SCPanelMetrics& m, int& px, int& py);
void CalcResizePanelPosition(int handle, const RECT& sel, int vx, int vy, int vw, int vh, const SCPanelMetrics& m, int& px, int& py);
void RestoreDirtyRegion(HDC backDC, HDC memDC, const RECT& dirty, double dpiScale);
RECT InflateRectBy(const RECT& r, int margin);
bool IsValidRect(const RECT& r);
RECT UnionRectSafe(const RECT& a, const RECT& b);
void DrawInfoPanel(HDC hdc, int panelX, int panelY, COLORREF color, HDC memDC, int vx, int vy, int mx, int my, double dpiScale, const SCGdiResources& gdi, const SCPanelMetrics& m, int virtualW, int virtualH);
RECT DrawSizeLabel(HDC hdc, int width, int height, int refLeft, int refTop, int refRight, int refBottom, int virtualW, int virtualH, const SCGdiResources& gdi, const SCPanelMetrics& m);
RECT DrawSelection(HDC hdc, int x1, int y1, int x2, int y2, int vx, int vy, int vw, int vh, const SCGdiResources& gdi, const SCPanelMetrics& m);
void DrawWindowHighlight(HDC hdc, const RECT& rect, int vx, int vy, const SCGdiResources& gdi);
void DrawDimMask(HDC backDC, const SCGdiResources& gdi, int selLeft, int selTop, int selRight, int selBottom, int virtualW, int virtualH, int radius);
RECT NormalizeRect(const RECT& r);
bool PointInRect(int x, int y, const RECT& r);
int HitTestCornerRadiusHandle(int x, int y, const RECT& sel, int handleSize, int inset, int radius);
int FindNearestCornerRadiusHandle(int x, int y, const RECT& sel, int handleSize, int inset, int radius, int proximityMargin);
RECT CornerHandleDirtyRect(const CaptureContext* ctx, int corner);
int HitTestHandle(int x, int y, const RECT& sel, int handleSize);
LPCWSTR HandleCursor(int handle);
void CalcToolbarPosition(const RECT& selRel, int virtualX, int virtualY,
                         int virtualW, int virtualH, const SCToolbarMetrics& m, RECT& out);
int HitTestToolbar(int x, int y, const RECT& toolbarRect, const SCToolbarMetrics& m);
void DrawResizeHandles(HDC hdc, const RECT& selRel, int handleSize);
void DrawCornerRadiusHandle(HDC hdc, const RECT& selRel, int handleSize, int inset, int radius, int corner);
void DrawConfirmedBorder(HDC hdc, const RECT& selRel, const SCGdiResources& gdi, int radius);
void AddRoundedRect(Gdiplus::GraphicsPath& outPath, int x, int y, int w, int h, int radius);
void DrawToolbar(HDC hdc, const RECT& toolbarRect, int hoverBtn, int activeTool, const SCGdiResources& gdi, const SCToolbarMetrics& m, const SCIconCache& icons);
// 工具栏 title 式 tooltip：Tick 由会话空闲循环轮询（维护停顿/显示状态并失效气泡矩形），
// Draw 在 OnPaint 工具栏之后调用（气泡画进 backDC，盖在工具栏/子菜单之上）。
void TickToolbarTooltip(CaptureContext* ctx, HWND overlayWnd);
void DrawToolbarTooltip(HDC hdc, CaptureContext* ctx);
bool MosaicBlitRect(HDC targetDC, HDC srcDC, int dstX0, int dstY0, int dstW, int dstH, int srcAbsX0, int srcAbsY0, int blockPx, int virtualX, int virtualY, double dpiScale);
void FreeMosaicBase(CaptureContext* ctx);
void InitMosaicBrushCursors(CaptureContext* ctx);
void FreeMosaicBrushCursors(CaptureContext* ctx);
bool RebuildMosaicBase(CaptureContext* ctx);
bool MosaicBaseNeedsRebuild(const CaptureContext* ctx);
bool HasMosaicToRender(const std::vector<Annotation>& annotations, const Annotation* curDrawing);
void RevealMosaicToTarget(HDC targetDC, HDC mosaicBase, const std::vector<Annotation>& annotations, const Annotation* curDrawing, const RECT& contentBounds, float ox, float oy);
void DrawAnnotations(HDC hdc, const RECT& selRel, int virtualX, int virtualY, const std::vector<Annotation>& annotations, const Annotation* curDrawing);
void CompositeAnnotations(HDC finalDC, HDC srcDC, const std::vector<Annotation>& annotations, const RECT& rect, int virtualX, int virtualY, double dpiScale, int mosaicBlockPx);
std::string BitmapToBase64Png(HBITMAP hBitmap);
bool SaveBitmapToClipboard(HBITMAP hBitmap);
// 将 HBITMAP 编码为 PNG：可选产出 base64 / 原始字节，可选直接写入文件（单次编码）。
bool EncodeHBitmapPng(HBITMAP hBitmap, std::string* base64Out, std::string* rawOut,
                      const wchar_t* filePath);
// 将 HALFTONE 缩放模式设置到目标 DC（output_windows.cpp / long_capture_windows.cpp 共用）。
// HALFTONE 在做下采样缩放时比默认 COLORONCOLOR 质量更好，但 BrushOrg 会被 StretchBlt 用到，
// 因此同时把画刷原点复位到 (0,0) 避免抖动（MSDN 推荐配套调用）。
void SetHalftoneStretchMode(HDC dc);

ScreenshotResult* ExtractRegionResult(HDC memDC, const RECT& rect, int vx, int vy,
    double dpiScale, const std::vector<Annotation>& anns, int radius, int mosaicSizeIdx);
// 统一的 ScreenshotResult 发射口：字段参数化构造结果并经截图会话 TSFN 回传 JS。
// 内部统一处理守卫：TSFN 未就绪或 napi_tsfn_nonblocking 因队列满返回非 napi_ok 时，
// 自行 delete 分配的 result 防泄漏（CallScreenshotJs 只在成功入队时才 delete）。
// 仅允许在截图线程内调用。各发射点（确认/取消/保存/ESC/长截图完成等）统一走此函数。
// success=false 时坐标/尺寸/base64 全置 0/空（与取消路径语义一致）。
void EmitScreenshotResult(bool success, int x = 0, int y = 0, int x2 = 0, int y2 = 0,
                          int width = 0, int height = 0, const std::string& base64 = "");
// 会话初始化失败快速回传：构造 {success:false} 结果并经 EmitScreenshotResult 回传 JS，
// 唤醒 await 方避免永久挂起（早退路径统一收口点）。
void FailFast();
void BeginLongCapture(CaptureContext* ctx, HWND overlayHwnd);
bool RunLongCapture(LongCaptureContext* c);
void LongCaptureAbort();
std::wstring PromptSaveFilePath(HWND hwndOwner);
bool SaveRegionToPngFile(HDC memDC, const RECT& rect, int vx, int vy, double dpiScale,
    const std::vector<Annotation>& anns, const std::wstring& filePath, int radius, int mosaicSizeIdx);
void ClampCornerRadius(CaptureContext* ctx);
bool CalcAnnotationsBounds(std::vector<Annotation>& anns, RECT& out, HDC hdc);
RECT MeasureTextAnnotation(HDC hdc, Annotation& a);
int HitTestTextAnnotations(std::vector<Annotation>& anns, int x, int y, HDC hdc);
RECT MeasureAnnotationBounds(Annotation& a, HDC hdc);
int HitTestAnnotation(std::vector<Annotation>& anns, int x, int y, HDC hdc);
int HitTestAnnotationResizeHandle(const Annotation& a, int x, int y, HDC hdc, int handleSize);
void TransformAnnotationByBox(Annotation& a, const RECT& oldBox, const RECT& newBox);
void MeasureTextGdip(HDC hdc, const std::wstring& text, int fontPx, float& outOffsetX, float& outOffsetY, float& outW, float& outH);
int CalcCaretPosFromMouse(HDC hdc, const std::wstring& text, int fontPx, int textX, int mouseX);
void InvalidateAnnotationOp(HWND hwnd, CaptureContext* ctx, const RECT& curBox);
RECT CalcSelectionDirty(CaptureContext* ctx, bool includeToolbar);
void InvalidateTextLine(HWND hwnd, CaptureContext* ctx);

// ---- 选区状态机辅助（实现在 wndproc_windows.cpp，消息处理共用）----
void EnterConfirmed(CaptureContext* ctx, const RECT& sel);
RECT ResizeSelectionFromHandle(const RECT& startSelection, int handle, int dx, int dy,
                               const RECT& virtualBounds,
                               bool hasContent, const RECT& contentBounds,
                               bool enforceMinSize);
void GetResizeHandleAnchor(int handle, const RECT& sel, int& ax, int& ay);
void ApplyResizeSelection(HWND hwnd, CaptureContext* ctx);
bool HandleSelectionNudgeKey(HWND hwnd, CaptureContext* ctx, WPARAM vk);

// ---- 覆盖层消息处理（实现在 overlay_paint/overlay_input，由 WndProc 分发）----
LRESULT OnPaint(HWND hwnd, CaptureContext* ctx);
LRESULT OnLButtonDown(HWND hwnd, CaptureContext* ctx);
LRESULT OnMouseMove(HWND hwnd, CaptureContext* ctx);
LRESULT OnLButtonUp(HWND hwnd, CaptureContext* ctx);
LRESULT OnKeyDown(HWND hwnd, WPARAM wParam, CaptureContext* ctx);
LRESULT OnImeComposition(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx);
LRESULT OnChar(HWND hwnd, WPARAM wParam, CaptureContext* ctx);
LRESULT OnSetCursor(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx);

LRESULT CALLBACK ScreenshotOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ScreenshotCaptureThread();
