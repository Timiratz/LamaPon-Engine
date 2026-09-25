#pragma once

#include "LamaPon/Core/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // メモリプロファイラーが内訳を分類する単位です。値はJSONへ名前で
    // 保存するため、並び順を変えても保存済みのスナップショットは読めます。
    enum class MemoryCategory : std::uint8_t
    {
        Texture,
        Model,
        TextTexture,
        RenderTexture,
        Audio,
        Animation,
        DataAsset,
        PrefetchedFile,
        Other,
        Count
    };

    // 1つの資源（テクスチャ1枚、モデル1つなど）のメモリ量です。
    // GPU量は資源の形式と寸法からの見積もりで、ドライバーの配置による
    // 余白は含みません。CPU量はエンジンが保持している複製の大きさです。
    struct MemorySnapshotEntry final
    {
        MemoryCategory category{ MemoryCategory::Other };
        // 同じ分類の中で資源を識別する名前です（アセットパスなど）。
        // スナップショットの比較はcategoryとnameの組で対応付けます。
        std::string name;
        // 「1024x1024 BC3 11ミップ」のような補足です。
        std::string detail;
        std::uint64_t gpuBytes{};
        std::uint64_t cpuBytes{};

        [[nodiscard]] std::uint64_t TotalBytes() const noexcept
        {
            return gpuBytes + cpuBytes;
        }
    };

    // プロセス全体の量です。GraphicsMemoryStatisticsの必要な値だけを
    // 写し、CoreからGraphicsへ依存しないようにしています。
    struct MemoryProcessTotals final
    {
        std::uint64_t processWorkingSetBytes{};
        std::uint64_t processPrivateBytes{};
        std::uint64_t localVideoMemoryUsageBytes{};
        std::uint64_t nonLocalVideoMemoryUsageBytes{};
        bool videoMemoryAvailable{};
    };

    struct MemorySnapshot final
    {
        // 表示用の名前です（保存時刻やファイル名）。
        std::string label;
        std::string capturedAt;
        MemoryProcessTotals process;
        std::vector<MemorySnapshotEntry> entries;
    };

    struct MemoryCategoryTotal final
    {
        std::size_t count{};
        std::uint64_t gpuBytes{};
        std::uint64_t cpuBytes{};
    };

    using MemoryCategoryTotals = std::array<
        MemoryCategoryTotal,
        static_cast<std::size_t>(MemoryCategory::Count)>;

    enum class MemoryEntryChange : std::uint8_t
    {
        Added,
        Removed,
        Changed
    };

    // 同じ資源の前後の量です。deltaは後 - 前で、増えたときに正です。
    struct MemoryEntryDifference final
    {
        MemoryCategory category{ MemoryCategory::Other };
        std::string name;
        std::string detail;
        MemoryEntryChange change{ MemoryEntryChange::Changed };
        std::uint64_t beforeBytes{};
        std::uint64_t afterBytes{};

        [[nodiscard]] std::int64_t DeltaBytes() const noexcept
        {
            return static_cast<std::int64_t>(afterBytes)
                - static_cast<std::int64_t>(beforeBytes);
        }
    };

    struct MemorySnapshotComparison final
    {
        MemoryCategoryTotals before{};
        MemoryCategoryTotals after{};
        std::int64_t processPrivateDelta{};
        std::int64_t processWorkingSetDelta{};
        std::int64_t localVideoMemoryDelta{};
        // 増減した資源だけを、差の絶対値が大きい順に並べます。
        std::vector<MemoryEntryDifference> entries;
    };

    [[nodiscard]] LAMAPON_API std::string_view MemoryCategoryName(
        MemoryCategory category) noexcept;
    // JSONへ保存する英語の識別子です。
    [[nodiscard]] LAMAPON_API std::string_view MemoryCategoryKey(
        MemoryCategory category) noexcept;

    [[nodiscard]] LAMAPON_API MemoryCategoryTotals
        SummarizeMemorySnapshot(const MemorySnapshot& snapshot);
    [[nodiscard]] LAMAPON_API MemorySnapshotComparison
        CompareMemorySnapshots(
            const MemorySnapshot& before,
            const MemorySnapshot& after);

    // 「12.5 MiB」のように読みやすい単位へ整えます。
    [[nodiscard]] LAMAPON_API std::string FormatMemoryBytes(
        std::uint64_t bytes);
    [[nodiscard]] LAMAPON_API std::string FormatMemoryDelta(
        std::int64_t bytes);

    [[nodiscard]] LAMAPON_API bool WriteMemorySnapshotJson(
        const std::filesystem::path& path,
        const MemorySnapshot& snapshot) noexcept;
    // 失敗時はfalseを返し、snapshotは変更しません。
    [[nodiscard]] LAMAPON_API bool LoadMemorySnapshotJson(
        const std::filesystem::path& path,
        MemorySnapshot& snapshot,
        std::string* error = nullptr);
    [[nodiscard]] LAMAPON_API bool ParseMemorySnapshotJson(
        std::string_view text,
        MemorySnapshot& snapshot,
        std::string* error = nullptr);

    // 形式・寸法・ミップ数から、テクスチャ1枚分の大きさを見積もります。
    // bitsPerPixelはブロック圧縮なら1ピクセルあたりの平均（BC1は4）です。
    // ブロック圧縮では各ミップを4x4単位へ切り上げます。
    [[nodiscard]] LAMAPON_API std::uint64_t EstimateTextureBytes(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depthOrArraySize,
        std::uint32_t mipLevels,
        std::uint32_t bitsPerPixel,
        bool blockCompressed) noexcept;
}
