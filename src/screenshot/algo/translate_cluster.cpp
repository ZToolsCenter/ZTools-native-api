// 截图翻译子系统：OCR 文本行 → 段落的空间聚类实现（契约见
// translate_cluster_internal.h）。
//
// 整体流水线：
//   1) 基础数据预处理：抽取每行几何量（左/右/上/下/宽/高），按 (top,left)
//      排成阅读序，后续所有逻辑都在该序列上进行；
//   2) 估计页面正常行高：全部行高的中位数（对标题/脚注等离群行高鲁棒）；
//   3) 按垂直位置建立候选相邻关系：每行找「正下方且横向有实质重叠的最近行」
//      作为唯一相邻判定对象——横向无重叠的另一栏不会成为候选，天然适配
//      多栏版面；同行（侧边并排）的行被「下方偏移」条件排除；
//   4) 计算相邻文本行的多维特征：行距 pitch（上缘到上缘）/ 几何空隙 vGap
//      （上行下缘到下行上缘）/ 行高比 hRatio / 左缘差 leftDiff / 右端差
//      rightShort（下行右缘超过上行右缘的量，段末短行信号）；
//   5) 判断「连续关系」还是「段落边界」：R1~R4 阈值规则（见 k* 常量组注释）
//      全部通过才判连续，任一命中即边界；
//   6) 将连续文本行聚合成段落：连续判定构成单向链（每行至多一进一出），
//      无前驱的行作段首沿链收集成员，段行数超上限处强制截断；
//   7) 对段落结果进行二次修正：孤行吸收（单行段若与后段紧排/对齐/同高且
//      右端接近后段右缘——即满宽行而非段末短行——则并回，典型目标是行距
//      中位数被版面混排污染导致的误切）+ 段行高以成员中位数平滑（标题混入
//      时不抬高正文段的字号）。
//
// 本文件为纯算法：只依赖 translate_cluster_internal.h 与 C++ 标准库，
// 不触碰 internal.h（napi / GDI+ 依赖链）；所有调参常量集中在下方常量组
// 定义并附判定语义，供真实数据驱动的阈值调优。
#include "translate_cluster_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

// ==================== 调参常量（尺度为 ×medianH / ×pitchRef） ====================

// R1 行高一致：相邻行高之比低于该值的对（标题↔正文、正文↔脚注）判边界

constexpr double kHeightRatioMin = 0.62;

// R2 垂直节奏：pitch 超过 1.5×max(pitchRef, medianH) 判边界；几何空隙 vGap
// 超过 max(0.9×medianH, 0.55×pitchRef) 判边界（对段落间距最可靠的信号，
// 紧排中文文档 vGap≈0 不触发，靠缩进/短行判定）；pitch 绝对上限 3.5×medianH
// 兜底（行距估计被污染时仍保住「离谱即边界」）

constexpr double kPitchTolerance = 1.5;
constexpr double kGapRatioMaxH = 0.9;
constexpr double kGapRatioMaxPitch = 0.55;
constexpr double kPitchAbsMaxRatio = 3.5;

// R3 左缘对齐：相邻行左缘差超过 0.7×medianH 判边界（段首缩进/悬挂缩进/
// 换栏都会表现为左缘跳变；段内行的左缘抖动远小于该容差）

constexpr double kLeftAlignTolerance = 0.7;

// R4 段末短行：上行右端比下行右端短 ≥2×medianH 且行间略有空隙
// （vGap ≥ 0.25×medianH）判边界——两端对齐排版的段末行显著短于下一行，
// 是段边界信号；紧排（vGap≈0）时不触发（中文密排文档靠 R3 缩进判段），
// 避免左对齐（右边不齐）排版的段内随机短行被误切

constexpr double kShortEndWidth = 2.0;
constexpr double kShortEndGap = 0.25;

// 候选相邻判定：横向重叠 ≥ 0.2×min(两行宽)；下行上缘需比本行上缘低
// ≥ 0.5×min(两行高)（排除同一视觉行内侧边并排的行）

constexpr double kOverlapRatioMin = 0.2;
constexpr double kBelowOffsetRatio = 0.5;

// 段行数上限：超过处在链上强制截断（防病态整栏合并，也约束译文面板规模）

constexpr int kMaxParaLines = 20;

