#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon
{
    // プロファイラーやメモリプロファイラーが保存する記録ファイルの
    // 置き場所と名前付けをまとめます。記録はプロジェクトの.lamapon配下へ
    // 置き、Gitの追跡対象にはしません。
    namespace DebugCaptureFiles
    {
        // 「ファイルを開く」ダイアログです。EditorLayerがWin32の
        // ダイアログで実装し、パネルはキャンセル時にnulloptを受け取ります。
        using OpenFileDialog =
            std::function<std::optional<std::filesystem::path>(
                const std::filesystem::path& initialDirectory)>;

        // 現地時刻の「YYYYMMDD-HHMMSS」です。同じ秒に複数保存した場合も
        // 上書きしないよう、UniqueCapturePathが連番を付けます。
        [[nodiscard]] std::string LocalTimestamp();

        // directory/prefix-時刻.extension の、まだ存在しないパスです。
        [[nodiscard]] std::filesystem::path UniqueCapturePath(
            const std::filesystem::path& directory,
            const std::string& prefix,
            const std::string& extension);

        // 保存先フォルダーを作り、中身をGitの対象外にする.gitignore（*）を
        // 置きます。プロジェクト直下の.gitignoreに記載の無い既存プロジェクト
        // でも、端末ごとの計測値がコミットへ混ざらないようにするためです。
        [[nodiscard]] bool EnsureCaptureDirectory(
            const std::filesystem::path& directory) noexcept;

        // directory直下のextensionのファイルを新しい順に返します。
        // フォルダーが無い、または読めない場合は空です。
        [[nodiscard]] std::vector<std::filesystem::path> ListCaptures(
            const std::filesystem::path& directory,
            const std::string& extension);
    }
}
