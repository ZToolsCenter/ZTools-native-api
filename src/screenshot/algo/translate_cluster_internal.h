// 截图翻译子系统：OCR 文本行 → 段落的空间聚类（纯算法，跨平台）。
//
// 拆分动机：OCR 识别结果只按行给出（行级坐标 + 行文本），逐行翻译会把一句
// 话的多个分行拆散翻译、逐行覆盖会把一个段落渲染成多个碎面板。本模块按
// 空间关系把「同一段落」的连续文本行聚类还原，供调用方（translate_windows.cpp）
// 以段落为单位发起翻译并渲染整段覆盖面板。
//
// 与长截图的 lc_match_core.cpp 同级的纯算法单元：只依赖 C++ 标准库，不触碰
// internal.h（napi / GDI+ 依赖链），双平台可共用；输入输出均为纯数据结构，
// 天然可单测（test/translate-cluster-selftest.cpp）。
#pragma once

#include <string>
#include <vector>

// 聚类输入：一行 OCR 文本（提交图像内像素坐标；x/y 为左上角，w/h 为宽高，
// 与 translate_windows.cpp 的 OcrRawBlock 同形，由调用方直接转换）

struct TcLine {
    std::string text;
    double x = 0, y = 0, w = 0, h = 0;
};

// 聚类产出的一个段落

struct TcParagraph {
    std::vector<int> lineIdx;               // 成员行下标（输入 lines 的原始下标，按阅读序）
    double x = 0, y = 0, w = 0, h = 0;      // 成员行包围盒并集（提交图像内像素坐标）
    double lineHeight = 0;                  // 段落原文行高估计（成员行高中位数，译文覆盖字号适配依据）
    std::string text;                       // 成员行文本按 CJK/拉丁规则拼接的结果（翻译请求体）
};

// 聚类结果 + 全局统计（供调用方日志观测与阈值调优）

struct TcClusterResult {
    std::vector<TcParagraph> paragraphs;    // 段落（按首行阅读序排列）
    double medianLineH = 0;                 // 页面正常行高估计（全部行高的中位数）
    double medianPitch = 0;                 // 页面正常行距估计（行高一致相邻对的 pitch 中位数）
    int boundaryCount = 0;                  // 判为段落边界的相邻对数量
};

// 主入口：OCR 文本行数组 → 段落数组。纯函数（无副作用、无平台依赖）：
//   输入为空 → 输出空；单行 → 输出单行段落；
//   判定流水线（预处理 → 行高/行距估计 → 候选相邻 → 多维特征 → 连续/边界
//   判定 → 链式聚合 → 孤行吸收等二次修正）见 translate_cluster.cpp 头部注释。
TcClusterResult TcClusterLines(const std::vector<TcLine>& lines);
