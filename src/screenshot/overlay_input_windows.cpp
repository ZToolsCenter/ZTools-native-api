// 截图模块：覆盖层鼠标/键盘/IME 消息处理（从原 screenshot_windows.cpp 的 WndProc 提取）
#include "internal.h"
#include <imm.h>   // IME 输入法支持（OnImeComposition 取 GCS_RESULTSTR 合成结果字符串）

// ==================== 文字提交 / 参数回显公共块 ====================

// 把输入缓冲中的文字按当前字号/颜色固化为一条文字标注并入栈历史（缓冲为空则无任何副作用）。
// 六条提交路径（子菜单字号/颜色点击、工具栏按钮、选区内换位、选区外退出、Enter 键）
// 构造参数完全一致，统一走此函数避免逐处手抄漂移。

static void CommitPendingText(CaptureContext* ctx) {
    if (!ctx->textBuf.empty()) {
        Annotation textAnnotation = {};
        textAnnotation.type = AT_Text;
        textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
        textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
        textAnnotation.x1 = ctx->textAnchorX;
        textAnnotation.y1 = ctx->textAnchorY;
        textAnnotation.text = ctx->textBuf;
        PushAnnotationHistory(ctx);
        ctx->annotations.push_back(textAnnotation);
    }
}

// 提交文字后的完整编辑出口：清空缓冲/光标/选择区间并回到确认态，全屏重绘。
// Enter 提交路径历史上不清选择区间、点选区内路径保持编辑态，二者只复用
// 上面的提交核心，不走本函数（保持各自原有清理序列不变）。

static void CommitPendingTextAndExitEditing(HWND hwnd, CaptureContext* ctx) {
    ctx->textBuf.clear();
    ctx->textCaretPos = 0;
    ctx->textSelStart = -1;
    ctx->textSelEnd = -1;
    ctx->state = CS_Confirmed;
    ctx->needFullRedraw = true;
    InvalidateRect(hwnd, NULL, FALSE);
}

// 参数回显公共块：在对应预设表中顺序查找首个匹配项并写回子菜单高亮索引；
// 无匹配时保持原索引不变（与逐处内联循环语句逐句等价）。矢量标注回显粗细、
// 文字标注回显字号到第一组，颜色到第二组。

static void EchoThickIdx(CaptureContext* ctx, int thicknessPx) {
    for (int i = 0; i < SC_THICK_COUNT; i++) {
        if (SC_THICK_PRESETS[i] == thicknessPx) { ctx->drawThickIdx = i; break; }
    }
}

static void EchoFontIdx(CaptureContext* ctx, int fontSizePx) {
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        if (SC_FONT_SIZES[i] == fontSizePx) { ctx->fontSizeIdx = i; break; }
    }
}

static void EchoColorIdx(CaptureContext* ctx, COLORREF color) {
    for (int i = 0; i < SC_COLOR_COUNT; i++) {
        if (SC_COLOR_PRESETS[i] == color) { ctx->drawColorIdx = i; break; }
    }
}

