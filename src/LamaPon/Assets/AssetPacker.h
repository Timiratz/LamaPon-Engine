#pragma once

#include "LamaPon/Core/Crypto.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace LamaPon
{
    struct AssetPackResult final
    {
        // 梱包したファイル数
        std::size_t fileCount{};
        // 変換後の平文の総バイト数
        std::uint64_t totalBytes{};
        // 配布出力しない梱包一覧
        std::vector<std::filesystem::path> includedFiles;
        // 配布対象外の相対パス一覧
        std::vector<std::filesystem::path> excludedFiles;
    };

    // 元ファイルを変えず暗号化前の内容を変換する(relativePath: ルート内の相対パス, contents: 変換する平文)。
    using AssetPackTransform = std::function<void(
        const std::filesystem::path& relativePath,
        std::vector<std::uint8_t>& contents)>;

    // 認証付きアーカイブへ梱包し、失敗は例外にする(sourceDirectory: 元アセットのルート, archiveOutputPath: 上書きする出力先, key: 書き出しごとの暗号化鍵, skipExtensions: 除外する小文字・ドット付き拡張子, transform: 任意の内容変換)。
    [[nodiscard]] AssetPackResult PackAssets(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& archiveOutputPath,
        const Crypto::AesKey& key,
        const std::vector<std::wstring>& skipExtensions = {},
        const AssetPackTransform& transform = {});
}
