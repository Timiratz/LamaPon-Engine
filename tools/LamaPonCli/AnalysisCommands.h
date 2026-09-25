#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace LamaPon
{
    struct MemorySnapshot;
    struct MemorySnapshotComparison;
    struct ProfileAnalysis;
    struct ProfileComparison;
}

namespace LamaPon::Cli
{
    // エディターの「プロファイル分析」「メモリプロファイラー」と同じ解析を
    // 保存済みの記録へ行い、結果をJSONで返します。
    //
    //   profile analyze <capture.json> [--first N] [--last M] [--top K]
    //   profile compare <a.json> <b.json> [--top K]
    //   memory summary <snapshot.json> [--top K]
    //   memory compare <a.json> <b.json> [--top K]
    //
    // argumentsはサブコマンド以降（wmainのargv[2]以降）です。引数の誤りは
    // std::invalid_argument、記録を読めない場合はstd::runtime_errorを送出します。
    [[nodiscard]] nlohmann::json RunAnalysisCommand(
        std::wstring_view command,
        std::span<const std::wstring_view> arguments);

    [[nodiscard]] nlohmann::json ProfileAnalysisJson(
        const ProfileAnalysis& analysis,
        std::size_t topCount);
    [[nodiscard]] nlohmann::json ProfileComparisonJson(
        const ProfileComparison& comparison,
        std::size_t topCount);
    [[nodiscard]] nlohmann::json MemorySummaryJson(
        const MemorySnapshot& snapshot,
        std::size_t topCount);
    [[nodiscard]] nlohmann::json MemoryComparisonJson(
        const MemorySnapshotComparison& comparison,
        std::size_t topCount);
}
