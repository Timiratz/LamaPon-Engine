#pragma once

#include <filesystem>

namespace LamaPon
{
    // エディターの異常終了を検出するための目印です。
    //
    // 起動時に`.lamapon/editor-session`を作り、正常終了で消します。
    // 次の起動時にファイルが残っていれば、前回はクラッシュや強制終了で
    // 終わったと判断できます（プロセスが消えてもファイルは残るため）。
    class CrashSentinel final
    {
    public:
        explicit CrashSentinel(
            const std::filesystem::path& projectRoot);
        ~CrashSentinel();

        CrashSentinel(const CrashSentinel&) = delete;
        CrashSentinel& operator=(const CrashSentinel&) = delete;

        // 前回の実行が正常に終了しなかったか。
        [[nodiscard]] bool PreviousRunCrashed() const noexcept
        {
            return m_previousRunCrashed;
        }

        // 正常終了として目印を消します（デストラクタでも実行）。
        void MarkCleanExit() noexcept;

    private:
        std::filesystem::path m_sentinelPath;
        bool m_previousRunCrashed{};
    };
}
