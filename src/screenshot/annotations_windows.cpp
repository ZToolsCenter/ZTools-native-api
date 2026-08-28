// 截图模块：标注模型（渲染、几何/命中/变换、撤销重做、文本测量）
#include "internal.h"

// 判断某工具按钮是否为可绘制矢量工具

bool IsVectorTool(int btn) {
    return btn == TB_Rect || btn == TB_Circle || btn == TB_Arrow || btn == TB_Brush;
}

bool IsDragTool(int btn) {
    return btn == TB_Drag;
}

bool CanShowStylePopupTool(int btn) {
    return IsVectorTool(btn) || btn == TB_Text;
}

// ToolButton -> AnnotationType

AnnotationType ToolToAnnotationType(int btn) {
    switch (btn) {
        case TB_Rect:   return AT_Rect;
        case TB_Circle: return AT_Circle;
        case TB_Arrow:  return AT_Arrow;
        case TB_Brush:  return AT_Brush;
        default:        return AT_Rect;
    }
}

// AnnotationType -> ToolButton（ToolToAnnotationType 的逆映射）。
// 用途：选中已有标注时，工具栏回显该标注对应的工具按钮（高亮 + 打开粗细/颜色子菜单）。
// AT_Mosaic 无对应工具按钮（马赛克标注不可选中），返回 -1。

int AnnotationTypeToTool(AnnotationType t) {
    switch (t) {
        case AT_Rect:   return TB_Rect;
        case AT_Circle: return TB_Circle;
        case AT_Arrow:  return TB_Arrow;
        case AT_Brush:  return TB_Brush;
        case AT_Text:   return TB_Text;
        default:        return -1;  // AT_Mosaic 等无对应工具按钮
    }
}

void PushAnnotationHistory(CaptureContext* ctx) {
    ctx->undoStack.push_back(ctx->annotations);
    // 限深：仅保留最近 SC_UNDO_MAX_DEPTH 份整份快照，防止长会话下随操作数平方级累积内存；
    // 超限时裁掉最老历史（pop_front），最近 50 步撤销不受影响。
    if ((int)ctx->undoStack.size() > SC_UNDO_MAX_DEPTH) {
        ctx->undoStack.pop_front();
    }
    ctx->redoStack.clear();
}

static void ResetAnnotationInteraction(CaptureContext* ctx) {
    ctx->selectedTextAnnotation = -1;
    ctx->hoveredTextAnnotation = -1;
    ctx->draggingTextAnnotation = -1;
    ctx->selectedAnnotation = -1;
    ctx->hoveredAnnotation = -1;
    ctx->draggingAnnotation = -1;
    ctx->resizingAnnotation = -1;
    ctx->annotationResizeHandle = RH_None;
    ctx->annotationOpHistoryPushed = false;
    ctx->hasLastAnnotationBox = false;
}

bool UndoAnnotations(CaptureContext* ctx) {
    if (ctx->undoStack.empty()) return false;
    ctx->redoStack.push_back(ctx->annotations);
    ctx->annotations = ctx->undoStack.back();
    ctx->undoStack.pop_back();
    ResetAnnotationInteraction(ctx);
    return true;
}

bool RedoAnnotations(CaptureContext* ctx) {
    if (ctx->redoStack.empty()) return false;
    ctx->undoStack.push_back(ctx->annotations);
    ctx->annotations = ctx->redoStack.back();
    ctx->redoStack.pop_back();
    ResetAnnotationInteraction(ctx);
    return true;
}

// ==================== 标注绘制（GDI+ 抗锯齿） ====================

