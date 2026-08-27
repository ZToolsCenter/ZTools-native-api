// 截图模块：SVG 图标光栅化与缓存（nanosvg 唯一实例化编译单元）
#include "internal.h"

// ---- nanosvg：SVG 光栅化（单文件库，宏实例化）----
// 两个 .h 必须在同一编译单元用宏实例化一次；本文件（icons_windows.cpp）为唯一实例化编译单元。
#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvgrast.h"

// ---- 截图工具栏图标 SVG 文本（构建期由 scripts/gen-icons.js 从 src/assets 生成）----
#include "../generated/icon_svgs.h"

// ---- SVG 图标光栅化 + 缓存 ----

// 将单个 SVG 文本光栅化为 32bpp 预乘 ARGB HBITMAP，尺寸 px×px。
// 把 SVG 中的 currentColor 替换为目标 color（normal/active 两色复用同一文本）。

HBITMAP RenderSvgToBitmap(const char* svgText, COLORREF color, int px) {
    if (!svgText || px <= 0) return NULL;

    // 1. 替换 currentColor -> #RRGGBB（nanosvg 解析会改写 buffer，需可写副本）
    char colorHex[8];
    sprintf_s(colorHex, sizeof(colorHex), "#%02X%02X%02X",
              color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
    std::string svg(svgText);
    const std::string token = "currentColor";
    size_t pos = 0;
    while ((pos = svg.find(token, pos)) != std::string::npos) {
        svg.replace(pos, token.size(), colorHex);
        pos += 6;
    }

    // 2. 解析（nsvgParse 会就地修改传入字符串）
    NSVGimage* image = nsvgParse(&svg[0], "px", 96.0f);
    if (!image) return NULL;

    // 3. 光栅化到 RGBA 缓冲（非预乘）
    std::vector<unsigned char> rgba(px * px * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return NULL; }
    // 内容缩放到 72% 并居中，四周各留 14% 内边距，避免图标顶满按钮。
    // nanosvg 解析后 shape 已在 image->width 坐标系（viewBox 已折算），
    // 故 scale = px * 0.72 / image->width，偏移 tx = ty = px * 0.14。
    const float contentScale = 0.72f;
    const float pad = (1.0f - contentScale) * 0.5f;
    float refSize = (image->width > 0) ? image->width
                  : (image->height > 0) ? image->height : 24.0f;
    float scale = (float)px * contentScale / refSize;
    float tx = px * pad;
    float ty = px * pad;
    nsvgRasterize(rast, image, tx, ty, scale, rgba.data(), px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // 4. RGBA -> 预乘 BGRA（用于 AlphaBlend，避免黑边）
    //    nanosvg 输出 RGBA 字节序 [R,G,B,A]；而 32bpp BI_RGB DIB 内存布局为
    //    BGRA（像素值 0xAARRGGBB 在小端内存里是 B,G,R,A）。故预乘时需把 R、B
    //    对调写入，否则 memcpy 后通道会反，蓝色被画成黄色（灰度色看不出来）。
    for (int i = 0; i < px * px; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        // 预乘后按 DIB 的 BGRA 字节序写入
        rgba[i * 4 + 0] = (unsigned char)((b * a + 127) / 255);  // B
        rgba[i * 4 + 1] = (unsigned char)((g * a + 127) / 255);  // G
        rgba[i * 4 + 2] = (unsigned char)((r * a + 127) / 255);  // R
        rgba[i * 4 + 3] = a;                                      // A
    }

    // 5. 创建 32bpp ARGB HBITMAP
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return NULL;
    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) { ReleaseDC(NULL, screenDC); return NULL; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = px;
    bmi.bmiHeader.biHeight = -px;  // 自上而下，避免垂直翻转
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        memcpy(bits, rgba.data(), px * px * 4);
    }
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    return bmp;
}

// 计算工具栏宽度（拖拽把手格 + 所有按钮 + 间距 + 左右内边距 + 边框），按 metrics 缩放。
// 最左为「6 点拖拽把手」单元格（与按钮同宽），各工具按钮自第 1 格起排布，
// 与 HitTestToolbar / DrawToolbar 的单元格映射严格一致。

int CalcToolbarWidth(const SCToolbarMetrics& m) {
    return (TB_Count + 1) * (m.btn + m.gap) - m.gap + m.pad * 2 + m.border * 2;
}

// SCIconCache::Init/Cleanup 实体（声明在 internal.h；仅本文件可见 nanosvg 光栅化依赖）

void SCIconCache::Init(int physicalIconSize) {
    if (inited) Cleanup();
    iconSize = physicalIconSize;
    COLORREF darkColor = RGB(60, 60, 60);
    // 选中态图标用主题蓝 #3B8BF2（SC_THEME_TOOLBAR_BLUE），与浅蓝高亮底搭配
    COLORREF activeColor = SC_THEME_TOOLBAR_BLUE;
    for (int i = 0; i < TB_Count; i++) {
        // 分隔线（TB_Separator1/2）及未映射的项：kIconSvgs[i] 为 nullptr，跳过
        if (kIconSvgs[i]) {
            dark[i] = RenderSvgToBitmap(kIconSvgs[i], darkColor, iconSize);
            active[i] = RenderSvgToBitmap(kIconSvgs[i], activeColor, iconSize);
        }
    }
    inited = true;
}

void SCIconCache::Cleanup() {
    for (int i = 0; i < TB_Count; i++) {
        if (dark[i]) { DeleteObject(dark[i]); dark[i] = NULL; }
        if (active[i]) { DeleteObject(active[i]); active[i] = NULL; }
    }
    inited = false;
}

// CR-018: 防常量漂移。kIconSvgs（generated/icon_svgs.h，按 ToolButton 枚举顺序）
// 元素数必须等于 TB_Count：Init/Cleanup 均以 TB_Count 为循环上界索引 kIconSvgs[i]，
// 漂移将越界读 nullptr 或漏绘图标。
static_assert(sizeof(kIconSvgs) / sizeof(kIconSvgs[0]) == TB_Count,
    "kIconSvgs element count must equal TB_Count (ToolButton enum)");
