#pragma once

#include "LamaPon/Core/Api.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    class LAMAPON_API CrashReporter final
    {
    public:
        // 異常終了時の診断出力を登録します(outputDirectory: 出力先、空なら作業場所のCrashes, applicationName: ファイル名に使うアプリ名)。
        // 登録や設定の変更は診断出力と同時に行いません。
        static void Install(
            std::filesystem::path outputDirectory,
            std::string applicationName);
        // 登録前の未処理例外フィルターへ戻します。
        static void Uninstall() noexcept;

        // 異常終了時の例外フィルターを登録済みかを返します。
        [[nodiscard]] static bool IsInstalled() noexcept;
        // ダンプを伴わない診断文書を出力します(reason: 診断理由、最大1024バイト)。
        [[nodiscard]] static bool WriteDiagnostic(
            std::string_view reason) noexcept;
    };
}
