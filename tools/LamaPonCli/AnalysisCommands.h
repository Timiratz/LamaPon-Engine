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
    // 記録を解析しJSON結果を返します(command: 種別, arguments: argv[2以降)
    // CLIの書式はprofile analyze/compareとmemory summary/compareです。
    // 不正引数はinvalid_argument、読込失敗はruntime_errorになります。
    [[nodiscard]] nlohmann::json RunAnalysisCommand(
        std::wstring_view command,
        std::span<const std::wstring_view> arguments);

    // profile解析結果をJSON化します(analysis: 解析結果, topCount: 上位件数)
    [[nodiscard]] nlohmann::json ProfileAnalysisJson(
        const ProfileAnalysis& analysis,
        std::size_t topCount);
    // profile比較結果をJSON化します(comparison: 比較結果, topCount: 上位件数)
    [[nodiscard]] nlohmann::json ProfileComparisonJson(
        const ProfileComparison& comparison,
        std::size_t topCount);
    // memory snapshotをJSON化します(snapshot: 記録, topCount: 上位件数)
    [[nodiscard]] nlohmann::json MemorySummaryJson(
        const MemorySnapshot& snapshot,
        std::size_t topCount);
    // memory比較結果をJSON化します(comparison: 比較結果, topCount: 上位件数)
    [[nodiscard]] nlohmann::json MemoryComparisonJson(
        const MemorySnapshotComparison& comparison,
        std::size_t topCount);
}