// 绘制单条标注（不含 clip）。
// 标注坐标为绝对虚拟屏幕坐标；ox/oy 为绘制偏移，把绝对坐标换算到目标 DC 的局部坐标：
//   - 覆盖层（backDC）：ox/oy = -virtualX/-virtualY（backDC 原点 = 虚拟屏幕左上角）。
//   - 合成（finalDC）：ox/oy = -rect.left/-rect.top（finalDC 原点 = 选区左上角）。
static void DrawOneAnnotation(Gdiplus::Graphics& graphics, const Annotation& a,
                              float ox, float oy) {
    Gdiplus::Color c(GetRValue(a.color), GetGValue(a.color), GetBValue(a.color));
    float thick = (float)a.thickness;
    if (thick < 1.0f) thick = 1.0f;
    Gdiplus::Pen pen(c, thick);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush brush(c);

    switch (a.type) {
        case AT_Rect: {
            float x = a.x1 + ox;
            float y = a.y1 + oy;
            float w = (float)(a.x2 - a.x1);
            float h = (float)(a.y2 - a.y1);
            graphics.DrawRectangle(&pen, (std::min)(x, x + w), (std::min)(y, y + h),
                                   std::fabs(w), std::fabs(h));
            break;
        }
        case AT_Circle: {
            float x = a.x1 + ox;
            float y = a.y1 + oy;
            float w = (float)(a.x2 - a.x1);
            float h = (float)(a.y2 - a.y1);
            graphics.DrawEllipse(&pen, (std::min)(x, x + w), (std::min)(y, y + h),
                                 std::fabs(w), std::fabs(h));
            break;
        }
        case AT_Arrow: {
            float sx = a.x1 + ox;
            float sy = a.y1 + oy;
            float ex = a.x2 + ox;
            float ey = a.y2 + oy;
            float dx = ex - sx, dy = ey - sy;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) break;
            // 箭头几何：机翼状——底边内凹（向尖端方向凹入 notch），两翼后掠，
            // 从箭身末端张开，呈「细尾→锥杆→张开两翼→尖」的箭矢形态。
            float headLen = thick * 4.0f + 8.0f;
            float headHalfW = thick * 2.4f + 5.0f;
            float notch = headLen * 0.4f;        // 内凹深度：底边中点向尖端凹入
            if (headLen > len) headLen = len * 0.6f;
            float ux = dx / len, uy = dy / len;
            float nx = -uy, ny = ux;
            // 箭头底部中心（沿箭头方向后退 headLen）与内凹点
            float baseX = ex - ux * headLen;
            float baseY = ey - uy * headLen;
            float notchX = baseX + ux * notch;
            float notchY = baseY + uy * notch;
            // 箭身：起点细、终点粗的锥形。终点延伸至内凹点并略超出（overlap），
            // 由箭头覆盖重叠区，避免抗锯齿在拼接处留细缝；终点宽 < 两翼宽，
            // 使两翼从箭身末端明显张开、内凹缺口清晰可见。
            float startHalfW = (std::max)(thick * 0.5f, 0.75f);
            float endHalfW = headHalfW * 0.55f;
            float overlap = 1.5f;
            float bodyEndX = notchX + ux * overlap;
            float bodyEndY = notchY + uy * overlap;
            Gdiplus::PointF body[4] = {
                Gdiplus::PointF(sx + nx * startHalfW, sy + ny * startHalfW),
                Gdiplus::PointF(sx - nx * startHalfW, sy - ny * startHalfW),
                Gdiplus::PointF(bodyEndX - nx * endHalfW, bodyEndY - ny * endHalfW),
                Gdiplus::PointF(bodyEndX + nx * endHalfW, bodyEndY + ny * endHalfW),
            };
            graphics.FillPolygon(&brush, body, 4);
            // 机翼状箭头：尖 → 右翼 → 内凹点 → 左翼（底边内凹而非平直三角）
            Gdiplus::PointF head[4] = {
                Gdiplus::PointF(ex, ey),
                Gdiplus::PointF(baseX + nx * headHalfW, baseY + ny * headHalfW),
                Gdiplus::PointF(notchX, notchY),
                Gdiplus::PointF(baseX - nx * headHalfW, baseY - ny * headHalfW),
            };
            graphics.FillPolygon(&brush, head, 4);
            break;
        }
        case AT_Brush: {
            if (a.pts.size() < 2) break;
            std::vector<Gdiplus::PointF> pts;
            pts.reserve(a.pts.size());
            for (const POINT& p : a.pts) {
                pts.push_back(Gdiplus::PointF((float)(p.x) + ox, (float)(p.y) + oy));
            }
            graphics.DrawLines(&pen, pts.data(), (INT)pts.size());
            break;
        }
        case AT_Text: {
            if (a.text.empty()) break;
            // 字号 = thickness；用 GDI+ 字体绘制，顶部对齐到锚点 y1
            int fontPx = a.thickness;
            if (fontPx < 8) fontPx = 8;
            Gdiplus::FontFamily fontFamily(SC_FONT_FACE);
            Gdiplus::Font font(&fontFamily, (Gdiplus::REAL)fontPx, Gdiplus::FontStyleRegular,
                                Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(c);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentNear);
            sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
            // 用 MeasureString 得到合适布局矩形（避免被裁）
            Gdiplus::RectF layoutRect((Gdiplus::REAL)(a.x1 + ox), (Gdiplus::REAL)(a.y1 + oy),
                                      10000.0f, (Gdiplus::REAL)(fontPx * 2));
            graphics.DrawString(a.text.c_str(), -1, &font, layoutRect, &sf, &textBrush);
            break;
        }
    }
}

// 覆盖层渲染矢量/文字标注（不含马赛克，马赛克由 reveal-mask 单独处理）。
// selRel：选区在 backDC 局部坐标的矩形；标注为绝对虚拟屏幕坐标，偏移 = -virtualX/-virtualY。
// 用 SetClip 限制到选区内部，避免画到遮罩区。

void DrawAnnotations(HDC hdc, const RECT& selRel, int virtualX, int virtualY,
                            const std::vector<Annotation>& annotations,
                            const Annotation* curDrawing) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用（Graphics 按 hdc 新建）。
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 限制绘制范围在选区内
        Gdiplus::Rect clipRect(selRel.left, selRel.top,
                               selRel.right - selRel.left,
                               selRel.bottom - selRel.top);
        // 限制绘制范围在选区内（与选区矩形求交；局部帧时还会与 backDC 裁剪区 dirtyRect 求交，
        // 因 Graphics(backDC) 继承 GDI 裁剪区，CombineModeIntersect 保证标注不超出 dirtyRect∩选区）。
        graphics.SetClip(clipRect, Gdiplus::CombineModeIntersect);

        float ox = (float)-virtualX;
        float oy = (float)-virtualY;

        for (const Annotation& a : annotations) {
            if (a.type == AT_Mosaic) continue;  // 马赛克单独渲染
            DrawOneAnnotation(graphics, a, ox, oy);
        }
        if (curDrawing && curDrawing->type != AT_Mosaic) {
            DrawOneAnnotation(graphics, *curDrawing, ox, oy);
        }
    }
}

// 合成标注进最终 PNG：finalDC 原点 = 选区左上角，故偏移 = -rect.left/-rect.top。
// srcDC = memDC（原始屏幕位图，物理像素），用于马赛克像素化取源。
// mosaicBlockPx：马赛克块大小（与编辑器当前全局块大小保持一致，保证导出与所见一致）。

