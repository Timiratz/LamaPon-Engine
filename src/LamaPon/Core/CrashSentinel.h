#pragma once

#include <filesystem>

namespace LamaPon
{
    // .lamapon/editor-sessionの残存を使ってエディターの前回の異常終了を検出します。
    class CrashSentinel final
    {
    public:
        // 前回の目印を確認して今回の実行を記録します(projectRoot: プロジェクトのルート)。
        explicit CrashSentinel(
            const std::filesystem::path& projectRoot);
        // 正常終了として目印を削除します。
        ~CrashSentinel();

        // 終了処理の重複を避けるためコピーを禁止します。
        CrashSentinel(const CrashSentinel&) = delete;
        // 終了処理の重複を避けるためコピー代入を禁止します。
        CrashSentinel& operator=(const CrashSentinel&) = delete;

        // 前回の実行の目印が残っていたかを返します。
        [[nodiscard]] bool PreviousRunCrashed() const noexcept
        {
            return m_previousRunCrashed;
        }

        // 正常終了として目印の削除を試みます。
        void MarkCleanExit() noexcept;

    private:
        // 今回の実行を記録する目印のパス
        std::filesystem::path m_sentinelPath;
        // 前回の実行の目印が残っていたか
        bool m_previousRunCrashed{};
    };
}
