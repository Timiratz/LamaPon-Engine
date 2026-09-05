#pragma once

#include "LamaPon/Core/Crypto.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    struct AssetPackResult final
    {
        std::size_t fileCount{};
        std::uint64_t totalBytes{};
    };

    // sourceDirectory以下のファイルを暗号化し、archiveOutputPathの
    // 単一アーカイブへ格納します。
    // skipExtensionsに挙げた拡張子（小文字、ドット付き）は
    // アーカイブへ入れません。書き出しでHLSLソースを外すために
    // 使います。
    //
    // 鍵は呼び出し側が渡します。書き出しのたびに新しい鍵を作り、
    // それを実行ファイルへ焼き込むためです（エンジン共通の鍵だと、
    // 1本解かれた時点で全ゲームのアーカイブが開いてしまいます）。
    [[nodiscard]] AssetPackResult PackAssets(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& archiveOutputPath,
        const Crypto::AesKey& key,
        const std::vector<std::wstring>& skipExtensions = {});
}
