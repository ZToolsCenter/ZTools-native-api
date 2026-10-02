// 截图翻译子系统：文本行聚类（translate_cluster.cpp）离线自测。
// 不依赖 node-addon / 平台 UI：直接与纯算法单元一起编译为独立可执行文件，
// 断言段落划分、联合框、行高平滑与 CJK/拉丁拼接规则。
//
// 运行（MSVC，自动调用 vcvars）：
//   scripts\run-translate-cluster-selftest.cmd
// 或任意编译器手动编译：
//   cl /EHsc /std:c++17 /utf-8 /I src\screenshot\algo ^
//      test\translate-cluster-selftest.cpp src\screenshot\algo\translate_cluster.cpp
#include "translate_cluster_internal.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_total = 0;
static int g_failed = 0;

// 断言：失败时打印定位与说明，不中断后续用例
#define TC_CHECK(cond, msg)                                              \
    do {                                                                 \
        g_total++;                                                       \
        if (!(cond)) {                                                   \
            g_failed++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));  \
        }                                                                \
    } while (0)

// 构造一行（左上角 + 宽高）
static TcLine MkLine(const char* text, double x, double y, double w, double h) {
    TcLine l;
    l.text = text;
    l.x = x;
    l.y = y;
    l.w = w;
    l.h = h;
    return l;
}

// 断言段落的成员行下标（输入原始下标，按阅读序）
static void ExpectMembers(const TcParagraph& p, const std::vector<int>& want, const char* tag) {
    TC_CHECK(p.lineIdx == want, tag);
}