LRESULT OnLButtonDown(HWND hwnd, CaptureContext* ctx) {
    // 长截图期间覆盖层已被隐藏（灰蒙版 + 面板 + 工具栏接管全部交互），不会收到消息；
    // 保险起见直接忽略，不进入任何编辑态分支。
    if (ctx->state == CS_LongCapturing) return 0;
    if (ctx->state == CS_Idle) {
        // 开始新的框选
        ctx->startX = ctx->mouseX;
        ctx->startY = ctx->mouseY;
        ctx->endX = ctx->mouseX;
        ctx->endY = ctx->mouseY;
        ctx->selectionCornerRadius = 0;  // 新框选从直角开始
        ctx->state = CS_Selecting;
        ctx->needFullRedraw = true;
    } else if (ctx->state == CS_TextEditing) {
        // 文字编辑态：允许点击工具栏/子菜单操作，或点击当前输入框内选择文字
        int mxRel = ctx->mouseX - ctx->virtualX;
        int myRel = ctx->mouseY - ctx->virtualY;

        // 命中粗细/颜色子菜单：切换属性，提交文字，回到确认态
        if (ctx->popupOpen) {
            int hit = HitTestPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
            if (hit > 0) {
                // 字号切换
                ctx->fontSizeIdx = hit - 1;
                // 提交当前文字（如果有），应用新属性给选中的文字标注
                CommitPendingTextAndExitEditing(hwnd, ctx);
                return 0;
            }
            if (hit < 0) {
                // 颜色切换
                ctx->drawColorIdx = -hit - 1;
                // 提交当前文字
                CommitPendingTextAndExitEditing(hwnd, ctx);
                return 0;
            }
        }

        // 点击工具栏按钮：先提交当前文字，再回到确认态
        int toolbarBtn = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);
        if (toolbarBtn >= 0 && toolbarBtn != TB_Separator1 && toolbarBtn != TB_Separator2) {
            // 先提交当前文字（如果有）
            CommitPendingTextAndExitEditing(hwnd, ctx);
            return 0;
        }

        // 点击当前输入框内：开始文字选择或移动光标
        // 命中区与绘制边框完全一致（用 GDI+ 紧凑度量），否则点边框附近算"框外"会提交文字。
        int textX = ctx->textAnchorX - ctx->virtualX;
        int textY = ctx->textAnchorY - ctx->virtualY;
        int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];

        float offX = 0, offY = 0, textW = 0, textH = 0;
        MeasureTextGdip(ctx->backDC, ctx->textBuf, fontPx, offX, offY, textW, textH);
        float glyphLeft = (float)textX + offX;
        float glyphTop  = (float)textY + offY;
        float glyphW = (textW > 20.0f || !ctx->textBuf.empty()) ? textW : 20.0f;
        float glyphH = (textH > 0 ? textH : (float)fontPx);

        const float padding = 4.0f;
        RECT inputBox = {
            (int)floorf(glyphLeft - padding),
            (int)floorf(glyphTop - padding),
            (int)ceilf(glyphLeft + glyphW + padding),
            (int)ceilf(glyphTop + glyphH + padding)
        };

        if (PointInRect(mxRel, myRel, inputBox)) {
            // 点击输入框内：计算光标位置并开始选择
            int caretPos = CalcCaretPosFromMouse(ctx->backDC, ctx->textBuf, fontPx, textX, mxRel);

            ctx->textCaretPos = caretPos;
            ctx->textSelStart = caretPos;
            ctx->textSelEnd = caretPos;
            ctx->textDraggingSelection = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 点击选区内其他位置：提交当前文字，开始新的输入
        if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            CommitPendingText(ctx);
            ctx->textBuf.clear();
            ctx->textAnchorX = ctx->mouseX;
            ctx->textAnchorY = ctx->mouseY;
            ctx->textCaretPos = 0;
            ctx->textSelStart = -1;
            ctx->textSelEnd = -1;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 点击选区外：提交文字并退出文字编辑态
        CommitPendingTextAndExitEditing(hwnd, ctx);
        return 0;
    } else if (ctx->state == CS_Confirmed) {
        // 实时命中测试（不依赖 hover 缓存值）。
        // 远程桌面（RDP）下 WM_MOUSEMOVE 常被节流/合并，hoverToolbarBtn/hoveredTextAnnotation
        // 可能滞后于实际鼠标位置。若继续用缓存判断，点击文字标注时可能误命中残留的工具栏
        // 索引并提前 return，导致文字选中/拖拽分支永远执行不到。
        int mxRel = ctx->mouseX - ctx->virtualX;
        int myRel = ctx->mouseY - ctx->virtualY;
        int b = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);

        // 最左「6 点把手」：进入工具栏整体拖拽（不触发任何工具按钮）。
        // 置 toolbarPlaced 后 OnPaint 不再随选区自动重算位置，此后由
        // MOUSEMOVE 按「按下矩形 + 鼠标位移」平移并钳制在虚拟屏幕内。
        if (b == SC_TB_GRIP) {
            ctx->toolbarDragging = true;
            ctx->toolbarPlaced = true;
            ctx->toolbarDragStartX = ctx->mouseX;
            ctx->toolbarDragStartY = ctx->mouseY;
            ctx->toolbarDragStartRect = ctx->toolbarRect;
            return 0;
        }

        // 点击工具栏按钮（实时命中）
        if (b >= 0 && b != TB_Separator1 && b != TB_Separator2) {
            // 选中态保留规则（与 Figma/PowerPoint 一致）：
            //   - 点击"与已选中标注同类型"的绘制工具按钮（含文字）-> 保留选中，仅切换子菜单开合
            //     （例：选中矩形后点矩形按钮 -> 关闭/重开粗细颜色子菜单，矩形仍选中）。
            //   - 点击"不同类型"的绘制工具按钮 -> 切换到该绘制工具，取消当前选中
            //     （子菜单参数将用于后续绘制，不再作用于旧选中元素，避免参数错配）。
            //   - 点击确认/取消/保存/撤销/重做等"与选中元素无关"的按钮 -> 取消所有选中。
            bool isDrawToolBtn = IsVectorTool(b) || b == TB_Mosaic || b == TB_Text || b == TB_Drag;
            bool matchesSelection = false;
            if (isDrawToolBtn) {
                if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
                    && AnnotationTypeToTool(ctx->annotations[ctx->selectedAnnotation].type) == b) {
                    matchesSelection = true;
                }
                if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()
                    && b == TB_Text) {
                    matchesSelection = true;
                }
            }
            bool hadAnnSel = (ctx->selectedAnnotation >= 0);
            bool hadTextSel = (ctx->selectedTextAnnotation >= 0);
            if (!matchesSelection) {
                // 不匹配选中项：取消所有选中（切换到其它工具或执行无关操作）。
                // 切换到不同类型工具时 activeTool 必变，高亮按钮位移 + popup 可能切换，
                // 故脏区须含工具栏+popup（includeToolbar=true）。需在清状态前算脏区。
                RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->selectedTextAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                if (hadAnnSel || hadTextSel) {
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
            }
            // 确定：提取选区并完成截图
            if (b == TB_Confirm) {
                ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                    ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations, ctx->selectionCornerRadius,
                    ctx->mosaicSizeIdx);
                // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放 result 防泄漏。
                EmitScreenshotResult(result->success, result->x, result->y, result->x2, result->y2,
                                     result->width, result->height, result->base64);
                delete result;
                ctx->state = CS_Done;
                DestroyWindow(hwnd);
                return 0;
            }
            // 取消：回调失败并关闭（统一走 EmitScreenshotResult）
            if (b == TB_Cancel) {
                EmitScreenshotResult(false);
                ctx->state = CS_Cancelled;
                DestroyWindow(hwnd);
                return 0;
            }
            // 矢量工具：切换激活态 + 打开/关闭粗细颜色子菜单
            if (b == TB_Rect || b == TB_Circle || b == TB_Arrow || b == TB_Brush) {
                if (ctx->activeTool == b) {
                    // 再次点同一工具：关闭工具与子菜单
                    ctx->activeTool = -1;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                } else {
                    ctx->activeTool = b;
                    ctx->popupTool = b;
                    ctx->popupOpen = true;
                    // 匹配选中项时回显该标注的粗细/颜色到子菜单
                    if (matchesSelection && ctx->selectedAnnotation >= 0
                        && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                        const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];
                        EchoThickIdx(ctx, selA.thickness);
                        EchoColorIdx(ctx, selA.color);
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 文字工具：切换激活态 + 打开/关闭子菜单（字号+颜色）
            if (b == TB_Text) {
                if (ctx->activeTool == b) {
                    // 再次点同一工具：关闭工具与子菜单
                    ctx->activeTool = -1;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                } else {
                    ctx->activeTool = b;
                    ctx->popupTool = b;
                    ctx->popupOpen = true;
                    // 匹配选中文字时回显该标注的字号/颜色到子菜单
                    if (matchesSelection && ctx->selectedTextAnnotation >= 0
                        && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                        const Annotation& selT = ctx->annotations[ctx->selectedTextAnnotation];
                        EchoFontIdx(ctx, selT.thickness);
                        EchoColorIdx(ctx, selT.color);
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 马赛克工具：切换激活态 + 打开/关闭子菜单（模式+块大小）
            if (b == TB_Mosaic) {
                if (ctx->activeTool == b) {
                    // 再次点同一工具：关闭工具与子菜单
                    ctx->activeTool = -1;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                } else {
                    ctx->activeTool = b;
                    ctx->popupTool = b;
                    ctx->popupOpen = true;
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (b == TB_Drag) {
                if (ctx->activeTool == b) {
                    ctx->activeTool = -1;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                } else {
                    ctx->activeTool = b;
                    if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                        ctx->popupTool = TB_Text;
                        const Annotation& selT = ctx->annotations[ctx->selectedTextAnnotation];
                        EchoFontIdx(ctx, selT.thickness);
                        EchoColorIdx(ctx, selT.color);
                        ctx->popupOpen = true;
                    } else if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                        const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];
                        ctx->popupTool = AnnotationTypeToTool(selA.type);
                        EchoThickIdx(ctx, selA.thickness);
                        EchoColorIdx(ctx, selA.color);
                        ctx->popupOpen = CanShowStylePopupTool(ctx->popupTool);
                    } else {
                        ctx->popupTool = -1;
                        ctx->popupOpen = false;
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 撤销：恢复上一份标注快照
            if (b == TB_Undo) {
                if (UndoAnnotations(ctx)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            // 重做：恢复下一份标注快照
            if (b == TB_Redo) {
                if (RedoAnnotations(ctx)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            // 保存到本地：弹出系统保存对话框，保存为 PNG 后关闭截图
            if (b == TB_Save) {
                std::wstring filePath = PromptSaveFilePath(hwnd);
                if (!filePath.empty()) {
                    bool saved = SaveRegionToPngFile(ctx->memDC, ctx->selection,
                        ctx->virtualX, ctx->virtualY, ctx->dpiScale,
                        ctx->annotations, filePath, ctx->selectionCornerRadius, ctx->mosaicSizeIdx);
                    // 无论保存成功与否，均关闭截图窗口（用户已选择保存路径）
                    // 通过回调告知 JS 结果（成功/失败），不回传路径。
                    // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放防泄漏
                    // （原代码先 new 后判 TSFN，TSFN 为空时 result 泄漏 —— 此处一并修复）。
                    if (saved) {
                        EmitScreenshotResult(true, ctx->selection.left, ctx->selection.top,
                            ctx->selection.right, ctx->selection.bottom,
                            ctx->selection.right - ctx->selection.left,
                            ctx->selection.bottom - ctx->selection.top);
                    } else {
                        EmitScreenshotResult(false);
                    }
                    ctx->state = CS_Done;
                    DestroyWindow(hwnd);
                }
                // 用户取消保存对话框：不关闭，留在编辑态
                return 0;
            }
            // 长截图：复用当前选区进入手动滚动捕获（隐藏覆盖层 → 预览面板 → 滚轮增量拼接）
            if (b == TB_LongCapture) {
                BeginLongCapture(ctx, hwnd);
                return 0;
            }
            return 0;
        }

        // 命中马赛克子菜单（模式切换 + 块大小 + 涂抹半径）
        // 马赛克标注本身不可选中，且这些选项只影响后续绘制、不作用于已选中的其它元素，
        // 故均属于"非拖拽/resize 动作"-> 取消当前选中态。
        if (ctx->popupOpen && ctx->popupTool == TB_Mosaic) {
            int hit = HitTestMosaicPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
            // 清除选中态并在清空前计算脏区（马赛克 popup 不改 activeTool，仅清覆盖物选中）。
            // 返回 dirty（backDC 坐标）；调用方据 IsValidRect 决定局部或全屏 invalidate。
            auto clearSel = [&]() {
                RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                ctx->selectedAnnotation = -1;
                ctx->selectedTextAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                return dirty;
            };
            if (hit == 1) {
                ctx->mosaicRectMode = false;  // 涂抹模式
                { RECT d = clearSel();
                  if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            if (hit == 2) {
                ctx->mosaicRectMode = true;   // 框选模式
                { RECT d = clearSel();
                  if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            // 命中码编码预留区间远宽于预设表实际档数（见 HitTestMosaicPopup 返回约定），
            // 索引合法性不能依赖生成端巧合：超出现有档位的命中码直接忽略本次点击，
            // 防止常量表扩档或编码变动后 mosaicSizeIdx/mosaicRadiusIdx 越界（下游多处无校验直取数组）。
            if (hit >= 101 && hit < 200) {
                if ((size_t)(hit - 101) >= (size_t)SC_MOSAIC_COUNT) return 0;
                ctx->mosaicSizeIdx = hit - 101;
                { RECT d = clearSel();
                  if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
            if (hit >= 201) {
                if ((size_t)(hit - 201) >= (size_t)SC_MOSAIC_RADIUS_COUNT) return 0;
                ctx->mosaicRadiusIdx = hit - 201;
                { RECT d = clearSel();
                  if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                return 0;
            }
        }

        // 命中粗细/颜色子菜单
        if (ctx->popupOpen) {
            int hit = HitTestPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
            if (hit > 0) {
                // 第一组：文字工具时为字号索引，矢量工具时为粗细索引
                if (ctx->popupTool == TB_Text) {
                    ctx->fontSizeIdx = hit - 1;
                    // 如果选中了文字标注，修改其字号（选中文字时子菜单改动作用于该标注）
                    if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                        int newSize = SC_FONT_SIZES[ctx->fontSizeIdx];
                        if (ctx->annotations[ctx->selectedTextAnnotation].thickness != newSize) {
                            PushAnnotationHistory(ctx);
                            ctx->annotations[ctx->selectedTextAnnotation].thickness = newSize;
                            ctx->annotations[ctx->selectedTextAnnotation].textCacheValid = false;
                        }
                    }
                } else {
                    ctx->drawThickIdx = hit - 1;
                    // 矢量工具改粗细：若已选中矢量标注，则作用于该标注（保持选中）；
                    // 否则作用于后续绘制。
                    if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                        int newThickness = SC_THICK_PRESETS[ctx->drawThickIdx];
                        if (ctx->annotations[ctx->selectedAnnotation].thickness != newThickness) {
                            PushAnnotationHistory(ctx);
                            ctx->annotations[ctx->selectedAnnotation].thickness = newThickness;
                        }
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (hit < 0) {
                ctx->drawColorIdx = -hit - 1;
                // 如果选中了文字标注，修改其颜色（选中文字时颜色改动作用于该标注）
                if (ctx->popupTool == TB_Text && ctx->selectedTextAnnotation >= 0
                    && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                    COLORREF newColor = SC_COLOR_PRESETS[ctx->drawColorIdx];
                    if (ctx->annotations[ctx->selectedTextAnnotation].color != newColor) {
                        PushAnnotationHistory(ctx);
                        ctx->annotations[ctx->selectedTextAnnotation].color = newColor;
                    }
                }
                // 矢量工具改颜色：若已选中矢量标注，则作用于该标注（保持选中）；
                // 否则作用于后续绘制。
                if (ctx->popupTool != TB_Text && ctx->selectedAnnotation >= 0
                    && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                    COLORREF newColor = SC_COLOR_PRESETS[ctx->drawColorIdx];
                    if (ctx->annotations[ctx->selectedAnnotation].color != newColor) {
                        PushAnnotationHistory(ctx);
                        ctx->annotations[ctx->selectedAnnotation].color = newColor;
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        // ===== 通用标注交互（选中/拖拽/缩放，优先于绘制工具）=====
        // 1) 已选中标注的四角 resize 手柄命中 -> 进入缩放
        //    （须在普通命中之前：手柄贴在选中框角上，可能与标注本体重叠）
        //    不复用 CS_Resizing（该状态会改 ctx->selection 选区），用 resizingAnnotation 标志
        //    在 CS_Confirmed 下独立处理，与文字拖拽(draggingTextAnnotation)机制对称。
        if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
            && !IsDragTool(ctx->activeTool)) {
            Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
            RECT box = MeasureAnnotationBounds(sel, ctx->backDC);
            // 按类型命中缩放手柄：箭头=2 端点；矩形/圆=8 手柄；画笔=无（仅可拖动）
            int handle = HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY, ctx->backDC, ctx->handleMetrics.handleSize);
            if (handle != RH_None) {
                ctx->resizingAnnotation = ctx->selectedAnnotation;
                ctx->annotationResizeHandle = handle;
                ctx->annotationDragStartX = ctx->mouseX;
                ctx->annotationDragStartY = ctx->mouseY;
                ctx->annotationResizeStartBox = box;
                ctx->dragStartAnnotation = ctx->annotations[ctx->selectedAnnotation];
                ctx->annotationOpHistoryPushed = false;
                ctx->needFullRedraw = true;
                return 0;
            }
        }
        // 2) 文字标注命中 -> 优先选中并可拖动
        //    文字与画笔/矩形等覆盖物重叠时，优先进入文字选中逻辑，避免被非文字命中分支吞掉。
        int hitText = HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
        if (hitText >= 0) {
            if (ctx->activeTool == TB_Text) {
                // 选中文字标注，保持确认态。
                // selectedTextAnnotation 持久保持选中态（与 hover 解耦），鼠标移开仍高亮，
                // 直到点击空白或进入其他操作才清除。
                // 精确脏区：清掉上一个选中项（可能为非文字/另一文字）的选中边框。
                // 须在赋新选中前算脏区，否则读不到旧选中项的包围盒。
                RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->selectedTextAnnotation = hitText;
                ctx->hoveredTextAnnotation = hitText;
                // 工具栏回显：文字工具保持高亮，并继续回显文字参数。
                ctx->activeTool = TB_Text;
                ctx->popupTool = TB_Text;
                EchoFontIdx(ctx, ctx->annotations[hitText].thickness);
                EchoColorIdx(ctx, ctx->annotations[hitText].color);
                ctx->popupOpen = true;
                ctx->draggingTextAnnotation = hitText;
                ctx->textDragStartX = ctx->mouseX;
                ctx->textDragStartY = ctx->mouseY;
                ctx->dragStartX = ctx->annotations[hitText].x1;
                ctx->dragStartY = ctx->annotations[hitText].y1;
                ctx->annotationOpHistoryPushed = false;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            } else {
                // 非文字工具或空态下：切换选中目标到文字。
                const int handleMargin = ctx->handleMetrics.handleMargin;
                RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                RECT newBox = MeasureTextAnnotation(ctx->backDC, ctx->annotations[hitText]);
                newBox.left -= ctx->virtualX; newBox.top -= ctx->virtualY;
                newBox.right -= ctx->virtualX; newBox.bottom -= ctx->virtualY;
                dirty = UnionRectSafe(dirty, InflateRectBy(newBox, handleMargin));
                ctx->draggingTextAnnotation = hitText;
                ctx->hoveredAnnotation = -1;
                ctx->selectedAnnotation = -1;
                ctx->hoveredTextAnnotation = hitText;
                ctx->selectedTextAnnotation = hitText;
                // 工具栏回显：切到文字工具按钮，打开字号/颜色子菜单，同步该标注参数。
                ctx->activeTool = TB_Text;
                ctx->popupTool = TB_Text;
                ctx->popupOpen = true;
                EchoFontIdx(ctx, ctx->annotations[hitText].thickness);
                EchoColorIdx(ctx, ctx->annotations[hitText].color);
                ctx->textDragStartX = ctx->mouseX;
                ctx->textDragStartY = ctx->mouseY;
                ctx->dragStartX = ctx->annotations[hitText].x1;
                ctx->dragStartY = ctx->annotations[hitText].y1;
                ctx->annotationOpHistoryPushed = false;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
        }

        // 3) 任意非文字标注命中 -> 选中并可拖拽
        //    工具激活时也优先选中已有对象（与 Figma/PowerPoint 一致），点空白才绘制。
        int hitAnn = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
        if (hitAnn >= 0 && ctx->annotations[hitAnn].type != AT_Text) {
            // 切换选中目标：脏区 = 旧选中项（清掉其边框/手柄）∪ 新目标（显示新边框/手柄）
            // ∪ 工具栏+popup（activeTool 可能变化导致高亮按钮位移）。
            // 须在改 selectedAnnotation 之前算旧选中脏区。
            const int handleMargin = ctx->handleMetrics.handleMargin;
            RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
            RECT newBox = MeasureAnnotationBounds(ctx->annotations[hitAnn], ctx->backDC);
            newBox.left -= ctx->virtualX; newBox.top -= ctx->virtualY;
            newBox.right -= ctx->virtualX; newBox.bottom -= ctx->virtualY;
            dirty = UnionRectSafe(dirty, InflateRectBy(newBox, handleMargin));
            ctx->selectedAnnotation = hitAnn;
            ctx->hoveredAnnotation = hitAnn;
            // 与文字选中互斥：选中非文字时清文字选中
            ctx->selectedTextAnnotation = -1;
            ctx->hoveredTextAnnotation = -1;
            // 工具栏回显：保持原有回显逻辑。
            // 同步当前粗细/颜色索引为该标注的值，子菜单高亮与选中元素一致（回显参数）。
            const Annotation& hitA = ctx->annotations[hitAnn];
            ctx->activeTool = AnnotationTypeToTool(hitA.type);
            ctx->popupTool = ctx->activeTool;
            ctx->popupOpen = true;
            EchoThickIdx(ctx, hitA.thickness);
            EchoColorIdx(ctx, hitA.color);
            // 进入拖拽模式：按下即可移动（按下未移动时，松开仅保持选中态）
            ctx->draggingAnnotation = hitAnn;
            ctx->annotationDragStartX = ctx->mouseX;
            ctx->annotationDragStartY = ctx->mouseY;
            ctx->dragStartAnnotation = ctx->annotations[hitAnn];
            ctx->annotationOpHistoryPushed = false;
            // 触发重绘：needFullRedraw 仅是 WM_PAINT 内的提示，必须有 InvalidateRect 才会触发 WM_PAINT。
            if (IsValidRect(dirty)) {
                InvalidateRect(hwnd, &dirty, FALSE);
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        // 矢量工具激活时，点击选区内部/工具栏外 -> 开始绘制
        // 子菜单保持打开，绘制中可继续看到当前粗细/颜色。
        // 注意：上面通用标注命中分支已拦截「点中已有标注」的情况，此处仅点空白才进入。
        if (IsVectorTool(ctx->activeTool) && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            // 点空白绘制新标注：清除已有选中态
            ctx->selectedAnnotation = -1;
            ctx->hoveredAnnotation = -1;
            ctx->hasCurDrawing = true;
            ctx->curDrawing = {};
            ctx->curDrawing.type = ToolToAnnotationType(ctx->activeTool);
            ctx->curDrawing.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
            ctx->curDrawing.thickness = SC_THICK_PRESETS[ctx->drawThickIdx];
            // 起点用绝对虚拟屏幕坐标
            ctx->curDrawing.x1 = ctx->mouseX;
            ctx->curDrawing.y1 = ctx->mouseY;
            ctx->curDrawing.x2 = ctx->curDrawing.x1;
            ctx->curDrawing.y2 = ctx->curDrawing.y1;
            if (ctx->curDrawing.type == AT_Brush) {
                POINT p = { ctx->curDrawing.x1, ctx->curDrawing.y1 };
                ctx->curDrawing.pts.push_back(p);
            }
            ctx->state = CS_Drawing;
            ctx->needFullRedraw = true;
            return 0;
        }

        // 马赛克工具激活时，点击选区内部 -> 开始马赛克绘制
        // 子菜单保持打开，绘制中可继续看到当前模式/块大小。
        if (ctx->activeTool == TB_Mosaic && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            // 点空白绘制新标注：清除已有选中态
            ctx->selectedAnnotation = -1;
            ctx->hoveredAnnotation = -1;
            ctx->hasCurDrawing = true;
            ctx->curDrawing = {};
            ctx->curDrawing.type = AT_Mosaic;
            ctx->curDrawing.color = 0;  // 马赛克无颜色
            ctx->curDrawing.mosaicRect = ctx->mosaicRectMode;
            ctx->curDrawing.mosaicSize = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
            ctx->curDrawing.brushRadius = SC_MOSAIC_RADIUS[ctx->mosaicRadiusIdx];
            ctx->curDrawing.x1 = ctx->mouseX;
            ctx->curDrawing.y1 = ctx->mouseY;
            ctx->curDrawing.x2 = ctx->curDrawing.x1;
            ctx->curDrawing.y2 = ctx->curDrawing.y1;
            if (!ctx->mosaicRectMode) {
                // 涂抹模式：记录路径起点（揭示由 WM_PAINT 统一处理）
                POINT p = { ctx->curDrawing.x1, ctx->curDrawing.y1 };
                ctx->curDrawing.pts.push_back(p);
            }
            ctx->state = CS_Drawing;
            ctx->needFullRedraw = true;
            return 0;
        }

        // 文字工具激活时：点击选区内空白 -> 进入输入态
        if (ctx->activeTool == TB_Text) {
            if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                ctx->textBuf.clear();
                ctx->textAnchorX = ctx->mouseX;
                ctx->textAnchorY = ctx->mouseY;
                ctx->textCaretPos = 0;
                ctx->textSelStart = -1;
                ctx->textSelEnd = -1;
                // 精确脏区：清掉上一个选中项的选中边框/手柄。须在清状态前算。
                RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                ctx->state = CS_TextEditing;
                // 进入输入态时清除文字/非文字标注选中，避免残留选中边框
                ctx->hoveredTextAnnotation = -1;
                ctx->selectedTextAnnotation = -1;
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                // 保持子菜单打开，清空绘制标志
                // ctx->popupOpen 保持不变
                ctx->hasCurDrawing = false;
                // 文字工具保持激活，避免工具栏视觉状态丢失
                // ctx->activeTool 保持为 TB_Text
                // 不置 needFullRedraw：进入编辑态仅改变选中边框，用局部脏区即可，避免全屏重绘。
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
        }

        // 命中调整手柄 -> 进入 Resizing
        int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection, ctx->handleMetrics.handleSize);
        if (h != RH_None) {
            ctx->selectedTextAnnotation = -1;  // 进入手柄调整，清除文字选中
            ctx->selectedAnnotation = -1;      // 清除非文字标注选中
            ctx->hoveredAnnotation = -1;
            ctx->resizeHandle = h;
            ctx->dragStartX = ctx->mouseX;
            ctx->dragStartY = ctx->mouseY;
            ctx->dragStartSelection = ctx->selection;
            ctx->kbDX = 0;
            ctx->kbDY = 0;
            ctx->state = CS_Resizing;
            ctx->needFullRedraw = true;
            return 0;
        }
        // 命中圆角手柄 -> 进入圆角调整（复用 CS_Resizing + 对应角手柄）
        int corner = HitTestCornerRadiusHandle(ctx->mouseX, ctx->mouseY, ctx->selection,
            ctx->handleMetrics.handleSize, ctx->handleMetrics.cornerKnobInset, ctx->selectionCornerRadius);
        if (corner != RH_None) {
            ctx->selectedTextAnnotation = -1;
            ctx->selectedAnnotation = -1;
            ctx->hoveredAnnotation = -1;
            ctx->resizeHandle = corner;  // 记录具体角，决定拖拽方向与光标
            ctx->dragStartX = ctx->mouseX;
            ctx->dragStartY = ctx->mouseY;
            ctx->dragStartSelection = ctx->selection;
            ctx->dragStartRadius = ctx->selectionCornerRadius;
            ctx->kbDX = 0;
            ctx->kbDY = 0;
            ctx->state = CS_Resizing;
            ctx->needFullRedraw = true;
            return 0;
        }
        // 点击选区内部 -> 整体拖动（已有标注内容时禁止，避免标注与背景错位）
        if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            if (!ctx->annotations.empty()) {
                // 已有预处理内容：只能通过手柄改范围，不允许整体拖动。
                // 点空白（选中态的拖动被禁止）-> 取消当前选中。
                if (ctx->selectedAnnotation >= 0 || ctx->selectedTextAnnotation >= 0) {
                    // 精确脏区：清掉选中项的选中边框/手柄。须在清状态前算。
                    RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                    ctx->selectedTextAnnotation = -1;
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    ctx->hoveredTextAnnotation = -1;
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                return 0;
            }
            ctx->selectedTextAnnotation = -1;  // 进入选区拖动，清除文字选中
            ctx->selectedAnnotation = -1;      // 清除非文字标注选中
            ctx->hoveredAnnotation = -1;
            ctx->hoveredTextAnnotation = -1;
            ctx->dragStartX = ctx->mouseX;
            ctx->dragStartY = ctx->mouseY;
            ctx->dragStartSelection = ctx->selection;
            ctx->state = CS_Moving;
            ctx->needFullRedraw = true;
            return 0;
        }
        // 点击选区外空白：清除文字/非文字标注选中，进入确认态后不可重新框选，忽略点击
        {
            RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
            ctx->selectedTextAnnotation = -1;
            ctx->selectedAnnotation = -1;
            ctx->hoveredAnnotation = -1;
            ctx->hoveredTextAnnotation = -1;
            if (IsValidRect(dirty)) {
                InvalidateRect(hwnd, &dirty, FALSE);
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }
    return 0;
}

LRESULT OnMouseMove(HWND hwnd, CaptureContext* ctx) {
    POINT pt;
    GetCursorPos(&pt);
    ctx->mouseX = pt.x;
    ctx->mouseY = pt.y;
    // currentColor 仅用于放大镜信息面板（DrawInfoPanel）。该面板在 CS_Idle/CS_Selecting
    // 以及 CS_Resizing（标准手柄）态绘制：前两者在此取色（鼠标处），CS_Resizing 的取色
    // 由 ApplyResizeSelection 在活动手柄锚点处完成，故此处不再重复。启动时已取一次初值。
    if (ctx->state == CS_Idle || ctx->state == CS_Selecting) {
        ctx->currentColor = GetPixelColorFromBitmap(ctx->memDC,
            ctx->mouseX, ctx->mouseY, ctx->virtualX, ctx->virtualY, ctx->dpiScale);
    }

    // 工具栏拖拽中（把手按下）：按「按下矩形 + 鼠标位移」平移工具栏并局部刷新
    // 旧 ∪ 新区域。子菜单锚定工具栏，同步预估新位置一并失效；ctx->toolbarRect /
    // popupRect 即刻更新，保证拖拽帧间命中测试与绘制一致。
    if (ctx->toolbarDragging) {
        int dx = pt.x - ctx->toolbarDragStartX;
        int dy = pt.y - ctx->toolbarDragStartY;
        RECT n = ctx->toolbarDragStartRect;
        n.left += dx; n.right += dx; n.top += dy; n.bottom += dy;
        int tw = n.right - n.left, th = n.bottom - n.top;
        // 钳制在整个虚拟屏幕内（显式拖放允许跨屏，不受单显示器边界约束）
        if (n.left < 0) { n.left = 0; n.right = tw; }
        if (n.top < 0) { n.top = 0; n.bottom = th; }
        if (n.right > ctx->virtualW) { n.right = ctx->virtualW; n.left = n.right - tw; }
        if (n.bottom > ctx->virtualH) { n.bottom = ctx->virtualH; n.top = n.bottom - th; }
        RECT dirty = InflateRectBy(UnionRectSafe(ctx->toolbarRect, n), 2);
        if (ctx->popupOpen) {
            RECT np = {0, 0, 0, 0};
            if (ctx->popupTool == TB_Mosaic) {
                int mpw, mph;
                CalcMosaicPopupSize(ctx->popupMetrics, mpw, mph);
                CalcPopupPlacement(n, ctx->virtualX, ctx->virtualY,
                    ctx->virtualW, ctx->virtualH, ctx->popupMetrics, mpw, mph, np);
            } else {
                CalcPopupPosition(n, ctx->virtualX, ctx->virtualY,
                    ctx->virtualW, ctx->virtualH, ctx->popupMetrics, np);
            }
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->popupRect, 2));
            dirty = UnionRectSafe(dirty, InflateRectBy(np, 2));
            ctx->popupRect = np;
        }
        ctx->toolbarRect = n;
        InvalidateRect(hwnd, &dirty, FALSE);
        return 0;
    }

    if (ctx->state == CS_Selecting) {
        ctx->endX = ctx->mouseX;
        ctx->endY = ctx->mouseY;
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (ctx->state == CS_Idle) {
        int newHovered = FindWindowAtPoint(ctx->windows, ctx->mouseX, ctx->mouseY);
        ctx->hoveredWindow = newHovered;
        // 像素信息浮窗跟随鼠标：刷新旧面板位置 ∪ 新面板位置（放大镜跟随，两块都需重绘）。
        // 新面板位置在此预算（与 WM_PAINT 的 CalcPanelPosition 同源）。
        int npx, npy;
        CalcPanelPosition(ctx->mouseX, ctx->mouseY,
            ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->panelMetrics, npx, npy);
        RECT newPanel = { npx - ctx->virtualX, npy - ctx->virtualY,
                          npx - ctx->virtualX + ctx->panelMetrics.w, npy - ctx->virtualY + ctx->panelMetrics.h };
        RECT dirty = InflateRectBy(UnionRectSafe(ctx->lastPanelRect, newPanel), 2);
        // 窗口高亮变化也纳入（hoveredWindow 切换时旧/新高亮框）
        if (newHovered >= 0 && newHovered < (int)ctx->windows.size()) {
            RECT hr = ctx->windows[newHovered].rect;
            hr.left -= ctx->virtualX; hr.top -= ctx->virtualY;
            hr.right -= ctx->virtualX; hr.bottom -= ctx->virtualY;
            dirty = UnionRectSafe(dirty, InflateRectBy(hr, 5));
        }
        dirty = UnionRectSafe(dirty, InflateRectBy(ctx->lastHighlightRect, 5));
        InvalidateRect(hwnd, &dirty, FALSE);
    } else if (ctx->state == CS_Resizing) {
        if (IsCornerRadiusHandle(ctx->resizeHandle)) {
            // 圆角调整：手柄沿所在角对角线滑动，四角同步（共用同一 radius）。
            // 取鼠标位移在该角对角线方向的投影（两向内分量之和/2）作为半径增量：
            // 垂直于对角线的位移被忽略，故手柄轨迹恒为对角线。
            int dx = pt.x - ctx->dragStartX;
            int dy = pt.y - ctx->dragStartY;
            int inX, inY;
            switch (ctx->resizeHandle) {
                case RH_CornerRadiusTL: inX =  dx; inY =  dy; break;  // 右下为内
                case RH_CornerRadiusTR: inX = -dx; inY =  dy; break;  // 左下为内
                case RH_CornerRadiusBL: inX =  dx; inY = -dy; break;  // 右上为内
                case RH_CornerRadiusBR: inX = -dx; inY = -dy; break;  // 左上为内
                default:               inX =  dx; inY =  dy; break;
            }
            int diagDelta = (inX + inY) / 2;  // 对角线方向位移（轴向）
            int w = ctx->selection.right - ctx->selection.left;
            int h = ctx->selection.bottom - ctx->selection.top;
            int maxR = (std::min)(w, h) / 2;
            if (maxR < 0) maxR = 0;
            int r = ctx->dragStartRadius + diagDelta;
            if (r < 0) r = 0;
            if (r > maxR) r = maxR;
            ctx->selectionCornerRadius = r;
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
        // 每帧都从按下时的完整快照重算：活动端可以越过固定端并翻转，固定端不会
        // 因上一帧 NormalizeRect 后的 left/right、top/bottom 角色交换而漂移。
        // 鼠标位移 + 键盘微调(kbDX/kbDY) 共同决定活动端；放大镜焦点取活动手柄锚点。
        ApplyResizeSelection(hwnd, ctx);
        }
    } else if (ctx->state == CS_Moving) {
        // 整体平移
        int dx = pt.x - ctx->dragStartX;
        int dy = pt.y - ctx->dragStartY;
        int sw = ctx->dragStartSelection.right - ctx->dragStartSelection.left;
        int sh = ctx->dragStartSelection.bottom - ctx->dragStartSelection.top;
        int nl = ctx->dragStartSelection.left + dx;
        int nt = ctx->dragStartSelection.top + dy;
        // 约束到虚拟屏幕
        if (nl < ctx->virtualX) nl = ctx->virtualX;
        if (nt < ctx->virtualY) nt = ctx->virtualY;
        if (nl + sw > ctx->virtualX + ctx->virtualW) nl = ctx->virtualX + ctx->virtualW - sw;
        if (nt + sh > ctx->virtualY + ctx->virtualH) nt = ctx->virtualY + ctx->virtualH - sh;
        ctx->selection.left = nl;
        ctx->selection.top = nt;
        ctx->selection.right = nl + sw;
        ctx->selection.bottom = nt + sh;
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (ctx->state == CS_Drawing) {
        // 更新正在绘制的标注终点/路径（选区相对坐标）
        if (ctx->hasCurDrawing) {
            // 终点用绝对坐标，钳制到选区内
            int selL = ctx->selection.left, selR = ctx->selection.right;
            int selT = ctx->selection.top, selB = ctx->selection.bottom;
            int ax = (std::max)(selL, (std::min)(ctx->mouseX, selR));
            int ay = (std::max)(selT, (std::min)(ctx->mouseY, selB));
            if (ctx->curDrawing.type == AT_Brush) {
                POINT p = { ax, ay };
                ctx->curDrawing.pts.push_back(p);
            } else if (ctx->curDrawing.type == AT_Mosaic && !ctx->curDrawing.mosaicRect) {
                // 马赛克涂抹模式：记录路径点。揭示由 WM_PAINT 统一处理（reveal-mask 模型，
                // 每帧从预计算的 base 揭示蒙版区域，无需增量绘制）。
                POINT p = { ax, ay };
                ctx->curDrawing.pts.push_back(p);
            } else {
                ctx->curDrawing.x2 = ax;
                ctx->curDrawing.y2 = ay;
            }
            // 局部刷新：上帧 curDrawing 包围盒 ∪ 本帧鼠标点附近（标注都在选区内）。
            // 坐标转 backDC（减 virtualX/Y）。lastDrawingBox 由 WM_PAINT 每帧更新。
            RECT mouseBox = { ax - ctx->virtualX - 8, ay - ctx->virtualY - 8,
                              ax - ctx->virtualX + 8, ay - ctx->virtualY + 8 };
            RECT drawDirty;
            if (ctx->hasLastDrawingBox) {
                RECT ldb = { ctx->lastDrawingBox.left - ctx->virtualX,
                             ctx->lastDrawingBox.top - ctx->virtualY,
                             ctx->lastDrawingBox.right - ctx->virtualX,
                             ctx->lastDrawingBox.bottom - ctx->virtualY };
                drawDirty = InflateRectBy(UnionRectSafe(ldb, mouseBox), 4);
            } else {
                drawDirty = InflateRectBy(mouseBox, 4);
            }
            InvalidateRect(hwnd, &drawDirty, FALSE);
        }
    } else if (ctx->state == CS_TextEditing) {
        // 文字编辑态：拖动选择文字
        if (ctx->textDraggingSelection) {
            int mxRel = ctx->mouseX - ctx->virtualX;
            int textX = ctx->textAnchorX - ctx->virtualX;
            int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];
            int caretPos = CalcCaretPosFromMouse(ctx->backDC, ctx->textBuf, fontPx, textX, mxRel);

            ctx->textSelEnd = caretPos;
            ctx->textCaretPos = caretPos;
            InvalidateTextLine(hwnd, ctx);
        }
    } else if (ctx->resizingAnnotation >= 0) {
        // 非文字标注缩放：四角手柄走包围盒变换；箭头端点手柄仅平移单个端点
        int idx = ctx->resizingAnnotation;
        int dx = ctx->mouseX - ctx->annotationDragStartX;
        int dy = ctx->mouseY - ctx->annotationDragStartY;
        if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
            PushAnnotationHistory(ctx);
            ctx->annotationOpHistoryPushed = true;
        }
        // 从按下时快照还原再变换，避免累积浮点误差
        ctx->annotations[idx] = ctx->dragStartAnnotation;
        Annotation& a = ctx->annotations[idx];
        if (ctx->annotationResizeHandle == RH_ArrowStart
            || ctx->annotationResizeHandle == RH_ArrowEnd) {
            // 箭头端点拖拽：仅移动对应端点，另一端点保持快照值不变
            if (ctx->annotationResizeHandle == RH_ArrowStart) {
                a.x1 = ctx->dragStartAnnotation.x1 + dx;
                a.y1 = ctx->dragStartAnnotation.y1 + dy;
            } else {
                a.x2 = ctx->dragStartAnnotation.x2 + dx;
                a.y2 = ctx->dragStartAnnotation.y2 + dy;
            }
        } else {
            // 包围盒缩放：4 角 + 4 边中点手柄，根据拖拽手柄更新包围盒，
            // 再按包围盒变换映射标注坐标（矩形/圆均支持 8 手柄）。
            const RECT& o = ctx->annotationResizeStartBox;
            RECT n = o;
            switch (ctx->annotationResizeHandle) {
                case RH_TopLeft:     n.left = o.left + dx; n.top = o.top + dy; break;
                case RH_TopRight:    n.right = o.right + dx; n.top = o.top + dy; break;
                case RH_BottomLeft:  n.left = o.left + dx; n.bottom = o.bottom + dy; break;
                case RH_BottomRight: n.right = o.right + dx; n.bottom = o.bottom + dy; break;
                case RH_Left:        n.left = o.left + dx; break;
                case RH_Right:       n.right = o.right + dx; break;
                case RH_Top:         n.top = o.top + dy; break;
                case RH_Bottom:      n.bottom = o.bottom + dy; break;
            }
            // 防翻转：保证 right>left、bottom>top（至少 1px）
            n = NormalizeRect(n);
            if (n.right - n.left < 2) n.right = n.left + 2;
            if (n.bottom - n.top < 2) n.bottom = n.top + 2;
            TransformAnnotationByBox(a, o, n);
        }
        InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC));
    } else if (ctx->draggingAnnotation >= 0) {
        // 非文字标注整体拖拽：对按下时快照做 dx/dy 平移后写回（避免累积误差）
        int idx = ctx->draggingAnnotation;
        int dx = ctx->mouseX - ctx->annotationDragStartX;
        int dy = ctx->mouseY - ctx->annotationDragStartY;
        if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
            PushAnnotationHistory(ctx);
            ctx->annotationOpHistoryPushed = true;
        }
        ctx->annotations[idx] = ctx->dragStartAnnotation;
        Annotation& a = ctx->annotations[idx];
        switch (a.type) {
            case AT_Rect:
            case AT_Circle:
            case AT_Arrow:
            case AT_Mosaic:
                if (a.type == AT_Mosaic && !a.mosaicRect) {
                    for (POINT& p : a.pts) { p.x += dx; p.y += dy; }
                } else {
                    a.x1 += dx; a.y1 += dy; a.x2 += dx; a.y2 += dy;
                }
                break;
            case AT_Brush:
                for (POINT& p : a.pts) { p.x += dx; p.y += dy; }
                break;
            case AT_Text:
                a.x1 += dx; a.y1 += dy;
                break;
        }
        InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC));
    } else if (ctx->draggingTextAnnotation >= 0) {
        // 拖动文字标注位置
        int dx = ctx->mouseX - ctx->textDragStartX;
        int dy = ctx->mouseY - ctx->textDragStartY;
        if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
            PushAnnotationHistory(ctx);
            ctx->annotationOpHistoryPushed = true;
        }
        ctx->annotations[ctx->draggingTextAnnotation].x1 = ctx->dragStartX + dx;
        ctx->annotations[ctx->draggingTextAnnotation].y1 = ctx->dragStartY + dy;
        InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[ctx->draggingTextAnnotation], ctx->backDC));
    } else if (ctx->state == CS_Confirmed) {
        // hover 手柄/工具栏/文字/非文字标注变化需重绘以更新光标提示与高亮
        int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection, ctx->handleMetrics.handleSize);
        int mxRel = ctx->mouseX - ctx->virtualX;
        int myRel = ctx->mouseY - ctx->virtualY;
        int tb = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);
        int ht = HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
        // 非文字标注 hover：优先用选中项的手柄命中（箭头=端点；矩形/圆=8 手柄；画笔=无），否则普通命中
        int ha = -1;
        if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
            Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
            bool onHandle = (HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY,
                ctx->backDC, ctx->handleMetrics.handleSize) != RH_None);
            if (onHandle) {
                ha = ctx->selectedAnnotation;  // 悬停在手柄上视为悬停选中项（光标由 SETCURSOR 处理）
            }
        }
        if (ha < 0) {
            ha = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
        }
        // 倒角手柄"靠近"判定：感应区比命中框大一圈（handleSize + cornerProximity），
        // 鼠标靠近某角即显示该角手柄；选区四角互不相邻，取最近的一个。
        int newNear = FindNearestCornerRadiusHandle(ctx->mouseX, ctx->mouseY, ctx->selection,
            ctx->handleMetrics.handleSize, ctx->handleMetrics.cornerKnobInset,
            ctx->selectionCornerRadius, ctx->handleMetrics.cornerProximity);
        // 仅当 hover 状态真正变化时才重绘（去掉纯 moved 无变化的重绘，减少无意义全屏帧）。
        // 脏区域 = 各变化项的旧位置 ∪ 新位置（工具栏/标注边框高亮/倒角手柄变化）。
        if (h != ctx->resizeHandle || tb != ctx->hoverToolbarBtn
            || ht != ctx->hoveredTextAnnotation || ha != ctx->hoveredAnnotation
            || newNear != ctx->hoveredCornerHandle) {
            RECT dirty = {0,0,0,0};
            // 工具栏 hover 变化：整条工具栏（按钮高亮）
            if (tb != ctx->hoverToolbarBtn) {
                dirty = UnionRectSafe(dirty, InflateRectBy(ctx->toolbarRect, 2));
            }
            // 文字标注 hover 变化：旧 ∪ 新 标注盒（backDC 坐标）
            if (ht != ctx->hoveredTextAnnotation) {
                if (ctx->hoveredTextAnnotation >= 0 && ctx->hoveredTextAnnotation < (int)ctx->annotations.size()) {
                    RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ctx->hoveredTextAnnotation]);
                    r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                    r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                    dirty = UnionRectSafe(dirty, InflateRectBy(r, 3));
                }
                if (ht >= 0 && ht < (int)ctx->annotations.size()) {
                    RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ht]);
                    r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                    r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                    dirty = UnionRectSafe(dirty, InflateRectBy(r, 3));
                }
            }
            // 非文字标注 hover 变化：旧 ∪ 新 标注盒
            // 选中态标注在 hover 进入/离开时会显示/隐藏 resize 手柄，手柄贴在包围盒边缘外，
            // inflate 量需覆盖手柄半径（handleSize/2 + 余量 = handleMetrics.handleMargin），否则手柄痕迹残留。
            if (ha != ctx->hoveredAnnotation) {
                const int handleMargin = ctx->handleMetrics.handleMargin;
                if (ctx->hoveredAnnotation >= 0 && ctx->hoveredAnnotation < (int)ctx->annotations.size()) {
                    RECT r = MeasureAnnotationBounds(ctx->annotations[ctx->hoveredAnnotation], ctx->backDC);
                    r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                    r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                    dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
                }
                if (ha >= 0 && ha < (int)ctx->annotations.size()) {
                    RECT r = MeasureAnnotationBounds(ctx->annotations[ha], ctx->backDC);
                    r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                    r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                    dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
                }
            }
            // 倒角手柄靠近变化：旧 ∪ 新角手柄位置重绘（出现/消失/切换角）。
            if (newNear != ctx->hoveredCornerHandle) {
                if (IsCornerRadiusHandle(ctx->hoveredCornerHandle)) {
                    dirty = UnionRectSafe(dirty, CornerHandleDirtyRect(ctx, ctx->hoveredCornerHandle));
                }
                if (IsCornerRadiusHandle(newNear)) {
                    dirty = UnionRectSafe(dirty, CornerHandleDirtyRect(ctx, newNear));
                }
            }
            ctx->resizeHandle = h;
            ctx->hoverToolbarBtn = tb;
            ctx->hoveredTextAnnotation = ht;
            ctx->hoveredAnnotation = ha;
            ctx->hoveredCornerHandle = newNear;
            if (IsValidRect(dirty)) {
                InvalidateRect(hwnd, &dirty, FALSE);
            } else {
                // 兜底（理论上不会到这里）
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
    }
    return 0;
}

LRESULT OnLButtonUp(HWND hwnd, CaptureContext* ctx) {
    // 工具栏拖拽结束：位置已在 MOUSEMOVE 固化（toolbarPlaced=true），
    // 只退出拖拽态，不落入任何状态分支
    if (ctx->toolbarDragging) {
        ctx->toolbarDragging = false;
        return 0;
    }
    if (ctx->state == CS_Selecting) {
        int w = abs(ctx->endX - ctx->startX);
        int h = abs(ctx->endY - ctx->startY);

        RECT finalRect;
        if (w <= 1 && h <= 1) {
            // 点击 -> 使用悬停窗口矩形
            int idx = FindWindowAtPoint(ctx->windows, ctx->mouseX, ctx->mouseY);
            if (idx >= 0) {
                finalRect = ctx->windows[idx].rect;
            } else {
                // 匹配不到窗口时，默认选区为鼠标所在的屏幕
                POINT pt = { ctx->mouseX, ctx->mouseY };
                HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                if (hMonitor) {
                    MONITORINFO monitorInfo;
                    monitorInfo.cbSize = sizeof(MONITORINFO);
                    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                        finalRect = monitorInfo.rcMonitor;
                    } else {
                        // 获取失败，降级为虚拟屏幕
                        finalRect = { ctx->virtualX, ctx->virtualY,
                                      ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH };
                    }
                } else {
                    // 获取显示器失败，降级为虚拟屏幕
                    finalRect = { ctx->virtualX, ctx->virtualY,
                                  ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH };
                }
            }
        } else {
            finalRect.left = (std::min)(ctx->startX, ctx->endX);
            finalRect.top = (std::min)(ctx->startY, ctx->endY);
            finalRect.right = (std::max)(ctx->startX, ctx->endX);
            finalRect.bottom = (std::max)(ctx->startY, ctx->endY);
        }

        // 进入确认态（可调整/拖动/工具栏），而非直接完成
        EnterConfirmed(ctx, finalRect);
        // 自动确认模式：跳过编辑态，直接提取选区并完成截图
        if (ctx->autoConfirm) {
            ScreenshotResult* result = ExtractRegionResult(ctx->memDC, finalRect,
                ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations, ctx->selectionCornerRadius,
                ctx->mosaicSizeIdx);
            // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放 result 防泄漏。
            EmitScreenshotResult(result->success, result->x, result->y, result->x2, result->y2,
                                 result->width, result->height, result->base64);
            delete result;
            ctx->state = CS_Done;
            DestroyWindow(hwnd);
            return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (ctx->state == CS_Resizing) {
        if (IsCornerRadiusHandle(ctx->resizeHandle)) {
            // 圆角调整结束：钳制半径即足够（选区矩形未变），无需最小尺寸逻辑。
            ClampCornerRadius(ctx);
            ctx->resizeHandle = RH_None;
            // 松手后重算靠近角：鼠标仍在该角附近则保持显示，否则回 RH_None 由 MOUSEMOVE 再探测。
            ctx->hoveredCornerHandle = FindNearestCornerRadiusHandle(ctx->mouseX, ctx->mouseY,
                ctx->selection, ctx->handleMetrics.handleSize, ctx->handleMetrics.cornerKnobInset,
                ctx->selectionCornerRadius, ctx->handleMetrics.cornerProximity);
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
        // resize 结束：从按下快照和最终鼠标位置重算，并只沿活动端当前所在侧补足最小尺寸。
        // 不能复用 EnterConfirmed 的固定向右/下扩张，否则穿越后会移动按下时的固定点。
        // 叠加键盘微调位移(kbDX/kbDY)，使其在松开时一并固化到最终选区。
        int dx = (ctx->mouseX - ctx->dragStartX) + ctx->kbDX;
        int dy = (ctx->mouseY - ctx->dragStartY) + ctx->kbDY;
        RECT virtualBounds = {
            ctx->virtualX, ctx->virtualY,
            ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH
        };
        RECT contentBounds = {0, 0, 0, 0};
        bool hasContent = CalcAnnotationsBounds(ctx->annotations, contentBounds, ctx->backDC);
        ctx->selection = ResizeSelectionFromHandle(
            ctx->dragStartSelection, ctx->resizeHandle, dx, dy, virtualBounds,
            hasContent, contentBounds, true);
        ctx->kbDX = 0;
        ctx->kbDY = 0;
        ctx->resizeHandle = RH_None;
        ctx->hoveredCornerHandle = RH_None;
        ctx->state = CS_Confirmed;
        ctx->needFullRedraw = true;
        ClampCornerRadius(ctx);  // 选区尺寸变化后，半径可能越界，钳制
        InvalidateRect(hwnd, NULL, FALSE);
        }
    } else if (ctx->state == CS_Moving) {
        // 整体拖动结束仍可走通用确认流程；移动不会改变 resize 固定点语义。
        EnterConfirmed(ctx, ctx->selection);
        InvalidateRect(hwnd, NULL, FALSE);
    } else if (ctx->state == CS_Drawing) {
        // 绘制结束 -> 提交标注（仅在有有效尺寸时）
        bool valid = false;
        if (ctx->hasCurDrawing) {
            if (ctx->curDrawing.type == AT_Brush) {
                valid = ctx->curDrawing.pts.size() >= 2;
            } else if (ctx->curDrawing.type == AT_Mosaic) {
                if (ctx->curDrawing.mosaicRect) {
                    // 框选模式：需要有效矩形尺寸
                    valid = (abs(ctx->curDrawing.x2 - ctx->curDrawing.x1) >= 2
                          || abs(ctx->curDrawing.y2 - ctx->curDrawing.y1) >= 2);
                } else {
                    // 涂抹模式：至少 1 个点（单击也能产生一个马赛克圆）
                    valid = ctx->curDrawing.pts.size() >= 1;
                }
            } else {
                valid = (abs(ctx->curDrawing.x2 - ctx->curDrawing.x1) >= 2
                      || abs(ctx->curDrawing.y2 - ctx->curDrawing.y1) >= 2);
            }
        }
        if (valid) {
            PushAnnotationHistory(ctx);
            ctx->annotations.push_back(ctx->curDrawing);
            // reveal-mask 模型：base 与标注无关，下一帧 WM_PAINT 自动把新蒙版揭示出来。
        }
        ctx->hasCurDrawing = false;
        ctx->curDrawing = {};
        ctx->mosaicDrawLastIdx = 0;
        ctx->state = CS_Confirmed;
        ctx->needFullRedraw = true;
    } else if (ctx->state == CS_TextEditing) {
        // 文字选择结束
        if (ctx->textDraggingSelection) {
            ctx->textDraggingSelection = false;
            // 如果选择范围相同，清除选择
            if (ctx->textSelStart == ctx->textSelEnd) {
                ctx->textSelStart = -1;
                ctx->textSelEnd = -1;
            }
        }
    } else if (ctx->resizingAnnotation >= 0) {
        // 非文字标注缩放结束：先清缩放标志，再刷新最后位置与上帧位置的并集。
        // WM_PAINT 将按最终确认态重画选中手柄，避免沿用拖拽态光标/外观。
        int idx = ctx->resizingAnnotation;
        RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
        ctx->resizingAnnotation = -1;
        ctx->annotationResizeHandle = RH_None;
        ctx->annotationOpHistoryPushed = false;
        InvalidateAnnotationOp(hwnd, ctx, finalBox);
    } else if (ctx->draggingAnnotation >= 0) {
        // 非文字标注拖拽结束：先退出拖拽态再补刷最终脏区，不退化为全屏重绘。
        int idx = ctx->draggingAnnotation;
        RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
        ctx->draggingAnnotation = -1;
        ctx->annotationOpHistoryPushed = false;
        InvalidateAnnotationOp(hwnd, ctx, finalBox);
    } else if (ctx->draggingTextAnnotation >= 0) {
        // 文字拖动结束：先退出拖动态，再按最终文字边框区域完成局部刷新。
        int idx = ctx->draggingTextAnnotation;
        RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
        ctx->draggingTextAnnotation = -1;
        ctx->annotationOpHistoryPushed = false;
        InvalidateAnnotationOp(hwnd, ctx, finalBox);
    }
    return 0;
}

LRESULT OnKeyDown(HWND hwnd, WPARAM wParam, CaptureContext* ctx) {
    // 方向键微调选区：调整手柄拖拽中(CS_Resizing)微调活动边，或确认态整体平移。
    // 文字编辑态(CS_TextEditing)不拦截，走下方原有光标逻辑。
    if ((ctx->state == CS_Resizing || ctx->state == CS_Confirmed)
        && (wParam == VK_LEFT || wParam == VK_RIGHT
            || wParam == VK_UP || wParam == VK_DOWN)
        && HandleSelectionNudgeKey(hwnd, ctx, wParam)) {
        return 0;
    }
    if (wParam == VK_ESCAPE) {
        // 文字编辑态：ESC 取消输入，回到确认态
        if (ctx->state == CS_TextEditing) {
            ctx->textBuf.clear();
            ctx->textCaretPos = 0;
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        // 其它状态：ESC 取消截图
        ctx->state = CS_Cancelled;
        // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放防泄漏。
        EmitScreenshotResult(false);
        DestroyWindow(hwnd);
    } else if (wParam == VK_RETURN) {
        // 文字编辑态：Enter 提交文字
        if (ctx->state == CS_TextEditing) {
            // 历史上 Enter 路径不清 textSelStart/End（仅清缓冲与光标），保持原清理序列
            CommitPendingText(ctx);
            ctx->textBuf.clear();
            ctx->textCaretPos = 0;
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        // 确认态：Enter 确认截图
        if (ctx->state == CS_Confirmed) {
            ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations, ctx->selectionCornerRadius,
                ctx->mosaicSizeIdx);
            // 统一走 EmitScreenshotResult：TSFN 空 / nonblocking 失败均自动释放 result 防泄漏。
            EmitScreenshotResult(result->success, result->x, result->y, result->x2, result->y2,
                                 result->width, result->height, result->base64);
            delete result;
            ctx->state = CS_Done;
            DestroyWindow(hwnd);
        }
    } else if (wParam == VK_BACK) {
        // 文字编辑态：退格删除（文字内容变化，整行重排，刷新文字行区域）
        if (ctx->state == CS_TextEditing && ctx->textCaretPos > 0) {
            ctx->textBuf.erase(ctx->textCaretPos - 1, 1);
            ctx->textCaretPos--;
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
    } else if (wParam == VK_DELETE) {
        // 文字编辑态：Delete 删除
        if (ctx->state == CS_TextEditing && ctx->textCaretPos < (int)ctx->textBuf.size()) {
            ctx->textBuf.erase(ctx->textCaretPos, 1);
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
        // 确认态：Delete 删除当前选中的覆盖物
        if (ctx->state == CS_Confirmed) {
            if (ctx->selectedTextAnnotation >= 0
                && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                PushAnnotationHistory(ctx);
                ctx->annotations.erase(ctx->annotations.begin() + ctx->selectedTextAnnotation);
                ctx->selectedTextAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                ctx->draggingTextAnnotation = -1;
                ctx->activeTool = TB_Drag;
                ctx->popupTool = -1;
                ctx->popupOpen = false;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (ctx->selectedAnnotation >= 0
                && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                PushAnnotationHistory(ctx);
                ctx->annotations.erase(ctx->annotations.begin() + ctx->selectedAnnotation);
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->draggingAnnotation = -1;
                ctx->resizingAnnotation = -1;
                ctx->annotationResizeHandle = RH_None;
                ctx->activeTool = TB_Drag;
                ctx->popupTool = -1;
                ctx->popupOpen = false;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
        }
    } else if (wParam == VK_LEFT) {
        // 文字编辑态：左箭头移动光标（仅光标位置变化）
        if (ctx->state == CS_TextEditing && ctx->textCaretPos > 0) {
            ctx->textCaretPos--;
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
    } else if (wParam == VK_RIGHT) {
        // 文字编辑态：右箭头移动光标
        if (ctx->state == CS_TextEditing && ctx->textCaretPos < (int)ctx->textBuf.size()) {
            ctx->textCaretPos++;
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
    } else if (wParam == VK_HOME) {
        // 文字编辑态：Home 移动到行首
        if (ctx->state == CS_TextEditing) {
            ctx->textCaretPos = 0;
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
    } else if (wParam == VK_END) {
        // 文字编辑态：End 移动到行尾
        if (ctx->state == CS_TextEditing) {
            ctx->textCaretPos = (int)ctx->textBuf.size();
            InvalidateTextLine(hwnd, ctx);
            return 0;
        }
    }
    return 0;
}

LRESULT OnImeComposition(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx) {
    // 处理中文输入法（IME）输入
    if (ctx->state == CS_TextEditing) {
        if (lParam & GCS_RESULTSTR) {
            HIMC hIMC = ImmGetContext(hwnd);
            if (hIMC) {
                // 获取输入法完成的字符串长度
                LONG len = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, NULL, 0);
                if (len > 0) {
                    // 分配缓冲区并获取字符串
                    std::wstring result(len / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, &result[0], len);
                    result.resize(len / sizeof(wchar_t));

                    // 插入到当前光标位置
                    ctx->textBuf.insert(ctx->textCaretPos, result);
                    ctx->textCaretPos += (int)result.size();
                    InvalidateTextLine(hwnd, ctx);
                }
                ImmReleaseContext(hwnd, hIMC);
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT OnChar(HWND hwnd, WPARAM wParam, CaptureContext* ctx) {
    // 文字编辑态：接收字符输入（ASCII 和直接输入）
    if (ctx->state == CS_TextEditing) {
        wchar_t ch = (wchar_t)wParam;
        // 过滤控制字符（除了可打印字符）
        // 忽略 IME 相关的控制字符
        if (ch >= 32 && ch != 127) {
            ctx->textBuf.insert(ctx->textCaretPos, 1, ch);
            ctx->textCaretPos++;
            InvalidateTextLine(hwnd, ctx);
        }
        return 0;
    }
    return 0;
}

LRESULT OnSetCursor(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx) {
    // 长截图：统一箭头（遮罩上无交互；洞内光标由底层应用自行控制）
    if (ctx->state == CS_LongCapturing) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
        return TRUE;
    }
    // 工具栏拖拽中（按住 6 点把手）：全程四向箭头
    if (ctx->toolbarDragging) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
        return TRUE;
    }
    // 马赛克涂抹模式（确认态/绘制态）：用预生成的半径圆光标（OS 跟随，无重绘延迟）
    if (ctx->activeTool == TB_Mosaic && !ctx->mosaicRectMode
        && (ctx->state == CS_Confirmed || ctx->state == CS_Drawing)
        && ctx->mosaicBrushCursorsInited
        && ctx->mosaicRadiusIdx >= 0 && ctx->mosaicRadiusIdx < SC_MOSAIC_RADIUS_COUNT
        && ctx->mosaicBrushCursors[ctx->mosaicRadiusIdx]) {
        // 工具栏/子菜单上仍用箭头/手型
        int mxRel = ctx->mouseX - ctx->virtualX;
        int myRel = ctx->mouseY - ctx->virtualY;
        if (PointInRect(mxRel, myRel, ctx->toolbarRect)) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        if (ctx->popupOpen && PointInRect(mxRel, myRel, ctx->popupRect)) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            SetCursor(ctx->mosaicBrushCursors[ctx->mosaicRadiusIdx]);
            return TRUE;
        }
    }
    // 文字编辑中：I-beam 光标
    if (ctx->state == CS_TextEditing) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM));
        return TRUE;
    }
    // 绘制中：十字光标
    if (ctx->state == CS_Drawing) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
        return TRUE;
    }
    // 非文字标注缩放中：对应四角 resize 光标
    if (ctx->resizingAnnotation >= 0) {
        SetCursor(LoadCursorW(NULL, HandleCursor(ctx->annotationResizeHandle)));
        return TRUE;
    }
    // 非文字标注拖拽中：四向箭头光标
    if (ctx->draggingAnnotation >= 0) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
        return TRUE;
    }
    // 拖动文字标注中：四向箭头光标
    if (ctx->draggingTextAnnotation >= 0) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
        return TRUE;
    }
    // 确认态：根据 hover 位置切换 resize/move/箭头/十字光标
    if (ctx->state == CS_Confirmed) {
        // 工具栏或子菜单 -> 箭头（把手上为四向箭头，提示可拖动工具栏）
        int mxRel = ctx->mouseX - ctx->virtualX;
        int myRel = ctx->mouseY - ctx->virtualY;
        if (PointInRect(mxRel, myRel, ctx->toolbarRect)) {
            if (HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics) == SC_TB_GRIP) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            } else {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            }
            return TRUE;
        }
        if (ctx->popupOpen && PointInRect(mxRel, myRel, ctx->popupRect)) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        // 手柄 -> 对应 resize 光标
        int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection, ctx->handleMetrics.handleSize);
        if (h != RH_None) {
            SetCursor(LoadCursorW(NULL, HandleCursor(h)));
            return TRUE;
        }
        // 圆角手柄 -> 对应对角 resize 光标（按所在角）
        int cornerH = HitTestCornerRadiusHandle(ctx->mouseX, ctx->mouseY, ctx->selection,
            ctx->handleMetrics.handleSize, ctx->handleMetrics.cornerKnobInset, ctx->selectionCornerRadius);
        if (cornerH != RH_None) {
            SetCursor(LoadCursorW(NULL, HandleCursor(cornerH)));
            return TRUE;
        }
        // 已选中的非文字标注手柄 -> 对应 resize 光标（缩放入入口）
        //   箭头=起点/终点端点手柄（固定四向箭头）；矩形/圆=8 手柄（方向自适应）；画笔=无
        // 独立命中测试，不依赖 hovered 缓存（RDP 节流下缓存会滞后）
        if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
            && ctx->annotations[ctx->selectedAnnotation].type != AT_Text) {
            Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
            int handle = HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY, ctx->backDC, ctx->handleMetrics.handleSize);
            if (handle != RH_None) {
                SetCursor(LoadCursorW(NULL, HandleCursor(handle)));
                return TRUE;
            }
        }
        // 非文字标注悬停 -> 四向箭头（可拖动/选中），与下方文字悬停判定并列
        {
            int hit = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
            if (hit >= 0 && ctx->annotations[hit].type != AT_Text) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
                return TRUE;
            }
        }
        // 文字标注悬停 -> 四向箭头（可拖动/选中）
        // 注意：此处必须独立做命中测试，不能依赖 ctx->hoveredTextAnnotation。
        // 在远程桌面（RDP）下鼠标移动事件常被节流/合并，WM_MOUSEMOVE 更新
        // hoveredTextAnnotation 存在滞后，导致 WM_SETCURSOR 看到过期值。
        // 文字工具未激活时，悬停已确认文字 -> 拖动光标；
        // 文字工具激活时，悬停已确认文字 -> 仍为拖动光标（可选中改属性）。
        if (HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC) >= 0) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            return TRUE;
        }
        // 选区内部：
        //   矢量/文字/马赛克工具激活 -> 十字；
        //   已有标注内容 -> 箭头（禁止整体拖动）；
        //   否则 -> 移动光标
        if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            if (IsVectorTool(ctx->activeTool) || ctx->activeTool == TB_Text
                || ctx->activeTool == TB_Mosaic) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
            } else if (!ctx->annotations.empty()) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            } else {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            }
            return TRUE;
        }
        // 选区外 -> 箭头
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
        return TRUE;
    }
    if (ctx->state == CS_Resizing) {
        SetCursor(LoadCursorW(NULL, HandleCursor(ctx->resizeHandle)));
        return TRUE;
    }
    if (ctx->state == CS_Moving) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
        return TRUE;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
