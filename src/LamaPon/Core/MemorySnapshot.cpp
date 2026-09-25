#include "LamaPon/Core/MemorySnapshot.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace LamaPon
{
    namespace
    {
        void SetError(std::string* error, std::string message)
        {
            if (error != nullptr)
            {
                *error = std::move(message);
            }
        }

        [[nodiscard]] MemoryCategory CategoryFromKey(
            const std::string_view key) noexcept
        {
            for (std::size_t index{};
                index < static_cast<std::size_t>(MemoryCategory::Count);
                ++index)
            {
                const auto category =
                    static_cast<MemoryCategory>(index);
                if (MemoryCategoryKey(category) == key)
                {
                    return category;
                }
            }
            return MemoryCategory::Other;
        }

        // INT64_MINでも符号反転であふれないよう、符号なしで絶対値を作ります。
        [[nodiscard]] std::uint64_t Magnitude(
            const std::int64_t value) noexcept
        {
            return value < 0
                ? static_cast<std::uint64_t>(-(value + 1)) + 1u
                : static_cast<std::uint64_t>(value);
        }

        [[nodiscard]] std::int64_t Delta(
            const std::uint64_t before,
            const std::uint64_t after) noexcept
        {
            return static_cast<std::int64_t>(after)
                - static_cast<std::int64_t>(before);
        }
    }

    std::string_view MemoryCategoryName(
        const MemoryCategory category) noexcept
    {
        switch (category)
        {
        case MemoryCategory::Texture:
            return "テクスチャ";
        case MemoryCategory::Model:
            return "モデル";
        case MemoryCategory::TextTexture:
            return "文字テクスチャ";
        case MemoryCategory::RenderTexture:
            return "レンダーテクスチャ";
        case MemoryCategory::Audio:
            return "オーディオ";
        case MemoryCategory::Animation:
            return "アニメーション";
        case MemoryCategory::DataAsset:
            return "データアセット";
        case MemoryCategory::PrefetchedFile:
            return "先読みファイル";
        case MemoryCategory::Other:
        case MemoryCategory::Count:
            break;
        }
        return "その他";
    }

    std::string_view MemoryCategoryKey(
        const MemoryCategory category) noexcept
    {
        switch (category)
        {
        case MemoryCategory::Texture:
            return "texture";
        case MemoryCategory::Model:
            return "model";
        case MemoryCategory::TextTexture:
            return "text-texture";
        case MemoryCategory::RenderTexture:
            return "render-texture";
        case MemoryCategory::Audio:
            return "audio";
        case MemoryCategory::Animation:
            return "animation";
        case MemoryCategory::DataAsset:
            return "data-asset";
        case MemoryCategory::PrefetchedFile:
            return "prefetched-file";
        case MemoryCategory::Other:
        case MemoryCategory::Count:
            break;
        }
        return "other";
    }

    MemoryCategoryTotals SummarizeMemorySnapshot(
        const MemorySnapshot& snapshot)
    {
        MemoryCategoryTotals totals{};
        for (const auto& entry : snapshot.entries)
        {
            const auto index = std::min(
                static_cast<std::size_t>(entry.category),
                static_cast<std::size_t>(MemoryCategory::Other));
            auto& total = totals[index];
            ++total.count;
            total.gpuBytes += entry.gpuBytes;
            total.cpuBytes += entry.cpuBytes;
        }
        return totals;
    }

    MemorySnapshotComparison CompareMemorySnapshots(
        const MemorySnapshot& before,
        const MemorySnapshot& after)
    {
        MemorySnapshotComparison comparison;
        comparison.before = SummarizeMemorySnapshot(before);
        comparison.after = SummarizeMemorySnapshot(after);
        comparison.processPrivateDelta = Delta(
            before.process.processPrivateBytes,
            after.process.processPrivateBytes);
        comparison.processWorkingSetDelta = Delta(
            before.process.processWorkingSetBytes,
            after.process.processWorkingSetBytes);
        comparison.localVideoMemoryDelta = Delta(
            before.process.localVideoMemoryUsageBytes,
            after.process.localVideoMemoryUsageBytes);

        // 同じ名前の資源が同じ分類に複数ある場合（同じテクスチャを
        // 別の用途で読み込んだ場合など）は合算して1つとして比べます。
        using Key = std::pair<MemoryCategory, std::string>;
        struct Side final
        {
            std::uint64_t before{};
            std::uint64_t after{};
            bool inBefore{};
            bool inAfter{};
            std::string detail;
        };
        std::map<Key, Side> sides;
        for (const auto& entry : before.entries)
        {
            auto& side = sides[{ entry.category, entry.name }];
            side.before += entry.TotalBytes();
            side.inBefore = true;
            side.detail = entry.detail;
        }
        for (const auto& entry : after.entries)
        {
            auto& side = sides[{ entry.category, entry.name }];
            side.after += entry.TotalBytes();
            side.inAfter = true;
            side.detail = entry.detail;
        }

        for (auto& [key, side] : sides)
        {
            if (side.inBefore && side.inAfter
                && side.before == side.after)
            {
                continue;
            }
            MemoryEntryDifference difference;
            difference.category = key.first;
            difference.name = key.second;
            difference.detail = std::move(side.detail);
            difference.beforeBytes = side.before;
            difference.afterBytes = side.after;
            difference.change = !side.inBefore
                ? MemoryEntryChange::Added
                : (!side.inAfter
                    ? MemoryEntryChange::Removed
                    : MemoryEntryChange::Changed);
            comparison.entries.push_back(std::move(difference));
        }
        std::ranges::stable_sort(
            comparison.entries,
            [](const MemoryEntryDifference& left,
                const MemoryEntryDifference& right)
            {
                return Magnitude(left.DeltaBytes())
                    > Magnitude(right.DeltaBytes());
            });
        return comparison;
    }

    std::string FormatMemoryBytes(const std::uint64_t bytes)
    {
        char text[32]{};
        const auto value = static_cast<double>(bytes);
        if (bytes >= 1024ull * 1024ull * 1024ull)
        {
            std::snprintf(
                text,
                sizeof(text),
                "%.2f GiB",
                value / (1024.0 * 1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull * 1024ull)
        {
            std::snprintf(
                text,
                sizeof(text),
                "%.2f MiB",
                value / (1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull)
        {
            std::snprintf(
                text,
                sizeof(text),
                "%.1f KiB",
                value / 1024.0);
        }
        else
        {
            std::snprintf(
                text,
                sizeof(text),
                "%llu B",
                static_cast<unsigned long long>(bytes));
        }
        return text;
    }

    std::string FormatMemoryDelta(const std::int64_t bytes)
    {
        return (bytes < 0 ? "-" : "+")
            + FormatMemoryBytes(Magnitude(bytes));
    }

    std::uint64_t EstimateTextureBytes(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t depthOrArraySize,
        const std::uint32_t mipLevels,
        const std::uint32_t bitsPerPixel,
        const bool blockCompressed) noexcept
    {
        std::uint64_t total{};
        std::uint64_t levelWidth = std::max<std::uint32_t>(width, 1);
        std::uint64_t levelHeight = std::max<std::uint32_t>(height, 1);
        const auto levels = std::max<std::uint32_t>(mipLevels, 1);
        for (std::uint32_t level{}; level < levels; ++level)
        {
            const auto paddedWidth = blockCompressed
                ? (levelWidth + 3) / 4 * 4
                : levelWidth;
            const auto paddedHeight = blockCompressed
                ? (levelHeight + 3) / 4 * 4
                : levelHeight;
            total +=
                paddedWidth * paddedHeight * bitsPerPixel / 8;
            levelWidth = std::max<std::uint64_t>(levelWidth / 2, 1);
            levelHeight = std::max<std::uint64_t>(levelHeight / 2, 1);
        }
        return total
            * std::max<std::uint32_t>(depthOrArraySize, 1);
    }

    bool WriteMemorySnapshotJson(
        const std::filesystem::path& path,
        const MemorySnapshot& snapshot) noexcept
    {
        try
        {
            nlohmann::json entries = nlohmann::json::array();
            for (const auto& entry : snapshot.entries)
            {
                entries.push_back({
                    { "category",
                        std::string(MemoryCategoryKey(entry.category)) },
                    { "name", entry.name },
                    { "detail", entry.detail },
                    { "gpuBytes", entry.gpuBytes },
                    { "cpuBytes", entry.cpuBytes },
                });
            }
            const nlohmann::json document{
                { "format", "LamaPonMemorySnapshot" },
                { "version", 1 },
                { "label", snapshot.label },
                { "capturedAt", snapshot.capturedAt },
                { "process", {
                    { "workingSetBytes",
                        snapshot.process.processWorkingSetBytes },
                    { "privateBytes",
                        snapshot.process.processPrivateBytes },
                    { "localVideoMemoryBytes",
                        snapshot.process.localVideoMemoryUsageBytes },
                    { "nonLocalVideoMemoryBytes",
                        snapshot.process.nonLocalVideoMemoryUsageBytes },
                    { "videoMemoryAvailable",
                        snapshot.process.videoMemoryAvailable },
                } },
                { "entries", std::move(entries) },
            };
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(
                path,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            // 名前にUTF-8以外が混ざっても保存を失敗させないよう、
            // 不正な列は置換文字にします。
            output << document.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace);
            output << '\n';
            return output.good();
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseMemorySnapshotJson(
        const std::string_view text,
        MemorySnapshot& snapshot,
        std::string* error)
    {
        const auto document = nlohmann::json::parse(
            text.begin(),
            text.end(),
            nullptr,
            false);
        if (document.is_discarded() || !document.is_object())
        {
            SetError(error, "JSONとして読み込めませんでした。");
            return false;
        }
        try
        {
            if (document.value("format", std::string{})
                != "LamaPonMemorySnapshot")
            {
                SetError(
                    error,
                    "LamaPonMemorySnapshot形式のJSONではありません。");
                return false;
            }
            if (document.value("version", 0) != 1)
            {
                SetError(
                    error,
                    "対応していないメモリスナップショットの版です。");
                return false;
            }

            MemorySnapshot loaded;
            loaded.label = document.value("label", std::string{});
            loaded.capturedAt =
                document.value("capturedAt", std::string{});
            if (const auto process = document.find("process");
                process != document.end() && process->is_object())
            {
                loaded.process.processWorkingSetBytes =
                    process->value("workingSetBytes", std::uint64_t{});
                loaded.process.processPrivateBytes =
                    process->value("privateBytes", std::uint64_t{});
                loaded.process.localVideoMemoryUsageBytes =
                    process->value(
                        "localVideoMemoryBytes",
                        std::uint64_t{});
                loaded.process.nonLocalVideoMemoryUsageBytes =
                    process->value(
                        "nonLocalVideoMemoryBytes",
                        std::uint64_t{});
                loaded.process.videoMemoryAvailable =
                    process->value("videoMemoryAvailable", false);
            }
            if (const auto entries = document.find("entries");
                entries != document.end() && entries->is_array())
            {
                for (const auto& entryJson : *entries)
                {
                    if (!entryJson.is_object())
                    {
                        continue;
                    }
                    MemorySnapshotEntry entry;
                    entry.category = CategoryFromKey(
                        entryJson.value("category", std::string{}));
                    entry.name = entryJson.value("name", std::string{});
                    entry.detail =
                        entryJson.value("detail", std::string{});
                    entry.gpuBytes =
                        entryJson.value("gpuBytes", std::uint64_t{});
                    entry.cpuBytes =
                        entryJson.value("cpuBytes", std::uint64_t{});
                    loaded.entries.push_back(std::move(entry));
                }
            }
            snapshot = std::move(loaded);
            return true;
        }
        catch (const nlohmann::json::exception& exception)
        {
            SetError(
                error,
                std::string{ "スナップショットの値が不正です: " }
                    + exception.what());
            return false;
        }
    }

    bool LoadMemorySnapshotJson(
        const std::filesystem::path& path,
        MemorySnapshot& snapshot,
        std::string* error)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            SetError(error, "ファイルを開けませんでした。");
            return false;
        }
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        return ParseMemorySnapshotJson(text, snapshot, error);
    }
}
