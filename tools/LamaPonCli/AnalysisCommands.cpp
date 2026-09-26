#include "AnalysisCommands.h"

#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/ProfileAnalysis.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace LamaPon::Cli
{
    namespace
    {
        constexpr std::size_t DefaultTopCount = 20;

        [[nodiscard]] std::string ToUtf8(const std::filesystem::path& path)
        {
            const auto text = path.generic_u8string();
            return { text.begin(), text.end() };
        }

        [[nodiscard]] std::string ToUtf8(const std::wstring_view text)
        {
            return ToUtf8(std::filesystem::path{ std::wstring{ text } });
        }

        struct ParsedArguments final
        {
            std::vector<std::filesystem::path> files;
            std::optional<std::size_t> first;
            std::optional<std::size_t> last;
            std::size_t top{ DefaultTopCount };
        };

        [[nodiscard]] std::size_t ParseCount(
            const std::wstring_view option,
            const std::wstring_view value)
        {
            try
            {
                std::size_t consumed{};
                const auto parsed =
                    std::stoll(std::wstring{ value }, &consumed);
                if (consumed == value.size() && parsed >= 0)
                {
                    return static_cast<std::size_t>(parsed);
                }
            }
            catch (const std::exception&)
            {
            }
            throw std::invalid_argument(
                ToUtf8(option) + " requires a non-negative integer.");
        }

        [[nodiscard]] ParsedArguments ParseArguments(
            const std::span<const std::wstring_view> arguments)
        {
            ParsedArguments parsed;
            for (std::size_t index = 1; index < arguments.size(); ++index)
            {
                const auto argument = arguments[index];
                const auto value = [&]() -> std::wstring_view
                {
                    if (index + 1 >= arguments.size())
                    {
                        throw std::invalid_argument(
                            ToUtf8(argument) + " requires a value.");
                    }
                    return arguments[++index];
                };
                if (argument == L"--first")
                {
                    parsed.first = ParseCount(argument, value());
                }
                else if (argument == L"--last")
                {
                    parsed.last = ParseCount(argument, value());
                }
                else if (argument == L"--top")
                {
                    parsed.top = ParseCount(argument, value());
                }
                else if (argument.starts_with(L"--"))
                {
                    throw std::invalid_argument(
                        "Unknown option: " + ToUtf8(argument));
                }
                else
                {
                    parsed.files.emplace_back(std::wstring{ argument });
                }
            }
            return parsed;
        }

        void RequireFileCount(
            const ParsedArguments& parsed,
            const std::size_t count,
            const char* usage)
        {
            if (parsed.files.size() != count)
            {
                throw std::invalid_argument(usage);
            }
        }

        [[nodiscard]] std::vector<ProfileFrame> LoadFrames(
            const std::filesystem::path& path)
        {
            std::vector<ProfileFrame> frames;
            std::string error;
            if (!LoadProfileJson(path, frames, &error))
            {
                throw std::runtime_error(
                    "Could not read profile " + ToUtf8(path) + ": " + error);
            }
            return frames;
        }

        [[nodiscard]] MemorySnapshot LoadSnapshot(
            const std::filesystem::path& path)
        {
            MemorySnapshot snapshot;
            std::string error;
            if (!LoadMemorySnapshotJson(path, snapshot, &error))
            {
                throw std::runtime_error(
                    "Could not read memory snapshot " + ToUtf8(path)
                    + ": " + error);
            }
            return snapshot;
        }

        [[nodiscard]] nlohmann::json ValueJson(
            const ProfileValueStatistics& statistics)
        {
            return {
                { "count", statistics.count },
                { "minimum", statistics.minimum },
                { "maximum", statistics.maximum },
                { "mean", statistics.mean },
                { "median", statistics.median },
                { "percentile95", statistics.percentile95 },
                { "maximumFrameIndex", statistics.maximumFrameIndex },
            };
        }

        [[nodiscard]] std::string_view ChangeName(
            const MemoryEntryChange change) noexcept
        {
            switch (change)
            {
            case MemoryEntryChange::Added:
                return "added";
            case MemoryEntryChange::Removed:
                return "removed";
            case MemoryEntryChange::Changed:
                break;
            }
            return "changed";
        }

        [[nodiscard]] nlohmann::json CategoryTotalsJson(
            const MemoryCategoryTotals& totals)
        {
            auto categories = nlohmann::json::array();
            for (std::size_t index{}; index < totals.size(); ++index)
            {
                const auto& total = totals[index];
                if (total.count == 0)
                {
                    continue;
                }
                categories.push_back({
                    { "category",
                        std::string(MemoryCategoryKey(
                            static_cast<MemoryCategory>(index))) },
                    { "count", total.count },
                    { "gpuBytes", total.gpuBytes },
                    { "cpuBytes", total.cpuBytes },
                });
            }
            return categories;
        }
    }

    nlohmann::json ProfileAnalysisJson(
        const ProfileAnalysis& analysis,
        const std::size_t topCount)
    {
        // 重い順に並べ、先頭topCount件だけを返します。
        std::vector<const ProfileMarkerStatistics*> markers;
        markers.reserve(analysis.markers.size());
        for (const auto& marker : analysis.markers)
        {
            markers.push_back(&marker);
        }
        std::ranges::stable_sort(
            markers,
            [](const ProfileMarkerStatistics* left,
                const ProfileMarkerStatistics* right)
            {
                return left->milliseconds.median
                    > right->milliseconds.median;
            });
        auto markerJson = nlohmann::json::array();
        for (std::size_t index{};
            index < std::min(topCount, markers.size());
            ++index)
        {
            const auto& marker = *markers[index];
            markerJson.push_back({
                { "path", marker.path },
                { "depth", marker.depth },
                { "milliseconds", ValueJson(marker.milliseconds) },
                { "selfMeanMilliseconds", marker.selfMilliseconds.mean },
                { "callsPerFrame",
                    marker.presentFrameCount == 0
                        ? 0.0
                        : static_cast<double>(marker.totalCalls)
                            / static_cast<double>(
                                marker.presentFrameCount) },
                { "presentFrames", marker.presentFrameCount },
            });
        }
        return {
            { "frameCount", analysis.frameCount },
            { "firstFrameIndex", analysis.firstFrameIndex },
            { "lastFrameIndex", analysis.lastFrameIndex },
            { "frameMilliseconds", ValueJson(analysis.frameMilliseconds) },
            { "markerCount", analysis.markers.size() },
            { "markers", std::move(markerJson) },
        };
    }

    nlohmann::json ProfileComparisonJson(
        const ProfileComparison& comparison,
        const std::size_t topCount)
    {
        auto markers = nlohmann::json::array();
        for (std::size_t index{};
            index < std::min(topCount, comparison.markers.size());
            ++index)
        {
            const auto& marker = comparison.markers[index];
            markers.push_back({
                { "path", marker.path },
                { "status",
                    !marker.presentInA
                        ? "added"
                        : (!marker.presentInB ? "removed" : "changed") },
                { "medianA", marker.medianA },
                { "medianB", marker.medianB },
                { "medianDifference", marker.medianDifference },
                { "medianRelativeChange", marker.medianRelativeChange },
                { "meanDifference", marker.meanDifference },
                { "callsPerFrameA", marker.callsPerFrameA },
                { "callsPerFrameB", marker.callsPerFrameB },
            });
        }
        return {
            { "frameCountA", comparison.a.frameCount },
            { "frameCountB", comparison.b.frameCount },
            { "frameMedianA", comparison.a.frameMilliseconds.median },
            { "frameMedianB", comparison.b.frameMilliseconds.median },
            { "frameMedianDifference", comparison.frameMedianDifference },
            { "frameMeanDifference", comparison.frameMeanDifference },
            { "markers", std::move(markers) },
        };
    }

    nlohmann::json MemorySummaryJson(
        const MemorySnapshot& snapshot,
        const std::size_t topCount)
    {
        std::vector<const MemorySnapshotEntry*> entries;
        entries.reserve(snapshot.entries.size());
        for (const auto& entry : snapshot.entries)
        {
            entries.push_back(&entry);
        }
        std::ranges::stable_sort(
            entries,
            [](const MemorySnapshotEntry* left,
                const MemorySnapshotEntry* right)
            {
                return left->TotalBytes() > right->TotalBytes();
            });
        auto entryJson = nlohmann::json::array();
        for (std::size_t index{};
            index < std::min(topCount, entries.size());
            ++index)
        {
            const auto& entry = *entries[index];
            entryJson.push_back({
                { "category",
                    std::string(MemoryCategoryKey(entry.category)) },
                { "name", entry.name },
                { "detail", entry.detail },
                { "gpuBytes", entry.gpuBytes },
                { "cpuBytes", entry.cpuBytes },
            });
        }
        return {
            { "label", snapshot.label },
            { "capturedAt", snapshot.capturedAt },
            { "process", {
                { "workingSetBytes",
                    snapshot.process.processWorkingSetBytes },
                { "privateBytes", snapshot.process.processPrivateBytes },
                { "localVideoMemoryBytes",
                    snapshot.process.localVideoMemoryUsageBytes },
            } },
            { "entryCount", snapshot.entries.size() },
            { "categories",
                CategoryTotalsJson(SummarizeMemorySnapshot(snapshot)) },
            { "entries", std::move(entryJson) },
        };
    }

    nlohmann::json MemoryComparisonJson(
        const MemorySnapshotComparison& comparison,
        const std::size_t topCount)
    {
        auto categories = nlohmann::json::array();
        for (std::size_t index{}; index < comparison.before.size(); ++index)
        {
            const auto& before = comparison.before[index];
            const auto& after = comparison.after[index];
            if (before.count == 0 && after.count == 0)
            {
                continue;
            }
            const auto beforeBytes = before.gpuBytes + before.cpuBytes;
            const auto afterBytes = after.gpuBytes + after.cpuBytes;
            categories.push_back({
                { "category",
                    std::string(MemoryCategoryKey(
                        static_cast<MemoryCategory>(index))) },
                { "beforeBytes", beforeBytes },
                { "afterBytes", afterBytes },
                { "deltaBytes",
                    static_cast<std::int64_t>(afterBytes)
                        - static_cast<std::int64_t>(beforeBytes) },
            });
        }
        auto entries = nlohmann::json::array();
        for (std::size_t index{};
            index < std::min(topCount, comparison.entries.size());
            ++index)
        {
            const auto& entry = comparison.entries[index];
            entries.push_back({
                { "category",
                    std::string(MemoryCategoryKey(entry.category)) },
                { "name", entry.name },
                { "change", std::string(ChangeName(entry.change)) },
                { "beforeBytes", entry.beforeBytes },
                { "afterBytes", entry.afterBytes },
                { "deltaBytes", entry.DeltaBytes() },
            });
        }
        return {
            { "privateBytesDelta", comparison.processPrivateDelta },
            { "workingSetBytesDelta", comparison.processWorkingSetDelta },
            { "localVideoMemoryBytesDelta",
                comparison.localVideoMemoryDelta },
            { "changedEntryCount", comparison.entries.size() },
            { "categories", std::move(categories) },
            { "entries", std::move(entries) },
        };
    }

    nlohmann::json RunAnalysisCommand(
        const std::wstring_view command,
        const std::span<const std::wstring_view> arguments)
    {
        if (arguments.empty())
        {
            throw std::invalid_argument(
                command == L"profile"
                    ? "profile requires analyze or compare."
                    : "memory requires summary or compare.");
        }
        const auto action = arguments.front();
        const auto parsed = ParseArguments(arguments);
        nlohmann::json response{
            { "ok", true },
            { "command", ToUtf8(command) + " " + ToUtf8(action) },
        };

        if (command == L"profile" && action == L"analyze")
        {
            RequireFileCount(
                parsed,
                1,
                "profile analyze requires one capture file.");
            const auto frames = LoadFrames(parsed.files.front());
            if (frames.empty())
            {
                throw std::runtime_error(
                    "The profile capture has no frames.");
            }
            // --first/--lastは記録内の位置（0始まり、両端を含む）です。
            const auto first = std::min(
                parsed.first.value_or(0),
                frames.size() - 1);
            const auto last = std::clamp(
                parsed.last.value_or(frames.size() - 1),
                first,
                frames.size() - 1);
            response["file"] = ToUtf8(parsed.files.front());
            response["range"] = { { "first", first }, { "last", last } };
            response["analysis"] = ProfileAnalysisJson(
                AnalyzeProfile(
                    std::span<const ProfileFrame>(frames)
                        .subspan(first, last - first + 1)),
                parsed.top);
            return response;
        }
        if (command == L"profile" && action == L"compare")
        {
            RequireFileCount(
                parsed,
                2,
                "profile compare requires two capture files.");
            const auto a = LoadFrames(parsed.files[0]);
            const auto b = LoadFrames(parsed.files[1]);
            response["fileA"] = ToUtf8(parsed.files[0]);
            response["fileB"] = ToUtf8(parsed.files[1]);
            response["comparison"] = ProfileComparisonJson(
                CompareProfiles(AnalyzeProfile(a), AnalyzeProfile(b)),
                parsed.top);
            return response;
        }
        if (command == L"memory" && action == L"summary")
        {
            RequireFileCount(
                parsed,
                1,
                "memory summary requires one snapshot file.");
            response["file"] = ToUtf8(parsed.files.front());
            response["summary"] = MemorySummaryJson(
                LoadSnapshot(parsed.files.front()),
                parsed.top);
            return response;
        }
        if (command == L"memory" && action == L"compare")
        {
            RequireFileCount(
                parsed,
                2,
                "memory compare requires two snapshot files.");
            response["fileA"] = ToUtf8(parsed.files[0]);
            response["fileB"] = ToUtf8(parsed.files[1]);
            response["comparison"] = MemoryComparisonJson(
                CompareMemorySnapshots(
                    LoadSnapshot(parsed.files[0]),
                    LoadSnapshot(parsed.files[1])),
                parsed.top);
            return response;
        }
        throw std::invalid_argument(
            "Unknown " + ToUtf8(command) + " action: " + ToUtf8(action));
    }
}