int main() {
    // ---- T1：紧排中文同段（vGap≈0、左缘对齐、右缘对齐）→ 3 行并 1 段，
    //      文本无空格直连（CJK 拼接规则）----
    {
        std::vector<TcLine> in = {
            MkLine("\u7b2c\u4e00\u884c", 100, 0, 600, 20),
            MkLine("\u7b2c\u4e8c\u884c", 100, 22, 600, 20),
            MkLine("\u7b2c\u4e09\u884c", 100, 44, 600, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 1, "T1 one paragraph");
        ExpectMembers(r.paragraphs[0], {0, 1, 2}, "T1 members");
        TC_CHECK(r.paragraphs[0].x == 100 && r.paragraphs[0].y == 0, "T1 box origin");
        TC_CHECK(r.paragraphs[0].w == 600 && r.paragraphs[0].h == 64, "T1 box size");
        TC_CHECK(r.paragraphs[0].lineHeight == 20, "T1 line height");
        TC_CHECK(r.paragraphs[0].text == "\u7b2c\u4e00\u884c\u7b2c\u4e8c\u884c\u7b2c\u4e09\u884c",
                 "T1 cjk join without spaces");
        TC_CHECK(r.boundaryCount == 0, "T1 no boundary");
    }

    // ---- T2：两端对齐英文两段（末行短 + 段间空隙）→ R4 段末短行切段，
    //      段内拉丁词空格拼接 ----
    {
        std::vector<TcLine> in = {
            MkLine("The quick brown", 100, 0, 600, 20),
            MkLine("fox jumps over", 100, 22, 600, 20),
            MkLine("the lazy dog", 100, 44, 600, 20),
            MkLine("end.", 100, 66, 200, 20),      // 段末短行（r=300）
            MkLine("New paragraph", 100, 96, 600, 20),  // 段间距 vGap=10
            MkLine("starts here.", 100, 118, 550, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T2 two paragraphs");
        ExpectMembers(r.paragraphs[0], {0, 1, 2, 3}, "T2 para1 members");
        ExpectMembers(r.paragraphs[1], {4, 5}, "T2 para2 members");
        TC_CHECK(r.paragraphs[0].text ==
                 "The quick brown fox jumps over the lazy dog end.", "T2 latin join");
        TC_CHECK(r.paragraphs[1].text == "New paragraph starts here.", "T2 para2 join");
        TC_CHECK(r.boundaryCount == 1, "T2 one boundary");
    }

    // ---- T3：紧排中文缩进分界（vGap≈0、第二段首行缩进 2 字）→ R3 左缘跳变切段 ----
    {
        std::vector<TcLine> in = {
            MkLine("\u7b2c\u4e00\u6bb5", 100, 0, 600, 20),
            MkLine("\u7b2c\u4e8c\u884c", 100, 22, 600, 20),
            MkLine("\u6bb5\u843d\u672b\u5c3e", 100, 44, 600, 20),
            MkLine("\u7f29\u8fdb\u65b0\u6bb5", 140, 66, 560, 20),  // 左缘 +40 = 2×medianH
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T3 indent splits");
        ExpectMembers(r.paragraphs[0], {0, 1, 2}, "T3 para1 members");
        ExpectMembers(r.paragraphs[1], {3}, "T3 para2 members");
    }

    // ---- T4：标题 + 正文（行高/行距跳变）→ 分开；正文段行高按成员中位数
    //      平滑为 20（不被标题的 40 抬高）----
    {
        std::vector<TcLine> in = {
            MkLine("Chapter One", 100, 0, 600, 40),
            MkLine("body line 1", 100, 54, 600, 20),
            MkLine("body line 2", 100, 76, 600, 20),
            MkLine("body line 3", 100, 98, 600, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T4 title separated");
        ExpectMembers(r.paragraphs[0], {0}, "T4 title members");
        ExpectMembers(r.paragraphs[1], {1, 2, 3}, "T4 body members");
        TC_CHECK(r.paragraphs[0].lineHeight == 40, "T4 title line height");
        TC_CHECK(r.paragraphs[1].lineHeight == 20, "T4 body line height smoothed");
        TC_CHECK(r.medianLineH == 20, "T4 median height");
    }

    // ---- T5：双栏并排（横向无重叠）→ 各自成段互不串联 ----
    {
        std::vector<TcLine> in = {
            MkLine("col1 a", 100, 0, 280, 20),
            MkLine("col1 b", 100, 22, 280, 20),
            MkLine("col1 c", 100, 44, 280, 20),
            MkLine("col2 a", 420, 0, 280, 20),
            MkLine("col2 b", 420, 22, 280, 20),
            MkLine("col2 c", 420, 44, 280, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T5 two columns");
        ExpectMembers(r.paragraphs[0], {0, 1, 2}, "T5 col1 members");
        ExpectMembers(r.paragraphs[1], {3, 4, 5}, "T5 col2 members");
    }

    // ---- T6：单行 → 单行段落，行高即行高 ----
    {
        std::vector<TcLine> in = {MkLine("  only one  ", 50, 60, 300, 24)};
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 1, "T6 one paragraph");
        ExpectMembers(r.paragraphs[0], {0}, "T6 members");
        TC_CHECK(r.paragraphs[0].lineHeight == 24, "T6 line height");
        TC_CHECK(r.paragraphs[0].text == "only one", "T6 trimmed text");
    }

    // ---- T7：孤行吸收——左栏行距 36 超过被右栏密集行距拉低的中位数阈值而误切，
    //      误切出的单行段是满宽行（右端≈后段右缘）→ 二次修正并回 ----
    {
        std::vector<TcLine> in = {
            MkLine("l1", 100, 0, 280, 20),
            MkLine("l2", 100, 36, 280, 20),    // pitch 36 > 1.5×pitchRef(22)
            MkLine("l3", 100, 58, 280, 20),
            MkLine("c1", 420, 0, 280, 20),     // 右栏紧排，拉低 pitchRef
            MkLine("c2", 420, 22, 280, 20),
            MkLine("c3", 420, 44, 280, 20),
            MkLine("c4", 420, 66, 280, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T7 orphan absorbed");
        ExpectMembers(r.paragraphs[0], {0, 1, 2}, "T7 col1 merged");
        ExpectMembers(r.paragraphs[1], {3, 4, 5, 6}, "T7 col2 members");
    }

    // ---- T8：行数上限——25 行紧排长链截断为 20 + 5 ----
    {
        std::vector<TcLine> in;
        for (int i = 0; i < 25; i++) in.push_back(MkLine("x", 100, i * 22.0, 600, 20));
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 2, "T8 capped");
        TC_CHECK(r.paragraphs[0].lineIdx.size() == 20, "T8 first cap 20");
        TC_CHECK(r.paragraphs[1].lineIdx.size() == 5, "T8 remainder 5");
    }

    // ---- T9：左对齐（右边不齐）排版——段内随机短行不触发 R4（vGap 紧排）----
    {
        std::vector<TcLine> in = {
            MkLine("ragged long line here", 100, 0, 600, 20),
            MkLine("shorter", 100, 22, 400, 20),   // 右端短 200，但 vGap=2 紧排
            MkLine("another full line", 100, 44, 600, 20),
        };
        TcClusterResult r = TcClusterLines(in);
        TC_CHECK(r.paragraphs.size() == 1, "T9 ragged stays one paragraph");
        ExpectMembers(r.paragraphs[0], {0, 1, 2}, "T9 members");
    }

    std::printf("%s: %d checks, %d failed\n",
                g_failed == 0 ? "PASS" : "FAIL", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
