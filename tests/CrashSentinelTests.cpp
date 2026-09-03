#include "LamaPon/Core/CrashSentinel.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "crash-sentinel";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const auto sentinelPath =
            root / ".lamapon" / "editor-session";

        // 初回起動：前回のクラッシュは無く、目印が作られます。
        {
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                !sentinel.PreviousRunCrashed(),
                "a first run must not report a crash");
            Require(
                std::filesystem::is_regular_file(
                    sentinelPath),
                "the sentinel file must exist while running");
        }
        // 正常終了：目印が消えます。
        Require(
            !std::filesystem::exists(sentinelPath),
            "a clean exit must remove the sentinel");

        // 2回目：正常終了の後なのでクラッシュ扱いになりません。
        {
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                !sentinel.PreviousRunCrashed(),
                "a run after a clean exit must be normal");
        }

        // クラッシュを再現：目印を残したまま次を起動します。
        {
            LamaPon::CrashSentinel crashed(root);
            static_cast<void>(crashed);
            // MarkCleanExitを呼ばずにファイルを残します。
            std::filesystem::copy_file(
                sentinelPath,
                root / "keep",
                std::filesystem::copy_options::
                    overwrite_existing);
        }
        std::filesystem::copy_file(
            root / "keep",
            sentinelPath,
            std::filesystem::copy_options::
                overwrite_existing);
        {
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                sentinel.PreviousRunCrashed(),
                "a leftover sentinel must report a crash");
        }
        Require(
            !std::filesystem::exists(sentinelPath),
            "the sentinel is cleared after being handled");

        // 明示的なMarkCleanExitは二重呼び出しでも安全です。
        {
            LamaPon::CrashSentinel sentinel(root);
            sentinel.MarkCleanExit();
            sentinel.MarkCleanExit();
            Require(
                !std::filesystem::exists(sentinelPath),
                "MarkCleanExit must be idempotent");
        }

        std::cout << "Crash sentinel tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
