// 截图模块：覆盖层 WM_PAINT 脏区绘制管线（从原 screenshot_windows.cpp 的 WndProc 提取）
#include "internal.h"

LRESULT OnPaint(HWND hwnd, CaptureContext* ctx) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC backDC = ctx->backDC;

    // 计算浮窗位置
    int panelX, panelY;
    CalcPanelPosition(ctx->mouseX, ctx->mouseY,
        ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->panelMetrics, panelX, panelY);
    // 转为相对坐标
    int panelXRel = panelX - ctx->virtualX;
    int panelYRel = panelY - ctx->virtualY;

    RECT curPanelRect = { panelXRel, panelYRel,
        panelXRel + ctx->panelMetrics.w, panelYRel + ctx->panelMetrics.h };

    // 当前选区矩形
    RECT curSelRect = {0,0,0,0};
    if (ctx->state == CS_Selecting) {
        curSelRect.left = (std::min)(ctx->startX, ctx->endX) - ctx->virtualX;
        curSelRect.top = (std::min)(ctx->startY, ctx->endY) - ctx->virtualY;
        curSelRect.right = (std::max)(ctx->startX, ctx->endX) - ctx->virtualX;
        curSelRect.bottom = (std::max)(ctx->startY, ctx->endY) - ctx->virtualY;
    } else if (ctx->state == CS_Confirmed || ctx->state == CS_Resizing
               || ctx->state == CS_Moving || ctx->state == CS_Drawing
               || ctx->state == CS_TextEditing || ctx->state == CS_LongCapturing) {
        curSelRect.left = ctx->selection.left - ctx->virtualX;
        curSelRect.top = ctx->selection.top - ctx->virtualY;
        curSelRect.right = ctx->selection.right - ctx->virtualX;
        curSelRect.bottom = ctx->selection.bottom - ctx->virtualY;
    }

    // 当前高亮窗口矩形
    RECT curHlRect = {0,0,0,0};
    if (ctx->state == CS_Idle && ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
        const RECT& wr = ctx->windows[ctx->hoveredWindow].rect;
        curHlRect = { wr.left - ctx->virtualX, wr.top - ctx->virtualY,
            wr.right - ctx->virtualX, wr.bottom - ctx->virtualY };
    }

    double ds = ctx->dpiScale;
    int physW = (int)(ctx->virtualW * ds + 0.5);
    int physH = (int)(ctx->virtualH * ds + 0.5);

    // P1 脏区域优化：仅"选区正在移动/缩放"的状态（Selecting/Resizing/Moving）
    // 因遮罩边界大范围变化，必须整屏恢复背景。其余 confirmedMode 状态（Confirmed/Drawing/
    // TextEditing）选区静止，可用 ps.rcPaint 局部刷新（由各调用点 InvalidateRect 传精确矩形）。
    // CS_LongCapturing：黑底底栏标签随拼接变化，且部分重绘会露出冻结快照底，故同样整屏重绘。
    if (ctx->state == CS_Selecting || ctx->state == CS_Resizing || ctx->state == CS_Moving
        || ctx->state == CS_LongCapturing) {
        ctx->needFullRedraw = true;
    }

    // 本帧脏区域：fullFrame 时为全屏，否则取 ps.rcPaint（由 InvalidateRect 矩形决定）。
    // 客户区坐标 = backDC 坐标（窗口原点 = 虚拟屏幕左上角），可直接用于背景恢复与裁剪。
    bool fullFrame = ctx->needFullRedraw;
    RECT dirtyRect;
    HRGN dirtyRgn = NULL;
    if (fullFrame) {
        dirtyRect = { 0, 0, ctx->virtualW, ctx->virtualH };
    } else {
        // ps.rcPaint 在 InvalidateRect(NULL) 时为整个客户区（等价全屏），传矩形时为该矩形。
        dirtyRect = ps.rcPaint;
        // 限定到虚拟屏幕范围内（防御性：ps.rcPaint 理论上不会超出客户区=虚拟屏幕）
        if (dirtyRect.left < 0) dirtyRect.left = 0;
        if (dirtyRect.top < 0) dirtyRect.top = 0;
        if (dirtyRect.right > ctx->virtualW) dirtyRect.right = ctx->virtualW;
        if (dirtyRect.bottom > ctx->virtualH) dirtyRect.bottom = ctx->virtualH;
    }

    // 恢复背景：fullFrame 全屏恢复；局部帧只恢复 dirtyRect（其余区域保留上帧最终画面）。
    if (fullFrame) {
        if (ds > 1.01 || ds < 0.99) {
            StretchBlt(backDC, 0, 0, ctx->virtualW, ctx->virtualH,
                ctx->memDC, 0, 0, physW, physH, SRCCOPY);
        } else {
            BitBlt(backDC, 0, 0, ctx->virtualW, ctx->virtualH,
                ctx->memDC, 0, 0, SRCCOPY);
        }
        ctx->needFullRedraw = false;
    } else {
        // 局部恢复脏区域为原始背景（后续渲染管线会对该区域重画遮罩/标注等，
        // 因 AlphaBlend 作用于已恢复的清晰背景，结果与全屏渲染一致）。
        RestoreDirtyRegion(backDC, ctx->memDC, dirtyRect, ds);
        // 设置裁剪区：后续所有 GDI/GDI+ 绘制自动限制在 dirtyRect 内，
        // 避免重绘未变化区域（GDI+ Graphics(backDC) 会继承此裁剪区）。
        dirtyRgn = CreateRectRgn(dirtyRect.left, dirtyRect.top,
                                 dirtyRect.right, dirtyRect.bottom);
        SelectClipRgn(backDC, dirtyRgn);
    }

    // 绘制窗口高亮（Idle 状态）
    if (ctx->state == CS_Idle) {
        if (ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
            // 高亮悬停的窗口
            DrawWindowHighlight(backDC, ctx->windows[ctx->hoveredWindow].rect,
                ctx->virtualX, ctx->virtualY, ctx->gdi);
        } else {
            // 没有匹配到窗口时，高亮鼠标所在的屏幕
            POINT pt = { ctx->mouseX, ctx->mouseY };
            HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            if (hMonitor) {
                MONITORINFO monitorInfo;
                monitorInfo.cbSize = sizeof(MONITORINFO);
                if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                    DrawWindowHighlight(backDC, monitorInfo.rcMonitor,
                        ctx->virtualX, ctx->virtualY, ctx->gdi);
                }
            }
        }
    }

    // ---- 确认/调整/移动/绘制态：遮罩 + 边框 + 手柄 + 标注 + 工具栏 ----
    RECT curLabelRect = {0,0,0,0};
    RECT curToolbarRect = {0,0,0,0};
    RECT curPopupRect = {0,0,0,0};
    bool confirmedMode = (ctx->state == CS_Confirmed || ctx->state == CS_Resizing
                          || ctx->state == CS_Moving || ctx->state == CS_Drawing
                          || ctx->state == CS_TextEditing);
    // CS_LongCapturing 期间覆盖层已被隐藏（独立灰蒙版 + 小地图面板 + 工具栏接管），
    // 这里不绘制任何编辑态内容。
    if (confirmedMode) {
        // 选区外遮罩（选区内部保持清晰）
        DrawDimMask(backDC, ctx->gdi,
            curSelRect.left, curSelRect.top, curSelRect.right, curSelRect.bottom,
            ctx->virtualW, ctx->virtualH, ctx->selectionCornerRadius);
        // 已提交标注 + 正在绘制的标注（绘制范围 clip 在选区内）
        // 调整选区时也保持显示，便于看清内容是否会被裁掉。
        if (ctx->state == CS_Confirmed || ctx->state == CS_Drawing || ctx->state == CS_Resizing) {
            const Annotation* cur = ctx->hasCurDrawing ? &ctx->curDrawing : nullptr;
            // 马赛克（reveal-mask 模型）：只有当前帧确实包含马赛克内容时才延迟生成 base，
            // 普通截图确认态不再承担整张虚拟屏幕的马赛克预处理。
            if (HasMosaicToRender(ctx->annotations, cur)) {
                if (MosaicBaseNeedsRebuild(ctx)) RebuildMosaicBase(ctx);
                if (ctx->mosaicBaseDC) {
                    // 全屏缓存与 backDC 均以虚拟屏幕左上角为原点；把标注绝对坐标转换到
                    // backDC 局部坐标，并用选区矩形硬裁剪马赛克笔刷的可见范围。
                    RevealMosaicToTarget(backDC, ctx->mosaicBaseDC,
                                         ctx->annotations, cur, curSelRect,
                                         (float)-ctx->virtualX, (float)-ctx->virtualY);
                }
            }
            DrawAnnotations(backDC, curSelRect, ctx->virtualX, ctx->virtualY, ctx->annotations, cur);
            // 缓存正在绘制标注的包围盒（绝对虚拟屏幕坐标），供 CS_Drawing 局部刷新计算旧位置
            if (ctx->state == CS_Drawing && ctx->hasCurDrawing) {
                ctx->lastDrawingBox = MeasureAnnotationBounds(ctx->curDrawing, backDC);
                ctx->hasLastDrawingBox = true;
            } else if (ctx->state != CS_Drawing) {
                ctx->hasLastDrawingBox = false;
            }
            // 正在拖拽的矩形马赛克：叠加虚线边框提示当前框选范围（backDC 绝对坐标）。
            // 涂抹模式不画矩形边框（其范围由预览圆体现）。
            if (ctx->state == CS_Drawing && ctx->hasCurDrawing
                && ctx->curDrawing.type == AT_Mosaic && ctx->curDrawing.mosaicRect) {
                int rx1 = (std::min)(ctx->curDrawing.x1, ctx->curDrawing.x2);
                int ry1 = (std::min)(ctx->curDrawing.y1, ctx->curDrawing.y2);
                int rx2 = (std::max)(ctx->curDrawing.x1, ctx->curDrawing.x2);
                int ry2 = (std::max)(ctx->curDrawing.y1, ctx->curDrawing.y2);
                // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
                {
                    Gdiplus::Graphics graphics(backDC);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    // 虚线笔：白色底 + 深色虚线，保证在任意背景上可见
                    Gdiplus::Pen whitePen(Gdiplus::Color(255, 255, 255, 255), 3.0f);
                    graphics.DrawRectangle(&whitePen, (float)rx1, (float)ry1,
                                           (float)(rx2 - rx1), (float)(ry2 - ry1));
                    Gdiplus::Pen dashPen(Gdiplus::Color(255, 0x1E, 0x88, 0xE5), 1.5f);
                    dashPen.SetDashStyle(Gdiplus::DashStyleDash);
                    graphics.DrawRectangle(&dashPen, (float)rx1, (float)ry1,
                                           (float)(rx2 - rx1), (float)(ry2 - ry1));
                }
            }
        }
        // 文字编辑态：绘制输入光标和选中文字标注的边框
        if (ctx->state == CS_TextEditing) {
            if (HasMosaicToRender(ctx->annotations, nullptr)) {
                if (MosaicBaseNeedsRebuild(ctx)) RebuildMosaicBase(ctx);
                if (ctx->mosaicBaseDC) {
                    RevealMosaicToTarget(backDC, ctx->mosaicBaseDC,
                                         ctx->annotations, nullptr, curSelRect,
                                         (float)-ctx->virtualX, (float)-ctx->virtualY);
                }
            }
            // 绘制已提交的标注
            DrawAnnotations(backDC, curSelRect, ctx->virtualX, ctx->virtualY, ctx->annotations, nullptr);

            // 绘制当前输入的文字和光标（统一用 GDI+，与提交态 DrawString 完全一致，
            // 避免旧 GDI TextOutW 导致的文字偏靠下、右侧间距偏大的问题）
            // 注意：测量与绘制共用同一个 GDI+ Graphics 作用域，避免在 backDC 上反复
            // Startup/Shutdown 及创建多个 Graphics 对象引发的状态混乱/崩溃。
            int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];
            COLORREF textColor = SC_COLOR_PRESETS[ctx->drawColorIdx];

            // 文字锚点转换为相对坐标
            int textX = ctx->textAnchorX - ctx->virtualX;
            int textY = ctx->textAnchorY - ctx->virtualY;

            // 单次 GDI+ 作用域：完成测量（整体包围盒 + 逐字符宽度）与文字绘制。
            // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
            // 关键：测量与绘制必须在同一 Graphics 作用域内，故仍用内层 {} 包住。
            float offX = 0, offY = 0, textW = 0, textH = 0;
            std::vector<float> charWidths;
            {
                Gdiplus::Graphics graphics(backDC);
                graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
                // 复用会话缓存的 FontFamily/StringFormat/Font（指针非空，InitGdipResources 保证）
                Gdiplus::Font* font = GetGdipFont(ctx, fontPx);
                Gdiplus::StringFormat* sf = ctx->gdipStrFmt;

                // 整体紧凑包围盒
                Gdiplus::RectF origin(0, 0, 0, 0);
                Gdiplus::RectF bounds;
                if (!ctx->textBuf.empty()) {
                    graphics.MeasureString(ctx->textBuf.c_str(), (INT)ctx->textBuf.size(),
                                           font, origin, sf, &bounds);
                    offX = bounds.X;
                    offY = bounds.Y;
                    textW = bounds.Width;
                    textH = bounds.Height;
                } else {
                    textH = (float)fontPx;
                }
                // 逐字符累计宽度（与 DrawString 渲染进度一致）
                charWidths.assign(ctx->textBuf.size() + 1, 0.0f);
                if (!ctx->textBuf.empty()) {
                    std::wstring sub;
                    sub.reserve(ctx->textBuf.size());
                    for (size_t i = 1; i <= ctx->textBuf.size(); i++) {
                        sub.assign(ctx->textBuf, 0, i);
                        graphics.MeasureString(sub.c_str(), (INT)sub.size(),
                                               font, origin, sf, &bounds);
                        charWidths[i] = bounds.X + bounds.Width;
                    }
                }

                // 绘制文字（与提交态一致：顶部左对齐到锚点）
                if (!ctx->textBuf.empty()) {
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(GetRValue(textColor),
                                                                 GetGValue(textColor),
                                                                 GetBValue(textColor)));
                    Gdiplus::RectF layoutRect((float)textX, (float)textY, 10000.0f,
                                              (float)(fontPx * 2));
                    graphics.DrawString(ctx->textBuf.c_str(), -1, font, layoutRect,
                                        sf, &textBrush);
                }
            }

            // 字形可见区域的左上角（含 GDI+ 内部偏移）
            float glyphLeft = (float)textX + offX;
            float glyphTop  = (float)textY + offY;
            // 文字可见宽度（最小给一个占位宽度，空输入框也有合理大小）
            float glyphW = (textW > 20.0f || !ctx->textBuf.empty()) ? textW : 20.0f;
            float glyphH = (textH > 0 ? textH : (float)fontPx);

            // 绘制边框：左右对称 padding，上下基于字形紧凑高度。
            // 用字形可见区域 + padding，保证左侧与右侧间距一致，并完整包住文字。
            const float padding = 4.0f;
            int boxLeft   = (int)floorf(glyphLeft - padding);
            int boxTop    = (int)floorf(glyphTop - padding);
            int boxRight  = (int)ceilf(glyphLeft + glyphW + padding);
            int boxBottom = (int)ceilf(glyphTop + glyphH + padding);
            HPEN boxPen = CreatePen(PS_SOLID, 1, textColor);
            HGDIOBJ oldPen2 = SelectObject(backDC, boxPen);
            HGDIOBJ oldBrush2 = SelectObject(backDC, GetStockObject(NULL_BRUSH));
            Rectangle(backDC, boxLeft, boxTop, boxRight, boxBottom);
            SelectObject(backDC, oldBrush2);
            SelectObject(backDC, oldPen2);
            DeleteObject(boxPen);

            // 绘制文字选择高亮（基于 GDI+ 字符宽度）
            if (ctx->textSelStart >= 0 && ctx->textSelEnd >= 0 && ctx->textSelStart != ctx->textSelEnd
                && ctx->textSelStart < (int)charWidths.size()
                && ctx->textSelEnd < (int)charWidths.size()) {
                int selStart = (std::min)(ctx->textSelStart, ctx->textSelEnd);
                int selEnd = (std::max)(ctx->textSelStart, ctx->textSelEnd);

                float selLeftF = (float)textX + charWidths[selStart];
                float selRightF = (float)textX + charWidths[selEnd];
                int selLeft = (int)floorf(selLeftF);
                int selRight = (int)ceilf(selRightF);
                int selW = selRight - selLeft;
                int selY = (int)floorf(glyphTop);
                int selH = (int)ceilf(glyphH);
                if (selW > 0 && selH > 0) {
                    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 100, 0 };
                    HDC tempDC = CreateCompatibleDC(backDC);
                    HBITMAP tempBmp = CreateCompatibleBitmap(backDC, selW, selH);
                    SelectObject(tempDC, tempBmp);
                    RECT tempRect = {0, 0, selW, selH};
                    FillRect(tempDC, &tempRect, ctx->gdi.textSelBrush);
                    AlphaBlend(backDC, selLeft, selY, selW, selH, tempDC, 0, 0, selW, selH, blend);
                    DeleteDC(tempDC);
                    DeleteObject(tempBmp);
                }
            }

            // 绘制光标（闪烁效果，基于 GDI+ 字符宽度，高度对齐字形）
            // 始终缓存光标几何（即使本帧不可见），供光标闪烁/方向键的 InvalidateRect 局部刷新。
            if (ctx->textCaretPos >= 0 && ctx->textCaretPos < (int)charWidths.size()) {
                float caretXF = (float)textX + charWidths[ctx->textCaretPos];
                int caretX = (int)floorf(caretXF);
                int caretY2 = (int)floorf(glyphTop);
                int caretH = (int)ceilf(glyphH);
                // 缓存光标矩形（backDC 坐标，含 2px 宽度），供局部刷新
                ctx->lastCaretRect = { caretX - 1, caretY2, caretX + 3, caretY2 + caretH };
                ctx->hasLastCaret = true;
                if (ctx->textCaretVisible) {
                    HPEN caretPen = CreatePen(PS_SOLID, 2, textColor);
                    HGDIOBJ oldPenC = SelectObject(backDC, caretPen);
                    MoveToEx(backDC, caretX, caretY2, NULL);
                    LineTo(backDC, caretX, caretY2 + caretH);
                    SelectObject(backDC, oldPenC);
                    DeleteObject(caretPen);
                }
            }
        }
        // 确认态和文字编辑态：文字标注的选中边框
        // - selectedTextAnnotation：已选中的标注（点击后持久保持），实线高亮边框
        // 注：文字悬浮辅助边框已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
        if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
            // 已选中：实线蓝色边框（醒目）
            if (ctx->selectedTextAnnotation >= 0
                && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                RECT annRect = MeasureTextAnnotation(backDC, ctx->annotations[ctx->selectedTextAnnotation]);
                // 缓存选中文字标注包围盒（绝对坐标），供拖动时局部刷新算旧位置
                ctx->lastAnnotationBox = annRect;
                ctx->hasLastAnnotationBox = true;
                annRect.left -= ctx->virtualX;
                annRect.top -= ctx->virtualY;
                annRect.right -= ctx->virtualX;
                annRect.bottom -= ctx->virtualY;
                HGDIOBJ oldPenS = SelectObject(backDC, ctx->gdi.annTextSelPen);
                HGDIOBJ oldBrushS = SelectObject(backDC, GetStockObject(NULL_BRUSH));
                Rectangle(backDC, annRect.left, annRect.top, annRect.right, annRect.bottom);
                SelectObject(backDC, oldBrushS);
                SelectObject(backDC, oldPenS);
            }
            // 注：文字悬浮辅助边框已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
        }
        // 非文字标注的选中/悬浮边框（确认态/文字编辑态）
        // - selectedAnnotation：已选中，按工具类型差异化渲染（见下方分支）
        // 注：鼠标悬浮辅助包围盒已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
        if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
            // 选中态（按工具类型差异化）：
            //   矩形：无包围盒 + 8 手柄（4 角 + 4 边中点）
            //   圆形：蓝色虚线包围盒 + 8 手柄（4 角 + 4 边中点）
            //   箭头：无包围盒 + 2 端点手柄
            //   画笔：蓝色虚线包围盒 + 无手柄（仅可整体拖动）
            // 手柄统一为白色圆形 + 红色描边（GDI+ 抗锯齿绘制，见下方）。
            if (ctx->selectedAnnotation >= 0
                && ctx->selectedAnnotation < (int)ctx->annotations.size()
                && ctx->annotations[ctx->selectedAnnotation].type != AT_Text) {
                RECT box = MeasureAnnotationBounds(ctx->annotations[ctx->selectedAnnotation], backDC);
                // 缓存选中标注包围盒（绝对坐标），供拖拽/缩放时局部刷新算旧位置
                ctx->lastAnnotationBox = box;
                ctx->hasLastAnnotationBox = true;
                box.left -= ctx->virtualX; box.top -= ctx->virtualY;
                box.right -= ctx->virtualX; box.bottom -= ctx->virtualY;
                const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];

                // 包围盒：仅圆形/画笔画蓝色虚线框；矩形/箭头不画。
                if (selA.type == AT_Circle || selA.type == AT_Brush) {
                    HGDIOBJ oldPenA = SelectObject(backDC, ctx->gdi.annHoverPen);
                    HGDIOBJ oldBrushA = SelectObject(backDC, GetStockObject(NULL_BRUSH));
                    Rectangle(backDC, box.left, box.top, box.right, box.bottom);
                    SelectObject(backDC, oldBrushA);
                    SelectObject(backDC, oldPenA);
                }

                // 收集手柄坐标（backDC 局部坐标）。画笔无手柄；箭头取 2 端点；
                // 矩形/圆形取 8 个（4 角 + 4 边中点）。
                int handles[8][2];
                int handleCount = 0;
                if (selA.type == AT_Arrow) {
                    handles[0][0] = selA.x1 - ctx->virtualX;
                    handles[0][1] = selA.y1 - ctx->virtualY;
                    handles[1][0] = selA.x2 - ctx->virtualX;
                    handles[1][1] = selA.y2 - ctx->virtualY;
                    handleCount = 2;
                } else if (selA.type == AT_Rect || selA.type == AT_Circle) {
                    int cx = (box.left + box.right) / 2;
                    int cy = (box.top + box.bottom) / 2;
                    // 4 角
                    handles[0][0] = box.left;  handles[0][1] = box.top;
                    handles[1][0] = box.right; handles[1][1] = box.top;
                    handles[2][0] = box.left;  handles[2][1] = box.bottom;
                    handles[3][0] = box.right; handles[3][1] = box.bottom;
                    // 4 边中点
                    handles[4][0] = cx;        handles[4][1] = box.top;
                    handles[5][0] = cx;        handles[5][1] = box.bottom;
                    handles[6][0] = box.left;  handles[6][1] = cy;
                    handles[7][0] = box.right; handles[7][1] = cy;
                    handleCount = 8;
                }
                // 白色圆形手柄（红色描边），半径 = handleSize/2（DPI 缩放后），与方块视觉大小一致。
                // GDI+ 抗锯齿绘制（GDI Ellipse 边缘有硬锯齿）：白色填充 + 红色 1px 描边。
                if (handleCount > 0) {
                    int half = ctx->handleMetrics.handleSize / 2;
                    Gdiplus::Graphics graphics(backDC);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::SolidBrush cBrush(Gdiplus::Color(255, 255, 255, 255));
                    Gdiplus::Pen cPen(Gdiplus::Color(255, 229, 57, 53), 1.0f);
                    Gdiplus::GraphicsPath handlePath;
                    for (int i = 0; i < handleCount; i++) {
                        // 直径 2*half 与原 GDI Ellipse(x-half,y-half,x+half,y+half)
                        // 的像素覆盖一致（GDI 右下边 exclusive，直径 = 2*half）。
                        handlePath.AddEllipse(handles[i][0] - half, handles[i][1] - half,
                                              2 * half, 2 * half);
                    }
                    graphics.FillPath(&cBrush, &handlePath);
                    graphics.DrawPath(&cPen, &handlePath);
                }
            }
        }
        // 确认态边框和调整手柄最后绘制，避免马赛克及其他标注覆盖交互轮廓。
        DrawConfirmedBorder(backDC, curSelRect, ctx->gdi, ctx->selectionCornerRadius);
        // 正在拖拽倒角手柄（CS_Resizing + 角手柄）：此时隐藏选区 resize 手柄，仅留被拖的倒角手柄。
        bool draggingCorner = (ctx->state == CS_Resizing && IsCornerRadiusHandle(ctx->resizeHandle));
        // 选区 resize 手柄：确认态/绘制标注/调整中/整体拖动中显示，并随 curSelRect 实时跟随。
        // 倒角手柄拖拽时隐藏，避免两类手柄挤在同一角；文字编辑时不绘制，避免遮挡光标。
        if (!draggingCorner && (ctx->state == CS_Confirmed || ctx->state == CS_Drawing
            || ctx->state == CS_Resizing || ctx->state == CS_Moving)) {
            DrawResizeHandles(backDC, curSelRect, ctx->handleMetrics.handleSize);
        }
        // 倒角拖拽手柄：默认不显示，仅"鼠标靠近某角"或"正拖拽某角"时显示该角一个。
        // 拖拽 -> 取正在拖的角（resizeHandle）；确认态靠近 -> 取 hoveredCornerHandle；
        // 沿对角线内移 d = clamp(inset+radius, 0, maxR)：radius 增大时沿对角线向中心滑动；
        // 松手后 radius 保持 -> 手柄停在原地，不回弹到静止位 inset。
        int visibleCorner = RH_None;
        if (draggingCorner) {
            visibleCorner = ctx->resizeHandle;
        } else if (ctx->state == CS_Confirmed) {
            visibleCorner = ctx->hoveredCornerHandle;
        }
        if (visibleCorner != RH_None) {
            DrawCornerRadiusHandle(backDC, curSelRect,
                ctx->handleMetrics.handleSize, ctx->handleMetrics.cornerKnobInset,
                ctx->selectionCornerRadius, visibleCorner);
        }
        // 调整选区（标准手柄）时显示放大镜：焦点取活动手柄锚点（随活动边移动），
        // 面板置于选区外侧避免遮挡目标。读 memDC 干净像素，所见即真实屏幕。
        if (ctx->state == CS_Resizing && !IsCornerRadiusHandle(ctx->resizeHandle)) {
            int ax, ay;
            GetResizeHandleAnchor(ctx->resizeHandle, ctx->selection, ax, ay);
            int pX, pY;
            CalcResizePanelPosition(ctx->resizeHandle, ctx->selection,
                ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH,
                ctx->panelMetrics, pX, pY);
            int pXRel = pX - ctx->virtualX, pYRel = pY - ctx->virtualY;
            curPanelRect = { pXRel, pYRel,
                pXRel + ctx->panelMetrics.w, pYRel + ctx->panelMetrics.h };
            DrawInfoPanel(backDC, pXRel, pYRel, ctx->currentColor,
                ctx->memDC, ctx->virtualX, ctx->virtualY, ax, ay, ctx->dpiScale,
                ctx->gdi, ctx->panelMetrics, ctx->virtualW, ctx->virtualH);
        }
        // 悬浮工具栏 + 粗细/颜色子菜单
        // 整体拖动选区(CS_Moving)时保持显示并实时跟随；调整选区(CS_Resizing)时仍隐藏，避免手柄附近抖动。
        // 绘制标注(CS_Drawing)/文字编辑(CS_TextEditing)时保持显示，便于随时查看/切换工具与样式。
        if (ctx->state == CS_Confirmed || ctx->state == CS_Moving || ctx->state == CS_Drawing || ctx->state == CS_TextEditing) {
            if (ctx->toolbarPlaced) {
                // 用户拖放后的手动位置：直接沿用（拖拽中由 MOUSEMOVE 实时更新），
                // 不再随选区自动重算
                curToolbarRect = ctx->toolbarRect;
            } else {
                CalcToolbarPosition(curSelRect, ctx->virtualX, ctx->virtualY,
                    ctx->virtualW, ctx->virtualH,
                    ctx->toolbarMetrics, curToolbarRect);
                ctx->toolbarRect = curToolbarRect;
            }
            // hover 命中（相对坐标），确认态和文字编辑态均可更新 hover
            if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
                int mxRel = ctx->mouseX - ctx->virtualX;
                int myRel = ctx->mouseY - ctx->virtualY;
                ctx->hoverToolbarBtn = HitTestToolbar(mxRel, myRel, curToolbarRect, ctx->toolbarMetrics);
            } else {
                ctx->hoverToolbarBtn = -1;
            }
            DrawToolbar(backDC, curToolbarRect, ctx->hoverToolbarBtn, ctx->activeTool,
                ctx->gdi, ctx->toolbarMetrics, ctx->iconCache);
            int popupTool = ctx->popupTool;
            // 马赛克子菜单：模式切换 + 块大小
            if (ctx->popupOpen && popupTool == TB_Mosaic) {
                int mpw, mph;
                CalcMosaicPopupSize(ctx->popupMetrics, mpw, mph);
                CalcPopupPlacement(curToolbarRect, ctx->virtualX, ctx->virtualY,
                    ctx->virtualW, ctx->virtualH,
                    ctx->popupMetrics, mpw, mph, curPopupRect);
                ctx->popupRect = curPopupRect;
                int modeIdx = ctx->mosaicRectMode ? 1 : 0;
                DrawMosaicPopup(backDC, curPopupRect, modeIdx, ctx->mosaicSizeIdx,
                    ctx->mosaicRadiusIdx, ctx->popupMetrics);
            }
            // 粗细/颜色子菜单：文字工具激活时始终显示（含文字编辑态）
            else if (ctx->popupOpen && CanShowStylePopupTool(popupTool)) {
                CalcPopupPosition(curToolbarRect, ctx->virtualX, ctx->virtualY,
                    ctx->virtualW, ctx->virtualH,
                    ctx->popupMetrics, curPopupRect);
                ctx->popupRect = curPopupRect;
                bool isText = (popupTool == TB_Text);
                int firstIdx = isText ? ctx->fontSizeIdx : ctx->drawThickIdx;
                DrawPopup(backDC, curPopupRect, ctx->drawColorIdx, firstIdx, isText,
                    ctx->popupMetrics);
            }
        } else {
            ctx->hoverToolbarBtn = -1;
        }
        // 工具栏 title 式 tooltip 气泡：最后绘制，盖在工具栏/子菜单之上
        DrawToolbarTooltip(backDC, ctx);
        // 确认态不绘制尺寸标签；放大镜仅在调整手柄(CS_Resizing)时绘制（见上方）
    } else {
        // ---- Idle/Selecting 态：原有逻辑 ----
        // 绘制选区外遮罩（微信风格，仅 Selecting 状态），选区内部保持清晰
        if (ctx->state == CS_Selecting) {
            DrawDimMask(backDC, ctx->gdi,
                curSelRect.left, curSelRect.top, curSelRect.right, curSelRect.bottom,
                ctx->virtualW, ctx->virtualH, 0);
        }

        // 绘制选区或窗口尺寸标签
        if (ctx->state == CS_Selecting) {
            curLabelRect = DrawSelection(backDC, ctx->startX, ctx->startY, ctx->endX, ctx->endY,
                ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->gdi, ctx->panelMetrics);
        } else if (ctx->state == CS_Idle) {
            RECT screenRect = {};
            int ww = 0, wh = 0;
            bool haveScreenSize = false;

            if (ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
                // 显示悬停窗口的尺寸
                const RECT& wr = ctx->windows[ctx->hoveredWindow].rect;
                ww = wr.right - wr.left;
                wh = wr.bottom - wr.top;
                screenRect = wr;
                haveScreenSize = true;
            } else {
                // 没有匹配到窗口时，显示当前屏幕的尺寸
                POINT pt = { ctx->mouseX, ctx->mouseY };
                HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                if (hMonitor) {
                    MONITORINFO monitorInfo;
                    monitorInfo.cbSize = sizeof(MONITORINFO);
                    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                        screenRect = monitorInfo.rcMonitor;
                        ww = screenRect.right - screenRect.left;
                        wh = screenRect.bottom - screenRect.top;
                        haveScreenSize = true;
                    }
                }
            }

            // 显示器查询失败（热插拔/驱动重置等瞬态）时不绘制尺寸标签，
            // 避免 screenRect/ww/wh 未就绪即传入 DrawSizeLabel。
            if (haveScreenSize) {
                curLabelRect = DrawSizeLabel(backDC, ww, wh,
                    screenRect.left - ctx->virtualX, screenRect.top - ctx->virtualY,
                    screenRect.right - ctx->virtualX, screenRect.bottom - ctx->virtualY,
                    ctx->virtualW, ctx->virtualH, ctx->gdi, ctx->panelMetrics);
            }
        }

        // 绘制放大镜信息面板
        DrawInfoPanel(backDC, panelXRel, panelYRel, ctx->currentColor,
            ctx->memDC, ctx->virtualX, ctx->virtualY,
            ctx->mouseX, ctx->mouseY, ctx->dpiScale, ctx->gdi, ctx->panelMetrics,
            ctx->virtualW, ctx->virtualH);
    }

    // 更新脏区域追踪
    ctx->lastPanelRect = curPanelRect;
    ctx->lastSelectionRect = curSelRect;
    ctx->lastLabelRect = curLabelRect;
    ctx->lastHighlightRect = curHlRect;
    ctx->lastToolbarRect = curToolbarRect;
    ctx->lastPopupRect = curPopupRect;

    // 取消裁剪区（局部帧设置的），避免影响后续 GDI 操作
    if (dirtyRgn) {
        SelectClipRgn(backDC, NULL);
        DeleteObject(dirtyRgn);
    }

    // 后台缓冲 -> 窗口（局部帧只拷脏区域，fullFrame 拷全屏）
    BitBlt(hdc, dirtyRect.left, dirtyRect.top,
           dirtyRect.right - dirtyRect.left, dirtyRect.bottom - dirtyRect.top,
           backDC, dirtyRect.left, dirtyRect.top, SRCCOPY);
    EndPaint(hwnd, &ps);
    return 0;
}