void CompositeAnnotations(HDC finalDC, HDC srcDC,
                                 const std::vector<Annotation>& annotations,
                                 const RECT& rect, int virtualX, int virtualY,
                                 double dpiScale, int mosaicBlockPx) {
    if (annotations.empty()) return;

    // 马赛克先渲染到底图上，后续矢量/文字标注保持清晰覆盖在其上方。
    if (HasMosaicToRender(annotations, nullptr)) {
        int blockPx = mosaicBlockPx;
        if (blockPx < 2) blockPx = 2;
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        HDC screenDC = GetDC(NULL);
        if (screenDC) {
            HDC baseDC = CreateCompatibleDC(screenDC);
            HBITMAP baseBmp = baseDC ? CreateCompatibleBitmap(screenDC, w, h) : NULL;
            if (baseDC && baseBmp) {
                HGDIOBJ oldBase = SelectObject(baseDC, baseBmp);
                bool generated = oldBase && oldBase != HGDI_ERROR
                    && MosaicBlitRect(baseDC, srcDC, 0, 0, w, h,
                                      rect.left, rect.top, blockPx,
                                      virtualX, virtualY, dpiScale);
                if (generated) {
                    // 揭示蒙版区域（finalDC 原点 = 选区左上角，base 同为选区相对，ox=-rect.left）
                    float ox = (float)-rect.left;
                    float oy = (float)-rect.top;
                    RevealMosaicToTarget(finalDC, baseDC, annotations, nullptr,
                                         RECT{0, 0, w, h}, ox, oy);
                }
                if (oldBase && oldBase != HGDI_ERROR) SelectObject(baseDC, oldBase);
            }
            if (baseBmp) DeleteObject(baseBmp);
            if (baseDC) DeleteDC(baseDC);
            ReleaseDC(NULL, screenDC);
        }
    }

    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
    {
        Gdiplus::Graphics graphics(finalDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float ox = (float)-rect.left;
        float oy = (float)-rect.top;
        for (const Annotation& a : annotations) {
            if (a.type == AT_Mosaic) continue;  // 马赛克单独渲染
            DrawOneAnnotation(graphics, a, ox, oy);
        }
    }
}

static RECT MeasureArrowAnnotationBounds(const Annotation& a) {
    float sx = (float)a.x1;
    float sy = (float)a.y1;
    float ex = (float)a.x2;
    float ey = (float)a.y2;
    float minX = (std::min)(sx, ex);
    float minY = (std::min)(sy, ey);
    float maxX = (std::max)(sx, ex);
    float maxY = (std::max)(sy, ey);

    float thick = (float)a.thickness;
    if (thick < 1.0f) thick = 1.0f;
    float dx = ex - sx;
    float dy = ey - sy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len >= 1.0f) {
        float headLen = thick * 4.0f + 8.0f;
        float headHalfW = thick * 2.4f + 5.0f;
        if (headLen > len) headLen = len * 0.6f;
        float notch = headLen * 0.4f;
        float ux = dx / len;
        float uy = dy / len;
        float nx = -uy;
        float ny = ux;
        float baseX = ex - ux * headLen;
        float baseY = ey - uy * headLen;
        float notchX = baseX + ux * notch;
        float notchY = baseY + uy * notch;
        float startHalfW = (std::max)(thick * 0.5f, 0.75f);
        float endHalfW = headHalfW * 0.55f;
        float overlap = 1.5f;
        float bodyEndX = notchX + ux * overlap;
        float bodyEndY = notchY + uy * overlap;

        auto expand = [&](float x, float y) {
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
        };
        expand(sx + nx * startHalfW, sy + ny * startHalfW);
        expand(sx - nx * startHalfW, sy - ny * startHalfW);
        expand(bodyEndX - nx * endHalfW, bodyEndY - ny * endHalfW);
        expand(bodyEndX + nx * endHalfW, bodyEndY + ny * endHalfW);
        expand(baseX + nx * headHalfW, baseY + ny * headHalfW);
        expand(notchX, notchY);
        expand(baseX - nx * headHalfW, baseY - ny * headHalfW);
    }

    const float margin = 2.0f;
    return { (int)floorf(minX - margin), (int)floorf(minY - margin),
             (int)ceilf(maxX + margin), (int)ceilf(maxY + margin) };
}

// 计算所有标注内容的包围盒（选区相对逻辑坐标）。
// 用于限制选区缩放：选区不可缩小到裁掉已添加内容。
// 返回 false 表示无标注（无约束）。
// 计算所有标注内容的包围盒（绝对虚拟屏幕坐标）。
// 用于限制选区缩放：选区不可缩小到裁掉已添加内容。
// 返回 false 表示无标注（无约束）。hdc 用于测量文字宽高。
// 非常量引用：AT_Text 分支会经 MeasureTextAnnotation 回填文字测量缓存。