// 二次修正——孤行吸收：单行段 A 紧跟段 B，若 vGap ≤ 0.9×medianH、行高比
// ≥ 0.7、左缘差 ≤ 0.3×medianH、且 A 右端已达 B 最大右缘 - 1.5×medianH
// （满宽行，非段末短行），则并回 B。四个条件合取：只回收被行距阈值误切的
// 满宽行，真正的独立短段（右端远离后段右缘）与 R4 判出的段末短行均不回收

constexpr double kOrphanGapMax = 0.9;
constexpr double kOrphanHeightMin = 0.7;
constexpr double kOrphanLeftTol = 0.3;
constexpr double kOrphanRightTol = 1.5;

// ==================== 小工具 ====================

// 行几何量：聚类内部按阅读序操作的视图（text 指回输入，避免整段拷贝）

struct LineBox {
    int src = -1;                          // 输入 lines 的原始下标
    double l = 0, t = 0, r = 0, b = 0;     // 左/上/右/下（x、x+w、y、y+h）
    double w = 0, h = 0;
    const std::string* text = nullptr;
};

// 中位数：空输入返回 0；偶数个取中间两数均值（nth_element 部分排序）

double MedianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)mid, v.end());
    double upper = v[mid];
    if (v.size() % 2 == 1) return upper;
    double lower = *std::max_element(v.begin(), v.begin() + (std::ptrdiff_t)mid);
    return (lower + upper) / 2.0;
}

// 取 UTF-8 字符串首码点（无效/截断序列返回 0）

unsigned DecodeFirstCodepoint(const std::string& s) {
    if (s.empty()) return 0;
    unsigned char c0 = (unsigned char)s[0];
    size_t need = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
    if (need > s.size()) return 0;
    unsigned cp = c0 >= 0xF0 ? (c0 & 0x07u) : c0 >= 0xE0 ? (c0 & 0x0Fu)
                 : c0 >= 0xC0 ? (c0 & 0x1Fu) : c0;
    for (size_t i = 1; i < need; i++) {
        unsigned char ci = (unsigned char)s[i];
        if ((ci & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (ci & 0x3Fu);
    }
    return cp;
}

// 取 UTF-8 字符串末码点：从尾部回溯越过 continuation 字节（10xxxxxx，至多 3 个）
// 找到起始字节后按首码点解码

unsigned DecodeLastCodepoint(const std::string& s) {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    size_t back = 0;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80 && back < 3) {
        i--;
        back++;
    }
    return DecodeFirstCodepoint(s.substr(i));
}

// 是否 CJK 码点（部首/注音/表意区/兼容表意/全角形式/扩展区；与
// translate_windows.cpp 的 IsCjkForWrap 同源判定，供拼接规则使用）

bool IsCjkCodepoint(unsigned cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF)
        || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFF00 && cp <= 0xFFEF)
        || (cp >= 0x20000 && cp <= 0x2FA1F);
}

// 去首尾 ASCII 空白

std::string TcTrim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

// 段落文本拼接：成员行按阅读序连接，CJK ↔ 任意文字直接连接（中文换行处
// 无空格），拉丁词之间补一个空格；空白成员跳过

std::string JoinParaText(const std::vector<LineBox>& seq, const std::vector<int>& members) {
    std::string joined;
    for (int m : members) {
        std::string piece = TcTrim(*seq[m].text);
        if (piece.empty()) continue;
        if (!joined.empty()
            && !IsCjkCodepoint(DecodeLastCodepoint(joined))
            && !IsCjkCodepoint(DecodeFirstCodepoint(piece))) {
            joined += ' ';
        }
        joined += piece;
    }
    return joined;
}

}  // namespace

// ==================== 主流程 ====================

// 文本行 → 段落聚类主入口（流水线步骤见文件头部注释）。

