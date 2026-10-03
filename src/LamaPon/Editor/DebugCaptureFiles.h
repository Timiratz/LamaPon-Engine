#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon
{
    // 端末固有の計測記録をプロジェクトの.lamapon配下へ保存しGitの共有対象から外す。
    namespace DebugCaptureFiles
    {
        // ファイル選択を依頼しキャンセルならnulloptを返す(initialDirectory: 最初に表示するフォルダー)。
        using OpenFileDialog =
            std::function<std::optional<std::filesystem::path>(
                const std::filesystem::path& initialDirectory)>;

        // 現地時刻をYYYYMMDD-HHMMSS形式のファイル名用文字列で返す。
        [[nodiscard]] std::string LocalTimestamp();

        // 連番の探索は999まででパスを予約しないため実際の保存時にも衝突を確認する。
        // 接頭辞・時刻・必要なら連番から保存パスを探す(directory: 計測記録の保存フォルダー, prefix: ファイル名の接頭辞, extension: ドットを含む拡張子)。
        [[nodiscard]] std::filesystem::path UniqueCapturePath(
            const std::filesystem::path& directory,
            const std::string& prefix,
            const std::string& extension);

        // 保存フォルダーを確認・作成し未作成の.gitignoreに全除外設定を書く(directory: 計測記録の保存フォルダー)。
        [[nodiscard]] bool EnsureCaptureDirectory(
            const std::filesystem::path& directory) noexcept;

        // 拡張子が一致する直下の通常ファイルを更新時刻の降順で返す(directory: 計測記録の保存フォルダー, extension: ドットを含む拡張子)。
        [[nodiscard]] std::vector<std::filesystem::path> ListCaptures(
            const std::filesystem::path& directory,
            const std::string& extension);
    }
}
