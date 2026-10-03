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
        // 診断を指定先へ格納します(error: 出力先、不要ならnull, message: 診断本文)。
        void SetError(std::string* error, std::string message)
        {
            if (error != nullptr)
            {
                *error = std::move(message);
            }
        }

        // JSONの分類名を分類値へ戻します(key: 保存用の分類名)。
        [[nodiscard]] MemoryCategory CategoryFromKey(
            const std::string_view key) noexcept
        {
            // 分類名を照合する列挙値の位置
            for (std::size_t index{};
                index < static_cast<std::size_t>(MemoryCategory::Count);
                ++index)
            {
                // 現在照合するメモリー分類
                const auto category =
                    static_cast<MemoryCategory>(index);
                if (MemoryCategoryKey(category) == key)
                {
                    return category;
                }
            }
            return MemoryCategory::Other;
        }

        // 符号なしの絶対値を返します(value: 符号付きの増減量)。
        // INT64_MINの直接反転を避けて桁あふれを防ぎます。
        [[nodiscard]] std::uint64_t Magnitude(
            const std::int64_t value) noexcept
        {
            return value < 0
                ? static_cast<std::uint64_t>(-(value + 1)) + 1u
                : static_cast<std::uint64_t>(value);
        }

        // 比較後から比較前を引いた量を返します(before: 比較前のバイト数, after: 比較後のバイト数)。
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
        // 分類別の資源数と合計量
        MemoryCategoryTotals totals{};
        // 分類別に集計する資源の記録
        for (const auto& entry : snapshot.entries)
        {
            // 範囲外をその他へ寄せた分類位置
            const auto index = std::min(
                static_cast<std::size_t>(entry.category),
                static_cast<std::size_t>(MemoryCategory::Other));
            // 現在の分類の集計先
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
        // プロセス統計と資源別の比較結果
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

        using Key = std::pair<MemoryCategory, std::string>;
        struct Side final
        {
            // 比較前の同一資源の合計量
            std::uint64_t before{};
            // 比較後の同一資源の合計量
            std::uint64_t after{};
            // 比較前に資源が存在するか
            bool inBefore{};
            // 比較後に資源が存在するか
            bool inAfter{};
            // 比較後を優先する資源の補足
            std::string detail;
        };
        // 分類と名前で対応付けた前後の量
        std::map<Key, Side> sides;
        // 比較前の資源の記録
        for (const auto& entry : before.entries)
        {
            // 同じ分類と名前の資源の集計先
            auto& side = sides[{ entry.category, entry.name }];
            side.before += entry.TotalBytes();
            side.inBefore = true;
            side.detail = entry.detail;
        }
        // 比較後の資源の記録
        for (const auto& entry : after.entries)
        {
            // 同じ分類と名前の資源の集計先
            auto& side = sides[{ entry.category, entry.name }];
            side.after += entry.TotalBytes();
            side.inAfter = true;
            side.detail = entry.detail;
        }

        // key: 分類と名前, side: 前後の量
        for (auto& [key, side] : sides)
        {
            if (side.inBefore && side.inAfter
                && side.before == side.after)
            {
                continue;
            }
            // 変化した資源の差分記録
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
        // 絶対増減量の降順を判定します(left: 比較元の資源差分, right: 比較先の資源差分)。
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
        // 単位付きメモリー量の出力領域
        char text[32]{};
        // 小数で単位換算するバイト数
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
        // 全ミップの推定バイト数
        std::uint64_t total{};
        // 現在のミップの幅
        std::uint64_t levelWidth = std::max<std::uint32_t>(width, 1);
        // 現在のミップの高さ
        std::uint64_t levelHeight = std::max<std::uint32_t>(height, 1);
        // 最小1を保証したミップ数
        const auto levels = std::max<std::uint32_t>(mipLevels, 1);
        // サイズを積算するミップの番号
        for (std::uint32_t level{}; level < levels; ++level)
        {
            // 圧縮ブロック単位へ切り上げた幅
            const auto paddedWidth = blockCompressed
                ? (levelWidth + 3) / 4 * 4
                : levelWidth;
            // 圧縮ブロック単位へ切り上げた高さ
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
            // 保存する資源内訳のJSON配列
            nlohmann::json entries = nlohmann::json::array();
            // JSONへ保存する資源の記録
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
            // 保存形式とメモリー統計のJSON
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
            // JSONを書き込む出力ファイル
            std::ofstream output(
                path,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            // 名前にUTF-8以外が混ざっても保存を失敗させないよう、不正な列は置換文字にします。
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
        // 読み込んだスナップショットJSON
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

            // 検証後に出力へ移す読み込み結果
            MemorySnapshot loaded;
            loaded.label = document.value("label", std::string{});
            loaded.capturedAt =
                document.value("capturedAt", std::string{});
            // プロセス統計のJSON項目
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
            // 資源内訳のJSON配列
            if (const auto entries = document.find("entries");
                entries != document.end() && entries->is_array())
            {
                // 読み込む資源記録のJSON
                for (const auto& entryJson : *entries)
                {
                    if (!entryJson.is_object())
                    {
                        continue;
                    }
                    // 構築する資源別メモリー記録
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
        // JSON値の型などを検証した際の例外
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
        // JSONを読み込む入力ファイル
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            SetError(error, "ファイルを開けませんでした。");
            return false;
        }
        // 読み込んだJSON本文
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        return ParseMemorySnapshotJson(text, snapshot, error);
    }
}
