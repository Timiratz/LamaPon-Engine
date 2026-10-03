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
    // 2つの記録と解析結果を所有し、読み込み元や範囲が変わったときだけ集計・比較する。
    class ProfileAnalyzerPanel final
    {
    public:
        // 本文と失敗フラグを通知するcallback
        using StatusSink = std::function<void(std::string, bool)>;

        struct Dataset final
        {
            // 画面に表示する記録の名前
            std::string label;
            // 読み込んだ計測フレーム一覧
            std::vector<ProfileFrame> frames;
            // 解析範囲の開始位置・両端を含む
            int rangeFirst{};
            // 解析範囲の終了位置・両端を含む
            int rangeLast{};
            // 選択した範囲の解析結果
            ProfileAnalysis analysis;
            // 範囲または記録が変わり再解析するか
            bool dirty{ true };

            // 1つ以上の計測フレームを保持しているかを返す。
            [[nodiscard]] bool IsLoaded() const noexcept
            {
                return !frames.empty();
            }
        };

        // 結果通知とファイル選択を所有する解析panelを作る(status: 状態通知callback・空も可, openFile: 文書選択callback・空も可)。
        ProfileAnalyzerPanel(
            StatusSink status,
            DebugCaptureFiles::OpenFileDialog openFile);

        // 計測記録と解析状態の共有を禁止する。
        ProfileAnalyzerPanel(const ProfileAnalyzerPanel&) = delete;
        // 計測記録と解析状態の共有を禁止する。
        ProfileAnalyzerPanel& operator=(
            const ProfileAnalyzerPanel&) = delete;

        // 記録の読込・範囲編集と集計・比較・時間分布を描く(title: ウィンドウの表示名とID, open: panelの表示状態の参照, captureDirectory: 計測文書を探すフォルダー)。
        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory);

        // 空でない記録を指定枠へ移し全範囲を再解析対象にする(slot: 記録Aは0・Bは1, label: 記録の表示名の所有先, frames: 空でないフレーム一覧の所有先)。
        void SetDataset(
            std::size_t slot,
            std::string label,
            std::vector<ProfileFrame> frames);
        // 計測JSONを読み指定枠へ移し不正または空なら失敗理由を通知する(slot: 記録Aは0・Bは1, path: 読み取る計測JSONのパス)。
        [[nodiscard]] bool LoadDataset(
            std::size_t slot,
            const std::filesystem::path& path);
        // 指定枠の記録を再設定まで借用する(slot: 有効な枠番号・Aは0・Bは1)。
        [[nodiscard]] const Dataset& DatasetAt(
            std::size_t slot) const noexcept
        {
            return m_datasets[slot];
        }
        // 変更された記録の範囲を制限して再集計し両方あれば比較結果を更新する。
        void RefreshAnalysis();

    private:
        // 記録枠の読込元と解析範囲を編集する(slot: 記録Aは0・Bは1, captureDirectory: 計測文書を探すフォルダー)。
        void DrawDatasetControls(
            std::size_t slot,
            const std::filesystem::path& captureDirectory);
        // 記録Aの区間統計を検索・並べ替えできる表として描く。
        void DrawSingleView();
        // 記録AからBへの区間時間の差を検索・並べ替えできる表として描く。
        void DrawComparisonView();
        // 選択区間の時間系列を変更時だけ再構成し同じ縦軸で描く。
        void DrawMarkerDistribution();
        // 通知先があれば操作結果を渡す(message: 通知本文の所有先, error: 失敗として通知するか)。
        void SetStatus(std::string message, bool error = false) const;

        // 状態と失敗理由を通知するcallback
        StatusSink m_status;
        // 記録ファイルを選ぶdialog処理
        DebugCaptureFiles::OpenFileDialog m_openFile;
        // 比較するA・Bの計測記録の所有先
        std::array<Dataset, 2> m_datasets;
        // AとBの区間の比較解析結果
        ProfileComparison m_comparison;
        // 記録の更新で比較をやり直すか
        bool m_comparisonDirty{ true };
        // 選べる保存済み計測文書の一覧
        std::vector<std::filesystem::path> m_captureFiles;
        // 表示区間の経路の検索文字列
        std::string m_filter;
        // 時間分布を見る区間の経路
        std::string m_selectedPath;
        // 全フレームの経路を再構成する負荷を避け、選択・範囲・記録の変更時だけ区間時間の系列を作り直す。
        // AとBの区間時間の系列・ミリ秒
        std::array<std::vector<float>, 2> m_series;
        // 系列作成時の区間の経路
        std::string m_seriesPath;
        // 範囲や記録が変わり系列を作り直すか
        bool m_seriesDirty{ true };
        // 最上位の区間だけを表示するか
        bool m_topLevelOnly{};
        // 単一集計または比較の表示選択
        int m_view{};
    };
}
