// 截图模块：导出管线（PNG 编码、Base64、剪贴板、保存对话框）
#include "internal.h"
#include <commdlg.h>   // GetSaveFileNameW 保存对话框（PromptSaveFilePath 唯一使用方）
#include <shlobj.h>    // SHGetKnownFolderPath 已知文件夹路径（默认保存目录兜底，PromptSaveFilePath 唯一使用方）
#include <memory>   // std::unique_ptr（GetPngEncoderClsid 编码器表分配）
#include <cwchar>   // std::wcscmp（GetPngEncoderClsid 匹配 PNG MIME）

// Base64 编码表

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 编码

static std::string Base64Encode(const BYTE* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (data[i] << 16) | ((i + 1 < len ? data[i + 1] : 0) << 8) | (i + 2 < len ? data[i + 2] : 0);
        result.push_back(base64_chars[(b >> 18) & 0x3F]);
        result.push_back(base64_chars[(b >> 12) & 0x3F]);
        result.push_back(i + 1 < len ? base64_chars[(b >> 6) & 0x3F] : '=');
        result.push_back(i + 2 < len ? base64_chars[b & 0x3F] : '=');
    }
    return result;
}

// 取流的真实已写字节长度。
// CreateStreamOnHGlobal 的底层 HGLOBAL 按堆粒度分配，GlobalSize 返回的是分配容量而非
// PNG 实际写入的字节数：尾部长度虚高会进入 base64 编码、落盘与剪贴板 PNG 输出
// （体积膨胀、严格消费方读取出错），因此统一经 IStream::Stat 取逻辑长度。
// STATFLAG_NONAME 免去名字拷贝，st.pwcsName 为空无需 CoTaskMemFree；
// 失败返回 0（调用方按 len==0 走既有空数据分支）。须在 stream->Release() 之前调用。

static size_t GetStreamLength(IStream* stream) {
    STATSTG st = {};
    if (!stream || FAILED(stream->Stat(&st, STATFLAG_NONAME))) return 0;
    return (size_t)st.cbSize.QuadPart;
}

// 获取 PNG 编码器 CLSID（从 binding_windows.cpp 迁入截图目录）。
// 进程内 PNG 编码器 CLSID 不变，首次查找后用 static 缓存，后续直接返回缓存值，
// 避免每次编码都分配并遍历编码器表。返回值 >= 0 表示成功（返回的是匹配到的编码器索引）。
// 仍返回 int 索引以保持原有调用契约（调用方判 >= 0 / == -1）；CLSID 经 pClsid 带出。
// 须在 GDI+ 已初始化（截图会话级 InitGdipResources 或调用方局部 GdiPlusInit）后调用。
// 线程安全：截图捕获线程与 binding 的 IconWorker 线程可能并发调用，用 std::call_once
// 保证缓存查找与写入只发生一次，后续读缓存为只读（CLSID/索引在进程内不变，无数据竞争）。

int GetPngEncoderClsid(CLSID* pClsid) {
    static CLSID s_clsid = {};
    static int s_index = -1;
    static std::once_flag s_once;
    std::call_once(s_once, [&]() {
        UINT num = 0u;
        UINT size = 0u;
        Gdiplus::GetImageEncodersSize(std::addressof(num), std::addressof(size));
        if (size == 0u) { s_index = -1; return; }

        std::unique_ptr<Gdiplus::ImageCodecInfo> pImageCodecInfo(
            static_cast<Gdiplus::ImageCodecInfo*>(static_cast<void*>(new BYTE[size])));
        if (pImageCodecInfo == nullptr) { s_index = -1; return; }

        Gdiplus::GetImageEncoders(num, size, pImageCodecInfo.get());

        for (UINT i = 0u; i < num; i++) {
            if (std::wcscmp(pImageCodecInfo.get()[i].MimeType, L"image/png") == 0) {
                s_clsid = pImageCodecInfo.get()[i].Clsid;
                s_index = (int)i;
                break;
            }
        }
    });
    if (pClsid) *pClsid = s_clsid;
    return s_index;
}

// 将 HALFTONE 缩放模式设置到目标 DC（output_windows.cpp / long_capture_windows.cpp 共用）。
// HALFTONE 在做下采样缩放时比默认 COLORONCOLOR 质量更好，但 BrushOrg 会被 StretchBlt 用到，
// 因此同时把画刷原点复位到 (0,0) 避免抖动（MSDN 推荐配套调用）。

