#pragma once

#include "LamaPon/Core/ProfileAnalysis.h"
#include "LamaPon/Editor/DebugCaptureFiles.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace LamaPon
{
    // 記録済みのプロファイルを複数フレームにわたって集計し、2つの記録を
    // 比較するパネルです（UnityのProfile Analyzerに相当）。
    // 読み込んだデータセットと解析結果を所有し、範囲や読み込み元が
    // 変わったときだけ再解析します。
    class ProfileAnalyzerPanel final
    {
    public:
        using StatusSink = std::function<void(std::string, bool)>;

        struct Dataset final
        {
            std::string label;
            std::vector<ProfileFrame> frames;
            // 解析するフレームの位置（両端を含む）です。
            int rangeFirst{};
            int rangeLast{};
            ProfileAnalysis analysis;
            bool dirty{ true };

            [[nodiscard]] bool IsLoaded() const noexcept
            {
                return !frames.empty();
            }
        };

        ProfileAnalyzerPanel(
            StatusSink status,
            DebugCaptureFiles::OpenFileDialog openFile);

        ProfileAnalyzerPanel(const ProfileAnalyzerPanel&) = delete;
        ProfileAnalyzerPanel& operator=(
            const ProfileAnalyzerPanel&) = delete;

        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory);

        // slotは0がA、1がBです。framesが空の場合は何もしません。
        void SetDataset(
            std::size_t slot,
            std::string label,
            std::vector<ProfileFrame> frames);
        [[nodiscard]] bool LoadDataset(
            std::size_t slot,
            const std::filesystem::path& path);
        [[nodiscard]] const Dataset& DatasetAt(
            std::size_t slot) const noexcept
        {
            return m_datasets[slot];
        }
        // 範囲が変わった、または読み込み直したデータセットを再解析します。
        void RefreshAnalysis();

    private:
        void DrawDatasetControls(
            std::size_t slot,
            const std::filesystem::path& captureDirectory);
        void DrawSingleView();
        void DrawComparisonView();
        void DrawMarkerDistribution();
        void SetStatus(std::string message, bool error = false) const;

        StatusSink m_status;
        DebugCaptureFiles::OpenFileDialog m_openFile;
        std::array<Dataset, 2> m_datasets;
        ProfileComparison m_comparison;
        bool m_comparisonDirty{ true };
        std::vector<std::filesystem::path> m_captureFiles;
        std::string m_filter;
        std::string m_selectedPath;
        // 選択中の区間のフレームごとの時間です。全フレームの経路を
        // 組み立て直す処理は重いため、選択か範囲が変わったときだけ
        // 作り直します。
        std::array<std::vector<float>, 2> m_series;
        std::string m_seriesPath;
        bool m_seriesDirty{ true };
        bool m_topLevelOnly{};
        int m_view{};
    };
}
