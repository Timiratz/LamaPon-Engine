#pragma once

#include "LamaPon/Core/Profiler.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>

namespace LamaPon
{
    struct ProfileFrameTree;

    // CPUプロファイラーの履歴を、フレームごとのタイムラインと呼び出し木で
    // 表示するパネルです。Profilerから差分でフレームを取り込み、表示用の
    // 履歴と選択状態を所有します。記録を止めると、履歴と選択中の
    // フレームを保持したまま調べられます。
    class ProfilerPanel final
    {
    public:
        using StatusSink = std::function<void(std::string, bool)>;

        enum class ViewMode
        {
            Hierarchy,
            SelfTime
        };

        explicit ProfilerPanel(StatusSink status);

        ProfilerPanel(const ProfilerPanel&) = delete;
        ProfilerPanel& operator=(const ProfilerPanel&) = delete;

        // captureDirectoryは「記録を保存」の保存先です。空なら保存
        // できません。frameBudgetMillisecondsはタイムラインの目安線です。
        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory,
            float frameBudgetMilliseconds);

        // Profilerの新しいフレームを履歴へ取り込みます。Drawからも
        // 呼ばれます。Profiler::Clearでindexが振り直された場合は、
        // 古い履歴を捨てて取り込み直します。
        void PullFrames();

        [[nodiscard]] const std::deque<ProfileFrame>& History()
            const noexcept
        {
            return m_history;
        }
        // 最新フレームへ追従中なら最新、選択中ならそのフレームです。
        // 選択したフレームが履歴から外れた場合は最新へ戻ります。
        [[nodiscard]] const ProfileFrame* SelectedFrame() const noexcept;
        [[nodiscard]] bool IsFollowingLatest() const noexcept
        {
            return m_followLatest;
        }
        void SelectFrame(std::uint64_t frameIndex) noexcept;
        // 履歴内で相対的に移動します（負で古い方へ）。
        void StepSelection(int offset) noexcept;
        void FollowLatest() noexcept;
        void ClearHistory() noexcept;
        [[nodiscard]] bool SaveHistory(
            const std::filesystem::path& captureDirectory);

    private:
        void DrawToolbar(const std::filesystem::path& captureDirectory);
        void DrawTimeline(float frameBudgetMilliseconds);
        void DrawSelectedFrame(const ProfileFrame& frame);
        void DrawHierarchy(
            const ProfileFrame& frame,
            const ProfileFrameTree& tree);
        void DrawSelfTimeTable(const ProfileFrame& frame);
        void TrimHistory() noexcept;
        void SetStatus(std::string message, bool error = false) const;

        StatusSink m_status;
        std::deque<ProfileFrame> m_history;
        std::uint64_t m_lastPulledIndex{};
        std::uint64_t m_selectedFrameIndex{};
        bool m_followLatest{ true };
        ViewMode m_viewMode{ ViewMode::Hierarchy };
        std::string m_filter;
    };
}