void SetHalftoneStretchMode(HDC dc) {
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
}

// 将 HBITMAP 转换为 PNG base64 字符串

std::string BitmapToBase64Png(HBITMAP hBitmap) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
    std::string result;
    {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hBitmap, NULL);
        if (bmp) {
            CLSID pngClsid;
            if (GetPngEncoderClsid(&pngClsid) >= 0) {
                IStream* stream = NULL;
                CreateStreamOnHGlobal(NULL, TRUE, &stream);
                if (stream && bmp->Save(stream, &pngClsid, NULL) == Gdiplus::Ok) {
                    // 真实长度取自流（须在 Release 前）；GlobalSize 只是堆分配容量
                    size_t len = GetStreamLength(stream);
                    HGLOBAL hMem = NULL;
                    GetHGlobalFromStream(stream, &hMem);
                    BYTE* ptr = (BYTE*)GlobalLock(hMem);
                    if (ptr && len > 0) {
                        result = "data:image/png;base64," + Base64Encode(ptr, len);
                    }
                    GlobalUnlock(hMem);
                }
                if (stream) stream->Release();
            }
            delete bmp;
        }
    }
    return result;
}

// 打开剪贴板（供本文件两条剪贴板导出路径共用，保持开门阶段语义一致）。
// 输入法/剪贴板管理器会高频短暂占用剪贴板使 OpenClipboard 瞬时失败，
// 因此失败后以 Sleep(10) 为间隔重试，总等待约 300ms 后仍失败才放弃。
// 返回 true 表示已持有剪贴板（此后必须以 CloseClipboard 归还）。

static bool OpenClipboardWithRetry() {
    const int kMaxRetries = 30;
    for (int i = 0;; ++i) {
        if (OpenClipboard(NULL)) return true;
        if (i >= kMaxRetries) return false;
        Sleep(10);
    }
}

// 把不透明位图放入剪贴板（CF_BITMAP）。
// 成功 SetClipboardData 后 hCopy 由系统接管（不得再 DeleteObject）；
// 失败或 CopyImage 失败时须自行删除副本并返回 false —— 剪贴板此时已被清空，
// 不能像传 NULL 那样注册延迟渲染，也不得向上游谎报成功。

bool SaveBitmapToClipboard(HBITMAP hBitmap) {
    if (!OpenClipboardWithRetry()) return false;
    EmptyClipboard();
    BITMAP bm;
    GetObject(hBitmap, sizeof(BITMAP), &bm);
    HBITMAP hCopy = (HBITMAP)CopyImage(hBitmap, IMAGE_BITMAP, bm.bmWidth, bm.bmHeight, LR_COPYRETURNORG);
    bool ok = false;
    if (hCopy) {
        ok = NULL != SetClipboardData(CF_BITMAP, hCopy);
        if (!ok) DeleteObject(hCopy);  // 未被系统接管，自行删除避免泄漏
    }
    CloseClipboard();
    return ok;
}

