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

    // Profilerの差分を表示用履歴へ取り込み、記録停止後もフレームの選択と解析を保持する。
    class ProfilerPanel final
    {
    public:
        // 本文と失敗フラグを通知するcallback
        using StatusSink = std::function<void(std::string, bool)>;

        enum class ViewMode
        {
            Hierarchy,
            SelfTime
        };

        // 保存結果の通知先を所有する計測表示を作る(status: 状態通知のcallback・空も可)。
        explicit ProfilerPanel(StatusSink status);

        // 表示履歴と選択状態の共有を禁止する。
        ProfilerPanel(const ProfilerPanel&) = delete;
        // 表示履歴と選択状態の共有を禁止する。
        ProfilerPanel& operator=(const ProfilerPanel&) = delete;

        // 新規フレームを取り込んで履歴と区間解析を描く(title: ウィンドウの表示名とID, open: panelの表示状態の参照, captureDirectory: 記録JSONの保存先・空は保存不可, frameBudgetMilliseconds: 時間予算の目安・ミリ秒)。
        void Draw(
            const char* title,
            bool& open,
            const std::filesystem::path& captureDirectory,
            float frameBudgetMilliseconds);

        // 新規フレームを差分で取り込み番号リセット時は旧履歴を捨てる。
        void PullFrames();

        // 保持中の表示履歴を更新・消去まで借用する。
        [[nodiscard]] const std::deque<ProfileFrame>& History()
            const noexcept
        {
            return m_history;
        }
        // 選択または最新フレームを履歴更新まで借用し履歴が空ならnullを返す。
        [[nodiscard]] const ProfileFrame* SelectedFrame() const noexcept;
        // 最新フレームの表示に追従中かを返す。
        [[nodiscard]] bool IsFollowingLatest() const noexcept
        {
            return m_followLatest;
        }
        // 指定フレームを選択して最新への追従を止める(frameIndex: 選択する計測フレーム番号)。
        void SelectFrame(std::uint64_t frameIndex) noexcept;
        // 履歴範囲内で選択を相対移動する(offset: 移動量・負なら古いフレーム)。
        void StepSelection(int offset) noexcept;
        // 最新フレームへの追従を再開する。
        void FollowLatest() noexcept;
        // Profilerと表示履歴を消去して最新への追従を再開する。
        void ClearHistory() noexcept;
        // 全表示履歴をJSONへ保存して成否を通知する(captureDirectory: 計測JSONの保存先フォルダー)。
        [[nodiscard]] bool SaveHistory(
            const std::filesystem::path& captureDirectory);

    private:
        // 計測・フレーム選択・履歴容量・記録保存の操作を表示する(captureDirectory: 計測JSONの保存先フォルダー)。
        void DrawToolbar(const std::filesystem::path& captureDirectory);
        // 最上位区間の積み上げ履歴とフレーム選択の操作を描く(frameBudgetMilliseconds: 時間予算の目安・ミリ秒)。
        void DrawTimeline(float frameBudgetMilliseconds);
        // 選択フレームを親子階層または自己時間順で表示する(frame: 解析・表示するフレーム)。
        void DrawSelectedFrame(const ProfileFrame& frame);
        // 親子関係と合計時間・自己時間を階層表に表示する(frame: 表示するフレーム, tree: 当該フレームの親子解析結果)。
        void DrawHierarchy(
            const ProfileFrame& frame,
            const ProfileFrameTree& tree);
        // 区間名で絞り込める自己時間順の集計を表示する(frame: 表示するフレーム)。
        void DrawSelfTimeTable(const ProfileFrame& frame);
        // Profilerの設定容量に合わせて古い表示フレームを捨てる。
        void TrimHistory() noexcept;
        // 通知先があれば操作結果を渡す(message: 通知本文の所有先, error: 失敗として通知するか)。
        void SetStatus(std::string message, bool error = false) const;

        // 保存結果などを通知するcallback
        StatusSink m_status;
        // 表示用に保持する計測フレーム
        std::deque<ProfileFrame> m_history;
        // 最後に取り込んだフレーム番号
        std::uint64_t m_lastPulledIndex{};
        // 追従停止時に選ぶフレーム番号
        std::uint64_t m_selectedFrameIndex{};
        // 最新フレームへ追従するか
        bool m_followLatest{ true };
        // 階層表示または自己時間順の選択
        ViewMode m_viewMode{ ViewMode::Hierarchy };
        // 自己時間表の区間名検索文字列
        std::string m_filter;
    };
}
