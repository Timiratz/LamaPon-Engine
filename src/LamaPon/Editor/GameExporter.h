#pragma once

#include "LamaPon/Core/ProjectSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace LamaPon
{
    struct GameExportOptions final
    {
        std::filesystem::path runtimeDirectory;
        std::filesystem::path assetDirectory;
        std::filesystem::path outputDirectory;
        ProjectSettings projectSettings;
        std::filesystem::path gameModulePath;
        // trueなら出力フォルダーの隣へ配布用ZIPも作成します。
        bool createZipArchive{ false };
    };

    struct GameExportResult final
    {
        std::filesystem::path outputDirectory;
        // ゲーム名を反映した実行ファイルのパス。
        std::filesystem::path executablePath;
        // createZipArchive時のみ設定されるZIPのパス。
        std::filesystem::path zipPath;
        std::uintmax_t totalBytes{};
        std::size_t fileCount{};
    };

    // ゲーム名をWindowsのファイル名に使える形へ整えます。
    // 使用不可の文字は「_」へ置換し、結果が空になる場合や
    // 予約デバイス名は安全な名前へ調整します。
    [[nodiscard]] std::wstring SanitizeGameFileName(
        const std::string& gameName);

    [[nodiscard]] GameExportResult ExportGamePackage(
        const GameExportOptions& options);
}