bool CalcAnnotationsBounds(std::vector<Annotation>& anns, RECT& out, HDC hdc) {
    if (anns.empty()) return false;
    int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
    auto expand = [&](int x, int y) {
        if (x < minL) minL = x;
        if (y < minT) minT = y;
        if (x > maxR) maxR = x;
        if (y > maxB) maxB = y;
    };
    for (Annotation& a : anns) {
        if (a.type == AT_Brush) {
            for (const POINT& p : a.pts) expand(p.x, p.y);
        } else if (a.type == AT_Mosaic) {
            if (a.mosaicRect) {
                // 框选模式：矩形角点
                expand(a.x1, a.y1);
                expand(a.x2, a.y2);
            } else {
                // 涂抹模式：路径包围盒 + 半径
                int r = a.brushRadius;
                for (const POINT& p : a.pts) {
                    expand(p.x - r, p.y - r);
                    expand(p.x + r, p.y + r);
                }
            }
        } else if (a.type == AT_Text) {
            // 复用 MeasureTextAnnotation：与选中时的外边框（含 padding）完全一致，
            // 保证 resize 选区时文字包围盒约束 = 视觉选中边框，不会被裁掉。
            RECT r = MeasureTextAnnotation(hdc, a);
            expand(r.left, r.top);
            expand(r.right, r.bottom);
        } else if (a.type == AT_Arrow) {
            RECT r = MeasureArrowAnnotationBounds(a);
            expand(r.left, r.top);
            expand(r.right, r.bottom);
        } else {
            expand(a.x1, a.y1);
            expand(a.x2, a.y2);
        }
    }
    if (minL == INT_MAX) return false;
    out.left = minL;
    out.top = minT;
    out.right = maxR;
    out.bottom = maxB;
    return true;
}

// 测量文字标注的包围盒（绝对虚拟屏幕坐标）
// 用 GDI+ MeasureString 测量，与提交态 DrawString 渲染同源：
//   - 字形左上角相对锚点 (x1,y1) 有内部偏移 (offX, offY)
//   - 宽高为紧凑字形范围（不含 GDI GetTextExtentPoint32W 的尾部 overhang）
//   - 边框左右 padding 对称，完整包住文字
// 性能：测量结果只依赖 (text, fontPx)，与锚点无关，故缓存"相对锚点的偏移/尺寸"。
//   命中缓存（textCacheValid && textCacheFontPx == fontPx）时直接复用，跳过 GDI+ 测量；
//   锚点变化（TransformAnnotationByBox 平移）不影响缓存有效性。
//   缓存对象（FontFamily/StringFormat/Font）取自会话级 InitGdipResources（经 g_captureCtx）。

RECT MeasureTextAnnotation(HDC hdc, Annotation& a) {
    RECT rect = { a.x1, a.y1, a.x1, a.y1 };
    if (a.type != AT_Text || a.text.empty()) return rect;

    int fontPx = a.thickness;
    if (fontPx < 8) fontPx = 8;

    float offX = 0, offY = 0, w = 0, h = 0;
    // 缓存命中：直接复用相对锚点的偏移/尺寸（与 fontPx 校验）
    if (a.textCacheValid && a.textCacheFontPx == fontPx) {
        offX = a.textCacheOffX;
        offY = a.textCacheOffY;
        w = a.textCacheW;
        h = a.textCacheH;
    } else if (g_captureCtx && g_captureCtx->gdipInited
               && g_captureCtx->gdipFontFamily && g_captureCtx->gdipStrFmt) {
        // 缓存未命中：做一次 GDI+ 测量并回填缓存
        Gdiplus::Graphics graphics(hdc);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
        Gdiplus::StringFormat* sf = g_captureCtx->gdipStrFmt;
        Gdiplus::RectF origin(0, 0, 0, 0);
        Gdiplus::RectF bounds;
        graphics.MeasureString(a.text.c_str(), (INT)a.text.size(), font, origin, sf, &bounds);
        offX = bounds.X;
        offY = bounds.Y;
        w = bounds.Width;
        h = bounds.Height;
        a.textCacheValid = true;
        a.textCacheFontPx = fontPx;
        a.textCacheOffX = offX;
        a.textCacheOffY = offY;
        a.textCacheW = w;
        a.textCacheH = h;
    }

    const int padding = 4;
    // 字形左上角（绝对坐标）= 锚点 + GDI+ 内部偏移
    int glyphLeft = a.x1 + (int)floorf(offX);
    int glyphTop  = a.y1 + (int)floorf(offY);
    int glyphRight = a.x1 + (int)ceilf(offX + w);
    int glyphBottom = a.y1 + (int)ceilf(offY + h);
    rect.left   = glyphLeft - padding;
    rect.top    = glyphTop - padding;
    rect.right  = glyphRight + padding;
    rect.bottom = glyphBottom + padding;
    return rect;
}

// 命中测试文字标注，返回标注索引（-1 表示未命中）
// 非常量引用：MeasureTextAnnotation 会回填文字测量缓存。

int HitTestTextAnnotations(std::vector<Annotation>& anns, int x, int y, HDC hdc) {
    for (int i = (int)anns.size() - 1; i >= 0; i--) {
        if (anns[i].type == AT_Text) {
            RECT rect = MeasureTextAnnotation(hdc, anns[i]);
            if (PointInRect(x, y, rect)) {
                return i;
            }
        }
    }
    return -1;
}

// ==================== 通用标注几何（选中/拖拽/缩放） ====================
// 以下函数把「仅文字标注」具备的 hover/选中/拖拽/缩放能力推广到所有标注类型。
// 坐标系与 Annotation 一致：绝对虚拟屏幕坐标（与 ctx->mouseX/selection 同帧）。

// 点 (px,py) 到线段 (ax,ay)-(bx,by) 的最短距离（像素）。
// 用于画笔/箭头/马赛克涂抹等线条型标注的命中检测。