// 单次编码同时满足多个输出需求（base64 / 原始字节 / 落盘），
// 避免长截图这类超大位图被 GDI+ 反复编码。任一输出成功即返回 true。
bool EncodeHBitmapPng(HBITMAP hBitmap, std::string* base64Out, std::string* rawOut,
                      const wchar_t* filePath) {
    if (!hBitmap) return false;
    bool ok = false;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hBitmap, NULL);
    if (bmp) {
        CLSID pngClsid;
        if (GetPngEncoderClsid(&pngClsid) >= 0) {
            IStream* stream = NULL;
            if (CreateStreamOnHGlobal(NULL, TRUE, &stream) == S_OK && stream) {
                if (bmp->Save(stream, &pngClsid, NULL) == Gdiplus::Ok) {
                    HGLOBAL hMem = NULL;
                    if (GetHGlobalFromStream(stream, &hMem) == S_OK && hMem) {
                        // 真实长度取自流（须在 Release 前）；GlobalSize 只是堆分配容量
                        size_t len = GetStreamLength(stream);
                        BYTE* ptr = (BYTE*)GlobalLock(hMem);
                        if (ptr && len > 0) {
                            if (rawOut) rawOut->assign((const char*)ptr, len);
                            if (base64Out) *base64Out = "data:image/png;base64," + Base64Encode(ptr, len);
                            ok = true;
                            if (filePath && *filePath) {
                                // 原子落盘：先写同目录临时文件（与目标同目录保证同一卷，
                                // MoveFileEx 才能做元数据级原子替换），写入全部成功且句柄
                                // 关闭后再以 MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH
                                // 一次性替换目标文件。避免磁盘满/权限中断时旧图已被
                                // CREATE_ALWAYS 覆盖、只留截断 PNG 的情况。
                                // 任一步失败都删除临时文件并保持 ok=false 错误信号。
                                std::wstring tmpPath = std::wstring(filePath) + L".tmp";
                                bool saved = false;
                                HANDLE hf = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
                                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                                if (hf != INVALID_HANDLE_VALUE) {
                                    DWORD written = 0;
                                    if (WriteFile(hf, ptr, (DWORD)len, &written, NULL)
                                        && written == (DWORD)len)
                                        saved = true;
                                    CloseHandle(hf);  // 先关句柄再替换/删除，否则会因占用失败
                                }
                                if (!saved || !MoveFileExW(tmpPath.c_str(), filePath,
                                                           MOVEFILE_REPLACE_EXISTING |
                                                           MOVEFILE_WRITE_THROUGH)) {
                                    DeleteFileW(tmpPath.c_str());  // 尽力清理失败遗留的临时文件
                                    ok = false;
                                }
                            }
                        }
                        if (ptr) GlobalUnlock(hMem);
                    }
                }
                stream->Release();
            }
        }
        delete bmp;
    }
    return ok;
}

// 将预乘 32bpp ARGB 像素缓冲编码为 PNG，可选产出 data URL base64 与/或原始 PNG 字节。
// 用 Gdiplus::Bitmap 直接包装外部预乘缓冲（PixelFormat32bppPARGB），
// PNG 编码器会转为非预乘写入 PNG（同时与剪贴板 CF_DIB 的预乘约定一致），保留 alpha。

static bool EncodePremulArgbPng(const void* bits, int w, int h, int stride,
                                std::string* base64Out, std::string* rawOut) {
    if (!bits || w <= 0 || h <= 0) return false;
    CLSID pngClsid;
    if (GetPngEncoderClsid(&pngClsid) < 0) return false;
    IStream* stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap bmp(w, h, stride, PixelFormat32bppPARGB, (BYTE*)bits);
        if (bmp.Save(stream, &pngClsid, NULL) == Gdiplus::Ok) {
            HGLOBAL hMem = NULL;
            if (GetHGlobalFromStream(stream, &hMem) == S_OK && hMem) {
                // 真实长度取自流（须在 Release 前）；GlobalSize 只是堆分配容量
                size_t len = GetStreamLength(stream);
                BYTE* ptr = (BYTE*)GlobalLock(hMem);
                if (ptr && len > 0) {
                    if (rawOut) rawOut->assign((const char*)ptr, len);
                    if (base64Out) *base64Out = "data:image/png;base64," + Base64Encode(ptr, len);
                    ok = true;
                }
                if (ptr) GlobalUnlock(hMem);
            }
        }
    }
    stream->Release();
    return ok;
}

// 圆角透明图复制到剪贴板：CF_DIB（BITMAPV4HEADER, BI_BITFIELDS, 预乘 ARGB, 自下而上）
// + 注册 PNG 格式（透明度跨应用最可靠，浏览器/Slack/图像编辑器按此读取）。
// 仅 radius>0 调用；radius==0 仍走 SaveBitmapToClipboard。
// MSDN 约定：调用 GetDIBits 时目标位图不得选入传入的 DC（函数内部自行建空内存 DC，
// 不依赖调用方任何 DC 的选中状态），否则部分驱动返回失败/错位数据，粘贴后为空白。

