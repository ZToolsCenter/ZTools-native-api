// 截图模块：工具栏「翻译」按钮（OCR 识别选区文字 → 翻译 → 译文覆盖原文字区域）。
//
// 流程（点击 TB_Translate 后）：
//   1. 截图线程（覆盖层窗口过程所在线程）把选区按物理分辨率从预截屏位图裁出并编码为
//      PNG base64（GDI/GDI+ 调用全部留在截图线程内，规避跨线程使用会话级 GDI+ 的
//      生命周期竞争），随后立即转入 TRL_Busy 状态并显示进度气泡；
//   2. 工作线程经 Provider 桥（src/provider_bridge.h）原生编排两步 provider 调用：
//      a. type="ocr" 提交选区图像，识别结果为带行级坐标（top/left/right/bottom，
//         提交图像内像素坐标）的文本行数组——行高即原文字高度，是译文覆盖定位
//         与字号适配的依据；
//      b. 文本行先经空间关系聚类还原成段落（algo/translate_cluster.cpp：行高/行距
//         估计 → 候选相邻 → 多维特征判定连续/边界 → 聚合 + 孤行吸收二次修正，
//         解决 OCR 按行输出导致的段落信息丢失），再对每个段落调用一次
//         type="translation"（契约入参为单条 text，段落文本已按 CJK/拉丁规则拼接；
//         段为单位串行调用以尊重 provider 限流），在原生层把译文与对应段落的成员行
//         联合框合并成可渲染块，段落原文行数随块带给渲染层定字号；
//         OCR provider 未返回坐标（如纯文本 AI 识别）时走整图兜底：整段文本一次
//         翻译、以整个选区为单一面板；
//   3. 结果打包后经「单飞行结果槽 + PostMessage」回投截图线程：译文块坐标换算回
//      选区绝对逻辑坐标存入 ctx（连同原段落行数），进入 TRL_Shown；OnPaint 在标注
//      之上绘制译文覆盖面板（白底盖住原文字），译文字号按段落独立搜索：min(能装进
//      段框的最大字号, 原文行距对应字号)——行距对齐保证观感与原版面一致、可行性
//      收缩保证译文变长时放得下（见 sctranslatedraw::FitParagraph），确认/保存导出
//      时同样合成进最终图像。TRL_Shown 下再次点击「翻译」为切换退出：清除覆盖
//      回到 TRL_Idle 并取消按钮激活，不重跑翻译；需重译时再点一次重新发起即可。
//
// provider 约定（宿主 JS 侧 ProviderBridge.start 的 handler 直接映射 providerManager.invoke）：
//   ocr:        入参 {"image":"data:image/png;base64,..."}
//               （选区裁剪，物理像素分辨率）
//          =>  返回 {"text":"..","blocks":[{"text":"行文本","left":L,"top":T,
//               "right":R,"bottom":B},...],"confidence":0.9}，blocks 为行级对象数组，
//               坐标为提交图像内像素坐标；仅回 text / blocks 为字符串数组（无坐标）
//               时自动走整图兜底
//   translation: 入参 {"text":"行文本"}（from/to 缺省用 provider 默认：自动检测
//               源语言、翻译到默认目标语言）
//          =>  返回 {"text":"译文","detectedFrom":"en"(可选)}
//
// 线程模型：工作线程只持有任务入参的拷贝（图像 base64 / 选区矩形 / DPI / 窗口句柄），
// 不触碰 CaptureContext；结果经互斥锁保护的单飞行槽（g_translateSlot）交还截图线程，
// 会话收尾时由 TeardownTranslateJobs 清槽，窗口销毁导致的投递失败由发布方自行回收，
// 不存在跨线程共享可变状态。
#include "internal.h"
#include "../algo/translate_cluster_internal.h"
#include "../../provider_bridge.h"
#include "../../logger.h"

#include <memory>

// OCR provider 调用等待 JS 侧结果的超时（大图 base64 + 识别引擎耗时，取宽裕值）

static const DWORD SC_OCR_TIMEOUT_MS = 30000;

// 单行翻译 provider 调用等待 JS 侧结果的超时（逐行串行调用，每行独立计时）

static const DWORD SC_TRANSLATE_LINE_TIMEOUT_MS = 15000;

// 错误状态气泡自动隐藏时长

static const DWORD SC_TRANSLATE_ERROR_TTL_MS = 4000;

// ==================== 极简 JSON 解析（provider 返回值解析专用） ====================
// 项目不引入第三方 JSON 库，这里实现一个覆盖 JSON 全语法的递归下降解析器：
// null/bool/number/string（含 \uXXXX 代理对）/array/object。数值按 double 存储，
// 字符串解码为 UTF-8。仅在本文件使用。

namespace scminijson {

struct Value {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type type = NUL;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> members;   // 保持出现顺序（小块解析无需哈希）

    // 取对象成员（不存在或非对象时返回 nullptr）
    const Value* find(const char* key) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : members) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;