static double PointToSegmentDist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double lenSq = dx * dx + dy * dy;
    double t = 0;
    if (lenSq > 1e-9) {
        t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
        if (t < 0) t = 0;
        else if (t > 1) t = 1;
    }
    double cx = ax + t * dx;
    double cy = ay + t * dy;
    double ex = px - cx, ey = py - cy;
    return std::sqrt(ex * ex + ey * ey);
}

// 点 (px,py) 到折线 pts 的最短距离（像素）。

static double PointToPolylineDist(double px, double py, const std::vector<POINT>& pts) {
    if (pts.empty()) return 1e18;
    if (pts.size() == 1) {
        double ex = px - pts[0].x, ey = py - pts[0].y;
        return std::sqrt(ex * ex + ey * ey);
    }
    double best = 1e18;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        double d = PointToSegmentDist(px, py, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
        if (d < best) best = d;
    }
    return best;
}

// 计算单个标注的包围盒（绝对虚拟屏幕坐标）。
// 返回的矩形完整包住标注可见区域（含线宽/半径/文字 padding），用于选中框与四角 resize 手柄定位。
// hdc 仅在 AT_Text 时用于 GDI+ 测量文字宽高。
// 非常量引用：AT_Text 分支会经 MeasureTextAnnotation 回填文字测量缓存。

RECT MeasureAnnotationBounds(Annotation& a, HDC hdc) {
    RECT r = { 0, 0, 0, 0 };
    auto setBox = [&](int x1, int y1, int x2, int y2) {
        r.left = (std::min)(x1, x2);
        r.top = (std::min)(y1, y2);
        r.right = (std::max)(x1, x2);
        r.bottom = (std::max)(y1, y2);
    };
    switch (a.type) {
        case AT_Rect:
        case AT_Circle:
        case AT_Mosaic:
            if (a.type == AT_Mosaic && !a.mosaicRect) {
                // 涂抹模式：路径包围盒 + 半径
                if (a.pts.empty()) { r = { 0,0,0,0 }; return r; }
                int rad = a.brushRadius;
                int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
                for (const POINT& p : a.pts) {
                    if (p.x < minL) minL = p.x;
                    if (p.x > maxR) maxR = p.x;
                    if (p.y < minT) minT = p.y;
                    if (p.y > maxB) maxB = p.y;
                }
                r.left = minL - rad; r.top = minT - rad;
                r.right = maxR + rad; r.bottom = maxB + rad;
                return r;
            }
            // Rect/Circle/MosaicRect：以两个对角点为包围盒
            setBox(a.x1, a.y1, a.x2, a.y2);
            return r;
        case AT_Arrow:
            return MeasureArrowAnnotationBounds(a);
        case AT_Brush: {
            if (a.pts.empty()) { r = { 0,0,0,0 }; return r; }
            int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
            for (const POINT& p : a.pts) {
                if (p.x < minL) minL = p.x;
                if (p.x > maxR) maxR = p.x;
                if (p.y < minT) minT = p.y;
                if (p.y > maxB) maxB = p.y;
            }
            r.left = minL; r.top = minT; r.right = maxR; r.bottom = maxB;
            return r;
        }
        case AT_Text:
            return MeasureTextAnnotation(hdc, a);
    }
    return r;
}

// 命中测试任意标注，返回索引（-1 表示未命中）。
// 从顶层（数组末尾，绘制最上层）向底层遍历，命中第一个即返回（与视觉 z-order 一致）。
// 容差按线宽自适应：细线给 6px 余量，粗线给半个线宽，避免细线难以点中。
// 非常量引用：AT_Text 分支会回填文字测量缓存。
// AT_Mosaic 在循环顶部 continue 跳过（马赛克不可选中），故 switch 内无 AT_Mosaic case。

int HitTestAnnotation(std::vector<Annotation>& anns, int x, int y, HDC hdc) {
    for (int i = (int)anns.size() - 1; i >= 0; i--) {
        Annotation& a = anns[i];
        // 马赛克区域（框选/涂抹）不可选中、不可拖拽，直接跳过命中测试。
        if (a.type == AT_Mosaic) continue;
        double tol = (std::max)(6.0, a.thickness / 2.0 + 2.0);
        switch (a.type) {
            case AT_Rect: {
                // 仅命中矩形四条边轮廓（空心框），内部空白不选中。
                // 到任一边线段的最短距离 ≤ tol 即命中（容差向外扩散几像素辅助探测）。
                double d1 = PointToSegmentDist((double)x, (double)y, a.x1, a.y1, a.x2, a.y1);
                double d2 = PointToSegmentDist((double)x, (double)y, a.x2, a.y2, a.x1, a.y2);
                double d3 = PointToSegmentDist((double)x, (double)y, a.x1, a.y2, a.x1, a.y1);
                double d4 = PointToSegmentDist((double)x, (double)y, a.x2, a.y1, a.x2, a.y2);
                double dm = (std::min)((std::min)(d1, d2), (std::min)(d3, d4));
                if (dm <= tol) return i;
                break;
            }
            case AT_Circle: {
                // 椭圆（由包围盒定义）轮廓命中：用归一化径向距离近似。
                // r = hypot((x-cx)/a, (y-cy)/b)，r≈1 即落在椭圆上。
                double cx = (a.x1 + a.x2) * 0.5;
                double cy = (a.y1 + a.y2) * 0.5;
                double aax = std::fabs((double)a.x2 - a.x1) * 0.5;
                double aay = std::fabs((double)a.y2 - a.y1) * 0.5;
                if (aax < 0.5 && aay < 0.5) {
                    // 退化为点：直接点距
                    double ex = x - cx, ey = y - cy;
                    if (std::sqrt(ex * ex + ey * ey) <= tol) return i;
                } else if (aax < 0.5) {
                    // 退化为竖直线段
                    if (PointToSegmentDist((double)x, (double)y, cx, a.y1, cx, a.y2) <= tol) return i;
                } else if (aay < 0.5) {
                    // 退化为水平线段
                    if (PointToSegmentDist((double)x, (double)y, a.x1, cy, a.x2, cy) <= tol) return i;
                } else {
                    double r = std::sqrt(((x - cx) / aax) * ((x - cx) / aax)
                                         + ((y - cy) / aay) * ((y - cy) / aay));
                    // (r-1)*min(a,b) 把归一化距离换算回像素（用短半轴近似像素半径，保守且足够命中探测）
                    double minAxis = (std::min)(aax, aay);
                    if (std::fabs(r - 1.0) * minAxis <= tol) return i;
                }
                break;
            }
            case AT_Arrow: {
                if (PointToSegmentDist((double)x, (double)y, a.x1, a.y1, a.x2, a.y2) <= tol)
                    return i;
                break;
            }
            case AT_Brush: {
                if (PointToPolylineDist((double)x, (double)y, a.pts) <= tol) return i;
                break;
            }
            case AT_Text: {
                RECT box = MeasureTextAnnotation(hdc, a);
                if (PointInRect(x, y, box)) return i;
                break;
            }
        }
    }
    return -1;
}

