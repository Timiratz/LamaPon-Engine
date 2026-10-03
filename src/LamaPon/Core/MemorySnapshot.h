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
    // メモリプロファイラーが内訳を分類する単位です。
    // 値はJSONへ名前で保存するため、並び順を変えても保存済みのスナップショットは読めます。
    enum class MemoryCategory : std::uint8_t
    {
        // 読み込み済みのテクスチャ
        Texture,
        // 読み込み済みのモデル
        Model,
        // 文字描画用のテクスチャ
        TextTexture,
        // 描画先のレンダーテクスチャ
        RenderTexture,
        // 読み込み済みの音声
        Audio,
        // アニメーションデータ
        Animation,
        // 汎用データアセット
        DataAsset,
        // 先読み済みのファイル
        PrefetchedFile,
        // その他の資源
        Other,
        // 分類数を表す終端値
        Count
    };

    // 1つの資源（テクスチャ1枚、モデル1つなど）のメモリ量です。
    // GPU量は資源の形式と寸法からの見積もりで、ドライバーの配置による余白は含みません。
    // CPU量はエンジンが保持している複製の大きさです。
    struct MemorySnapshotEntry final
    {
        // 資源のメモリー分類
        MemoryCategory category{ MemoryCategory::Other };
        // 分類内で資源を識別する名前
        // スナップショットの比較はcategoryとnameの組で対応付けます。
        std::string name;
        // 資源の寸法や形式の補足
        std::string detail;
        // GPU上の推定バイト数
        std::uint64_t gpuBytes{};
        // CPU上の保持バイト数
        std::uint64_t cpuBytes{};

        // GPUとCPUの合計バイト数を返します。
        [[nodiscard]] std::uint64_t TotalBytes() const noexcept
        {
            return gpuBytes + cpuBytes;
        }
    };

    // プロセス全体の量です。
    // GraphicsMemoryStatisticsの必要な値だけを写し、CoreからGraphicsへ依存しないようにしています。
    struct MemoryProcessTotals final
    {
        // プロセスの常駐メモリーバイト数
        std::uint64_t processWorkingSetBytes{};
        // プロセスの専用メモリーバイト数
        std::uint64_t processPrivateBytes{};
        // ローカルビデオ使用量、バイト
        std::uint64_t localVideoMemoryUsageBytes{};
        // 非ローカルビデオ使用量、バイト
        std::uint64_t nonLocalVideoMemoryUsageBytes{};
        // ビデオメモリー統計を取得したか
        bool videoMemoryAvailable{};
    };

    struct MemorySnapshot final
    {
        // スナップショットの表示名
        std::string label;
        // スナップショットの取得日時
        std::string capturedAt;
        // プロセス全体のメモリー量
        MemoryProcessTotals process;
        // 資源ごとのメモリー内訳
        std::vector<MemorySnapshotEntry> entries;
    };

    struct MemoryCategoryTotal final
    {
        // 分類内の資源数
        std::size_t count{};
        // 分類内のGPU合計バイト数
        std::uint64_t gpuBytes{};
        // 分類内のCPU合計バイト数
        std::uint64_t cpuBytes{};
    };

    using MemoryCategoryTotals = std::array<
        MemoryCategoryTotal,
        static_cast<std::size_t>(MemoryCategory::Count)>;

    enum class MemoryEntryChange : std::uint8_t
    {
        // 比較後にだけ存在する資源
        Added,
        // 比較前にだけ存在する資源
        Removed,
        // メモリー量が変化した資源
        Changed
    };

    // 同じ資源の前後の量です。
    // deltaは後 - 前で、増えたときに正です。
    struct MemoryEntryDifference final
    {
        // 比較する資源の分類
        MemoryCategory category{ MemoryCategory::Other };
        // 分類内で資源を識別する名前
        std::string name;
        // 比較後を優先した資源の補足
        std::string detail;
        // 追加・削除・量の変更の種別
        MemoryEntryChange change{ MemoryEntryChange::Changed };
        // 比較前の合計バイト数
        std::uint64_t beforeBytes{};
        // 比較後の合計バイト数
        std::uint64_t afterBytes{};

        // 比較後から比較前を引いたバイト数を返します。
        [[nodiscard]] std::int64_t DeltaBytes() const noexcept
        {
            return static_cast<std::int64_t>(afterBytes)
                - static_cast<std::int64_t>(beforeBytes);
        }
    };

    struct MemorySnapshotComparison final
    {
        // 比較前の分類別メモリー量
        MemoryCategoryTotals before{};
        // 比較後の分類別メモリー量
        MemoryCategoryTotals after{};
        // プロセス専用メモリーの増減量
        std::int64_t processPrivateDelta{};
        // プロセス常駐メモリーの増減量
        std::int64_t processWorkingSetDelta{};
        // ローカルビデオメモリーの増減量
        std::int64_t localVideoMemoryDelta{};
        // 絶対増減量の降順の資源差分
        std::vector<MemoryEntryDifference> entries;
    };

    // メモリー分類の表示名を返します(category: 資源の分類)。
    [[nodiscard]] LAMAPON_API std::string_view MemoryCategoryName(
        MemoryCategory category) noexcept;
    // JSON保存用の分類名を返します(category: 資源の分類)。
    [[nodiscard]] LAMAPON_API std::string_view MemoryCategoryKey(
        MemoryCategory category) noexcept;

    // メモリー量と資源数を分類別に集計します(snapshot: 集計対象のスナップショット)。
    [[nodiscard]] LAMAPON_API MemoryCategoryTotals
        SummarizeMemorySnapshot(const MemorySnapshot& snapshot);
    // スナップショット間の増減を返します(before: 比較前, after: 比較後)。
    // 同じ分類と名前の資源を合算し、変化した資源だけを絶対増減量の降順で返します。
    [[nodiscard]] LAMAPON_API MemorySnapshotComparison
        CompareMemorySnapshots(
            const MemorySnapshot& before,
            const MemorySnapshot& after);

    // バイト数をB・KiB・MiB・GiBの文字列へ変換します(bytes: 表示するバイト数)。
    [[nodiscard]] LAMAPON_API std::string FormatMemoryBytes(
        std::uint64_t bytes);
    // 増減量を符号付きの単位文字列へ変換します(bytes: 表示するバイト数の増減)。
    [[nodiscard]] LAMAPON_API std::string FormatMemoryDelta(
        std::int64_t bytes);

    // スナップショットをJSON保存し成功可否を返します(path: 保存先, snapshot: 保存する内容)。
    [[nodiscard]] LAMAPON_API bool WriteMemorySnapshotJson(
        const std::filesystem::path& path,
        const MemorySnapshot& snapshot) noexcept;
    // JSONファイルを読み込みます(path: 読み込み元, snapshot: 成功時の出力先, error: 診断の出力先、不要ならnull)。
    // 失敗時はfalseを返し、snapshotは変更しません。
    [[nodiscard]] LAMAPON_API bool LoadMemorySnapshotJson(
        const std::filesystem::path& path,
        MemorySnapshot& snapshot,
        std::string* error = nullptr);
    // JSON文字列を解釈します(text: JSON本文, snapshot: 成功時の出力先, error: 診断の出力先、不要ならnull)。
    // 失敗時はfalseを返し、snapshotは変更しません。
    [[nodiscard]] LAMAPON_API bool ParseMemorySnapshotJson(
        std::string_view text,
        MemorySnapshot& snapshot,
        std::string* error = nullptr);

    // テクスチャのバイト数を見積もります(width: 幅, height: 高さ, depthOrArraySize: 層数, mipLevels: ミップ数, bitsPerPixel: 1ピクセル平均ビット数, blockCompressed: ブロック圧縮か)。
    // 寸法と層数とミップ数は最小1とし、ブロック圧縮では各ミップの幅と高さを4の倍数へ切り上げます。
    [[nodiscard]] LAMAPON_API std::uint64_t EstimateTextureBytes(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depthOrArraySize,
        std::uint32_t mipLevels,
        std::uint32_t bitsPerPixel,
        bool blockCompressed) noexcept;
}
