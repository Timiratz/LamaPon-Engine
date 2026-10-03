#pragma once

#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Editor/DebugCaptureFiles.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon
{
    // 取得結果と比較対象を所有し、資源別のメモリ量と差分を表示します。
    class MemoryProfilerPanel final
    {
    public:
        // 状態を通知します（文字列: 通知内容、bool: エラーか）。
        using StatusSink = std::function<void(std::string, bool)>;
        // 現在のメモリ内訳を取得します。
        using CaptureFunction = std::function<MemorySnapshot()>;

        // 取得・通知・選択処理を保持します(capture: 取得処理・空も可, status: 状態通知・空も可, openFile: ファイル選択処理・空も可)。
        MemoryProfilerPanel(
            CaptureFunction capture,
            StatusSink status,
            DebugCaptureFiles::OpenFileDialog openFile);

        // 取得結果の独立性を保つためコピーを禁止します。
        MemoryProfilerPanel(const MemoryProfilerPanel&) = delete;
        // 取得結果の独立性を保つためコピー代入を禁止します。
        MemoryProfilerPanel& operator=(
            const MemoryProfilerPanel&) = delete;

        // 資源量と比較結果を描画します(title: ウィンドウ名, open: 表示状態, captureDirectory: 取得結果の保存先)。
        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory);

        // 新しい取得結果を現在とBに置き、直前の現在をAへ移します。
        void TakeSnapshot();
        // 現在の取得結果をJSONで保存します(captureDirectory: 保存先ディレクトリ)。
        [[nodiscard]] bool SaveCurrent(
            const std::filesystem::path& captureDirectory);
        // 現在の取得結果を借用し、次の取得まで参照できます。
        [[nodiscard]] const std::optional<MemorySnapshot>& Current()
            const noexcept
        {
            return m_current;
        }

    private:
        struct ComparisonSlot final
        {
            // 比較対象の取得結果
            std::optional<MemorySnapshot> snapshot;
        };

        // 取得名の入力と取得・保存操作を描画します(captureDirectory: 取得結果の保存先)。
        void DrawToolbar(const std::filesystem::path& captureDirectory);
        // 現在の資源量を分類・名称で絞り込んで表示します。
        void DrawCurrentView();
        // AからBへの資源量の増減を表示します(captureDirectory: 保存済み結果の参照先)。
        void DrawComparisonView(
            const std::filesystem::path& captureDirectory);
        // 比較枠の取得結果を選びます(slot: 有効な枠番号・0はA、1はB, captureDirectory: 保存済み結果の参照先)。
        void DrawSlotSelector(
            std::size_t slot,
            const std::filesystem::path& captureDirectory);
        // 取得結果を読み込んで比較枠を更新します(slot: 有効な枠番号・0はA、1はB, path: 読込対象のJSONパス)。
        void LoadSlot(
            std::size_t slot,
            const std::filesystem::path& path);
        // 状態通知があれば呼び出します(message: 通知内容, error: エラー通知か)。
        void SetStatus(std::string message, bool error = false) const;

        // 取得処理の所有先
        CaptureFunction m_capture;
        // 状態通知の所有先
        StatusSink m_status;
        // ファイル選択処理の所有先
        DebugCaptureFiles::OpenFileDialog m_openFile;
        // 現在の取得結果
        std::optional<MemorySnapshot> m_current;
        // 比較枠・0はA、1はB
        std::array<ComparisonSlot, 2> m_slots;
        // 比較結果のキャッシュ
        std::optional<MemorySnapshotComparison> m_comparison;
        // 比較結果の再計算が必要か
        bool m_comparisonDirty{ true };
        // 保存済み取得結果のパス一覧
        std::vector<std::filesystem::path> m_captureFiles;
        // 次回の取得結果に付ける名前
        std::string m_label;
        // 資源名の部分一致フィルター
        std::string m_filter;

        // 表示分類の添字・-1は全分類
        int m_categoryFilter{ -1 };
    };
}