// 命中测试标注包围盒的 8 个手柄（4 角 + 4 边中点），返回 ResizeHandle 或 RH_None。
// 容差沿用选区手柄的 handleSize，保证与选区手柄一致的可点击范围。

static int HitTestAnnotationHandle(int x, int y, const RECT& box, int handleSize) {
    int hs = handleSize;
    int cx = (box.left + box.right) / 2;
    int cy = (box.top + box.bottom) / 2;
    struct { int hx, hy; int handle; } tests[] = {
        { box.left,  box.top,    RH_TopLeft },
        { box.right, box.top,    RH_TopRight },
        { box.left,  box.bottom, RH_BottomLeft },
        { box.right, box.bottom, RH_BottomRight },
        { cx,        box.top,    RH_Top },
        { cx,        box.bottom, RH_Bottom },
        { box.left,  cy,         RH_Left },
        { box.right, cy,         RH_Right },
    };
    for (auto& t : tests) {
        RECT hitBox = { t.hx - hs, t.hy - hs, t.hx + hs, t.hy + hs };
        if (PointInRect(x, y, hitBox)) return t.handle;
    }
    return RH_None;
}

// 命中测试箭头的起点/终点端点手柄，返回 RH_ArrowStart / RH_ArrowEnd / RH_None。
// 箭头只允许拖拽两个端点（而非四角包围盒缩放），故单独命中 a.x1,y1 / a.x2,y2。
// 容差沿用 handleSize，与其它标注手柄可点击范围一致。

static int HitTestArrowEndpoints(int x, int y, const Annotation& a, int handleSize) {
    int hs = handleSize;
    struct { int hx, hy; int handle; } tests[] = {
        { a.x1, a.y1, RH_ArrowStart },
        { a.x2, a.y2, RH_ArrowEnd   },
    };
    for (auto& t : tests) {
        RECT hitBox = { t.hx - hs, t.hy - hs, t.hx + hs, t.hy + hs };
        if (PointInRect(x, y, hitBox)) return t.handle;
    }
    return RH_None;
}

// 按标注类型返回其缩放手柄命中（统一入口，供 LButtonDown/MouseMove/SETCURSOR 复用）：
//   箭头 = 起点/终点 2 端点；矩形/圆 = 包围盒 8 手柄（4 角 + 4 边中点）；
//   画笔/马赛克 = 无手柄（RH_None，不可缩放，画笔仅可整体拖动）。
// hdc 仅用于测量文字包围盒（此处文字不会进入，但签名与 HitTestAnnotation 对齐便于扩展）。

int HitTestAnnotationResizeHandle(const Annotation& a, int x, int y, HDC hdc, int handleSize) {
    switch (a.type) {
        case AT_Arrow:
            return HitTestArrowEndpoints(x, y, a, handleSize);
        case AT_Rect:
        case AT_Circle: {
            RECT box = MeasureAnnotationBounds(const_cast<Annotation&>(a), hdc);
            return HitTestAnnotationHandle(x, y, box, handleSize);
        }
        default:
            // AT_Brush / AT_Mosaic / AT_Text 无缩放手柄
            return RH_None;
    }
}

// 按包围盒变换（平移 + 等比/非等比缩放）映射标注所有坐标。
// oldBox -> newBox：标注内每个点 p 映射为 newBox.left + (p-oldBox.left)*sx 等。
// 用于四角 resize（画笔/涂抹路径按包围盒整体缩放，保持形状比例）。
// sx/sy 防 0：旧宽/高为 0 时退化为平移。