static bool SaveArgbBitmapToClipboard(HBITMAP hbmp, int w, int h,
                                      const std::string& pngBytes) {
    if (!OpenClipboardWithRetry()) return false;  // 与 SaveBitmapToClipboard 相同的重试策略
    EmptyClipboard();
    // CF_DIB：用 GetDIBits 转成自下而上、BI_BITFIELDS 的 32bpp 预乘 ARGB
    BITMAPV4HEADER b4 = {};
    b4.bV4Size = sizeof(BITMAPV4HEADER);
    b4.bV4Width = w;
    b4.bV4Height = h;  // 正值 = 自下而上
    b4.bV4Planes = 1;
    b4.bV4BitCount = 32;
    b4.bV4V4Compression = BI_BITFIELDS;
    b4.bV4RedMask   = 0x00FF0000;
    b4.bV4GreenMask = 0x0000FF00;
    b4.bV4BlueMask  = 0x000000FF;
    b4.bV4AlphaMask = 0xFF000000;
    b4.bV4SizeImage = (DWORD)((SIZE_T)w * (SIZE_T)h * 4);
    SIZE_T dibSize = sizeof(BITMAPV4HEADER) + (SIZE_T)w * (SIZE_T)h * 4;
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (hDib) {
        BYTE* p = (BYTE*)GlobalLock(hDib);
        if (p) {
            memcpy(p, &b4, sizeof(b4));
            // 用私有空内存 DC 完成 GetDIBits：hbmp 保证不选入该 DC，
            // 也不要求调用方先摘出位图，契约在函数内部恒成立。
            HDC dibDC = CreateCompatibleDC(NULL);
            bool dibOk = false;
            if (dibDC) {
                // 返回值为拷贝的扫描行数，==h 才算完整转换；失败则不注册 CF_DIB
                // 并释放缓冲，免得未填充的像素数据被粘到剪贴板（PNG 格式仍可携带真实内容）。
                dibOk = h > 0 && GetDIBits(dibDC, hbmp, 0, h, p + sizeof(b4),
                                           (BITMAPINFO*)&b4, DIB_RGB_COLORS) == h;
                DeleteDC(dibDC);
            }
            GlobalUnlock(hDib);
            if (dibOk) {
                if (!SetClipboardData(CF_DIB, hDib)) GlobalFree(hDib);
            } else {
                GlobalFree(hDib);
            }
        } else {
            GlobalFree(hDib);
        }
    }
    // PNG 格式：透明度最可靠的载体
    if (!pngBytes.empty()) {
        static UINT pngFmt = 0;
        if (pngFmt == 0) pngFmt = RegisterClipboardFormatW(L"PNG");
        if (pngFmt) {
            HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngBytes.size());
            if (hPng) {
                BYTE* pp = (BYTE*)GlobalLock(hPng);
                if (pp) {
                    memcpy(pp, pngBytes.data(), pngBytes.size());
                    GlobalUnlock(hPng);
                    if (!SetClipboardData(pngFmt, hPng)) GlobalFree(hPng);
                } else GlobalFree(hPng);
            }
        }
    }
    CloseClipboard();
    return true;
}

// 生成圆角透明的 32bpp 预乘 ARGB DIB（radius>0 用）。
// 复用现有像素提取+标注合成流程，但用 32bpp ARGB DIB 替代不透明位图；
// 合成前把 alpha 统一置 255（保证标注以不透明背景混合、AA 边正确），合成后按圆角
// 蒙版逐像素写入 alpha（内部不透明、弧边抗锯齿预乘、外部透明）。
// 返回 HBITMAP（调用方 DeleteObject）；outDC 调用方 DeleteDC；outBits 为 DIB 像素指针。
// 任一拷贝/缩放环节失败时清理全部已建 GDI 资源并返回 NULL（out* 均保持空值/零值），
// 调用方按失败处理，不会拿到内容未初始化的半成品。