TcClusterResult TcClusterLines(const std::vector<TcLine>& lines) {
    TcClusterResult out;
    const int n = (int)lines.size();
    if (n == 0) return out;

    // ---- 1) 基础数据预处理：几何量抽取 + 按 (top,left) 排阅读序 ----
    std::vector<LineBox> seq;
    seq.reserve(n);
    for (int i = 0; i < n; i++) {
        LineBox lb;
        lb.src = i;
        lb.l = lines[i].x;
        lb.t = lines[i].y;
        lb.w = lines[i].w;
        lb.h = lines[i].h;
        lb.r = lb.l + lb.w;
        lb.b = lb.t + lb.h;
        lb.text = &lines[i].text;
        seq.push_back(lb);
    }
    std::sort(seq.begin(), seq.end(), [](const LineBox& a, const LineBox& b) {
        if (a.t != b.t) return a.t < b.t;
        return a.l < b.l;
    });

    // ---- 2) 估计页面正常行高：全部行高中位数 ----
    std::vector<double> heights;
    heights.reserve(n);
    for (const LineBox& s : seq) heights.push_back(s.h);
    const double medianH = MedianOf(heights);
    out.medianLineH = medianH;

    // ---- 3) 候选相邻：每行找「正下方且横向有实质重叠的最近行」 ----
    // O(n²) 双扫（截图选区内文本行数量级 ~百，可忽略）；横向无重叠的行
    // （另一栏/另一列）不进入候选，同行并排的行被下方偏移条件排除。
    std::vector<int> below(n, -1);
    for (int i = 0; i < n; i++) {
        double bestTop = std::numeric_limits<double>::max();
        int best = -1;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            const double minH2 = (std::min)(seq[i].h, seq[j].h);
            if (seq[j].t - seq[i].t < kBelowOffsetRatio * minH2) continue;
            const double overlap =
                (std::min)(seq[i].r, seq[j].r) - (std::max)(seq[i].l, seq[j].l);
            if (overlap < kOverlapRatioMin * (std::min)(seq[i].w, seq[j].w)) continue;
            if (seq[j].t < bestTop) {
                bestTop = seq[j].t;
                best = j;
            }
        }
        below[i] = best;
    }

    // ---- 3b) 估计页面正常行距：行高一致相邻对的 pitch 中位数 ----
    // 过滤行高比不达标（kHeightRatioMin）的对，避免「大标题→正文」类离群对
    // 抬高行距估计；无可统计对时 pitchRef=0（后续阈值退化为按行高尺度）。
    std::vector<double> pitches;
    for (int i = 0; i < n; i++) {
        const int j = below[i];
        if (j < 0) continue;
        if ((std::min)(seq[i].h, seq[j].h)
            < kHeightRatioMin * (std::max)(seq[i].h, seq[j].h)) continue;
        pitches.push_back(seq[j].t - seq[i].t);
    }
    const double pitchRef = MedianOf(pitches);
    out.medianPitch = pitchRef;
    const double pitchMax = kPitchTolerance * (std::max)(pitchRef, medianH);
    const double gapMax = (std::max)(kGapRatioMaxH * medianH, kGapRatioMaxPitch * pitchRef);

    // ---- 4)+5) 相邻对多维特征与连续/边界判定 ----
    // link[i]：i 判定为连续的下邻行；pred[j]：j 的连续前驱（每行至多一进一出，
    // 后到的前驱候选让位给先到者——同列中只有最近的上行会选中同一下行）。
    std::vector<int> link(n, -1), pred(n, -1);
    for (int i = 0; i < n; i++) {
        const int j = below[i];
        if (j < 0 || pred[j] >= 0) continue;
        const double pitch = seq[j].t - seq[i].t;
        const double vGap = seq[j].t - seq[i].b;
        const double hRatio =
            (std::min)(seq[i].h, seq[j].h) / (std::max)(seq[i].h, seq[j].h);
        const double leftDiff = seq[j].l - seq[i].l;
        const double rightShort = seq[j].r - seq[i].r;
        const bool continuous =
            hRatio >= kHeightRatioMin                                         // R1 行高一致
            && pitch <= pitchMax                                              // R2a 行距节奏
            && pitch <= kPitchAbsMaxRatio * medianH                           // R2c 行距绝对上限
            && vGap <= gapMax                                                 // R2b 几何空隙
            && std::fabs(leftDiff) <= kLeftAlignTolerance * medianH          // R3 左缘对齐
            && !(rightShort >= kShortEndWidth * medianH                       // R4 段末短行
                 && vGap >= kShortEndGap * medianH);
        if (!continuous) {
            out.boundaryCount++;
            continue;
        }
        link[i] = j;
        pred[j] = i;
    }

    // ---- 6) 聚合成段：未消费行作段首沿 link 链收集成员；行数超上限截断 ----
    // 扫描按阅读序（t 升序），链方向严格向下，故到达任一未消费行时其上游链
    // 必已消费完毕；截断后链上剩余节点由外层循环作为新段首接管，行不丢失。
    std::vector<std::vector<int>> paras;
    {
        std::vector<bool> consumed(n, false);
        for (int i = 0; i < n; i++) {
            if (consumed[i]) continue;
            std::vector<int> members;
            int cur = i;
            while (cur >= 0 && !consumed[cur]) {
                consumed[cur] = true;
                members.push_back(cur);
                if ((int)members.size() >= kMaxParaLines) break;
                cur = link[cur];
            }
            paras.push_back(std::move(members));
        }
    }

    // ---- 7) 二次修正——孤行吸收 ----
    // 单行段 A 的候选下邻（below）所在段 B 若满足吸收四条件（见 kOrphan*
    // 常量注释），则把 A 的行并入 B 的头部。按空间下邻而非段列表顺序寻找
    // 吸收对象：多栏版面的阅读序交错，列表相邻的两段未必空间相邻。
    // 合并后可能与前段再构成吸收关系，循环至稳定。
    bool merged = true;
    while (merged) {
        merged = false;
        std::vector<int> paraOf(n, -1);   // seq 下标 → 段下标（合并后重建）
        for (size_t pi = 0; pi < paras.size(); pi++) {
            for (int m : paras[pi]) paraOf[m] = (int)pi;
        }
        for (size_t a = 0; a < paras.size() && !merged; a++) {
            const std::vector<int>& A = paras[a];
            if (A.size() != 1) continue;
            const LineBox& la = seq[A[0]];
            const int j = below[A[0]];
            if (j < 0) continue;
            const int bp = paraOf[j];
            if (bp < 0 || bp == (int)a) continue;
            const std::vector<int>& B = paras[bp];
            if ((int)(A.size() + B.size()) > kMaxParaLines) continue;
            const LineBox& lb = seq[j];
            const double vGap = lb.t - la.b;
            const double hRatio = (std::min)(la.h, lb.h) / (std::max)(la.h, lb.h);
            double maxRightB = lb.r;
            for (int m : B) maxRightB = (std::max)(maxRightB, seq[m].r);
            if (vGap <= kOrphanGapMax * medianH
                && hRatio >= kOrphanHeightMin
                && std::fabs(lb.l - la.l) <= kOrphanLeftTol * medianH
                && la.r >= maxRightB - kOrphanRightTol * medianH) {
                std::vector<int> combined;
                combined.reserve(A.size() + B.size());
                combined.insert(combined.end(), A.begin(), A.end());
                combined.insert(combined.end(), B.begin(), B.end());
                paras[bp] = std::move(combined);
                paras.erase(paras.begin() + (std::ptrdiff_t)a);
                merged = true;
            }
        }
    }

    // 吸收可能把段首上移（列表顺序不再等于阅读序），按各段首成员位置重排
    std::stable_sort(paras.begin(), paras.end(),
                     [&seq](const std::vector<int>& a, const std::vector<int>& b) {
                         if (seq[a[0]].t != seq[b[0]].t) return seq[a[0]].t < seq[b[0]].t;
                         return seq[a[0]].l < seq[b[0]].l;
                     });

    // ---- 8) 产出段落对象：联合框 / 行高中位数平滑 / 文本拼接 ----
    out.paragraphs.reserve(paras.size());
    for (const std::vector<int>& members : paras) {
        TcParagraph p;
        double L = std::numeric_limits<double>::max();
        double T = std::numeric_limits<double>::max();
        double R = std::numeric_limits<double>::lowest();
        double Bm = std::numeric_limits<double>::lowest();
        std::vector<double> memberHeights;
        memberHeights.reserve(members.size());
        for (int m : members) {
            const LineBox& s = seq[m];
            L = (std::min)(L, s.l);
            T = (std::min)(T, s.t);
            R = (std::max)(R, s.r);
            Bm = (std::max)(Bm, s.b);
            memberHeights.push_back(s.h);
            p.lineIdx.push_back(s.src);
        }
        p.x = L;
        p.y = T;
        p.w = R - L;
        p.h = Bm - T;
        p.lineHeight = MedianOf(memberHeights);
        p.text = JoinParaText(seq, members);
        out.paragraphs.push_back(std::move(p));
    }
    return out;
}