void TransformAnnotationByBox(Annotation& a, const RECT& oldBox, const RECT& newBox) {
    double oldW = oldBox.right - oldBox.left;
    double oldH = oldBox.bottom - oldBox.top;
    double newW = newBox.right - newBox.left;
    double newH = newBox.bottom - newBox.top;
    double sx = (oldW > 0.5) ? newW / oldW : 1.0;
    double sy = (oldH > 0.5) ? newH / oldH : 1.0;
    auto mapX = [&](int v) { return (int)(newBox.left + (v - oldBox.left) * sx + 0.5); };
    auto mapY = [&](int v) { return (int)(newBox.top + (v - oldBox.top) * sy + 0.5); };
    switch (a.type) {
        case AT_Rect:
        case AT_Circle:
        case AT_Arrow:
        case AT_Mosaic:
            if (a.type == AT_Mosaic && !a.mosaicRect) {
                for (POINT& p : a.pts) { p.x = mapX(p.x); p.y = mapY(p.y); }
            } else {
                a.x1 = mapX(a.x1); a.y1 = mapY(a.y1);
                a.x2 = mapX(a.x2); a.y2 = mapY(a.y2);
            }
            break;
        case AT_Brush:
            for (POINT& p : a.pts) { p.x = mapX(p.x); p.y = mapY(p.y); }
            break;
        case AT_Text:
            // 文字不经过此路径（走原 draggingTextAnnotation 平移），此处兜底平移锚点
            a.x1 = mapX(a.x1); a.y1 = mapY(a.y1);
            break;
    }
}

// ==================== GDI+ 文字测量（与提交态渲染同源） ====================
// 提交态文字用 GDI+ DrawString 渲染（StringAlignmentNear 顶部左对齐）。
// 为保证输入态边框/光标/命中与提交态视觉一致，输入态也必须用 GDI+ 测量。
// 旧实现用 GDI GetTextExtentPoint32W，其宽度含尾部 overhang（右侧多出留白），
// 且垂直基线与 GDI+ 不同（导致文字偏靠下），故统一改用 GDI+。

// 测量文字的紧凑包围盒：返回 DrawString(StringAlignmentNear/Near) 下字形相对锚点的偏移与尺寸。
//   outOffsetX/outOffsetY：字形左上角相对锚点 (0,0) 的偏移（通常 X≈0，Y 为字体内部顶部 leading）。
//   outW/outH：字形紧凑宽高。
// 这样边框 = 锚点 + offset ± padding，可左右对称、紧贴字形。

void MeasureTextGdip(HDC hdc, const std::wstring& text, int fontPx,
                             float& outOffsetX, float& outOffsetY, float& outW, float& outH) {
    outOffsetX = 0; outOffsetY = 0; outW = 0; outH = 0;
    if (text.empty()) {
        outH = (float)fontPx;
        return;
    }
    // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
    if (!g_captureCtx || !g_captureCtx->gdipInited
        || !g_captureCtx->gdipFontFamily || !g_captureCtx->gdipStrFmt) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
    Gdiplus::StringFormat* sf = g_captureCtx->gdipStrFmt;
    // MeasureString 返回与 DrawString 一致的布局包围盒（含 GDI+ 的字体内部间距）。
    Gdiplus::RectF origin(0, 0, 0, 0);
    Gdiplus::RectF bounds;
    graphics.MeasureString(text.c_str(), (INT)text.size(), font, origin, sf, &bounds);
    outOffsetX = bounds.X;
    outOffsetY = bounds.Y;
    outW = bounds.Width;
    outH = bounds.Height;
}

// 测量逐字符累计宽度（用于光标定位/选中高亮）。
// widths[i] 表示前 i 个字符（text[0..i-1]）的累计紧凑宽度，widths[0]=0。
// 与 DrawString 渲染进度一致，光标 x = textX + widths[i]。
// P2 优化：原实现逐前缀循环调 MeasureString（O(n²)），改用 MeasureCharacterRanges
// 一次测出各前缀边界（O(n)）。MeasureCharacterRanges 会通过 SetMeasurableCharacterRanges
// 修改 StringFormat，故不能就地改会话共享的 ctx->gdipStrFmt，这里栈上复制一份使用。

static void MeasureCharWidthsGdip(HDC hdc, const std::wstring& text, int fontPx,
                                   std::vector<float>& widths) {
    widths.assign(text.size() + 1, 0.0f);
    if (text.empty()) return;
    // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
    if (!g_captureCtx || !g_captureCtx->gdipInited
        || !g_captureCtx->gdipFontFamily || !g_captureCtx->gdipStrFmt) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
    // 复制会话级 StringFormat（Near/Near），在其上设置可测范围，避免污染共享对象。
    Gdiplus::StringFormat localSf(g_captureCtx->gdipStrFmt);

    const int n = (int)text.size();
    // 布局矩形足够大以容纳整串，避免 Near/Near 文本换行或裁剪。
    Gdiplus::RectF layoutRect(0, 0, 1000000.0f, 1000000.0f);

    // SetMeasurableCharacterRanges 单次可设范围数受 GDI+ 内部缓冲限制，分批处理。
    const int kBatch = 16;
    Gdiplus::CharacterRange ranges[kBatch];
    Gdiplus::Region regions[kBatch];
    for (int start = 1; start <= n; start += kBatch) {
        int cnt = (n - start + 1 < kBatch) ? (n - start + 1) : kBatch;
        for (int j = 0; j < cnt; j++) {
            int i = start + j;              // 前缀长度 i (1..n)
            ranges[j].First = 0;
            ranges[j].Length = i;
        }
        if (localSf.SetMeasurableCharacterRanges(cnt, ranges) != Gdiplus::Ok) continue;
        if (graphics.MeasureCharacterRanges(text.c_str(), n, font, layoutRect,
                                             &localSf, cnt, regions) != Gdiplus::Ok) continue;
        for (int j = 0; j < cnt; j++) {
            int i = start + j;
            Gdiplus::RectF bounds;
            // 前 i 个字符区域的右边缘 = 原 MeasureString(prefix) 的 bounds.X + bounds.Width。
            if (regions[j].GetBounds(&bounds, &graphics) == Gdiplus::Ok) {
                widths[i] = bounds.GetRight();
            }
        }
    }
}

