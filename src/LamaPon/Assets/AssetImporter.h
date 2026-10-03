#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    struct ImportedAssetFile final
    {
        // コピー元の絶対パス
        std::filesystem::path source;
        // アセットルート内の相対パス
        std::filesystem::path path;
        // 衝突回避による入力名の変更
        bool renamed{};
    };

    struct AssetImportFailure final
    {
        // 失敗した入力パス
        std::filesystem::path source;
        // 取り込み失敗の理由
        std::string message;
    };

    struct AssetImportResult final
    {
        // 取り込みに成功したファイル
        std::vector<ImportedAssetFile> files;
        // 入力単位の失敗一覧
        std::vector<AssetImportFailure> failures;
        // 成功した入力フォルダー数
        std::size_t importedDirectoryCount{};
        // 衝突回避で改名した入力数
        std::size_t renamedSourceCount{};
        // 除外した.metaファイル数
        std::size_t skippedMetadataCount{};
        // 除外したリンク数
        std::size_t skippedLinkCount{};
    };

    class AssetImporter final
    {
    public:
        // 入力単位でコピーし、失敗を結果へ記録する(sources: 元ファイル・フォルダー, assetRoot: アセットのルート, targetDirectory: ルート内の相対保存先)。
        [[nodiscard]] static AssetImportResult Import(
            const std::vector<std::filesystem::path>& sources,
            const std::filesystem::path& assetRoot,
            const std::filesystem::path& targetDirectory);
    };
}