    bool Parse(const std::string& text, Value& out) {
        p = text.data();
        end = p + text.size();
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return p == end;   // 尾部不允许多余内容
    }

private:
    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool Consume(char c) {
        if (p < end && *p == c) { p++; return true; }
        return false;
    }
    bool ParseValue(Value& v) {
        if (p >= end) return false;
        switch (*p) {
        case '{': return ParseObject(v);
        case '[': return ParseArray(v);
        case '"':
            v.type = Value::STR;
            return ParseString(v.str);
        case 't':
            v.type = Value::BOOL; v.boolean = true;
            return ParseLiteral("true");
        case 'f':
            v.type = Value::BOOL; v.boolean = false;
            return ParseLiteral("false");
        case 'n':
            v.type = Value::NUL;
            return ParseLiteral("null");
        default:
            return ParseNumber(v);
        }
    }
    bool ParseLiteral(const char* lit) {
        size_t n = strlen(lit);
        if ((size_t)(end - p) < n || strncmp(p, lit, n) != 0) return false;
        p += n;
        return true;
    }
    bool ParseNumber(Value& v) {
        if (p >= end) return false;
        char* stop = nullptr;
        double d = strtod(p, &stop);
        if (stop == p) return false;
        v.type = Value::NUM;
        v.number = d;
        p = stop;
        return true;
    }
    // UTF-8 编码一个码点
    static void AppendCodepoint(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool ParseHex4(unsigned& out) {
        if (end - p < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }
    bool ParseString(std::string& out) {
        if (!Consume('"')) return false;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!ParseHex4(cp)) return false;
                // 代理对：高代理后必须跟 \uDC00-\uDFFF 的低代理，合成真实码点
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    unsigned lo = 0;
                    if (!ParseHex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;   // 孤立低代理
                }
                AppendCodepoint(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;   // 未闭合
    }
    bool ParseArray(Value& v) {
        if (!Consume('[')) return false;
        v.type = Value::ARR;
        SkipWs();
        if (Consume(']')) return true;
        while (true) {
            v.arr.emplace_back();
            SkipWs();
            if (!ParseValue(v.arr.back())) return false;
            SkipWs();
            if (Consume(',')) continue;
            return Consume(']');
        }
    }
    bool ParseObject(Value& v) {
        if (!Consume('{')) return false;
        v.type = Value::OBJ;
        SkipWs();
        if (Consume('}')) return true;
        while (true) {
            SkipWs();
            std::string key;
            if (p >= end || *p != '"') return false;
            if (!ParseString(key)) return false;
            SkipWs();
            if (!Consume(':')) return false;
            v.members.emplace_back();
            v.members.back().first = std::move(key);
            SkipWs();
            if (!ParseValue(v.members.back().second)) return false;
            SkipWs();
            if (Consume(',')) continue;
            return Consume('}');
        }
    }
};

}  // namespace scminijson

// ==================== 小工具 ====================

// UTF-8 → UTF-16（provider 返回的文本/错误信息消费用）；转换失败按原样可用部分截断

static std::wstring Utf8ToWideForTranslate(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

// 是否 CJK 字符（基本区 + 扩展区粗判）：换行算法据此允许在任意 CJK 字符后断行

static bool IsCjkForWrap(wchar_t c) {
    return c >= 0x2E80 && c <= 0x9FFF    // 部首/中日韩部首/拼音/基本区
        || c >= 0x3000 && c <= 0x303F    // 中日韩标点
        || c >= 0xF900 && c <= 0xFAFF    // 兼容表意
        || c >= 0xFF00 && c <= 0xFFEF;   // 全角形式
}

// ==================== 翻译任务数据与结果槽 ====================

// 工作线程任务入参（截图线程在点击时打包，所有权随 detach 转移给工作线程）

struct TranslateJobInput {
    std::string imageBase64;   // data:image/png;base64,...（选区裁剪，物理像素分辨率）
    int imageW = 0;            // 提交图像宽（物理像素，兜底单块坐标用）
    int imageH = 0;
    RECT selection = {};       // 选区矩形（绝对虚拟屏幕逻辑坐标）
    double dpiScale = 1.0;
    HWND overlayHwnd = NULL;   // 截图覆盖层窗口（结果回投目标）
};

// 工作线程产出（经结果槽移交截图线程，由接管方释放）

struct TranslateJobResult {
    bool ok = false;
    std::wstring error;                          // ok=false 时的用户可读错误
    std::vector<TranslateBlock> blocks;          // ok=true 时的译文块（绝对逻辑坐标）
};

// 单飞行结果槽：同一时刻至多一个翻译任务在跑（点击侧 TRL_Busy 门禁保证），
// 故单指针即可；工作线程发布、截图线程取走，互斥保护跨线程交接。

static std::mutex g_translateSlotMutex;
static TranslateJobResult* g_translateSlot = nullptr;

// 发布结果：先落槽再投递消息。消息投递失败（覆盖层已销毁）时消息永不会被处理，
// 由发布方回收；投递成功但消息仍被销毁冲刷（DestroyWindow 清队）的极小竞争窗口
// 里，残留槽由会话收尾的 TeardownTranslateJobs 兜底释放（见下）。

static void PublishTranslateResult(TranslateJobResult* res, HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_translateSlotMutex);
        delete g_translateSlot;   // 兜底清残留（正常时序下恒为空）
        g_translateSlot = res;
    }
    if (hwnd == NULL || !PostMessageW(hwnd, WM_SCREENSHOT_TRANSLATE_RESULT, 0, 0)) {
        std::lock_guard<std::mutex> lock(g_translateSlotMutex);
        delete g_translateSlot;
        g_translateSlot = nullptr;
    }
}

// 会话收尾清槽（截图线程在窗口销毁后调用）：释放仍在槽中未被取走的结果，
// 使跨会话泄漏上界为零（迟到的发布者随后会因 PostMessage 失败自行回收）。

void TeardownTranslateJobs() {
    std::lock_guard<std::mutex> lock(g_translateSlotMutex);
    delete g_translateSlot;
    g_translateSlot = nullptr;
}

// ==================== provider 响应解析与调用辅助 ====================

// OCR 文本行（提交图像内的像素坐标；text 先为 OCR 原文，翻译成功后替换为译文）。
// origLineCount 为原段落行数（段落块 = 聚类成员行数），随块流向下渲染层，作为
// 行距对齐字号搜索的输入（兜底块 origLineCount = 0 表示未知）。

struct OcrRawBlock {
    std::string text;
    double x = 0, y = 0, w = 0, h = 0;
    int origLineCount = 0;
};

// 从 ocr 响应解析文本行：{"text":"..","blocks":[{text,left,top,right,bottom},..],"confidence":..}。
// 兼容 x/y/width/height 坐标命名与 blocks 为纯字符串数组的历史契约：
//   - 带有效坐标与文本的对象行 → outLines（逐行翻译/覆盖定位依据）
//   - 字符串行 / 缺坐标的对象行 → outPlain（无坐标，走整图兜底整段翻译）
// 空白文本行直接丢弃（不参与翻译也不参与兜底拼接）。

