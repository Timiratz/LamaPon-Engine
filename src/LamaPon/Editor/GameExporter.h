#pragma once

#include "LamaPon/Core/ProjectSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace LamaPon
{
    struct GameSigningOptions final
    {
        // コード署名を行うか
        bool enabled{ false };
        // signtoolの実行ファイルパス
        std::filesystem::path signToolPath;
        // Current UserのMyストアにあるコード署名証明書を指定します。
        // 署名証明書のSHA-1
        std::string certificateSha1;
        // 署名時刻を認証するURL
        std::string timestampUrl;
    };

    struct GameExportOptions final
    {
        // 配布Runtime一式の保存先
        std::filesystem::path runtimeDirectory;
        // 書き出すassetsの基準パス
        std::filesystem::path assetDirectory;
        // 完成したゲームの出力先
        std::filesystem::path outputDirectory;
        // 配布物へ反映する設定
        ProjectSettings projectSettings;
        // 同梱するGame ModuleのDLLパス
        std::filesystem::path gameModulePath;
        // 出力先の隣へZIPも作るか
        bool createZipArchive{ false };
        // 配布物に保存しない署名設定
        GameSigningOptions signing;
    };

    struct GameExportResult final
    {
        // 完成したゲームの出力先
        std::filesystem::path outputDirectory;
        // ゲーム名を反映したexeパス
        std::filesystem::path executablePath;
        // 作成したZIP・未作成なら空
        std::filesystem::path zipPath;
        // 書き出したファイルの総byte数
        std::uintmax_t totalBytes{};
        // 書き出した通常ファイル数
        std::size_t fileCount{};
    };


    // ゲーム名の禁止文字と予約名を調整し、Windows用のファイル名を返します(gameName: 元のゲーム名)。
    [[nodiscard]] std::wstring SanitizeGameFileName(
        const std::string& gameName);


    // 有効な署名設定を検証し、不正ならinvalid_argumentを投げます(options: 配布物に保存しない署名設定)。
    void ValidateGameSigningOptions(const GameSigningOptions& options);

    // ZIP作成はフォルダー公開後に行うため、ZIPの失敗では公開済みフォルダーを元に戻しません。
    // 資産とシェーダーを準備し、署名検証後に完成物を公開します(options: 書き出し元・配布先・署名設定)。
    [[nodiscard]] GameExportResult ExportGamePackage(
        const GameExportOptions& options);
}