static HBITMAP BuildRoundedArgbFinal(HDC memDC, const RECT& rect, int vx, int vy,
    double dpiScale, const std::vector<Annotation>& anns, int radius, int mosaicSizeIdx,
    HDC& outDC, void*& outBits, int& outW, int& outH) {
    outDC = NULL; outBits = NULL; outW = 0; outH = 0;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return NULL;

    int lx = rect.left - vx;
    int ly = rect.top - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    int pw = (int)(width * dpiScale + 0.5);
    int ph = (int)(height * dpiScale + 0.5);

    HDC screenDC = GetDC(NULL);
    if (!screenDC) return NULL;

    // 物理尺寸 32bpp ARGB DIB（top-down）；BitBlt 后 RGB 就位，alpha 字节不可靠（下方统一置 255）
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP regionBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC regionDC = regionBmp ? CreateCompatibleDC(screenDC) : NULL;
    if (regionDC) SelectObject(regionDC, regionBmp);

    // 选区拷贝失败（锁屏瞬断/源不可读等）会残留未初始化像素：整体返回 NULL，
    // 由 ExtractRegionResult / SaveRegionToPngFile 的失败分支收尾，不输出黑图。
    if (!regionDC || !regionBmp ||
        !BitBlt(regionDC, 0, 0, pw, ph, memDC, px, py, SRCCOPY)) {
        if (regionDC) DeleteDC(regionDC);
        if (regionBmp) DeleteObject(regionBmp);
        ReleaseDC(NULL, screenDC);
        return NULL;
    }

    HBITMAP finalBmp = regionBmp;
    HDC finalDC = regionDC;
    void* finalBits = bits;
    int finalW = pw, finalH = ph;

    if (dpiScale > 1.01 || dpiScale < 0.99) {
        BITMAPINFO bi2 = {};
        bi2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi2.bmiHeader.biWidth = width;
        bi2.bmiHeader.biHeight = -height;
        bi2.bmiHeader.biPlanes = 1;
        bi2.bmiHeader.biBitCount = 32;
        bi2.bmiHeader.biCompression = BI_RGB;
        void* bits2 = NULL;
        HBITMAP scaledBmp = CreateDIBSection(screenDC, &bi2, DIB_RGB_COLORS, &bits2, NULL, 0);
        HDC scaledDC = scaledBmp ? CreateCompatibleDC(screenDC) : NULL;
        if (scaledDC && bits2) {
            SelectObject(scaledDC, scaledBmp);
            SetHalftoneStretchMode(scaledDC);
        }
        // 缩放拷贝任一步失败：清理两套资源后同上按整体失败处理，不出半成品。
        if (!scaledDC || !bits2 ||
            !StretchBlt(scaledDC, 0, 0, width, height, regionDC, 0, 0, pw, ph, SRCCOPY)) {
            if (scaledDC) DeleteDC(scaledDC);
            if (scaledBmp) DeleteObject(scaledBmp);
            DeleteDC(regionDC);
            DeleteObject(regionBmp);
            ReleaseDC(NULL, screenDC);
            return NULL;
        }
        DeleteDC(regionDC);
        DeleteObject(regionBmp);
        finalBmp = scaledBmp; finalDC = scaledDC; finalBits = bits2;
        finalW = width; finalH = height;
    }

    // 合成前置 alpha=255：标注按不透明背景混合（结果不透明），后续圆角蒙版只负责透明
    if (finalBits) {
        BYTE* p = (BYTE*)finalBits;
        int cnt = finalW * finalH;
        for (int i = 0; i < cnt; i++) p[i * 4 + 3] = 255;
    }
    CompositeAnnotations(finalDC, memDC, anns, rect, vx, vy, dpiScale,
                         SC_MOSAIC_SIZES[mosaicSizeIdx]);

    // 圆角蒙版：同尺寸 32bpp DIB，不透明黑底 + GDI+ 填白圆角路径，取 RGB 通道作 coverage
    int r = (std::min)(radius, (std::min)(finalW, finalH) / 2);
    if (r >= 1 && finalBits) {
        BITMAPINFO mbi = {};
        mbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mbi.bmiHeader.biWidth = finalW;
        mbi.bmiHeader.biHeight = -finalH;
        mbi.bmiHeader.biPlanes = 1;
        mbi.bmiHeader.biBitCount = 32;
        mbi.bmiHeader.biCompression = BI_RGB;
        void* maskBits = NULL;
        HBITMAP maskBmp = CreateDIBSection(screenDC, &mbi, DIB_RGB_COLORS, &maskBits, NULL, 0);
        if (maskBmp && maskBits) {
            BYTE* mp = (BYTE*)maskBits;
            int mcnt = finalW * finalH;
            for (int i = 0; i < mcnt; i++) { mp[i*4]=0; mp[i*4+1]=0; mp[i*4+2]=0; mp[i*4+3]=255; }
            HDC maskDC = CreateCompatibleDC(screenDC);
            HGDIOBJ oldMask = SelectObject(maskDC, maskBmp);
            {
                Gdiplus::Graphics g(maskDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                Gdiplus::GraphicsPath path;
                AddRoundedRect(path, 0, 0, finalW, finalH, r);
                g.FillPath(&white, &path);
            }
            SelectObject(maskDC, oldMask);
            DeleteDC(maskDC);
            // 逐像素：final alpha = coverage（mask B 通道，==G==R），RGB 按其预乘
            BYTE* dst = (BYTE*)finalBits;
            BYTE* msk = (BYTE*)maskBits;
            int dcnt = finalW * finalH;
            for (int i = 0; i < dcnt; i++) {
                int a = msk[i * 4];
                if (a <= 0) {
                    dst[i*4]=0; dst[i*4+1]=0; dst[i*4+2]=0; dst[i*4+3]=0;
                } else if (a >= 255) {
                    dst[i*4+3]=255;  // RGB 不变（不透明）
                } else {
                    dst[i*4]   = (BYTE)((int)dst[i*4]   * a / 255);
                    dst[i*4+1] = (BYTE)((int)dst[i*4+1] * a / 255);
                    dst[i*4+2] = (BYTE)((int)dst[i*4+2] * a / 255);
                    dst[i*4+3] = (BYTE)a;
                }
            }
            DeleteObject(maskBmp);
        }
    }

    ReleaseDC(NULL, screenDC);
    outDC = finalDC; outBits = finalBits; outW = finalW; outH = finalH;
    return finalBmp;
}

// 不透明位图路径（radius==0）的公共合成：从预截屏位图按选区提取区域，若有 DPI 缩放则
// 缩放回逻辑尺寸，再合成标注。ExtractRegionResult（base64+剪贴板）与 SaveRegionToPngFile
// （落盘）的 radius==0 分支此前为近逐行两份重复，收口至此单一实现。
// 成功时经 outFinalDC/outFinalBmp 带出已合成的位图（调用方负责 DeleteDC/DeleteObject）；
// 失败时内部已清理全部 GDI 资源、out* 保持 NULL，返回 false。
// mosaicSizeIdx：马赛克块大小索引（显式传参，消除对全局 g_captureCtx 的穿透耦合）。

static bool ComposeSelectedBitmap(HDC memDC, const RECT& rect, int vx, int vy,
    double dpiScale, const std::vector<Annotation>& anns, int mosaicSizeIdx,
    HDC& outFinalDC, HBITMAP& outFinalBmp) {
    outFinalDC = NULL; outFinalBmp = NULL;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    // 从物理尺寸位图提取区域
    int lx = rect.left - vx;
    int ly = rect.top - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    int pw = (int)(width * dpiScale + 0.5);
    int ph = (int)(height * dpiScale + 0.5);

    HDC screenDC = GetDC(NULL);
    HDC regionDC = screenDC ? CreateCompatibleDC(screenDC) : NULL;
    HBITMAP regionBmp = screenDC ? CreateCompatibleBitmap(screenDC, pw, ph) : NULL;
    if (regionDC && regionBmp) SelectObject(regionDC, regionBmp);

    // 拷贝/缩放任一环节失败都按失败收尾：未初始化的 CompatibleBitmap 内容会被当成截图结果
    // （黑图报成功），已创建资源在尾部统一清理。
    bool captured = regionDC && regionBmp &&
                    BitBlt(regionDC, 0, 0, pw, ph, memDC, px, py, SRCCOPY);

    // 如果有 DPI 缩放，缩放回逻辑尺寸
    HBITMAP finalBmp = regionBmp;
    HDC finalDC = regionDC;
    if (captured && (dpiScale > 1.01 || dpiScale < 0.99)) {
        HDC scaledDC = CreateCompatibleDC(screenDC);
        HBITMAP scaledBmp = scaledDC ? CreateCompatibleBitmap(screenDC, width, height) : NULL;
        if (scaledDC && scaledBmp)
            SelectObject(scaledDC, scaledBmp);
        if (scaledDC && scaledBmp &&
            StretchBlt(scaledDC, 0, 0, width, height, regionDC, 0, 0, pw, ph, SRCCOPY)) {
            DeleteDC(regionDC);
            DeleteObject(regionBmp);
            finalBmp = scaledBmp;
            finalDC = scaledDC;
        } else {
            captured = false;  // 缩放失败不得输出半成品
            if (scaledDC) DeleteDC(scaledDC);
            if (scaledBmp) DeleteObject(scaledBmp);
        }
    }

    if (captured) {
        // 合成标注进最终图像（finalDC 原点 = 选区原点，标注为绝对坐标，偏移 = -rect.left/top）
        CompositeAnnotations(finalDC, memDC, anns, rect, vx, vy, dpiScale,
                             SC_MOSAIC_SIZES[mosaicSizeIdx]);
    }

    ReleaseDC(NULL, screenDC);

    if (!captured) {
        // 失败：清理已建资源（final* 与 region* 共享所有权时仅此一处删除）
        DeleteDC(finalDC);
        DeleteObject(finalBmp);
        return false;
    }

    outFinalDC = finalDC;
    outFinalBmp = finalBmp;
    return true;
}

// 从预截屏位图提取区域，生成 base64 并复制到剪贴板。
// anns：可选的标注列表，会合成进最终 PNG（选区相对坐标，finalDC 原点 = 选区原点）。
// radius>0 走圆角透明导出（32bpp ARGB + 圆角蒙版 + PNG/PNG剪贴板）；radius==0 维持不透明位图路径不变。
// mosaicSizeIdx：马赛克块大小索引（显式传参，消除对全局 g_captureCtx 的穿透耦合）。
// 拷贝/缩放失败时 success 保持 false 且不产出 base64/剪贴板数据（资源全量清理）。

ScreenshotResult* ExtractRegionResult(HDC memDC, const RECT& rect,
    int vx, int vy, double dpiScale, const std::vector<Annotation>& anns,
    int radius, int mosaicSizeIdx) {
    ScreenshotResult* result = new ScreenshotResult();
    result->success = false;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    result->x = rect.left;
    result->y = rect.top;
    result->x2 = rect.right;
    result->y2 = rect.bottom;
    result->width = width;
    result->height = height;

    if (width <= 0 || height <= 0) return result;

    // radius>0：圆角透明导出
    if (radius > 0) {
        HDC fDC = NULL; void* fBits = NULL; int fw = 0, fh = 0;
        HBITMAP fbmp = BuildRoundedArgbFinal(memDC, rect, vx, vy, dpiScale, anns, radius,
                                             mosaicSizeIdx, fDC, fBits, fw, fh);
        if (fbmp && fBits) {
            std::string rawPng, b64;
            if (EncodePremulArgbPng(fBits, fw, fh, fw * 4, &b64, &rawPng)) {
                result->base64 = b64;
                result->success = SaveArgbBitmapToClipboard(fbmp, fw, fh, rawPng);
            }
        }
        if (fDC) DeleteDC(fDC);
        if (fbmp) DeleteObject(fbmp);
        return result;
    }

    // radius==0：原有不透明位图路径（公共合成，收口至 ComposeSelectedBitmap）
    HDC finalDC = NULL; HBITMAP finalBmp = NULL;
    if (ComposeSelectedBitmap(memDC, rect, vx, vy, dpiScale, anns, mosaicSizeIdx,
                              finalDC, finalBmp)) {
        // 生成 base64
        result->base64 = BitmapToBase64Png(finalBmp);
        // 复制到剪贴板
        result->success = SaveBitmapToClipboard(finalBmp);
    }

    DeleteDC(finalDC);          // NULL 安全
    DeleteObject(finalBmp);

    return result;
}

// ==================== 保存对话框辅助 ====================

// 生成默认保存文件名：Screenshot_YYYYMMDD_HHMMSS.png

static std::wstring MakeDefaultScreenshotName() {
    time_t now = time(NULL);
    struct tm lt;
    localtime_s(&lt, &now);
    wchar_t buf[64];
    wsprintfW(buf, L"Screenshot_%04d%02d%02d_%02d%02d%02d.png",
              lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
              lt.tm_hour, lt.tm_min, lt.tm_sec);
    return std::wstring(buf);
}

// 弹出系统保存对话框，返回用户选择的文件完整路径（含 .png 后缀）；
// 用户取消或失败时返回空字符串。
// hwndOwner：父窗口句柄（截图覆盖层），用于模态居中。
// 注意：覆盖层是 WS_EX_TOPMOST 全屏窗口，通用对话框可能被遮挡。
//       弹出前临时移除其 TOPMOST（让对话框自然置顶），关闭后恢复，保证对话框可见可交互。

std::wstring PromptSaveFilePath(HWND hwndOwner) {
    // 默认目录：图片库（FOLDERID_Pictures），获取失败则退化为桌面
    wchar_t* defaultDir = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, 0, NULL, &defaultDir);
    std::wstring initDir;
    if (SUCCEEDED(hr) && defaultDir) {
        initDir = defaultDir;
        CoTaskMemFree(defaultDir);
    } else {
        wchar_t* desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &desktop)) && desktop) {
            initDir = desktop;
            CoTaskMemFree(desktop);
        }
    }

    std::wstring defaultName = MakeDefaultScreenshotName();

    wchar_t fileBuf[MAX_PATH] = {0};
    wcsncpy_s(fileBuf, MAX_PATH, defaultName.c_str(), _TRUNCATE);

    // 临时取消覆盖层 TOPMOST，确保保存对话框显示在最上层
    if (hwndOwner) {
        SetWindowPos(hwndOwner, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"png";  // 用户未输扩展名时自动补 .png
    if (!initDir.empty()) {
        ofn.lpstrInitialDir = initDir.c_str();
    }
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
                | OFN_NOCHANGEDIR;

    BOOL ok = GetSaveFileNameW(&ofn);

    // 恢复覆盖层 TOPMOST（用户取消保存时需要回到置顶全屏状态）
    if (hwndOwner) {
        SetWindowPos(hwndOwner, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (ok) {
        return std::wstring(fileBuf);
    }
    return std::wstring();
}

// 将已合成标注的选区 HBITMAP 保存为 PNG 文件。
// 返回 true 表示保存成功。
// mosaicSizeIdx：马赛克块大小索引（显式传参，消除对全局 g_captureCtx 的穿透耦合）。
// 选区拷贝/缩放任一环节失败时同样返回 false 并清理全部已建 GDI 资源，不会落盘黑图。

bool SaveRegionToPngFile(HDC memDC, const RECT& rect, int vx, int vy,
                                double dpiScale, const std::vector<Annotation>& anns,
                                const std::wstring& filePath, int radius, int mosaicSizeIdx) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || filePath.empty()) return false;

    // radius>0：圆角透明导出（32bpp ARGB + 圆角蒙版），GDI+ 直接保存为 PNG 文件
    if (radius > 0) {
        HDC fDC = NULL; void* fBits = NULL; int fw = 0, fh = 0;
        HBITMAP fbmp = BuildRoundedArgbFinal(memDC, rect, vx, vy, dpiScale, anns, radius,
                                             mosaicSizeIdx, fDC, fBits, fw, fh);
        bool ok = false;
        if (fbmp && fBits) {
            CLSID pngClsid;
            if (GetPngEncoderClsid(&pngClsid) >= 0) {
                Gdiplus::Bitmap bmp(fw, fh, fw * 4, PixelFormat32bppPARGB, (BYTE*)fBits);
                ok = (bmp.Save(filePath.c_str(), &pngClsid, NULL) == Gdiplus::Ok);
            }
        }
        if (fDC) DeleteDC(fDC);
        if (fbmp) DeleteObject(fbmp);
        return ok;
    }

    // radius==0：原有不透明位图路径（公共合成，收口至 ComposeSelectedBitmap）
    HDC finalDC = NULL; HBITMAP finalBmp = NULL;
    bool ok = false;
    if (ComposeSelectedBitmap(memDC, rect, vx, vy, dpiScale, anns, mosaicSizeIdx,
                              finalDC, finalBmp)) {
        // 用 GDI+ 保存为 PNG 文件（GDI+ 已由会话级 InitGdipResources 启动）
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(finalBmp, NULL);
        if (bmp) {
            CLSID pngClsid;
            if (GetPngEncoderClsid(&pngClsid) >= 0) {
                ok = (bmp->Save(filePath.c_str(), &pngClsid, NULL) == Gdiplus::Ok);
            }
            delete bmp;
        }
    }

    DeleteDC(finalDC);          // NULL 安全
    DeleteObject(finalBmp);
    return ok;
}