static void ParseOcrBlocks(const scminijson::Value& root,
                           std::vector<OcrRawBlock>& outLines,
                           std::vector<std::string>& outPlain) {
    const scminijson::Value* blocks = root.find("blocks");
    if (!blocks || blocks->type != scminijson::Value::ARR) return;
    for (const scminijson::Value& b : blocks->arr) {
        if (b.type == scminijson::Value::STR) {
            if (b.str.find_first_not_of(" \t\r\n") != std::string::npos) outPlain.push_back(b.str);
            continue;
        }
        if (b.type != scminijson::Value::OBJ) continue;
        const scminijson::Value* t = b.find("text");
        if (!t || t->type != scminijson::Value::STR) continue;
        if (t->str.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        OcrRawBlock rb;
        rb.text = t->str;
        if (const auto* v = b.find("left"))   if (v->type == scminijson::Value::NUM) rb.x = v->number;
        if (const auto* v = b.find("top"))    if (v->type == scminijson::Value::NUM) rb.y = v->number;
        if (const auto* v = b.find("x"))      if (v->type == scminijson::Value::NUM) rb.x = v->number;
        if (const auto* v = b.find("y"))      if (v->type == scminijson::Value::NUM) rb.y = v->number;
        if (rb.w <= 0) {
            if (const auto* v = b.find("right")) if (v->type == scminijson::Value::NUM) rb.w = v->number - rb.x;
        }
        if (rb.h <= 0) {
            if (const auto* v = b.find("bottom")) if (v->type == scminijson::Value::NUM) rb.h = v->number - rb.y;
        }
        if (rb.w <= 0) {
            if (const auto* v = b.find("width")) if (v->type == scminijson::Value::NUM) rb.w = v->number;
        }
        if (rb.h <= 0) {
            if (const auto* v = b.find("height")) if (v->type == scminijson::Value::NUM) rb.h = v->number;
        }
        if (rb.w > 0 && rb.h > 0) outLines.push_back(std::move(rb));
        else outPlain.push_back(std::move(rb.text));   // 有文本无坐标：并兜底文本
    }
}

// 把字符串安全编码为 JSON 字符串字面量内容（不含两侧引号）：
// 引号/反斜杠/控制字符转义，UTF-8 字节透传。供构造 translation 请求用。

static std::string JsonEscapeForTranslate(const std::string& s) {
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                out += "\\u00";
                out.push_back(HEX[((unsigned char)c >> 4) & 0xF]);
                out.push_back(HEX[(unsigned char)c & 0xF]);
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

// 调用 translation provider 翻译一段文本（OCR 单行或兜底整段）。
// 入参 {"text":".."}（from/to 缺省走 provider 默认：自动检测源语言、默认目标语言），
// 返回 {"text":"译文",..}。任何失败返回 false 并给出可读错误（errOut 可为空串）。

static bool InvokeTranslation(const std::string& text, std::string& textOut, std::string& errOut) {
    std::string req = "{\"text\":\"" + JsonEscapeForTranslate(text) + "\"}";
    ztools_provider_bridge::InvokeResult tr =
        ztools_provider_bridge::Invoke("translation", req, SC_TRANSLATE_LINE_TIMEOUT_MS);
    if (!tr.ok) {
        errOut = tr.error;
        return false;
    }
    scminijson::Value root;
    if (!scminijson::Parser().Parse(tr.value, root)) {
        errOut = "翻译结果解析失败";
        return false;
    }
    const scminijson::Value* t = root.find("text");
    if (!t || t->type != scminijson::Value::STR) {
        errOut = "翻译结果缺少 text 字段";
        return false;
    }
    textOut = t->str;
    return true;
}

// ==================== 工作线程主流程 ====================

// 原生编排（OCR 行级识别 → 逐行翻译 → 坐标合并）→ 坐标换算 → 发布结果。
// 任何阶段失败都以 ok=false + 中文错误收束，由截图线程转为错误气泡展示（数秒后自动消失）。

static void TranslateWorkerMain(TranslateJobInput* job) {
    std::unique_ptr<TranslateJobInput> in(job);
    auto res = std::make_unique<TranslateJobResult>();

    // 译文块（提交图像内的像素坐标；text 已替换为译文）
    std::vector<OcrRawBlock> translated;

    do {
        // ---- 0) 桥接就绪检查（宿主未注册 ProviderBridge 时给出可操作提示）----
        if (!ztools_provider_bridge::IsReady()) {
            ZLOG_WARN("translate", "provider bridge not ready");
            res->error = L"翻译服务未就绪（未注册 ProviderBridge）";
            break;
        }

        // ---- 1) OCR：提交选区图像，取回带行级坐标的文本行 ----
        ZLOG_INFO("translate", "ocr request (image %dx%d, %zu bytes)",
                  in->imageW, in->imageH, in->imageBase64.size());
        std::string ocrReq = "{\"image\":\"" + in->imageBase64 + "\"}";
        ztools_provider_bridge::InvokeResult ocr =
            ztools_provider_bridge::Invoke("ocr", ocrReq, SC_OCR_TIMEOUT_MS);
        if (!ocr.ok) {
            ZLOG_ERROR("translate", "ocr invoke failed: %s", ocr.error.c_str());
            res->error = L"文字识别失败：" + Utf8ToWideForTranslate(ocr.error);
            break;
        }
        scminijson::Value ocrRoot;
        if (!scminijson::Parser().Parse(ocr.value, ocrRoot)) {
            res->error = L"识别结果解析失败";
            break;
        }
        std::vector<OcrRawBlock> lines;       // 带坐标的文本行
        std::vector<std::string> plainTexts;  // 无坐标的文本（兜底用）
        ParseOcrBlocks(ocrRoot, lines, plainTexts);
        if (lines.empty() && plainTexts.empty()) {
            // 仅整段 text（无 blocks，纯文本 AI 识别契约）仍可走 2b 整图兜底；
            // 连 text 也无内容才是真正的「未识别到文字」
            const auto* t = ocrRoot.find("text");
            bool hasWholeText = t && t->type == scminijson::Value::STR
                && t->str.find_first_not_of(" \t\r\n") != std::string::npos;
            if (!hasWholeText) {
                ZLOG_INFO("translate", "ocr returned no text");
                res->error = L"未识别到文字";
                break;
            }
        }
        ZLOG_INFO("translate", "ocr ok (%zu line blocks, %zu plain)", lines.size(), plainTexts.size());

        std::string lastErr;
        if (!lines.empty()) {
            // ---- 2a) 文本行聚类成段落 → 逐段翻译 ----
            // OCR 只按行给结果：逐行翻译会把跨行句子拆散（丢失段落上下文）、
            // 逐行覆盖会把段落渲染成碎面板。先按空间关系把连续文本行聚成段落
            // （判定流水线见 algo/translate_cluster.cpp 头部注释），再以段落为
            // 单位翻译与覆盖。串行请求以尊重 provider 限流；单段失败跳过（记
            // 日志），全部失败才整体报错。
            std::vector<TcLine> clusterInput;
            clusterInput.reserve(lines.size());
            for (const OcrRawBlock& rb : lines) {
                TcLine tl;
                tl.text = rb.text;
                tl.x = rb.x;
                tl.y = rb.y;
                tl.w = rb.w;
                tl.h = rb.h;
                clusterInput.push_back(std::move(tl));
            }
            TcClusterResult clustered = TcClusterLines(clusterInput);
            ZLOG_INFO("translate",
                      "cluster: %zu lines -> %zu paragraphs (medianH=%.1f, pitch=%.1f, boundaries=%d)",
                      lines.size(), clustered.paragraphs.size(),
                      clustered.medianLineH, clustered.medianPitch, clustered.boundaryCount);

            for (size_t i = 0; i < clustered.paragraphs.size(); i++) {
                const TcParagraph& para = clustered.paragraphs[i];
                std::string outText;
                if (!InvokeTranslation(para.text, outText, lastErr)) {
                    ZLOG_WARN("translate", "para %zu (%zu lines) translation failed: %s",
                              i, para.lineIdx.size(), lastErr.c_str());
                    continue;
                }
                if (outText.find_first_not_of(" \t\r\n") == std::string::npos) {
                    ZLOG_WARN("translate", "para %zu translation returned empty", i);
                    continue;
                }
                OcrRawBlock tb;
                tb.x = para.x;
                tb.y = para.y;
                tb.w = para.w;
                tb.h = para.h;
                tb.origLineCount = (int)para.lineIdx.size();   // 原段落行数（渲染层字号搜索用）
                tb.text = std::move(outText);
                translated.push_back(std::move(tb));
            }
            if (translated.empty()) {
                res->error = L"翻译失败：" + Utf8ToWideForTranslate(lastErr);
                break;
            }
            ZLOG_INFO("translate", "translation done (%zu/%zu paragraphs)",
                      translated.size(), clustered.paragraphs.size());
        } else {
            // ---- 2b) 整图兜底：OCR 未给坐标（如纯文本 AI 识别）时整段翻译，
            // 以整个选区为单一覆盖面板（行级定位能力由 provider 决定，此处无法补救）
            std::string wholeText;
            if (const auto* t = ocrRoot.find("text");
                t && t->type == scminijson::Value::STR) {
                wholeText = t->str;
            }
            if (wholeText.find_first_not_of(" \t\r\n") == std::string::npos) {
                std::string joined;
                for (size_t i = 0; i < plainTexts.size(); i++) {
                    if (i > 0) joined += "\n";
                    joined += plainTexts[i];
                }
                wholeText = std::move(joined);
            }
            std::string outText;
            if (!InvokeTranslation(wholeText, outText, lastErr)) {
                ZLOG_ERROR("translate", "whole-text translation failed: %s", lastErr.c_str());
                res->error = L"翻译失败：" + Utf8ToWideForTranslate(lastErr);
                break;
            }
            OcrRawBlock fb;
            fb.x = 0.0;
            fb.y = 0.0;
            fb.w = (double)in->imageW;
            fb.h = (double)in->imageH;
            fb.text = std::move(outText);
            translated.push_back(std::move(fb));
            ZLOG_INFO("translate", "whole-text fallback ok");
        }

        // ---- 3) 坐标换算：图像像素（物理）→ 选区绝对逻辑坐标，并夹回选区内 ----
        double ds = in->dpiScale > 0.01 ? in->dpiScale : 1.0;
        for (const OcrRawBlock& rb : translated) {
            RECT box = {};
            box.left   = in->selection.left + (int)(rb.x / ds + 0.5);
            box.top    = in->selection.top  + (int)(rb.y / ds + 0.5);
            box.right  = box.left + (int)(rb.w / ds + 0.5);
            box.bottom = box.top  + (int)(rb.h / ds + 0.5);
            RECT clipped = {};
            if (!IntersectRect(&clipped, &box, &in->selection)) continue;
            if (clipped.right - clipped.left < 2 || clipped.bottom - clipped.top < 2) continue;

            TranslateBlock tb;
            tb.box = clipped;
            tb.text = Utf8ToWideForTranslate(rb.text);
            if (tb.text.find_first_not_of(L" \t\r\n") == std::wstring::npos) continue;
            tb.origLineCount = rb.origLineCount;
            res->blocks.push_back(std::move(tb));
        }
        if (res->blocks.empty()) {
            ZLOG_INFO("translate", "no visible blocks after clipping");
            res->error = L"未识别到文字";
            break;
        }
        res->ok = true;
        ZLOG_INFO("translate", "done: %zu blocks rendered", res->blocks.size());
    } while (false);

    PublishTranslateResult(res.release(), in->overlayHwnd);
}

// ==================== 状态气泡（进度 / 错误） ====================

// 设置状态气泡文本并计算气泡矩形（backDC 相对坐标）：水平居中于选区、置于选区上沿
// 外侧（放不下转选区内顶部）。尺寸测量与工具栏 tooltip 同款（GDI DrawTextW +
// 12px×ds 微软雅黑）。调用方负责触发重绘。

static void SetTranslateStatus(CaptureContext* ctx, const wchar_t* text, bool isError) {
    ctx->translateStatusText = text ? text : L"";
    ctx->translateStatusError = isError;
    ctx->translateStatusShown = !ctx->translateStatusText.empty();
    ctx->translateStatusRect = {};
    ctx->translateStatusAt = GetTickCount();
    if (!ctx->translateStatusShown) return;

    double ds = ctx->dpiScale;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    int padX = sc(8), padY = sc(5);
    HDC screen = GetDC(NULL);
    if (screen) {
        HFONT fnt = CreateFontW(-sc(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, SC_FONT_FACE);
        HGDIOBJ old = SelectObject(screen, fnt);
        RECT tr = {0, 0, 0, 0};
        DrawTextW(screen, ctx->translateStatusText.c_str(), -1, &tr, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(screen, old);
        DeleteObject(fnt);
        ReleaseDC(NULL, screen);
        int w = tr.right - tr.left + padX * 2 + 2;
        int h = tr.bottom - tr.top + padY * 2 + 2;
        int selL = ctx->selection.left - ctx->virtualX;
        int selT = ctx->selection.top - ctx->virtualY;
        int selW = ctx->selection.right - ctx->selection.left;
        int x = selL + (selW - w) / 2;
        if (x < sc(4)) x = sc(4);
        if (x + w > ctx->virtualW - sc(4)) x = ctx->virtualW - sc(4) - w;
        int y = selT - h - sc(6);
        if (y < sc(4)) y = selT + sc(6);   // 上方放不下：转选区内顶部
        ctx->translateStatusRect = {x, y, x + w, y + h};
    }
}

// 空闲循环节拍（会话空闲循环每拍调用）：错误气泡超过 TTL 后收起（进度气泡常驻
// 至结果到达）。只失效气泡自身矩形，局部刷新。

void TickTranslateStatus(CaptureContext* ctx, HWND hwnd) {
    if (!ctx || !hwnd || !ctx->translateStatusShown || !ctx->translateStatusError) return;
    if (GetTickCount() - ctx->translateStatusAt < SC_TRANSLATE_ERROR_TTL_MS) return;
    RECT r = InflateRectBy(ctx->translateStatusRect, 2);
    ctx->translateStatusShown = false;
    ctx->translateStatusText.clear();
    InvalidateRect(hwnd, &r, FALSE);
}

// 绘制状态气泡：深色圆角底 + 居中文本（错误态淡红文本），样式与工具栏 tooltip 一致

static void DrawTranslateStatusBubble(HDC hdc, CaptureContext* ctx) {
    double ds = ctx->dpiScale;
    const RECT& rc = ctx->translateStatusRect;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   (int)(4 * ds + 0.5));
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 41, 41, 41));
    graphics.FillPath(&bg, &path);
    Gdiplus::FontFamily ff(SC_FONT_FACE);
    Gdiplus::Font fnt(&ff, (Gdiplus::REAL)(12 * ds + 0.5),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF layout((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                          (Gdiplus::REAL)(rc.right - rc.left),
                          (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::Color textColor = ctx->translateStatusError
        ? Gdiplus::Color(255, 255, 160, 160)
        : Gdiplus::Color(255, 255, 255, 255);
    Gdiplus::SolidBrush textBrush(textColor);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.DrawString(ctx->translateStatusText.c_str(), -1, &fnt, layout, &sf, &textBrush);
}

// ==================== 译文覆盖绘制（OnPaint 与导出合成共用） ====================

namespace sctranslatedraw {

// 译文面板内边距 / 圆角（逻辑像素）

static const int PANEL_PAD = 4;
static const int PANEL_RADIUS = 4;

// 译文字号搜索区间（逻辑像素）：min(能装进段框的最大字号, 原文行距对应字号)，
// 见 FitParagraph；端点只作钳制，不直接决定字号。

static const int FONT_MIN_PX = 9;
static const int FONT_MAX_PX = 48;

// 整图兜底块（无行级特征）的默认字号上限。

static const int FALLBACK_FONT_PX = 16;

// ---- 某一候选字号下的整段真实排版测量 ----
// 按段落宽度真实折行后统计：实际行数 / 行高 / 总高与高度可行性，供字号搜索
// 与最终排版复用。

struct LayoutMeasure {
    std::vector<std::wstring> lines;      // 折行结果
    float lineH = 0.0f;                   // 行高（font.GetHeight，含字体默认行距）
    float totalH = 0.0f;                  // 行数 × 行高
    bool fitsHeight = false;              // 总高 ≤ 段框高（垂直内边距由绘制侧让位）
};

// 用逐字符宽度贪心折行。字符宽按「前缀累宽差分」测量（与 GDI+ 渲染宽度同源），
// 断行点：CJK 字符后、空格后；拉丁单词整体不下断时回退硬断。
// 返回的行不含换行符；maxW 为可用文本宽度（逻辑像素）。

static std::vector<std::wstring> WrapText(Gdiplus::Graphics& g, const Gdiplus::Font& font,
                                          const std::wstring& text, float maxW) {
    std::vector<std::wstring> lines;
    size_t n = text.size();
    if (n == 0 || maxW <= 1) {
        if (n > 0) lines.push_back(text);
        return lines;
    }
    // 前缀累宽：widths[i] = 前 i 个字符的渲染宽度
    std::vector<float> widths(n + 1, 0.0f);
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF layout(0.0f, 0.0f, 65536.0f, 65536.0f), bound;
    for (size_t i = 1; i <= n; i++) {
        g.MeasureString(text.c_str(), (INT)i, &font, layout, &sf, &bound);
        widths[i] = bound.X + bound.Width;
    }
    size_t lineStart = 0;    // 当前行起点
    size_t lastBreak = 0;    // 行内最后一个可断点之后的起点（空格后 / CJK 字符边界后）
    bool hasBreak = false;
    auto breakableAfter = [&](size_t idx) {   // idx 为字符下标；返回其后是否可断
        wchar_t c = text[idx];
        return c == L' ' || IsCjkForWrap(c);
    };
    for (size_t i = 0; i < n; i++) {
        if (breakableAfter(i)) {
            lastBreak = i + 1;
            hasBreak = true;
        }
        float lineW = widths[i + 1] - widths[lineStart];
        if (lineW <= maxW) continue;
        // 溢出：优先在行内最后一个可断点断开（可断点在行首则硬断在当前字符前）
        if (hasBreak && lastBreak > lineStart) {
            lines.emplace_back(text, lineStart, lastBreak - lineStart);
            lineStart = lastBreak;
        } else if (i > lineStart) {
            lines.emplace_back(text, lineStart, i - lineStart);
            lineStart = i;
        } else {
            lines.emplace_back(text, lineStart, 1);
            lineStart = i + 1;
        }
        // 新行继承断点状态：断点前移后重扫（简单起见直接清零，行首断点无意义）
        hasBreak = false;
        lastBreak = lineStart;
        // 补齐新行起点的前缀宽度基准（widths 与 lineStart 配合已足够，无需额外处理）
    }
    if (lineStart < n) lines.emplace_back(text, lineStart, n - lineStart);
    if (lines.empty()) lines.push_back(text);
    return lines;
}

// 候选字号真实排版测量：按段宽折行 → 行高/总高 → 高度可行性。
// availW 为文本区宽（框 − 双内边距）；fitH 为高度可行域 = 段框高（非
// 「框高 − 双内边距」：OCR 框紧贴字形，单行段减去双内边距后连最小字号一行
// 都放不下，字号会被钉死在下限；内边距成为垂直余量，由绘制侧垂直居中吸收）。

static LayoutMeasure MeasureAt(Gdiplus::Graphics& g, const Gdiplus::FontFamily& ff,
                               const std::wstring& text, int fontPx,
                               float availW, float fitH) {
    LayoutMeasure m;
    Gdiplus::Font font(&ff, (Gdiplus::REAL)fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    m.lines = WrapText(g, font, text, availW);
    m.lineH = font.GetHeight(&g);
    if (m.lineH < 1.0f) m.lineH = (float)fontPx;
    m.totalH = m.lines.size() * m.lineH;
    m.fitsHeight = m.totalH <= fitH + 0.5f;
    return m;
}

// 字体排版行高比（GetHeight / fontPx，微软雅黑 ≈ 1.3）：把「原文行距」换算为
// 字号的除数（行距 ÷ 行高比 = 与该行距观感一致的字号）。运行时用探针字号实测
// 一次，不依赖具体字体的硬编码行距。

static float LineHeightRatio(Gdiplus::Graphics& g, const Gdiplus::FontFamily& ff) {
    Gdiplus::Font probe(&ff, 24.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    float h = probe.GetHeight(&g);
    return h > 1.0f ? h / 24.0f : 1.3f;
}

// 段落字号搜索（每段独立，绝不整图统一字号）：字号由段落自身几何决定，不依赖
// 「OCR 行框 → 原字号」的换算假设（行框语义随 provider 而异，紧贴字形 / 整字高
// 框都有，乘系数估算会在整字高框上系统性高估，且同页正文行高一致导致各段字号
// 被钉到同一个高估值——过大且统一）：
//   1) 可行性硬约束 = 真实排版总高不超段框高（fitH，见 MeasureAt）；字号对
//      总高单调（字号越大行高越大且行数不减），故先二分出最大可行字号 maxFit
//      （译文比原文长时收缩到装得下为止）；
//   2) 行距对齐上限 cap = 段框高 ÷ 原文行数 ÷ 行高比：译文的行距与原段落行距
//      一致，短译文（折行后行数少于原文）不会被放大去撑满段框；
//   3) 字号 = min(maxFit, cap)。段框高与行数每段不同，字号随段落区域自适应。
// 整图兜底块（origLineCount = 0，无行级特征）用默认字号上限；连下限字号都装
// 不下时退回下限（面板扩高 + 末行截断由绘制侧兜底）。
// 返回最终字号，mOut 带回该字号下的完整测量供最终排版。

static int FitParagraph(Gdiplus::Graphics& g, const Gdiplus::FontFamily& ff,
                        const std::wstring& text, int origLineCount,
                        float availW, float fitH, LayoutMeasure& mOut) {
    LayoutMeasure atMin = MeasureAt(g, ff, text, FONT_MIN_PX, availW, fitH);
    if (!atMin.fitsHeight) {              // 下限也装不下：退回下限，溢出走兜底
        mOut = std::move(atMin);
        return FONT_MIN_PX;
    }
    int maxFit = FONT_MIN_PX;
    {
        int lo = FONT_MIN_PX, hi = FONT_MAX_PX;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (MeasureAt(g, ff, text, mid, availW, fitH).fitsHeight) {
                maxFit = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
    }
    int cap = FONT_MAX_PX;
    if (origLineCount > 0) {
        cap = (int)(fitH / (float)origLineCount / LineHeightRatio(g, ff));
    } else {
        cap = FALLBACK_FONT_PX;           // 兜底块无行级特征：默认字号上限
    }
    if (cap < FONT_MIN_PX) cap = FONT_MIN_PX;
    if (cap > FONT_MAX_PX) cap = FONT_MAX_PX;
    int fontPx = maxFit < cap ? maxFit : cap;
    mOut = MeasureAt(g, ff, text, fontPx, availW, fitH);
    return fontPx;
}

// 绘制单个译文覆盖面板：白底圆角面板盖住原段落区域，整段译文按搜索出的字号
// 折行排版（行距对齐 + 框高可行性收缩，见 FitParagraph）。搜索含十次级真实排版
// 测量，结果按「绘制时框宽高」缓存在块内（选区不变时每帧直接复用）；仅当
// 连下限字号都装不下时面板才向下扩展（不超 clip）并截断末行，属兜底路径。
// tb.box/clip 为绝对逻辑坐标，ox/oy 为绘制平移（OnPaint = -虚拟屏原点，
// 导出 = -选区左上角），交集与排版均在平移后坐标系内进行。

static void DrawBlockPanel(HDC hdc, TranslateBlock& tb, const RECT& clip, float ox, float oy) {
    // 与裁剪边界（选区）求交：选区缩小后落在界外的块整体跳过，跨界块按交集绘制
    RECT box = { tb.box.left + (int)ox, tb.box.top + (int)oy,
                 tb.box.right + (int)ox, tb.box.bottom + (int)oy };
    RECT clipOff = { clip.left + (int)ox, clip.top + (int)oy,
                     clip.right + (int)ox, clip.bottom + (int)oy };
    RECT boxI = {};
    if (!IntersectRect(&boxI, &box, &clipOff)) return;
    int boxW = boxI.right - boxI.left;
    int boxH = boxI.bottom - boxI.top;
    if (boxW < 2 || boxH < 2) return;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    int pad = PANEL_PAD;
    int availW = boxW - pad * 2;
    if (availW < 8) availW = 8;
    // 高度可行域 = 段框高（非框高 − 双内边距，见 MeasureAt）；
    // 垂直内边距由绘制侧垂直居中吸收
    float fitH = (float)boxH;
    float maxPanelBottom = (float)clipOff.bottom;

    // ---- 字号搜索（带缓存：键 = 绘制时框宽高，几何变化才重排）----
    if (tb.fitKeyW != boxW || tb.fitKeyH != boxH || tb.fitLines.empty()
        || tb.fitFontPx < FONT_MIN_PX) {
        LayoutMeasure m;
        int fontPx = FitParagraph(graphics, Gdiplus::FontFamily(SC_FONT_FACE), tb.text,
                                  tb.origLineCount, (float)availW, fitH, m);
        tb.fitKeyW = boxW;
        tb.fitKeyH = boxH;
        tb.fitFontPx = fontPx;
        tb.fitLineH = m.lineH;
        tb.fitLines = std::move(m.lines);
    }

    int fontPx = tb.fitFontPx;
    float lineH = tb.fitLineH > 0.5f ? tb.fitLineH : (float)fontPx;
    const std::vector<std::wstring>& lines = tb.fitLines;

    // 面板高度：文字总高不超过段框高（字号搜索的可行性约束）时严格取原段落高
    // （覆盖框即原段框，保持原版面占位），文字垂直居中、垂直内边距让位；
    // 下限字号仍溢出（可行性早退的兜底路径）才向下扩展到裁剪边界
    float textH = lines.size() * lineH;
    float panelH = (float)boxH;
    if (textH > panelH) {
        panelH = textH + pad * 2;
        if (boxI.top + panelH > maxPanelBottom) panelH = maxPanelBottom - boxI.top;
        if (panelH < (float)boxH) panelH = (float)boxH;
    }
    float padY = (panelH - textH) / 2;
    if (padY < 0) padY = 0;

    // 面板：近实心白底盖住原文字 + 浅灰描边
    Gdiplus::RectF panel((Gdiplus::REAL)boxI.left, (Gdiplus::REAL)boxI.top,
                         (Gdiplus::REAL)boxW, panelH);
    {
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, (int)panel.X, (int)panel.Y, (int)panel.Width, (int)panel.Height,
                       PANEL_RADIUS);
        Gdiplus::SolidBrush fill(Gdiplus::Color(250, 255, 255, 255));
        graphics.FillPath(&fill, &path);
        Gdiplus::Pen border(Gdiplus::Color(255, 205, 205, 205), 1.0f);
        graphics.DrawPath(&border, &path);
    }

    // 逐行绘制译文：可见行数受面板高度约束；放不下的行截断为「…」
    Gdiplus::FontFamily ff(SC_FONT_FACE);
    Gdiplus::Font font(&ff, (Gdiplus::REAL)fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 51, 51, 51));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentNear);
    sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(Gdiplus::StringTrimmingNone);

    // 默认全显（可行字号必在框内），仅兜底扩高被裁剪边界压回时按剩余高度截断
    int visible = (int)lines.size();
    if ((float)visible * lineH > panelH - padY * 2 + 0.5f) {
        visible = (int)((panelH - padY * 2) / lineH);
        if (visible < 1) visible = 1;
    }
    float y = panel.Y + padY;
    for (int i = 0; i < (int)lines.size() && i < visible; i++) {
        std::wstring line = lines[i];
        if (i == visible - 1 && (size_t)visible < lines.size()) {
            // 还有未展示的行：当前行末尾追加省略号
            line += L"…";
        }
        Gdiplus::RectF layout(panel.X + pad, y, (Gdiplus::REAL)availW, lineH);
        graphics.DrawString(line.c_str(), (INT)line.size(), &font, layout, &sf, &textBrush);
        y += lineH;
    }
}

}  // namespace sctranslatedraw

// 在指定 DC 上按偏移绘制全部译文覆盖块。hdc 的坐标系为逻辑像素：
//   - OnPaint（backDC）：origin = 虚拟屏幕左上角，ox/oy = -virtualX/-virtualY
//   - 导出合成（finalDC）：origin = 选区左上角，ox/oy = -rect.left/-rect.top
// clip 为选区矩形（绝对逻辑坐标），约束面板向下扩展的边界。

static void DrawTranslateBlocksAt(HDC hdc, std::vector<TranslateBlock>& blocks,
                                  const RECT& clip, float ox, float oy) {
    // 逐块绘制：块坐标为绝对值，平移量直接传给面板（交集/排版在平移后坐标系内
    // 进行，字号搜索缓存写回块本身，几何不变时每帧复用）
    for (TranslateBlock& tb : blocks) {
        sctranslatedraw::DrawBlockPanel(hdc, tb, clip, ox, oy);
    }
}

// OnPaint 入口：确认态下绘制译文覆盖（结果块）与状态气泡（进度/错误）。
// 画在标注之上、选区轮廓/工具栏/tooltip 之下。

void DrawTranslateOverlay(HDC hdc, CaptureContext* ctx) {
    if (!ctx) return;
    if (ctx->translateState == TRL_Shown && !ctx->translateBlocks.empty()) {
        RECT clip = ctx->selection;
        DrawTranslateBlocksAt(hdc, ctx->translateBlocks, clip,
                              (float)-ctx->virtualX, (float)-ctx->virtualY);
    }
    if (ctx->translateStatusShown && !ctx->translateStatusText.empty()
        && IsValidRect(ctx->translateStatusRect)) {
        DrawTranslateStatusBubble(hdc, ctx);
    }
}

// 导出合成入口（确认提取/保存落盘共用）：把译文覆盖块合成进最终位图。
// finalDC 原点 = 选区左上角（逻辑像素，已完成 DPI 下采样）。

void CompositeTranslateBlocks(HDC finalDC, const std::vector<TranslateBlock>& blocks,
                              const RECT& rect) {
    if (!finalDC || blocks.empty()) return;
    // 排版缓存需要可写块：导出为一次性路径，拷贝后缓存不回写调用方
    std::vector<TranslateBlock> mut = blocks;
    DrawTranslateBlocksAt(finalDC, mut, rect, (float)-rect.left, (float)-rect.top);
}

// ==================== 点击入口与结果接管 ====================

// 清除已展示的译文覆盖（重跑翻译前 / 交互需要时调用）。仅清状态，不负责重绘。

static void ClearTranslateOverlay(CaptureContext* ctx) {
    ctx->translateBlocks.clear();
    if (ctx->translateState == TRL_Shown) ctx->translateState = TRL_Idle;
}

// 工具栏「翻译」按钮点击入口（截图线程调用）：裁剪选区 → 编码 PNG → 转入进行中
// 状态并起工作线程。重复点击忽略（进行中）/ 退出翻译状态并清掉覆盖（已展示，与
// 矩形/文字等工具一致的切换语义）。返回 false 表示未能发起（裁剪/编码失败等），
// 调用方无需额外处理。

bool BeginTranslateOverlay(CaptureContext* ctx, HWND overlayHwnd) {
    if (!ctx) return false;
    if (ctx->translateState == TRL_Busy) return false;   // 进行中忽略重复点击
    if (ctx->translateState == TRL_Shown) {
        // 展示中再次点击：退出翻译状态（清除译文覆盖并取消按钮激活），不重跑翻译
        ClearTranslateOverlay(ctx);
        ctx->activeTool = -1;
        InvalidateRect(overlayHwnd, NULL, FALSE);
        return true;
    }
    RECT sel = ctx->selection;
    int w = sel.right - sel.left, h = sel.bottom - sel.top;
    if (w <= 0 || h <= 0) return false;
    double ds = ctx->dpiScale;

    // ---- 物理分辨率裁剪选区（预截屏位图 → 独立 32bpp DIB）----
    // 坐标换算与导出管线同式；GDI 调用全部在本线程完成，工作线程只拿 base64。
    int px = (int)((sel.left - ctx->virtualX) * ds + 0.5);
    int py = (int)((sel.top - ctx->virtualY) * ds + 0.5);
    int pw = (int)(w * ds + 0.5);
    int ph = (int)(h * ds + 0.5);
    if (pw <= 0 || ph <= 0) return false;

    std::string imageBase64;
    {
        HDC screenDC = GetDC(NULL);
        if (!screenDC) return false;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = pw;
        bi.bmiHeader.biHeight = -ph;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP cropBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HDC cropDC = cropBmp ? CreateCompatibleDC(screenDC) : NULL;
        // 必须把 DIB 选进内存 DC（与导出管线同式）：否则 BitBlt 写入默认 1x1 单色位图，
        // DIB 保持全零，编码出纯黑图（BitBlt 仍返回 TRUE，不会被 captured 判定拦截）
        HGDIOBJ oldCropBmp = cropDC ? SelectObject(cropDC, cropBmp) : NULL;
        bool captured = cropDC && bits &&
                        BitBlt(cropDC, 0, 0, pw, ph, ctx->memDC, px, py, SRCCOPY);
        if (captured) {
            // BitBlt 后 alpha 字节不可靠（与导出管线同因），统一置 255 再编码
            BYTE* p = (BYTE*)bits;
            for (int i = 0; i < pw * ph; i++) p[i * 4 + 3] = 255;
            imageBase64 = BitmapToBase64Png(cropBmp);
        }
        if (cropDC && oldCropBmp) SelectObject(cropDC, oldCropBmp);
        if (cropDC) DeleteDC(cropDC);
        if (cropBmp) DeleteObject(cropBmp);
        ReleaseDC(NULL, screenDC);
    }
    if (imageBase64.empty()) {
        SetTranslateStatus(ctx, L"选区图像提取失败", true);
        InvalidateRect(overlayHwnd, NULL, FALSE);
        return false;
    }

    // ---- 转入进行中：高亮按钮 + 进度气泡 ----
    ctx->translateState = TRL_Busy;
    ctx->activeTool = TB_Translate;
    SetTranslateStatus(ctx, L"正在识别并翻译…", false);
    InvalidateRect(overlayHwnd, NULL, FALSE);

    // ---- 起工作线程：入参所有权随 detach 转移 ----
    auto* job = new TranslateJobInput();
    job->imageBase64 = std::move(imageBase64);
    job->imageW = pw;
    job->imageH = ph;
    job->selection = sel;
    job->dpiScale = ds;
    job->overlayHwnd = overlayHwnd;
    std::thread(TranslateWorkerMain, job).detach();
    return true;
}

// WM_SCREENSHOT_TRANSLATE_RESULT 接管（截图线程调用）：取走结果槽负载并落到
// ctx——成功 → 译文块存入并进入展示态；失败 → 状态回退并弹出错误气泡（TTL 自动消失）。

void HandleTranslateJobResult(CaptureContext* ctx, HWND hwnd) {
    if (!ctx) return;
    TranslateJobResult* res = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_translateSlotMutex);
        res = g_translateSlot;
        g_translateSlot = nullptr;
    }
    if (!res) return;
    std::unique_ptr<TranslateJobResult> guard(res);
    // 会话收尾后理论上不会再收到消息；状态非进行中（已取消/已清）时静默丢弃
    if (ctx->translateState != TRL_Busy) return;

    if (!res->ok) {
        ctx->translateState = TRL_Idle;
        ctx->activeTool = -1;
        SetTranslateStatus(ctx, res->error.c_str(), true);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    ctx->translateBlocks = std::move(res->blocks);
    ctx->translateState = TRL_Shown;
    // 收起进度气泡（保留 activeTool 高亮：覆盖展示中按钮保持激活观感）
    ctx->translateStatusShown = false;
    ctx->translateStatusText.clear();
    InvalidateRect(hwnd, NULL, FALSE);
}
