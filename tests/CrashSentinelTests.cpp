#include "LamaPon/Core/CrashSentinel.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const char* message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

// 起動目印の作成・正常破棄・残存時の異常検出と明示解除の冪等性を検査する。
int main()
{
    // CrashSentinel検査の例外を終了コードへ変換します。
    try
    {
        // 起動目印を作る検査用Projectルート
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "crash-sentinel";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        // 起動中だけ存在する目印のパス
        const auto sentinelPath =
            root / ".lamapon" / "editor-session";

        // 初回起動の目印作成と正常破棄を検査する。
        {
            // 起動判定と破棄時の解除の検査対象
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                !sentinel.PreviousRunCrashed(),
                "a first run must not report a crash");
            Require(
                std::filesystem::is_regular_file(
                    sentinelPath),
                "the sentinel file must exist while running");
        }
        // 正常破棄後に起動目印が残らないことを検査する。
        Require(
            !std::filesystem::exists(sentinelPath),
            "a clean exit must remove the sentinel");

        // 正常終了に続く起動を異常扱いしないことを検査する。
        {
            // 起動判定と破棄時の解除の検査対象
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                !sentinel.PreviousRunCrashed(),
                "a run after a clean exit must be normal");
        }

        // 破棄前の目印を保存して後から戻し、異常終了時の残存を再現する。
        {
            // 復元用に起動目印を作る検査対象
            LamaPon::CrashSentinel crashed(root);
            static_cast<void>(crashed);
            // Destructorで消される前に目印を退避する。
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
            // 起動判定と破棄時の解除の検査対象
            const LamaPon::CrashSentinel sentinel(root);
            Require(
                sentinel.PreviousRunCrashed(),
                "a leftover sentinel must report a crash");
        }
        Require(
            !std::filesystem::exists(sentinelPath),
            "the sentinel is cleared after being handled");

        // 明示的な正常解除を二重に呼んでも目印が残らないことを検査する。
        {
            // 明示解除を二重に呼ぶ検査対象
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
    // 検査失敗を標準エラーへ出す(exception: 捕捉した検査エラー)。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