// 根据鼠标位置计算光标在文字中的位置（字符索引）
// 用 GDI+ 逐字符测量，与 DrawString 渲染进度一致。

int CalcCaretPosFromMouse(HDC hdc, const std::wstring& text, int fontPx, int textX, int mouseX) {
    if (text.empty()) return 0;

    std::vector<float> widths;
    MeasureCharWidthsGdip(hdc, text, fontPx, widths);

    int bestPos = 0;
    float bestDist = FLT_MAX;
    for (size_t i = 0; i <= text.size(); i++) {
        float x = (float)textX + widths[i];
        float dist = fabsf(x - (float)mouseX);
        if (dist < bestDist) {
            bestDist = dist;
            bestPos = (int)i;
        }
    }
    return bestPos;
}

// P1 脏区域辅助：使"被拖拽/缩放标注的旧位置 ∪ 新位置"区域无效。
// 用于 resizingAnnotation/draggingAnnotation/draggingTextAnnotation 的 MOUSEMOVE。
// curBox 为当前帧标注包围盒（绝对虚拟屏幕坐标，由调用方用 MeasureAnnotationBounds 算）；
// 与上帧缓存的 lastAnnotationBox 求并集后转 backDC 坐标并扩大。lastAnnotationBox 由 WM_PAINT 更新。
// 扩大量需覆盖选中态的 resize 手柄：手柄圆心贴在包围盒边缘，半径 = handleSize/2，
// 故 inflate 量取 handleSize/2 + 余量（= handleMetrics.handleMargin），确保旧/新手柄范围都在脏区内，
// 避免拖拽时残留手柄痕迹。

void InvalidateAnnotationOp(HWND hwnd, CaptureContext* ctx, const RECT& curBox) {
    RECT b;
    if (ctx->hasLastAnnotationBox) {
        b = UnionRectSafe(ctx->lastAnnotationBox, curBox);
    } else {
        b = curBox;
    }
    b.left -= ctx->virtualX; b.top -= ctx->virtualY;
    b.right -= ctx->virtualX; b.bottom -= ctx->virtualY;
    const int handleMargin = ctx->handleMetrics.handleMargin;  // 手柄半径 + 描边/抗锯齿余量
    InvalidateRect(hwnd, &InflateRectBy(b, handleMargin), FALSE);
}

// 选中/取消选中覆盖物时的精确脏区计算。
// 根据当前 selectedAnnotation / selectedTextAnnotation 的包围盒算出需重绘的脏区
// （含 resize 手柄半径余量），可选合并工具栏 + popup 旧位置（切换不同类型工具时）。
// 调用时机：必须在调用方清空 selected*/hovered* 之前调用，否则读不到旧选中项的包围盒。
// 坐标基准：返回 backDC/客户区坐标（已减 virtualX/Y），可直接用于 InvalidateRect(hwnd, &r, FALSE)。
// 参数 includeToolbar：true 时并集工具栏矩形和 popup 矩形（覆盖 activeTool 切换导致的高亮按钮
//   位移 + popup 打开/关闭/切换），用于「切换到不同类型工具」场景。
// 返回值可能为无效矩形（{0,0,0,0}）：表示当前无任何选中项，调用方据此决定是否全屏重绘兜底。

RECT CalcSelectionDirty(CaptureContext* ctx, bool includeToolbar) {
    RECT dirty = {0, 0, 0, 0};
    const int handleMargin = ctx->handleMetrics.handleMargin;  // 与 InvalidateAnnotationOp 一致的手柄半径余量

    // 非文字标注选中项的包围盒
    if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
        RECT r = MeasureAnnotationBounds(ctx->annotations[ctx->selectedAnnotation], ctx->backDC);
        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
    }
    // 文字标注选中项的包围盒（MeasureTextAnnotation 含文字 padding）
    if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
        RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ctx->selectedTextAnnotation]);
        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
    }
    // 切换不同类型工具：activeTool 变化后高亮按钮位移，popup 可能打开/关闭/切换类型，
    // 将整条工具栏和当前 popup 都纳入脏区（宁大勿小，避免按钮高亮残影）。
    if (includeToolbar) {
        if (IsValidRect(ctx->toolbarRect)) {
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->toolbarRect, 4));
        }
        if (IsValidRect(ctx->popupRect)) {
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->popupRect, 4));
        }
    }
    return dirty;
}

// P1 脏区域辅助：使文字行区域无效（用于文字编辑态的键盘输入/删除/光标移动）。
// 文字行 y 范围取自上帧光标高度（lastCaretRect），x 范围覆盖整个选区宽度
// （文字必然在选区内，选区宽度是文字行可能宽度的安全上界）。lastCaret 无效时全屏。
// 坐标基准：backDC 坐标 = 客户区坐标（已减 virtualX/Y），与 InvalidateRect 一致。

void InvalidateTextLine(HWND hwnd, CaptureContext* ctx) {
    if (!ctx->hasLastCaret) {
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    // 文字行矩形：选区宽度 × 光标行高（backDC 坐标）
    RECT line = {
        ctx->selection.left - ctx->virtualX,
        ctx->lastCaretRect.top,
        ctx->selection.right - ctx->virtualX,
        ctx->lastCaretRect.bottom
    };
    InvalidateRect(hwnd, &InflateRectBy(line, 4), FALSE);
}
