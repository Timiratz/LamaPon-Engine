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
    // 資源ごとのメモリ内訳を表示し、2つのスナップショットを比べる
    // パネルです（UnityのMemory Profilerに相当）。スナップショットの
    // 取得は呼び出し側から受け取った関数へ任せ、パネルは取得結果と
    // 比較対象、表示の絞り込み状態を所有します。
    class MemoryProfilerPanel final
    {
    public:
        using StatusSink = std::function<void(std::string, bool)>;
        using CaptureFunction = std::function<MemorySnapshot()>;

        MemoryProfilerPanel(
            CaptureFunction capture,
            StatusSink status,
            DebugCaptureFiles::OpenFileDialog openFile);

        MemoryProfilerPanel(const MemoryProfilerPanel&) = delete;
        MemoryProfilerPanel& operator=(
            const MemoryProfilerPanel&) = delete;

        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory);

        // 取得したスナップショットを「現在」として保持し、比較のBにも
        // 入れます（直前の現在はAへ移ります）。
        void TakeSnapshot();
        [[nodiscard]] bool SaveCurrent(
            const std::filesystem::path& captureDirectory);
        [[nodiscard]] const std::optional<MemorySnapshot>& Current()
            const noexcept
        {
            return m_current;
        }

    private:
        struct ComparisonSlot final
        {
            std::optional<MemorySnapshot> snapshot;
        };

        void DrawToolbar(const std::filesystem::path& captureDirectory);
        void DrawCurrentView();
        void DrawComparisonView(
            const std::filesystem::path& captureDirectory);
        void DrawSlotSelector(
            std::size_t slot,
            const std::filesystem::path& captureDirectory);
        void LoadSlot(
            std::size_t slot,
            const std::filesystem::path& path);
        void SetStatus(std::string message, bool error = false) const;

        CaptureFunction m_capture;
        StatusSink m_status;
        DebugCaptureFiles::OpenFileDialog m_openFile;
        std::optional<MemorySnapshot> m_current;
        std::array<ComparisonSlot, 2> m_slots;
        std::optional<MemorySnapshotComparison> m_comparison;
        bool m_comparisonDirty{ true };
        std::vector<std::filesystem::path> m_captureFiles;
        std::string m_label;
        std::string m_filter;
        // -1は全分類です。
        int m_categoryFilter{ -1 };
    };
}
